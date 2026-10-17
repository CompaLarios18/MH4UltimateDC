// The scheduler (N1-06): guest threads on fibers under strict priority per virtual core, the 3DS's
// preemption rules, timeouts and kernel timers on the game clock, host work applied at safe points,
// and the counters the CPU budget needs. It implements the `Scheduler` interface of kernel.h (the
// kernel objects call it) and routes the code host's budget callback (cpu.h).
//
// Model (rules from Azahar's ThreadManager, structure ours):
//  - Two virtual cores. Each has a ready queue per priority 0 (best) to 63, first in first out. A
//    thread runs until it blocks, yields (SleepThread with 0 ns), exits, or a better priority
//    becomes ready on its core. There is no time slicing between equal priorities. A thread that is
//    displaced goes back to the FRONT of its queue (Azahar: SwitchContext), one that wakes or is
//    moved to another priority to the back.
//  - One host thread runs both cores. The core that runs gets a slice of `slice_us`; at the first
//    preemption check after the slice is up, and the other core has work, the other core runs. A
//    core that becomes runnable with something better than what its core has (or on an idle core)
//    gets the host thread at the next check, without waiting for the slice: real cores run in
//    parallel.
//  - Preemption: code library code checks `*PreemptFlag()` (RC_TICK) and calls Preempt(). The flag
//    is raised from the timer thread (at the earliest timer deadline or the end of the slice), by
//    Post, and when a thread becomes ready on the other core. Preempt() clears the flag, applies
//    posted work and due timers, calls the safe-point hooks and switches threads if it should. An
//    SVC that makes a better thread ready switches before it returns to its caller: the SVC layer
//    (N1-07) brackets every SVC with BeginSvc/EndSvc, and EndSvc does what Azahar's
//    PrepareReschedule does after the call.
//  - Fibers are switched only by guest threads that call into the scheduler on the host thread that
//    runs them. The timer thread and signal handlers only raise the flag.
//
// Blocking: the kernel object that blocks a thread sets its status (Block, Sleep or the kernel's own
// arbiter and IPC paths), and the thread then calls Park(): the fiber switches away and Park
// returns what the wake-up delivered when the thread runs again. The registers are left alone: the
// SVC layer writes r0 (and r1 for the index of WaitSynchronizationN) from the returned WaitOutcome,
// as it does for every other SVC. `Wake(thread, result, index)` of kernel.h is recorded for that
// purpose.
//
// Threads: the kernel's `Thread` object carries priority and wait state; Attach() gives it a fiber
// (a GuestThread of the code host) and Start() makes it ready. Creating threads from the guest is
// N1-27's; this class takes whatever it makes.
//
// Lifetime: the scheduler owns the threads it was given until they are dead (Terminate and
// ExitCurrent are the ways to end one; Thread::Stop on its own leaves it in the queues). Timer and
// Thread objects call back into the scheduler when they are destroyed (they cancel their events),
// and it must outlive them; the Kernel must outlive the scheduler's threads. So: Shutdown(), then
// the kernel (its handle table drops the timers), then the scheduler; or leave it to process exit.
// RaiseFlag is not async-signal-safe; the timer thread calls it, a signal handler must not.
#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/cpu.h"
#include "mh4n/kernel.h"
#include "mh4n/types.h"

namespace mh4n {

// ---- what a wake-up delivers -----------------------------------------------------------------------

struct WaitOutcome {
    // The value for r0: kResultSuccess, kResultTimeout or what a wake-up hook chose. Empty after a
    // sleep or an IPC wait that leaves r0 alone.
    std::optional<Result> result;
    // WaitSynchronizationN's output (the index of the object that woke a wait on any), else -1.
    s32 index = -1;
    // False if the call was satisfied (or refused with a timeout of 0) without waiting.
    bool blocked = false;
};

// ---- the sources of asynchronous preemption --------------------------------------------------------

// Something that calls `raise` from another host thread when a deadline passes, so that running guest
// code reaches Preempt(). The deterministic mode (N1-25) replaces it with a cycle budget; tests use
// a manual one that raises when the manual time passes the deadline.
class PreemptionSource {
public:
    virtual ~PreemptionSource() = default;
    // Called once, before Arm. `raise` is safe to call from any host thread.
    virtual void Start(std::function<void()> raise) = 0;
    // No `raise` call after it returns. Idempotent.
    virtual void Stop() = 0;
    // Ask for one `raise` when the TimeSource reads `deadline_ns` (immediately if it already does);
    // replaces the previous request; nullopt cancels it.
    virtual void Arm(std::optional<s64> deadline_ns) = 0;
};

// The real one: a host thread that sleeps until the deadline. It never touches guest state.
std::unique_ptr<PreemptionSource> MakeTimerThreadPreemption(TimeSource& source);

// ---- counters --------------------------------------------------------------------------------------

// Cumulative since the scheduler was made. Times are host nanoseconds of the TimeSource (not game
// time: a paused clock does not stop them). The runner samples them once per game frame; read them on
// the scheduler's thread (a safe-point hook is the place).
struct SchedulerCounters {
    struct Named {
        u64 ns = 0;
        u64 calls = 0;
    };
    std::map<u32, u64> guest_ns;                 // thread id -> time in its guest code
    std::array<Named, 256> svc{};                // by SVC number: time inside, number of calls
    std::map<std::string, Named, std::less<>> services;  // by service name (N1-10 adds the names)
    u64 idle_ns = 0;                             // every core idle, waiting for work or a deadline
    u64 thread_switches = 0;                     // fiber switches (each costs a system call on ucontext)
    u64 preemptions = 0;                         // a running thread displaced while it could still run
    u64 core_switches = 0;                       // the host thread moved from one virtual core to the other
    u64 preempt_checks = 0;                      // calls of Preempt()
};

// ---- the scheduler ---------------------------------------------------------------------------------

enum class RunStatus {
    Stopped,  // the predicate held, or RequestStop() was called
    Idle,     // return_when_idle: nothing is runnable, no timer is pending and no work is posted
    Failed,   // the code host recorded a failure (CodeHost::Failure()): exit status 4
};

class GuestScheduler final : public Scheduler {
public:
    struct Options {
        u32 slice_us = 1000;  // the runner's --slice-us
    };
    // Plugs itself into `kernel` (SetScheduler, SetClock) and into `host` (the budget callback).
    // `preemption` may be null: then nothing raises the flag by itself, which suits a program with
    // no guest code that polls it. It must outlive the scheduler.
    GuestScheduler(Kernel& kernel, CodeHost& host, GameClock& clock, TimeSource& time,
                   PreemptionSource* preemption, Options options);
    GuestScheduler(Kernel& kernel, CodeHost& host, GameClock& clock, TimeSource& time,
                   PreemptionSource* preemption = nullptr)
        : GuestScheduler(kernel, host, clock, time, preemption, Options{}) {}
    ~GuestScheduler() override;

    // ---- the Scheduler interface of kernel.h --------------------------------------------------------
    void Wake(const std::shared_ptr<Thread>& thread, std::optional<Result> result,
              s32 index) override;
    void OnPriorityChanged(Thread& thread, u32 old_priority) override;
    void PrepareReschedule() override;
    Thread* CurrentThread() override;
    u64 NowTicks() override { return clock_.Now(); }
    // The timer queue. Events run on the scheduler thread at a safe point, in (tick, id) order. An
    // event whose tick has passed runs at the next safe point, never inside this call.
    u64 ScheduleAt(u64 tick, std::function<void()> fn) override;
    bool Cancel(u64 id) override;
    // Any host thread. The function runs on the scheduler thread at the next safe point.
    void Post(std::function<void()> fn) override;

    // ---- threads ------------------------------------------------------------------------------------
    // Gives `thread` a fiber and takes ownership of it until it is dead. With an entry the fiber runs
    // it (tests, tools; it must end the thread with ExitCurrent), else the guest function at
    // Context().r[15]. The returned GuestThread is where N1-27 puts the registers.
    GuestThread& Attach(const std::shared_ptr<Thread>& thread, GuestThread::Entry entry = {});
    // Attach with a C++ body that runs and then ends the thread.
    GuestThread& AttachFunction(const std::shared_ptr<Thread>& thread, std::function<void()> body);
    // Dormant -> Ready, at the back of its core's queue for its priority.
    void Start(const std::shared_ptr<Thread>& thread);
    // The running thread ends (svcExitThread): Thread::Stop(), then the next thread runs. Holds no
    // owning pointer on the fiber's stack at the last switch, as cpu.h requires.
    [[noreturn]] void ExitCurrent();
    // Ends another thread (svcTerminateThread, process exit): takes it off the queues, Stop(). For the
    // running thread it is ExitCurrent().
    void Terminate(const std::shared_ptr<Thread>& thread);
    // The running guest thread, as a shared pointer (nullptr outside guest code).
    std::shared_ptr<Thread> CurrentThreadPtr();
    // The thread each virtual core is on (the running one is one of them), or nullptr.
    Thread* ThreadOnCore(int core) const;
    std::size_t ReadyCount(int core) const;

    // ---- blocking (the calling thread must be the running one) ---------------------------------------
    // WaitSynchronization1/N as Azahar does it: acquires at once what is available (the first object
    // for a wait on any, all of them for a wait on all), refuses with kResultTimeout when a timeout of
    // 0 cannot be met, else waits on the objects with `timeout_ns` (-1 for ever) until a signal
    // (kResultSuccess, and the index for a wait on any) or the timeout (kResultTimeout) wakes it.
    WaitOutcome Block(Thread& thread, std::vector<std::shared_ptr<WaitObject>> objects,
                      bool wait_all, s64 timeout_ns);
    // The thread has been put in a wait state by the kernel (status not Running): switch away and
    // return what its wake-up delivered once it runs again. For the arbiter, IPC and HLE waits.
    WaitOutcome Park(Thread& thread);
    // SleepThread, for the running thread. -1 waits for ever. 0 ns, as Azahar: with no thread ready on
    // the core it returns at once; with one of its priority (or better) ready it goes to the back of
    // its queue and they run first, in order; with only lower priorities ready it waits for a zero
    // delay, so they run until the next preemption check processes the due wake-up. Other values wait
    // that long on the game clock.
    void Sleep(s64 ns);

    // ---- running ------------------------------------------------------------------------------------
    // Runs guest threads on the calling host thread until `until` holds (checked at every safe point
    // and whenever the host loop turns), RequestStop() is called, or the code host fails. `until` may
    // be empty. With return_when_idle it returns Idle instead of waiting when there is nothing to
    // wait for. May be called again to continue; threads stay where they were.
    RunStatus Run(const std::function<bool()>& until = {}, bool return_when_idle = false);
    // Makes the current or next Run return Stopped. Any thread.
    void RequestStop();
    // Applies posted work and due timers without any guest fiber, until none is left (the replay
    // tool). Returns whether anything ran. Threads that were woken stay ready.
    bool RunPending();

    // What RC_TICK does: call Preempt() if the flag is set. For code that is not recompiled (tests,
    // the interpreter).
    void PreemptPoint() {
        if (__atomic_load_n(&preempt_flag_, __ATOMIC_ACQUIRE) != 0) {
            Preempt();
        }
    }
    // The code host's budget callback. Runs on a guest fiber.
    void Preempt();
    // The flag the code library reads (CodeLibrary::Load's `preempt_flag`).
    volatile u32* PreemptFlag() { return &preempt_flag_; }
    // Any thread: makes the next preemption check call Preempt() and wakes an idle scheduler.
    void RaiseFlag();
    // The SVC layer brackets every SVC with these. BeginSvc starts the SVC's counters and calls the
    // safe-point hooks; EndSvc switches threads if a PrepareReschedule or the flag asks for it, then
    // goes back to counting guest time. A thread that blocks inside the SVC stays in BeginSvc..EndSvc.
    void BeginSvc(u32 number);
    void EndSvc();
    // The same for the time a service handler takes (N1-10): inside an SVC, the handler's time goes to
    // the service. Calls nest; a Leave restores the previous category.
    void BeginService(std::string_view name);
    void EndService();

    // Called at every preemption point, SVC and turn of the host loop, on the scheduler thread; they
    // must not switch fibers (they may RequestStop or Post). While any is registered, a running guest
    // gets a preemption check at least once per slice.
    void OnSafePoint(std::function<void()> hook);
    void ClearSafePointHooks();

    // For core 1's time limit (N1-27): called for every thread the scheduler is about to run from the
    // ready queue of `core`, before it does. Returning true means the filter kept the thread back
    // (Azahar's CpuLimiter puts it to WaitSleep and wakes it later with ResumeFromWait); the scheduler
    // then takes the next one. The filter runs inside the scheduler: it must not switch fibers.
    // A thread kept back after it was woken keeps the outcome of that wake-up (Wake does not overwrite
    // an earlier result or index with an empty one), so the wait still returns its r0 and index.
    using SelectionFilter = std::function<bool(Thread& candidate)>;
    void SetSelectionFilter(int core, SelectionFilter filter);

    u32 SliceUs() const { return slice_us_; }
    void SetSliceUs(u32 us) { slice_us_ = us; }
    const SchedulerCounters& Counters() const { return counters_; }
    GameClock& Clock() { return clock_; }

    // Cancels the timeouts of the threads it holds and lets go of them and of their fibers. The
    // destructor does it too.
    void Shutdown();

private:
    struct SchedThread;
    struct Core {
        std::array<std::deque<Thread*>, 64> ready;
        u64 mask = 0;  // bit p: ready[p] is not empty
        Thread* current = nullptr;
        SelectionFilter filter;
    };

    static SchedThread& Sd(Thread& thread);
    int CoreOf(const Thread& thread) const;
    void Enqueue(Thread& thread, bool front);
    void Dequeue(Thread& thread);
    bool Runnable(int core) const;
    bool Urgent(int core) const;
    void Normalize(Core& core);
    Thread* PopNextReady(Core& core);
    void ChooseCore(s64 now_ns);
    Thread* Dispatch();
    void SwitchFromSelf(GuestThread* self);
    void Reschedule();
    void NeedReschedule(int core);
    bool ProcessPending(bool until_quiet);
    bool RunDueTimers();
    void RunHooks();
    bool StopWanted();
    void UpdateArm();
    void IdleWait();
    void Reap();
    void AccountTo(SchedThread* to);
    void FlushCategory();

    Kernel& kernel_;
    CodeHost& host_;
    GameClock& clock_;
    TimeSource& time_;
    PreemptionSource* preemption_;
    u32 slice_us_;

    // Scheduler-thread state.
    std::array<Core, 2> cores_;
    int active_ = 0;
    bool slice_active_ = false;
    s64 slice_end_ns_ = 0;
    bool guest_running_ = false;
    bool reschedule_pending_ = false;
    std::unordered_map<Thread*, std::shared_ptr<Thread>> threads_;  // owned until reaped
    std::vector<std::shared_ptr<Thread>> graveyard_;                // dead, fibers freed later
    std::map<std::pair<u64, u64>, std::function<void()>> timers_;   // (tick, id) -> fn
    std::unordered_map<u64, u64> timer_ticks_;                      // id -> tick
    u64 next_timer_id_ = 1;
    std::vector<std::function<void()>> hooks_;
    std::function<bool()> until_;
    std::optional<s64> last_armed_;
    bool in_pending_ = false;
    SchedulerCounters counters_;
    SchedThread* acct_ = nullptr;  // the thread whose time is being counted
    s64 acct_start_ns_ = 0;

    // Shared with other host threads.
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::function<void()>> posted_;
    volatile u32 preempt_flag_ = 0;
    bool stop_requested_ = false;  // guarded by mutex_
};

// RAII forms of the brackets above.
class SvcScope {
public:
    SvcScope(GuestScheduler& scheduler, u32 number) : scheduler_(scheduler) {
        scheduler_.BeginSvc(number);
    }
    ~SvcScope() { scheduler_.EndSvc(); }
    SvcScope(const SvcScope&) = delete;
    SvcScope& operator=(const SvcScope&) = delete;

private:
    GuestScheduler& scheduler_;
};

class ServiceScope {
public:
    ServiceScope(GuestScheduler& scheduler, std::string_view name) : scheduler_(scheduler) {
        scheduler_.BeginService(name);
    }
    ~ServiceScope() { scheduler_.EndService(); }
    ServiceScope(const ServiceScope&) = delete;
    ServiceScope& operator=(const ServiceScope&) = delete;

private:
    GuestScheduler& scheduler_;
};

}  // namespace mh4n
