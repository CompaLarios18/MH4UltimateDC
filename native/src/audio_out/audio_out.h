// The audio output stream (N3-05, D63): what sits between the DSP, on the scheduler thread, and an
// audio device, on its own thread. No SDL here (the device is native/app/audio, N3-06).
//
//   - kDspRate and the numbers below: the DSP makes 160 samples every 1,310,720 ticks of the game
//     clock, and the audio device never paces the game.
//   - StreamSink (`--audio stream`): a single-producer single-consumer ring of stereo s16 frames.
//     Queue (scheduler thread) copies and returns, with no lock, no allocation and no wait. Pull
//     (the device's thread) copies what there is and fills the rest with the fade-out and silence.
//   - DriftController (drift_controller.h): the factor by which the consumer multiplies its
//     resampling ratio to keep the ring near its target fill.
//   - the `wav` sink (--audio wav --audio-file FILE) writes every queued frame to a WAV file.
//
// Every number marked "first guess" is one (docs/native/wp/N3.md, rule 7); the maintainer's runs
// confirm it and no worker retunes it. docs/native/audio-out.md has the details.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/host.h"
#include "mh4n/types.h"

namespace mh4n {
class Runtime;
}

namespace mh4n::audio_out {

// ---- the DSP's rate ----------------------------------------------------------------------------------

struct Fraction {
    u64 num;
    u64 den;
    constexpr double Value() const { return static_cast<double>(num) / static_cast<double>(den); }
};

inline constexpr u32 kDspFrameSamples = 160;         // sample frames the DSP makes per audio frame
inline constexpr u64 kDspFrameTicks = 1310720;       // game ticks between two audio frames
// 160 samples every 1,310,720 ticks = kTicksPerSecond / 8192 = 32,728.498... Hz of game time, exactly.
inline constexpr Fraction kDspRate{kTicksPerSecond, 8192};
static_assert(kDspRate.num * kDspFrameTicks == kTicksPerSecond * kDspFrameSamples * kDspRate.den,
              "kDspRate is 160 samples per 1,310,720 ticks");
// The rate the device-facing side and the WAV file declare (kDspRate rounded down).
inline constexpr u32 kStreamSampleRate = 32728;

// Sample frames in `ms` milliseconds of the DSP's output, rounded to the nearest frame.
constexpr std::size_t MsToFrames(double ms) {
    return static_cast<std::size_t>(kDspRate.Value() * ms / 1000.0 + 0.5);
}

// ---- the stream's numbers ----------------------------------------------------------------------------

inline constexpr std::size_t kRingFrames = 8192;     // about 250 ms; a power of two
inline constexpr u32 kDefaultLatencyMs = 40;         // the target fill: first guess
inline constexpr u32 kMinLatencyMs = 10;             // --audio-latency's range: first guess
inline constexpr u32 kMaxLatencyMs = 200;
inline constexpr std::size_t kOverflowFrames = 6546; // 200 ms: the fill above which the oldest frames are dropped (first guess)
inline constexpr std::size_t kFadeFrames = 64;       // the underrun fade-out (first guess)
static_assert(MsToFrames(200) == kOverflowFrames && kOverflowFrames < kRingFrames);
static_assert((kRingFrames & (kRingFrames - 1)) == 0);

struct StreamStats {
    u64 queued = 0;      // frames accepted into the ring
    u64 pulled = 0;      // frames Pull gave from the ring (not the fade and the silence)
    u64 underruns = 0;   // runs of short pulls, counted once each, after the first frame was played
    u64 overflows = 0;   // Queue calls that left the fill above kOverflowFrames
    u64 dropped = 0;     // frames lost: skipped by Pull after an overflow, or discarded by Queue on a full ring
};

// The stream. One producer thread calls Queue, one consumer thread calls Pull; Fill, Stats, Target
// and Priming may be read from any thread.
//
//   offered frames == pulled + dropped + Fill()   (once the producer has stopped and Pull has run
//   once more: Fill leaves out the frames an overflow marked to be skipped, which `dropped` counts
//   when Pull skips them)
class StreamSink final : public AudioSink {
public:
    // `target_frames` is the fill the consumer waits for before it starts, and after an underrun.
    explicit StreamSink(std::size_t target_frames = MsToFrames(kDefaultLatencyMs));

    // ---- producer (the scheduler thread) ----
    // Copies the frames into the ring and returns. Never blocks, never allocates. Frames that do
    // not fit (nobody pulls) are discarded and counted in `dropped`; a fill above kOverflowFrames
    // counts an overflow and marks the excess oldest frames for the consumer to skip.
    void Queue(const s16* stereo, std::size_t frames) override;

    // ---- consumer (the device's thread) ----
    // Writes `frames` stereo frames to `out` and returns how many of them came from the ring (the
    // first ones); the rest are the underrun rule: from the last sample given, a linear fade to
    // zero over kFadeFrames, then zeros. While "priming" (at the start, and after an underrun)
    // it gives silence until the ring holds max(target, frames) frames. No locks, no logging.
    std::size_t Pull(s16* out, std::size_t frames);

    // ---- any thread ----
    // Frames waiting to be pulled, not counting those an overflow has marked to be skipped.
    std::size_t Fill() const;
    StreamStats Stats() const;
    std::size_t Target() const { return target_.load(std::memory_order_relaxed); }
    void SetTarget(std::size_t frames) { target_.store(frames, std::memory_order_relaxed); }
    // True from the start, and after an underrun, until the ring has refilled to the target: the
    // consumer should not steer with the DriftController meanwhile.
    bool Priming() const { return priming_flag_.load(std::memory_order_relaxed); }

private:
    static constexpr std::size_t kMask = kRingFrames - 1;
    static constexpr std::size_t kCacheLine = 64;

    std::vector<s16> ring_;  // kRingFrames * 2, allocated once

    // The producer's side.
    alignas(kCacheLine) std::atomic<u64> write_{0};      // frames ever written; the producer stores
    std::atomic<u64> min_read_{0};                       // the read index the consumer must be at least at
    std::atomic<u64> overflows_{0};
    std::atomic<u64> discarded_{0};
    std::atomic<u64> queued_{0};

    // The consumer's side.
    alignas(kCacheLine) std::atomic<u64> read_{0};       // frames ever read or skipped; the consumer stores
    std::atomic<u64> skipped_{0};
    std::atomic<u64> pulled_{0};
    std::atomic<u64> underruns_{0};
    std::atomic<bool> priming_flag_{true};
    bool priming_ = true;                                 // consumer thread only, from here on
    bool started_ = false;                                // a real frame was given
    bool in_short_run_ = false;
    std::size_t fade_pos_ = 0;
    s16 last_[2] = {0, 0};

    alignas(kCacheLine) std::atomic<std::size_t> target_;
};

// ---- finding the stream, and the game frame ------------------------------------------------------------

// The stream of the runtime's `stream` sink (the app's device pulls from it, N3-06), or null when
// the runtime has no such sink. The pointer keeps the stream alive.
std::shared_ptr<StreamSink> Find(Runtime& runtime);

// The newest game frame (the frame watcher's counter), for the audio log. `stream` subscribes to
// Runtime::OnFrame and calls PublishFrame; LastFrame is 0 until the first frame. Any thread. One
// value per process: a second runtime in the same process shares it.
void PublishFrame(u32 game_frame);
u32 LastFrame();

}  // namespace mh4n::audio_out
