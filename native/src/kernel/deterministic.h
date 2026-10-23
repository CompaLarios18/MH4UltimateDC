// The deterministic mode (N1-25): `--deterministic` and `--seed N` of the native runner.
//
// In real time the game clock follows the host and the preemption points come from a timer thread, so
// two runs interleave differently. This mode takes the host out of the clock:
//
//   - The clock is VirtualTimeSource: game ticks that move only when guest code has run (the cycles the
//     `_native_det` code library charges to RCtx::budget, one tick per cycle at 268,111,856 Hz), when a
//     system call has taken its modelled time, or when the scheduler is idle and skips to its next
//     timer. GameClock sits on top of it unchanged, so the timer queue, the services' delays and the
//     VBlank, HID and DSP events are all exact ticks of this clock.
//   - The preemption source is DeterministicPreemption: it does not raise anything from another thread.
//     It remembers the earliest deadline the scheduler asked for (the next timer, the end of the slice),
//     and CycleDriver turns it into the budget of the running thread, so that guest code reaches
//     GuestScheduler::Preempt() when the clock reaches the deadline (or the block that crosses it ends:
//     the budget is charged at the start of each block, as in the paged build).
//   - CycleDriver is the code host's budget and SVC handler. It adds the cycles a thread has used to
//     the clock whenever the thread enters the host (an SVC or an exhausted budget), and gives the
//     thread a new budget when it runs again. The SVC layer charges a call's own cost from Azahar's
//     table (SvcCycles) through kernel_detail::ChargeCycles, once it has noted the time for its trace.
//   - With a seed, the length of each scheduling slice is drawn from a seeded generator between half
//     and one and a half times the default, which moves the points where the two virtual cores take
//     turns; the same seed gives the same run, another seed another interleaving.
//
// Services that used host threads answer on the scheduler thread in this mode (services/fs: the I/O
// pool is not used), so the whole run is a function of the program, the options and the seed. A
// real-time run is not made repeatable by any of this and must not be expected to be.
//
// The classes are the kernel module's own (the public contracts are untouched); Runtime installs them
// when `--deterministic` is given.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
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

// The scheduling slice when both virtual cores have work, in microseconds: Azahar's MAX_SLICE_LENGTH,
// BASE_CLOCK_RATE_ARM11 / 234 = 1,145,774 cycles (4.2735 ms), which RunLoop gives each core in turn.
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

// Host time that moves only when told: game ticks, shown to the rest of the runtime as nanoseconds
// (TicksToNsCeil, so that the GameClock built over a source that starts at 0 reads exactly the ticks).
class VirtualTimeSource final : public TimeSource {
public:
    explicit VirtualTimeSource(u64 start_ticks = 0) : ticks_(start_ticks) {}

    s64 NowNs() override { return static_cast<s64>(TicksToNsCeil(Ticks())); }
    // Waiting for a deadline skips the idle time (nothing else can happen before it: the work that
    // other host threads would post does not exist in this mode); without one it waits for `pred`.
    bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                   std::optional<s64> deadline_ns, const std::function<bool()>& pred) override;

    u64 Ticks() const { return ticks_.load(std::memory_order_acquire); }
    void AdvanceTicks(u64 ticks) { ticks_.fetch_add(ticks, std::memory_order_acq_rel); }
    // Moves to `tick` unless the clock is already there or past it.
    void AdvanceToTick(u64 tick);
    void AdvanceToNs(s64 ns) { AdvanceToTick(TicksAtOrAfterNs(ns)); }

    // The first tick at which NowNs() reads `ns` or more.
    static u64 TicksAtOrAfterNs(s64 ns);

private:
    std::atomic<u64> ticks_;
    bool warned_idle_ = false;
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
    // How many slice lengths a seeded run has drawn (0 without a seed).
    u64 SliceDraws() const { return draws_; }
    // The slice length the last draw gave, in microseconds (the default without a seed).
    u32 LastSliceUs() const { return last_slice_us_; }

private:
    void OnBudget(RCtx& ctx);
    void OnSvc(RCtx& ctx, u32 imm, u32 next_pc);
    // Adds the cycles the thread used since it was given its budget.
    void Sync(RCtx& ctx);
    // Gives the thread its budget for the time until the scheduler's next deadline.
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
}  // namespace kernel_detail

}  // namespace mh4n
