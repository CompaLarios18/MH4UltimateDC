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
                      [--user DIR] [--time-dependent FILE]... [--reply-timeout MS]
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
difference; its thread is ended and the next request has a new one.

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
  I/O thread works as it does in the runtime. Services that read the game clock see the host's time.
