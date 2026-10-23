// dsp_replay_test.cpp - N3-04.
//
// mh4n_dsp_replay on captures the test writes itself: a rig plays the application (writes the sources'
// configurations into the region the DSP reads next, as the DSP oracle's driver does), runs our own
// dsp_hle::Dsp and writes what the reference runner's --capture-dsp would (the two regions at the start
// of every frame, the frame's output, the sample data it read with its bytes only the first time a key
// appears). The captures of the reference's own DSP code (Azahar's, through the oracle harness) are
// replayed by tests/test_native_dsp_replay.py. Nothing here comes from the game.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "audio/dsp_layout.h"
#include "dsp_hle/dsp.h"
#include "dsp_hle/mixers.h"
#include "dsp_replay/capture.h"
#include "dsp_replay/judge.h"
#include "dsp_replay/replay.h"
#include "dsp_replay/tool.h"
#include "mh4n/crc32.h"

namespace {

using namespace mh4n;
using namespace mh4n::dsp_replay;

constexpr u32 kFcram = 0x20000000;
constexpr u32 kWindow = 0x400000;

struct Memory final : dsp_hle::SampleMemory {
    std::vector<u8> data = std::vector<u8>(kWindow);
    std::span<const u8> Read(u32 address, u32 size) override {
        if (address < kFcram || address - kFcram >= kWindow) {
            return {};
        }
        const u32 offset = address - kFcram;
        return {data.data() + offset, std::min<std::size_t>(size, kWindow - offset)};
    }
};

struct Lcg {
    u32 state;
    explicit Lcg(u32 seed) : state(seed) {}
    u32 Next() {
        state = state * 1664525u + 1013904223u;
        return state >> 16;
    }
};

// A random walk of s16 samples, small enough that the mix stays far from clipping.
void FillWalk16(std::vector<u8>& data, u32 at, u32 bytes, u32 seed) {
    Lcg rng(seed);
    int value = 0;
    for (u32 i = 0; i + 1 < bytes; i += 2) {
        value = std::clamp(value + static_cast<int>(rng.Next() % 401) - 200, -8000, 8000);
        const u16 raw = static_cast<u16>(static_cast<s16>(value));
        data[at + i] = static_cast<u8>(raw & 0xFF);
        data[at + i + 1] = static_cast<u8>(raw >> 8);
    }
}
void FillNoise(std::vector<u8>& data, u32 at, u32 bytes, u32 seed) {
    Lcg rng(seed);
    for (u32 i = 0; i < bytes; ++i) {
        data[at + i] = static_cast<u8>(rng.Next() & 0xFF);
    }
}
void FillAdpcm(std::vector<u8>& data, u32 at, u32 bytes, u32 seed) {
    Lcg rng(seed);
    for (u32 i = 0; i < bytes; i += 8) {
        data[at + i] = static_cast<u8>(((rng.Next() % 8) << 4) | (rng.Next() % 5));
        for (u32 k = 1; k < 8 && i + k < bytes; ++k) {
            data[at + i + k] = static_cast<u8>(rng.Next() & 0xFF);
        }
    }
}

// ---- a capture in memory ---------------------------------------------------------------------------------

struct Rec {
    u32 type;
    u32 frame;
    std::vector<u8> payload;
};

void Put32(std::string& out, u32 v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

std::string Serialize(const std::vector<Rec>& recs, bool header = true) {
    std::string out;
    if (header) {
        out = "MDSP";
        Put32(out, 1);
    }
    u64 ticks = 0;
    for (const Rec& r : recs) {
        Put32(out, r.type);
        Put32(out, r.frame);
        Put32(out, static_cast<u32>(ticks));
        Put32(out, static_cast<u32>(ticks >> 32));
        Put32(out, static_cast<u32>(r.payload.size()));
        out.append(r.payload.begin(), r.payload.end());
        ticks += 1310720;
    }
    return out;
}

Verdict ReplayOf(const std::string& bytes) {
    std::istringstream in(bytes);
    return Replay(in);
}

// ---- the rig: the application, our DSP, and the recorder ---------------------------------------------------------

constexpr u32 kA = kFcram + 0x1000;  // where the test's data sits

class Rig {
public:
    Rig() : memory_(audio::kDspMemorySize, 0), dsp_(memory_.data()) {
        dsp_.SetSampleMemory(&data_);
        dsp_.PipeWrite(static_cast<u32>(audio::DspPipe::Audio), std::array<u8, 4>{0, 0, 0, 0});
        dsp_.SetFrameStartHook([this](const u8* memory) { OnFrameStart(memory); });
        dsp_.SetFrameOutput([this](std::span<const s16> samples) { OnOutput(samples); });
        for (auto& c : shadow_) {
            std::memset(&c, 0, sizeof(c));
        }
    }

    Memory& Data() { return data_; }

    // A source with `buffers` of (address, samples, looping) queued at the frame `frame`.
    struct Buf {
        u32 address;
        u32 samples;
        bool loop;
    };
    // format: 0 PCM8, 1 PCM16, 2 ADPCM; channels 1 or 2.
    void Start(u32 frame, u32 source, u32 format, u32 channels, float rate, float gain, std::vector<Buf> buffers) {
        audio::SourceConfiguration& c = shadow_[source];
        c.flags1_raw = static_cast<u16>(channels | (format << 2));
        c.rate_multiplier = rate;
        c.interpolation_mode = 1;
        c.gain[0][0] = gain;
        c.gain[0][1] = gain;
        c.enable = 1;
        c.dirty_raw |= audio::dirty::kFormat | audio::dirty::kMonoOrStereo | audio::dirty::kEnable |
                       audio::dirty::kRateMultiplier | audio::dirty::kInterpolation | audio::dirty::kGain0;
        u16 id = static_cast<u16>(1 + source * 10);
        u32 slot = 0;
        for (const Buf& b : buffers) {
            audio::QueuedBufferConfig& q = c.buffers[slot];
            q.physical_address = audio::SwapHalves(b.address);
            q.length = audio::SwapHalves(b.samples);
            q.is_looping = b.loop ? 1 : 0;
            q.buffer_id = id++;
            c.buffers_dirty = static_cast<u16>(c.buffers_dirty | (1u << slot));
            c.dirty_raw |= audio::dirty::kBufferQueue;
            ++slot;
            u32 bytes = 0;
            if (format == 0) {
                bytes = b.samples * channels;
            } else if (format == 1) {
                bytes = b.samples * 2 * channels;
            } else {
                bytes = (b.samples + 13) / 14 * 8;
            }
            reads_.push_back({frame, b.address, bytes, b.loop && format == 1});
        }
        if (format == 2) {
            for (u32 i = 0; i < 16; ++i) {
                coefficients_[source][i] = static_cast<s16>((i * 517) % 2000 - 900);
            }
            c.dirty_raw |= audio::dirty::kAdpcmCoefficients;
        }
        starts_.push_back(frame);
    }

    // Plays `frames` frames; `step(frame)` may start sources before each.
    std::vector<Rec> Run(u32 frames, const std::function<void(Rig&, u32)>& step = {}) {
        recs_.clear();
        for (frame_ = 0; frame_ < frames; ++frame_) {
            if (step) {
                step(*this, frame_);
            }
            const std::size_t target = (frame_ + 1) % 2;
            u8* region = memory_.data() + audio::kRegionOffset[target];
            for (u32 s = 0; s < audio::kNumSources; ++s) {
                std::memcpy(region + audio::shm::kSourceConfigurations + s * sizeof(audio::SourceConfiguration),
                            &shadow_[s], sizeof(audio::SourceConfiguration));
                shadow_[s].dirty_raw = 0;
                shadow_[s].buffers_dirty = 0;
            }
            std::memcpy(region + audio::shm::kAdpcmCoefficients, coefficients_, sizeof(coefficients_));
            if (frame_ < 2) {  // the master volume, into both regions
                dsp_hle::DspConfiguration cfg;
                std::memcpy(&cfg, region + audio::shm::kDspConfiguration, sizeof(cfg));
                cfg.master_volume = 1.0f;
                cfg.dirty_raw |= dsp_hle::config_dirty::kMasterVolume;
                std::memcpy(region + audio::shm::kDspConfiguration, &cfg, sizeof(cfg));
            }
            const u16 counter = static_cast<u16>(frame_ + 1);
            std::memcpy(region + audio::shm::kFrameCounter, &counter, 2);
            REQUIRE(dsp_.Tick());
            // The statuses this frame wrote, for the tests' own bookkeeping.
            const std::size_t write = frame_ % 2;
            statuses_.emplace_back(memory_.begin() + audio::kRegionOffset[write] + audio::shm::kSourceStatuses,
                                   memory_.begin() + audio::kRegionOffset[write] + audio::shm::kSourceStatuses + 24 * 12);
        }
        return recs_;
    }

    // Our own status block of `source` after `frame`.
    std::array<u8, 12> Status(u32 frame, u32 source) const {
        std::array<u8, 12> out;
        std::memcpy(out.data(), statuses_[frame].data() + source * 12, 12);
        return out;
    }

private:
    struct ReadPlan {
        u32 frame, address, bytes;
        bool every;
    };

    void OnFrameStart(const u8* memory) {
        Rec regions{1, frame_ / 2, {}};
        regions.payload.assign(memory + audio::kRegionOffset[0], memory + audio::kRegionOffset[0] + audio::kRegionSize);
        regions.payload.insert(regions.payload.end(), memory + audio::kRegionOffset[1],
                               memory + audio::kRegionOffset[1] + audio::kRegionSize);
        recs_.push_back(std::move(regions));
        for (const ReadPlan& r : reads_) {
            if (r.frame != frame_ && !(r.every && r.frame < frame_)) {
                continue;
            }
            const u8* bytes = data_.data.data() + (r.address - kFcram);
            const u32 crc = Crc32(bytes, r.bytes);
            Rec rec{3, frame_ / 2, {}};
            Put(rec.payload, r.address);
            Put(rec.payload, r.bytes);
            Put(rec.payload, crc);
            if (seen_.insert({r.address, r.bytes, crc}).second) {
                rec.payload.insert(rec.payload.end(), bytes, bytes + r.bytes);
            }
            recs_.push_back(std::move(rec));
        }
    }
    void OnOutput(std::span<const s16> samples) {
        Rec rec{2, frame_ / 2, {}};
        for (s16 s : samples) {
            rec.payload.push_back(static_cast<u8>(static_cast<u16>(s) & 0xFF));
            rec.payload.push_back(static_cast<u8>(static_cast<u16>(s) >> 8));
        }
        recs_.push_back(std::move(rec));
    }
    static void Put(std::vector<u8>& out, u32 v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
        }
    }

    std::vector<u8> memory_;
    dsp_hle::Dsp dsp_;
    Memory data_;
    audio::SourceConfiguration shadow_[audio::kNumSources];
    s16 coefficients_[audio::kNumSources][16] = {};
    std::vector<ReadPlan> reads_;
    std::vector<u32> starts_;
    std::set<std::tuple<u32, u32, u32>> seen_;
    std::vector<Rec> recs_;
    std::vector<std::vector<u8>> statuses_;
    u32 frame_ = 0;
};

// The data every scenario uses, at fixed places.
void FillScenario(Memory& m) {
    FillWalk16(m.data, kA - kFcram + 0x00000, 4000, 11);   // s0: 2000 samples PCM16 mono
    FillNoise(m.data, kA - kFcram + 0x10000, 4000, 12);    // s1: PCM8 stereo
    FillAdpcm(m.data, kA - kFcram + 0x20000, 2000, 13);    // s2: ADPCM
    FillWalk16(m.data, kA - kFcram + 0x30000, 16000, 14);  // s3: PCM16 stereo
}

constexpr u32 kS0 = kA;
constexpr u32 kS1 = kA + 0x10000;
constexpr u32 kS2 = kA + 0x20000;
constexpr u32 kS3 = kA + 0x30000;

// Four sources of each kind, the fourth enabled at frame 20. The gains keep the signal quiet (RMS of a few
// hundred), so that a difference of 300 in one sample is below 60 dB.
std::vector<Rec> Scenario(u32 frames = 120) {
    Rig rig;
    FillScenario(rig.Data());
    return rig.Run(frames, [](Rig& r, u32 frame) {
        if (frame == 0) {
            r.Start(0, 0, 1, 1, 1.0f, 0.02f, {{kS0, 800, true}});                       // PCM16 mono, looping
            r.Start(0, 1, 0, 2, 1.5f, 0.02f, {{kS1, 480, false}, {kS1 + 960, 480, false}, {kS1 + 1920, 480, false}});
            r.Start(0, 2, 2, 1, 1.0f, 0.02f, {{kS2, 700, false}, {kS2 + 400, 700, false}});  // ADPCM
        }
        if (frame == 20) {
            r.Start(20, 3, 1, 2, 0.75f, 0.02f, {{kS3, 1500, false}, {kS3 + 6000, 1500, false}});
        }
    });
}

int CountType(const std::vector<Rec>& recs, u32 type, bool with_bytes_only = false, bool without_bytes_only = false) {
    int n = 0;
    for (const Rec& r : recs) {
        if (r.type != type) {
            continue;
        }
        const bool bytes = r.payload.size() > 12;
        if ((with_bytes_only && !bytes) || (without_bytes_only && bytes)) {
            continue;
        }
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("dsp_replay: a capture written from our own DSP replays bit-exact") {
    const std::vector<Rec> recs = Scenario();
    // The capture has what the reference's has: both regions and an output per frame; the sample data's
    // bytes appear only the first time a key does, so some type-3 records stop after the CRC (the looping
    // buffer's, every frame).
    CHECK(CountType(recs, 1) == 120);
    CHECK(CountType(recs, 2) == 120);
    CHECK(CountType(recs, 3, true) == 8);   // the bytes of each of the 1 + 3 + 2 + 2 buffers, once
    CHECK(CountType(recs, 3, false, true) > 50);
    bool sound = false;
    for (const Rec& r : recs) {
        if (r.type == 2) {
            for (u8 b : r.payload) {
                sound = sound || b != 0;
            }
        }
    }
    CHECK(sound);

    const Verdict r = ReplayOf(Serialize(recs));
    CHECK(r.pass);
    CHECK(r.total_frames == 120);
    CHECK(r.settled);
    CHECK(r.first_judged_index > 0);
    CHECK(r.first_judged_index < 20);  // the source enabled at frame 20 does not hold judging back
    CHECK(r.judged == 120 - r.first_judged_index);
    CHECK(r.bit_exact == r.judged);
    CHECK(r.not_exact.empty());
    CHECK(r.status_equal);
    CHECK(std::isinf(r.whole.db));
    const std::string line = SummaryLine(r);
    CHECK(line.find("bit-exact, snr inf, worst window") != std::string::npos);
    CHECK(line.find("status equal, PASS") != std::string::npos);
    CHECK(line.rfind("dsp-replay: ", 0) == 0);
}

TEST_CASE("dsp_replay: one output sample changed by 300 fails in its window and lists its frame") {
    std::vector<Rec> recs = Scenario();
    // Frame 70: the sample 100 (left channel of the 51st stereo pair) of its output record.
    int seen = 0;
    u32 hit_counter = 0;
    for (Rec& rec : recs) {
        if (rec.type != 2 || seen++ != 70) {
            continue;
        }
        s16 v;
        std::memcpy(&v, rec.payload.data() + 100 * 2, 2);
        v = static_cast<s16>(v + 300);
        std::memcpy(rec.payload.data() + 100 * 2, &v, 2);
    }
    const Verdict r = ReplayOf(Serialize(recs));
    CHECK(!r.pass);
    REQUIRE(r.not_exact.size() == 1);
    CHECK(r.not_exact[0].index == 70);
    CHECK(r.not_exact[0].max_diff == 300);
    hit_counter = r.not_exact[0].counter;
    CHECK(hit_counter == 71);  // the DSP's counter of record 70 (the rig counts from 1)
    CHECK(r.bit_exact == r.judged - 1);
    CHECK(r.status_equal);
    CHECK(r.fail_reason.find("window") != std::string::npos);
    // The window that holds the frame is the one below the limit.
    REQUIRE(!r.window_spans.empty());
    bool found = false;
    for (std::size_t i = 0; i < r.windows.size(); ++i) {
        const u32 first = r.first_judged_index + r.windows[i].first;
        const bool holds = 70 >= first && 70 < first + r.windows[i].count;
        CHECK(r.window_spans[i].pass == !holds);
        found = found || holds;
    }
    CHECK(found);
    CHECK(r.worst_window_db < 60.0);
    const std::string line = SummaryLine(r);
    CHECK(line.find("FAIL") != std::string::npos);
    std::ostringstream details;
    WriteDetails(r, details);
    CHECK(details.str().find("not bit-exact: frame 71 (record 70), largest difference 300") != std::string::npos);
}

TEST_CASE("dsp_replay: a changed status block of the next frame is reported at its frame") {
    std::vector<Rec> recs = Scenario();
    // The REGIONS record of frame 50 holds the statuses frame 49 wrote (the region it reads).
    int seen = 0;
    for (Rec& rec : recs) {
        if (rec.type != 1 || seen++ != 50) {
            continue;
        }
        // frame 49 wrote region 49 % 2 = 1; flip the sync count of source 1's block there
        rec.payload[audio::kRegionSize + audio::shm::kSourceStatuses + 12 + 2] ^= 1;
    }
    const Verdict r = ReplayOf(Serialize(recs));
    CHECK(!r.status_equal);
    CHECK(r.first_status_difference_index == 49);
    CHECK(r.first_status_difference_counter == 50);
    CHECK(!r.pass);
    CHECK(r.fail_reason.find("status") != std::string::npos);
    CHECK(SummaryLine(r).find("status first difference at frame 50, FAIL") != std::string::npos);
}

TEST_CASE("dsp_replay: the SNR and the windows against hand-computed values") {
    CHECK(SnrDb(1000000, 1) == doctest::Approx(60.0));
    CHECK(SnrDb(1000000, 1000) == doctest::Approx(30.0));
    CHECK(SnrDb(4, 1) == doctest::Approx(6.0206).epsilon(1e-4));
    CHECK(std::isinf(SnrDb(5, 0)));
    CHECK(SnrDb(0, 5) < 0);
    CHECK(FormatDb(60.0) == "60.00");
    CHECK(FormatDb(SnrDb(1, 0)) == "inf");

    auto sizes = [](u32 n) {
        std::vector<u32> out;
        for (const Window& w : MakeWindows(n)) {
            out.push_back(w.count);
        }
        return out;
    };
    CHECK(sizes(0).empty());
    CHECK(sizes(1) == std::vector<u32>{1});
    CHECK(sizes(100) == std::vector<u32>{100});
    CHECK(sizes(204) == std::vector<u32>{204});
    CHECK(sizes(205) == std::vector<u32>{205});
    CHECK(sizes(300) == std::vector<u32>{300});            // 205 + 95: the short last window joins
    CHECK(sizes(307) == std::vector<u32>{307});            // 205 + 102
    CHECK(sizes(308) == std::vector<u32>{205, 103});       // 103 is not shorter than 103
    CHECK(sizes(410) == std::vector<u32>{205, 205});
    CHECK(sizes(615 + 50) == std::vector<u32>{205, 205, 255});
    const std::vector<Window> w = MakeWindows(308);
    CHECK(w[1].first == 205);

    // A window whose reference RMS is below 1 is judged by its largest difference.
    auto frames = [](u32 n, u64 ref, u64 err, u32 max_diff) {
        std::vector<FrameStat> v(n);
        for (u32 i = 0; i < n; ++i) {
            v[i].index = i;
            v[i].ref_energy = ref;
            v[i].err_energy = err;
            v[i].max_diff = max_diff;
        }
        return v;
    };
    // 2 frames, 640 samples, energy 600 in all: RMS < 1; one sample off by 1 passes, by 2 fails
    Span quiet = JudgeSpan(frames(2, 300, 1, 1), 0, 2);
    CHECK(quiet.low_rms);
    CHECK(quiet.pass);
    CHECK(quiet.db == doctest::Approx(10 * std::log10(600.0 / 2.0)));
    CHECK(!JudgeSpan(frames(2, 300, 4, 2), 0, 2).pass);
    // 2 frames with energy 320 each: RMS exactly 1: judged by the SNR (the largest difference is not used)
    Span edge = JudgeSpan(frames(2, 320, 0, 0), 0, 2);
    CHECK(!edge.low_rms);
    CHECK(edge.pass);
    CHECK(!JudgeSpan(frames(2, 320, 1, 1), 0, 2).pass);  // 640 / 2 = 25 dB
    // exactly 60 dB passes, a hair under does not
    CHECK(JudgeSpan(frames(1, 1000000, 1, 1), 0, 1).pass);
    CHECK(!JudgeSpan(frames(1, 999999, 1, 1), 0, 1).pass);
    // the stream passes at 78 dB while its quiet second window is at 48 dB
    std::vector<FrameStat> mixed = frames(410, 1000000000ull, 0, 0);
    for (u32 i = 205; i < 410; ++i) {
        mixed[i].ref_energy = 1000000;
    }
    mixed[300].err_energy = 3000;
    mixed[300].max_diff = 55;
    CHECK(JudgeSpan(mixed, 0, 410).pass);
    CHECK(JudgeSpan(mixed, 0, 205).pass);
    const Span second = JudgeSpan(mixed, 205, 205);
    CHECK(!second.pass);
    CHECK(second.db == doctest::Approx(10 * std::log10(205e6 / 3000.0)));
}

namespace {

// The status of `source` in the rig's own bookkeeping: the frame of its n-th dequeue.
u32 FrameOfDequeue(Rig& rig, u32 frames, u32 source, int n) {
    int seen = 0;
    for (u32 f = 0; f < frames; ++f) {
        if (rig.Status(f, source)[1] != 0 && ++seen == n) {
            return f;
        }
    }
    return 0xFFFFFFFF;
}

}  // namespace

TEST_CASE("dsp_replay: a source with queued buffers settles at its second dequeue") {
    Rig rig;
    FillScenario(rig.Data());
    const std::vector<Rec> recs = rig.Run(60, [](Rig& r, u32 frame) {
        if (frame == 0) {
            r.Start(0, 1, 0, 2, 1.0f, 0.02f, {{kS1, 480, false}, {kS1 + 960, 480, false}, {kS1 + 1920, 480, false}});
        }
    });
    const u32 expected = FrameOfDequeue(rig, 60, 1, 2);
    REQUIRE(expected > 1);
    REQUIRE(expected < 20);
    const Verdict r = ReplayOf(Serialize(recs));
    CHECK(r.settled);
    CHECK(r.first_judged_index == expected);
    CHECK(r.pass);
}

TEST_CASE("dsp_replay: a looping source settles after one full loop") {
    Rig rig;
    FillScenario(rig.Data());
    const std::vector<Rec> recs = rig.Run(60, [](Rig& r, u32 frame) {
        if (frame == 0) {
            r.Start(0, 0, 1, 1, 1.0f, 0.02f, {{kS0, 800, true}});  // 5 frames a loop at 160 samples a frame
        }
    });
    const Verdict r = ReplayOf(Serialize(recs));
    CHECK(r.settled);
    // the buffer is dequeued in frame 0 (silent), plays frames 1 to 5 and loops in frame 6
    CHECK(r.first_judged_index >= 5);
    CHECK(r.first_judged_index <= 7);
    CHECK(r.pass);
    CHECK(r.bit_exact == r.judged);
}

TEST_CASE("dsp_replay: a source that stops settles at its stop") {
    Rig rig;
    FillScenario(rig.Data());
    const std::vector<Rec> recs = rig.Run(60, [](Rig& r, u32 frame) {
        if (frame == 0) {
            r.Start(0, 0, 1, 1, 1.0f, 0.02f, {{kS0, 800, false}});  // one buffer, then nothing left
        }
    });
    u32 stop = 0xFFFFFFFF;
    for (u32 f = 0; f < 60 && stop == 0xFFFFFFFF; ++f) {
        if (rig.Status(f, 0)[0] == 0 && f > 0) {
            stop = f;
        }
    }
    REQUIRE(stop < 20);
    const Verdict r = ReplayOf(Serialize(recs));
    CHECK(r.settled);
    CHECK(r.first_judged_index == stop);
    CHECK(r.pass);
}

TEST_CASE("dsp_replay: a source that never settles is `not settled`") {
    Rig rig;
    FillScenario(rig.Data());
    const std::vector<Rec> recs = rig.Run(60, [](Rig& r, u32 frame) {
        if (frame == 0) {
            r.Start(0, 0, 1, 1, 1.0f, 0.02f, {{kS3, 8000, false}});  // 50 frames of audio in one buffer
            r.Start(0, 1, 0, 2, 1.0f, 0.02f, {{kS1, 480, false}, {kS1 + 960, 480, false}});
        }
    });
    // source 0 plays one buffer for 50 frames: no second dequeue, no loop, no stop before frame 52...
    // it stops at frame 52 (8000 / 160 + 2), so cut the capture before that
    std::vector<Rec> cut;
    int regions = 0;
    for (const Rec& rec : recs) {
        regions += rec.type == 1 ? 1 : 0;
        if (regions > 40) {
            break;
        }
        cut.push_back(rec);
    }
    const Verdict r = ReplayOf(Serialize(cut));
    CHECK(!r.settled);
    CHECK(!r.pass);
    CHECK(r.judged == 0);
    CHECK(r.unsettled_sources == std::vector<u32>{0});
    CHECK(r.fail_reason == "not settled");
    const std::string line = SummaryLine(r);
    CHECK(line.find("0 frames judged of 40") != std::string::npos);
    CHECK(line.find("FAIL") != std::string::npos);
    std::ostringstream details;
    WriteDetails(r, details);
    CHECK(details.str().find("not settled") != std::string::npos);
}

TEST_CASE("dsp_replay: fewer than 80 % of the frames judged is `not settled`") {
    Rig slow;
    FillScenario(slow.Data());
    const std::vector<Rec> slow_recs = slow.Run(100, [](Rig& r, u32 frame) {
        if (frame == 0) {
            r.Start(0, 3, 1, 1, 0.5f, 0.02f, {{kS3, 7000, true}});  // 80 samples a frame: 87.5 frames a loop
        }
    });
    const Verdict r = ReplayOf(Serialize(slow_recs));
    CHECK(r.settled);
    CHECK(r.first_judged_index > 80);
    CHECK(r.judged * 100 < r.total_frames * 80);
    CHECK(!r.pass);
    CHECK(r.fail_reason == "not settled");
}

TEST_CASE("dsp_replay: the first frame's enabled sources are those of the captured statuses too") {
    // A capture whose first frame has a source enabled in the statuses of the region the DSP reads, which
    // our DSP, starting from nothing, does not play: it is `stopped` at once and judged from the start, so
    // the frames that differ from the reference are listed rather than hidden.
    Rig rig;
    FillScenario(rig.Data());
    std::vector<Rec> recs = rig.Run(40, [](Rig& r, u32 frame) {
        if (frame == 30) {
            r.Start(30, 0, 1, 1, 1.0f, 0.02f, {{kS0, 800, true}});  // our DSP plays nothing before frame 30
        }
    });
    Rec& first = recs.front();
    REQUIRE(first.type == 1);
    // region 1 is the one the first frame reads: mark source 5 enabled in its statuses, and give the first
    // frame an output that our DSP does not make
    first.payload[audio::kRegionSize + audio::shm::kSourceStatuses + 5 * 12] = 1;
    for (Rec& rec : recs) {
        if (rec.type == 2) {
            rec.payload[0] ^= 0x40;
            break;
        }
    }
    const Verdict r = ReplayOf(Serialize(recs));
    CHECK(r.settled);
    CHECK(r.first_judged_index == 0);
    REQUIRE(r.not_exact.size() == 1);
    CHECK(r.not_exact[0].index == 0);
    CHECK(!r.pass);
}

TEST_CASE("dsp_replay: captures that cannot be read") {
    const std::vector<Rec> good = Scenario(10);
    const std::string bytes = Serialize(good);
    CHECK_NOTHROW(ReplayOf(bytes));

    CHECK_THROWS_AS(ReplayOf(""), CaptureError);
    CHECK_THROWS_AS(ReplayOf("MHID\x01\0\0\0"), CaptureError);
    std::string wrong_version = bytes;
    wrong_version[4] = 2;
    CHECK_THROWS_AS(ReplayOf(wrong_version), CaptureError);
    CHECK_THROWS_AS(ReplayOf(bytes.substr(0, bytes.size() - 7)), CaptureError);  // a cut payload
    CHECK_THROWS_AS(ReplayOf(bytes.substr(0, 8 + 9)), CaptureError);             // a cut header
    CHECK_THROWS_AS(ReplayOf(Serialize({})), CaptureError);                        // no frames

    std::vector<Rec> bad = good;
    bad[0].type = 9;
    CHECK_THROWS_AS(ReplayOf(Serialize(bad)), CaptureError);

    // a frame without its output
    bad = good;
    bad.erase(std::remove_if(bad.begin(), bad.end(), [](const Rec& r) { return r.type == 2; }), bad.end());
    CHECK_THROWS_AS(ReplayOf(Serialize(bad)), CaptureError);

    // a read without bytes whose key was never given any
    bad = good;
    for (Rec& r : bad) {
        if (r.type == 3 && r.payload.size() > 12) {
            r.payload.resize(12);
            break;
        }
    }
    CHECK_THROWS_AS(ReplayOf(Serialize(bad)), CaptureError);

    // bytes that do not match their CRC-32
    bad = good;
    for (Rec& r : bad) {
        if (r.type == 3 && r.payload.size() > 12) {
            r.payload[12] ^= 1;
            break;
        }
    }
    CHECK_THROWS_AS(ReplayOf(Serialize(bad)), CaptureError);
}

TEST_CASE("dsp_replay: the sample memory is the image of the captured reads, by key") {
    // The same buffer data at two addresses, and the same address with different data later: each read
    // copies its key's bytes to its address at its own frame.
    Rig rig;
    FillScenario(rig.Data());
    const std::vector<Rec> recs = rig.Run(40, [](Rig& r, u32 frame) {
        if (frame == 0) {
            r.Start(0, 0, 1, 1, 1.0f, 0.02f, {{kS0, 800, false}});
        }
        if (frame == 10) {
            // the game rewrites the buffer in place and plays it again
            FillWalk16(r.Data().data, kS0 - kFcram, 1600, 99);
            r.Start(10, 1, 1, 1, 1.0f, 0.02f, {{kS0, 800, false}});
        }
    });
    CHECK(CountType(recs, 3, true) == 2);  // the same address and size, two CRCs: two sets of bytes
    const Verdict r = ReplayOf(Serialize(recs));
    CHECK(r.pass);
    CHECK(r.bit_exact == r.judged);
}

TEST_CASE("dsp_replay: the program") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "mh4n_dsp_replay_test";
    std::filesystem::create_directories(dir);
    const std::string good_path = (dir / "good.mdsp").string();
    const std::string bad_path = (dir / "bad.mdsp").string();
    const std::string report_path = (dir / "report.txt").string();
    std::vector<Rec> recs = Scenario();
    {
        std::ofstream out(good_path, std::ios::binary);
        const std::string bytes = Serialize(recs);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    {
        int seen = 0;
        for (Rec& rec : recs) {
            if (rec.type == 2 && seen++ == 70) {
                rec.payload[200] ^= 0x7F;
            }
        }
        std::ofstream out(bad_path, std::ios::binary);
        const std::string bytes = Serialize(recs);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    std::ostringstream out, err;
    CHECK(dsp_replay::RunDspReplay({good_path, "--report", report_path}, out, err) == 0);
    CHECK(out.str().find("status equal, PASS\n") != std::string::npos);
    CHECK(err.str().empty());
    {
        std::ifstream rep(report_path);
        std::stringstream text;
        text << rep.rdbuf();
        CHECK(text.str().find("PASS") != std::string::npos);
    }
    out.str("");
    CHECK(dsp_replay::RunDspReplay({bad_path}, out, err) == 1);
    CHECK(out.str().find("not bit-exact: frame 71") != std::string::npos);
    CHECK(out.str().find("FAIL\n") != std::string::npos);
    out.str("");
    err.str("");
    CHECK(dsp_replay::RunDspReplay({(dir / "missing.mdsp").string()}, out, err) == 2);
    CHECK(dsp_replay::RunDspReplay({}, out, err) == 2);
    CHECK(dsp_replay::RunDspReplay({good_path, "--nope"}, out, err) == 2);
    {
        std::ofstream junk((dir / "junk.mdsp").string(), std::ios::binary);
        junk << "not a capture at all";
    }
    CHECK(dsp_replay::RunDspReplay({(dir / "junk.mdsp").string()}, out, err) == 2);
    std::filesystem::remove_all(dir);
}
