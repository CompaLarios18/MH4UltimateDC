// Derived from Azahar src/audio_core/hle/hle.cpp, src/audio_core/hle/shared_memory.h,
// src/audio_core/hle/source.cpp, src/audio_core/interpolate.cpp, src/audio_core/codec.cpp,
// src/audio_core/hle/aac_decoder.cpp and src/core/hle/service/dsp/dsp_dsp.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2
// or any later version"): the DSP addresses of the structures (the table in shared_memory.h), the
// ticks of an audio frame and the rule for the region the DSP reads (hle.cpp), the way a source steps
// through its buffers, written here again with the sample data kept (source.cpp, interpolate.cpp,
// codec.cpp) to check the model against, the answer of the binary pipe (aac_decoder.cpp) and the
// commands of dsp::DSP with their headers, replies and results (dsp_dsp.cpp).
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (source.cpp)
// Copyright 2016 Citra Emulator Project (interpolate.cpp, codec.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (aac_decoder.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (dsp_dsp.cpp)
//
// dsp::DSP without sound (N1-18): the model of the DSP on synthetic shared memory, checked against a
// transcription of Azahar's source that keeps the sample data; the service through the SVC layer, the
// frame timer on the game clock, the null sink and --capture-dsp read back by tools/native/capfmt.py.
// No game data: the memory, the buffers and the programs are made here.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "audio/dsp_layout.h"
#include "audio/dsp_model.h"
#include "audio/mdsp_writer.h"
#include "mh4n/crc32.h"
#include "mh4n/host.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"

using namespace mh4n;
using namespace mh4n::audio;

namespace {

// ---- the model on synthetic memory ---------------------------------------------------------------------

constexpr u32 kValidAddress = 0x20000000;    // FCRAM
constexpr u32 kInvalidAddress = 0x08000000;  // nothing is there

struct Message {
    static std::vector<u8> Audio(u8 state_change) { return {state_change, 0, 0, 0}; }
};

struct Interrupt {
    InterruptType type;
    DspPipe pipe;
    friend bool operator==(const Interrupt&, const Interrupt&) = default;
};

// Captures what the log says during a scope.
struct LogCatcher {
    std::vector<std::string> lines;
    LogCatcher() {
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { lines.push_back(std::string(r.message)); });
    }
    ~LogCatcher() {
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    int Count(const std::string& needle) const {
        int n = 0;
        for (const std::string& line : lines) {
            n += line.find(needle) != std::string::npos ? 1 : 0;
        }
        return n;
    }
};

// A DSP over its own 512 KiB, recording the interrupts it raises (and the log, which a test reads).
struct Dsp {
    LogCatcher log;
    std::vector<u8> memory = std::vector<u8>(kDspMemorySize);
    DspModel model{memory.data()};
    std::vector<Interrupt> interrupts;

    Dsp() {
        model.SetInterruptHandler(
            [this](InterruptType type, DspPipe pipe) { interrupts.push_back({type, pipe}); });
    }
    u8* Region(std::size_t index) { return memory.data() + kRegionOffset[index]; }
    void Initialize() { model.PipeWrite(static_cast<u32>(DspPipe::Audio), Message::Audio(0)); }

    void SetCounter(std::size_t region, u16 value) {
        std::memcpy(Region(region) + shm::kFrameCounter, &value, 2);
    }
    SourceConfiguration Config(std::size_t region, u32 source) {
        SourceConfiguration config;
        std::memcpy(&config, Region(region) + shm::kSourceConfigurations + source * sizeof(config),
                    sizeof(config));
        return config;
    }
    void SetConfig(std::size_t region, u32 source, const SourceConfiguration& config) {
        std::memcpy(Region(region) + shm::kSourceConfigurations + source * sizeof(config), &config,
                    sizeof(config));
    }
    SourceStatus Status(std::size_t region, u32 source) {
        SourceStatus status;
        std::memcpy(&status, Region(region) + shm::kSourceStatuses + source * sizeof(status),
                    sizeof(status));
        return status;
    }
};

// The fields of a status the tests compare: enabled, "buffer changed", sync count, position, ids.
struct Seen {
    u32 enabled, dirty, sync, position, current, last;
    friend bool operator==(const Seen&, const Seen&) = default;
};
Seen Look(const SourceStatus& s) {
    return {s.is_enabled, s.current_buffer_id_dirty, s.sync_count, SwapHalves(s.buffer_position),
            s.current_buffer_id, s.last_buffer_id};
}
std::ostream& operator<<(std::ostream& out, const Seen& s) {
    return out << "{enabled " << s.enabled << ", dirty " << s.dirty << ", sync " << s.sync
               << ", position " << s.position << ", current " << s.current << ", last " << s.last
               << "}";
}

// Builders of the dirty configuration the application writes.
enum Format : u16 { kPcm8 = 0, kPcm16 = 1, kAdpcm = 2 };

void SetFormat(SourceConfiguration& c, Format format, u16 mono_or_stereo = 1) {
    c.flags1_raw = static_cast<u16>((format << 2) | mono_or_stereo);
    c.dirty_raw |= dirty::kFormat | dirty::kMonoOrStereo;
}
void SetEnable(SourceConfiguration& c, bool on) {
    c.enable = on ? 1 : 0;
    c.dirty_raw |= dirty::kEnable;
}
void SetRate(SourceConfiguration& c, float rate) {
    c.rate_multiplier = rate;
    c.dirty_raw |= dirty::kRateMultiplier;
}
void SetSync(SourceConfiguration& c, u16 sync) {
    c.sync_count = sync;
    c.dirty_raw |= dirty::kSyncCount;
}
void Queue(SourceConfiguration& c, u32 slot, u32 address, u32 length, u16 id, bool looping = false) {
    QueuedBufferConfig& b = c.buffers[slot];
    b = {};
    b.physical_address = SwapHalves(address);
    b.length = SwapHalves(length);
    b.is_looping = looping ? 1 : 0;
    b.buffer_id = id;
    c.buffers_dirty = static_cast<u16>(c.buffers_dirty | (1u << slot));
    c.dirty_raw |= dirty::kBufferQueue;
}
void SetEmbedded(SourceConfiguration& c, u32 address, u32 length, u16 id, bool looping = false) {
    c.physical_address = SwapHalves(address);
    c.length = SwapHalves(length);
    c.buffer_id = id;
    c.flags2_raw = static_cast<u16>((c.flags2_raw & ~2u) | (looping ? 2u : 0u));
    c.dirty_raw |= dirty::kEmbeddedBuffer;
}
void SetPlayPosition(SourceConfiguration& c, u32 position) {
    c.play_position = SwapHalves(position);
    c.dirty_raw |= dirty::kPlayPosition;
}

// ---- Azahar's source with the sample data kept ------------------------------------------------------------
//
// A transcription of Source::ParseConfig, GenerateFrame, DequeueBuffer and GetCurrentStatus, of
// AudioInterp::StepOverSamples and of the sizes Codec::Decode* make, with the samples reduced to
// placeholders: it is what the model's counting must agree with. It works on the same configuration
// words as the model, and clears the dirty bits as Azahar does, one by one.

namespace oracle {

using Sample = std::array<s16, 2>;
using Samples = std::deque<Sample>;
constexpr u64 scale_factor = 1 << 24;

struct Interp {
    Sample xn1{};
    Sample xn2{};
    u64 fposition = 0;
};

void StepOverSamples(Interp& state, Samples& input, float rate, std::size_t& outputi) {
    if (input.empty())
        return;
    input.insert(input.begin(), {state.xn2, state.xn1});
    const u64 step_size = static_cast<u64>(rate * scale_factor);
    u64 fposition = state.fposition;
    std::size_t inputi = 0;
    while (outputi < kSamplesPerFrame) {
        inputi = static_cast<std::size_t>(fposition / scale_factor);
        if (inputi + 2 >= input.size()) {
            inputi = input.size() - 2;
            break;
        }
        ++outputi;
        fposition += step_size;
    }
    state.xn2 = input[inputi];
    state.xn1 = input[inputi + 1];
    state.fposition = fposition - inputi * scale_factor;
    input.erase(input.begin(), std::next(input.begin(), static_cast<std::ptrdiff_t>(inputi + 2)));
}

bool Valid(u32 address) {
    return address >= kValidAddress;
}

struct Buffer {
    u32 physical_address;
    u32 length;
    bool is_looping;
    u16 buffer_id;
    u8 format;
    bool from_queue;
    u32 play_position;
    bool has_played;
};
struct BufferOrder {
    bool operator()(const Buffer& a, const Buffer& b) const { return a.buffer_id > b.buffer_id; }
};

struct Source {
    bool enabled = false;
    u16 sync_count = 0;
    std::priority_queue<Buffer, std::vector<Buffer>, BufferOrder> input_queue;
    u8 format = kAdpcm;
    u32 current_sample_number = 0;
    u32 current_buffer_physical_address = 0;
    u32 current_buffer_length = 0;
    bool current_buffer_is_looping = false;
    u8 current_buffer_format = kAdpcm;
    Samples current_buffer;
    bool buffer_update = false;
    u16 last_buffer_id = 0;
    u16 current_buffer_id = 0;
    float rate_multiplier = 1.0f;
    Interp interp_state;

    static Samples Decode(u8 format, u32 sample_count) {
        // DecodeADPCM makes a multiple of two samples; PCM8 and PCM16 make sample_count.
        return Samples(format == kAdpcm ? sample_count + sample_count % 2 : sample_count);
    }

    SourceStatus Tick(SourceConfiguration& config) {
        ParseConfig(config);
        if (enabled) {
            GenerateFrame();
        }
        return GetCurrentStatus();
    }

    void ParseConfig(SourceConfiguration& config) {
        if (!config.dirty_raw) {
            return;
        }
        auto take = [&](u32 bit) {
            const bool was = (config.dirty_raw & bit) != 0;
            config.dirty_raw &= ~bit;
            return was;
        };
        if (take(dirty::kReset)) {
            *this = Source{};
        }
        if (take(dirty::kPartialReset)) {
            input_queue = {};
        }
        if (take(dirty::kEnable)) {
            enabled = config.enable != 0;
        }
        if (take(dirty::kSyncCount)) {
            sync_count = config.sync_count;
        }
        if (take(dirty::kRateMultiplier)) {
            rate_multiplier = config.rate_multiplier;
            if (rate_multiplier <= 0) {
                rate_multiplier = 1.0f;
            }
        }
        // (the ADPCM coefficients, gains, filters and interpolation: only their bits)
        take(dirty::kAdpcmCoefficients | dirty::kGain0 | dirty::kGain1 | dirty::kGain2 |
             dirty::kFiltersEnabled | dirty::kSimpleFilter | dirty::kBiquadFilter |
             dirty::kInterpolation);
        if ((config.dirty_raw & dirty::kFormat) || (config.dirty_raw & dirty::kEmbeddedBuffer)) {
            config.dirty_raw &= ~dirty::kFormat;
            format = static_cast<u8>((config.flags1_raw >> 2) & 3);
        }
        if ((config.dirty_raw & dirty::kMonoOrStereo) || (config.dirty_raw & dirty::kEmbeddedBuffer)) {
            config.dirty_raw &= ~dirty::kMonoOrStereo;
        }
        u32 play_position = 0;
        if (take(dirty::kPlayPosition)) {
            play_position = SwapHalves(config.play_position);
        }
        if (take(dirty::kPartialEmbeddedBuffer)) {
            if (Valid(current_buffer_physical_address & 0xFFFFFFFC)) {
                bool valid = false;
                switch (format) {
                case kPcm16:
                    current_buffer = Decode(kPcm16, SwapHalves(config.length));
                    current_buffer_length = SwapHalves(config.length);
                    current_buffer_format = format;
                    current_buffer_is_looping = (config.flags2_raw & 2) != 0;
                    valid = true;
                    break;
                default:
                    FAIL("the generator must not make a partial update of a format Azahar does not handle");
                }
                if (valid) {
                    if (current_buffer.size() < current_sample_number) {
                        current_sample_number = 0;
                    } else {
                        current_buffer.erase(current_buffer.begin(),
                                             std::next(current_buffer.begin(), current_sample_number));
                    }
                }
            }
        }
        if (take(dirty::kEmbeddedBuffer)) {
            if (static_cast<s32>(SwapHalves(config.length)) >= 0) {
                input_queue.push(Buffer{SwapHalves(config.physical_address), SwapHalves(config.length),
                                        (config.flags2_raw & 2) != 0, config.buffer_id, format, false,
                                        play_position, false});
            }
        }
        take(dirty::kLoopRelated);
        if (take(dirty::kBufferQueue)) {
            for (std::size_t i = 0; i < 4; i++) {
                if (config.buffers_dirty & (1 << i)) {
                    const auto& b = config.buffers[i];
                    if (static_cast<s32>(SwapHalves(b.length)) >= 0) {
                        input_queue.push(Buffer{SwapHalves(b.physical_address), SwapHalves(b.length),
                                                b.is_looping != 0, b.buffer_id, format, true, 0,
                                                false});
                    }
                }
            }
            config.buffers_dirty = 0;
        }
        config.dirty_raw = 0;
    }

    void GenerateFrame() {
        if (!current_buffer.empty() && current_buffer_is_looping && current_buffer_format == kPcm16 &&
            current_buffer_physical_address != 0 && current_sample_number < current_buffer_length) {
            const u32 byte_offset = current_sample_number * 1 * 2;  // the generator uses mono
            if (Valid((current_buffer_physical_address & 0xFFFFFFFC) + byte_offset)) {
                current_buffer = Decode(kPcm16, current_buffer_length - current_sample_number);
            }
        }
        if (current_buffer.empty()) {
            if (DequeueBuffer()) {
                return;
            }
            enabled = false;
            buffer_update = true;
            last_buffer_id = current_buffer_id;
            current_buffer_id = 0;
            return;
        }
        std::size_t frame_position = 0;
        while (frame_position < kSamplesPerFrame) {
            if (current_buffer.empty() && !DequeueBuffer()) {
                break;
            }
            const std::size_t input_size_before = current_buffer.size();
            StepOverSamples(interp_state, current_buffer, rate_multiplier, frame_position);
            current_sample_number += static_cast<u32>(input_size_before - current_buffer.size());
        }
    }

    bool DequeueBuffer() {
        if (input_queue.empty())
            return false;
        Buffer buf = input_queue.top();
        input_queue.pop();
        if (Valid(buf.physical_address & 0xFFFFFFFC)) {
            current_buffer = Decode(buf.format, buf.length);
        } else {
            current_buffer.clear();
            return true;
        }
        current_sample_number = (!buf.has_played) ? buf.play_position : 0;
        current_buffer_physical_address = buf.physical_address;
        current_buffer_length = buf.length;
        current_buffer_format = buf.format;
        current_buffer_is_looping = buf.is_looping;
        current_buffer_id = buf.buffer_id;
        last_buffer_id = 0;
        buffer_update = buf.from_queue && !buf.has_played;
        if (buf.is_looping) {
            buf.has_played = true;
            input_queue.push(buf);
        }
        current_buffer.erase(current_buffer.begin(),
                             std::next(current_buffer.begin(), current_sample_number));
        return true;
    }

    SourceStatus GetCurrentStatus() {
        SourceStatus ret{};
        ret.is_enabled = enabled;
        ret.current_buffer_id_dirty = buffer_update ? 1 : 0;
        buffer_update = false;
        ret.sync_count = sync_count;
        ret.buffer_position = SwapHalves(current_sample_number);
        ret.current_buffer_id = current_buffer_id;
        ret.last_buffer_id = last_buffer_id;
        return ret;
    }
};

}  // namespace oracle

// A deterministic random sequence.
struct Lcg {
    u64 state;
    explicit Lcg(u64 seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    u32 Next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(state >> 33);
    }
    u32 Below(u32 n) { return Next() % n; }
    bool Chance(u32 percent) { return Below(100) < percent; }
};

// The words the audio pipe holds after Initialize: the count, then the DSP word addresses of the
// structures of the first region in the table of shared_memory.h's comment, in the order the pipe lists
// them.
const std::vector<u16> kAudioPipeWords = {
    15,                                      // the count
    0xBFFF,                                  // 0: frame counter
    0x9E92,                                  // 1: source configurations
    0x8680,                                  // 2: source statuses
    0xA792,                                  // 3: ADPCM coefficients
    0x9430,                                  // 4: DSP configuration
    0x8400,                                  // 5: DSP status
    0x8540,                                  // 6: final mix samples
    0x9492,                                  // 7: intermediate mix samples
    0x8710,                                  // 8: compressor
    0x8410,                                  // 9: DSP debug
    0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C,  // 10-14: surround
};

}  // namespace

// ---- layout ------------------------------------------------------------------------------------------------

TEST_CASE("dsp: the audio frame is 1,310,720 ticks and the regions are where the capture format says") {
    CHECK(kAudioFrameTicks == 1310720);
    CHECK(kAudioFrameTicks == 160u * 8192u);
    CHECK(kRegionOffset[0] == 0x50000);
    CHECK(kRegionOffset[1] == 0x70000);
    CHECK(kRegionSize == 0x8000);
    CHECK(shm::kSourceStatuses == 0xD00);  // capfmt.py STATUS_OFFSET
    CHECK(shm::kFrameCounter == 0x7FFE);   // capfmt.py COUNTER_OFFSET
    CHECK(sizeof(SourceConfiguration) == 192);
    CHECK(sizeof(SourceStatus) == 12);
}

TEST_CASE("dsp: Initialize on the audio pipe writes the fifteen structure addresses and raises the pipe's interrupt") {
    Dsp dsp;
    CHECK(dsp.model.State() == DspState::Off);
    CHECK(dsp.model.RecvData(0) == 1);  // off: the application's "has it shut down" read says yes
    dsp.Initialize();
    CHECK(dsp.model.State() == DspState::On);
    CHECK(dsp.model.RecvData(0) == 0);
    CHECK(dsp.interrupts == std::vector<Interrupt>{{InterruptType::Pipe, DspPipe::Audio}});
    CHECK(dsp.model.PipeReadableSize(2) == 2 + 15 * 2);

    // The addresses in the table of shared_memory.h's comment, in the order the audio pipe lists them.
    const std::vector<u16>& expected = kAudioPipeWords;
    const std::vector<u8> bytes = dsp.model.PipeRead(2, 32);
    REQUIRE(bytes.size() == 32);
    std::vector<u16> got;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        got.push_back(static_cast<u16>(bytes[i] | (bytes[i + 1] << 8)));
    }
    CHECK(got == expected);
    CHECK(dsp.model.PipeReadableSize(2) == 0);
}

TEST_CASE("dsp: the pipes and the states") {
    Dsp dsp;
    LogCatcher& log = dsp.log;
    // the audio pipe takes exactly four bytes
    dsp.model.PipeWrite(2, std::vector<u8>{0, 0, 0});
    CHECK(dsp.model.State() == DspState::Off);
    CHECK(log.Count("Unexpected buffer length 3") == 1);

    // Initialize clears the whole DSP memory
    std::fill(dsp.memory.begin(), dsp.memory.end(), u8{0xAA});
    dsp.Initialize();
    CHECK(std::all_of(dsp.memory.begin(), dsp.memory.end(), [](u8 b) { return b == 0; }));

    // Sleep keeps a copy, Wakeup brings it back and answers on the pipe again
    dsp.Region(0)[0x123] = 0x5A;
    dsp.model.PipeRead(2, 100);
    dsp.interrupts.clear();
    dsp.model.PipeWrite(2, Message::Audio(3));
    CHECK(dsp.model.State() == DspState::Sleeping);
    CHECK(dsp.model.RecvData(0) == 1);
    CHECK(dsp.interrupts.size() == 1);
    CHECK_FALSE(dsp.model.Tick());  // asleep: nothing happens
    dsp.Region(0)[0x123] = 0;
    dsp.model.PipeRead(2, 100);
    dsp.model.PipeWrite(2, Message::Audio(2));
    CHECK(dsp.model.State() == DspState::On);
    CHECK(dsp.Region(0)[0x123] == 0x5A);
    CHECK(dsp.model.PipeReadableSize(2) == 32);
    CHECK(dsp.interrupts.size() == 2);

    // Shutdown, and a state change that does not exist (which turns it off, as Azahar's does)
    dsp.model.PipeWrite(2, Message::Audio(1));
    CHECK(dsp.model.State() == DspState::Off);
    dsp.model.PipeWrite(2, Message::Audio(0));
    CHECK(dsp.model.State() == DspState::On);
    dsp.model.PipeWrite(2, Message::Audio(9));
    CHECK(dsp.model.State() == DspState::Off);

    // reads: a pipe that does not exist is empty, a read is cut to what the pipe holds
    CHECK(dsp.model.PipeReadableSize(8) == 0);
    CHECK(dsp.model.PipeRead(200, 4).empty());
    dsp.Initialize();
    CHECK(dsp.model.PipeRead(2, 1000).size() == 32);
    CHECK(dsp.model.PipeRead(2, 4).empty());

    // pipes the model does not implement are ignored, not fatal
    dsp.model.PipeWrite(0, std::vector<u8>{1, 2, 3, 4});
    dsp.model.PipeWrite(5, std::vector<u8>{1, 2, 3, 4});
    CHECK(log.Count("unimplemented") == 2);
    // the register
    CHECK(dsp.model.RecvDataIsReady(0));
}

TEST_CASE("dsp: the binary pipe answers like Azahar's AAC decoder") {
    Dsp dsp;
    dsp.Initialize();
    dsp.interrupts.clear();
    auto message = [](u16 codec, u16 cmd, std::initializer_list<u32> words) {
        std::vector<u8> bytes(32, 0);
        std::memcpy(bytes.data(), &codec, 2);
        std::memcpy(bytes.data() + 2, &cmd, 2);
        const u32 result = 1;  // WriteProcessPipe has set the result field to 1
        std::memcpy(bytes.data() + 4, &result, 4);
        std::size_t at = 8;
        for (const u32 word : words) {
            std::memcpy(bytes.data() + at, &word, 4);
            at += 4;
        }
        return bytes;
    };
    auto word = [](const std::vector<u8>& bytes, std::size_t index) {
        u32 value;
        std::memcpy(&value, bytes.data() + index * 4, 4);
        return value;
    };

    // Init: the request comes back with a success result
    dsp.model.PipeWrite(3, message(1, 0, {1, 0xFFFFFFFF, 1, 0, 1, 0x20}));
    CHECK(dsp.interrupts == std::vector<Interrupt>{{InterruptType::Pipe, DspPipe::Binary}});
    std::vector<u8> reply = dsp.model.PipeRead(3, 32);
    REQUIRE(reply.size() == 32);
    CHECK(word(reply, 0) == 0x00000001);  // codec 1, command 0
    CHECK(word(reply, 1) == 0);           // success
    CHECK(word(reply, 2) == 1);
    CHECK(word(reply, 3) == 0xFFFFFFFF);

    // Decode: no decoder, so the answer of a decoder that failed: 48000 Hz, 2 channels, the size
    // asked, 1024 samples, and the default result (error)
    dsp.model.PipeWrite(3, message(1, 1, {0x20001000, 0x300, 0x20002000, 0x20003000}));
    reply = dsp.model.PipeRead(3, 32);
    REQUIRE(reply.size() == 32);
    CHECK(word(reply, 0) == 0x00010001);
    CHECK(word(reply, 1) == 1);
    CHECK(word(reply, 2) == 0);      // Rate48000
    CHECK(word(reply, 3) == 2);      // channels
    CHECK(word(reply, 4) == 0x300);  // size
    CHECK(word(reply, 7) == 1024);   // samples

    // Shutdown / LoadState / SaveState succeed; an unknown command and another codec do not
    dsp.model.PipeWrite(3, message(1, 2, {}));
    CHECK(word(dsp.model.PipeRead(3, 32), 1) == 0);
    dsp.model.PipeWrite(3, message(1, 9, {}));
    reply = dsp.model.PipeRead(3, 32);
    CHECK(word(reply, 0) == 0);
    CHECK(word(reply, 1) == 1);
    const std::size_t raised = dsp.interrupts.size();
    dsp.model.PipeWrite(3, message(2, 0, {}));  // the encoder: not handled, no answer
    CHECK(dsp.model.PipeReadableSize(3) == 0);
    dsp.model.PipeWrite(3, std::vector<u8>(8, 0));  // the wrong size
    CHECK(dsp.model.PipeReadableSize(3) == 0);
    CHECK(dsp.interrupts.size() == raised);
}

TEST_CASE("dsp: the DSP reads the region with the higher frame counter, with the wrap-around rule") {
    Dsp dsp;
    struct Case {
        u16 c0, c1;
        std::size_t read;
    };
    const Case cases[] = {
        {0, 0, 1},           // equal: region 1
        {1, 0, 0},           // the higher one
        {0, 1, 1},
        {5, 7, 1},
        {7, 5, 0},
        {0xFFFF, 0, 1},      // region 0 has wrapped
        {0, 0xFFFF, 0},      // region 1 has wrapped
        {0xFFFF, 0xFFFE, 0},  // not a wrap-around: 0xFFFF is the higher
        {0xFFFE, 0xFFFF, 1},
        {0xFFFF, 0xFFFF, 1},  // both: the first rule does not apply (c1 != 0xFFFE), region 1
    };
    for (const Case& c : cases) {
        INFO("counters " << c.c0 << " " << c.c1);
        dsp.SetCounter(0, c.c0);
        dsp.SetCounter(1, c.c1);
        CHECK(dsp.model.CurrentRegionIndex() == c.read);
    }
}

// ---- the sources ---------------------------------------------------------------------------------------------

// Source 0 of region 1 (the one read when both counters are 0) is configured; the statuses appear in
// region 0.
TEST_CASE("dsp: two queued buffers advance the status as Azahar's source does (worked by hand)") {
    Dsp dsp;
    dsp.Initialize();
    SourceConfiguration config{};
    SetFormat(config, kPcm16);
    SetEnable(config, true);
    SetSync(config, 3);
    Queue(config, 0, kValidAddress, 400, 5);
    Queue(config, 1, kValidAddress + 0x1000, 100, 6);
    dsp.SetConfig(1, 0, config);

    // Frame 1 only dequeues. A rate of 1.0 steps 2^24 per output sample, and the position holds one
    // sample of the fraction of the step back, so a frame consumes 159 samples the first time and 160
    // after: 159, 319, then the last 81 of the first buffer and 79 of the second.
    const Seen expected[] = {
        {1, 1, 3, 0, 5, 0},     // buffer 5 dequeued from the queue: "the buffer changed"
        {1, 0, 3, 159, 5, 0},
        {1, 0, 3, 319, 5, 0},
        {1, 1, 3, 79, 6, 0},    // 400 consumed, buffer 6 dequeued and 79 of it
        {1, 0, 3, 100, 6, 0},   // the rest of it (21); nothing is left to dequeue
        {0, 1, 3, 100, 0, 6},   // the next frame finds the queue empty: stopped, last buffer 6
        {0, 0, 3, 100, 0, 6},
    };
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        INFO("audio frame " << i + 1);
        CHECK(dsp.model.Tick());
        CHECK(Look(dsp.Status(0, 0)) == expected[i]);
    }
    // the DSP cleared what the application had set
    const SourceConfiguration after = dsp.Config(1, 0);
    CHECK(after.dirty_raw == 0);
    CHECK(after.buffers_dirty == 0);
    // the other sources are idle
    CHECK(Look(dsp.Status(0, 1)) == Seen{0, 0, 0, 0, 0, 0});
}

TEST_CASE("dsp: a rate multiplier of one half takes twice as long (worked by hand)") {
    Dsp dsp;
    dsp.Initialize();
    SourceConfiguration config{};
    SetFormat(config, kPcm8);
    SetEnable(config, true);
    SetRate(config, 0.5f);
    SetEmbedded(config, kValidAddress, 100, 9);
    dsp.SetConfig(1, 0, config);
    // embedded buffers are not "from the queue": the first dequeue does not flag a change
    const Seen expected[] = {
        {1, 0, 0, 0, 9, 0},
        {1, 0, 0, 79, 9, 0},
        {1, 0, 0, 100, 9, 0},
        {0, 1, 0, 100, 0, 9},
    };
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        INFO("audio frame " << i + 1);
        CHECK(dsp.model.Tick());
        CHECK(Look(dsp.Status(0, 0)) == expected[i]);
    }
}

TEST_CASE("dsp: a play position starts the embedded buffer there, and an ADPCM buffer has an even size") {
    Dsp dsp;
    dsp.Initialize();
    SourceConfiguration config{};
    SetFormat(config, kAdpcm);
    SetEnable(config, true);
    SetEmbedded(config, kValidAddress, 15, 4);  // 15 samples are decoded as 16
    SetPlayPosition(config, 10);
    dsp.SetConfig(1, 0, config);
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{1, 0, 0, 10, 4, 0});
    // 6 samples are left (16 - 10): input 8, steps of 1: inputi = 0, 1, ... until inputi + 2 >= 8
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{1, 0, 0, 16, 4, 0});
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{0, 1, 0, 16, 0, 4});
}

TEST_CASE("dsp: a buffer at an address nothing is mapped at is skipped, with a warning") {
    Dsp dsp;
    LogCatcher& log = dsp.log;
    dsp.Initialize();
    SourceConfiguration config{};
    SetFormat(config, kPcm16);
    SetEnable(config, true);
    Queue(config, 0, kInvalidAddress, 50, 9);
    Queue(config, 1, kValidAddress, 50, 10);
    dsp.SetConfig(1, 0, config);
    CHECK(dsp.model.Tick());  // buffer 9 is dequeued, found invalid: nothing else changes
    CHECK(Look(dsp.Status(0, 0)) == Seen{1, 0, 0, 0, 0, 0});
    CHECK(log.Count("Invalid physical address 0x08000000") == 1);
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{1, 1, 0, 0, 10, 0});
}

TEST_CASE("dsp: a looping buffer is dequeued again and the buffers go in the order of their ids") {
    Dsp dsp;
    dsp.Initialize();
    SourceConfiguration config{};
    SetFormat(config, kPcm16);
    SetEnable(config, true);
    Queue(config, 0, kValidAddress, 10, 8);  // queued out of order
    Queue(config, 1, kValidAddress, 10, 3);
    dsp.SetConfig(1, 0, config);
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)).current == 3);  // the lower id first
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)).current == 8);  // buffer 3 ended inside this frame
}

TEST_CASE("dsp: a looping buffer of length zero cannot hold a frame for ever") {
    Dsp dsp;
    LogCatcher& log = dsp.log;
    dsp.Initialize();
    SourceConfiguration config{};
    SetFormat(config, kPcm16);
    SetEnable(config, true);
    Queue(config, 0, kValidAddress, 10, 1);
    Queue(config, 1, kValidAddress, 0, 2, /*looping=*/true);
    dsp.SetConfig(1, 0, config);
    for (int i = 0; i < 4; ++i) {
        CHECK(dsp.model.Tick());  // Azahar never returns from the second
    }
    CHECK(log.Count("buffers dequeued in one frame") >= 1);
    CHECK(Look(dsp.Status(0, 0)).enabled == 1);
}

TEST_CASE("dsp: reset, partial reset, sync count, a negative length and a rate that is not valid") {
    Dsp dsp;
    LogCatcher& log = dsp.log;
    dsp.Initialize();
    SourceConfiguration config{};
    SetFormat(config, kPcm16);
    SetEnable(config, true);
    SetSync(config, 0x1234);
    SetRate(config, -1.0f);
    Queue(config, 0, kValidAddress, 0x80000001u, 1);  // length below zero as a signed number: skipped
    Queue(config, 1, kValidAddress, 200, 2);
    dsp.SetConfig(1, 0, config);
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{1, 1, 0x1234, 0, 2, 0});
    CHECK(log.Count("invalid rate multiplier") == 1);
    CHECK(log.Count("improper value for length") == 1);

    // A partial reset empties the queue before what the same configuration adds to it (the order of
    // ParseConfig): buffer 3 is queued. A rate of 1.0 (the invalid one was replaced) takes 159 of the
    // 200 samples of buffer 2.
    config = dsp.Config(1, 0);
    config.dirty_raw |= dirty::kPartialReset;
    Queue(config, 0, kValidAddress, 100, 3);
    dsp.SetConfig(1, 0, config);
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{1, 0, 0x1234, 159, 2, 0});
    // A partial reset of its own throws buffer 3 away: buffer 2 ends (41 samples), nothing follows.
    config = dsp.Config(1, 0);
    config.dirty_raw |= dirty::kPartialReset;
    dsp.SetConfig(1, 0, config);
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{1, 0, 0x1234, 200, 2, 0});
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{0, 1, 0x1234, 200, 0, 2});

    // A reset forgets everything (the sync count, the position, the ids), then applies what the same
    // configuration says: enabled, with nothing to play.
    config = dsp.Config(1, 0);
    config.dirty_raw |= dirty::kReset;
    SetEnable(config, true);
    dsp.SetConfig(1, 0, config);
    CHECK(dsp.model.Tick());
    CHECK(Look(dsp.Status(0, 0)) == Seen{0, 1, 0, 0, 0, 0});
}

TEST_CASE("dsp: the mixers' configuration, the DSP status and the final samples") {
    Dsp dsp;
    dsp.Initialize();
    u8* const configuration = dsp.Region(1) + shm::kDspConfiguration;
    auto put32 = [&](u32 offset, u32 value) { std::memcpy(configuration + offset, &value, 4); };
    auto put16 = [&](u32 offset, u16 value) { std::memcpy(configuration + offset, &value, 2); };
    auto get32 = [&](u32 offset) {
        u32 value;
        std::memcpy(&value, configuration + offset, 4);
        return value;
    };
    auto get16 = [&](u32 offset) {
        u16 value;
        std::memcpy(&value, configuration + offset, 2);
        return value;
    };
    // aux bus 0 enabled, the first delay effect and a field outside the bits the model looks at dirty
    put32(dspconfig::kDirtyRaw, dspconfig::kAuxBusEnable0Dirty | dspconfig::kDelayEffect0Dirty |
                                    (1u << 16) /* master volume */);
    put16(dspconfig::kAuxBusEnable, 1);
    put16(dspconfig::kDelayEffect, 7);  // the delay effect's own dirty word
    put16(dspconfig::kDelayEffect + dspconfig::kDelayEffectSize, 7);  // the second one's: not dirty
    std::memset(dsp.Region(0) + shm::kFinalSamples, 0x55, shm::kFinalSamplesSize);
    std::memset(dsp.Region(0) + shm::kIntermediateMixSamples, 0x66, 2 * shm::kIntermediateMixSize);
    std::memset(dsp.Region(0) + shm::kDspStatus, 0x77, 32);
    CHECK(dsp.model.Tick());
    CHECK(get32(dspconfig::kDirtyRaw) == 0);
    CHECK(get16(dspconfig::kDelayEffect) == 0);                              // cleared
    CHECK(get16(dspconfig::kDelayEffect + dspconfig::kDelayEffectSize) == 7);  // not touched
    const u8* const written = dsp.Region(0);
    // silence in the final samples, and in the intermediate mix of the bus that is enabled
    CHECK(std::all_of(written + shm::kFinalSamples, written + shm::kFinalSamples + 640,
                      [](u8 b) { return b == 0; }));
    CHECK(std::all_of(written + shm::kIntermediateMixSamples,
                      written + shm::kIntermediateMixSamples + shm::kIntermediateMixSize,
                      [](u8 b) { return b == 0; }));
    CHECK(written[shm::kIntermediateMixSamples + shm::kIntermediateMixSize] == 0x66);  // bus 1 is off
    // DspStatus{unknown, dropped_frames} are 0, its padding is left alone
    CHECK(written[shm::kDspStatus] == 0);
    CHECK(written[shm::kDspStatus + 3] == 0);
    CHECK(written[shm::kDspStatus + 4] == 0x77);
    // the bus stays enabled without a new dirty bit
    std::memset(dsp.Region(0) + shm::kIntermediateMixSamples, 0x66, 2 * shm::kIntermediateMixSize);
    CHECK(dsp.model.Tick());
    CHECK(written[shm::kIntermediateMixSamples] == 0);
}

TEST_CASE("dsp: the frame-start hook sees the memory before the frame changes it") {
    Dsp dsp;
    dsp.Initialize();
    SourceConfiguration config{};
    SetFormat(config, kPcm16);
    SetEnable(config, true);
    Queue(config, 0, kValidAddress, 400, 5);
    dsp.SetConfig(1, 0, config);
    std::vector<Seen> seen_dirty_bits;
    std::vector<u32> config_dirty;
    int calls = 0;
    dsp.model.SetFrameStartHook([&](const u8* memory) {
        ++calls;
        SourceStatus status;
        std::memcpy(&status, memory + kRegionOffset[0] + shm::kSourceStatuses, sizeof(status));
        seen_dirty_bits.push_back(Look(status));
        u32 dirty_raw;
        std::memcpy(&dirty_raw, memory + kRegionOffset[1] + shm::kSourceConfigurations, 4);
        config_dirty.push_back(dirty_raw);
    });
    dsp.model.Tick();
    dsp.model.Tick();
    CHECK(calls == 2);
    CHECK(seen_dirty_bits[0] == Seen{0, 0, 0, 0, 0, 0});  // nothing written yet
    CHECK(config_dirty[0] != 0);                          // the application's bits, not yet cleared
    CHECK(seen_dirty_bits[1].enabled == 1);               // the first frame's status
    CHECK(config_dirty[1] == 0);
    dsp.model.PipeWrite(2, Message::Audio(1));
    dsp.model.Tick();
    CHECK(calls == 2);  // a DSP that is not on does not call it
}

// The model against the transcription of Azahar's source, on random configurations. The generator
// leaves out what Azahar does not handle or what makes it hang or read past a buffer: a partial update
// of a format other than PCM16 and the format value 3 (Azahar logs "unimplemented" and leaves the
// buffer empty, which the transcription does not model), a looping buffer of no samples (Azahar loops
// for ever) and a play position past the end (Azahar erases past the end of the buffer).
TEST_CASE("dsp: the model counts as Azahar's source does, on random configurations") {
    LogCatcher quiet;  // the buffers at invalid addresses are warned about
    constexpr u32 kLengths[] = {0, 1, 2, 3, 13, 14, 15, 100, 159, 160, 161, 400, 1000};
    constexpr float kRates[] = {0.5f, 1.0f, 1.0f, 1.5f, 2.0f, 0.333f, 3.7f, 0.25f, 1.0f / 3.0f};
    u64 compared = 0;
    for (u32 seed = 1; seed <= 300; ++seed) {
        Lcg rng(seed);
        SourceModel model;
        oracle::Source reference;
        SourceConfiguration memory_config{};  // the words in the shared memory, kept between frames
        SetFormat(memory_config, kPcm16);
        SetEnable(memory_config, true);
        for (u32 frame = 0; frame < 300; ++frame) {
            if (frame != 0) {
                if (rng.Chance(2)) {
                    memory_config.dirty_raw |= dirty::kReset;
                }
                if (rng.Chance(4)) {
                    memory_config.dirty_raw |= dirty::kPartialReset;
                }
                if (rng.Chance(5)) {
                    SetEnable(memory_config, rng.Chance(70));
                }
                if (rng.Chance(5)) {
                    SetSync(memory_config, static_cast<u16>(rng.Below(65536)));
                }
                if (rng.Chance(8)) {
                    SetRate(memory_config, kRates[rng.Below(std::size(kRates))]);
                }
                if (rng.Chance(4)) {
                    SetFormat(memory_config, static_cast<Format>(rng.Below(3)),
                              1 /* mono: the transcription's refresh assumes it */);
                }
            }
            if (rng.Chance(8)) {
                const u32 length = kLengths[rng.Below(std::size(kLengths))];
                const bool looping = length != 0 && rng.Chance(30);
                const u32 address = rng.Chance(10) ? kInvalidAddress : kValidAddress + rng.Below(64) * 4;
                SetEmbedded(memory_config, address, length, static_cast<u16>(rng.Below(20)), looping);
                if (rng.Chance(30)) {
                    SetPlayPosition(memory_config, rng.Below(length + 1));
                }
            }
            if (rng.Chance(12)) {
                for (u32 slot = 0; slot < 4; ++slot) {
                    if (rng.Chance(40)) {
                        const u32 length = kLengths[rng.Below(std::size(kLengths))];
                        const bool looping = length != 0 && rng.Chance(20);
                        const u32 address =
                            rng.Chance(10) ? kInvalidAddress : kValidAddress + rng.Below(64) * 4;
                        Queue(memory_config, slot, address, length,
                              static_cast<u16>(rng.Below(20)), looping);
                    }
                }
            }
            // the format the source will have latched by the time the partial update is handled
            u8 latched = reference.format;
            if (memory_config.dirty_raw & dirty::kReset) {
                latched = kAdpcm;
            }
            if (memory_config.dirty_raw & (dirty::kFormat | dirty::kEmbeddedBuffer)) {
                latched = static_cast<u8>((memory_config.flags1_raw >> 2) & 3);
            }
            if (rng.Chance(3) && latched == kPcm16 &&
                !(memory_config.dirty_raw & dirty::kEmbeddedBuffer)) {
                memory_config.length = SwapHalves(kLengths[rng.Below(std::size(kLengths))]);
                memory_config.dirty_raw |= dirty::kPartialEmbeddedBuffer;
            }

            SourceConfiguration for_model = memory_config;
            SourceConfiguration for_reference = memory_config;
            const SourceStatus got = model.Tick(for_model, 0);
            const SourceStatus want = reference.Tick(for_reference);
            INFO("seed " << seed << ", audio frame " << frame);
            REQUIRE(Look(got) == Look(want));
            // both leave the same words behind in the shared memory
            REQUIRE(for_model.dirty_raw == for_reference.dirty_raw);
            REQUIRE(for_model.buffers_dirty == for_reference.buffers_dirty);
            memory_config = for_model;
            ++compared;
        }
    }
    CHECK(compared == 300 * 300);
}


// ---- the service, the frame timer, the sink and the capture, through the SVC layer ------------------------------------

#if !__has_include("recomp.h")
TEST_CASE("dsp_test's service cases need recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <sys/types.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros
#include "../src/services/dsp/dsp_shared.h"

namespace {

constexpr u32 kText = 0x00100000;
constexpr Result kTimeout = 0x09401BFE;
constexpr Result kTooManyEvents = 0xC860A7FF;  // InvalidResultValue, DSP, OutOfResource, Status
constexpr Result kInvalidInterrupt = MakeResult(1005, 41, 7, 27);

// ---- harness: an ELF whose main thread runs a C++ body (as srv_test's) ----------------------------------

void Put16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v), b[at + 1] = static_cast<u8>(v >> 8);
}
void Put32(std::vector<u8>& b, std::size_t at, u32 v) {
    Put16(b, at, v & 0xFFFF), Put16(b, at + 2, v >> 16);
}

std::vector<u8> MakeElf() {
    std::vector<u8> b(52 + 32, 0);
    b[0] = 0x7F, b[1] = 'E', b[2] = 'L', b[3] = 'F', b[4] = 1, b[5] = 1, b[6] = 1;
    Put16(b, 16, 2), Put16(b, 18, 40), Put32(b, 20, 1), Put32(b, 24, kText);
    Put32(b, 28, 52), Put16(b, 40, 52), Put16(b, 42, 32), Put16(b, 44, 1);
    Put32(b, 52, 1);  // PT_LOAD
    Put32(b, 52 + 4, 84), Put32(b, 52 + 8, kText), Put32(b, 52 + 12, kText);
    Put32(b, 52 + 16, 0x40), Put32(b, 52 + 20, 0x40), Put32(b, 52 + 24, 5),
        Put32(b, 52 + 28, 0x1000);
    for (u32 i = 0; i < 0x40; ++i) {
        b.push_back(static_cast<u8>(i));
    }
    return b;
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_dsp_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string File(const std::string& name) const { return (path_ / name).string(); }
    std::string WriteElf() const {
        const std::string path = File("program.elf");
        const std::vector<u8> bytes = MakeElf();
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return path;
    }

private:
    std::filesystem::path path_;
};

// What the test sink saw: the game clock when each frame arrived, and whether any sample was not 0.
struct AudioLog {
    Runtime* runtime = nullptr;
    std::vector<u64> ticks;
    std::vector<std::size_t> sizes;
    bool silent = true;
};
AudioLog g_audio;

class LoggingSink final : public AudioSink {
public:
    void Queue(const s16* stereo, std::size_t frames) override {
        g_audio.ticks.push_back(g_audio.runtime->Clock().Now());
        g_audio.sizes.push_back(frames);
        for (std::size_t i = 0; i < frames * 2; ++i) {
            g_audio.silent = g_audio.silent && stereo[i] == 0;
        }
    }
};

MH4N_REGISTER(AudioSinkFactory, "test-dsp-audio", [](Runtime& runtime) -> std::unique_ptr<AudioSink> {
    g_audio = AudioLog{};
    g_audio.runtime = &runtime;
    return std::make_unique<LoggingSink>();
});

struct Fixture {
    ManualTimeSource* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    LogCatcher log;
    Fixture() {
        auto source = std::make_unique<ManualTimeSource>();
        time = source.get();
        RuntimeConfig config;
        config.time = std::move(source);
        runtime = std::make_unique<Runtime>(std::move(config));
    }
    ~Fixture() { runtime.reset(); }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

// Boots the ELF whose main thread runs `body`, which must end the process.
int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra,
             std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf()};
    args.insert(args.end(), extra.begin(), extra.end());
    f.runtime->SetMainThreadEntry([body = std::move(body)](GuestThread&) { body(); });
    return f.runtime->Boot(Parse(args));
}

struct Regs {
    std::array<u32, 16> r{};
};

Regs CallSvc(Runtime& rt, u32 number, std::initializer_list<u32> in) {
    RCtx ctx{};
    int i = 0;
    for (const u32 value : in) {
        ctx.r[i++] = value;
    }
    ctx.r[14] = kText + 0x20;
    rt.Svc().CallSvc(ctx, number, kText + 0x11);
    Regs out;
    for (int k = 0; k < 16; ++k) {
        out.r[static_cast<std::size_t>(k)] = ctx.r[k];
    }
    return out;
}

void ExitProcess(Runtime& rt) {
    CallSvc(rt, 0x03, {});
}

// The guest side of one run: the main thread's command buffer and a scratch area on its stack.
struct Guest {
    Runtime& rt;
    explicit Guest(Runtime& runtime) : rt(runtime) {}

    u32 Cmd() const { return CommandBufferAddress(*rt.MainThread()); }
    u32 Scratch(u32 offset = 0) const { return rt.MainThread()->stack_top - 0x3000 + offset; }

    void PutBytes(u32 address, const std::vector<u8>& bytes) {
        REQUIRE(rt.Memory().WriteBlock(address, bytes.data(), static_cast<u32>(bytes.size())));
    }
    std::vector<u8> GetBytes(u32 address, std::size_t count) {
        std::vector<u8> bytes(count);
        REQUIRE(rt.Memory().ReadBlock(bytes.data(), address, static_cast<u32>(count)));
        return bytes;
    }
    void PutWords(u32 address, const std::vector<u32>& words) {
        REQUIRE(rt.Memory().WriteBlock(address, words.data(), static_cast<u32>(words.size() * 4)));
    }
    std::vector<u32> GetWords(u32 address, std::size_t count) {
        std::vector<u32> words(count);
        REQUIRE(rt.Memory().ReadBlock(words.data(), address, static_cast<u32>(count * 4)));
        return words;
    }

    Result ConnectToPort(const std::string& name, Handle* handle) {
        std::vector<u8> text(name.begin(), name.end());
        text.push_back(0);
        PutBytes(Scratch(0x2000), text);
        const Regs out = CallSvc(rt, 0x2D, {0, Scratch(0x2000)});
        *handle = out.r[1];
        return out.r[0];
    }
    Result Send(Handle session) { return CallSvc(rt, 0x32, {session}).r[0]; }
    Result Wait(Handle handle, s64 ns) {
        // svcWaitSynchronization1: r0 the handle, r2 and r3 the timeout in nanoseconds
        return CallSvc(rt, 0x24,
                       {handle, 0, static_cast<u32>(ns), static_cast<u32>(static_cast<u64>(ns) >> 32)})
            .r[0];
    }
    void Sleep(s64 ns) {
        CallSvc(rt, 0x0A, {static_cast<u32>(ns), static_cast<u32>(static_cast<u64>(ns) >> 32)});
    }
    Handle NewEvent() { return CallSvc(rt, 0x17, {0}).r[1]; }

    std::vector<u32> Request(Handle session, const std::vector<u32>& words, std::size_t reply_words,
                             Result* result = nullptr) {
        PutWords(Cmd(), words);
        const Result r = Send(session);
        if (result != nullptr) {
            *result = r;
        }
        return GetWords(Cmd(), reply_words);
    }
};

// Packs a service name into the two words of GetServiceHandle (8 bytes, zero padded).
Handle ConnectDsp(Guest& g) {
    Handle srv = 0;
    REQUIRE(g.ConnectToPort("srv:", &srv) == kResultSuccess);
    std::array<u32, 2> name{};
    std::memcpy(name.data(), "dsp::DSP", 8);
    const std::vector<u32> reply =
        g.Request(srv, {ipc::MakeHeader(0x0005, 4, 0), name[0], name[1], 8, 0}, 4);
    REQUIRE(reply[0] == 0x00050042);
    REQUIRE(reply[1] == kResultSuccess);
    return reply[3];
}

// dsp::DSP's commands, as the application sends them.
struct DspClient {
    Guest& g;
    Handle session;

    std::vector<u32> Call(const std::vector<u32>& words, std::size_t reply_words) {
        return g.Request(session, words, reply_words);
    }
    Result WritePipe(u32 channel, const std::vector<u8>& bytes) {
        g.PutBytes(g.Scratch(0x100), bytes);
        return Call({ipc::MakeHeader(0x000D, 2, 2), channel, static_cast<u32>(bytes.size()),
                     ipc::StaticBufferDesc(bytes.size(), 0), g.Scratch(0x100)},
                    2)[1];
    }
    // Initialize / Shutdown / Wakeup / Sleep on the audio pipe.
    Result AudioPipe(u8 state_change) { return WritePipe(2, {state_change, 0xAA, 0xBB, 0xCC}); }
    // ReadPipeIfPossible: the reply words, and the bytes the receive buffer got.
    std::pair<std::vector<u32>, std::vector<u8>> ReadPipe(u32 channel, u32 size) {
        g.PutWords(g.Cmd() + 4 * ipc::StaticBufferDescriptorWord(0),
                   {ipc::StaticBufferDesc(0x100, 0), g.Scratch(0x400)});
        std::vector<u32> reply =
            Call({ipc::MakeHeader(0x0010, 3, 0), channel, 0, size}, 5);
        return {reply, g.GetBytes(g.Scratch(0x400), reply[2])};
    }
    // ReadPipe (0x000E), which asks for exactly `size` bytes: the reply words, and the bytes the receive
    // buffer got (as many as the reply's static buffer descriptor says).
    std::pair<std::vector<u32>, std::vector<u8>> ReadPipeExact(u32 channel, u32 size) {
        g.PutWords(g.Cmd() + 4 * ipc::StaticBufferDescriptorWord(0),
                   {ipc::StaticBufferDesc(0x100, 0), g.Scratch(0x400)});
        std::vector<u32> reply = Call({ipc::MakeHeader(0x000E, 3, 0), channel, 0, size}, 4);
        return {reply, g.GetBytes(g.Scratch(0x400), ipc::StaticBufferDescSize(reply[2]))};
    }
    // GetPipeReadableSize (0x000F): the three reply words.
    std::vector<u32> Readable(u32 channel) {
        return Call({ipc::MakeHeader(0x000F, 2, 0), channel, 0}, 3);
    }
    Result Register(u32 interrupt, u32 channel, Handle event) {
        return Call({ipc::MakeHeader(0x0015, 2, 2), interrupt, channel, ipc::CopyHandleDesc(1), event},
                    2)[1];
    }
};

std::shared_ptr<DspShared> SharedOf(Fixture& f) {
    ServiceManager* const manager = FindServiceManager(*f.runtime);
    REQUIRE(manager != nullptr);
    return manager->Context().State<DspShared>("dsp::DSP");
}

// The words of a source's configuration in the application's view of the DSP memory.
u32 ConfigAddress(std::size_t region, u32 source) {
    return kDspRamVaddr + kRegionOffset[region] + shm::kSourceConfigurations + source * 192;
}
u32 StatusAddress(std::size_t region, u32 source) {
    return kDspRamVaddr + kRegionOffset[region] + shm::kSourceStatuses + source * 12;
}

std::vector<u8> Bytes(const SourceConfiguration& config) {
    std::vector<u8> bytes(sizeof(config));
    std::memcpy(bytes.data(), &config, sizeof(config));
    return bytes;
}

Seen LookAt(Guest& g, u32 address) {
    SourceStatus status;
    const std::vector<u8> bytes = g.GetBytes(address, sizeof(status));
    std::memcpy(&status, bytes.data(), sizeof(status));
    return Look(status);
}

// ---- tools/native/capfmt.py ---------------------------------------------------------------------------------------

std::filesystem::path CapfmtPath() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "tools" /
           "native" / "capfmt.py";
}

struct Ran {
    int status = -1;
    std::string output;
};

Ran Run(const std::string& command) {
    Ran ran;
#ifdef _WIN32
    std::FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
    std::FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (pipe == nullptr) {
        return ran;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        ran.output += buffer;
    }
#ifdef _WIN32
    ran.status = _pclose(pipe);
#else
    const int raw = pclose(pipe);
    ran.status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
    return ran;
}

// True if python3 and capfmt.py can be run. A copy of native/ without the repository around it has
// no capfmt.py: that skips, as the cases that need recomp.h do. In the repository, a missing python3
// fails the test under MH4N_REQUIRE_TEST_DATA=1 (CI sets it) instead of skipping it: the capture's
// reader is the point of the check.
bool HaveCapfmt() {
    if (!std::filesystem::exists(CapfmtPath())) {
        WARN_MESSAGE(false, "skipped: tools/native/capfmt.py is not next to native/");
        return false;
    }
    if (Run("python3 -c \"print(1)\"").status == 0) {
        return true;
    }
    const char* require = std::getenv("MH4N_REQUIRE_TEST_DATA");
    if (require != nullptr && std::string(require) == "1") {
        FAIL("python3 is missing, and MH4N_REQUIRE_TEST_DATA=1");
    }
    WARN_MESSAGE(false, "skipped: python3 is not available");
    return false;
}

Ran Capfmt(const std::string& arguments) {
    return Run("python3 -I \"" + CapfmtPath().string() + "\" " + arguments);
}

std::vector<u8> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("dsp::DSP: the commands answer as Azahar's do") {
    Fixture f;
    TempDir dir;
    std::vector<u32> recv, ready, sem, convert, force, mask, event, flush, invalidate, unload,
        headphones, unknown, load;
    std::vector<u8> firmware(64);
    for (std::size_t i = 0; i < firmware.size(); ++i) {
        firmware[i] = static_cast<u8>(i * 7);
    }
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        DspClient dsp{g, ConnectDsp(g)};
        recv = dsp.Call({ipc::MakeHeader(0x0001, 1, 0), 0}, 3);
        ready = dsp.Call({ipc::MakeHeader(0x0002, 1, 0), 0}, 3);
        sem = dsp.Call({ipc::MakeHeader(0x0007, 1, 0), 0x1234}, 2);
        convert = dsp.Call({ipc::MakeHeader(0x000C, 1, 0), 0x8000 + shm::kFrameCounter / 2}, 3);
        force = dsp.Call({ipc::MakeHeader(0x0020, 1, 0), 1}, 2);
        mask = dsp.Call({ipc::MakeHeader(0x0017, 1, 0), 0x0F0F}, 2);
        event = dsp.Call({ipc::MakeHeader(0x0016, 0, 0)}, 4);
        flush = dsp.Call({ipc::MakeHeader(0x0013, 2, 2), 0x00100000, 0x1000, ipc::CopyHandleDesc(1),
                          kHandleCurrentProcess},
                         2);
        invalidate = dsp.Call({ipc::MakeHeader(0x0014, 2, 2), 0x00100000, 0x1000,
                               ipc::CopyHandleDesc(1), kHandleCurrentProcess},
                              2);
        unload = dsp.Call({ipc::MakeHeader(0x0012, 0, 0)}, 2);
        headphones = dsp.Call({ipc::MakeHeader(0x001F, 0, 0)}, 3);
        unknown = dsp.Call({ipc::MakeHeader(0x0003, 1, 0), 5}, 2);
        // LoadComponent: 64 bytes in a mapped buffer, given back in the reply
        g.PutBytes(g.Scratch(0x200), firmware);
        load = dsp.Call({ipc::MakeHeader(0x0011, 3, 2), 64, 0xFF, 0xFFFF,
                         ipc::MappedBufferDesc(64, ipc::R), g.Scratch(0x200)},
                        5);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(recv == std::vector<u32>{0x00010080, kResultSuccess, 1});     // the DSP is off
    CHECK(ready == std::vector<u32>{0x00020080, kResultSuccess, 1});
    CHECK(sem == std::vector<u32>{0x00070040, kResultSuccess});
    // the word address of the frame counter in the DSP's DRAM (0xBFFF), in the first region
    CHECK(convert == std::vector<u32>{0x000C0080, kResultSuccess, 0x1FF50000 + shm::kFrameCounter});
    CHECK(force == std::vector<u32>{0x00200040, kResultSuccess});
    CHECK(mask == std::vector<u32>{0x00170040, kResultSuccess});
    REQUIRE(event.size() == 4);
    CHECK(event[0] == 0x00160042);
    CHECK(event[1] == kResultSuccess);
    CHECK(event[2] == ipc::CopyHandleDesc(1));
    CHECK(event[3] != 0);
    CHECK(flush == std::vector<u32>{0x00130040, kResultSuccess});
    CHECK(invalidate == std::vector<u32>{0x00140040, kResultSuccess});
    CHECK(unload == std::vector<u32>{0x00120040, kResultSuccess});
    CHECK(headphones == std::vector<u32>{0x001F0080, kResultSuccess, 0});
    // an unknown command: its id, one normal parameter, the result 0 (Azahar's answer), logged once
    CHECK(unknown == std::vector<u32>{0x00030040, 0});
    CHECK(f.log.Count("unimplemented dsp::DSP 0x00030040") == 1);
    REQUIRE(load.size() == 5);
    CHECK(load[0] == 0x00110082);
    CHECK(load[1] == kResultSuccess);
    CHECK(load[2] == 1);
    CHECK(load[3] == ipc::MappedBufferDesc(64, ipc::R));
    // the firmware is identified by its CRC-32 in the log
    char expected[64];
    std::snprintf(expected, sizeof(expected), "Firmware crc32: 0x%08x", Crc32(firmware.data(), 64));
    CHECK(f.log.Count(expected) == 1);
}

TEST_CASE("dsp::DSP: ReadPipe and GetPipeReadableSize answer as Azahar's do") {
    Fixture f;
    TempDir dir;
    std::vector<u32> size_off, size_init, size_part, size_drained, size_binary, size_binary_after,
        size_bad_pipe;
    std::pair<std::vector<u32>, std::vector<u8>> head, rest, empty, binary, none;
    Result garbage_audio = 0, garbage_binary = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        DspClient dsp{g, ConnectDsp(g)};
        size_off = dsp.Readable(2);  // the DSP is off: nothing on the pipe
        // Initialize, with the bytes the game leaves at random at 1-3 of the message
        garbage_audio = dsp.WritePipe(2, {0, 0xAA, 0xBB, 0xCC});
        size_init = dsp.Readable(2);
        head = dsp.ReadPipeExact(2, 4);  // the count and the first address
        size_part = dsp.Readable(2);
        rest = dsp.ReadPipeExact(2, 28);  // the other fourteen addresses
        size_drained = dsp.Readable(2);
        empty = dsp.ReadPipeExact(2, 32);  // more than the pipe holds
        none = dsp.ReadPipeExact(2, 0);    // nothing asked, nothing given
        size_bad_pipe = dsp.Readable(9);   // no such pipe
        // the binary pipe: an Init request whose bytes 4-7 are what the game left on its stack
        std::vector<u8> init(32, 0);
        init[0] = 1;  // the codec: AAC decoder
        init[1] = 0;
        init[2] = 0;  // the command: Init
        init[3] = 0;
        init[4] = 0xDE, init[5] = 0xAD, init[6] = 0xBE, init[7] = 0xEF;
        for (std::size_t i = 8; i < init.size(); ++i) {
            init[i] = static_cast<u8>(i);
        }
        garbage_binary = dsp.WritePipe(3, init);
        size_binary = dsp.Readable(3);
        binary = dsp.ReadPipeExact(3, 32);
        size_binary_after = dsp.Readable(3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(size_off == std::vector<u32>{0x000F0080, kResultSuccess, 0});
    CHECK(garbage_audio == kResultSuccess);
    CHECK(size_init == std::vector<u32>{0x000F0080, kResultSuccess, 32});
    // ReadPipe: the header 0x000E0042 (1 normal word, 2 translate words), the result, the static
    // buffer descriptor (buffer id 0, the size read) and the address of the receive buffer
    auto words_of = [](const std::vector<u8>& bytes) {
        std::vector<u16> words;
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
            words.push_back(static_cast<u16>(bytes[i] | (bytes[i + 1] << 8)));
        }
        return words;
    };
    REQUIRE(head.first.size() == 4);
    CHECK(head.first[0] == 0x000E0042);
    CHECK(head.first[1] == kResultSuccess);
    CHECK(head.first[2] == ipc::StaticBufferDesc(4, 0));
    CHECK(head.first[3] != 0);
    CHECK(words_of(head.second) == std::vector<u16>{15, 0xBFFF});
    CHECK(size_part == std::vector<u32>{0x000F0080, kResultSuccess, 28});
    REQUIRE(rest.first.size() == 4);
    CHECK(rest.first[0] == 0x000E0042);
    CHECK(rest.first[2] == ipc::StaticBufferDesc(28, 0));
    std::vector<u16> all = words_of(head.second);
    const std::vector<u16> tail = words_of(rest.second);
    all.insert(all.end(), tail.begin(), tail.end());
    CHECK(all == kAudioPipeWords);  // the count 15 and the fifteen addresses
    CHECK(size_drained == std::vector<u32>{0x000F0080, kResultSuccess, 0});
    // Azahar aborts (UNREACHABLE) on a ReadPipe for more than the pipe holds; the model logs it and
    // answers an empty buffer
    REQUIRE(empty.first.size() == 4);
    CHECK(empty.first[0] == 0x000E0042);
    CHECK(empty.first[1] == kResultSuccess);
    CHECK(empty.first[2] == ipc::StaticBufferDesc(0, 0));
    CHECK(empty.second.empty());
    CHECK(f.log.Count("ReadPipe of 32 bytes from pipe 2 with 0 readable") == 1);
    CHECK(none.first[2] == ipc::StaticBufferDesc(0, 0));
    CHECK(none.second.empty());
    CHECK(size_bad_pipe == std::vector<u32>{0x000F0080, kResultSuccess, 0});
    // The bytes the game leaves in a message are never seen by the DSP: the audio pipe took the
    // Initialize whatever bytes 1-3 held (above), and the binary pipe answers an Init with a success
    // result and the request's data, whatever bytes 4-7 held. WriteProcessPipe's clearing of those bytes
    // (audio 2-3, binary 4-7) cannot be observed any other way: no branch of the model reads them.
    CHECK(garbage_binary == kResultSuccess);
    CHECK(size_binary == std::vector<u32>{0x000F0080, kResultSuccess, 32});
    REQUIRE(binary.second.size() == 32);
    CHECK(binary.first[2] == ipc::StaticBufferDesc(32, 0));
    CHECK(binary.second[0] == 1);  // codec 1, command 0
    CHECK(std::vector<u8>(binary.second.begin() + 1, binary.second.begin() + 8) ==
          std::vector<u8>(7, 0));  // command 0, result 0 (success)
    for (std::size_t i = 8; i < 32; ++i) {
        CHECK(binary.second[i] == static_cast<u8>(i));
    }
    CHECK(size_binary_after == std::vector<u32>{0x000F0080, kResultSuccess, 0});
}

TEST_CASE("dsp::DSP: the pipe interrupt, the audio pipe's addresses and the interrupt slots") {
    Fixture f;
    TempDir dir;
    Result off_wait = 0, init_wait = 0, again = 0, shutdown_result = 0;
    std::vector<u32> readable_reply;
    std::vector<u8> readable_bytes;
    std::vector<Result> registered;
    Result seventh = 0, after_unregister = 0, bad_interrupt = 0, bad_channel = 0;
    Result frame_wait = 0, after_shutdown = 0;
    std::vector<u32> empty_reply;
    const int status = BootWith(f, dir, {"--ms", "200"}, [&] {
        Guest g(*f.runtime);
        DspClient dsp{g, ConnectDsp(g)};
        const Handle audio = g.NewEvent();
        registered.push_back(dsp.Register(2, 2, audio));
        // the DSP is off: it has no frames
        off_wait = g.Wait(audio, 30'000'000);
        // Initialize: the structure addresses are on the pipe and its interrupt is raised at once
        shutdown_result = dsp.AudioPipe(0);
        init_wait = g.Wait(audio, 0);
        std::tie(readable_reply, readable_bytes) = dsp.ReadPipe(2, 32);
        std::tie(empty_reply, std::ignore) = dsp.ReadPipe(2, 32);
        // then one interrupt per frame
        frame_wait = g.Wait(audio, 20'000'000);
        again = g.Wait(audio, 20'000'000);
        // a DSP that was shut down has no frames
        dsp.AudioPipe(1);
        g.Wait(audio, 0);  // (an interrupt that was already raised)
        after_shutdown = g.Wait(audio, 30'000'000);

        // six interrupt slots
        dsp.Register(2, 2, 0);
        const Handle events[6] = {g.NewEvent(), g.NewEvent(), g.NewEvent(),
                                  g.NewEvent(), g.NewEvent(), g.NewEvent()};
        registered.clear();
        registered.push_back(dsp.Register(0, 0, events[0]));
        registered.push_back(dsp.Register(1, 0, events[1]));
        registered.push_back(dsp.Register(2, 0, events[2]));
        registered.push_back(dsp.Register(2, 1, events[3]));
        registered.push_back(dsp.Register(2, 2, events[4]));
        registered.push_back(dsp.Register(2, 3, events[5]));
        seventh = dsp.Register(2, 4, g.NewEvent());
        dsp.Register(2, 0, 0);  // unregister one
        after_unregister = dsp.Register(2, 4, g.NewEvent());
        bad_interrupt = dsp.Register(3, 0, g.NewEvent());
        bad_channel = dsp.Register(2, 8, g.NewEvent());
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(registered == std::vector<Result>(6, kResultSuccess));
    CHECK(off_wait == kTimeout);
    CHECK(shutdown_result == kResultSuccess);
    CHECK(init_wait == kResultSuccess);
    REQUIRE(readable_reply.size() == 5);
    CHECK(readable_reply[0] == 0x00100082);
    CHECK(readable_reply[1] == kResultSuccess);
    CHECK(readable_reply[2] == 32);
    CHECK(readable_reply[3] == ipc::StaticBufferDesc(32, 0));
    REQUIRE(readable_bytes.size() == 32);
    CHECK(readable_bytes[0] == 15);
    CHECK(readable_bytes[2] == 0xFF);  // 0xBFFF, the frame counter's address, little-endian
    CHECK(readable_bytes[3] == 0xBF);
    CHECK(empty_reply[2] == 0);  // the pipe is empty now: nothing is read, and the size says so
    CHECK(frame_wait == kResultSuccess);
    CHECK(again == kResultSuccess);
    CHECK(after_shutdown == kTimeout);
    CHECK(seventh == kTooManyEvents);
    CHECK(after_unregister == kResultSuccess);
    CHECK(bad_interrupt == kInvalidInterrupt);
    CHECK(bad_channel == kInvalidInterrupt);
}

TEST_CASE("dsp::DSP: an audio frame every 1,310,720 ticks from the game clock, with no drift") {
    Fixture f;
    TempDir dir;
    u64 start = 0, end = 0;
    struct Frame {
        u64 deadline, now;
    };
    std::vector<Frame> frames;
    const int status = BootWith(f, dir, {"--ms", "600", "--audio", "test-dsp-audio"}, [&] {
        Guest g(*f.runtime);
        DspClient dsp{g, ConnectDsp(g)};
        SharedOf(f)->frame_observer = [&](u64 deadline, u64 now) { frames.push_back({deadline, now}); };
        start = f.runtime->Clock().Now();
        dsp.AudioPipe(0);
        // Three stretches of 100 ms of game time, with the host stalling in between for a time that
        // is not a multiple of the frame: the frames that fall in a stall run late, and the next ones
        // must still be on the grid.
        for (int stretch = 0; stretch < 3; ++stretch) {
            g.Sleep(100'000'000);
            f.time->Advance(12'345'678);
            g.Sleep(1);
        }
        end = f.runtime->Clock().Now();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // 300 ms and the stalls: 337 ms of game time, about 69 frames
    REQUIRE(frames.size() >= 68);
    CHECK(frames.size() <= 70);
    CHECK(frames.size() == g_audio.ticks.size());  // the DSP was on for all of them
    // Every deadline is a multiple of the frame and the next one is exactly one frame later,
    // however late the frame before it ran: nothing accumulates and none is skipped.
    CHECK(frames[0].deadline % kAudioFrameTicks == 0);
    CHECK(frames[0].deadline > start);
    CHECK(frames[0].deadline - start <= kAudioFrameTicks + 50000);  // (the request and its reply delay)
    std::size_t late = 0;
    for (std::size_t k = 0; k < frames.size(); ++k) {
        INFO("frame " << k << " due at tick " << frames[k].deadline);
        CHECK(frames[k].deadline == frames[0].deadline + k * kAudioFrameTicks);
        CHECK(frames[k].now >= frames[k].deadline);
        // Run when it was due, give or take the tick a nanosecond clock cannot show, or late
        // because the host had stalled.
        late += frames[k].now - frames[k].deadline > 1 ? 1 : 0;
        CHECK(g_audio.sizes[k] == kSamplesPerFrame);
    }
    CHECK(late >= 3 * 2);  // each stall of 12.3 ms (2.5 frames) made two or three frames late
    CHECK(late <= 3 * 4);
    CHECK(g_audio.silent);
    CHECK(frames.back().deadline <= end);
    CHECK(end - frames.back().deadline < 2 * kAudioFrameTicks);
}

TEST_CASE("dsp::DSP: frames that were late are all run, from the deadlines, not from the clock") {
    Fixture f;
    TempDir dir;
    u64 before = 0, after = 0, now_after = 0, frames_before = 0, frames_after = 0;
    std::shared_ptr<DspShared> shared;
    const int status = BootWith(f, dir, {"--ms", "400", "--audio", "test-dsp-audio"}, [&] {
        Guest g(*f.runtime);
        DspClient dsp{g, ConnectDsp(g)};
        shared = SharedOf(f);
        dsp.AudioPipe(0);
        g.Sleep(10'000'000);
        before = shared->NextDeadline();
        frames_before = shared->FramesRun();
        // the host stalls for 40 ms of game time: eight frames are due when the scheduler looks
        f.time->Advance(40'000'000);
        g.Sleep(1);
        after = shared->NextDeadline();
        frames_after = shared->FramesRun();
        now_after = f.runtime->Clock().Now();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(before % kAudioFrameTicks == 0);
    CHECK(after % kAudioFrameTicks == 0);
    // the next deadline is the first multiple of the frame after the clock: nothing is due, nothing skipped
    CHECK(after == (now_after / kAudioFrameTicks + 1) * kAudioFrameTicks);
    CHECK(after - before >= 8 * kAudioFrameTicks);
    CHECK(frames_after - frames_before == (after - before) / kAudioFrameTicks);
    // the sink got one frame for each
    CHECK(g_audio.ticks.size() >= 8);
}

TEST_CASE("dsp::DSP: the DSP memory the game sees is the one the model works on") {
    Fixture f;
    TempDir dir;
    std::vector<Seen> statuses;
    std::vector<u8> config_after;
    Result waits[8] = {};
    const int status = BootWith(f, dir, {"--ms", "300"}, [&] {
        Guest g(*f.runtime);
        DspClient dsp{g, ConnectDsp(g)};
        const Handle audio = g.NewEvent();
        dsp.Register(2, 2, audio);
        dsp.AudioPipe(0);
        g.Wait(audio, 0);  // the addresses

        SourceConfiguration config{};
        SetFormat(config, kPcm16);
        SetEnable(config, true);
        SetSync(config, 3);
        Queue(config, 0, kValidAddress, 400, 5);
        Queue(config, 1, kValidAddress + 0x1000, 100, 6);
        // both frame counters are 0: the DSP reads region 1 and writes its statuses to region 0
        g.PutBytes(ConfigAddress(1, 0), Bytes(config));
        for (int i = 0; i < 7; ++i) {
            waits[i] = g.Wait(audio, 20'000'000);
            statuses.push_back(LookAt(g, StatusAddress(0, 0)));
        }
        config_after = g.GetBytes(ConfigAddress(1, 0), sizeof(SourceConfiguration));
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    for (int i = 0; i < 7; ++i) {
        CHECK(waits[i] == kResultSuccess);
    }
    const Seen expected[] = {
        {1, 1, 3, 0, 5, 0},   {1, 0, 3, 159, 5, 0}, {1, 0, 3, 319, 5, 0}, {1, 1, 3, 79, 6, 0},
        {1, 0, 3, 100, 6, 0}, {0, 1, 3, 100, 0, 6}, {0, 0, 3, 100, 0, 6},
    };
    REQUIRE(statuses.size() == 7);
    for (std::size_t i = 0; i < 7; ++i) {
        INFO("audio frame " << i + 1);
        CHECK(statuses[i] == expected[i]);
    }
    // the DSP cleared the dirty bits in the application's configuration
    SourceConfiguration cleared;
    REQUIRE(config_after.size() == sizeof(cleared));
    std::memcpy(&cleared, config_after.data(), sizeof(cleared));
    CHECK(cleared.dirty_raw == 0);
    CHECK(cleared.buffers_dirty == 0);
}

TEST_CASE("dsp::DSP: --capture-dsp writes MDSP records that capfmt.py reads") {
    if (!HaveCapfmt()) {
        return;
    }
    Fixture f;
    TempDir dir;
    const std::string file = dir.File("dsp.mdsp");
    const int status = BootWith(f, dir, {"--ms", "200", "--capture-dsp", "0:3:" + file}, [&] {
        Guest g(*f.runtime);
        DspClient dsp{g, ConnectDsp(g)};
        const Handle audio = g.NewEvent();
        dsp.Register(2, 2, audio);
        dsp.AudioPipe(0);
        g.Wait(audio, 0);
        SourceConfiguration config{};
        SetFormat(config, kPcm16);
        SetEnable(config, true);
        Queue(config, 0, kValidAddress, 400, 5);
        g.PutBytes(ConfigAddress(1, 0), Bytes(config));
        g.Sleep(60'000'000);  // twelve frames: three are recorded
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);

    // the file: header, three type-1 records of two regions each, the manifest next to it
    const std::vector<u8> bytes = ReadFile(file);
    REQUIRE(bytes.size() == 8 + 3 * (20 + 2 * kRegionSize));
    CHECK(std::memcmp(bytes.data(), "MDSP", 4) == 0);
    u32 version;
    std::memcpy(&version, bytes.data() + 4, 4);
    CHECK(version == 1);
    std::vector<u64> record_ticks;
    for (int k = 0; k < 3; ++k) {
        const u8* record = bytes.data() + 8 + k * (20 + 2 * kRegionSize);
        u32 type, frame, size;
        u64 ticks;
        std::memcpy(&type, record, 4);
        std::memcpy(&frame, record + 4, 4);
        std::memcpy(&ticks, record + 8, 8);
        std::memcpy(&size, record + 16, 4);
        CHECK(type == 1);
        CHECK(frame == 0);  // this program has no frame counter
        CHECK(size == 2 * kRegionSize);
        record_ticks.push_back(ticks);
        // "as the HLE DSP sees them at the start of the audio frame": the application's dirty bits are
        // still there in the first, cleared in the second; the status is written only after
        u32 dirty_raw;
        std::memcpy(&dirty_raw, record + 20 + kRegionSize + shm::kSourceConfigurations, 4);
        CHECK((dirty_raw != 0) == (k == 0));
        CHECK(record[20 + shm::kSourceStatuses] == (k == 0 ? 0 : 1));  // the status' "enabled"
    }
    CHECK(record_ticks[1] - record_ticks[0] >= kAudioFrameTicks - 1);
    CHECK(record_ticks[1] - record_ticks[0] <= kAudioFrameTicks + 1);
    CHECK(record_ticks[2] - record_ticks[1] >= kAudioFrameTicks - 1);
    CHECK(record_ticks[2] - record_ticks[1] <= kAudioFrameTicks + 1);
    const std::vector<u8> manifest = ReadFile(file + ".json");
    const std::string json(manifest.begin(), manifest.end());
    CHECK(json.find("\"format\": \"MDSP\"") != std::string::npos);
    CHECK(json.find("\"frames\": 3") != std::string::npos);
    CHECK(json.find("\"1\": 3") != std::string::npos);

    // capfmt.py reads it
    Ran ran = Capfmt("check \"" + file + "\"");
    CHECK(ran.status == 0);
    CHECK(ran.output.find("3 records") != std::string::npos);
    ran = Capfmt("summary --json \"" + file + "\"");
    CHECK(ran.status == 0);
    CHECK(ran.output.find("\"kind\": \"dsp\"") != std::string::npos);
    CHECK(ran.output.find("\"dsp_frames\": 3") != std::string::npos);
    CHECK(ran.output.find("\"dsp_counter_first\": 0") != std::string::npos);
    CHECK(ran.output.find("\"last_frame\": 0") != std::string::npos);
    ran = Capfmt("diff-dsp \"" + file + "\" \"" + file + "\"");
    CHECK(ran.status == 0);
    CHECK(ran.output.find("equal") != std::string::npos);
    // and tells a different status from the same one (a copy with a status block changed)
    std::vector<u8> changed = bytes;
    changed[8 + 20 + shm::kSourceStatuses + 8] ^= 0x5A;  // region 0, source 0, current_buffer_id
    const std::string other = dir.File("other.mdsp");
    std::ofstream(other, std::ios::binary)
        .write(reinterpret_cast<const char*>(changed.data()), static_cast<std::streamsize>(changed.size()));
    ran = Capfmt("diff-dsp \"" + file + "\" \"" + other + "\"");
    CHECK(ran.status == 1);
    CHECK(ran.output.find("source 0") != std::string::npos);
}

TEST_CASE("dsp::DSP: --capture-dsp starts at a game frame and refuses a bad argument") {
    {
        // START above the frame counter this program has (none: 0): nothing is recorded
        Fixture f;
        TempDir dir;
        const std::string file = dir.File("late.mdsp");
        const int status = BootWith(f, dir, {"--ms", "100", "--capture-dsp", "f5:2:" + file}, [&] {
            Guest g(*f.runtime);
            DspClient dsp{g, ConnectDsp(g)};
            dsp.AudioPipe(0);
            g.Sleep(40'000'000);
            ExitProcess(*f.runtime);
        });
        CHECK(status == 0);
        const std::vector<u8> bytes = ReadFile(file);
        CHECK(bytes.size() == 8);
        const std::vector<u8> manifest = ReadFile(file + ".json");
        CHECK(std::string(manifest.begin(), manifest.end()).find("\"frames\": 0") != std::string::npos);
    }
    for (const char* bad : {"3:0:x", "a:3:x", "3:3", "3:3:", "1234567890:3:x", "::x"}) {
        INFO("--capture-dsp " << bad);
        Fixture f;
        TempDir dir;
        bool ran = false;
        const int status = BootWith(f, dir, {"--ms", "100", "--capture-dsp", bad}, [&] { ran = true; });
        CHECK(status == 2);
        CHECK_FALSE(ran);
    }
    {
        Fixture f;
        TempDir dir;
        bool ran = false;
        const int status = BootWith(f, dir, {"--ms", "100", "--capture-dsp", "0:3:" + dir.File("no/such/dir/x")},
                                    [&] { ran = true; });
        CHECK(status == 2);
        CHECK_FALSE(ran);
    }
}

TEST_CASE("dsp::DSP: the null audio sink is the default and takes frames") {
    const AudioSinkFactory* factory = Registry<AudioSinkFactory>::Instance().Find("null");
    REQUIRE(factory != nullptr);
    Runtime runtime;
    const std::unique_ptr<AudioSink> sink = (*factory)(runtime);
    REQUIRE(sink != nullptr);
    const std::array<s16, 2 * kSamplesPerFrame> silence{};
    sink->Queue(silence.data(), kSamplesPerFrame);

    // a run that names no sink uses it: the DSP's frames are dropped without a word
    Fixture f;
    TempDir dir;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        DspClient dsp{g, ConnectDsp(g)};
        dsp.AudioPipe(0);
        g.Sleep(30'000'000);
        CHECK(f.runtime->Interfaces().audio != nullptr);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
}

#endif
