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
`perf_log` before every run (one file per run).

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
