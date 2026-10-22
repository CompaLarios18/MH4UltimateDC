// The `stream` AudioSink (N3-05, D63): a single-producer single-consumer ring of stereo s16 frames
// between the DSP (the scheduler thread) and an audio device (its own thread). See audio_out.h.
//
// Indices are frame counts that only grow (write_, read_, min_read_); a slot is index & kMask. The
// producer alone stores write_ and min_read_, the consumer alone stores read_.
//   - Queue stores the frames, then publishes write_ (release). If the ring is physically full the
//     frames that do not fit are discarded. If the fill would pass kOverflowFrames it raises
//     min_read_ (after write_, release) to the oldest frame that may stay.
//   - Pull loads min_read_ (acquire), then write_ (acquire): min_read_ <= write_ always holds for
//     what it sees. If min_read_ is ahead of read_ it skips to it and counts the skipped frames as
//     dropped. It copies, then publishes read_ (release), which frees the slots for Queue.
// Queue never writes read_, so it never takes frames from a Pull in progress.
#include <algorithm>
#include <cstring>

#include "audio_out/audio_out.h"

namespace mh4n::audio_out {

StreamSink::StreamSink(std::size_t target_frames) : ring_(kRingFrames * 2, 0), target_(target_frames) {}

void StreamSink::Queue(const s16* stereo, std::size_t frames) {
    if (frames == 0) {
        return;
    }
    const u64 w = write_.load(std::memory_order_relaxed);
    const u64 r = read_.load(std::memory_order_acquire);

    // Only what fits is written: the ring holds w - r frames, and Queue never moves the read index.
    const std::size_t room = kRingFrames - static_cast<std::size_t>(w - r);
    const std::size_t n = std::min(frames, room);
    if (n < frames) {
        discarded_.fetch_add(frames - n, std::memory_order_relaxed);
    }
    if (n == 0) {
        return;
    }
    const std::size_t first = static_cast<std::size_t>(w) & kMask;
    const std::size_t head = std::min(n, kRingFrames - first);
    std::memcpy(&ring_[first * 2], stereo, head * 2 * sizeof(s16));
    if (head < n) {
        std::memcpy(&ring_[0], stereo + head * 2, (n - head) * 2 * sizeof(s16));
    }
    const u64 new_w = w + n;
    write_.store(new_w, std::memory_order_release);
    queued_.fetch_add(n, std::memory_order_relaxed);

    // Overflow: more than kOverflowFrames would be waiting. The consumer skips the oldest, at its next Pull.
    const u64 m = min_read_.load(std::memory_order_relaxed);
    const u64 effective_read = std::max(r, m);
    if (new_w - effective_read > kOverflowFrames) {
        min_read_.store(new_w - kOverflowFrames, std::memory_order_release);
        overflows_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t StreamSink::Pull(s16* out, std::size_t frames) {
    if (frames == 0) {
        return 0;
    }
    const u64 m = min_read_.load(std::memory_order_acquire);
    const u64 w = write_.load(std::memory_order_acquire);
    u64 r = read_.load(std::memory_order_relaxed);
    if (m > r) {
        skipped_.fetch_add(m - r, std::memory_order_relaxed);
        r = m;
        read_.store(r, std::memory_order_release);
    }
    const u64 available = w - r;

    if (priming_) {
        const std::size_t need =
            std::min(std::max(target_.load(std::memory_order_relaxed), frames), kOverflowFrames);
        if (available >= need) {
            priming_ = false;
            priming_flag_.store(false, std::memory_order_relaxed);
        }
    }

    std::size_t n = 0;
    if (!priming_) {
        n = static_cast<std::size_t>(std::min<u64>(frames, available));
        if (n > 0) {
            const std::size_t first = static_cast<std::size_t>(r) & kMask;
            const std::size_t head = std::min(n, kRingFrames - first);
            std::memcpy(out, &ring_[first * 2], head * 2 * sizeof(s16));
            if (head < n) {
                std::memcpy(out + head * 2, &ring_[0], (n - head) * 2 * sizeof(s16));
            }
            read_.store(r + n, std::memory_order_release);
            pulled_.fetch_add(n, std::memory_order_relaxed);
            last_[0] = out[(n - 1) * 2];
            last_[1] = out[(n - 1) * 2 + 1];
            started_ = true;
            fade_pos_ = 0;
        }
    }

    if (n == frames) {
        in_short_run_ = false;
        return n;
    }

    // A short pull. If it ran dry while playing, the ring has to refill to the target first.
    if (!priming_) {
        priming_ = true;
        priming_flag_.store(true, std::memory_order_relaxed);
    }
    // One underrun per run of short pulls, and only once a frame has been played: a device that
    // opens before the game has made any sound has nothing to interrupt.
    if (started_ && !in_short_run_) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }
    in_short_run_ = true;

    // From the last sample given, a linear fade to zero over kFadeFrames, then zeros.
    for (std::size_t i = n; i < frames; ++i) {
        if (fade_pos_ < kFadeFrames) {
            ++fade_pos_;
            const s32 scale = static_cast<s32>(kFadeFrames - fade_pos_);
            out[i * 2] = static_cast<s16>(last_[0] * scale / static_cast<s32>(kFadeFrames));
            out[i * 2 + 1] = static_cast<s16>(last_[1] * scale / static_cast<s32>(kFadeFrames));
        } else {
            out[i * 2] = 0;
            out[i * 2 + 1] = 0;
        }
    }
    return n;
}

std::size_t StreamSink::Fill() const {
    // m and r first, w last: both are then at most w.
    const u64 m = min_read_.load(std::memory_order_acquire);
    const u64 r = read_.load(std::memory_order_acquire);
    const u64 w = write_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(w - std::max(m, r));
}

StreamStats StreamSink::Stats() const {
    StreamStats s;
    s.queued = queued_.load(std::memory_order_relaxed);
    s.pulled = pulled_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.overflows = overflows_.load(std::memory_order_relaxed);
    s.dropped = discarded_.load(std::memory_order_relaxed) + skipped_.load(std::memory_order_relaxed);
    return s;
}

// ---- the game frame, for the audio log ------------------------------------------------------------------

namespace {
std::atomic<u32> g_last_frame{0};
}

void PublishFrame(u32 game_frame) {
    g_last_frame.store(game_frame, std::memory_order_relaxed);
}

u32 LastFrame() {
    return g_last_frame.load(std::memory_order_relaxed);
}

}  // namespace mh4n::audio_out
