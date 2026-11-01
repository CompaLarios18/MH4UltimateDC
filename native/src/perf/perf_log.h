// --perf-log FILE (N1-23): the CPU budget's log. One CSV row per game frame, from the counters the
// scheduler keeps anyway (N1-06, SchedulerCounters); nothing is added to the hot path.
//
//   # perf v1 <date> game=0x<title id> cpu=native new3ds=off clock=<host|virtual> mode=<realtime|deterministic>
//   frame,present_us,main_us,guest_us,svc_us,service_us,idle_us,switches,preempts
//   12,8333.511,2210.004,2890.113,640.220,95.300,4210.000,31,2
//   ...
//   # frames=<n> dropped=0
//
// `frame` is the game's frame counter (the `f=` of --state-trace, so the two files line up). Times are
// microseconds of the scheduler's TimeSource (host time in real time), no microsecond counted twice:
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
//
// The scheduler's host loop between guest runs (Reap, ProcessPending, RunHooks, Dispatch) is in no
// column: present_us - idle_us is at least the sum of guest_us, svc_us and service_us (over the log;
// per row, up to the few microseconds of the hooks run as the system call that ends the row starts).
//
// A frame seen at a preemption check is written at the first system call after it (the counters are
// flushed there), so a row ends at the first system call after the frame change; frames seen with no
// system call between them give the first the time and the others empty rows.
//
// The scheduler counts system calls and services by number and name, not by thread, so the game
// thread's time in them cannot be told from the other threads': tools/native/perf_report.py's
// `game_us` (main_us + svc_us + service_us) is an upper bound of it.
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
};

// Reads the scheduler's counters; `main_thread_id` 0 gives main_ns 0.
Totals Snapshot(const SchedulerCounters& counters, u32 main_thread_id, u64 now_ns);

// The column line, without a newline.
const char* ColumnLine();
// `# perf v1 ...` line, without a newline.
std::string HeaderLine(u64 program_id, bool virtual_clock);
// The row of `frame`: what happened between `before` and `now`. No newline.
std::string FormatRow(u32 frame, const Totals& before, const Totals& now);

}  // namespace mh4n::perf
