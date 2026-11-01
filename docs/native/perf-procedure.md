# Performance baseline: the steps (N0-15)

For the maintainer. Goal: where the 8.3 ms of a 120-fps frame go today, on the reference PC
(Ryzen 5 5600X, RX 6700 XT, a 3440x1440 monitor) and on the M2 Mac. You run the game, the app
writes a CSV per run, `tools/native/perf_report.py` turns each CSV into a table, and you give the
tables to the orchestrator, who writes `docs/research/perf-baseline.md`. The CSV holds only
timings, no game data; still keep the files in a folder outside the repository.

## What is measured

One CSV row per frame (the time from one vertical blank of the emulated GPU to the next):

| Column | Meaning |
|---|---|
| `present_us` | the frame time (present to present) |
| `recomp_us` | time in the game's recompiled code (the interpreter fallback included), without the next two |
| `svc_us` | time in system calls (kernel and services), without the GPU commands they ran |
| `gpu_us` | time the emulation thread spends processing the GPU's commands (command lists, display transfers, fills) |

Each microsecond is counted once. The report adds `other_us` = `present_us` minus the three:
waiting for the renderer's worker threads, the frame limiter and vsync. Azahar's Vulkan renderer
works on other threads and has no GPU timestamps, so **the GPU's own time is `other_us`, not
`gpu_us`**: read `gpu_us` as the cost of translating the game's command lists.

## 1. Get the build

1. Dispatch the Windows build of the `native` branch: GitHub, Actions, *Windows build*, *Run
   workflow*, branch `native` (or `gh workflow run windows.yml --ref native`).
2. Download the artifact `mh4u-port-windows` of that run and unpack it. For the Mac, dispatch
   `macos.yml` the same way and unpack its artifact.
3. Set the game up as the package's instructions say (`Build code library.bat`, your own dump).

## 2. Settings for every run

All in `mh4u-app.ini` and `mh4u.ini`, in Azahar's user folder (the folder that holds both files;
the app writes them on its first start).

`mh4u-app.ini` (comments go on their own lines, not after a value):
```
[Game]
new_3ds = off
[Display]
renderer = vulkan
fullscreen = on
vsync = on
resolution = 0
custom_textures = off
fps_counter = top-left
[Debug]
perf_log = D:\perf\village_hd-off_fps120.csv
```
`resolution = 0` means as many times the 3DS's size as the window needs (3440x1440 full
screen); `custom_textures = on` is the HD pack; `vsync` is explained in the notes. Change
`perf_log` before every run (one file per run). `new_3ds = off` stays the same in every run: its
default, `auto`, turns New 3DS mode **on** when `custom_textures` is off and off when it is on, so
the two columns of the matrix would differ in the console model (memory layout, the second core's
extra work) as well as in the HD pack. The header line of a log records `new3ds=` so a run with
the wrong value is visible.

`mh4u.ini`: `fps = 60`, `120` or `180`, as the matrix says.

The matrix is every combination of **HD pack off/on** (`custom_textures`; the HD textures are in
`load/textures/0004000000126300/`) and **`fps` 60 / 120 / 180**, in each of the three scenes:
2 x 3 x 3 = 18 runs per machine. On the M2 run only the HD pack off and on at `fps = 60` (the
target there is a steady 60 fps), in the village and in the hunt.

Close every other program (browsers, overlays). Always start the app from a fresh launch, so the
shader cache state is the same: do **one** warm-up run first (any scene, 5 minutes) and then
record with the cache warm.

## 3. The three scenes

1. **The village.** Load your save in Moga Village; stand at the gate with the camera looking
   down the main street, then walk the length of the village and back at a steady pace. Do not
   open menus.
2. **The busy hunt (G3).** A quest with a large monster and plenty on screen, as G3 defines it
   (`docs/native/wp/N0.md`, N0-10): Arena Bambina, *Grudge Match: Yian Kut-Ku*, with the same
   preset loadout; fight it, do not stand still.
3. **A four-player hall.** Host a room, have three other players join (real players or three
   runners on another machine, `docs/SERVER.md`), meet in the Gathering Hall and walk around
   together; then go on a quest together and fight for the minute recorded.

## 4. Recording

- **Length:** 60 seconds of play per run, after the scene has loaded and you are in control (at
  120 fps that is 7200 rows). Start the app, wait 10 seconds in the scene, then start counting;
  the log covers the whole session, so the report skips the loading (`--skip`, below). Quit with
  the menu's *Quit* (Esc), not by killing the process, so the last rows are written (the app
  writes them at exit).
- **Where the CSV goes:** the path in `perf_log`. Name it
  `<machine>_<scene>_<hd on|off>_fps<N>.csv`, for example `pc_hunt_hd-on_fps120.csv`. Keep all
  of them in one folder outside the repository.
- `perf_log` takes effect at the next start of the app. Empty it to switch the log off.

## 5. The tables

For each CSV, from the repository:

```
python3 tools/native/perf_report.py pc_hunt_hd-on_fps120.csv --fps 120 --skip 1800
```

`--skip` leaves out the first frames (the loading and the 10 seconds of settling: 1800 frames at
`fps = 120`, 900 at 60, 2700 at 180; leave out more if the log started earlier). `--fps` sets the
frame budget (8.333 ms at 120; use the `fps` of the run). The output is a markdown table, per
column p50 / p95 / p99 / max in milliseconds, then the frames over the budget and the frames over
50 ms (stutters).

Give the orchestrator all the tables, with: the machine, the Windows or macOS build (the run's
URL), the scene, the HD pack, `fps`, and whether the monitor was at 240 Hz. If a run dropped rows
(the end of the CSV says `dropped=<n>`) record it again.

## 6. Reading them (the ROADMAP section 7 budget at 120 fps)

| Part | Budget | Column |
|---|---|---|
| Game thread: logic and building command lists | 4 ms | `recomp_us` + `svc_us` (p99) |
| PICA to Vulkan translation | 3 ms | `gpu_us` (p99) |
| GPU at 3440x1440 with the HD pack | 6 ms | `other_us` (p99), an upper bound |
| Whole frame | 8.333 ms | `present_us` (p99) |

The baseline decides whether those proposed budgets stand; do not tune anything while recording.

## Notes

- With `vsync = on` on a 240 Hz monitor and `fps = 120`, a frame lasts two refreshes and
  `present_us` clusters around 8.33 ms: the headroom shows in `other_us` (waiting), not in a
  shorter `present_us`. To see how fast a scene could go, also record it with `vsync = off`.
- The runner records the same CSV for a headless run: `mh4u_runner ... --perf-log FILE`.
  Headless runs use the software renderer, so their `gpu_us` and `other_us` say nothing about
  the PC's GPU.

---

# The native runtime: does the game thread fit 4 ms? (N1-23)

For the maintainer (on the reference PC, Windows 11, PowerShell). This is N1's exit question: at 120 fps on
the Ryzen 5 5600X, does the game thread of the native runtime, without Azahar, fit its 4 ms
([`ROADMAP.md`](ROADMAP.md) section 7)? If it does not, one host thread per core (N6) moves forward to N3. The
orchestrator writes the answer in `docs/research/perf-native-n1.md`; you run G3 until three runs count (at most ten
attempts) and give it the CSVs. Everything above this line is the Azahar-based app's procedure; the CSV of the
native runner is a different one, described here.

## What the native log holds

`mh4u_native_runner ... --perf-log FILE` writes one row per game frame (a change of the game's frame counter,
the `f=` of `--state-trace`; the first row also holds the boot). It holds the scheduler's own counters (N1-06, and
N1-32's per-thread system call and service time), read once per frame; times are microseconds of the host's clock.
The file starts with `# perf v2` (N1-32); a runner from before N1-32 writes `# perf v1` and the first nine columns
only:

| Column | Meaning |
|---|---|
| `frame` | the game's frame counter, the key to the state trace |
| `present_us` | time since the previous row |
| `main_us` | the game's main thread in guest code, with the scheduler work run at its preemption checks (see below) |
| `guest_us` | every guest thread in guest code, with the same work (`main_us` included) |
| `svc_us` | in system calls, every thread |
| `service_us` | in the services' handlers (fs, gsp, ...), every thread |
| `idle_us` | both virtual cores idle: waiting for the frame limiter, a timer or a reply |
| `switches`, `preempts` | fiber switches, and threads displaced while they could still run |
| `main_svc_us` | the main thread's own part of `svc_us`: its system calls, without its services (v2) |
| `main_service_us` | the main thread's own part of `service_us`: the service handlers it called (v2) |

No microsecond is in more than one of `guest_us`, `svc_us`, `service_us` and `idle_us` (`main_us`, `main_svc_us` and
`main_service_us` are parts of the first three), but not every one is counted. All guest threads run on **one** host
thread today. `busy_us` = `guest_us` + `svc_us` + `service_us` (what `perf_report.py` adds) is the counted work of
every guest thread on it. `present_us` minus `idle_us` is `busy_us` plus the scheduler's own host loop between two guest runs
(reaping, posted work and timer events, the safe-point hooks, picking the next thread). No column holds that loop,
so the difference is at least `busy_us` (over the log; a single row can fall short by the few microseconds of the
hooks run as the system call that began it starts, which the previous row's `present_us` holds).

The scheduler does not count its own host work separately. The timer events (the DSP's, HID's and GSP's), posted
replies (fs, GPU interrupts) and safe-point hooks are charged to whatever was being counted when they ran: the
running thread's guest time at a preemption check (its `guest_us`, and `main_us` for the main thread), `svc_us` at
the start or end of a system call, and nothing (neither busy nor idle) in the host loop between threads. The frame
subscribers (this log's row and `--state-trace`'s line) are among those hooks. A row is written at the first system
call after the frame change, once the counters are current.

The scheduler counts system calls and services by number and by name, and (since N1-32) by the thread that made the
call: a call's time goes to that thread even when another thread is running by the time it is closed, and the time a
thread spends blocked inside a call is nobody's (it is other threads' work or idle). So `perf_report.py` gives two
bounds of the game thread's time:

- `game_us` = `main_us` + `main_svc_us` + `main_service_us`, the **upper** bound: the game thread's own guest code,
  calls and services, and no other thread's. It still holds the scheduler's host work run at the main thread's
  preemption checks (timer events, posted work, the safe-point hooks: probably tens of microseconds per frame) and at
  the start and end of its system calls (the same, and picking the next thread; no estimate), because the scheduler
  does not count that work apart (D99).
- `main_us` alone, the **lower** bound, up to the scheduler work done at the main thread's preemption checks, which
  it also holds.

It also gives `game_all_us` = `main_us` + `svc_us` + `service_us`, N1-23's upper bound (every thread's calls and
services), as a row of the table and a line under the verdict, for comparison; the verdict does not use it. A v1 log
(from a runner before N1-32) still gets N1-23's report: `game_us` is then `main_us` + `svc_us` + `service_us`. An OVER
verdict whose `main_us` p99 is barely above 4 ms is not conclusive: say so to the orchestrator.

## Which mode: real time, not `--deterministic`

Real time, with the `--native` code library (built with `python tools\build_game.py --dump <your v1.3.0 update .cia>
--native --no-mods --out <folder>`, as *Runs on an idle PC* of `fps-audit.md` did; `Build code library.bat` builds
Azahar's library, which the native runner refuses; not the `_native_det` one). A library built before
`RC_HOST_VERSION` 6 (2026-10-28), such as `build\n3-17-user`, is refused too. Why real time:

- The deterministic mode (N1-25) takes the host out of the clock: its `TimeSource` is virtual, game ticks that
  move with the cycles the library charges, so every counter in it is the cycle model's time, not the CPU's. The
  log then says `clock=virtual` in its header and `perf_report.py` refuses it (NOT COUNTED). Since N3-32 the
  deterministic G3 follows the reference through the village to the hunt at 120 fps (`docs/research/det-120fps.md`),
  and that is why it is the mode for comparing *what the game does*; it cannot say how long the host takes.
- Real time is what a player runs: the cost of the real interleaving, the timer thread's preemptions and the
  host's caches are in the number.

So there is one measurement, in real time. (The price: real time does not repeat. See the script, next.)

## The script: G3 does not hold at 120 fps by frames

The G3 session (`tools/native/golden/G3.txt`) is keyed in game **frames** at 60 fps; at 120 fps its walks cover
half the distance and its talks outrun the text (N1-26, D94), so it cannot run as it is. Use, in this order:

1. **`G3-ms.txt`**, the same session keyed in game milliseconds from a 60-fps run's own timeline (N1-26: made from
   the state trace of the final G3 deterministic run at 60 fps, 19 lines; `docs/native/golden.md`, results of N1-26,
   *120 fps*). On this PC: `build\n1-26\G3-ms.txt`. It reached the hunt in 2 of 5 real-time runs at 120 fps at
   N1-26's base (2026-10-24, D94). That rate has not been measured since. At native `4a6eaf4` the frame-keyed G3 at
   60 fps reached the hunt in 3 of 5 idle real-time runs against N1-26's 10 of 10 (`fps-audit.md`, *Runs on an idle
   PC*), so G3-ms's rate may be lower now. Run it with `--ms 304100`, the game time N1-26 ran it to.
2. If that file is gone, make the frame-keyed one under `build\` (from the repository root):
   `New-Item -ItemType Directory -Force build\n1-23 | Out-Null; python tools\native\fps_audit.py scale tools\native\golden\G3.txt --factor 2 -o build\n1-23\G3-x2.txt`
   (every frame key doubled, milliseconds unchanged; N3-21/N3-22 used it). On the idle re-run at native `4a6eaf4`
   (`fps-audit.md`, *Runs on an idle PC*) it reached the hunt in 1 of 5 real-time runs at 120 fps. That one run
   was partly under an installer's load and would not count under the idle rule below. Of the four fully idle
   runs, none reached the hunt. To use it, in the run block below set `$script = "build\n1-23\G3-x2.txt"` and give
   `--frames 36000` in place of `--ms 304100`.

Both are scripts that wait for nothing, so a run that falls out of step stays in the village from then on. **A
run counts only if its state trace shows the hunt's scene, and `perf_report.py` checks it** (next section): the
CSV of a run that stayed in the village is discarded, not averaged in.

**How many runs.** Run again until **three** runs count, at most **ten** attempts (about five minutes each).
Report every attempt, counting or not (`attempts: N, counted: 3, discarded: N-3`, with the file names): the
discarded ones are information (their rate is the script's, not the runtime's). **If ten attempts give fewer than
three counting runs, stop and ask for a script that waits on the game's own state** (a press when the scene
or the dialog the game shows says so, N1-26 and D94 (4)). A candidate to decide on, not to use before the
orchestrator decides it: G3.txt at 60 fps switched to 120 fps after the quest loads, with `--set fN:fps=120`. Do
not play the hunt by hand, do not lower `--min-frames`, do not use `--deterministic` to get there: none of them is
this measurement.

An attempt is a run of the script that ended with exit status 0. A run that ends otherwise is **not** an attempt of
the script (see the run block); a run that breaks the idle rule is held, not an attempt: note it and run again in
a new folder.

## One run

On an **idle PC**, as D70 asks for a real-time run: no build, test, antivirus scan or other heavy program during
the run and the total CPU load before it at most 25 % (`fps-audit.md`, *Repeating it*); close the browser.
A busy host adds its own time to every column. Hold the emulator lock (`build\locks\emulator`, the folder
`fps_audit.py --lock` uses) for the whole attempt: the block below creates it (waiting while an agent's run holds
it), measures the load with it held, and removes it at the end, even if the run fails.
The default `--gpu null` is right: this measures the CPU side of the game thread; the translation to Vulkan and the
GPU are N2-19's budgets (N2a). From the root of the checkout whose `build\` holds the game folder, the save and the
library (paths as in `fps-audit.md`, *Repeating it*; the runner is `build\native\bin\mh4u_native_runner.exe`, or the
one in the CI artifact `mh4n-runner-windows`):

```
$runner = "build\native\bin\mh4u_native_runner.exe"
$prog   = "build\native-game"
$lib    = "build\n3-22b\lib-rt\recomp\0004000000126300_native.dll"   # the --native library of fps-audit.md's *Runs on an idle PC* (RC_HOST_VERSION 6)
$script = "build\n1-26\G3-ms.txt"
$out    = "build\n1-23\run01"                                         # one folder per attempt: run01, run02, ...
$lock   = "build\locks\emulator"
$env:PATH = (Resolve-Path build\game\toolchain\llvm-mingw-20241217-ucrt-x86_64\bin).Path + ";" + $env:PATH   # the toolchain build_game.py fetched; without it the runner waits on a hidden missing-DLL dialog (not needed with the CI artifact, whose DLLs are beside the exe)
$env:TZ = "UTC0"
while ($true) {                                  # wait while another run holds the lock
    try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null; break }
    catch [System.IO.IOException] { Start-Sleep 30 }
}
try {
    if (Test-Path $out) { throw "$out exists: every attempt, a held one too, takes a new runNN" }
    New-Item -ItemType Directory $out | Out-Null
    $load = (1..20 | ForEach-Object {
        (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
        Start-Sleep 1 } | Measure-Object -Average).Average
    "CPU load over the 20 s before the run: $load %" | Tee-Object $out\load.txt
    if ($load -gt 25) { throw "load $load % is over 25 %: wait for it to drop and start this attempt again (new folder)" }
    Copy-Item -Recurse build\golden\G3\user-before $out\user         # a fresh copy of the save
    & $runner $prog --code $lib --user $out\user --ms 304100 --input-file $script `
        --init-time 1767268800.5 --set 0:fps=120 --io-latency azahar `
        --state-trace $out\state.txt --observable config\native\observable.txt `
        --perf-log $out\perf.csv *> $out\runner.log
    "runner exit $LASTEXITCODE"
} finally {
    Remove-Item $lock
}
```

N1-32's re-run uses `build\n1-32\runNN` (`$out = "build\n1-32\run01"`, ...), with its own attempt count: N1-23's
`build\n1-23\run01` to `run05` are v1 attempts and are not counted with it.

`--init-time 1767268800.5` is G3's own (`# init-time:` in the session). The CSV is `$out\perf.csv`; the state trace
`$out\state.txt` holds the game's state, so it stays under `build\` and is never committed or sent anywhere (the CSV
holds timings only). A run held by the load check leaves its folder behind: use a new `runNN` for the next one.

The runner must have `--perf-log` (N1-23) and be built as the native-windows job builds it (RelWithDebInfo, the
default of `native/CMakeLists.txt`). Before the first attempt, and before you check the idle load (a build is a
heavy program), `& $runner --help | Select-String perf-log` must print the option. If it prints nothing, the
`build\native` is older than the merge, and the run would exit 2 with `unknown option --perf-log` in `runner.log`.
Then rebuild it with `cmake --build build\native --target mh4u_native_runner` (llvm-mingw, cmake and ninja on PATH),
or use the `mh4n-runner-windows` artifact of a push to `native` made after the merge.

A runner from before N1-32 has `--perf-log` too, but writes `# perf v1` (no per-thread columns). So after the first
attempt, check the first line of its CSV: `Get-Content $out\perf.csv -TotalCount 1` must start with `# perf v2`. If it
does not, that run is not an attempt (neither counting nor discarded): rebuild the runner as above (or take a newer
artifact) and run again in a new folder. Every counted `perf.csv` starts with `# perf v2`.

The block prints `runner exit N`:

- 0: read the run (next section).
- 2 (loading failed): a setup error (a path, the library). A library the runner refuses (Azahar's, a deterministic
  one, or one built for another `RC_HOST_VERSION`) ends the run at boot with this status and `runner.log` gives the
  reason: rebuild it with `--native` at this tree. It is not an attempt: fix it and run again in a new folder.
- 3 (a runtime error) or 4 (the recompiled code stopped): stop and give the orchestrator this run's folder and
  `runner.log` (it is not an attempt of the script). It is a fault of the runtime, not a miss of the script.

## Reading a run

```
python tools\native\perf_report.py $out\perf.csv --state-trace $out\state.txt --skip 600 --fps 120 > $out\report.md; "perf_report exit $LASTEXITCODE"
```

- `--state-trace` keeps the rows of the frames the trace shows the hunt's scene (aQuest, `0x00e655c8`; `--hunt-scene`
  names another) and prints `Hunt: N rows ...`. It takes the native runner's log only. `--skip 600` leaves out the
  first 5 s of the hunt (the stage still loading and settling). The run **counts** if `perf_report`'s exit status is
  0 and `report.md` holds the table. With exit status 1, `report.md` holds one line, `NOT COUNTED: ...`, with the
  reason: the hunt was never reached, fewer than `--min-frames` (default 2400, 20 s at 120 fps) rows were left, or
  the log is in virtual time or does not say `clock=host`; discard it, note the reason. With exit status 2 (a message
  on the terminal, `report.md` empty) it is a usage or file problem, not a verdict: fix it and read the run again.
- Under the table the report gives the verdict:
  `Game thread, p99 against its budget of 4.000 ms: game_us (upper bound, its own calls and services) X ms, main_us (lower bound) Y ms: V`.
  **WITHIN**: even the upper bound's p99 is within 4 ms. **OVER**: even the main thread alone is over it.
  **UNDECIDED**: 4 ms lies between the two bounds. The other threads' calls and services are no longer in `game_us`,
  so what decides then is how much of `game_us` is the scheduler's host work run at the main thread's preemption
  checks and system calls: say so and give the margin (`game_us` p99 minus 4 ms), do not pick a side. Counting that
  host work apart is the next step (D99). It is the maintainer's decision: the orchestrator opens it, with the margin,
  and does not settle it.
- The next line, `With every thread's calls and services (game_all_us, N1-23's upper bound), p99: W ms`, is N1-23's
  upper bound, for comparison with N1-23's runs; it takes no part in the verdict.
- The line after it, `Every guest thread together (busy_us ...)`, is the p99 of every guest thread's counted work on the
  one host thread (the scheduler's host loop is in no column): it is not the game-thread figure, but it is about
  what a single host thread would have to do in a frame (8.333 ms at 120 fps), and it shows how much the other
  threads add.
- The table's `present_us` should cluster at 8.333 ms (the frame limiter) and `idle_us` is the headroom.

**The verdict over the three runs** is the worst of them: the largest p99 of `game_us` (and of `main_us`) of the
three counting runs. If that verdict is UNDECIDED, the margin is the largest `game_us` p99 of the three minus 4 ms.
Give the orchestrator, once for the session: the output of `& $runner --version` (with the CI run's URL if you use
the artifact `mh4n-runner-windows`) and the `$lib` path with its date (`(Get-Item $lib).LastWriteTime`). Per run: the
folder name, the first line of `perf.csv` up to and including the date (`# perf v2 YYYY-MM-DD`), the CPU load before it (`load.txt`; D70: at most 25 %), the `Hunt:` and `Frames:` lines, the p99
of `game_us`, `main_us`, `game_all_us` and `busy_us`, the `Frames over 50 ms` line, and the verdict line. For all the
attempts: the count above. Send `report.md` and the CSVs of the counting runs; keep the rest.

## Overhead of the log

Without `--perf-log` the boot step returns on its first line: nothing is subscribed to `Runtime::OnFrame` or to the
system call hooks, no file or object is made, and the SVC layer and the code library are not touched. The counters
are the scheduler's, kept always, with or without the option. Since N1-32 the scheduler charges each closed stretch of
system call or service time to the thread that made the call as well: one more addition, through a pointer the
thread's scheduler data caches on its first charge of that kind, wherever a stretch of system call or service time is
closed (the end of a system call, the start and end of a service request, and a thread switch inside either). Only a thread's
first system call and first service request allocate (one map entry each); entries are never erased. `perf_test` boots
a program without the option and checks that no file appears and the frame subscribers see the same frames. With the
option, the cost is one call per game frame (120 a second) that reads the counters (a sum over the 256 system call
slots, the services and the threads, and two look-ups of the main thread: a few hundred additions) and one buffered
line of about 85 bytes (the file is written in 64 KB blocks, so a write to disk happens every few seconds), plus one
test of an empty list at each system call. Nothing else of the log's is added to a preemption check, a system call or a fiber
switch: the row is written from a system call's `before` hook, at the first system call after the frame change.
