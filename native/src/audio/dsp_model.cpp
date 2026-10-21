// Derived from Azahar src/audio_core/hle/hle.cpp, src/audio_core/hle/source.cpp,
// src/audio_core/hle/source.h, src/audio_core/hle/mixers.cpp, src/audio_core/hle/mixers.h,
// src/audio_core/hle/effects.cpp, src/audio_core/hle/aac_decoder.cpp, src/audio_core/hle/decoder.h,
// src/audio_core/interpolate.cpp, src/audio_core/interpolate.h, src/audio_core/codec.cpp and
// src/core/memory.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says
// "Licensed under GPLv2 or any later version"): the DSP's pipes and states (PipeRead, PipeWrite,
// Initialize, Sleep, Wakeup, AudioPipeWriteStructAddresses, CurrentRegionIndex, GenerateCurrentFrame),
// the source's handling of its configuration and buffer queue (Source::ParseConfig, GenerateFrame,
// DequeueBuffer, GetCurrentStatus), the way the interpolation steps over its input
// (StepOverSamples, 24-bit fixed point), the number of samples a decoded buffer holds (DecodeADPCM,
// DecodePCM8, DecodePCM16), what the mixers do to the configuration in memory (Mixers::ParseConfig,
// AuxSend, DelayEffect::ParseConfig), the binary pipe's answers (AACDecoder::ProcessRequest, Decode)
// and the physical memory areas (MemorySystem::GetPhysMemRegionInfo).
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (source.cpp, source.h,
//   mixers.cpp, mixers.h)
// Copyright 2026 Citra Emulator Project / Azahar Emulator Project (effects.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (aac_decoder.cpp)
// Copyright 2018-2025 Citra Emulator Project / Azahar Emulator Project (decoder.h)
// Copyright 2016 Citra Emulator Project (interpolate.cpp, interpolate.h, codec.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.cpp)
//
// The model of the DSP without sound (N1-18); see dsp_model.h for what it does and leaves out.
#include "audio/dsp_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "mh4n/log.h"
#include "mh4n/memory.h"

namespace mh4n::audio {

namespace {

constexpr const char* kLog = "Audio.DSP";

// The interpolation works in fixed point with 24 fractional bits (AudioInterp).
constexpr u64 kScaleFactor = 1u << 24;

constexpr u32 kN3dsExtraRamPaddr = 0x1F000000;
constexpr u32 kN3dsExtraRamSize = 0x00400000;

template <class T>
T Load(const u8* at) {
    T value;
    std::memcpy(&value, at, sizeof(T));
    return value;
}

template <class T>
void Store(u8* at, T value) {
    std::memcpy(at, &value, sizeof(T));
}

// The end of an area counts: "the user can pass in an address that represents an open right bound".
bool InArea(u32 address, u32 start, u32 size) {
    return address >= start && static_cast<u64>(address) <= static_cast<u64>(start) + size;
}

// static_cast<u64>(rate * scale_factor), saturating where the C++ conversion would be undefined.
u64 StepSize(float rate) {
    const float scaled = rate * static_cast<float>(kScaleFactor);
    if (!(scaled < 18446744073709551616.0f)) {
        return std::numeric_limits<u64>::max();
    }
    return static_cast<u64>(scaled);
}

}  // namespace

bool IsPhysicalAddressValid(u32 address) {
    // The areas in the order Azahar's GetPhysMemRegionInfo searches them; the FCRAM area has the New
    // 3DS's size whatever the console.
    return InArea(address, kVramPaddr, kVramSize) || InArea(address, kDspRamPaddr, kDspRamSize) ||
           InArea(address, kFcramPaddr, kFcramSizeNew3ds) ||
           InArea(address, kN3dsExtraRamPaddr, kN3dsExtraRamSize);
}

// ---- a source ------------------------------------------------------------------------------------------------

SourceStatus SourceModel::Tick(SourceConfiguration& config, u32 source_id) {
    ParseConfig(config, source_id);
    if (state_.enabled) {
        GenerateFrame(source_id);
    }
    return CurrentStatus();
}

void SourceModel::ParseConfig(SourceConfiguration& config, u32 source_id) {
    // Azahar clears each dirty bit as it handles it and the rest at the end; the conditions read
    // bits nothing has cleared yet, so the value at entry is the one to test.
    const u32 bits = config.dirty_raw;
    if (bits == 0) {
        return;
    }
    State& s = state_;

    if (bits & dirty::kReset) {
        Reset();
        MH4N_LOG(Trace, kLog, "source_id={} reset", source_id);
    }
    if (bits & dirty::kPartialReset) {
        s.input_queue = {};
        MH4N_LOG(Trace, kLog, "source_id={} partial_reset", source_id);
    }
    if (bits & dirty::kEnable) {
        s.enabled = config.enable != 0;
        MH4N_LOG(Trace, kLog, "source_id={} enable={}", source_id, s.enabled);
    }
    if (bits & dirty::kSyncCount) {
        s.sync_count = config.sync_count;
        MH4N_LOG(Trace, kLog, "source_id={} sync={}", source_id, s.sync_count);
    }
    if (bits & dirty::kRateMultiplier) {
        s.rate_multiplier = config.rate_multiplier;
        MH4N_LOG(Trace, kLog, "source_id={} rate={}", source_id, s.rate_multiplier);
        // Azahar tests `<= 0`; a NaN passes that test and then fails an assertion in the
        // interpolation, so it is as invalid as a zero here.
        if (!(s.rate_multiplier > 0)) {
            MH4N_LOG(Error, kLog, "Was given an invalid rate multiplier: source_id={} rate={}",
                     source_id, s.rate_multiplier);
            s.rate_multiplier = 1.0f;
            // Note: Actual firmware starts producing garbage if this occurs (Azahar's comment).
        }
    }
    // The ADPCM coefficients, the gains, the filters and the interpolation mode change nothing the
    // model keeps: their bits are only cleared (below, with the rest).
    if (bits & (dirty::kFormat | dirty::kEmbeddedBuffer)) {
        s.format = static_cast<u8>((config.flags1_raw >> 2) & 3);
        MH4N_LOG(Trace, kLog, "source_id={} format={}", source_id, s.format);
    }
    if (bits & (dirty::kMonoOrStereo | dirty::kEmbeddedBuffer)) {
        s.mono_or_stereo = static_cast<u8>(config.flags1_raw & 3);
        MH4N_LOG(Trace, kLog, "source_id={} mono_or_stereo={}", source_id, s.mono_or_stereo);
    }

    // play_position applies only to the embedded buffer, and defaults to 0 without a dirty bit.
    u32 play_position = 0;
    if (bits & dirty::kPlayPosition) {
        play_position = SwapHalves(config.play_position);
    }

    if (bits & dirty::kPartialEmbeddedBuffer) {
        // "A quick and dirty way of extending the current buffer is to just read the whole thing again
        // with the new length", from the latched address.
        if (IsPhysicalAddressValid(s.current_buffer_physical_address & 0xFFFFFFFCu)) {
            const u32 length = SwapHalves(config.length);
            bool valid = false;
            switch (s.format) {
            case kPcm16:
                s.current_buffer_size = length;
                s.current_buffer_length = length;
                s.current_buffer_mono_or_stereo = s.mono_or_stereo;
                s.current_buffer_format = s.format;
                s.current_buffer_is_looping = (config.flags2_raw & 2) != 0;
                valid = true;
                break;
            default:
                // Azahar's UNIMPLEMENTED_MSG logs that PCM8 and ADPCM are not handled for partial
                // buffer updates (and UNIMPLEMENTED the other values) and goes on, as here.
                MH4N_LOG(Error, kLog,
                         "source_id={}: format {} is not handled for partial buffer updates",
                         source_id, s.format);
                break;
            }
            // "re-consume the samples up to the current sample number". Tomodachi Life can shorten
            // the buffer: then the position starts again at 0 and nothing is dropped.
            if (valid) {
                if (s.current_buffer_size < s.current_sample_number) {
                    s.current_sample_number = 0;
                } else {
                    s.current_buffer_size -= s.current_sample_number;
                }
            }
        }
        MH4N_LOG(Trace, kLog, "source_id={} partially updating embedded buffer addr={:#010x} len={}",
                 source_id, s.current_buffer_physical_address, SwapHalves(config.length));
    }

    if (bits & dirty::kEmbeddedBuffer) {
        const u32 length = SwapHalves(config.length);
        // Azahar's hack for Luigi's Mansion Dark Moon, which passes an underflowed length.
        if (static_cast<s32>(length) < 0) {
            MH4N_LOG(Error, kLog,
                     "Skipping embedded buffer sample! Application passed in improper value for "
                     "length. addr {:X} length {:X}",
                     SwapHalves(config.physical_address), length);
        } else {
            s.input_queue.push(Buffer{
                SwapHalves(config.physical_address),
                length,
                (config.flags2_raw & 2) != 0,
                config.buffer_id,
                s.mono_or_stereo,
                s.format,
                false,
                play_position,
                false,
            });
        }
        MH4N_LOG(Trace, kLog, "enqueuing embedded addr={:#010x} len={} id={} start={}",
                 SwapHalves(config.physical_address), length, config.buffer_id, play_position);
    }

    if ((bits & dirty::kLoopRelated) && SwapHalves(config.loop_related) != 0) {
        MH4N_LOG(Warning, kLog, "Unhandled complex loop with loop_related={:#010x}",
                 SwapHalves(config.loop_related));
    }

    if (bits & dirty::kBufferQueue) {
        for (std::size_t i = 0; i < 4; ++i) {
            if (!(config.buffers_dirty & (1u << i))) {
                continue;
            }
            const QueuedBufferConfig& b = config.buffers[i];
            const u32 length = SwapHalves(b.length);
            if (static_cast<s32>(length) < 0) {
                MH4N_LOG(Error, kLog,
                         "Skipping buffer queue sample! Game passed in improper value for length. "
                         "addr {:X} length {:X}",
                         SwapHalves(b.physical_address), length);
            } else {
                s.input_queue.push(Buffer{
                    SwapHalves(b.physical_address),
                    length,
                    b.is_looping != 0,
                    b.buffer_id,
                    s.mono_or_stereo,
                    s.format,
                    true,   // from_queue
                    0,      // play_position
                    false,  // has_played
                });
            }
            MH4N_LOG(Trace, kLog, "enqueuing queued {} addr={:#010x} len={} id={}", i,
                     SwapHalves(b.physical_address), length, b.buffer_id);
        }
        config.buffers_dirty = 0;
    }

    config.dirty_raw = 0;
}

void SourceModel::GenerateFrame(u32 source_id) {
    State& s = state_;

    // A looping PCM16 buffer can be rewritten by the application while the DSP plays it: Azahar
    // reads the not-yet-consumed part again from the buffer's current offset. The model keeps no
    // samples, so only the length is set (and the read happens only if the address is valid).
    if (s.current_buffer_size != 0 && s.current_buffer_is_looping &&
        s.current_buffer_format == kPcm16 && s.current_buffer_physical_address != 0 &&
        s.current_sample_number < s.current_buffer_length) {
        const u32 channels = s.current_buffer_mono_or_stereo == 2 ? 2 : 1;
        const u32 byte_offset = s.current_sample_number * channels * static_cast<u32>(sizeof(s16));
        const u32 physical_address = (s.current_buffer_physical_address & 0xFFFFFFFCu) + byte_offset;
        if (IsPhysicalAddressValid(physical_address)) {
            s.current_buffer_size = s.current_buffer_length - s.current_sample_number;
        }
    }

    if (s.current_buffer_size == 0) {
        if (DequeueBuffer(source_id)) {
            return;
        }
        s.enabled = false;
        s.buffer_update = true;
        s.last_buffer_id = s.current_buffer_id;
        s.current_buffer_id = 0;
        return;
    }

    std::size_t frame_position = 0;
    u32 dequeues = 0;
    while (frame_position < kSamplesPerFrame) {
        if (s.current_buffer_size == 0) {
            if (dequeues++ >= kMaxDequeuesPerFrame) {
                // Azahar goes on for ever with a looping buffer of length 0, and goes on longer than
                // this cap with a short looping buffer at a high rate (see dsp_model.h).
                MH4N_LOG(Error, kLog,
                         "source_id={}: {} buffers dequeued in one frame without filling it; the "
                         "frame ends here",
                         source_id, kMaxDequeuesPerFrame);
                break;
            }
            if (!DequeueBuffer(source_id)) {
                break;
            }
        }
        const std::size_t size_before = s.current_buffer_size;
        StepOverSamples(frame_position);
        s.current_sample_number += static_cast<u32>(size_before - s.current_buffer_size);
    }
}

// Azahar's AudioInterp::StepOverSamples: three samples of history are prepended (two), the input is
// stepped over in steps of `rate`, one output sample per step, until the output frame is full or the
// input runs out; what the steps passed is erased. Only the number of samples matters here.
void SourceModel::StepOverSamples(std::size_t& output_index) {
    State& s = state_;
    const std::size_t size = s.current_buffer_size;
    if (size == 0) {
        return;
    }
    const std::size_t input_size = size + 2;  // the two samples of history, put in front
    const u64 step_size = StepSize(s.rate_multiplier);
    u64 fposition = s.fposition;
    std::size_t inputi = 0;
    while (output_index < kSamplesPerFrame) {
        inputi = static_cast<std::size_t>(fposition / kScaleFactor);
        if (inputi + 2 >= input_size) {
            inputi = input_size - 2;
            break;
        }
        ++output_index;
        fposition += step_size;
    }
    s.fposition = fposition - inputi * kScaleFactor;
    s.current_buffer_size = input_size - (inputi + 2);
}

bool SourceModel::DequeueBuffer(u32 source_id) {
    State& s = state_;
    if (s.input_queue.empty()) {
        return false;
    }
    Buffer buf = s.input_queue.top();
    s.input_queue.pop();

    // "This physical address masking occurs due to how the DSP DMA hardware is configured by the
    // firmware."
    if (IsPhysicalAddressValid(buf.physical_address & 0xFFFFFFFCu)) {
        switch (buf.format) {
        case kPcm8:
        case kPcm16:
            s.current_buffer_size = buf.length;
            break;
        case kAdpcm:
            // DecodeADPCM makes a buffer with a multiple of two samples.
            s.current_buffer_size = static_cast<std::size_t>(buf.length) + (buf.length % 2);
            break;
        default:
            // Azahar's UNIMPLEMENTED logs and leaves the buffer empty, as here.
            MH4N_LOG(Error, kLog, "source_id={} buffer_id={}: unknown format {}", source_id,
                     buf.buffer_id, buf.format);
            s.current_buffer_size = 0;
            break;
        }
    } else {
        MH4N_LOG(Warning, kLog, "source_id={} buffer_id={} length={}: Invalid physical address {:#010x}",
                 source_id, buf.buffer_id, buf.length, buf.physical_address);
        s.current_buffer_size = 0;
        return true;
    }

    // The first playthrough starts at play_position, loops start at the beginning of the buffer.
    s.current_sample_number = !buf.has_played ? buf.play_position : 0;
    s.current_buffer_physical_address = buf.physical_address;
    s.current_buffer_length = buf.length;
    s.current_buffer_mono_or_stereo = buf.mono_or_stereo;
    s.current_buffer_format = buf.format;
    s.current_buffer_is_looping = buf.is_looping;
    s.current_buffer_id = buf.buffer_id;
    s.last_buffer_id = 0;
    s.buffer_update = buf.from_queue && !buf.has_played;

    if (buf.is_looping) {
        buf.has_played = true;
        s.input_queue.push(buf);
    }

    // The samples before the starting position are consumed (a position past the end empties the
    // buffer; Azahar erases past the end).
    s.current_buffer_size -= std::min<std::size_t>(s.current_sample_number, s.current_buffer_size);
    return true;
}

SourceStatus SourceModel::CurrentStatus() {
    SourceStatus ret{};
    ret.is_enabled = state_.enabled ? 1 : 0;
    ret.current_buffer_id_dirty = state_.buffer_update ? 1 : 0;
    state_.buffer_update = false;
    ret.sync_count = state_.sync_count;
    ret.buffer_position = SwapHalves(state_.current_sample_number);
    ret.current_buffer_id = state_.current_buffer_id;
    ret.last_buffer_id = state_.last_buffer_id;
    return ret;
}

// ---- the DSP -------------------------------------------------------------------------------------------------------

DspModel::DspModel(u8* memory) : memory_(memory) {}

u16 DspModel::RecvData(u32 register_number) {
    if (register_number != 0) {
        // Azahar's ASSERT_MSG ends the process here (a departure, see dsp_model.h).
        MH4N_LOG(Error, kLog, "Unknown register_number {}", register_number);
    }
    // The application reads this after requesting DSP shutdown, to verify the DSP has indeed shut
    // down or slept.
    return state_ == DspState::On ? 0 : 1;
}

bool DspModel::RecvDataIsReady(u32 register_number) const {
    if (register_number != 0) {
        MH4N_LOG(Error, kLog, "Unknown register_number {}", register_number);
    }
    return true;
}

std::vector<u8> DspModel::PipeRead(u32 pipe, std::size_t length) {
    if (pipe >= kNumPipes) {
        MH4N_LOG(Error, kLog, "pipe_number = {} invalid", pipe);
        return {};
    }
    if (length > 0xFFFF) {  // can only read at most UINT16_MAX from the pipe
        MH4N_LOG(Error, kLog, "length of {} greater than max of {}", length, 0xFFFF);
        return {};
    }
    std::vector<u8>& data = pipes_[pipe];
    if (length > data.size()) {
        MH4N_LOG(Warning, kLog,
                 "pipe_number = {} is out of data, application requested read of {} but {} remain",
                 pipe, length, data.size());
        length = data.size();
    }
    if (length == 0) {
        return {};
    }
    std::vector<u8> ret(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(length));
    data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(length));
    return ret;
}

std::size_t DspModel::PipeReadableSize(u32 pipe) const {
    if (pipe >= kNumPipes) {
        MH4N_LOG(Error, kLog, "pipe_number = {} invalid", pipe);
        return 0;
    }
    return pipes_[pipe].size();
}

void DspModel::PipeWrite(u32 pipe, std::span<const u8> buffer) {
    switch (static_cast<DspPipe>(pipe)) {
    case DspPipe::Audio: {
        if (buffer.size() != 4) {
            MH4N_LOG(Error, kLog, "DspPipe::Audio: Unexpected buffer length {} was written",
                     buffer.size());
            return;
        }
        enum class StateChange { Initialize = 0, Shutdown = 1, Wakeup = 2, Sleep = 3 };
        // Waking up from sleep garbles some of the structs in the memory region in the real DSP
        // (Azahar's TODO): applications store the state of these structs before sleeping and put it
        // back after wakeup.
        switch (static_cast<StateChange>(buffer[0])) {
        case StateChange::Initialize:
            MH4N_LOG(Info, kLog, "Application has requested initialization of DSP hardware");
            Initialize();
            AudioPipeWriteStructAddresses();
            state_ = DspState::On;
            break;
        case StateChange::Shutdown:
            MH4N_LOG(Info, kLog, "Application has requested shutdown of DSP hardware");
            state_ = DspState::Off;
            break;
        case StateChange::Wakeup:
            MH4N_LOG(Info, kLog, "Application has requested wakeup of DSP hardware");
            Wakeup();
            AudioPipeWriteStructAddresses();
            state_ = DspState::On;
            break;
        case StateChange::Sleep:
            MH4N_LOG(Info, kLog, "Application has requested sleep of DSP hardware");
            Sleep();
            AudioPipeWriteStructAddresses();
            state_ = DspState::Sleeping;
            break;
        default:
            MH4N_LOG(Error, kLog,
                     "Application has requested unknown state transition of DSP hardware {}",
                     buffer[0]);
            state_ = DspState::Off;
            break;
        }
        return;
    }
    case DspPipe::Binary: {
        // Azahar: "TODO(B3N30): Make this async, and signal the interrupt".
        if (buffer.size() != kBinaryMessageSize) {
            MH4N_LOG(Error, kLog, "got binary pipe with wrong size {}", buffer.size());
            return;
        }
        const u16 codec = Load<u16>(buffer.data());
        if (codec != kCodecDecodeAac) {
            MH4N_LOG(Error, kLog, "got unknown codec {}", codec);
            return;
        }
        WriteBinaryResponse(buffer);
        RaiseInterrupt(InterruptType::Pipe, DspPipe::Binary);
        return;
    }
    default:
        MH4N_LOG(Error, kLog, "pipe_number = {} unimplemented", pipe);
        return;
    }
}

// AACDecoder::ProcessRequest, with no decoder behind it: Decode answers as Azahar's does when the
// decoding fails ("a hack to continue games when a failure occurs").
void DspModel::WriteBinaryResponse(std::span<const u8> request) {
    u8 response[kBinaryMessageSize] = {};
    const u16 cmd = Load<u16>(request.data() + 2);
    switch (cmd) {
    case kCommandInit:
        std::memcpy(response, request.data(), kBinaryMessageSize);
        Store<u32>(response + 4, kResultStatusSuccess);
        break;
    case kCommandEncodeDecode:
        // header: the request's codec and command, the result left at its default (Error);
        // data: sample_rate 48000 (0), num_channels 2, size as asked, num_samples 1024.
        std::memcpy(response, request.data(), 4);
        Store<u32>(response + 4, kResultStatusError);
        Store<u32>(response + 8 + 0, 0);
        Store<u32>(response + 8 + 4, 2);
        Store<u32>(response + 8 + 8, Load<u32>(request.data() + 8 + 4));
        Store<u32>(response + 8 + 20, 1024);
        MH4N_LOG(Warning, kLog, "binary pipe: an AAC frame was not decoded (the model has no decoder)");
        break;
    case 2:  // Shutdown
    case 3:  // LoadState
    case 4:  // SaveState
        MH4N_LOG(Warning, kLog, "Got unimplemented AAC binary request: {}", cmd);
        std::memcpy(response, request.data(), kBinaryMessageSize);
        Store<u32>(response + 4, kResultStatusSuccess);
        break;
    default:
        MH4N_LOG(Error, kLog, "Got unknown AAC binary request: {}", cmd);
        Store<u32>(response + 4, kResultStatusError);  // codec and command 0, as Azahar's
        break;
    }
    std::vector<u8>& data = pipes_[static_cast<std::size_t>(DspPipe::Binary)];
    data.resize(kBinaryMessageSize);
    std::memcpy(data.data(), response, kBinaryMessageSize);
}

void DspModel::RaiseInterrupt(InterruptType type, DspPipe pipe) {
    if (interrupt_) {
        interrupt_(type, pipe);
    }
}

void DspModel::Initialize() {
    // Azahar's own words: "This is NOT the right way to do this, but it is close enough. This makes
    // sure the DSP state is clean and consistent every time the HW is initialized."
    std::memset(memory_, 0, kDspMemorySize);
    mixers_ = MixerState{};
    for (SourceModel& source : sources_) {
        source.Reset();
    }
    ResetPipes();
}

void DspModel::Sleep() {
    backup_memory_.assign(memory_, memory_ + kDspMemorySize);
    mixers_backup_ = mixers_;
    for (SourceModel& source : sources_) {
        source.Sleep();
    }
}

void DspModel::Wakeup() {
    if (backup_memory_.empty()) {
        std::memset(memory_, 0, kDspMemorySize);  // a wakeup without a sleep restores zeros
    } else {
        std::memcpy(memory_, backup_memory_.data(), kDspMemorySize);
    }
    backup_memory_.clear();
    mixers_ = mixers_backup_;
    mixers_backup_ = MixerState{};
    for (SourceModel& source : sources_) {
        source.Wakeup();
    }
    ResetPipes();
}

void DspModel::ResetPipes() {
    for (std::vector<u8>& data : pipes_) {
        data.clear();
    }
}

void DspModel::WriteU16(DspPipe pipe, u16 value) {
    std::vector<u8>& data = pipes_[static_cast<std::size_t>(pipe)];
    data.push_back(static_cast<u8>(value & 0xFF));  // little endian
    data.push_back(static_cast<u8>(value >> 8));
}

void DspModel::AudioPipeWriteStructAddresses() {
    // These struct addresses are DSP DRAM addresses (see ConvertProcessAddressFromDspDram), in the
    // order the audio pipe lists them.
    static constexpr std::array<u16, 15> struct_addresses = {
        0x8000 + shm::kFrameCounter / 2,
        0x8000 + shm::kSourceConfigurations / 2,
        0x8000 + shm::kSourceStatuses / 2,
        0x8000 + shm::kAdpcmCoefficients / 2,
        0x8000 + shm::kDspConfiguration / 2,
        0x8000 + shm::kDspStatus / 2,
        0x8000 + shm::kFinalSamples / 2,
        0x8000 + shm::kIntermediateMixSamples / 2,
        0x8000 + shm::kCompressor / 2,
        0x8000 + shm::kDspDebug / 2,
        0x8000 + shm::kUnknown10 / 2,
        0x8000 + shm::kUnknown11 / 2,
        0x8000 + shm::kUnknown12 / 2,
        0x8000 + shm::kUnknown13 / 2,
        0x8000 + shm::kUnknown14 / 2,
    };
    // Begin with a u16 denoting the number of structs, then write the struct addresses.
    WriteU16(DspPipe::Audio, static_cast<u16>(struct_addresses.size()));
    for (const u16 address : struct_addresses) {
        WriteU16(DspPipe::Audio, address);
    }
    // Signal that we have data on this pipe.
    RaiseInterrupt(InterruptType::Pipe, DspPipe::Audio);
}

std::size_t DspModel::CurrentRegionIndex() const {
    // The region with the higher frame counter is chosen unless there is wraparound.
    const u16 frame_counter_0 = Load<u16>(Region(0) + shm::kFrameCounter);
    const u16 frame_counter_1 = Load<u16>(Region(1) + shm::kFrameCounter);
    if (frame_counter_0 == 0xFFFFu && frame_counter_1 != 0xFFFEu) {
        return 1;  // wraparound has occurred
    }
    if (frame_counter_1 == 0xFFFFu && frame_counter_0 != 0xFFFEu) {
        return 0;  // wraparound has occurred
    }
    return frame_counter_0 > frame_counter_1 ? 0 : 1;
}

bool DspModel::Tick() {
    if (state_ != DspState::On) {
        return false;
    }
    // Azahar's TODO: check the dsp::DSP semaphore (the application has finished writing the region).
    if (frame_start_) {
        frame_start_(memory_);
    }
    GenerateFrame();
    return true;
}

void DspModel::GenerateFrame() {
    const std::size_t read_index = CurrentRegionIndex();
    u8* const read = Region(read_index);
    u8* const write = Region(read_index == 0 ? 1 : 0);

    // The sources: their configuration in the region read, their status in the other.
    for (u32 i = 0; i < kNumSources; ++i) {
        u8* const at = read + shm::kSourceConfigurations + i * sizeof(SourceConfiguration);
        SourceConfiguration config;
        std::memcpy(&config, at, sizeof(config));
        const u32 original_dirty = config.dirty_raw;
        const SourceStatus status = sources_[i].Tick(config, i);
        if (original_dirty != 0) {
            // The DSP clears the dirty bits and the queue's: the only fields the source changes.
            Store<u32>(at + offsetof(SourceConfiguration, dirty_raw), config.dirty_raw);
            Store<u16>(at + offsetof(SourceConfiguration, buffers_dirty), config.buffers_dirty);
        }
        std::memcpy(write + shm::kSourceStatuses + i * sizeof(SourceStatus), &status, sizeof(status));
    }

    // The mixers: the configuration is read (and its dirty bits cleared), the aux buses of the
    // intermediate mixes are written, the DSP status is written; the final mix would be written to
    // the final samples.
    ParseMixerConfig(read + shm::kDspConfiguration);
    for (u32 bus = 0; bus < 2; ++bus) {
        if (mixers_.aux_bus_enable[bus]) {
            std::memset(write + shm::kIntermediateMixSamples + bus * shm::kIntermediateMixSize, 0,
                        shm::kIntermediateMixSize);
        }
    }
    // DspStatus{unknown = 0, dropped_frames = 0}
    Store<u16>(write + shm::kDspStatus, 0);
    Store<u16>(write + shm::kDspStatus + 2, 0);
    std::memset(write + shm::kFinalSamples, 0, shm::kFinalSamplesSize);
}

// Mixers::ParseConfig, to the extent that it changes the memory or the model's state: the aux bus
// enables, and the dirty bits (the delay effects' own dirty words included).
void DspModel::ParseMixerConfig(u8* configuration) {
    const u32 bits = Load<u32>(configuration + dspconfig::kDirtyRaw);
    if (bits == 0) {
        return;
    }
    if (bits & dspconfig::kAuxBusEnable0Dirty) {
        mixers_.aux_bus_enable[0] = Load<u16>(configuration + dspconfig::kAuxBusEnable) != 0;
    }
    if (bits & dspconfig::kAuxBusEnable1Dirty) {
        mixers_.aux_bus_enable[1] = Load<u16>(configuration + dspconfig::kAuxBusEnable + 2) != 0;
    }
    // DelayEffect::ParseConfig leaves its dirty word at 0 on every path.
    if (bits & dspconfig::kDelayEffect0Dirty) {
        Store<u16>(configuration + dspconfig::kDelayEffect, 0);
    }
    if (bits & dspconfig::kDelayEffect1Dirty) {
        Store<u16>(configuration + dspconfig::kDelayEffect + dspconfig::kDelayEffectSize, 0);
    }
    Store<u32>(configuration + dspconfig::kDirtyRaw, 0);
}

}  // namespace mh4n::audio
