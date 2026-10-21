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

#include <memory>
#include <mutex>
#include <optional>

#include "mh4n/clock.h"
#include "mh4n/runtime.h"
#include "mh4n/scheduler.h"
#include "mh4n/types.h"

namespace mh4n::ipc_replay {

class ReplayTimeSource final : public TimeSource {
public:
    ReplayTimeSource();

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

    struct State;

private:
    std::shared_ptr<State> state_;
};

// A RuntimeConfig whose time source is a ReplayTimeSource (at nanosecond 0) with its preemption
// source. Replay() with ReplayConfig::advance_time needs a runtime made with it.
RuntimeConfig MakeReplayRuntimeConfig();

}  // namespace mh4n::ipc_replay
