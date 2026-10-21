# The native runner and the runtime (N1-09)

`mh4u_native_runner` runs a program on the native modules, without Azahar. The logic is the
module `native/src/runtime/` (`mh4n_runtime`, the class `mh4n::Runtime` of
`native/include/mh4n/runtime.h`); the program in `native/runner/` is only `main`: the command
line, `--help`, `--version`, `--print-process` and the printing of the program's debug strings.
Its options are those of `mh4u_runner` (the reference, `port/README.md`) wherever the meaning is
the same.

```
mh4u_native_runner PROGRAM --code LIB [options]
```

`PROGRAM` is a game folder (`tools/native/prepare_game.py` writes it from the player's own dump) or
a 3DS ELF test program (`port/tests/`, built by `tests/test_conformance.py`). `LIB` is the
recompiled code library of that program, built with `--native` (`python3 -m recomp.build
--code-library --native`); the runner refuses a library that was not made from the program's text
segment (exit status 2). `mh4u_native_runner --help` prints the usage, which lists every option that
is linked in, including the ones other modules registered.

## The runner's own options

These are registered by `mh4n_runtime` (and, for the first four, by `main.cpp`) in the scope
`runner`. Other modules add theirs the same way, `MH4N_OPTION("runner", "--name", "ARG", "help")`
from their own directory, and they appear in `--help` by themselves: nothing here needs editing
for that.

| Option | Meaning |
|---|---|
| `--help`, `--version` | the usage; the git revision |
| `--log FILTER` | the log filter, `Category:Level` rules in order, `*` for every category (default `*:Info`) |
| `--print-process` | load `PROGRAM`, print its process image and segment map, and exit (status 2 if loading fails); ignores `--code` |
| `--code LIB` | the code library. Not needed by a program without guest code |
| `--ms N` | stop after `N` ms of game time. The default is 10000. `f<N>`: after game frame `N` (as `--frames`) |
| `--frames N` | stop when the game's frame counter, the `u32` at `0x010571d4`, reaches `N`. With `--frames` and no `--ms` there is no time limit |
| `--trace-svc FILE` | every system call, in the format of `tools/native/tracefmt.py` (below) |
| `--state-trace FILE` | the game's observable state at each frame, in the format of `tools/native/state_diff.py` (below) |
| `--observable FILE` | the values `--state-trace` records (default `config/native/observable.txt`, relative to the current directory) |
| `--slice-us N` | the scheduler's slice when both virtual cores have work (default 1000) |
| `--user DIR` | the user directory (NAND, SD card, saves) for the services that need one; never created by the runtime |
| `--init-time SECONDS` | the date the console starts at, seconds since the Unix epoch, UTC. The default, 1767268800 (2026-01-01 12:00 UTC), is the fixed start of the reference runs: the game seeds its random numbers from the clock |
| `--gpu NAME` | the GPU (default `null`) |
| `--input-source NAME` | where the buttons, circle pad and touch screen come from (default `script`) |
| `--audio NAME` | where the DSP's output goes (default `null`) |
| `--missing-entries FILE` | append the entry of each function the fallback interpreter had to run, in the format of `config/extra_entries.txt` (default `build/missing_entries.txt`; `""` for none), deduplicated against the file (N1-28) |
| `--trace-ipc FILE` | every IPC request with its reply, in the format of `tools/native/tracefmt.py` and `tools/compare_ipc_traces.py` (N0-21): session names (`srv:`, `<service>/<kind>#k`), `t=` in game ticks (N1-10) |
| `--trace-ipc-full` | with `--trace-ipc`: also the bytes of the input buffers as `data=<index>:<hex>`; alone it is refused with exit status 2 |
| `--input SCRIPT` | scripted input (N1-16): comma-separated presses `WHEN:BUTTON[:HOLD]`, `WHEN:TOUCH:x:y[:HOLD]`, `WHEN:CIRCLE:x:y[:HOLD]`, e.g. `3000:A,5000:TOUCH:160:120`; `WHEN` and `HOLD` are ms of game time or `fN` frames; repeatable. The grammar is the reference runner's (`--print-schedule`). `--input` values are parsed before `--input-file` values, so overlapping presses of the same kind from the two options can order differently from the reference runner's argv order |
| `--input-file PATH` | the same presses, one per line; `#` starts a comment (repeatable) |
| `--capture-hid FILE` | the HID shared memory (and ir:USER's, once the game sets it up) on every game frame, in the MHID format of `tools/native/capfmt.py` (N1-16); compare with `capfmt.py diff-hid` |
| `--capture-dsp START:FRAMES:FILE` | from the first DSP audio frame at or after game frame `START` (`fN` allowed), record `FRAMES` audio frames to `FILE`, MDSP type-1 records only (N1-18); compare with `capfmt.py diff-dsp`. A bad argument or an unwritable file ends the run with status 2 |
| `--io-latency azahar\|none` | when file replies arrive (N1-12, D26): no earlier than Azahar's modelled delay on the game clock (`azahar`, the default), or as soon as the data is read (`none`). Also an option of `mh4n_ipc_replay` |
| `--new-console-id` | give the console a new random unique ID before the game starts, saved in the user folder's config (N1-15). Without a config file the ID is 0 and nothing is written (Azahar makes a random one: open question in the batch PR) |
| `--region REGION` | the console's region (N1-15): `auto` (the game's region lockout from `icon.bin` in the game folder, the default; `prepare_game.py` does not write one yet, so GetRegion answers JPN and logs a warning once) or `0` JPN, `1` USA, `2` EUR, `3` AUS, `4` CHN, `5` KOR, `6` TWN. Also an option of `mh4n_ipc_replay`; every cfg replay passes it |
| `--swkbd-text TEXT` | the text the software keyboard answers with (N1-14), repeatable: the Nth keyboard the game opens gets the Nth; without one it answers with the user name of the cfg service. Also an option of `mh4n_ipc_replay` |
| `--set MS\|fN:KEY=VALUE` | at that game time (ms) or game frame (`fN`), change one of the hooks' options that can change while the game runs, e.g. `0:fps=120` or `f600:fps=60` (N1-20); repeatable. A key that cannot change while the game runs ends the run with status 2 (the reference stops with status 1) |
| `--options FILE` | the host's own settings in the layout of `mh4u-app.ini` (`[Section]` lines, `key = value` lines) (N1-20). All keys are stored for the renderer (N2b); only `aspect_ratio` answers Get/SetOption. A bad line or value ends Boot with status 2 |

A device name nothing has registered leaves that interface empty, with an Info line. Registered
today: the `null` GPU (N1-17), the `script` input source (N1-16) and the `null` audio sink (N1-18);
the capture player's GPUs come with N2 and the real audio sinks with N3.

## Output and exit status

The program's `svcOutputDebugString` text goes to standard output, one line each, as

```
OutputDebugString:0: <text>
```

so that the regular expression of `tests/test_conformance.py` reads both runners (the reference
logs the same words with its own prefix). Log lines (`MH4N_LOG`) go to standard error.

| Status | Meaning |
|---|---|
| 0 | the program exited (`svcExitProcess`), or the `--ms` / `--frames` limit was reached |
| 2 | loading failed: no or too many `PROGRAM`, a missing file, a bad option value, a code library that cannot be loaded or does not match the program, a trace file that cannot be written |
| 3 | a runtime error: `svcBreak` (the reason and the registers are logged), a fatal error through `err:f` (N1-10); a bad guest memory access ends the process with 3 by itself, after one line on standard error |
| 4 | the recompiled code stopped: code that was not recompiled, a bad return, a trap |

## The boot order

`Runtime::Boot(options)` does this, in order:

1. loads the program (`Loader::OpenGameFolder` or `OpenElf`);
2. makes guest memory, the game clock (stopped once the world is built: it keeps the ticks of the
   setup, a host-dependent few tens of thousands, and runs again when the scheduler starts, so the
   first call of a trace is at that tick and the process is created at a tick above 0, as `conf_info`
   expects), the code host, the kernel, the scheduler, the thread manager and the SVC table, and the
   process;
3. maps the segments, the configuration memory and the shared page;
4. loads the code library and checks it against the text segment;
5. runs the **boot steps** of the phases `devices`, `services` and `tracing`;
6. creates the main thread;
7. runs the steps of the phase `started` (the main thread exists and has not run);
8. starts the clock and runs the scheduler until the program exits or a limit is reached.

A boot step is registered by name from any module, and runs in phase order and then in name order:

```cpp
MH4N_REGISTER(BootStep, "services.srv", BootStep{Phase::Services, [](Runtime& rt) { ... }});
```

| Phase | For |
|---|---|
| `devices` | the host interfaces. The runtime's own step (`runtime.devices`) makes the GPU, input source and audio sink named by `--gpu`, `--input-source` and `--audio` from the registries of `gpu::GpuFactory`, `InputSourceFactory` and `AudioSinkFactory` (`mh4n/host.h`) |
| `services` | ports and services. N1-10's step `services.ipc` makes the `ServiceManager`, installs every registered `ServiceFactory` and sets `rt.Svc().SetConnectToPortHandler(...)` and `SetSendSyncRequestHandler(...)`; later services only register a factory |
| `tracing` | traces and captures: the runtime's `--trace-svc` and `--state-trace`; N1-10's `tracing.ipc` (`--trace-ipc`, `--trace-ipc-full`) |
| `started` | whatever needs the main thread (`rt.MainThread()`). N1-28's step `cpu.interp` installs the fallback interpreter as the missing-function handler, only when a code library is loaded; N1-20's step `platform.hooks` installs the native `Platform` and then the hooks on the loaded library, and arms the `--set` changes |

A step that cannot do its job (a trace file that cannot be written) logs it and calls
`rt.RequestExit(2, reason)`: Boot returns that status before the program runs.

`Runtime::Prepare(options, phases)` makes memory, the kernel and the process, and runs only the
steps of the given phases, with no program and no code library. The replay tool (N1-11) uses it
with `devices` and `services`, and drives deferred replies with `rt.Scheduler().RunPending()`.

What a module can use from the `Runtime` (the header documents each):

- `OnFrame(fn(frame, tick))`: called on the scheduler thread when the frame counter changes (the
  first reading counts). The frame watcher runs at every safe point of the scheduler, reads the
  counter, drives `--frames` and the state trace;
- `AddSvcHooks(SvcHooks)`: more subscribers to the SVC layer's hooks (every subscriber is called);
- `Own(object)` and `OnFinish(fn)`: keep state for the life of the runtime, and finish a trace when
  the run is over;
- `SetGpuInterruptSink`, `SetRefreshRateSink` and `SetRefreshRate`: how GSP (N1-17) and the
  platform (N1-20) meet; `GpuHost()` is the runtime's `gpu::Host`: the physical view of guest
  memory, interrupts and `Post` through the kernel.

`gpu::Host::PhysicalPointer(paddr, size)` is `GuestMemory::PhysicalPointer` plus the one rule
`mh4n/gpu.h` adds: size 0 accepts the address one past the end of an area (VRAM, DSP RAM, FCRAM;
FCRAM is the linear heap, so until the loader has set the heap's base it has no view). The
Old 3DS FCRAM ends at `0x28000000`.

`port/src/schedule.cpp` (N0-08: the parser of times, input events and scheduled options) is compiled
into `mh4n_runtime`, so a module that depends on it includes `"schedule.h"` and calls `Schedule::Parse*`; the
runtime parses `--ms` and `--frames` with it.

## The traces

Both are written by `tracing` steps of the runtime, with `cpu=native` in the header, and compare
with the reference's through the same tools.

`--trace-svc FILE` (N0-04, `tools/native/tracefmt.py`):

```
# svctrace v1 <date> game=0x<title id> cpu=native new3ds=off
# thread <id> parent=<id> entry=<hex> arg=<hex> stack=<hex> prio=<n> core=<requested>/<resolved>
<thread> <svc> pc=<hex> lr=<hex> <r0> <r1> <r2> <r3> t=<ticks> in4=<r4>,<r5>,<r6> out=<r0>,<r1>,<r2>,<r3>
```

A call's line is written when the call completes for its thread, so a call that blocked comes
after other threads' later calls; `out=-` marks a call that had not completed when the run ended.
`t=` is game-clock ticks since the scheduler started. The main thread's line (`parent=0`) comes
first.

`--state-trace FILE` (N0-07): a header `# statetrace v1 …` and a line `f=<frame> t=<ticks>
<name>=0x<hex>|-` whenever the frame counter changes. `--observable` names the list of values
(`config/native/observable.txt`).

A bad guest memory access ends the process at once (`_exit`, memory.h), so the last buffered lines
of a trace are lost; the files are closed and complete in every other case.

```
python3 tools/compare_svc_traces.py reference.svc native.svc --normalize --outputs \
    --ignore-regs config/native/svc_unused_registers.txt
python3 tools/native/state_diff.py reference.state native.state
```

## Differences from the reference that are on purpose

- **Time.** The reference charges emulated time to instructions; the native runtime's game clock
  follows the host's, so a run that depends on how long computing takes (threads that sleep for a
  few hundred microseconds between short computations) interleaves differently from run to run.
  `svc_test` is such a program: the calls, their places and their results agree with the
  reference, but the registers that carry a value built under a mutex at the time of the call
  (r1 of `ReleaseMutex`, which the call neither reads nor writes) differ from run to run. Without
  `--ignore-regs config/native/svc_unused_registers.txt` the comparison of the two traces fails
  there and only there (checked on 10 runs); with the rule file it passes. The deterministic mode
  (N1-25) is where cycle-driven time lives, and compares without the rules.
- **Services.** Only `srv:` and `err:f` exist (N1-10): until the services of N1-12 on are written,
  `srv:` GetServiceHandle for any other name answers Azahar's error (or waits for the service, with
  the flag's bit 0 clear) and `svcConnectToPort` answers "not found" for every other port. `conf_srv` (it asks for `cfg:u` and `ptm:u`, N1-15) and
  `conf_services` stay in `NATIVE_NOT_YET` of `tests/test_conformance.py` together with `conf_mem`
  (N1-05 does not alias shared-memory pages) and `conf_info` (the SYSTEM region is empty without the
  HID and GSP blocks).

## Running the conformance programs

```
cmake -S native -B build/native -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMH4N_WERROR=ON
cmake --build build/native
MH4N_RUNNER=$PWD/build/native/bin/mh4u_native_runner python3 -m pytest tests/test_conformance.py
```

The test builds each program with `--native` (needs `clang`, `ld.lld`, `llvm-objcopy` and
`llvm-readelf`), runs it with `--ms 5000`, and compares the debug lines with
`port/tests/expected/<program>.txt`. The `native` job of `.github/workflows/native.yml` does the
same after ctest.
