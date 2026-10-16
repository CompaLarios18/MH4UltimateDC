// Derived from Azahar src/core/hle/kernel/timer.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the reset types, Set, Cancel and Clear, the signal and the rescheduling of a
// periodic timer. The timer queue is the scheduler's (Kernel::ScheduleAt), not CoreTiming.
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project
#include <algorithm>

#include "internal.h"
#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

Timer::Timer(Kernel& kernel, ResetType reset_type) : WaitObject(kernel), reset_type_(reset_type) {}

Timer::~Timer() {
    Cancel();
    if (resource_limit) {
        resource_limit->Release(ResourceLimitType::Timer, 1);
    }
}

bool Timer::ShouldWait(const Thread*) const {
    return !signaled_;
}

void Timer::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "object unavailable!");
    if (reset_type_ == ResetType::OneShot) {
        signaled_ = false;
    }
}

void Timer::WakeupAllWaitingThreads() {
    WaitObject::WakeupAllWaitingThreads();
    if (reset_type_ == ResetType::Pulse) {
        signaled_ = false;
    }
}

void Timer::Set(s64 initial_ns, s64 interval_ns) {
    Cancel();  // get rid of any previous shot
    initial_delay_ = std::max<s64>(initial_ns, 0);
    interval_delay_ = std::max<s64>(interval_ns, 0);
    const u64 now = kernel_.NowTicks();
    if (initial_ns == 0) {
        deadline_ = now;
        Fire();  // the callback runs at once
    } else {
        Schedule(now + kernel_detail::NsToTicks(initial_delay_));
    }
}

void Timer::Cancel() {
    if (event_ != 0) {
        kernel_.Cancel(event_);
        event_ = 0;
    }
}

void Timer::Clear() {
    signaled_ = false;
}

void Timer::Schedule(u64 deadline) {
    deadline_ = deadline;
    const std::weak_ptr<Timer> weak = std::static_pointer_cast<Timer>(shared_from_this());
    event_ = kernel_.ScheduleAt(deadline, [weak] {
        if (auto timer = weak.lock()) {
            timer->Fire();
        }
    });
}

void Timer::Fire() {
    event_ = 0;  // the event that called us has run
    signaled_ = true;
    WakeupAllWaitingThreads();
    if (interval_delay_ != 0) {
        // Azahar schedules nsToCycles(interval) - cycles_late from now, i.e. one interval after
        // this shot was due. At least one tick, so that an interval below a tick does not make the
        // queue run the same instant forever.
        const u64 interval = std::max<u64>(kernel_detail::NsToTicks(interval_delay_), 1);
        Schedule(deadline_ + interval);
    }
}

} // namespace mh4n
