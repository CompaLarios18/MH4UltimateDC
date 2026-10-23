// Derived from Azahar src/audio_core/hle/hle.cpp, src/audio_core/hle/aac_decoder.cpp,
// src/audio_core/hle/decoder.h, src/audio_core/hle/shared_memory.h and src/audio_core/audio_types.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or
// any later version"): DspHle::Impl::RecvData, RecvDataIsReady, PipeRead, GetPipeReadableSize,
// PipeWrite, Initialize, Sleep, Wakeup, ResetPipes, WriteU16, AudioPipeWriteStructAddresses,
// CurrentRegionIndex, GenerateCurrentFrame and Tick, and AACDecoder::ProcessRequest and Decode (the
// failure answer).
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (aac_decoder.cpp)
// Copyright 2018-2025 Citra Emulator Project / Azahar Emulator Project (decoder.h)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
// Copyright 2017 Citra Emulator Project (audio_types.h)
//
// Our own HLE DSP (N3-03); see dsp.h.
#include "dsp_hle/dsp.h"

#include <cstring>

#include "mh4n/log.h"

namespace mh4n::dsp_hle {

namespace {

constexpr const char* kLog = "Audio.DSP.HLE";

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

// Memory that is not there: every buffer reads as unmapped.
struct NoSampleMemory final : SampleMemory {
    std::span<const u8> Read(u32, u32) override { return {}; }
};

}  // namespace

Dsp::Dsp(u8* memory) : memory_(memory) {}

u16 Dsp::RecvData(u32 register_number) {
    if (register_number != 0) {
        // Azahar's ASSERT_MSG ends the process here (a departure, as the model's).
        MH4N_LOG(Error, kLog, "Unknown register_number {}", register_number);
    }
    // The application reads this after requesting DSP shutdown, to verify the DSP has indeed shut
    // down or slept.
    return state_ == audio::DspState::On ? 0 : 1;
}

bool Dsp::RecvDataIsReady(u32 register_number) const {
    if (register_number != 0) {
        MH4N_LOG(Error, kLog, "Unknown register_number {}", register_number);
    }
    return true;
}

std::vector<u8> Dsp::PipeRead(u32 pipe, std::size_t length) {
    if (pipe >= audio::kNumPipes) {
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
                 "pipe_number = {} is out of data, application requested read of {} but {} remain", pipe,
                 length, data.size());
        length = data.size();
    }
    if (length == 0) {
        return {};
    }
    std::vector<u8> ret(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(length));
    data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(length));
    return ret;
}

std::size_t Dsp::PipeReadableSize(u32 pipe) const {
    if (pipe >= audio::kNumPipes) {
        MH4N_LOG(Error, kLog, "pipe_number = {} invalid", pipe);
        return 0;
    }
    return pipes_[pipe].size();
}

void Dsp::PipeWrite(u32 pipe, std::span<const u8> buffer) {
    switch (static_cast<audio::DspPipe>(pipe)) {
    case audio::DspPipe::Audio: {
        if (buffer.size() != 4) {
            MH4N_LOG(Error, kLog, "DspPipe::Audio: Unexpected buffer length {} was written", buffer.size());
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
            state_ = audio::DspState::On;
            break;
        case StateChange::Shutdown:
            MH4N_LOG(Info, kLog, "Application has requested shutdown of DSP hardware");
            state_ = audio::DspState::Off;
            break;
        case StateChange::Wakeup:
            MH4N_LOG(Info, kLog, "Application has requested wakeup of DSP hardware");
            Wakeup();
            AudioPipeWriteStructAddresses();
            state_ = audio::DspState::On;
            break;
        case StateChange::Sleep:
            MH4N_LOG(Info, kLog, "Application has requested sleep of DSP hardware");
            Sleep();
            AudioPipeWriteStructAddresses();
            state_ = audio::DspState::Sleeping;
            break;
        default:
            MH4N_LOG(Error, kLog, "Application has requested unknown state transition of DSP hardware {}",
                     buffer[0]);
            state_ = audio::DspState::Off;
            break;
        }
        return;
    }
    case audio::DspPipe::Binary: {
        // Azahar: "TODO(B3N30): Make this async, and signal the interrupt".
        if (buffer.size() != audio::kBinaryMessageSize) {
            MH4N_LOG(Error, kLog, "got binary pipe with wrong size {}", buffer.size());
            return;
        }
        const u16 codec = Load<u16>(buffer.data());
        if (codec != audio::kCodecDecodeAac) {
            MH4N_LOG(Error, kLog, "got unknown codec {}", codec);
            return;
        }
        WriteBinaryResponse(buffer);
        RaiseInterrupt(audio::InterruptType::Pipe, audio::DspPipe::Binary);
        return;
    }
    default:
        MH4N_LOG(Error, kLog, "pipe_number = {} unimplemented", pipe);
        return;
    }
}

// AACDecoder::ProcessRequest, with no decoder behind it: Decode answers as Azahar's does when the
// decoding fails ("a hack to continue games when a failure occurs").
void Dsp::WriteBinaryResponse(std::span<const u8> request) {
    u8 response[audio::kBinaryMessageSize] = {};
    const u16 cmd = Load<u16>(request.data() + 2);
    switch (cmd) {
    case audio::kCommandInit:
        std::memcpy(response, request.data(), audio::kBinaryMessageSize);
        Store<u32>(response + 4, audio::kResultStatusSuccess);
        break;
    case audio::kCommandEncodeDecode:
        // header: the request's codec and command, the result left at its default (Error);
        // data: sample_rate 48000 (0), num_channels 2, size as asked, num_samples 1024.
        std::memcpy(response, request.data(), 4);
        Store<u32>(response + 4, audio::kResultStatusError);
        Store<u32>(response + 8 + 0, 0);
        Store<u32>(response + 8 + 4, 2);
        Store<u32>(response + 8 + 8, Load<u32>(request.data() + 8 + 4));
        Store<u32>(response + 8 + 20, 1024);
        if (aac_decode_requests_ == 0) {
            MH4N_LOG(Warning, kLog, "dsp: AAC decode requested (not supported)");
        }
        ++aac_decode_requests_;
        break;
    case 2:  // Shutdown
    case 3:  // LoadState
    case 4:  // SaveState
        MH4N_LOG(Warning, kLog, "Got unimplemented AAC binary request: {}", cmd);
        std::memcpy(response, request.data(), audio::kBinaryMessageSize);
        Store<u32>(response + 4, audio::kResultStatusSuccess);
        break;
    default:
        MH4N_LOG(Error, kLog, "Got unknown AAC binary request: {}", cmd);
        Store<u32>(response + 4, audio::kResultStatusError);  // codec and command 0, as Azahar's
        break;
    }
    std::vector<u8>& data = pipes_[static_cast<std::size_t>(audio::DspPipe::Binary)];
    data.resize(audio::kBinaryMessageSize);
    std::memcpy(data.data(), response, audio::kBinaryMessageSize);
}

void Dsp::RaiseInterrupt(audio::InterruptType type, audio::DspPipe pipe) {
    if (interrupt_) {
        interrupt_(type, pipe);
    }
}

void Dsp::Initialize() {
    // Azahar's own words: "This is NOT the right way to do this, but it is close enough. This makes
    // sure the DSP state is clean and consistent every time the HW is initialized."
    std::memset(memory_, 0, audio::kDspMemorySize);
    mixers_.Reset();
    for (Source& source : sources_) {
        source.Reset();
    }
    ResetPipes();
}

void Dsp::Sleep() {
    backup_memory_.assign(memory_, memory_ + audio::kDspMemorySize);
    mixers_.Sleep();
    for (Source& source : sources_) {
        source.Sleep();
    }
}

void Dsp::Wakeup() {
    if (backup_memory_.empty()) {
        std::memset(memory_, 0, audio::kDspMemorySize);  // a wakeup without a sleep restores zeros
    } else {
        std::memcpy(memory_, backup_memory_.data(), audio::kDspMemorySize);
    }
    backup_memory_.clear();
    mixers_.Wakeup();
    for (Source& source : sources_) {
        source.Wakeup();
    }
    ResetPipes();
}

void Dsp::ResetPipes() {
    for (std::vector<u8>& data : pipes_) {
        data.clear();
    }
}

void Dsp::WriteU16(audio::DspPipe pipe, u16 value) {
    std::vector<u8>& data = pipes_[static_cast<std::size_t>(pipe)];
    data.push_back(static_cast<u8>(value & 0xFF));  // little endian
    data.push_back(static_cast<u8>(value >> 8));
}

void Dsp::AudioPipeWriteStructAddresses() {
    // These struct addresses are DSP DRAM addresses (see ConvertProcessAddressFromDspDram), in the
    // order the audio pipe lists them.
    using namespace audio;
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

std::size_t Dsp::CurrentRegionIndex() const {
    // The region with the higher frame counter is chosen unless there is wraparound.
    const u16 frame_counter_0 = Load<u16>(Region(0) + audio::shm::kFrameCounter);
    const u16 frame_counter_1 = Load<u16>(Region(1) + audio::shm::kFrameCounter);
    if (frame_counter_0 == 0xFFFFu && frame_counter_1 != 0xFFFEu) {
        return 1;  // wraparound has occurred
    }
    if (frame_counter_1 == 0xFFFFu && frame_counter_0 != 0xFFFEu) {
        return 0;  // wraparound has occurred
    }
    return frame_counter_0 > frame_counter_1 ? 0 : 1;
}

bool Dsp::Tick() {
    if (state_ != audio::DspState::On) {
        return false;
    }
    // Azahar's TODO: check the dsp::DSP semaphore (the application has finished writing the region).
    if (frame_start_) {
        frame_start_(memory_);
    }
    GenerateCurrentFrame();
    return true;
}

void Dsp::GenerateCurrentFrame() {
    using namespace audio;
    const std::size_t read_index = CurrentRegionIndex();
    u8* const read = Region(read_index);
    u8* const write = Region(read_index == 0 ? 1 : 0);

    NoSampleMemory no_memory;
    SampleMemory& sample_memory = sample_memory_ != nullptr ? *sample_memory_ : no_memory;

    AdpcmCoefficients coefficients;
    std::memcpy(&coefficients, read + shm::kAdpcmCoefficients, sizeof(coefficients));

    std::array<QuadFrame32, 3> intermediate_mixes = {};

    // Generate intermediate mixes
    for (u32 i = 0; i < kNumSources; i++) {
        u8* const at = read + shm::kSourceConfigurations + i * sizeof(SourceConfiguration);
        SourceConfiguration config;
        std::memcpy(&config, at, sizeof(config));
        const SourceStatus status = sources_[i].Tick(config, coefficients, sample_memory, i);
        // The DSP clears the dirty bits and the queue's: the only fields the source changes.
        Store<u32>(at + offsetof(SourceConfiguration, dirty_raw), config.dirty_raw);
        Store<u16>(at + offsetof(SourceConfiguration, buffers_dirty), config.buffers_dirty);
        std::memcpy(write + shm::kSourceStatuses + i * sizeof(SourceStatus), &status, sizeof(status));
        for (std::size_t mix = 0; mix < 3; mix++) {
            sources_[i].MixInto(intermediate_mixes[mix], mix);
        }
    }

    // Generate final mix
    DspConfiguration config;
    IntermediateMixSamples read_samples;
    IntermediateMixSamples write_samples;
    std::memcpy(&config, read + shm::kDspConfiguration, sizeof(config));
    std::memcpy(&read_samples, read + shm::kIntermediateMixSamples, sizeof(read_samples));
    std::memcpy(&write_samples, write + shm::kIntermediateMixSamples, sizeof(write_samples));
    const DspStatus dsp_status = mixers_.Tick(config, read_samples, write_samples, intermediate_mixes);
    std::memcpy(read + shm::kDspConfiguration, &config, sizeof(config));
    std::memcpy(write + shm::kIntermediateMixSamples, &write_samples, sizeof(write_samples));
    Store<u16>(write + shm::kDspStatus, dsp_status.unknown);
    Store<u16>(write + shm::kDspStatus + 2, dsp_status.dropped_frames);

    const StereoFrame16 output_frame = mixers_.GetOutput();

    // Write current output frame to the shared memory region
    std::array<s16, kSamplesPerFrame * 2> samples;
    for (std::size_t samplei = 0; samplei < output_frame.size(); samplei++) {
        for (std::size_t channeli = 0; channeli < output_frame[0].size(); channeli++) {
            samples[samplei * 2 + channeli] = output_frame[samplei][channeli];
        }
    }
    std::memcpy(write + shm::kFinalSamples, samples.data(), sizeof(samples));

    if (frame_output_) {
        frame_output_(samples);
    }
}

}  // namespace mh4n::dsp_hle
