// The host's timer resolution while a program runs (found by N1-21). Our own code.
//
// A host thread that waits with a deadline (the scheduler when every guest thread sleeps, the
// preemption timer) is woken at the host's timer tick. On Windows that tick is 15.6 ms unless the
// process asks for a finer one, so a guest thread that sleeps 12 ms, or a VBlank that falls due while
// the scheduler is idle, is handled up to 15 ms late: the game's frames then last between 0.2 and 1.8
// VBlank periods instead of one, and it runs at three quarters of its speed. TimerResolutionScope asks
// for 1 ms for as long as it lives, and (Windows 11) asks not to be power-throttled, which would make
// the host ignore that request for a process without a visible window, as every headless run is.
// Other hosts have a fine timer already: it does nothing there.
#pragma once

namespace mh4n::detail {

class TimerResolutionScope {
public:
    // Asks the host for a timer tick of `milliseconds` (1 by default).
    explicit TimerResolutionScope(unsigned milliseconds = 1);
    ~TimerResolutionScope();
    TimerResolutionScope(const TimerResolutionScope&) = delete;
    TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;

    // True when the host accepted the request (always false off Windows, where there is nothing to ask).
    bool active() const { return active_; }

private:
    [[maybe_unused]] unsigned milliseconds_;
    bool active_ = false;
};

}  // namespace mh4n::detail
