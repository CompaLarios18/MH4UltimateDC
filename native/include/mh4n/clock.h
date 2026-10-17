// The game clock (N1-06): the 3DS's 268,111,856 Hz tick counter on top of the host's monotonic
// clock, and the host-time interface behind it.
//
// Two layers, so that tests and later the deterministic mode (N1-25) can drive time by hand:
//   - `TimeSource` is the host: a monotonic nanosecond counter and the one place that waits. The
//     scheduler never calls std::chrono or a condition variable's timed wait itself; it asks the
//     source. `SteadyTimeSource` is the real one, `ManualTimeSource` moves only when told.
//   - `GameClock` turns a source into ticks, minus the time spent paused (`Pause`/`Resume`) or held
//     back (`Hold`, for net sync later).
//
// Conversions go through 128-bit intermediates: ns * 268111856 overflows 64 bits after about 68 s.
// The results are exact (floor) for every input; one year of ticks is 8.46e15, well inside 64 bits.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

#include "mh4n/types.h"

namespace mh4n {

inline constexpr u64 kTicksPerSecond = 268111856;  // Azahar's BASE_CLOCK_RATE_ARM11 (core_timing.h)
inline constexpr u64 kNanosecondsPerSecond = 1000000000;

// Ticks in `ns` nanoseconds, rounded down. Saturates at the largest u64.
constexpr u64 NsToTicks(u64 ns) {
    const unsigned __int128 ticks =
        static_cast<unsigned __int128>(ns) * kTicksPerSecond / kNanosecondsPerSecond;
    return ticks > ~u64{0} ? ~u64{0} : static_cast<u64>(ticks);
}

// Nanoseconds in `ticks` ticks, rounded down. Saturates at the largest u64.
constexpr u64 TicksToNs(u64 ticks) {
    const unsigned __int128 ns =
        static_cast<unsigned __int128>(ticks) * kNanosecondsPerSecond / kTicksPerSecond;
    return ns > ~u64{0} ? ~u64{0} : static_cast<u64>(ns);
}

// The least number of nanoseconds that covers `ticks` ticks (NsToTicks(result) >= ticks).
constexpr u64 TicksToNsCeil(u64 ticks) {
    const unsigned __int128 ns =
        (static_cast<unsigned __int128>(ticks) * kNanosecondsPerSecond + kTicksPerSecond - 1) /
        kTicksPerSecond;
    return ns > ~u64{0} ? ~u64{0} : static_cast<u64>(ns);
}

// ---- host time ---------------------------------------------------------------------------------------

class TimeSource {
public:
    virtual ~TimeSource() = default;
    // Monotonic host nanoseconds. The origin is the source's own.
    virtual s64 NowNs() = 0;
    // std::condition_variable::wait_until(lock, deadline, pred) with the deadline in NowNs() terms;
    // without a deadline it waits for the predicate alone. Returns the predicate's value when it
    // stops waiting: false means the deadline was reached first. `lock` is held when it returns.
    virtual bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                           std::optional<s64> deadline_ns, const std::function<bool()>& pred) = 0;
};

// std::chrono::steady_clock, counted from the construction of the source.
class SteadyTimeSource final : public TimeSource {
public:
    SteadyTimeSource() : epoch_(std::chrono::steady_clock::now()) {}
    s64 NowNs() override;
    bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                   std::optional<s64> deadline_ns, const std::function<bool()>& pred) override;

private:
    std::chrono::steady_clock::time_point epoch_;
};

// Time that moves only when `Advance` or `WaitUntil` moves it, for tests and the replay tool.
// WaitUntil with a deadline does not wait: it jumps the time to the deadline (the way a discrete
// event simulation skips idle time) unless the predicate is already true; without a deadline it
// waits for the predicate like the real source does, so work posted by another host thread is
// still seen.
class ManualTimeSource final : public TimeSource {
public:
    explicit ManualTimeSource(s64 start_ns = 0) : now_(start_ns) {}
    s64 NowNs() override { return now_.load(std::memory_order_acquire); }
    bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                   std::optional<s64> deadline_ns, const std::function<bool()>& pred) override;

    // Moves time forward by `ns` (or to `ns` for Set) and then calls the hook, if any, without
    // holding a lock of ours. Call it from the thread that runs the scheduler (or when it is idle).
    void Advance(s64 ns);
    void Set(s64 ns);
    // Called after every Advance and Set (tests use it to model the timer thread).
    void SetAdvanceHook(std::function<void()> hook) { hook_ = std::move(hook); }

private:
    std::atomic<s64> now_;
    std::function<void()> hook_;
};

// ---- the game clock ----------------------------------------------------------------------------------

class GameClock {
public:
    explicit GameClock(TimeSource& source);
    GameClock(const GameClock&) = delete;
    GameClock& operator=(const GameClock&) = delete;

    TimeSource& Source() const { return source_; }

    // Game ticks (268,111,856 per second) and nanoseconds since the clock was made. Any thread.
    u64 Now() const;
    u64 NowNs() const { return TicksToNs(Now()); }

    // While paused the clock stands still and the time that passes is not made up when it resumes.
    // Neither nests: Resume after two Pauses resumes. Any thread.
    void Pause();
    void Resume();
    bool Paused() const;

    // The clock never reads more than `until_tick` while the hold lasts (it stands still at that
    // value, as when paused, and the time spent standing there is not made up). A value below the
    // current reading holds the clock where it is: the clock never goes backwards. A second Hold
    // replaces the first. For net sync (later): a peer that is behind holds the others back.
    void Hold(u64 until_tick);
    void ReleaseHold();
    std::optional<u64> HeldAt() const;

    // The host time (TimeSource::NowNs terms) at which the clock reads `tick`: now, if it already
    // does; nullopt if it never will while it stays paused or held below `tick`.
    std::optional<s64> HostNsFor(u64 tick) const;

    // Called after every Pause, Resume, Hold and ReleaseHold that changed something, on the
    // calling thread and with no lock held (the scheduler wakes its idle wait with it).
    void SetChangeCallback(std::function<void()> callback);

private:
    u64 NowLocked() const;
    void Snapshot();  // base_ := the current reading, at the current host time
    void Notify();

    TimeSource& source_;
    mutable std::mutex mutex_;
    bool running_ = true;
    u64 base_ticks_ = 0;
    s64 base_host_ns_ = 0;
    std::optional<u64> limit_;
    std::function<void()> on_change_;
};

}  // namespace mh4n
