# The IPC replay (N1-11)

`mh4n_ipc_replay` takes the IPC trace of a run (`--trace-ipc FILE --trace-ipc-full` of either
runner, the format of [`tools/native/tracefmt.py`](../../tools/native/tracefmt.py) and
`tools/compare_ipc_traces.py`) and sends its requests, in the order they were made, to the native
services in a runtime that runs no guest code. Each reply is compared with the recorded one. A
service can be built and checked against a golden session's traffic without running the game.

The logic is the module `native/src/ipc_replay/` (`mh4n_ipc_replay`); the program in
`native/tools/ipc_replay/` is only `main`. The tests are `native/tests/ipc_replay_test.cpp`: every
trace in them is written by the test, against a fake service the test registers.

```
mh4n_ipc_replay TRACE [--svc-trace FILE] [--service NAME]... [--continue] [--game DIR]
                      [--user DIR] [--time-dependent FILE]... [--reply-timeout MS] [--no-time]
```

| Option | Meaning |
|---|---|
| `TRACE` | an IPC trace with `data=` fields (`--trace-ipc-full`); the bytes of the input buffers come from them |
| `--svc-trace FILE` | the SVC trace (`--trace-svc`) of the same run: the objects the requests carry are made from its creating calls |
| `--service NAME` | replay only this service, and the sessions it hands out (`NAME/...`, as `fs:USER/file#3`). Repeatable. Default: every service registered natively; the others are counted as skipped |
| `--continue` | report every difference. Without it a service stops being replayed after its first |
| `--game DIR` | the game folder the services are given (`ServiceContext::GameFolder`) |
| `--user DIR` | the user directory the services are given |
| `--time-dependent FILE` | a list of commands compared by result only (below). Repeatable |
| `--reply-timeout MS` | how long to wait for a deferred reply (default 30000) |
| `--no-time` | do not advance game time between requests (below). Default: advance |
| `--help` | print the usage and exit 0 |

`config/native/ipc_time_dependent.txt` is read first when it exists relative to the current
directory (run the tool from the repository root).

Exit status: 0 nothing differs; 1 something does; 2 the tool could not do its job: a usage error, a
trace that is missing, empty, damaged or of the wrong kind, a `--service` nobody registered, a
`--time-dependent` file that cannot be read (a directory, or not UTF-8 text, included), a `--game` that
is not a directory or cannot be examined (a symbolic link loop, a directory it may not enter), or a run
in which no request was replayed (so a wrong `--service`, or a trace of other services, never reads as a
pass).

## How a request is replayed

The runtime is `Runtime::Prepare` with the `devices` and `services` phases: guest memory, the kernel,
a process, the registered devices and services, no program and no code library. The requests go out
on one scratch thread of that process, one at a time, with the IPC layer's reply delay switched off;
a deferred reply is waited for in real time by draining the scheduler's posted work
(`GuestScheduler::RunPending`). A request that gets no reply within `--reply-timeout` is a
difference; its thread is ended and the next request has a new one. Between requests game time
advances to the recorded ticks (next section).

- **Requests in trace order**, which is the order `seq` numbers them (the order they were sent), not
  the order of the lines.
- **The command buffer** gets the recorded words. The buffers a request carries are written at the
  addresses the trace recorded (the pages are mapped for them), from the `data=` bytes; a buffer
  with none (a write-only mapped one) is zeros. The thread-local storage and one receive area for the
  replies' static buffers are mapped where no recorded buffer lies.
- **Sessions.** A session named after a port or a service (`fs:USER`) is connected the first time it
  is used: `svcConnectToPort` for a named port (`srv:`), the service manager's GetServiceHandle
  otherwise. A session a reply handed out is known by the name the trace gave it (`fs:USER/file#3`) and,
  if the native session has a port name, by that; when the same name is handed out again the newest
  session answers to it. A session that no reply handed out and that has no service name
  (`unnamed#k`) cannot be reached: its requests are skipped.
- **Handles in requests.** `0` and the pseudo handles stay. A handle a reply introduced is the native
  handle that reply gave (the map from reference handles to native ones is built as replies are
  compared, and a handle a request moves or the SVC trace closes is forgotten). A handle the SVC trace
  shows was created by SVC 0x13 (mutex, made unlocked), 0x15 (semaphore), 0x17 (event), 0x1A (timer),
  0x1E (memory block, from the BASE region at an address the kernel picks) or 0x21 (address arbiter) is a
  new object of that kind, one per creating call. The two traces are lined up as `tracefmt.py` lines
  them up (the n-th SendSyncRequest of a thread is its n-th request, not counting the ones that failed
  with `D8E007F7`). That result is what `svcSendSyncRequest` gives when its session handle is not a client
  session, and neither runtime writes an IPC record for such a call. In the native runtime a request
  whose command holds a handle the client does not have does get a record (`cmd=-`, `in=-`), and fails
  with `D9001BF7` instead, so it is counted. A call that fails before a request exists some other way
  (the server side of the session is closed, `C920181A`) has no record in either runtime and is counted
  too: the lining-up is wrong from that call on for that thread. The object is the one the last creating
  call before that SendSyncRequest made, in the SVC trace's file order. A CloseHandle in the SVC trace
  closes the native handle after the request that most recently precedes it there. Any other handle
  skips the request.
- **Skipped requests** are listed with a reason: no recorded request or reply (`cmd=-`, `out=-`), a
  handle that cannot be made, a service not registered natively (default mode), a session that
  cannot be reached, "after the first difference".

## Game time

A service whose answer depends on a timer (the software keyboard's update tick of APT, 4,468,352
ticks after `PrepareToStartLibraryApplet`; the reply latency of a file read; the HID update) needs the
game clock to move between requests, as it did in the recording. The IPC records carry the recorded
game time: `t=`, the ticks since the run started at which the recorder wrote the reply. By default
the replay moves game time to each record's tick before it sends that request:

- **The origin is the first record's tick** (the first in replay order, that is by `seq`). The first
  request is sent at the clock's reading when the replay starts (0), and a record whose tick is `n`
  ticks after the first is sent `n` ticks after that. Absolute ticks do not matter, only the distances.
- **Only forward.** `t=` is the tick of the reply, and a deferred reply is written after later requests
  of other threads, so ticks need not grow in `seq` order. A record whose tick is not beyond the
  clock's target so far does not move the clock; it is never moved back.
- **The timers run on the way, in order.** The clock is moved to each timer that falls due before
  the target, one at a time, in (tick, id) order, and the timer and the posted work run on the
  scheduler thread (`GuestScheduler::RunPending`) with the clock at the timer's own tick, as in the
  runtime; then the clock is set to the target tick. A timer due exactly at the target has run when
  the request is sent; one a tick later has not.
- **Nothing sleeps and nothing reads the host's clock.** The runtime of the tool is made with
  `MakeReplayRuntimeConfig()` (`replay_clock.h`): a time source that stands still until the replay
  sets it, and a preemption source that only keeps the scheduler's request to be woken at its
  earliest timer, which is how the replay finds the next timer. A gap of hours between two requests
  costs the events inside it (periodic timers such as the VBlank), not the time. A trace whose last
  tick is more than 24 hours of game time after its first (`ReplayConfig::max_span_ticks`) is refused
  (exit 2, with the line number): it is damaged, and its days of periodic timers would never end.
- **Skipped requests count.** Every record moves the clock, also one that is not replayed (a service
  not registered natively, a request with no reply), so that a timer of one service falls due on
  schedule while another's requests are skipped.

`--no-time` keeps the behaviour of N1-11: no request moves game time, the scheduler is run only while
a deferred reply is waited for, and the clock is the host's (`SteadyTimeSource`). A service that sets
a timer is then never seen to fire between requests. The comparison is the same in both modes: a
reply that differs is a difference, and nothing is tolerated because time passed.

**A deferred reply.** The wait stays as it was: the scheduler's posted work is run every millisecond
of real time, up to `--reply-timeout`, which is how a reply that an I/O thread completes arrives. With the
replay clock one thing is added, because a reply that waits for a timer its own request armed (a file
read's latency, the UDS delays of up to 5 s) would otherwise wait for ever on a clock that does not
move. The replay remembers the timers the request armed: those its handler armed while it ran, and those that such a
timer arms from its own callback (a chain of retries). While one of them is pending, the clock goes on
toward the earliest of them, timer by timer (the runtime's own periodic timers run on the way, each in
order with the clock at its tick, no sleep in between), until the reply is delivered. Timers that
anything else arms (the runtime's periodic ones re-arming themselves, work another host thread posts) are
not the request's, so the host's speed cannot change which timers count. Two consequences:

- A reply that waits only for a host thread (an I/O pool, the `--io-latency none` file read) arms no
  timer, so no game time passes however long the thread takes (a millisecond or a second on a cold
  disk): the clock after the wait, and every later reading of it, is the same on every run.
- A reply that waits for a timer arrives at that timer's tick, and the clock ends the wait exactly
  there (the file read with its latency, which also waits for an I/O thread, ends at the latency's
  tick, whether the thread is faster or slower than the clock's walk to it).

The clock moves at most 60 s of game time for one reply (a hung service must not run it away): a request
whose own timers lie beyond that is stepped up to the 60 s, no farther, and is then waited for in real
time as before; at `--reply-timeout` it is a difference ("no reply within N ms") with the clock where
the wait left it, up to 60 s ahead of the recording. One poll of real time (1 ms) goes before the first
step of a wait that has a timer of its own, so that an I/O thread that is about done delivers first.

Limits. `t=` is the tick of the reply, not of the request: the replay sends a request at its reply's
tick, so a timer a request sets falls due, in replay time, as long after the recording's as the request
took to answer (the IPC delay, 39 us = about 10,500 ticks, or the delay of a deferred reply). Two requests
within that distance of a timer's tick can see it on different sides. A request that arms a timer of its
own that its reply does not wait for (a handler that also starts a periodic timer) and then defers to a
host thread has the clock walk toward that timer while the thread works, so for that one request the
clock after the wait can depend on the thread's speed (the 1 ms poll above makes it rare, not
impossible); no service of this repository does both. Work that a host thread posts is not the request's
either: a timer it arms is not stepped toward, and a reply that waits for it ends as a "no reply within N
ms" difference, never as a clock that depends on the host.

**A reply that waits for a timer the request did not arm is not delivered.** The clock moves during a
wait only toward the request's own timers (above), so a reply that waits for a periodic timer of the
runtime ends as "no reply within N ms" after the whole `--reply-timeout` (30 s by default), with the
clock where the request left it; the timer runs at the next record's tick, with nobody waiting for it.
In this repository that is `gsp::Gpu` `SaveVramSysArea` (0x19) and `RestoreVramSysArea` (0x1A): the
request is put on the slot's VBlank waiters and answered by the next PDC1 VBlank, which `GspState::Start()`
armed when the service was made, not the request. The same holds for any command that defers to a
timer armed elsewhere. This is deliberate: stepping to any timer would make the clock after a wait
depend on how fast a host thread is. It is a difference with the first version of N1-11b, which stepped
to any timer after a poll of real time and so delivered these replies; a trace that has them reports
them as differences (use `--reply-timeout` with a small value to keep the run short, or `--no-time`).
There is no way to tell, from the scheduler alone, a reply that waits for a periodic timer from one
that waits for a host thread; a service would have to say so, which needs a change to a shared header
and is not part of this tool.

The replay clock's scheduler (`RequestTimers`) is installed in the kernel once, by the first replay on a
runtime, and belongs to the clock, so it lives as long as the runtime. With no replay running it only
forwards, and a timer a replay left pending (one beyond the 60 s cap, a retry chain) runs later as plain
work, also on a second replay on the same runtime.

## What is compared

For a request with a recorded reply:

1. the reply's **header word**;
2. the **result** (the first normal parameter) and the **other normal words**, which are reported by
   their number, never by value;
3. the **descriptors** the IPC trace lists for a reply (`outdesc`): their kind (`copy`, `move`,
   `pid`, `static`, the request's writable mapped buffers), index and size; the **CRC-32** of every
   buffer (a recorded `-` is not compared); for a handle descriptor, whether each handle is null or
   not, that it is in the client's table, and, when the trace names it as `<service>/<kind>#k`, that the
   native object is of that kind (`file`, `dir`, `session`, `event`, ...).

**Time-dependent commands** (`config/native/ipc_time_dependent.txt` and the files given with
`--time-dependent`) are compared by result only. A line is `<session> <header>   # reason`: the
session name as the trace writes it (`fs:USER`; `fs:USER/file` for every file session), the request's
header word in hex. `#` starts a comment. It has the shape of the file `tools/compare_ipc_traces.py --ignore`
reads, but the reader is this tool's own: it demands strict UTF-8 as Python's decoder does (no overlong
forms, no surrogates, nothing above U+10FFFF), splits a line at ASCII white space only (Python's `split`
also takes Unicode white space), reads the header as hex digits with an optional `0x` of at most 32 bits
(Python's `int(x, 16)` also takes a sign, underscores and any size), and does not end a line at a lone
CR. A list in the form above is read by both. A file with a line that does not parse, that
is not UTF-8 text or that is a directory is refused, naming the file and the line.

## Output

The report (standard output) has only the service, sequence number, session name, header, result
codes, descriptor kind, index, size and CRC, the position of a handle in its descriptor, the kind of an
object (`file`, `event`, ...), counts, and fixed phrases; never the words of a reply that differ, the
bytes of a buffer, or any other text of the trace. The error messages of the tool (standard error) name
the line of a trace, never its text. That holds because the reader checks what the report will print:
a session name must be `<service>`, `<service>/<kind>#<k>` or
`unnamed#<k>` (a service is letters, digits and `:_.$-`, a kind lowercase letters, digits and `_-`; at
most 64 characters); a descriptor must be of one of the kinds `static`, `mapped-r`, `mapped-w`,
`mapped-rw`, `mapped-0` (a mapped buffer with neither permission), `pxi`, `copy`, `move`, `pid`; a
buffer's token must be `-` or `0x` and eight hex digits, a handle descriptor's the names of its handles
joined by `+` (each `-`, `0` or a session name), and the token of `pxi` and `pid` must be `-`. A trace
with anything else in those places is refused with its line number and without the text.

```
DIFF fs:USER seq=12 session=fs:USER header=0x00080142
  result: expected 0x00000000, got 0xc8804464
  descriptor 0 (static, index 2): crc expected 0x12345678, got 0x9abcdef0
replayed 120 requests: 119 matched, 1 differ, 4 skipped
  fs:USER: 120 replayed, 1 differ
  skipped 3 x cfg:u: service not registered natively
```

The report is not all that is printed. The services' own log lines go to standard error as in the
runner, unfiltered: they can hold what the replayed requests carry (a service name that
`GetServiceHandle` asks for today, the paths of a file service once there is one). Do not paste them
either.

Traces hold game data (file names, save bytes). Keep them under `build/` and do not paste lines from
them.

## Notes for the services

- A service that needs the **game folder** is given the one in `--game`. `Runtime::Prepare` sets no game
  folder in the runtime (only `Runtime::Boot` does), so with `--game` the tool installs the registered
  services in a second `ServiceManager` that has it and points the SVC layer at that one.
- A GetServiceHandle for a service nobody registered, with the wait flag clear, waits for the service
  (as Azahar's): in the replay it ends as "no reply within N ms". Such a request is a difference
  whenever the reference had the service and the native runtime does not yet.
- Replies that arrive later are written when the posted work runs, so a service that completes from an
  I/O thread works as it does in the runtime. Services that read the game clock see replay time (the
  recorded distances from the first request) by default, and the host's time with `--no-time`.
- A service that schedules work with `Kernel::ScheduleAt` or `Kernel::Post` gets it run between requests
  (default). A timer that re-arms itself runs once per period inside a gap, so a long gap costs its
  period count.
- A program that calls `Replay` itself (the tests do) makes its runtime with `MakeReplayRuntimeConfig()`
  to advance time; with any other runtime `Replay` reports an error, unless `ReplayConfig::advance_time`
  is false.
