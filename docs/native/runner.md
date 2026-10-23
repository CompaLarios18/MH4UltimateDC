# The native runner and the runtime (N1-09)

`mh4u_native_runner` runs a program on the native modules, without Azahar. The logic is the
module `native/src/runtime/` (`mh4n_runtime`, the class `mh4n::Runtime` of
`native/include/mh4n/runtime.h`); the program in `native/runner/` is only `main`: the command
line, `--help`, `--version`, `--print-process` and the printing of the program's debug strings.
Its options are those of `mh4u_runner` (the reference, `port/README.md`) wherever the meaning is
the same. On Windows the CI artifact `mh4n-runner-windows` ([`CI.md`](CI.md)) holds
`mh4u_native_runner.exe`, the IPC replay, the capture player and the DLLs they need, with no game data.

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
| `--user DIR` | the user directory (NAND, SD card, saves) for the services that need one; the start-up step never creates it. When it exists and holds no config savegame, the start-up step makes the config once with a random console ID, atomically, in Azahar's format and path (D37, N1-15b); an existing valid config is never touched and a damaged one is left alone with an Error. With a missing `DIR` the console ID is 0 for that run and nothing is made (`mac.txt` and `mcu.dat` do not create it either; a saving `cfg:s`/`cfg:i` command still does). Without `--user` nothing is written at all. Without it the hooks get a placeholder folder and read no `mh4u.ini` (fps and widescreen keep their defaults); only `--user` makes them read `DIR/mh4u.ini` (N1-20b) |
| `--shots DIR`, `--shot-every MS\|fN`, `--shots-from MS\|fN` | save both screens as PPM images in `DIR` (`--shot-every` default 1000 ms; `fN` is in game frames), named as the reference's. Only a GPU that shows its screens (`present::Display`: `null-display`, and `renderer` from N2b-13) can take shots; with any other `--gpu` the run ends with status 2 and a message. Also writes `DIR/gas.txt` (below, *Parity with the reference runner*) |
| `--init-time SECONDS[.FRACTION]` | the date the console starts at, seconds since the Unix epoch, UTC (N0-35, D83). Grammar, nothing else accepted: `SECONDS` is 1 to 10 ASCII digits, `FRACTION` is `.` and 1 to 6 ASCII digits (microseconds); no sign, spaces, exponent, hex, `.5`, `5.` or second dot. The range, both ends included, is 946684800 (2000-01-01 00:00:00 UTC) to 4102444799.999999 (2099-12-31 23:59:59.999999 UTC); a malformed or out-of-range value ends the run with status 2 and a message naming `--init-time` and the text, before anything is booted (`Runtime::Run` and `Runtime::Prepare` alike). The default, 1767268800 (2026-01-01 12:00 UTC), is the fixed start of the reference runs: the game seeds its random numbers from the clock; a value without a fraction, or with an all-zero one, is exactly the integer, and a run without the option is what it was before the fraction existed. It moves the shared page's start date only (the date records are `floor((start + elapsed) / 1000)` milliseconds, the start in microseconds): the game's tick values and everything else are as they were |
| `--gpu NAME` | the GPU (default `null`) |
| `--input-source NAME` | where the buttons, circle pad and touch screen come from (default `script`, the presses of `--input` and `--input-file`). `live` (N2b-08, D59): the app feeds it from its own thread (`input_live::Find(runtime)->Set(...)`, `SetTouch(...)`) and the game sees a change only at its next frame, see `--input-latch` and `--record-input` |
| `--audio NAME` | where the DSP's output goes (default `null`). `stream` (N3-05, D63): a ring of 8192 stereo frames the app's audio device pulls from, with `audio_out::Find(rt)`; `wav`: every queued frame into the 16-bit stereo WAV given by `--audio-file`, at 32,728 Hz, its header finished at the end of the run; `sdl` (N3-06, `mh4u-native` only): the `stream` played on the default audio device through SDL3 (`docs/native/audio-out.md`) |
| `--audio-latency MS` | with `--audio stream`: the target fill of the ring, 10 to 200 ms (default 40; a first guess, D63); anything else ends the run with status 2 |
| `--audio-file FILE` | with `--audio wav`: the WAV file; missing or unwritable ends the run with status 2 |
| `--volume N` | with `--audio sdl` (`mh4u-native` only, N3-06): the volume, 0 to 100 (default 100); the gain is (N/100)³ through `SDL_SetAudioStreamGain`, Azahar's curve; anything else ends the run with status 2 (`docs/native/audio-out.md`) |
| `--audio-log FILE` | with `--audio sdl` (`mh4u-native` only, N3-06): a CSV `# audiolog v1 <date> device_rate=<Hz>`, a column line, then a row per second of host time and a last row at the end of the run: `t_ms,frame,fill_frames,ratio,underruns,overflows,dropped`; a file that cannot be created ends the run with status 2 |
| `--missing-entries FILE` | append the entry of each function the fallback interpreter had to run, in the format of `config/extra_entries.txt` (default `build/missing_entries.txt`; `""` for none), deduplicated against the file (N1-28) |
| `--trace-ipc FILE` | every IPC request with its reply, in the format of `tools/native/tracefmt.py` and `tools/compare_ipc_traces.py` (N0-21): session names (`srv:`, `<service>/<kind>#k`), `t=` in game ticks (N1-10) |
| `--trace-ipc-full` | with `--trace-ipc`: also the bytes of the input buffers as `data=<index>:<hex>`; alone it is refused with exit status 2 |
| `--input SCRIPT` | scripted input (N1-16): comma-separated presses `WHEN:BUTTON[:HOLD]`, `WHEN:TOUCH:x:y[:HOLD]`, `WHEN:CIRCLE:x:y[:HOLD]`, e.g. `3000:A,5000:TOUCH:160:120`; `WHEN` and `HOLD` are ms of game time or `fN` frames; repeatable. The grammar is the reference runner's (`--print-schedule`). `--input` values are parsed before `--input-file` values, so overlapping presses of the same kind from the two options can order differently from the reference runner's argv order |
| `--input-file PATH` | the same presses, one per line; `#` starts a comment (repeatable) |
| `--input-latch frame\|none` | with `--input-source live` (N2b-08, `mh4n_input_live`): `frame` (the default) = the state set last becomes the one the game reads at each change of its frame counter and stays until the next, so a change is seen up to one frame late and the session can be replayed exactly; before the counter has been seen (a program that never maps it) the state set last is read. `none` = the state set last, at once; such a session is recorded on the frame it was seen in and does not replay exactly. Any other value ends the run with status 2. Given with another input source it is ignored, with a warning |
| `--record-input FILE` | with `--input-source live` (N2b-08): write what the game saw as a script that `--input-file` reads back, when the run ends: `f<N>:<BUTTON>:f<held>`, `f<N>:TOUCH:<x>:<y>:f<held>`, `f<N>:CIRCLE:<x>:<y>:f<held>` (a press still held at the end is held through the last frame seen), in milliseconds of game time before the frame counter has been seen. The C-stick, ZL/ZR and the motion sensors are not written. A file that cannot be written ends the run with status 2 before the game starts. Given with another input source it is ignored, with a warning |
| `--capture-hid FILE` | the HID shared memory (and ir:USER's, once the game sets it up) on every game frame, in the MHID format of `tools/native/capfmt.py` (N1-16); compare with `capfmt.py diff-hid` |
| `--capture-dsp START:FRAMES:FILE` | from the first DSP audio frame at or after game frame `START` (`fN` allowed), record `FRAMES` audio frames to `FILE`, MDSP type-1 records only (N1-18); compare with `capfmt.py diff-dsp`. A bad argument or an unwritable file ends the run with status 2 |
| `--io-latency azahar\|none` | when file replies arrive (N1-12, D26): no earlier than Azahar's modelled delay on the game clock (`azahar`, the default), or as soon as the data is read (`none`). Also an option of `mh4n_ipc_replay` |
| `--new-console-id` | give the console a new random unique ID before the game starts, saved in the user folder's config (N1-15). It replaces the ID of a valid config and makes the config when `DIR` exists and holds none (it is the one explicit way to create a missing `DIR`); over a damaged config, a directory or a dangling link at the config path it exits 3 and writes nothing (N1-15b). Without `--new-console-id` the first start of an existing `DIR` with no config makes a random ID by itself (D37) |
| `--region REGION` | the console's region (N1-15): `auto` (the game's region lockout from `icon.bin` in the game folder, the default; `prepare_game.py` writes it from the base game's ExeFS icon (N1-15c); a game folder without one answers JPN for GetRegion and logs a warning once) or `0` JPN, `1` USA, `2` EUR, `3` AUS, `4` CHN, `5` KOR, `6` TWN. Also an option of `mh4n_ipc_replay`; every cfg replay passes it |
| `--swkbd-text TEXT` | the text the software keyboard answers with (N1-14), repeatable: the Nth keyboard the game opens gets the Nth; without one it answers with the user name of the cfg service. Also an option of `mh4n_ipc_replay` |
| `--set MS\|fN:KEY=VALUE` | at that game time (ms) or game frame (`fN`), change one of the hooks' options that can change while the game runs, e.g. `0:fps=120` or `f600:fps=60` (N1-20); repeatable. A key that cannot change while the game runs ends the run with status 2 (the reference stops with status 1) |
| `--options FILE` | the host's own settings in the layout of `mh4u-app.ini` (`[Section]` lines, `key = value` lines) (N1-20). All keys are stored for the renderer (N2b); only `aspect_ratio` answers Get/SetOption. A bad line or value ends Boot with status 2 |

A device name nothing has registered leaves that interface empty, with an Info line. Registered
today: the `null` GPU (N1-17), the `script` (N1-16) and `live` (N2b-08) input sources and the `null` (N1-18), `stream` and `wav` (N3-05) audio sinks;
the capture player's GPUs come with N2; the `sdl` audio sink (N3-06) is registered by the app module `native/app/audio/`, so only `mh4u-native` has it.

## Paths on Windows (D45)

On Windows the runner reads its command line as UTF-16 and works in UTF-8 inside (N1-13d, N1-13e): `--user`, the game folder, the ELF/program path and `--options FILE` may contain accents or other non-ASCII characters, and the saves, the config savegame, `mac.txt`, `mcu.dat` and `exheader.bin` are found there.

- The `mh4u.ini` the port's hooks read is opened by the hooks with an ANSI string (their file is shared with the Azahar build and is not edited). For a `--user` folder with non-ASCII characters the platform therefore gives the hooks the folder's 8.3 short name; if the volume has no short names (they can be switched off) the hooks get the placeholder folder, `mh4u.ini` is not read, fps and widescreen keep their defaults and one Warning says why and how to fix it (use an ASCII `--user` path, or `fsutil 8dot3name`).
- The developer options that write or read traces and captures keep the Windows active code page (`--trace-ipc`, `--trace-svc`, `--state-trace`, `--observable`, `--capture-hid`, `--capture-dsp`, `--tex-hashes-out`, `--missing-entries`, `--judge`, the GPU capture player, the HD pack tools and the replay tool's `--game` and `--svc-trace`): use ASCII paths for them. The goldens live in ASCII folders.

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

## Parity with the reference runner

`tools/native/golden.py check S --candidate-runner <this runner> --candidate-program <game folder> --candidate-code
<library>` starts the runner with `PROGRAM --code LIB --user DIR --frames N --input-file SESSION`, a `--set
MS:KEY=VALUE` for each one given, and then `--trace-svc`, `--trace-ipc`, `--trace-ipc-full`, `--state-trace`,
`--capture-hid` and a `--capture-dsp START:FRAMES:FILE` per DSP capture of the session, plus whatever
`--candidate-args` adds (checked in N1-09b by reading both runners' sources and running a conformance program
with the whole line; no game was involved). `--observable FILE` is passed too (to the candidate as well, since N1-21);
`--ms` and `--log` are not passed by golden.py; `--init-time` is passed (to the reference and to the candidate alike) when the session has an `# init-time:` header, and then the native runner sees the same date as the reference (N0-35). On Windows the runner asks for a 1 ms host timer
tick and opts out of power throttling for the length of the run (N1-21; D77), whatever launches it. `same`: the same syntax, and the same format where it writes one. `differs`: how. The status of a
value (times, ticks) is not part of this table: a different timing model is the `cross` profile's business, see
"Differences from the reference that are on purpose".

| Option | Status | Served by |
|---|---|---|
| `--user DIR` | `same` for a folder that exists (golden.py always passes a copy of the recording's `user-before/`): the layout is Azahar's. `differs`: a missing `DIR` is not created, and a bad folder is not an error of the option | `mh4n_runtime` (`Runtime::UserFolder`); the services that read it (cfg, fs, N1-12 to N1-15) and the platform's `mh4u.ini` (N1-20) |
| `--frames N` | `same` parser (`Schedule::ParseFrames`, `port/src/schedule.cpp`); the run stops at the first safe point at which the counter is at least `N`, after the frame's state line and HID record were written, with exit status 0 and no time limit unless `--ms` is also given. `differs`: a bad value ends with status 2 (the reference: 1) | `mh4n_runtime` (`Runtime::PollFrame`) |
| `--ms N\|fN` | `same` parser and default (10000 ms) | `mh4n_runtime` |
| `--code LIB` | `same` | `mh4n_runtime` (code host) |
| `--input-file PATH` | `same` parser (`Schedule::ParseInputFile`): `#` comments, several comma-separated events per line, `fN` times and holds: golden.py's session files (header lines `# key: value` are comments) are read as the reference reads them (a test of `hid_test` pins it). Repeatable; `--input` values are parsed before it. `differs`: an unreadable or bad file ends with status 2 before the program runs (the reference: 1) | `mh4n_input_script` (`ScriptInput`, `ParseScripts`) feeding the HID service |
| `--set MS\|fN:KEY=VALUE` | `same` syntax and keys (`Schedule::ParseOptionChange`; the hooks' own `SetOption`, compiled from `port/src/hooks.cpp`), applied at that game time or frame. `differs`: a bad argument or a key that cannot change ends with status 2 (the reference: 1). Note: the changes are armed at the `started` step and the hooks are installed on a code library, so a value has an effect only when a library is loaded (the reference's hooks need `--cpu recomp` and a library too); the key is validated either way | `mh4n_platform` (`InstallOptionChanges`) |
| `--trace-svc FILE` | `same` format and header (`# svctrace v1 <date> game=0x<id> cpu=native new3ds=off`, `# thread` lines, call lines, `out=-` for a call that had not completed); `cpu=native` and `new3ds=off` are the header's only differences | `mh4n_runtime` (`svc_trace.cpp`) |
| `--trace-ipc FILE` | `same` format: `# ipctrace v1` header, the thread lines of the SVC trace, `seq=`, `session=` (`srv:`, `<service>/file#k`, `<service>/dir#k`, `<service>/session#k`, `unnamed#k`), `cmd=`, `in=`, `desc=`, `out=`, `outdesc=`, `t=` in game ticks | `mh4n_ipc` (`trace.cpp`) |
| `--trace-ipc-full` | `same` (a flag; adds `data=<index>:<hex>` for readable input buffers). `differs`: without `--trace-ipc` it ends with status 2 (the reference: 1) | `mh4n_ipc` |
| `--state-trace FILE` | `same` format (`# statetrace v1` header; `f= t= <name>=0x<hex>\|-` whenever the counter changes, the first reading included) | `mh4n_runtime` (`state_trace.cpp`) |
| `--observable FILE` | `same` file format (`config/native/observable.txt`) and default (relative to the current directory: golden.py runs the runner from the repository root). A file that cannot be read ends with status 2 (the reference: 1) | `mh4n_runtime` |
| `--capture-hid FILE` | `same` MHID format and manifest (`FILE.json`; its `tool` is `mh4u_native_runner`): one type-1 record per change of the frame counter, a type-2 record once ir:USER has a shared memory block. `differs`: the HID memory is read from the service's own block, zeros until the game has mapped it; the reference reads it through the game's mapping | `mh4n_services_hid` (`hid_capture.cpp`) |
| `--capture-dsp START:FRAMES:FILE` | `same` syntax (`START` may be `fN`; `FILE` may hold colons) and MDSP format and manifest. `differs`: type-1 (`REGIONS`) records only: the reference's type-2 (`PCM`) and type-3 (`SAMPLES`) records are not written, and `capfmt.py diff-dsp` compares the type-1 records only | `mh4n_services_dsp` (`dsp_capture.cpp`) |
| `--log FILTER` | `differs`: the categories are the native modules' (`Kernel`, `Service.SRV`, ...), not Azahar's | `main.cpp` |
| `--input-latch frame\|none`, `--record-input FILE` | native only (the reference has neither; golden.py does not pass them): they belong to `--input-source live` (N2b-08). A `--record-input` file is an `--input-file`: both runners read it, with the same presses at the same frames | `mh4n_input_live` |
| `--shots DIR`, `--shot-every MS\|fN`, `--shots-from MS\|fN` | `same` syntax, defaults (1000 ms), file names and timing: the reference's own `Schedule::ShotSchedule` decides, asked once per published frame with the game clock's milliseconds and the game's frame counter. A shot is both screens as binary PPM, `f%07u_top.ppm` / `f%07u_bottom.ppm` for frames (named by the scheduled frame, taken at the first published frame that reads it or more; a frame the counter went past has no shot) and `%07u_top.ppm` for milliseconds (named by the scheduled time). Only a GPU that implements `present::Display` can take shots: `null-display` (N2b-07) and, from N2b-13, `renderer`; with any other `--gpu` (the default `null` included) `--shots` ends the run with status 2 and a message. `differs`: a bad `--shots-from` / `--shot-every` ends with status 2 whether or not `--shots` is given (the reference: 1); the folder is created like the reference's; a screen that is off has no file; a program that does not map the game's frame counter (`io_test`) has only millisecond shots; the picture is what the GPU shows at the VBlank (`null-display`: `DisplayShadow::BuildScreen` over guest memory, so a display transfer the null GPU skips is not in it); `DIR/gas.txt` is native only (below); no `--draw-window` (the screens are not drawn by the runner) | `mh4n_shots` (`ShotSink`), reading `present::Display` |
| `--init-time SECONDS[.FRACTION]` | `same` syntax, range and default (up to six decimals; status 2 for a malformed or out-of-range value, as the reference: N0-35); the same date reaches the game in the shared page's date records. golden.py passes it to both runners when the session has an `# init-time:` header | `mh4n_runtime` (`ParseInitTime` in `mh4n_loader`) |

The reference's other options are not in this runner, and an unknown option ends the run with status 2:
`--cpu`, `--new-3ds`, `--install`, `--dump`, `--replace`,
`--freeze`, `--stalls`, `--lockstep`, `--call-counts`, `--trace-timing`, `--count-unmapped`, `--capture-gpu`,
`--perf-log`, `--speed`, `--cpu-clock`, `--async-io`, `--draw-window`, `--room-host`, `--room-join`, `--nick`
and `--print-schedule`. golden.py does not pass any of them to a candidate runner (they are the reference's
`full` list), so a candidate run does not write `timing.txt`, `unmapped.txt`, a GPU capture or the
shared-page dump, and golden.py skips those comparisons for it. Screenshots (`--shots`, `--shots-from`,
`--shot-every`) are the exception: the runner takes them (N2b-14; N2b-17 makes golden.py pass them to a
candidate runner whose `--help` lists `--shots`, started with a GPU that shows its screens).

**`gas.txt`** (native only). With `--shots DIR` the runner also writes `DIR/gas.txt` when the run ends: one
line per shot that follows a published frame with `gas` set (a draw with fog mode 7, which Azahar does not
draw) since the previous shot, the frame the shot is taken from included. A line is the shot's number
without padding and without the `f`: `300` for `f0000300_top.ppm`, `1500` for `0001500_top.ppm`. The file
is there, empty, when there was no gas, so its absence means a runner that does not report gas (the
reference). The cross-profile image check (N2b-17) reports such shots and does not judge them. Under
`null-display` nothing sets `gas`, so the file is empty; the renderer (N2b-13) sets it.

**Threads.** A shot is asked for in `ScreenSink::Publish`, on the thread that handles the VBlank, and
nothing there waits for another thread: the files are written by the `done` callbacks of
`Display::ReadScreen`, inside the call (`null-display`) or later on the renderer's thread. At the end of the
run (`Runtime::OnFinish`, before the GPU is destroyed) the sink is removed, the callbacks still pending are
waited for (up to 30 s; an error line if some never come) and `gas.txt` is written. The run's last line of
this module: `Shots: <n> screenshots taken, <m> files written` at Info.
Under a GPU that queues its frames (`--gpu renderer`, from N2b-13) a shot due in the very last queued frames may be missed, because the sink is removed after the scheduler stops.

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
