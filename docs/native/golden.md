# Golden sessions

A golden session is a frame-keyed input script plus the recording of what the reference runner (the Azahar-based
`mh4u_runner`, Old 3DS) did with it. Later work packages check a candidate runtime against the recordings
(`tools/native/golden.py check`). The recordings are game data: they live in `build/golden/<S>/` and never in the
repository.

> **Status (N0-09).** G1, G2 and G6 are recorded with the Windows build of the reference runner (CI run 39075253705,
> native `e6cd840`), `--cpu recomp`, and every recording passes its own checkpoints and its capture checks. The
> reference runner is **not reproducible** on that PC, so `golden.py check` against the same runner does not pass yet:
> see [Reproducibility](#reproducibility-of-the-reference-runner). The `ipc` comparison of G1 passes in some runs and
> fails in others; `state`, `checkpoints` and `hid` (all sessions) and `images` (G1) were stable in the runs made, and
> `svc` failed in every one.

## Files

| Path | What |
|---|---|
| `tools/native/golden/<S>.txt` | the session: header lines and input events (the runner's `--input-file` format) |
| `tools/native/golden.py` | `list`, `record`, `check` |
| `build/golden/<S>/` | one recording (the shared checkout's `build/`, not a worktree's) |

### Session file

```
# name: <id>                    must equal the file name
# needs: none | save
# frames: <total>               the run stops here (--frames)
# checkpoints: <frame>:<scene>, ...     scene = the observable `scene` value (0x.. or decimal)
# captures: gpu:<start>:<frames>, dsp:<start>:<frames>, ...    (gpu at most 30 frames)
# draft: yes                    optional: placeholder numbers; `record` refuses the file
f600:START                      events: f<frame>:BUTTON[:hold], f<frame>:TOUCH:x:y[:hold], f<frame>:CIRCLE:x:y[:hold]
```

Event times are frames (`f<N>`, the game's frame counter), never milliseconds, so a script means the same on every CPU
backend and runtime. Several events may share a line, separated by commas. The scripts are only button presses and
touches: they may be committed. A checkpoint `<frame>:<scene>` holds when the first state-trace record at or after that
frame has that `scene`.

### A recording

| File | What |
|---|---|
| `user-before/`, `user-after/` | the user folder (and the save, if given) before the run, and as the run left it |
| `svc.txt`, `ipc.txt` | the SVC trace and the full IPC trace (`--trace-ipc-full`) |
| `state.txt`, `unmapped.txt` | the state trace and the unmapped-access report |
| `hid.mhid`, `dsp-<start>.mdsp` | the HID capture and each DSP capture |
| `gpu-<start>.mgpu` (with `.json` and `.frames/`) | each GPU capture |
| `shots/` | screenshots (`--shot-every`, default 1000 ms of emulated time; names are the emulated millisecond) |
| `runner.log` | the runner's output |
| `manifest.json` | the runner and code library used (with SHA-256), the time zone, the arguments, the checkpoint results, the capture problems, and size and SHA-256 of every file above |

`--shot-every` counts **emulated milliseconds**: the runner has no frame-keyed screenshot option, so a screenshot is
taken at a time, not at a game frame. The spec table calls the screenshots frame-keyed, but they are themselves exposed
to the reference runner's timing noise (see [Reproducibility](#reproducibility-of-the-reference-runner)): the same
millisecond can fall on a different game frame in two runs.

`check` runs into `build/golden/<S>/check/run<N>/` and writes `check/result.json`.

## Commands

```
golden.py list [--out DIR]
golden.py record S --runner RUN --game CXI --update CIA --user DIR [--code LIB] [--cpu jit|recomp] [--save DIR]
golden.py check  S --runner RUN --game CXI --update CIA [--code LIB]
                   [--candidate-runner RUN2] [--candidate-program PATH] [--candidate-code LIB]
                   [--candidate-args "..."] [--repeat N] [--ignore FILE]
```

The recordings of this document were made with:

```
python tools/native/golden.py record <S> --runner build/runner-win/mh4u-port-windows/mh4u_runner.exe \
    --game baserom/juego.cxi --update baserom/update_v130.cia --user build/userbase \
    --code build/game/recomp/0004000000126300.dll --cpu recomp --out build/golden
```

`build/userbase` is a user folder with the 1.3.0 update installed and no save. `--cpu recomp` because the recompiled
code is what the native runtime is compared with; `jit` is no more reproducible than `recomp` (see below).

- `RUN` and `LIB` are paths: `mh4u_runner.exe` and a `.dll` on Windows, `mh4u_runner` and a `.so` on Linux. A runner
  that is a `.py` script is started with the Python interpreter (the tests use that).
- `record` copies the user folder (and the save, merged over it) to `user-before/`, runs the runner on a working copy
  with `--new-3ds off --frames <total> --input-file <session>` and every recorder on, keeps the working copy as
  `user-after/`, validates the binary captures, evaluates the checkpoints against `state.txt` and writes `manifest.json`.
  It exits 1 if the runner failed, a capture is malformed or a checkpoint did not hold.
- `check` verifies the recording against its manifest, then runs again on a copy of `user-before/` and compares:
  `compare_svc_traces.py --normalize --outputs`, `compare_ipc_traces.py` (with `--ignore
  config/native/ipc_time_dependent.txt` when the file exists, plus any `--ignore`), `state_diff.py`, the checkpoints,
  `capfmt.py diff-hid`, `capfmt.py diff-dsp` for each DSP capture, and SHA-256 of the screenshots.
- A candidate gets its own program (`--candidate-program`; for the native runner the `prepare_game.py` folder) and code
  library (`--candidate-code`) and only the shared options: `--user`, `--frames`, `--input-file`, `--set`,
  `--trace-svc`, `--trace-ipc`, `--trace-ipc-full`, `--state-trace`, `--capture-hid`, `--capture-dsp`, plus
  `--candidate-args`. A comparison whose candidate file was not written is reported as **skipped**, not passed; for the
  reference runner it is a failure.
- `--repeat N` runs the run N times and also requires the state traces (header lines aside) to be identical.
- GPU captures are not compared by `check` (that is the capture player's job, N2).

## The software keyboard

The runner's keyboard is **not** answered by the runner: the game opens it, and it stays open until a touch presses its
`OK` key (the config's user name is not entered). The sessions therefore type four letters with touches and press
`OK` at the bottom right of the touch screen. Typed text is part of the input script, so every runtime sees the same
touches. The native keyboard (N1-14) has to take the same touches.

Touch coordinates used (pixels of the 320x240 touch screen): the first row of letter keys at y = 115 and the second at
y = 136, `OK` at (236, 228), the `Confirm` button of the creation menu at (285, 183).

## First start

The user folder has no save, so every session starts with five first-start prompts, then a notice and the intro movie.
The sessions answer them with `A` at frames 300, 450, 600, 750 and 900 (the prompts), then 1050, 1200 and 1350 (the
notice and the movie). The title is on screen from about frame 1360 and waits for `A`. (`docs/native/wp/N0.md` says "from game
frame 1200 on": that is the intro movie without input; with the presses above the title is at 1360.)

## Sessions

The `scene` observable (`config/native/observable.txt`) is `0x00` from frame 1 to the end of every session, and
`quest_id` is `-`: it never changes outside a quest. The checkpoints below are therefore all `<frame>:0x00` and can only
show that the run reached that frame with the scene unchanged; the discriminating checks are `state_diff.py` on the
other values and the screenshots. A richer observable is a decision for `config/native/observable.txt`.

### G1: power-on to the title

Frames: 1700. Input: the eight `A` presses of [First start](#first-start). Checkpoints: 1450:0x00, 1700:0x00. Captures:
`gpu:1500:3` (frames 1500 to 1502 of the title) and `dsp:1500:300` (300 DSP audio frames are game frames 1500 to 1587).
The title is stable from 1360 on (it waits for `A`), so the captures start 140 frames after it appears.
Recording: 60 s of host time, about 337 MiB (the GPU capture is 144 MB).

The title is **silent**: the DSP capture has 300 `REGIONS` and 300 `PCM` records, 0 non-silent PCM frames and no source
enabled (`sources_enabled_max: 0`, no type-3 records). The same holds at game frame 2300 on the same screen.

Where audio is there, measured on one more run of G1's script with `--capture-dsp 100:6000:<file>` (6000 DSP frames
from game frame 100, which cover game frames 100 to 1689; `--frames 1720`, `--cpu recomp`, `TZ=UTC0`, a fresh copy of the
user folder): 6000 `REGIONS`, 6000 `PCM` and 236 type-3 (`SAMPLES`) records, 1198 non-silent PCM frames, all of them
between game frames 300 and 1350, and at most one source enabled at a time (1204 `REGIONS` records have one). The
non-silent frames come in six stretches, 300-327, 451-467, 601-625, 751-772, 901-918 and 1234-1350, so the sound is
there from about frame 300 on and not at the title (none from 1360 on), and **none in game frames 1000 to 1087**. The
exact stretches vary between runs (the reference runner is not reproducible, below): treat the frames above as the
range of one run. Two consequences for other work packages:

- the item of N0-06 that asks for non-silent PCM "at G1's title" cannot hold, because the title (from about frame 1360)
  is silent; non-silent PCM has to come from a DSP capture over game frames 300 to 1350 of G1 (not part of this session's
  captures, which start at 1500), or from G2;
- G1 never opens `ir:USER` (its IPC trace has no request of a session named `ir:`), so no type-2 (`IR`) records can appear
  in its HID capture.

### G2: new game to the first control

Frames: 9000. Checkpoints: 1450:0x00, 2500:0x00, 3600:0x00, 8300:0x00, 9000:0x00. Capture: `gpu:2500:3`, in character
creation, with the name already entered and the creation menu showing.
Recording: 1100 s of host time, about 1245 MiB (323 files).

| Frames | What |
|---|---|
| 300 to 1350 | first-start prompts, notice, intro movie (as G1) |
| 1500, 1650 | `A`: title to menu, menu to new game |
| 1800 | `A` on "Name": the software keyboard opens |
| 1950 to 2100 | four touches on letter keys; 2400: `OK`; the creation menu is back |
| 2600 | touch `Confirm`; 2800: `A` answers the question that follows (second creation step) |
| 3000 | `A` on the second name entry: the keyboard opens again (G6) |
| 3250 to 3350 | three letters; 3550: `OK`; 3750: `Confirm`; 3950: `A` |
| 4150 to 7900 | `A` presses through the creation steps and the opening, every 150 frames |
| about 8240 | the first control |
| 8700 | circle pad forward for 60 frames |

### G6: the software keyboard again

Frames: 3700. Checkpoints: 1450:0x00, 2500:0x00, 3700:0x00. No capture (the spec asks for none). The first 3550
frames of G2: the keyboard is used for the first name (frames 1800 to 2400) and again for the second (3000 to 3550),
and the run stops on the creation screen it was opened from. Recording: 282 s of host time, about 575 MiB.

## Numbers from `gcap.py summary` of G1's capture

`gcap.py summary build/golden/G1/gpu-1500.mgpu`, frames 1500 to 1502, 3 `FRAME` records:

| Record | Count |
|---|---|
| `INIT` | 1 |
| `MEM` | 9 |
| `CPU_PAGES` | 23 |
| `GX` | 17 (`SUBMIT_CMDLIST` 6, `DISPLAY_TRANSFER` 6, `CACHE_FLUSH` 5) |
| `PICA_REG` | 6 |
| `GPU_PAGES` | 12 |
| `SWAP` | 6 |
| `IRQ` | 18 |
| `FLUSH` | 14 |
| `FRAME` | 3 |

Interrupts and the delays the reference gave them (nanoseconds):

| Interrupt | Id | Count | Delay min / median / max |
|---|---|---|---|
| PDC0 (top screen vertical blank) | 2 | 3 | 0 / 0 / 0 |
| PDC1 (bottom screen vertical blank) | 3 | 3 | 0 / 0 / 0 |
| PPF (display transfer done) | 4 | 6 | 92592 / 92592 / 92592 |
| P3D (command list done) | 5 | 6 | 49590 / 50454 / 50716 |

Per frame that is one `PDC0` and one `PDC1`, two `PPF` (one display transfer per screen) and two `P3D` (two command
lists). Fog modes set: none (3 times). The DSP capture of the same session has `dsp_counter_first` 5701 and
`dsp_counter_last` 6000; the HID capture has 1701 records, one per frame from 0 to 1700, with buttons on 48 of them
(`A` only).

## Time zone

Azahar's shared page turns the start date into the 3DS clock through the host's local time zone, so recordings are made
with `TZ=UTC0` in the child's environment (`record` and `check` set it; `manifest.json` has `"tz": "UTC0"`).

**The Windows (mingw) runner honours `TZ`.** Three runs of the first 200 frames, dumping the shared page
(`--dump f100:1FF81000:0x100`) with `TZ=UTC0`, with `TZ` unset (this PC: UTC-8) and with `TZ=PST8PDT` gave a date
field in the shared page that is 28,800,000 ms (8 hours) lower for the last two than for the first. Two runs with
`TZ=UTC0` differ in the same field only by about 0.9 s, which is host timing (below), not time zone. G1's state, SVC and IPC
traces recorded with `TZ=UTC0` and without it agree as much as two `TZ=UTC0` runs do: they do not differ because of the
time zone (the date does not reach the state or the IPC of G1), so the time zone cannot be told apart from the run-to-run
noise there. Keep `TZ=UTC0`: it is what makes the clock the game reads the same on every machine.

## Reproducibility of the reference runner

On this PC (Windows 11, the CI build of the runner) two runs with identical arguments are **not** identical, for
`--cpu recomp` and for `--cpu jit`, with and without the GPU capture and with `TZ=UTC0`. Measured on G1 (two runs each,
no screenshots):

- the state trace: the same scenes, `frame_counter` and `stage_loader_state` in every record; the tick count `t` of
  a record differs from some frame on (recomp: first at frame 468 by 8 ticks; jit: first at frame 29), 100 to 120 of 1701
  records; `smain_delta`, the game's own frame delta (`ignore` in the observable list), differs in about 500 to 700;
- the SVC trace: the first difference is in the registers that carry tick values around `svc 0x28`
  (GetSystemTick, which takes no register arguments), and later threads make different numbers of calls
  (one thread 108 against 97; the main thread 1014391 against 1021315);
- the IPC trace: equal in some pairs of G1 runs (a `jit` pair, and one `recomp` pair) and different in others (a
  thread one request short); for G2 one `fs:USER` write has a different CRC and two threads have one
  request more or fewer;
- the clock the game reads: the shared page's date at game frame 100 differs by about 0.9 s between two identical runs,
  so the game seeds its random numbers and times its loads from a clock that is not the fixed start date alone.

`golden.py check` against the same runner, on the recordings of this document:

Each row is a **sample** of the runs made so far (the author's, and a reviewer's re-run of G1), not a fixed result:

| Session | Passed in the sample | Failed in the sample | Not stable between runs |
|---|---|---|---|
| G1 | state, checkpoints, hid, dsp-1500, images | svc | **ipc** (passed in the author's run, FAILED in the reviewer's re-run) |
| G6 | ipc, state, checkpoints, hid | svc, images (13 of 130 screenshots differ) | ipc and images are one sample each |
| G2 | state, checkpoints, hid | svc, ipc, images (50 of 308 differ) | ipc and images are one sample each |

Across the runs made so far, **stable** comparisons: `state`, `checkpoints` and `hid` (all three sessions), and
`images` for G1. **Not stable**: `ipc` of G1 (it passes in some runs and fails in others, as the IPC paragraph above
says); `svc` failed in every run made and is not expected to pass until time is fixed; the G6 and G2 columns have one
sample each, so for them `ipc` and `images` are not known to be stable either way. `dsp-1500` of G1 passed in the
samples (a silent title, so it compares little).

The comparisons were not loosened to make them pass. A `check` against a candidate runtime needs the same thing the
roadmap already asks for: the deterministic mode (N1-25) for exact comparisons, and for the reference either a run on an
idle host with the shared page's clock fixed (a change in `port/`, not in this work package) or rules for the registers
and requests that carry time (`config/native/svc_unused_registers.txt`, `ipc_time_dependent.txt`: proposed to the
orchestrator, not edited here). Until then `check` against the reference runner is a measure of how much of a run is
host-timing noise, and `state_diff.py` (scenes within two frames, `eq` values equal), the checkpoints and the HID
capture are the comparisons that hold.
