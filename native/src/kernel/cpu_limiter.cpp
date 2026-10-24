// Derived from Azahar src/core/hle/kernel/thread.cpp, thread.h, kernel.cpp and kernel.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: thread.h, kernel.h and kernel.cpp say
// so; thread.cpp says GPL-2.0-or-later): CpuLimiterMulti (Start, End, UpdateAppCpuLimit,
// DoTimeLimit, OnTick, ChangeState, WakeupSleepingThreads) and KernelSystem::SetCore1ScheduleMode /
// UpdateCore1AppCpuLimit. The timing (the one game clock of real time, or the deterministic mode's two
// clocks, one per core, where the tick event is core 1's) and the way the scheduler consults the limiter
// (a selection filter instead of ThreadManager::PopNextReadyThread) are ours.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.cpp, thread.h,
//   kernel.cpp, kernel.h)
// PPSSPP Project (thread.h, kernel.h, second notice: Licensed under GPLv2 or any later version)
//
// The algorithm (Luma3DS's PM does the same on a real console): every 2 ms of game time is cut in
// two phases. In the App phase the threads of the application category on core 1 run; in the Sys
// phase they are held back, so that the system modules (which this runtime does not have: core 1
// simply idles) would get the rest. The App phase lasts `app_cpu_time` percent of the interval, the
// Sys phase the remainder. An activation starts in the Sys phase.
//
//   Update (current CpuTime value set to a non-zero p):  phase := Sys   event in S ticks
//   each event:   held threads wake;  phase flips;  the next event is in A (App) or S (Sys) ticks,
//                 less how late this one ran
//     A = trunc(base * (p / 100.f))   in single precision, base = 536223 ticks (2 ms)
//     S = base - A
//
// The tick event only flips the phase. A thread is held back when the scheduler is about to run it
// on core 1 (Azahar's PopNextReadyThread asks DoTimeLimit for the thread it selected, the running
// one included): the scheduler asks again after every timer event, as Azahar's RunLoop reschedules
// each core before every slice.
#include "cpu_limiter.h"

#include "deterministic.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"

namespace mh4n {

Core1Limiter::Core1Limiter(Kernel& kernel, GuestScheduler& scheduler)
    : kernel_(kernel), scheduler_(scheduler) {}

Core1Limiter::~Core1Limiter() {
    // The scheduler is going away or has dropped its timers already; do not wake anything.
    CancelTick();
}

void Core1Limiter::SetScheduleMode(Core1ScheduleMode mode) {
    if (mode == Core1ScheduleMode::Single) {
        // Azahar: 'LOG_WARNING(Kernel, "Unimplemented \"Single\" schedule mode.")'. It runs the
        // limiter of the Multi mode.
        MH4N_LOG(Warning, "Kernel", "Unimplemented \"Single\" schedule mode.");
    }
    End();
    mode_ = mode;
    Start();
}

void Core1Limiter::Start() {
    if (ready_) {
        return;
    }
    ready_ = true;
    active_ = false;
    phase_ = Core1Phase::App;  // so that ChangeState starts with Sys
    app_cpu_time_ = kCore1CpuTimeDisabled;
}

void Core1Limiter::End() {
    if (!ready_) {
        return;
    }
    ready_ = false;
    active_ = false;
    CancelTick();
    WakeupSleepingThreads();
}

void Core1Limiter::UpdateAppCpuLimit() {
    if (!ready_) {
        return;
    }
    app_cpu_time_ = kernel_.ApplicationResourceLimit()->GetCurrentValue(ResourceLimitType::CpuTime);
    if (app_cpu_time_ == kCore1CpuTimeDisabled) {
        // No preemption: stop the alternation and let everything go.
        active_ = false;
        CancelTick();
        WakeupSleepingThreads();
    } else {
        // Start preempting. If it was active already the pending event goes first, so that it is
        // not scheduled twice (the call also flips the phase, as in Azahar, which lets a change of
        // the share take effect sooner).
        if (active_) {
            CancelTick();
        }
        active_ = true;
        ChangeState(0);
    }
}

bool Core1Limiter::DoTimeLimit(Thread& thread) {
    if (!ready_ || !active_) {
        return false;  // preemption is not active
    }
    const auto process = thread.owner_process.lock();
    if (process == nullptr || process->resource_limit == nullptr) {
        return false;
    }
    const ResourceLimitCategory category = process->resource_limit->GetCategory();
    if (process->resource_limit->GetLimitValue(ResourceLimitType::CpuTime) ==
        kCore1CpuTimeExempted) {
        return false;  // exempt from preemption
    }
    // Azahar: "On real hardware, the kernel uses a KPreemptionTimer to determine if a thread needs to
    // be time limited. ... We can do this instead and it should be good enough."
    if ((category == ResourceLimitCategory::Application && phase_ == Core1Phase::Sys) ||
        (category == ResourceLimitCategory::Other && phase_ == Core1Phase::App)) {
        thread.status = ThreadStatus::WaitSleep;
        sleeping_.push_back(std::static_pointer_cast<Thread>(thread.shared_from_this()));
        return true;
    }
    return false;
}

void Core1Limiter::OnTick() {
    const u64 now = kernel_.NowTicks();
    const s64 late = now > tick_due_ ? static_cast<s64>(now - tick_due_) : 0;
    tick_event_ = 0;  // this event has run
    WakeupSleepingThreads();
    ChangeState(late);
}

void Core1Limiter::ChangeState(s64 cycles_late) {
    phase_ = phase_ == Core1Phase::Sys ? Core1Phase::App : Core1Phase::Sys;

    // Single precision, as Azahar's `base_tick_interval * (app_cpu_time / 100.f)`; the unsigned
    // subtraction too (a share above 100 % makes it wrap, and the event is then due at once).
    s64 next_timer = static_cast<s64>(static_cast<float>(kBaseTickInterval) *
                                      (static_cast<float>(app_cpu_time_) / 100.0f));
    if (phase_ == Core1Phase::Sys) {
        next_timer = static_cast<s64>(kBaseTickInterval - static_cast<u64>(next_timer));
    }
    if (next_timer > cycles_late) {
        next_timer -= cycles_late;
    }

    // Azahar: `ScheduleEvent(next_timer, tick_event, 0, 1)`, the event is core 1's, whichever core the call
    // comes from (UpdateAppCpuLimit runs in a system call of a thread of either), and it is due that long after
    // core 1's own clock (the deterministic mode keeps one clock per core; elsewhere there is one).
    tick_due_ = kernel_detail::CoreTicksOr(1, kernel_.NowTicks()) + (next_timer > 0 ? static_cast<u64>(next_timer) : 0);
    kernel_detail::ScheduleOnCore(1);
    tick_event_ = kernel_.ScheduleAt(tick_due_, [this] { OnTick(); });
}

void Core1Limiter::WakeupSleepingThreads() {
    while (!sleeping_.empty()) {
        const std::weak_ptr<Thread> weak = std::move(sleeping_.front());
        sleeping_.pop_front();
        const std::shared_ptr<Thread> thread = weak.lock();
        // A thread that ended meanwhile is gone or Dead; one that is not WaitSleep was woken by
        // other means.
        if (thread != nullptr && thread->status == ThreadStatus::WaitSleep) {
            thread->ResumeFromWait(std::nullopt);
        }
    }
}

void Core1Limiter::CancelTick() {
    if (tick_event_ != 0) {
        scheduler_.Cancel(tick_event_);
        tick_event_ = 0;
    }
}

std::size_t Core1Limiter::Held() const {
    std::size_t count = 0;
    for (const auto& weak : sleeping_) {
        const auto thread = weak.lock();
        if (thread != nullptr && thread->status == ThreadStatus::WaitSleep) {
            ++count;
        }
    }
    return count;
}

}  // namespace mh4n
