// The host's timer resolution while a program runs (found by N1-21). Our own code.
//
// A host thread that waits with a deadline (the scheduler when every guest thread sleeps, the
// preemption timer) is woken at the host's timer tick. On Windows that tick is 15.6 ms unless the
// process asks for a finer one, so a guest thread that sleeps 12 ms, or a VBlank that falls due while
// the scheduler is idle, is handled up to 15 ms late: the game's frames then drift off the VBlank and
// the run goes slower than real time (measured on one PC: about 1.3 VBlanks per frame instead of 1).
// TimerResolutionScope asks for 1 ms for as long as it lives, and (Windows 10 and 11) asks not to be
// power-throttled, which would make the host ignore that request for a process without a visible
// window, as every headless run is. The opt-out is made by the first scope alive and given back (the
// system manages the process again, as before) when the last one ends, so scopes nest. Other hosts
// have a fine timer already: it does nothing there.
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

    // How many scopes are alive in this process, whatever the host (a test or a log can see that a
    // run holds one).
    static int live_count();

    // The PROCESS_POWER_THROTTLING_* flags (EXECUTION_SPEED 0x1, IGNORE_TIMER_RESOLUTION 0x4) the
    // process has opted out of through the scopes alive now: 0 when none is alive, off Windows, and
    // when the host refused.
    static unsigned throttling_opt_out_flags();

private:
    [[maybe_unused]] unsigned milliseconds_;
    bool active_ = false;
};

}  // namespace mh4n::detail
