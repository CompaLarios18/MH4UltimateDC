// mh4n_audio_out (N3-05): the stream between the DSP and an audio device, the drift controller and
// the WAV sink. No game data and no SDL: the frames are made here, each one carries its own
// sequence number so that a lost, repeated or reordered frame shows.
//
// The simulated-clock tests drive a producer (the DSP, one audio frame of 160 samples every
// 1,310,720 ticks of the game clock) and a consumer (a device pulling 480-frame blocks at 48,000 Hz
// with its own crystal) on a ManualTimeSource, 10 simulated minutes in a fraction of a second. The
// two-thread test uses real threads. The WAV sink and the factories are checked through the runtime.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio_out/audio_out.h"
#include "audio_out/drift_controller.h"
#include "audio_out/wav_writer.h"
#include "mh4n/clock.h"
#include "mh4n/host.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

using namespace mh4n;
using namespace mh4n::audio_out;

namespace {

static_assert(std::atomic<u64>::is_always_lock_free, "Queue and Pull use plain atomic loads and stores");

// ---- frames that carry their sequence number -----------------------------------------------------------

// left = the low 15 bits of the sequence number, right = the next 15 (both non-negative as s16).
void MakeFrames(std::vector<s16>& out, u64 first_seq, std::size_t count) {
    out.resize(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        const u64 seq = first_seq + i;
        out[i * 2] = static_cast<s16>(seq & 0x7FFF);
        out[i * 2 + 1] = static_cast<s16>((seq >> 15) & 0x7FFF);
    }
}
u64 SeqOf(const s16* frame) {
    return static_cast<u64>(static_cast<u16>(frame[0])) | (static_cast<u64>(static_cast<u16>(frame[1])) << 15);
}

// What the underrun rule gives after the last real frame: frame `k` (1-based) of the run is
// last * (64 - k) / 64 for k <= 64, then 0.
s16 FadeSample(s16 last, std::size_t k) {
    if (k > kFadeFrames) {
        return 0;
    }
    return static_cast<s16>(static_cast<s32>(last) * static_cast<s32>(kFadeFrames - k) / static_cast<s32>(kFadeFrames));
}

// Checks that the frames a Pull did not take from the ring follow the rule, from the last real frame
// seen so far. `since` counts the frames given since then.
struct TailChecker {
    s16 last[2] = {0, 0};
    std::size_t since = 0;
    u64 bad = 0;
    void Observe(const s16* out, std::size_t real, std::size_t frames) {
        if (real > 0) {
            last[0] = out[(real - 1) * 2];
            last[1] = out[(real - 1) * 2 + 1];
            since = 0;
        }
        for (std::size_t i = real; i < frames; ++i) {
            ++since;
            if (out[i * 2] != FadeSample(last[0], since) || out[i * 2 + 1] != FadeSample(last[1], since)) {
                ++bad;
            }
        }
    }
};

// ---- the simulated producer and device -------------------------------------------------------------------

constexpr std::size_t kBlockFrames = 480;       // the device's block, at 48,000 Hz
constexpr double kDeviceHz = 48000.0;
constexpr s64 kSecond = 1000000000;

// The DSP on the game clock and a device with a crystal `ppm` parts per million fast, on one
// manual clock. The consumer steers as docs/native/audio-out.md says: before each block it gives
// the controller the fill in the middle of the block it is about to take.
class Sim {
public:
    explicit Sim(double ppm, std::size_t target_frames = MsToFrames(kDefaultLatencyMs))
        : stream(target_frames), ppm_(ppm) {
        next_block_ns_ = BlockNs(1);
    }

    ManualTimeSource clock;
    StreamSink stream;
    DriftController controller;
    TailChecker tail;

    // Statistics of the run.
    u64 offered = 0;                 // frames the producer offered
    u64 delivered = 0;               // real frames the device got
    u64 skipped_by_sequence = 0;     // frames missing between two real frames, by sequence number
    u64 duplicates_or_reordered = 0;
    double min_factor = 10.0, max_factor = 0.0;
    std::size_t min_fill = ~std::size_t{0}, max_fill = 0;  // sampled after the warm-up
    s64 sample_after_ns = 5 * kSecond;
    bool sampling = true;            // false: do not track the fill

    s64 NowNs() { return clock.NowNs(); }
    bool ProducerDue() const { return producing && next_produce_ns_ <= next_block_ns_; }
    s64 NextEventNs() const { return producing ? std::min(next_produce_ns_, next_block_ns_) : next_block_ns_; }

    // Runs the next event (the producer's audio frame, or the device's block), moving the clock to it.
    // Returns true for a producer event.
    bool Step() {
        const bool producer = ProducerDue();
        clock.Set(NextEventNs());
        if (producer) {
            Produce(kDspFrameSamples);
            game_ticks_ += kDspFrameTicks;
            next_produce_ns_ = produce_base_ns_ + static_cast<s64>(TicksToNs(game_ticks_ + kDspFrameTicks));
            Sample();
        } else {
            Block();
        }
        return producer;
    }

    void RunUntil(s64 end_ns) {
        while (NextEventNs() <= end_ns) {
            Step();
        }
        clock.Set(end_ns);
    }

    // The producer stands still for `ns` of host time (the game clock paused or held).
    void StopProducer() { producing = false; }
    void ResumeProducer() {
        producing = true;
        // The game clock did not move while it was stopped; the next audio frame is due one
        // frame after now.
        produce_base_ns_ = clock.NowNs() - static_cast<s64>(TicksToNs(game_ticks_));
        next_produce_ns_ = produce_base_ns_ + static_cast<s64>(TicksToNs(game_ticks_ + kDspFrameTicks));
    }

    void Produce(std::size_t frames) {
        MakeFrames(scratch_, offered, frames);
        stream.Queue(scratch_.data(), frames);
        offered += frames;
    }

    // One device block: steer, pull what the block needs, check what came back.
    void Block() {
        const s64 now = clock.NowNs();
        const double dt = static_cast<double>(now - last_block_ns_) / 1e9;
        last_block_ns_ = now;
        const double nominal = kBlockFrames * kDspRate.Value() / kDeviceHz;
        double factor = 1.0;
        if (stream.Priming()) {
            controller.Reset();
        } else {
            factor = controller.Update(static_cast<double>(stream.Fill()) - 0.5 * nominal,
                                       static_cast<double>(stream.Target()), dt);
            if (now >= sample_after_ns) {
                min_factor = std::min(min_factor, factor);
                max_factor = std::max(max_factor, factor);
            }
        }
        accumulator_ += nominal * factor;
        const std::size_t need = static_cast<std::size_t>(accumulator_);
        accumulator_ -= static_cast<double>(need);

        Sample();
        if (out_.size() < need * 2) {
            out_.resize(need * 2);
        }
        const std::size_t real = stream.Pull(out_.data(), need);
        for (std::size_t i = 0; i < real; ++i) {
            const u64 seq = SeqOf(&out_[i * 2]);
            if (have_last_seq_) {
                if (seq <= last_seq_) {
                    ++duplicates_or_reordered;
                } else {
                    skipped_by_sequence += seq - last_seq_ - 1;
                }
            } else {
                skipped_by_sequence += seq;  // frames before the first one the device got
            }
            have_last_seq_ = true;
            last_seq_ = seq;
        }
        delivered += real;
        tail.Observe(out_.data(), real, need);
        Sample();
        ++blocks;
        next_block_ns_ = BlockNs(blocks + 1);
    }

    u64 blocks = 0;
    bool producing = true;
    const std::vector<s16>& LastOutput() const { return out_; }

    void Sample() {
        if (!sampling || clock.NowNs() < sample_after_ns) {
            return;
        }
        const std::size_t fill = stream.Fill();
        min_fill = std::min(min_fill, fill);
        max_fill = std::max(max_fill, fill);
    }
    void ResetFillWindow() {
        min_fill = ~std::size_t{0};
        max_fill = 0;
        min_factor = 10.0;
        max_factor = 0.0;
    }

private:
    // The k-th block ends k * 480 / (48,000 * (1 + ppm / 1e6)) seconds after the start.
    s64 BlockNs(u64 k) const {
        const double base = 1e6 + ppm_;
        return static_cast<s64>(static_cast<double>(k) * 1e13 / base * 1.0 + 0.5);
    }

    double ppm_;
    std::vector<s16> scratch_, out_;
    double accumulator_ = 0;
    u64 game_ticks_ = 0;
    s64 produce_base_ns_ = 0;
    s64 next_produce_ns_ = static_cast<s64>(TicksToNs(kDspFrameTicks));
    s64 next_block_ns_ = 0;
    s64 last_block_ns_ = 0;
    u64 last_seq_ = 0;
    bool have_last_seq_ = false;
};

// The fill window the Done-when asks for: within 25 % of the target.
void CheckWithin25Percent(const Sim& sim) {
    const double target = static_cast<double>(sim.stream.Target());
    CHECK(static_cast<double>(sim.min_fill) >= 0.75 * target);
    CHECK(static_cast<double>(sim.max_fill) <= 1.25 * target);
}

void CheckNothingLost(const Sim& sim) {
    CHECK(sim.duplicates_or_reordered == 0);
    CHECK(sim.skipped_by_sequence == 0);
    CHECK(sim.tail.bad == 0);
}

}  // namespace

// ---- the numbers --------------------------------------------------------------------------------------------

TEST_CASE("the DSP's rate is 160 samples every 1,310,720 ticks, exactly") {
    CHECK(kDspRate.num == kTicksPerSecond);
    CHECK(kDspRate.den == 8192);
    CHECK(kDspRate.num * kDspFrameTicks == kDspFrameSamples * kTicksPerSecond * kDspRate.den);
    CHECK(kDspRate.Value() == 32728.498046875);  // 32728 + 255/512, exact in a double
    CHECK(static_cast<u32>(kDspRate.Value()) == kStreamSampleRate);
    CHECK(MsToFrames(40) == 1309);
    CHECK(MsToFrames(200) == kOverflowFrames);
    CHECK(kOverflowFrames < kRingFrames);
    CHECK(kRingFrames == 8192);
}

// ---- the stream: order, wrap-around, fill ---------------------------------------------------------------

TEST_CASE("Queue and Pull keep the order across the end of the ring, and Fill and Stats follow") {
    StreamSink stream(100);
    std::vector<s16> in, out(2000 * 2);
    u64 queued = 0, pulled = 0;
    // 100 rounds of 700 in and 700 out walk the indices round the ring (8192) several times
    for (int round = 0; round < 100; ++round) {
        MakeFrames(in, queued, 700);
        stream.Queue(in.data(), 700);
        queued += 700;
        CHECK(stream.Fill() == 700);
        CHECK(stream.Pull(out.data(), 700) == 700);
        for (std::size_t i = 0; i < 700; ++i) {
            REQUIRE(SeqOf(&out[i * 2]) == pulled + i);
        }
        pulled += 700;
        CHECK(stream.Fill() == 0);
    }
    const StreamStats s = stream.Stats();
    CHECK(s.queued == queued);
    CHECK(s.pulled == pulled);
    CHECK(s.underruns == 0);
    CHECK(s.overflows == 0);
    CHECK(s.dropped == 0);
    stream.Queue(in.data(), 0);  // nothing
    CHECK(stream.Fill() == 0);
}

TEST_CASE("a stream waits for its target before the first frame, and that is no underrun") {
    StreamSink stream(1000);
    CHECK(stream.Priming());
    std::vector<s16> in, out(500 * 2, 1);
    MakeFrames(in, 0, 999);
    stream.Queue(in.data(), 999);
    CHECK(stream.Pull(out.data(), 100) == 0);  // 999 < 1000
    for (std::size_t i = 0; i < 100 * 2; ++i) {
        CHECK(out[i] == 0);  // silence, not what was in the buffer
    }
    MakeFrames(in, 999, 1);
    stream.Queue(in.data(), 1);
    CHECK(stream.Pull(out.data(), 100) == 100);
    CHECK(!stream.Priming());
    CHECK(SeqOf(&out[0]) == 0);
    CHECK(stream.Stats().underruns == 0);
}

TEST_CASE("an underrun fades from the last sample over 64 frames, then zeros, and is counted once per run") {
    StreamSink stream(100);
    std::vector<s16> in(300 * 2), out(400 * 2, 7);
    for (std::size_t i = 0; i < 300; ++i) {
        in[i * 2] = 6400;       // left: a value that divides by 64
        in[i * 2 + 1] = -3201;  // right: one that does not (the fade rounds toward zero)
    }
    stream.Queue(in.data(), 300);
    CHECK(stream.Pull(out.data(), 50) == 50);
    CHECK(stream.Pull(out.data(), 250) == 250);  // exactly empties it: no underrun yet
    CHECK(stream.Stats().underruns == 0);

    // the ring is dry: 10 frames in each of 20 pulls; the fade runs across the pulls
    std::vector<s16> tail;
    for (int pull = 0; pull < 20; ++pull) {
        CHECK(stream.Pull(out.data(), 10) == 0);
        tail.insert(tail.end(), out.begin(), out.begin() + 20);
    }
    CHECK(stream.Stats().underruns == 1);
    for (std::size_t k = 1; k <= 200 / 1 && k <= tail.size() / 2; ++k) {
        const s16 left = tail[(k - 1) * 2], right = tail[(k - 1) * 2 + 1];
        if (k <= 64) {
            CHECK(left == 6400 * static_cast<int>(64 - k) / 64);
            CHECK(right == -3201 * static_cast<int>(64 - k) / 64);
        } else {
            CHECK(left == 0);
            CHECK(right == 0);
        }
    }
    CHECK(tail[63 * 2] == 0);  // the 64th frame has reached zero
    CHECK(tail[62 * 2] == 100);

    // a pull that is short mid-way: the real frames first, then the fade from the last of them
    CHECK(stream.Priming());
    MakeFrames(in, 0, 100);
    stream.Queue(in.data(), 100);  // reaches the target (100) and the block (100): it plays again
    CHECK(stream.Pull(out.data(), 100) == 100);
    CHECK(!stream.Priming());
    CHECK(stream.Stats().underruns == 1);  // the run of short pulls ended
    MakeFrames(in, 100, 60);
    stream.Queue(in.data(), 60);
    CHECK(stream.Pull(out.data(), 150) == 60);  // 90 frames short
    CHECK(stream.Priming());  // it ran dry: it waits for the target again
    CHECK(stream.Stats().underruns == 2);
    CHECK(SeqOf(&out[0]) == 100);
    CHECK(out[60 * 2] == FadeSample(out[59 * 2], 1));
    CHECK(out[60 * 2 + 1] == FadeSample(out[59 * 2 + 1], 1));
    CHECK(out[149 * 2] == FadeSample(out[59 * 2], 90));
}

TEST_CASE("a stream that ran dry stays silent until it has refilled to max(target, the block)") {
    StreamSink stream(100);
    std::vector<s16> in, out(1000 * 2);
    MakeFrames(in, 0, 100);
    stream.Queue(in.data(), 100);
    CHECK(stream.Pull(out.data(), 100) == 100);
    CHECK(stream.Pull(out.data(), 100) == 0);  // dry
    CHECK(stream.Priming());
    CHECK(stream.Stats().underruns == 1);

    MakeFrames(in, 100, 150);
    stream.Queue(in.data(), 150);
    CHECK(stream.Pull(out.data(), 200) == 0);  // 150 >= the target, but not the 200-frame block
    CHECK(stream.Priming());
    MakeFrames(in, 250, 60);
    stream.Queue(in.data(), 60);
    CHECK(stream.Pull(out.data(), 200) == 200);
    CHECK(SeqOf(&out[0]) == 100);  // nothing was lost while it waited
    CHECK(SeqOf(&out[199 * 2]) == 299);
    CHECK(stream.Stats().underruns == 1);
}

// ---- overflow and a full ring -----------------------------------------------------------------------------

TEST_CASE("an overflow leaves the oldest frames for Pull to skip and count, and Queue never moves the read side") {
    StreamSink stream(1000);
    std::vector<s16> in, out(8000 * 2);
    MakeFrames(in, 0, 4000);
    stream.Queue(in.data(), 4000);
    MakeFrames(in, 4000, 4000);  // 8000 would be waiting: 1454 past the mark
    stream.Queue(in.data(), 4000);
    StreamStats s = stream.Stats();
    CHECK(s.overflows == 1);
    CHECK(s.dropped == 0);  // the consumer has not run: nothing was counted yet
    CHECK(s.queued == 8000);
    CHECK(stream.Fill() == kOverflowFrames);

    CHECK(stream.Pull(out.data(), 100) == 100);
    s = stream.Stats();
    CHECK(s.dropped == 8000 - kOverflowFrames);
    CHECK(SeqOf(&out[0]) == 8000 - kOverflowFrames);  // the newest 200 ms are what is left
    CHECK(SeqOf(&out[99 * 2]) == 8000 - kOverflowFrames + 99);
    CHECK(stream.Fill() == kOverflowFrames - 100);
    CHECK(s.underruns == 0);

    // offered = pulled + dropped + fill
    CHECK(8000 == stream.Stats().pulled + stream.Stats().dropped + stream.Fill());
}

TEST_CASE("a full ring discards what does not fit and counts it as dropped") {
    StreamSink stream(1000);
    std::vector<s16> in, out(kRingFrames * 2);
    MakeFrames(in, 0, 6000);
    stream.Queue(in.data(), 6000);
    MakeFrames(in, 6000, 6000);  // only 2192 fit
    stream.Queue(in.data(), 6000);
    StreamStats s = stream.Stats();
    CHECK(s.queued == kRingFrames);
    CHECK(s.dropped == 12000 - kRingFrames);
    CHECK(s.overflows >= 1);
    MakeFrames(in, 12000, 100);
    stream.Queue(in.data(), 100);  // full: all of it goes
    CHECK(stream.Stats().queued == kRingFrames);
    CHECK(stream.Stats().dropped == 12100 - kRingFrames);

    // the consumer then sees the oldest frames up to the mark skipped, and the frames in order
    const std::size_t got = stream.Pull(out.data(), kRingFrames);
    CHECK(got == kOverflowFrames);
    CHECK(SeqOf(&out[0]) == kRingFrames - kOverflowFrames);
    CHECK(SeqOf(&out[(got - 1) * 2]) == kRingFrames - 1);
    CHECK(stream.Stats().dropped == 12100 - kRingFrames + (kRingFrames - kOverflowFrames));
}

TEST_CASE("with no consumer, 10 simulated minutes of Queue keep the ring full and count what is discarded") {
    StreamSink stream;
    ManualTimeSource clock;
    std::vector<s16> in;
    u64 offered = 0, ticks = 0;
    const s64 end = 600 * kSecond;
    while (true) {
        ticks += kDspFrameTicks;
        const s64 at = static_cast<s64>(TicksToNs(ticks));
        if (at > end) {
            break;
        }
        clock.Set(at);
        MakeFrames(in, offered, kDspFrameSamples);
        stream.Queue(in.data(), kDspFrameSamples);  // returns at once, whatever the ring holds
        offered += kDspFrameSamples;
    }
    CHECK(offered > 19000000);  // 600 s of 32,728.498 Hz
    const StreamStats s = stream.Stats();
    CHECK(s.queued == kRingFrames);
    CHECK(s.dropped == offered - kRingFrames);
    CHECK(s.underruns == 0);
    CHECK(s.overflows > 0);
    CHECK(stream.Fill() == kOverflowFrames);

    // a device that opens late finds the oldest frames gone and plays on from there
    std::vector<s16> out(kRingFrames * 2);
    CHECK(stream.Pull(out.data(), 256) == 256);
    CHECK(SeqOf(&out[0]) == kRingFrames - kOverflowFrames);
}

// ---- the drift controller ------------------------------------------------------------------------------------

TEST_CASE("the drift controller is 1 at rest, bounded, and moves the right way") {
    DriftController c;
    CHECK(c.Factor() == 1.0);
    for (int i = 0; i < 1000; ++i) {
        CHECK(c.Update(1000, 1000, 0.01) == doctest::Approx(1.0));
    }
    // too full: the consumer should use the stream faster; too empty: slower
    DriftController full, empty;
    for (int i = 0; i < 2000; ++i) {
        full.Update(2000, 1000, 0.01);
        empty.Update(0, 1000, 0.01);
        CHECK(full.Factor() <= DriftController::kMaxFactor);
        CHECK(empty.Factor() >= DriftController::kMinFactor);
    }
    CHECK(full.Factor() == doctest::Approx(1.005));
    CHECK(empty.Factor() == doctest::Approx(0.995));
    full.Reset();
    CHECK(full.Factor() == 1.0);

    // the low-pass: one step moves it by 1 - exp(-dt / 0.712) of the way
    DriftController slow;
    const double after = slow.Update(1050, 1000, 0.01);
    CHECK(after > 1.0);
    const double step = 0.005 * (1.0 - std::exp(-0.01 / 0.712));
    CHECK(after < 1.0 + step * 1.0001);
    CHECK(after > 1.0 + step * 0.9999);

    // inputs that make no sense change nothing
    DriftController odd;
    CHECK(odd.Update(500, 0, 0.01) == 1.0);
    CHECK(odd.Update(500, 1000, 0) == 1.0);
    CHECK(odd.Update(500, 1000, -1) == 1.0);
    CHECK(odd.Update(std::nan(""), 1000, 0.01) == 1.0);
    // a huge time step is as bounded as a small one
    CHECK(odd.Update(0, 1000, 1000.0) >= DriftController::kMinFactor);
    CHECK(odd.Update(1e9, 1000, 1000.0) <= DriftController::kMaxFactor);
}

// ---- the simulated clocks ----------------------------------------------------------------------------------

TEST_CASE("10 simulated minutes with the device's clock 0.01 % fast: the fill holds, nothing underruns or overflows") {
    Sim sim(+100.0);
    sim.RunUntil(600 * kSecond);
    const StreamStats s = sim.stream.Stats();
    CHECK(sim.blocks > 59000);
    CHECK(s.underruns == 0);
    CHECK(s.overflows == 0);
    CHECK(s.dropped == 0);
    CheckWithin25Percent(sim);
    CHECK(sim.min_factor >= DriftController::kMinFactor);
    CHECK(sim.max_factor <= DriftController::kMaxFactor);
    CheckNothingLost(sim);
    CHECK(sim.delivered + sim.stream.Fill() == sim.offered);
}

TEST_CASE("10 simulated minutes with the device's clock 0.01 % slow") {
    Sim sim(-100.0);
    sim.RunUntil(600 * kSecond);
    const StreamStats s = sim.stream.Stats();
    CHECK(s.underruns == 0);
    CHECK(s.overflows == 0);
    CHECK(s.dropped == 0);
    CheckWithin25Percent(sim);
    CHECK(sim.min_factor >= DriftController::kMinFactor);
    CHECK(sim.max_factor <= DriftController::kMaxFactor);
    CheckNothingLost(sim);
}

// Beyond the Done-when (which is 0.01 %): a crystal 0.3 % off needs most of the controller's range.
// The fill sits further from the target, so what is asserted is that the stream keeps going, inside
// half to one and a half targets.
TEST_CASE("10 simulated minutes with the device's clock 0.3 % fast and slow: nothing underruns, overflows or is lost") {
    for (const double ppm : {3000.0, -3000.0}) {
        CAPTURE(ppm);
        Sim sim(ppm);
        sim.RunUntil(600 * kSecond);
        const StreamStats s = sim.stream.Stats();
        CHECK(s.underruns == 0);
        CHECK(s.overflows == 0);
        CHECK(s.dropped == 0);
        CHECK(sim.min_factor >= DriftController::kMinFactor);
        CHECK(sim.max_factor <= DriftController::kMaxFactor);
        CheckNothingLost(sim);
        const double target = static_cast<double>(sim.stream.Target());
        CHECK(static_cast<double>(sim.min_fill) >= 0.5 * target);
        CHECK(static_cast<double>(sim.max_fill) <= 1.5 * target);
    }
}

TEST_CASE("a 2 s stop of the producer gives exactly one underrun, the fade, then zeros, and recovery") {
    Sim sim(+100.0);
    sim.RunUntil(30 * kSecond);
    CHECK(sim.stream.Stats().underruns == 0);
    CHECK(!sim.stream.Priming());
    const u64 fade_checked_before = sim.tail.bad;
    CHECK(fade_checked_before == 0);

    sim.StopProducer();
    sim.RunUntil(32 * kSecond);
    CHECK(sim.stream.Stats().underruns == 1);
    CHECK(sim.stream.Priming());
    CHECK(sim.stream.Fill() == 0);
    CHECK(sim.tail.bad == 0);  // every frame after the last real one follows the fade and the zeros
    CHECK(sim.tail.since > kFadeFrames);  // and it went on into zeros
    // the last block of the stall is silence
    const std::vector<s16>& last = sim.LastOutput();
    for (std::size_t i = 0; i < kBlockFrames / 2; ++i) {
        REQUIRE(last[i] == 0);
    }

    const u64 delivered_at_stop = sim.delivered;
    sim.ResumeProducer();
    sim.RunUntil(32 * kSecond + 10 * kSecond);  // back to the target within 10 s
    CHECK(!sim.stream.Priming());
    CHECK(sim.delivered > delivered_at_stop + 100000);
    sim.ResetFillWindow();
    sim.sample_after_ns = 0;
    sim.RunUntil(32 * kSecond + 60 * kSecond);
    CheckWithin25Percent(sim);
    const StreamStats s = sim.stream.Stats();
    CHECK(s.underruns == 1);  // the stall was one run, and the recovery added none
    CHECK(s.overflows == 0);
    CHECK(s.dropped == 0);
    CHECK(sim.tail.bad == 0);
    CheckNothingLost(sim);  // the frames after the stall follow the ones before it, none lost
}

TEST_CASE("the producer stopped for 2 s from the start of the run: silence, and no underrun to count") {
    Sim sim(0.0);
    sim.StopProducer();
    sim.RunUntil(2 * kSecond);
    CHECK(sim.stream.Stats().underruns == 0);
    CHECK(sim.stream.Priming());
    sim.ResumeProducer();
    sim.sample_after_ns = 8 * kSecond;
    sim.RunUntil(60 * kSecond);
    CHECK(!sim.stream.Priming());
    CHECK(sim.stream.Stats().underruns == 0);
    CheckWithin25Percent(sim);
    CHECK(sim.tail.bad == 0);
}

TEST_CASE("an overflow is dropped by the consumer, counted, and the fill returns to the target") {
    Sim sim(+100.0);
    sim.RunUntil(20 * kSecond);
    CHECK(sim.stream.Stats().overflows == 0);

    // the scheduler was late and the DSP catches up: 6500 frames at once, in one Queue call
    const std::size_t before = sim.stream.Fill();
    sim.Produce(6500);
    const std::size_t excess = before + 6500 - kOverflowFrames;
    CHECK(excess > 1000);
    CHECK(sim.stream.Stats().overflows == 1);
    CHECK(sim.stream.Fill() == kOverflowFrames);
    CHECK(sim.stream.Stats().dropped == 0);  // counted when the consumer skips them

    sim.sampling = false;  // the fill is not judged while it comes back
    for (int i = 0; i < 10 && sim.stream.Stats().dropped == 0; ++i) {
        sim.Step();  // up to the next device block, which is the first Pull since
    }
    // the skip is what was beyond the mark: the excess at the burst, plus the audio frames the DSP
    // made before the device's next block (each of those left the fill above the mark, and counted)
    const u64 overflows_at_skip = sim.stream.Stats().overflows;
    CHECK(overflows_at_skip >= 2);
    CHECK(overflows_at_skip <= 3);
    CHECK(sim.stream.Stats().dropped >= excess);
    CHECK(sim.stream.Stats().dropped <= excess + (overflows_at_skip - 1) * kDspFrameSamples);

    // The fill is at the mark, and the controller can take back only 0.5 % of the stream per second
    // (about 33 s for the 160 ms above the target). Until it is below the mark the DSP's next frames
    // go over it again, a few frames each time; after 20 s that is over and nothing more is lost.
    sim.RunUntil(20 * kSecond + 20 * kSecond);
    const StreamStats settled = sim.stream.Stats();
    CHECK(settled.overflows < 100);
    CHECK(settled.dropped < excess + 100 * kBlockFrames);
    sim.RunUntil(20 * kSecond + 90 * kSecond);  // and back to the target within 90 s
    sim.sampling = true;
    sim.sample_after_ns = 0;
    sim.ResetFillWindow();
    sim.RunUntil(20 * kSecond + 150 * kSecond);
    CheckWithin25Percent(sim);
    const StreamStats s = sim.stream.Stats();
    CHECK(s.underruns == 0);
    CHECK(s.overflows == settled.overflows);
    CHECK(s.dropped == settled.dropped);
    // what the device did not get is exactly the dropped frames, and nothing came twice or out of order
    CHECK(sim.duplicates_or_reordered == 0);
    CHECK(sim.skipped_by_sequence == s.dropped);
    CHECK(sim.tail.bad == 0);
    CHECK(sim.min_factor >= DriftController::kMinFactor);
    CHECK(sim.max_factor <= DriftController::kMaxFactor);
}

// ---- two real threads ---------------------------------------------------------------------------------------------

TEST_CASE("Pull and Queue on two threads for 10^6 frames, overflows forced, lose and repeat nothing but counted drops") {
    constexpr u64 kTotal = 1000000;
    StreamSink stream(500);
    std::atomic<bool> producer_done{false};
    std::atomic<u64> offered{0};

    std::thread producer([&] {
        std::vector<s16> in;
        const std::size_t sizes[] = {160, 1, 37, 480, 160, 3, 1024, 160};
        u64 seq = 0;
        std::size_t i = 0;
        u64 bursts = 0;
        while (seq < kTotal) {
            std::size_t n = sizes[i++ % std::size(sizes)];
            // every so often a burst bigger than the 200 ms mark, and one bigger than the ring: the
            // overflow and the full-ring paths happen whatever the consumer is doing
            if (i % 97 == 0) {
                n = (++bursts % 2) ? 7000 : 9000;
            }
            n = static_cast<std::size_t>(std::min<u64>(n, kTotal - seq));
            MakeFrames(in, seq, n);
            stream.Queue(in.data(), n);
            seq += n;
            if (i % 5 == 0) {
                std::this_thread::yield();
            }
        }
        offered.store(seq);
        producer_done.store(true, std::memory_order_release);
    });

    u64 delivered = 0, last_seq = 0, missing = 0, bad_order = 0, bad_tail = 0;
    bool have_last = false;
    std::vector<s16> out(4096 * 2);
    TailChecker tail;
    std::size_t sizes[] = {480, 327, 1, 1500, 64, 700, 2048, 5};
    std::size_t i = 0;
    auto take = [&](std::size_t frames) {
        const std::size_t real = stream.Pull(out.data(), frames);
        tail.Observe(out.data(), real, frames);
        for (std::size_t k = 0; k < real; ++k) {
            const u64 seq = SeqOf(&out[k * 2]);
            if (have_last && seq <= last_seq) {
                ++bad_order;
            } else {
                missing += have_last ? seq - last_seq - 1 : seq;
            }
            have_last = true;
            last_seq = seq;
        }
        delivered += real;
        return real;
    };
    while (!producer_done.load(std::memory_order_acquire)) {
        take(sizes[i++ % std::size(sizes)]);
        if (i % 7 == 0) {
            std::this_thread::yield();
        }
    }
    producer.join();
    // drain what is left: with the producer gone there is no more waiting for a target
    stream.SetTarget(0);
    while (stream.Fill() > 0) {
        take(1);
    }
    take(1);  // applies a skip that was marked last
    bad_tail = tail.bad;

    const StreamStats s = stream.Stats();
    CHECK(offered.load() == kTotal);
    CHECK(bad_order == 0);
    CHECK(bad_tail == 0);
    CHECK(s.overflows > 0);
    CHECK(s.dropped > 0);
    CHECK(s.pulled == delivered);
    // every offered frame was pulled or counted as dropped; the frames the device never got by
    // sequence number (those before its first, in between, and the ones at the end) are the drops
    CHECK(delivered + s.dropped == kTotal);
    CHECK(missing + (kTotal - 1 - last_seq) == s.dropped);
    CHECK(stream.Fill() == 0);
}

// ---- the WAV file -------------------------------------------------------------------------------------------------------

namespace {

std::filesystem::path TempFile(const std::string& name) {
    static std::atomic<int> counter{0};
    return std::filesystem::temp_directory_path() /
           ("mh4n_audio_out_test_" + std::to_string(counter++) + "_" + name);
}

std::vector<u8> ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

u32 Get32(const std::vector<u8>& b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<u32>(b[at + 3]) << 24);
}
u32 Get16(const std::vector<u8>& b, std::size_t at) {
    return b[at] | (b[at + 1] << 8);
}

// Checks the header of a stereo 16-bit WAV with `frames` frames and returns the samples.
std::vector<s16> ReadWav(const std::filesystem::path& path, u32 rate, u64 frames) {
    const std::vector<u8> b = ReadAll(path);
    REQUIRE(b.size() == 44 + frames * 4);
    CHECK(std::string(b.begin(), b.begin() + 4) == "RIFF");
    CHECK(Get32(b, 4) == 36 + frames * 4);
    CHECK(std::string(b.begin() + 8, b.begin() + 12) == "WAVE");
    CHECK(std::string(b.begin() + 12, b.begin() + 16) == "fmt ");
    CHECK(Get32(b, 16) == 16);
    CHECK(Get16(b, 20) == 1);        // PCM
    CHECK(Get16(b, 22) == 2);        // stereo
    CHECK(Get32(b, 24) == rate);
    CHECK(Get32(b, 28) == rate * 4);  // bytes per second
    CHECK(Get16(b, 32) == 4);        // block align
    CHECK(Get16(b, 34) == 16);
    CHECK(std::string(b.begin() + 36, b.begin() + 40) == "data");
    CHECK(Get32(b, 40) == frames * 4);
    std::vector<s16> samples(frames * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<s16>(Get16(b, 44 + i * 2));
    }
    return samples;
}

}  // namespace

TEST_CASE("WavWriter writes a header with the real sizes when it is closed, and the samples as given") {
    const auto path = TempFile("writer.wav");
    std::vector<s16> in(1000 * 2);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<s16>(static_cast<int>(i * 37) - 32768 + static_cast<int>(i % 5) * 11);
    }
    WavWriter w;
    CHECK(!w.IsOpen());
    REQUIRE(w.Open(path.string(), kStreamSampleRate, 2));
    CHECK(w.Write(in.data(), 300));
    CHECK(w.Write(in.data() + 600, 700));
    CHECK(w.Write(in.data(), 0));
    CHECK(w.FramesWritten() == 1000);
    CHECK(w.Close());
    CHECK(w.Close());  // again: nothing happens
    std::vector<s16> expected(in.begin(), in.begin() + 600);
    expected.insert(expected.end(), in.begin() + 600, in.end());
    CHECK(ReadWav(path, kStreamSampleRate, 1000) == expected);

    // an empty file is a header alone
    REQUIRE(w.Open(path.string(), kStreamSampleRate, 2));
    CHECK(w.Close());
    ReadWav(path, kStreamSampleRate, 0);

    // the destructor finishes a file that was not closed
    {
        WavWriter dropped;
        REQUIRE(dropped.Open(path.string(), kStreamSampleRate, 2));
        dropped.Write(in.data(), 10);
    }
    ReadWav(path, kStreamSampleRate, 10);

    // a path that cannot be created
    WavWriter bad;
    CHECK(!bad.Open((std::filesystem::temp_directory_path() / "mh4n_no_such_folder" / "x.wav").string(),
                    kStreamSampleRate, 2));
    CHECK(!bad.IsOpen());
    CHECK(!bad.Write(in.data(), 1));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// ---- through the runtime: the factories, the options, Find ---------------------------------------------------

#if !__has_include("recomp.h")
TEST_CASE("the runtime tests of audio_out_test need recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include "mh4n/memory.h"
#include "mh4n/runtime.h"

namespace {

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

struct LogCapture {
    std::vector<std::string> lines;
    LogCapture() {
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { lines.push_back(std::string(r.message)); });
    }
    ~LogCapture() { SetLogSink({}); }
    bool Has(const std::string& needle) const {
        return std::any_of(lines.begin(), lines.end(),
                           [&](const std::string& l) { return l.find(needle) != std::string::npos; });
    }
};

}  // namespace

TEST_CASE("`stream` and `wav` are registered, with their options") {
    const auto& sinks = Registry<AudioSinkFactory>::Instance();
    CHECK(sinks.Find("stream") != nullptr);
    CHECK(sinks.Find("wav") != nullptr);
    CHECK(sinks.Find("null") != nullptr);  // the default stays
    const std::string usage = Usage("runner");
    CHECK(usage.find("--audio-latency MS") != std::string::npos);
    CHECK(usage.find("--audio-file FILE") != std::string::npos);
}

TEST_CASE("--audio stream: the runtime's sink feeds the stream Find gives, at the target --audio-latency sets") {
    LogCapture log;
    {
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "stream"}), {Phase::Devices}));
        REQUIRE(rt.Interfaces().audio != nullptr);
        const auto stream = Find(rt);
        REQUIRE(stream != nullptr);
        CHECK(stream->Target() == MsToFrames(kDefaultLatencyMs));
        CHECK(Find(rt) == stream);
        std::vector<s16> in, out(500 * 2);
        MakeFrames(in, 0, 2000);
        rt.Interfaces().audio->Queue(in.data(), 2000);  // as the DSP does
        CHECK(stream->Fill() == 2000);
        CHECK(stream->Pull(out.data(), 500) == 500);
        CHECK(SeqOf(&out[0]) == 0);
        CHECK(!rt.ExitStatus().has_value());
        CHECK(log.Has("stream sink"));
    }
    {
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "stream", "--audio-latency", "60"}), {Phase::Devices}));
        const auto stream = Find(rt);
        REQUIRE(stream != nullptr);
        CHECK(stream->Target() == MsToFrames(60));
    }
    for (const char* bad : {"9", "201", "0", "-5", "forty"}) {
        CAPTURE(bad);
        Runtime rt;
        CHECK(rt.Prepare(Parse({"--audio", "stream", "--audio-latency", bad}), {Phase::Devices}));
        CHECK(rt.ExitStatus() == std::optional<int>(2));
        CHECK(rt.Interfaces().audio == nullptr);
        CHECK(Find(rt) == nullptr);
    }
    {
        Runtime rt;  // 10 and 200 are inside
        REQUIRE(rt.Prepare(Parse({"--audio", "stream", "--audio-latency", "10"}), {Phase::Devices}));
        CHECK(Find(rt)->Target() == MsToFrames(10));
        CHECK(!rt.ExitStatus().has_value());
        Runtime rt2;
        REQUIRE(rt2.Prepare(Parse({"--audio", "stream", "--audio-latency", "200"}), {Phase::Devices}));
        CHECK(Find(rt2)->Target() == MsToFrames(200));
    }
    {
        Runtime rt;  // no stream: other sinks have none to find
        REQUIRE(rt.Prepare(Parse({}), {Phase::Devices}));
        CHECK(Find(rt) == nullptr);
    }
}

TEST_CASE("the stream outlives the runtime for whoever holds it, and the frame the audio log reads follows the game") {
    std::shared_ptr<StreamSink> held;
    {
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "stream"}), {Phase::Devices}));
        held = Find(rt);
        REQUIRE(held != nullptr);

        // the frame watcher publishes the game's frame counter
        PublishFrame(0);
        REQUIRE(IsSuccess(rt.Memory().Map(0x01057000, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        REQUIRE(rt.Memory().Write<u32>(kFrameCounterAddress, 1234));
        rt.PollFrame();
        CHECK(LastFrame() == 1234);
    }
    std::vector<s16> in, out(100 * 2);
    MakeFrames(in, 0, 3000);
    held->Queue(in.data(), 3000);  // the runtime is gone; the stream is not
    CHECK(held->Pull(out.data(), 100) == 100);
    PublishFrame(7);
    CHECK(LastFrame() == 7);
}

TEST_CASE("a finished run forgets its stream, so a runtime at the same address does not find it") {
    std::shared_ptr<StreamSink> held;
    for (int round = 0; round < 8; ++round) {
        auto rt = std::make_unique<Runtime>();
        REQUIRE(rt->Prepare(Parse({"--audio", "stream"}), {Phase::Devices}));
        auto stream = Find(*rt);
        REQUIRE(stream != nullptr);
        held = stream;
        rt.reset();  // the run is over: Runtime::OnFinish
        // the allocator tends to give the next runtime the same address; whatever it gives, no stream
        auto next = std::make_unique<Runtime>();
        REQUIRE(next->Prepare(Parse({"--audio", "null"}), {Phase::Devices}));
        CHECK(Find(*next) == nullptr);
    }
    CHECK(held != nullptr);  // and the stream outlived its runtime
}

TEST_CASE("--audio wav writes every queued frame to the file, and finishes the header when the run ends") {
    LogCapture log;
    const auto path = TempFile("sink.wav");
    std::vector<s16> in;
    MakeFrames(in, 0, 5000);
    {
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "wav", "--audio-file", path.string()}), {Phase::Devices}));
        REQUIRE(rt.Interfaces().audio != nullptr);
        CHECK(!rt.ExitStatus().has_value());
        for (std::size_t at = 0; at < 5000; at += 160) {
            rt.Interfaces().audio->Queue(&in[at * 2], std::min<std::size_t>(160, 5000 - at));
        }
        CHECK(Find(rt) == nullptr);
    }  // Runtime::OnFinish
    CHECK(ReadWav(path, 32728, 5000) == in);

    {
        Runtime rt;  // a file that cannot be written, and no file at all
        CHECK(rt.Prepare(Parse({"--audio", "wav", "--audio-file",
                                (std::filesystem::temp_directory_path() / "mh4n_no_such_folder" / "x.wav").string()}),
                         {Phase::Devices}));
        CHECK(rt.ExitStatus() == std::optional<int>(2));
        CHECK(rt.Interfaces().audio == nullptr);
    }
    {
        Runtime rt;
        CHECK(rt.Prepare(Parse({"--audio", "wav"}), {Phase::Devices}));
        CHECK(rt.ExitStatus() == std::optional<int>(2));
        CHECK(log.Has("needs --audio-file"));
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

#endif  // __has_include("recomp.h")
