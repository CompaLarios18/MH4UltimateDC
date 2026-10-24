// The deterministic mode (N1-25): `--deterministic` and `--seed N` of the native runner.
//
// In real time the game clock follows the host and the preemption points come from a timer thread, so
// two runs interleave differently. This mode takes the host out of the clock:
//
//   - The clock is VirtualTimeSource: game ticks that move only when guest code has run (the cycles the
//     `_native_det` code library charges to RCtx::budget, one tick per cycle at 268,111,856 Hz), when a
//     system call has taken its modelled time, or when the scheduler is idle and skips to its next
//     timer. GameClock sits on top of it unchanged, so the timer queue, the services' delays and the
//     VBlank, HID and DSP events are all exact ticks of this clock. There are two of them, one per core, as
//     Azahar's two Timing::Timers are: a thread reads and moves the clock of its own core.
//   - The scheduler follows Azahar's System::RunLoop, tick for tick (GuestScheduler::Dispatch, LockstepState
//     below): core 0 runs for a stretch, then core 1 for as long as core 0 did, and they start over at the later
//     of the two clocks; a core that is ahead is waited for. A stretch ends when the budget the thread was given
//     runs out (the slice, or the next event of either core) or when a system call asks for a reschedule; the
//     events of each core run at the start of a RunLoop call, when its clock has got to them. That is what makes
//     a run equal to the reference runner's, not only repeatable: the order in which the two cores' threads
//     see each other's work, and the tick at which they do, are the reference's.
//   - CycleDriver is the code host's budget and SVC handler. It adds the cycles a thread has used to
//     its core's clock whenever the thread enters the host (an SVC or an exhausted budget), and gives
//     the thread a new budget when it runs again: what is left of its stretch. The SVC layer charges a
//     call's own cost from Azahar's table (SvcCycles) through kernel_detail::ChargeCycles, once it has
//     noted the time for its trace (the later of the two clocks, as the reference's `t`).
//   - With a seed, the cap of each stretch (RunLoop's max_slice, 1,145,774 ticks) is drawn from a seeded
//     generator between half and one and a half times the default, which moves the points where the two
//     virtual cores take turns; the same seed gives the same run, another seed another interleaving.
//
// Services that used host threads answer on the scheduler thread in this mode (services/fs: the I/O
// pool is not used), so the whole run is a function of the program, the options and the seed. A
// real-time run is not made repeatable by any of this and must not be expected to be.
//
// The classes are the kernel module's own (the public contracts are untouched); Runtime installs them
// when `--deterministic` is given.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

#include "mh4n/clock.h"
#include "mh4n/cpu.h"
#include "mh4n/scheduler.h"
#include "mh4n/svc.h"
#include "mh4n/types.h"

struct RCtx;

namespace mh4n {

// Where the reference runner starts its emulated clock (port/README.md, the determinism table:
// `init_ticks_override`), so that the game sees the same ticks and the same date.
inline constexpr u64 kDeterministicStartTicks = 2000000000;

// Azahar's MAX_SLICE_LENGTH: BASE_CLOCK_RATE_ARM11 / 234 = 1,145,774 cycles (4.2735 ms), the longest a core
// runs before RunLoop starts over.
inline constexpr u64 kDeterministicSliceTicks = 1145774;

// The same slice in whole microseconds, the unit of `--slice-us` and of the seeded draws (it is 136 ticks
// over; the lockstep scheduler uses kDeterministicSliceTicks when the option has this default).
inline constexpr u32 kDeterministicSliceUs = 4274;

// The most cycles a thread is given at once when the scheduler asks for no deadline (no timer, no hook,
// nothing on the other core): a stop request is seen at least this often.
inline constexpr u64 kMaxQuantumTicks = kTicksPerSecond / 16;

// What the kernel charges for system call `number`: the cycles of Azahar's SVC_Table (obtained from
// hardware by its authors), 0 for a call that Azahar does not implement. Azahar adds them to the
// running core's timer before the call runs.
u32 SvcCycles(u32 number);

// The extra cycles of svcArbitrateAddress with type Signal that wakes nobody, for a thread of priority
// 49 or worse (Azahar: "prevents lag from low priority threads that spam svcArbitrateAddress").
inline constexpr u32 kArbiterIdleSignalCycles = 1614;
inline constexpr u32 kArbiterIdleSignalMinPriority = 49;

// ---- the clock ----------------------------------------------------------------------------------------

// What the lockstep scheduler (GuestScheduler::Dispatch, scheduler.cpp) keeps between two calls: Azahar's
// System::RunLoop, which the deterministic mode follows tick for tick. The two cores have a clock each
// (Azahar's Timing::Timer::executed_ticks plus the progress of the slice that is running); one RunLoop
// call runs core 0 for a stretch, then core 1 for as long as core 0 ran, so that the cores take the same
// time side by side, and the next one starts at the later of the two clocks. A core that is ahead
// (a system call charged more than its slice) is waited for by the other, which then runs alone for the
// difference. The events (timer queue) belong to the core that scheduled them and run when that core's
// clock has got to them, at the start of a RunLoop call.
struct LockstepState {
    bool enabled = false;
    // What the next call of Dispatch does.
    enum class Stage { IterStart, Sync, Core0, Core1 } stage = Stage::IterStart;
    // The cap of a stretch (Azahar's MAX_SLICE_LENGTH, or the seeded draw) and what is left for the stretch that
    // comes next: core 1 gets as long as core 0 ran.
    u64 slice_cap = kDeterministicSliceTicks;
    u64 max_slice = kDeterministicSliceTicks;
    // The delayed branch: the length given to the core that is behind (SetNextSlice(delay) of its Advance).
    std::array<u64, 2> delayed_slice{0, 0};
    // The running stretch.
    bool in_stretch = false;
    int stretch_core = 0;
    u64 stretch_start = 0;
    u64 stretch_end = 0;      // the tick at which the thread's budget runs out
    Stage after_stretch = Stage::IterStart;
    // Something ran in this RunLoop call (a thread, not only idle time).
    bool ran_thread = false;
    // System::PrepareReschedule()'s `curr_core_reschedule_pending`: set by every kernel operation that asks for a
    // reschedule, cleared by the Reschedule() that follows a stretch. (The scheduler's own flag, which a system
    // call checks at its end, is the running core's `reschedule`: cleared when a stretch starts.)
    bool system_pending = false;
    // Dispatch gave the host loop a turn with no thread to run (idle time went by): Run goes round again
    // instead of waiting for time to pass, which this clock does by itself.
    bool progressed = false;
    // The timer queue by core, as Azahar's Timing::Timer keeps it. `events`: (tick, fifo, id) of the events
    // the core's Advance() runs, in order (the fifo number settles equal ticks: the order they entered the
    // queue). `pending`: what was scheduled for the core from the other one (its ts_queue), which only enters
    // the queue at the core's next Advance(): until then it neither limits a slice nor runs. The functions are
    // GuestScheduler's timers_, by (tick, id).
    struct Pending {
        u64 tick;
        u64 id;
    };
    std::array<std::set<std::tuple<u64, u64, u64>>, 2> events;
    std::array<std::deque<Pending>, 2> pending;
    std::unordered_map<u64, u64> fifo_of;  // id -> fifo, for the ids in `events`
    std::array<u64, 2> next_fifo{0, 0};
    // The core the next ScheduleAt belongs to when it is not the running one (Azahar's explicit core argument
    // of ScheduleEvent: the time limiter's tick is core 1's), the tick then counted on that core's own clock;
    // -1 for the running core.
    int schedule_core = -1;
    // The next ScheduleAt goes to the core's ts_queue (`pending`) even when it is the running core's: Azahar's
    // `thread_safe_mode` of ScheduleEvent, which the services use for what a host thread signals.
    bool schedule_pending = false;
    // How many iterations and idle stretches have gone by, for the tests.
    u64 iterations = 0;
};

// Host time that moves only when told: game ticks, shown to the rest of the runtime as nanoseconds
// (TicksToNsCeil, so that the GameClock built over a source that starts at 0 reads exactly the ticks).
//
// There are two clocks, one per core: Ticks() is the one of the core that runs, the one the game reads with
// svcGetSystemTick and every service takes its time from. Until a lockstep scheduler switches cores it is
// core 0's, so a program with one core is the single clock it always was.
class VirtualTimeSource final : public TimeSource {
public:
    explicit VirtualTimeSource(u64 start_ticks = 0) {
        ticks_[0].store(start_ticks, std::memory_order_relaxed);
        ticks_[1].store(start_ticks, std::memory_order_relaxed);
    }

    s64 NowNs() override { return static_cast<s64>(TicksToNsCeil(Ticks())); }
    // Waiting for a deadline skips the idle time (nothing else can happen before it: the work that
    // other host threads would post does not exist in this mode); without one it waits for `pred`.
    bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                   std::optional<s64> deadline_ns, const std::function<bool()>& pred) override;

    u64 Ticks() const { return ticks_[core_.load(std::memory_order_acquire)].load(std::memory_order_acquire); }
    u64 CoreTicks(int core) const { return ticks_[core].load(std::memory_order_acquire); }
    // Azahar's GetGlobalTicks: the later of the two. What its traces call `t`.
    u64 GlobalTicks() const { return std::max(CoreTicks(0), CoreTicks(1)); }
    int Core() const { return core_.load(std::memory_order_acquire); }
    // The core whose clock Ticks() reads and AdvanceTicks() moves.
    void SetCore(int core) { core_.store(core, std::memory_order_release); }
    // Both clocks start here (the reference's starting tick).
    void SetStartTicks(u64 ticks) {
        ticks_[0].store(ticks, std::memory_order_release);
        ticks_[1].store(ticks, std::memory_order_release);
    }
    void AdvanceTicks(u64 ticks) { ticks_[Core()].fetch_add(ticks, std::memory_order_acq_rel); }
    void AdvanceCoreTicks(int core, u64 ticks) { ticks_[core].fetch_add(ticks, std::memory_order_acq_rel); }
    // Moves to `tick` unless the clock is already there or past it.
    void AdvanceToTick(u64 tick) { AdvanceCoreToTick(Core(), tick); }
    void AdvanceCoreToTick(int core, u64 tick);
    void AdvanceToNs(s64 ns) { AdvanceToTick(TicksAtOrAfterNs(ns)); }

    // The first tick at which NowNs() reads `ns` or more.
    static u64 TicksAtOrAfterNs(s64 ns);

    LockstepState& Lockstep() { return lockstep_; }

private:
    std::array<std::atomic<u64>, 2> ticks_;
    std::atomic<int> core_{0};
    bool warned_idle_ = false;
    LockstepState lockstep_;
};

// ---- the preemption source ------------------------------------------------------------------------------

// Remembers the scheduler's deadline for CycleDriver (above). An Arm with a deadline that has passed
// calls `raise` at once, as the timer thread's contract says.
class DeterministicPreemption final : public PreemptionSource {
public:
    explicit DeterministicPreemption(VirtualTimeSource& time) : time_(time) {}

    void Start(std::function<void()> raise) override;
    void Stop() override;
    void Arm(std::optional<s64> deadline_ns) override;

    // `preempt_checks` returns GuestScheduler's count of Preempt() calls. A deadline stays open until a
    // Preempt() has run since it was armed (the scheduler forgets it then, and so does this): without
    // that, a deadline that was met would make every later block ask for a check.
    void SetEpochSource(std::function<u64()> preempt_checks);
    // The deadline the scheduler has asked for and no check has served yet.
    std::optional<s64> Armed() const;

private:
    VirtualTimeSource& time_;
    mutable std::mutex mutex_;
    std::function<void()> raise_;
    std::function<u64()> epoch_;
    std::optional<s64> deadline_;
    u64 armed_at_epoch_ = 0;
};

// ---- the cycle driver ---------------------------------------------------------------------------------------

struct CycleDriverOptions {
    bool seeded = false;
    u64 seed = 0;
    // The default slice, in microseconds; a seeded run draws each slice from half to one and a half
    // times this.
    u32 slice_us = kDeterministicSliceUs;
};

class CycleDriver {
public:
    // All the objects must outlive the driver. Nothing changes until Install().
    CycleDriver(CodeHost& host, GuestScheduler& scheduler, SvcTable& svc, VirtualTimeSource& time,
                DeterministicPreemption& preemption, CycleDriverOptions options);
    ~CycleDriver();
    CycleDriver(const CycleDriver&) = delete;
    CycleDriver& operator=(const CycleDriver&) = delete;

    // Becomes the code host's budget and SVC handler (SvcTable::CallSvc is still what runs the call),
    // and the target of kernel_detail::ChargeCycles.
    void Install();

    // Adds cycles to the clock (what a kernel operation costs beyond the SVC that called it).
    void Charge(u64 ticks) { time_.AdvanceTicks(ticks); }

    u64 Ticks() const { return time_.Ticks(); }
    u64 GlobalTicks() const { return time_.GlobalTicks(); }
    VirtualTimeSource& Time() { return time_; }
    GuestScheduler& Scheduler() { return scheduler_; }
    // How many slice lengths a seeded run has drawn (0 without a seed).
    u64 SliceDraws() const { return draws_; }
    // The slice length the last draw gave, in microseconds (the default without a seed).
    u32 LastSliceUs() const { return last_slice_us_; }

    // The lockstep scheduler starts a stretch of `thread` (RunLoop's ExecuteCore): its budget is the stretch's
    // length, as Azahar's ARM_Recomp::Execute(downcount) gives it.
    void BeginStretch(GuestThread& thread, s64 budget);
    // The length of the stretches of the next RunLoop call (the cap of its max_slice), in ticks: Azahar's
    // MAX_SLICE_LENGTH, or a draw from the seeded generator.
    u64 DrawSliceTicks();

private:
    void OnBudget(RCtx& ctx);
    void OnSvc(RCtx& ctx, u32 imm, u32 next_pc);
    // Adds the cycles the thread used since it was given its budget.
    void Sync(RCtx& ctx);
    // Gives the thread its budget: for the time until the scheduler's next deadline, or (with the lockstep
    // scheduler) until the end of its stretch.
    void Refill(RCtx& ctx);
    s64 NextBudget() const;
    void DrawSlice();
    u64 Next();
    static u32 KeyOf(const RCtx& ctx);

    CodeHost& host_;
    GuestScheduler& scheduler_;
    SvcTable& svc_;
    VirtualTimeSource& time_;
    DeterministicPreemption& preemption_;
    CycleDriverOptions options_;
    bool installed_ = false;
    // The budget each guest thread (by id; ids are never reused) was last given or left with.
    std::unordered_map<u32, s64> given_;
    u64 rng_state_;
    u64 draws_ = 0;
    u32 last_slice_us_;
};

namespace kernel_detail {
// Kernel operations that Azahar charges to the running core's timer on top of their SVC call this; it
// does nothing unless a CycleDriver is installed.
void ChargeCycles(u64 ticks);

// The lockstep state of a scheduler's clock: nullptr unless it is a VirtualTimeSource with a CycleDriver
// installed (the deterministic mode).
inline LockstepState* LockstepOf(TimeSource& time) {
    auto* virtual_time = dynamic_cast<VirtualTimeSource*>(&time);
    return virtual_time != nullptr && virtual_time->Lockstep().enabled ? &virtual_time->Lockstep() : nullptr;
}

// What the reference's traces call `t`: the later of the two cores' clocks in the deterministic mode,
// `running_core_ticks` (the reading of the core that runs) otherwise.
u64 TraceTicks(u64 running_core_ticks);
// The clock of core `core` when a deterministic CycleDriver is installed, `fallback` otherwise.
u64 CoreTicksOr(int core, u64 fallback);
// The next event the scheduler is asked to schedule belongs to core `core`, not to the running one
// (Azahar's core argument of Timing::ScheduleEvent). A no-op without the deterministic mode.
void ScheduleOnCore(int core);
// A thread's stretch starts (the lockstep scheduler): see CycleDriver::BeginStretch.
void BeginStretch(GuestThread& thread, s64 budget);
// The seeded cap of the next stretches (CycleDriver::DrawSliceTicks); the default one without a driver.
u64 DrawSliceTicks();
// Azahar's `ScheduleEvent(0, event, 0, core, thread_safe_mode = true)`: `fn` runs on core `core` in two slices'
// time (MAX_SLICE_LENGTH * 2 on that core's clock), and gets in the core's queue at its next Advance. Returns
// false, doing nothing, outside the deterministic mode.
bool ScheduleThreadSafe(int core, std::function<void()> fn);
}  // namespace kernel_detail

}  // namespace mh4n
