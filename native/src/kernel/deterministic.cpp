// Derived from Azahar src/core/hle/kernel/svc.cpp (SVC_Table: the cycles each implemented call
// costs, and CallSVC, which charges them to the running core before the call), src/core/hle/kernel/
// address_arbiter.cpp (the 1614 ticks of a Signal that wakes nobody), src/core/core_timing.h
// (MAX_SLICE_LENGTH) and src/core/core.cpp (RunLoop: the cores run one after the other in slices of
// that length) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp and core.cpp say
// "GPLv3 or any later version"; address_arbiter.cpp and core_timing.h say GPLv2 or any later version):
// the numbers and the rule, not the code. The clock, the budget accounting and the seeded slices are
// ours.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp, core.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (address_arbiter.cpp)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
//
// The deterministic mode (N1-25): see deterministic.h.
#include "deterministic.h"

#include <algorithm>

#include "mh4n/assert.h"
#include "mh4n/log.h"

// The registers of a guest thread (RCtx). Last: recomp.h defines short macros.
#include "../cpu/recomp_c.h"

namespace mh4n {

// ---- Azahar's cycle costs ---------------------------------------------------------------------------------

u32 SvcCycles(u32 number) {
    switch (number) {
    // 1000 cycles are Azahar's assumption for calls its authors could not profile.
    case 0x01: case 0x02: case 0x03: case 0x09: case 0x13: case 0x15: case 0x1A: case 0x1C:
    case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x27: case 0x29: case 0x2A:
    case 0x2C: case 0x2D: case 0x33: case 0x34: case 0x35: case 0x36: case 0x38: case 0x39:
    case 0x3A: case 0x3C: case 0x3D: case 0x47: case 0x65: case 0x76: case 0x79: case 0x7C:
    case 0x7D: case 0x90: case 0x93: case 0x94: case 0xA0: case 0xA1: case 0xB3:
        return 1000;
    case 0x08: return 5214;  // CreateThread
    case 0x0A: return 946;   // SleepThread
    case 0x0B: return 616;   // GetThreadPriority
    case 0x0C: return 1812;  // SetThreadPriority
    case 0x14: return 1324;  // ReleaseMutex
    case 0x16: return 2713;  // ReleaseSemaphore
    case 0x17: return 4329;  // CreateEvent
    case 0x18: return 3285;  // SignalEvent
    case 0x19: return 1389;  // ClearEvent
    case 0x1B: return 5163;  // SetTimer
    case 0x22: return 5664;  // ArbitrateAddress
    case 0x23: return 2937;  // CloseHandle
    case 0x24: return 4005;  // WaitSynchronization1
    case 0x25: return 6918;  // WaitSynchronizationN
    case 0x28: return 340;   // GetSystemTick
    case 0x2B: return 1510;  // GetProcessInfo
    case 0x32: return 5825;  // SendSyncRequest
    case 0x37: return 677;   // GetThreadId
    case 0x48: return 5122;  // CreateSessionToPort
    case 0x49: return 3492;  // CreateSession
    case 0x4A: return 1842;  // AcceptSession
    case 0x4F: return 8762;  // ReplyAndReceive
    case 0x52: return 9609;  // InvalidateProcessDataCache
    case 0x53: return 7174;  // StoreProcessDataCache
    case 0x54: return 9084;  // FlushProcessDataCache
    default: return 0;       // not implemented by Azahar: it does not charge them either
    }
}

// ---- VirtualTimeSource ---------------------------------------------------------------------------------------

u64 VirtualTimeSource::TicksAtOrAfterNs(s64 ns) {
    if (ns <= 0) {
        return 0;
    }
    // NowNs() is ceil(ticks * 1e9 / rate): it reaches `ns` at the first tick above (ns - 1) * rate / 1e9.
    return NsToTicks(static_cast<u64>(ns) - 1) + 1;
}

void VirtualTimeSource::AdvanceCoreToTick(int core, u64 tick) {
    std::atomic<u64>& clock = ticks_[core];
    u64 now = clock.load(std::memory_order_acquire);
    while (now < tick && !clock.compare_exchange_weak(now, tick, std::memory_order_acq_rel)) {
    }
}

bool VirtualTimeSource::WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                                  std::optional<s64> deadline_ns, const std::function<bool()>& pred) {
    if (pred()) {
        return true;
    }
    if (!deadline_ns) {
        // Nothing is scheduled: only another host thread (a live input, the app) can make something
        // happen. Say so once if it stays this way, because a run that is stuck here never ends.
        while (!pred()) {
            if (!cv.wait_for(lock, std::chrono::seconds(10), pred) && !warned_idle_) {
                warned_idle_ = true;
                MH4N_LOG(Warning, "Kernel",
                         "deterministic mode: every thread is waiting and nothing is scheduled, at tick {}; "
                         "the run will not go on by itself",
                         Ticks());
            }
        }
        return true;
    }
    if (lockstep_.enabled) {
        // Every core is idle: both clocks go there.
        const u64 tick = TicksAtOrAfterNs(*deadline_ns);
        AdvanceCoreToTick(0, tick);
        AdvanceCoreToTick(1, tick);
        return pred();
    }
    AdvanceToNs(*deadline_ns);
    return pred();
}

// ---- DeterministicPreemption ---------------------------------------------------------------------------

void DeterministicPreemption::Start(std::function<void()> raise) {
    std::lock_guard lock(mutex_);
    raise_ = std::move(raise);
}

void DeterministicPreemption::Stop() {
    std::lock_guard lock(mutex_);
    raise_ = nullptr;
    deadline_.reset();
}

void DeterministicPreemption::SetEpochSource(std::function<u64()> preempt_checks) {
    std::lock_guard lock(mutex_);
    epoch_ = std::move(preempt_checks);
}

void DeterministicPreemption::Arm(std::optional<s64> deadline_ns) {
    std::function<void()> raise;
    {
        std::lock_guard lock(mutex_);
        deadline_ = deadline_ns;
        armed_at_epoch_ = epoch_ ? epoch_() : 0;
        if (deadline_ns && time_.NowNs() >= *deadline_ns) {
            raise = raise_;  // already due: the contract is an immediate raise
        }
    }
    if (raise) {
        raise();
    }
}

std::optional<s64> DeterministicPreemption::Armed() const {
    std::lock_guard lock(mutex_);
    if (!deadline_) {
        return std::nullopt;
    }
    if (epoch_ && epoch_() != armed_at_epoch_) {
        return std::nullopt;  // a Preempt() has run since: the request is spent
    }
    return deadline_;
}

// ---- CycleDriver ---------------------------------------------------------------------------------------------------

namespace {

std::atomic<CycleDriver*> g_active_driver{nullptr};

// splitmix64: a small generator with the same output on every compiler and platform.
u64 SplitMix64(u64& state) {
    u64 z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// An event that does nothing and comes again one period after the tick it was due at (Azahar schedules
// `period - cycles_late` from the tick it ran at: the same).
void ScheduleRecurring(GuestScheduler& scheduler, u64 due, u64 period) {
    scheduler.ScheduleAt(due, [&scheduler, due, period] { ScheduleRecurring(scheduler, due + period, period); });
}

}  // namespace

CycleDriver::CycleDriver(CodeHost& host, GuestScheduler& scheduler, SvcTable& svc, VirtualTimeSource& time,
                         DeterministicPreemption& preemption, CycleDriverOptions options)
    : host_(host),
      scheduler_(scheduler),
      svc_(svc),
      time_(time),
      preemption_(preemption),
      options_(options),
      rng_state_(options.seed),
      last_slice_us_(options.slice_us) {
    preemption_.SetEpochSource([this] { return scheduler_.Counters().preempt_checks; });
}

CycleDriver::~CycleDriver() {
    CycleDriver* self = this;
    g_active_driver.compare_exchange_strong(self, nullptr);
    preemption_.SetEpochSource({});
    if (installed_) {
        host_.SetBudgetHandler({});
        host_.SetSvcHandler({});
    }
}

void CycleDriver::Install() {
    installed_ = true;
    host_.SetBudgetHandler([this](RCtx& ctx) { OnBudget(ctx); });
    host_.SetSvcHandler([this](RCtx& ctx, u32 imm, u32 next_pc) { OnSvc(ctx, imm, next_pc); });
    g_active_driver.store(this);
    scheduler_.SetSliceUs(options_.slice_us);
    // The scheduler follows Azahar's RunLoop from now on (scheduler.cpp, Dispatch).
    LockstepState& ls = time_.Lockstep();
    ls.enabled = true;
    ls.slice_cap = ls.max_slice = DrawSliceTicks();
    // Two events of the reference's timeline that nothing the game can see depends on, but each of which ends
    // a slice: the shared page's date update (at the start, then hourly) and the cheat engine's run (every
    // 50,000,000 ticks, Azahar's `run_interval_ticks`, which is there whether or not a cheat is).
    const u64 start = time_.CoreTicks(0);
    ScheduleRecurring(scheduler_, start, 268111856ull * 3600);
    ScheduleRecurring(scheduler_, start + 50000000, 50000000);
}

u64 CycleDriver::DrawSliceTicks() {
    if (options_.seeded) {
        // The last draw (DrawSlice, one at each entry into the host, as before the lockstep scheduler): whole
        // microseconds from half to one and a half times the default (the stretch that is run is capped at the
        // default by until_event when the core's event queue is empty, so the usual effective range is one half to
        // one times).
        return std::max<u64>(NsToTicks(static_cast<u64>(last_slice_us_) * 1000), 1);
    }
    // The default slice is Azahar's 1,145,777 cycles, which is not a whole number of microseconds.
    return options_.slice_us == kDeterministicSliceUs
               ? kDeterministicSliceTicks
               : std::max<u64>(NsToTicks(static_cast<u64>(options_.slice_us) * 1000), 1);
}

void CycleDriver::BeginStretch(GuestThread& thread, s64 budget) {
    thread.Context().budget = budget;
    given_[thread.Id()] = budget;
}

u32 CycleDriver::KeyOf(const RCtx& ctx) {
    if (ctx.user == nullptr) {
        return 0;
    }
    return static_cast<GuestThread*>(ctx.user)->Id();
}

u64 CycleDriver::Next() {
    return SplitMix64(rng_state_);
}

void CycleDriver::DrawSlice() {
    if (!options_.seeded) {
        return;
    }
    // From half to one and a half times the default, in whole microseconds, both ends included.
    const u64 base = std::max<u32>(options_.slice_us, 2);
    const u64 length = base / 2 + Next() % (base + 1);
    last_slice_us_ = static_cast<u32>(length);
    scheduler_.SetSliceUs(last_slice_us_);
    ++draws_;
}

void CycleDriver::Sync(RCtx& ctx) {
    s64& given = given_[KeyOf(ctx)];
    // The budget is charged at the start of a block, so the block that ran it out is already counted
    // (it runs once the thread is given a new budget, and is not charged twice).
    const s64 used = given - ctx.budget;
    if (used > 0) {
        time_.AdvanceTicks(static_cast<u64>(used));
    }
    given = ctx.budget;
}

s64 CycleDriver::NextBudget() const {
    const u64 now = time_.Ticks();
    u64 budget = kMaxQuantumTicks;
    if (const std::optional<s64> deadline = preemption_.Armed()) {
        const u64 due = VirtualTimeSource::TicksAtOrAfterNs(*deadline);
        budget = std::min<u64>(budget, due > now ? due - now : 1);
    }
    return static_cast<s64>(std::max<u64>(budget, 1));
}

void CycleDriver::Refill(RCtx& ctx) {
    const LockstepState& ls = time_.Lockstep();
    s64 budget;
    if (ls.enabled && ls.in_stretch) {
        // What is left of the stretch (Azahar's downcount after the cost of the call was added to it): it can be
        // zero or below, and the next block then yields at once, as there.
        budget = static_cast<s64>(ls.stretch_end) - static_cast<s64>(time_.CoreTicks(ls.stretch_core));
    } else {
        budget = NextBudget();
    }
    ctx.budget = budget;
    given_[KeyOf(ctx)] = budget;
}

void CycleDriver::OnBudget(RCtx& ctx) {
    Sync(ctx);
    DrawSlice();
    scheduler_.Preempt();
    Refill(ctx);
}

void CycleDriver::OnSvc(RCtx& ctx, u32 imm, u32 next_pc) {
    Sync(ctx);
    DrawSlice();
    // CallSvc charges the call's cycles (SvcCycles) once it has noted the time.
    svc_.CallSvc(ctx, imm, next_pc);
    Refill(ctx);
}

namespace kernel_detail {

void ChargeCycles(u64 ticks) {
    if (CycleDriver* driver = g_active_driver.load(std::memory_order_acquire)) {
        driver->Charge(ticks);
    }
}

u64 TraceTicks(u64 running_core_ticks) {
    if (CycleDriver* driver = g_active_driver.load(std::memory_order_acquire)) {
        return driver->GlobalTicks();
    }
    return running_core_ticks;
}

u64 CoreTicksOr(int core, u64 fallback) {
    if (CycleDriver* driver = g_active_driver.load(std::memory_order_acquire)) {
        return driver->Time().CoreTicks(core);
    }
    return fallback;
}

void ScheduleOnCore(int core) {
    if (CycleDriver* driver = g_active_driver.load(std::memory_order_acquire)) {
        driver->Time().Lockstep().schedule_core = core;
    }
}

void BeginStretch(GuestThread& thread, s64 budget) {
    if (CycleDriver* driver = g_active_driver.load(std::memory_order_acquire)) {
        driver->BeginStretch(thread, budget);
    }
}

u64 DrawSliceTicks() {
    if (CycleDriver* driver = g_active_driver.load(std::memory_order_acquire)) {
        return driver->DrawSliceTicks();
    }
    return kDeterministicSliceTicks;
}

bool ScheduleThreadSafe(int core, std::function<void()> fn) {
    CycleDriver* const driver = g_active_driver.load(std::memory_order_acquire);
    if (driver == nullptr) {
        return false;
    }
    LockstepState& ls = driver->Time().Lockstep();
    // The queues and the event table are not locked (Azahar's ts_queue is, which is what makes its flag thread-safe;
    // here the hand-over is `schedule_pending`, a plain field the next ScheduleAt reads): another host thread would
    // race on them. It has to Post the call to the scheduler instead.
    MH4N_ASSERT(ls.scheduler_thread == std::thread::id{} || ls.scheduler_thread == std::this_thread::get_id(),
                "ScheduleThreadSafe called from a host thread other than the scheduler's: Post it");
    ls.schedule_core = core;
    ls.schedule_pending = true;
    driver->Scheduler().ScheduleAt(driver->Time().CoreTicks(core) + 2 * kDeterministicSliceTicks, std::move(fn));
    return true;
}

}  // namespace kernel_detail

}  // namespace mh4n
