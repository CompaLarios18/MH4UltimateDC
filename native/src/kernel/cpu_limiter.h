// Derived from Azahar src/core/hle/kernel/thread.h, thread.cpp, kernel.h, kernel.cpp and
// src/core/core_timing.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: thread.h,
// kernel.h and kernel.cpp say so; thread.cpp says GPL-2.0-or-later, core_timing.h GPLv2+): the
// class CpuLimiterMulti (its names, states and the 2 ms base interval), which the schedule modes
// Multi and Single share, and how the kernel starts, ends and updates it (SetCore1ScheduleMode,
// UpdateCore1AppCpuLimit).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.h, thread.cpp,
//   kernel.h, kernel.cpp)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
// PPSSPP Project (thread.h, kernel.h, second notice: Licensed under GPLv2 or any later version)
//
// The time limit of application threads on core 1 (N1-27), the part of mh4n_kernel that is not
// public: threads.h has what other modules use. See cpu_limiter.cpp for the algorithm.
#pragma once

#include <deque>
#include <memory>

#include "mh4n/clock.h"
#include "mh4n/kernel.h"
#include "mh4n/scheduler.h"
#include "mh4n/threads.h"

namespace mh4n {

// Azahar's CpuLimiterMulti. One object serves both schedule modes: Azahar keeps one instance per
// mode, but only the current mode's is ever Start()ed (a mode change ends the old one and starts
// the new one from scratch), so they never differ in anything but name.
class Core1Limiter {
public:
    // CpuLimiterMulti::base_tick_interval: 2 ms of game time, as nsToCycles(2'000'000) rounds it.
    static constexpr u64 kBaseTickInterval = NsToTicks(2'000'000);
    static_assert(kBaseTickInterval == 536223);

    Core1Limiter(Kernel& kernel, GuestScheduler& scheduler);
    ~Core1Limiter();
    Core1Limiter(const Core1Limiter&) = delete;
    Core1Limiter& operator=(const Core1Limiter&) = delete;

    // KernelSystem::SetCore1ScheduleMode: End() the limiter, take the mode, Start() it.
    void SetScheduleMode(Core1ScheduleMode mode);
    Core1ScheduleMode ScheduleMode() const { return mode_; }

    void Start();
    void End();
    // KernelSystem::UpdateCore1AppCpuLimit: reads the Application resource limit's current CpuTime
    // value and (re)starts or stops the alternation.
    void UpdateAppCpuLimit();
    // The scheduler's selection filter for core 1: true if `thread` has to wait for the other phase
    // (it is then WaitSleep, and the next phase change or the end of the limit wakes it).
    bool DoTimeLimit(Thread& thread);

    bool Ready() const { return ready_; }
    bool Active() const { return active_; }
    Core1Phase Phase() const { return phase_; }
    std::size_t Held() const;

private:
    void OnTick();
    void ChangeState(s64 cycles_late);
    void WakeupSleepingThreads();
    void CancelTick();

    Kernel& kernel_;
    GuestScheduler& scheduler_;
    bool ready_ = false;
    bool active_ = false;
    s32 app_cpu_time_ = kCore1CpuTimeDisabled;
    Core1Phase phase_ = Core1Phase::App;
    Core1ScheduleMode mode_ = Core1ScheduleMode::Multi;
    std::deque<std::weak_ptr<Thread>> sleeping_;
    u64 tick_event_ = 0;  // the pending timer-queue event, 0 if none
    u64 tick_due_ = 0;    // when it is due
};

}  // namespace mh4n
