// --perf-log FILE (N1-23, v2 since N1-32): the CPU budget's log. One CSV row per game frame, from the
// counters the scheduler keeps anyway (N1-06 and N1-32, SchedulerCounters); this log adds nothing to the
// hot path.
//
//   # perf v2 <date> game=0x<title id> cpu=native new3ds=off clock=<host|virtual> mode=<realtime|deterministic>
//   frame,present_us,main_us,guest_us,svc_us,service_us,idle_us,switches,preempts,main_svc_us,main_service_us
//   12,8333.511,2210.004,2890.113,640.220,95.300,4210.000,31,2,402.118,61.250
//   ...
//   # frames=<n> dropped=0
//
// v1 (N1-23) had the first nine columns only; v2 keeps them in their places and adds the last two.
//
// `frame` is the game's frame counter (the `f=` of --state-trace, so the two files line up). Times are
// microseconds of the scheduler's TimeSource (host time in real time). No microsecond is in more than one
// of guest_us, svc_us, service_us and idle_us (main_us, main_svc_us and main_service_us are parts of the
// first three):
//
//   present_us  from the previous row (the first row: from the start of the log, so it includes the
//               boot) to this one
//   main_us     the game's main thread in guest code, with the timer events, posted work and safe-point
//               hooks (frame subscribers, this log included) run at its preemption checks
//   guest_us    the same for every thread (main_us included)
//   svc_us      inside system calls, every thread, without the services' time
//   service_us  inside the services' handlers, every thread
//   idle_us     every virtual core idle, waiting for work or a deadline
//   switches    fiber switches in the frame; preempts: threads displaced while they could still run
//   main_svc_us      the main thread's own part of svc_us: its system calls, without its services
//   main_service_us  the main thread's own part of service_us: the service handlers it called
//
// The scheduler's host loop between guest runs (Reap, ProcessPending, RunHooks, Dispatch) is in no
// column: present_us - idle_us is at least the sum of guest_us, svc_us and service_us (over the log;
// a single row can fall short by the few microseconds of the hooks run as the system call that began
// it starts, which the previous row's present_us holds).
//
// A frame seen at a preemption check is written at the first system call after it (the counters are
// flushed there), so a row ends at the first system call after the frame change; frames seen with no
// system call between them give the first the time and the others empty rows.
//
// The scheduler counts system calls and services by number and name, and by the thread that made the
// call (N1-32): the time a thread spends blocked inside a call is nobody's, and a call's time is charged
// to the thread that made it, not to the one running when it is closed. tools/native/perf_report.py's
// `game_us` (main_us + main_svc_us + main_service_us) is still an upper bound of the game thread's time:
// it holds the scheduler's host work run at the main thread's preemption checks and system calls. Its
// `game_all_us` (main_us + svc_us + service_us) is N1-23's upper bound, which holds every thread's calls.
//
// With --deterministic the TimeSource is virtual (game ticks that move with the cycles charged, not
// with the host): the header says clock=virtual, and such a log is not a measurement of the CPU.
#pragma once

#include <string>

#include "mh4n/scheduler.h"
#include "mh4n/types.h"

namespace mh4n::perf {

// The cumulative counters a row is the difference of. Nanoseconds of the TimeSource.
struct Totals {
    u64 now_ns = 0;
    u64 main_ns = 0;
    u64 guest_ns = 0;
    u64 svc_ns = 0;
    u64 service_ns = 0;
    u64 idle_ns = 0;
    u64 switches = 0;
    u64 preemptions = 0;
    u64 main_svc_ns = 0;      // the main thread's entry of SchedulerCounters::thread_svc_ns
    u64 main_service_ns = 0;  // the main thread's entry of SchedulerCounters::thread_service_ns
};

// Reads the scheduler's counters; `main_thread_id` 0 gives main_ns, main_svc_ns and main_service_ns 0,
// and so does an id with no entry.
Totals Snapshot(const SchedulerCounters& counters, u32 main_thread_id, u64 now_ns);

// The column line, without a newline.
const char* ColumnLine();
// `# perf v2 ...` line, without a newline.
std::string HeaderLine(u64 program_id, bool virtual_clock);
// The row of `frame`: what happened between `before` and `now`. No newline.
std::string FormatRow(u32 frame, const Totals& before, const Totals& now);

}  // namespace mh4n::perf
