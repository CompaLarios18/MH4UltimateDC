// The game clock and the two time sources (N1-06). Our own code; the only thing taken from Azahar is
// the tick rate (BASE_CLOCK_RATE_ARM11 of core/core_timing.h, a hardware constant).
#include "mh4n/clock.h"

#include <algorithm>

#include "mh4n/assert.h"

namespace mh4n {

// ---- SteadyTimeSource --------------------------------------------------------------------------------

s64 SteadyTimeSource::NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                epoch_)
        .count();
}

bool SteadyTimeSource::WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                                 std::optional<s64> deadline_ns, const std::function<bool()>& pred) {
    if (!deadline_ns) {
        cv.wait(lock, pred);
        return true;
    }
    return cv.wait_until(lock, epoch_ + std::chrono::nanoseconds(*deadline_ns), pred);
}

// ---- ManualTimeSource --------------------------------------------------------------------------------

bool ManualTimeSource::WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                                 std::optional<s64> deadline_ns, const std::function<bool()>& pred) {
    if (pred()) {
        return true;
    }
    if (!deadline_ns) {
        cv.wait(lock, pred);
        return true;
    }
    // Nothing else can happen before the deadline in a simulation: skip the idle time.
    s64 now = now_.load(std::memory_order_acquire);
    while (now < *deadline_ns && !now_.compare_exchange_weak(now, *deadline_ns)) {
    }
    return pred();
}

void ManualTimeSource::Advance(s64 ns) {
    MH4N_ASSERT(ns >= 0, "ManualTimeSource::Advance({})", ns);
    now_.fetch_add(ns, std::memory_order_acq_rel);
    if (hook_) {
        hook_();
    }
}

void ManualTimeSource::Set(s64 ns) {
    MH4N_ASSERT(ns >= now_.load(), "ManualTimeSource::Set({}) goes backwards", ns);
    now_.store(ns, std::memory_order_release);
    if (hook_) {
        hook_();
    }
}

// ---- GameClock ---------------------------------------------------------------------------------------

GameClock::GameClock(TimeSource& source) : source_(source), base_host_ns_(source.NowNs()) {}

u64 GameClock::NowLocked() const {
    if (!running_) {
        return base_ticks_;
    }
    const s64 elapsed = std::max<s64>(source_.NowNs() - base_host_ns_, 0);
    // base + elapsed: NsToTicks saturates, so does the sum.
    const u64 ticks = NsToTicks(static_cast<u64>(elapsed));
    u64 now = base_ticks_ + ticks;
    if (now < base_ticks_) {
        now = ~u64{0};
    }
    return limit_ ? std::min(now, *limit_) : now;
}

u64 GameClock::Now() const {
    std::lock_guard lock(mutex_);
    return NowLocked();
}

void GameClock::Snapshot() {
    base_ticks_ = NowLocked();
    base_host_ns_ = source_.NowNs();
}

void GameClock::Notify() {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex_);
        callback = on_change_;
    }
    if (callback) {
        callback();
    }
}

void GameClock::Pause() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        Snapshot();
        running_ = false;
    }
    Notify();
}

void GameClock::Resume() {
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
        base_host_ns_ = source_.NowNs();  // the paused time is dropped
    }
    Notify();
}

bool GameClock::Paused() const {
    std::lock_guard lock(mutex_);
    return !running_;
}

void GameClock::Hold(u64 until_tick) {
    {
        std::lock_guard lock(mutex_);
        Snapshot();  // the time that already stood still at an older limit is dropped here
        limit_ = std::max(until_tick, base_ticks_);
    }
    Notify();
}

void GameClock::ReleaseHold() {
    {
        std::lock_guard lock(mutex_);
        if (!limit_) {
            return;
        }
        Snapshot();
        limit_.reset();
    }
    Notify();
}

std::optional<u64> GameClock::HeldAt() const {
    std::lock_guard lock(mutex_);
    return limit_;
}

std::optional<s64> GameClock::HostNsFor(u64 tick) const {
    std::lock_guard lock(mutex_);
    const u64 now = NowLocked();
    if (tick <= now) {
        return source_.NowNs();
    }
    if (!running_ || (limit_ && tick > *limit_)) {
        return std::nullopt;
    }
    // base_host + the least time that covers (tick - base) ticks; the host nanoseconds are s64.
    const u64 dt_ns = TicksToNsCeil(tick - base_ticks_);
    if (dt_ns > static_cast<u64>(INT64_MAX - base_host_ns_)) {
        return std::nullopt;
    }
    return base_host_ns_ + static_cast<s64>(dt_ns);
}

void GameClock::SetChangeCallback(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    on_change_ = std::move(callback);
}

}  // namespace mh4n
