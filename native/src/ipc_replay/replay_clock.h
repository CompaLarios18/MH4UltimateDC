// The game clock of a replay (N1-11b): time that only the replay moves.
//
// A runtime made with the default time source runs its game clock off the host's steady clock, so a
// replay could only see game time pass as fast as the host lets it, and an hour between two recorded
// requests would take an hour. A replay runtime is made with MakeReplayRuntimeConfig() instead: the
// clock stands still until the replay sets it (ReplayTimeSource::SetNs), and the scheduler's
// request for a wake-up at its earliest timer (the PreemptionSource interface, which a runtime with a
// manual clock otherwise leaves out) is kept, so the replay knows when the next timer falls due and
// can move the clock there, one event at a time, as a runtime that never idles would reach it. Nothing
// in here reads the host's clock or waits.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "mh4n/clock.h"
#include "mh4n/kernel.h"
#include "mh4n/runtime.h"
#include "mh4n/scheduler.h"
#include "mh4n/types.h"

namespace mh4n::ipc_replay {

class RequestTimers;

class ReplayTimeSource final : public TimeSource {
public:
    ReplayTimeSource();
    ~ReplayTimeSource() override;

    s64 NowNs() override;
    // Never called by the replay (nothing runs the scheduler's idle wait); like ManualTimeSource: a
    // deadline is reached by jumping to it unless the predicate already holds, no deadline waits for
    // the predicate alone.
    bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                   std::optional<s64> deadline_ns, const std::function<bool()>& pred) override;

    // Moves the time to `ns`; a value in the past leaves it where it is (time never goes back).
    void SetNs(s64 ns);

    // The host time (NowNs terms) at which the scheduler last asked to be woken, which is its earliest
    // timer; none if it has no timer. Meaningful after GuestScheduler::RunPending, which brings it up
    // to date.
    std::optional<s64> NextDeadlineNs() const;

    // The preemption source that records the deadline for NextDeadlineNs. Give it to the same
    // RuntimeConfig as this time source (it shares this source's state, so the order in which the
    // runtime lets go of the two does not matter).
    std::unique_ptr<PreemptionSource> MakePreemption();

    // Puts a RequestTimers in `kernel` in place of the scheduler it has, once, and returns it; the next
    // calls (a second Replay on the same runtime) return the same one. It belongs to this source, which
    // the runtime owns and destroys after the kernel and the scheduler, so it lives as long as the
    // runtime does and the kernel's scheduler pointer is written only this once. Null if this source
    // already installed its timers in another kernel.
    RequestTimers* InstallTimers(Kernel& kernel);

    struct State;

private:
    std::shared_ptr<State> state_;
    std::unique_ptr<RequestTimers> timers_;
    Kernel* kernel_ = nullptr;
};

// The scheduler as the services see it (installed in the Kernel in place of the GuestScheduler, which it
// forwards every call to, by ReplayTimeSource::InstallTimers), with one addition: while a replay runs it
// remembers the timers that one request arms, so the replay knows what game time that request is waiting
// for.
//
// A request "owns" the timers it arms while its handler runs, and the timers those timers arm from
// their own callbacks, and so on (a retry chain). A timer that anything else arms (the runtime's
// periodic ones re-arming themselves, work another host thread posts) is not owned, so the host's speed
// cannot change which timers are. While a deferred reply is waited for, the replay moves the clock
// only toward the owned timers (EarliestOwnTick): a reply that waits for a host thread arms nothing
// and sees no game time pass, whatever the thread's speed.
//
// It is installed once and stays: it lives as long as the runtime, and outside a replay (tracking off)
// it is a plain pass-through. Each timer it wraps keeps the bookkeeping alive by a shared pointer, so a
// timer a replay left pending that runs after the replay (or on a later replay) just calls its function.
//
// Everything but Post is called on the scheduler's thread, as the Scheduler interface says; Post reads
// only the inner scheduler, which never changes.
class RequestTimers final : public Scheduler {
public:
    explicit RequestTimers(Scheduler& inner);

    // A replay starts or ends. On: the timers armed during a request are noted (what an earlier replay
    // noted is forgotten). Off: nothing is noted and BeginRequest, EndRequest and EarliestOwnTick do
    // nothing.
    void SetTracking(bool on);
    // A new request: the timers armed from now on, until EndRequest, are its own; those of earlier
    // requests are not any more.
    void BeginRequest();
    void EndRequest();
    // The earliest tick at which a timer of the current request is still pending.
    std::optional<u64> EarliestOwnTick() const;

    void Wake(const std::shared_ptr<Thread>& thread, std::optional<Result> result, s32 index) override {
        inner_.Wake(thread, result, index);
    }
    void OnPriorityChanged(Thread& thread, u32 old_priority) override {
        inner_.OnPriorityChanged(thread, old_priority);
    }
    void PrepareReschedule() override { inner_.PrepareReschedule(); }
    Thread* CurrentThread() override { return inner_.CurrentThread(); }
    u64 NowTicks() override { return inner_.NowTicks(); }
    u64 ScheduleAt(u64 tick, std::function<void()> fn) override;
    bool Cancel(u64 id) override;
    void Post(std::function<void()> fn) override { inner_.Post(std::move(fn)); }

private:
    struct Own {
        u64 tick;
        u64 request;
    };
    // What the wrapped timers need, shared with them so that it outlives this object if they do.
    struct Book {
        mutable std::mutex mutex;
        std::atomic<bool> tracking{false};
        std::map<u64, Own> pending;  // timer id -> its tick and the request it belongs to (guarded)
        u64 request = 0;             // the current request; 0 before the first (guarded)
        u64 arming_for = 0;          // the request whose timers are being armed now; 0 none (guarded)
    };
    static void Fire(const std::shared_ptr<Book>& book, u64 id, u64 request, const std::function<void()>& fn);

    Scheduler& inner_;
    std::shared_ptr<Book> book_;
};

// A RuntimeConfig whose time source is a ReplayTimeSource (at nanosecond 0) with its preemption
// source. Replay() with ReplayConfig::advance_time needs a runtime made with it.
RuntimeConfig MakeReplayRuntimeConfig();

}  // namespace mh4n::ipc_replay
