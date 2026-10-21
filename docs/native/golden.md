# Golden sessions

A golden session is a frame-keyed input script plus the recording of what the reference runner (the Azahar-based
`mh4u_runner`, Old 3DS) did with it. Later work packages check a candidate runtime against the recordings
(`tools/native/golden.py check`). The recordings are game data: they live in `build/golden/<S>/` and never in the
repository.

> **Status (N0-09).** G1, G2 and G6 are recorded with the N0-29 runner (the Windows CI build of
> `wp/N0-29-runner-exact` at `3aef9b8`), `--cpu recomp`, from the user folder `build/userbase-cfg` (see
> [The user folder](#the-user-folder)), and the `scene` observable of N0-27 (D32). Every recording passes its own
> checkpoints and capture checks, and `golden.py check <S> --repeat 2` against the same runner **exits 0 in the `same`
> profile for all three**: every text trace is identical apart from its first line, and the HID, DSP and GPU
> captures, the screenshots and the shared-page dump are identical by SHA-256. So a difference between two runs of
> the reference is a determinism regression (D30), not noise. See [Results](#results) for the numbers and
> [History](#history-the-wip-recordings-before-n0-29) for the measurements that led to D30 to D36.

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
# captures: gpu:<start>:<frames>, dsp:<start>:<frames>[:silent], ...    (gpu at most 30 frames)
# draft: yes                    optional: placeholder numbers; `record` refuses the file
f600:START                      events: f<frame>:BUTTON[:hold], f<frame>:TOUCH:x:y[:hold], f<frame>:CIRCLE:x:y[:hold]
```

Event times are frames (`f<N>`, the game's frame counter), never milliseconds, so a script means the same on every CPU
backend and runtime. Several events may share a line, separated by commas. The scripts are only button presses and
touches: they may be committed. A checkpoint `<frame>:<scene>` holds when the first state-trace record at or after that
frame has that `scene`. `:silent` marks a DSP capture that is expected to be silent (a capture that is not marked must
have sound, see [Capture problems](#capture-problems)).

### The scene

`scene` (`config/native/observable.txt`, `docs/research/scene-observable.md`, N0-27, D32) is **the class of the topmost
game area**: the vtable of the last `cArea` object in the area table of the `sArea` instance, a code address that is the
same in every runtime. Each session has a checkpoint in every scene it passes through that lasts more than 100 frames:

| Scene value | Class | Frames (G2, `--cpu recomp`) | What |
|---|---|---|---|
| `0x00e64d94` | `aInit` | 1 | boot |
| `0x00e6c100` | `aGameSys` | 2 to 35, and 1 to 3 frames between areas | the system area |
| `0x00e64d5c` | `aDemo` | 36 to 1651 | the intro movies and the title |
| `0x00e6c138` | `aNewGame` | 1652 to 4954 | character creation, both name entries |
| `0x00e6c170` | `aVillage` | 4958 to 5083 | the village |
| `0x00e655c8` | `aQuest` | 5086 to the end | the opening and the first control |

The scenes that last 100 frames or fewer (`aInit`, `aGameSys`, and the 1 to 3 frames of `aDemo` and `aGameSys` between
the creation and the village) have no checkpoint. A change of scene is *real* when it goes between two readable values
(N0-25's definition, `state_diff.real_scene_changes`): the first record and the change from `-` to a value are not.

### A recording

| File | What |
|---|---|
| `user-before/`, `user-after/` | the user folder (and the save, if given) before the run, and as the run left it |
| `svc.txt`, `ipc.txt` | the SVC trace and the full IPC trace (`--trace-ipc-full`) |
| `state.txt`, `unmapped.txt` | the state trace and the unmapped-access report |
| `timing.txt` | the CoreTiming events (`--trace-timing`), the first thing to look at when two runs differ (information in `check`, never gated) |
| `hid.mhid`, `dsp-<start>.mdsp` | the HID capture and each DSP capture (each with its `.json`) |
| `gpu-<start>.mgpu` (with `.json` and `.frames/`) | each GPU capture |
| `shots/` | frame-keyed screenshots, `f<frame>_top.ppm` and `f<frame>_bottom.ppm` (`--shots-from f300 --shot-every f60`; `--shot-every-frames N`, default 60, 0 for none), and the shared-page dump `mem_f0000300_1ff81000.bin` (`--dump f300:1FF81000:0x1000`: the 4 KiB shared page at frame 300, which holds the console's clock and its fixed MAC address) |
| `runner.log` | the runner's output |
| `manifest.json` | the runner and code library used (with SHA-256), the time zone, the arguments, the checkpoint results, the capture problems, the number of real scene changes in `state.txt`, whether `user-before/` holds the config savegame, and size and SHA-256 of every file above |

`check` runs into `build/golden/<S>/check/run<N>/` and writes `check/result.json`.

## Commands

```
golden.py list [--out DIR]
golden.py record S --runner RUN --game CXI --update CIA --user DIR [--code LIB] [--cpu jit|recomp] [--save DIR]
                   [--shot-every-frames N]
golden.py check  S --runner RUN --game CXI --update CIA [--code LIB]
                   [--candidate-runner RUN2] [--candidate-program PATH] [--candidate-code LIB]
                   [--candidate-args "..."] [--repeat N] [--profile same|cross] [--fallback]
                   [--require NAMES] [--ignore FILE]...
```

The recordings of this document were made with (PowerShell, from the worktree; `TZ` is set by `golden.py`):

```
python tools/native/golden.py record <S> --runner build/runner-win-n029/mh4u-port-windows/mh4u_runner.exe `
    --game baserom/juego.cxi --update baserom/update_v130.cia --user build/userbase-cfg `
    --code build/game/recomp/0004000000126300.dll --cpu recomp --out build/golden
python tools/native/golden.py check <S> --repeat 2 --require svc,ipc,state,checkpoints,hid,images,unmapped `
    --runner ... --game ... --update ... --code ... --out build/golden        # G1 also ,dsp
```

- `RUN` and `LIB` are paths: `mh4u_runner.exe` and a `.dll` on Windows, `mh4u_runner` and a `.so` on Linux. A runner
  that is a `.py` script is started with the Python interpreter (the tests use that).
- `record` copies the user folder (and the save, merged over it) to `user-before/`, runs the runner on a working copy
  with `--new-3ds off --install <update> --frames <total> --input-file <session>`, `--observable`, every recorder, the
  screenshots and the shared-page dump, keeps the working copy as `user-after/`, validates the captures, evaluates the
  checkpoints against `state.txt` and writes `manifest.json`. `--cpu recomp` (the default) needs `--code`.
  It exits 1 if the runner failed, a capture problem was found or a checkpoint did not hold.
- `check` verifies the recording against its manifest, then runs again on a copy of `user-before/` and compares.

### Capture problems

`record` exits 1 (and the manifest's `capture_problems` says why) for:

- a capture `gcap.py check` (GPU) or `capfmt.py` (HID, DSP) rejects, or one that was not written;
- a DSP capture not marked `:silent` that has no non-silent PCM frame, or no type-3 (`SAMPLES`) record;
- an IPC trace with an `ir:USER` request whose HID capture has no type-2 (`IR`) record from that request's frame on;
- checkpoints that do not hold;
- for G2: fewer than 2 real scene changes in `state.txt`, or checkpoints that hold fewer than 3 distinct scene values.

### Who runs, and what is compared

| `check` given | Who runs | Profile |
|---|---|---|
| nothing | RUN with the recording's arguments | `same` |
| `--candidate-args "..."` only | RUN with the recording's whole argument list (`--new-3ds off`, `--install`, every recorder, `--count-unmapped`, `--trace-timing`, the screenshots and the shared-page dump) plus those options; `--code` only when the run's `--cpu` (the one in `--candidate-args`, else the recording's) is `recomp` | `same` unless `--candidate-args` changes `--cpu`, then `cross` |
| `--candidate-runner RUN2` (with `--candidate-program PATH`, `--candidate-code LIB`) | RUN2 with its own program and library and only the options both runners share: `--user`, `--frames`, `--input-file`, `--set`, `--trace-svc`, `--trace-ipc`, `--trace-ipc-full`, `--state-trace`, `--capture-hid`, `--capture-dsp`, plus `--candidate-args` | `cross` |

`--profile same|cross` forces one. `--candidate-program` and `--candidate-code` need `--candidate-runner` (usage error otherwise).

**`same`** (D31; two runs of one backend; exact, no tolerance):

| Comparison | Rule |
|---|---|
| `svc`, `ipc`, `state`, `unmapped` | identical apart from their first line (the header; `#` lines after it, such as `# thread` lines, are content) |
| `hid`, `dsp-<start>`, `gpu-<start>`, `images`, `shared-page` | identical by SHA-256 (a capture with its `.json` and `.frames/`; every `.ppm` of `shots/`; the dump) |
| `checkpoints` | the candidate's `state.txt` holds the session's checkpoints |
| `timing` | information: the same rule as `svc`, never gated |

**D30's fallback** (`--fallback`, `same` and the reference runner only; for the case that N0-29 had ended `needs-decision`, which it did not):
`svc` with `compare_svc_traces.py --normalize --outputs --collapse-polling config/native/svc_polling.txt`; `state` with
`state_diff.py --tolerance 0 --min-changes <the recording's real scene changes>` (skipped when that is 0); `ipc` with
`compare_ipc_traces.py` and no `--ignore`; the rest as in `same`.

**`cross`** (D31; two timing models: `jit` against `recomp`, the reference against the native runtime):

| Comparison | Rule |
|---|---|
| `state` | `state_diff.py --min-changes <the recording's real scene changes>` (default tolerance 2); skipped when the recording has none: nothing to align |
| `checkpoints` | as in `same` |
| `ipc` | `compare_ipc_traces.py --ignore config/native/ipc_time_dependent.txt` plus every `--ignore FILE` |
| `svc` | `compare_svc_traces.py --normalize --outputs --cross-model config/native/svc_cross_model.txt` (N0-25) |
| `svc-full`, `images` | run and printed as `INFO`, never gated (the image threshold comes in N2b) |
| `hid`, `dsp-<start>` | `capfmt.py diff-hid`, `diff-dsp` |

`--ignore FILE` (repeatable) adds IPC ignore files to the `cross` profile only; with the `same` profile it is a usage
error, so a same-model check can never be loosened by a list.

A comparison whose input the candidate did not write is **skipped**, not passed (a failure for the reference runner).
`--repeat N` runs the candidate N times; each run is compared with the recording in the chosen profile, and runs 2 to N
are compared with run 1 in the `same` profile (`run2 against run1: svc`).

### Exit status of `check` (D34)

| Status | When |
|---|---|
| 0 | every gated comparison passed and at least one ran |
| 1 | a comparison failed (a failure wins over 3) |
| 2 | a usage problem |
| 3 | no gated comparison ran, or a comparison named in `--require` was skipped or did not run |

`--require` takes comma-separated names: `svc`, `ipc`, `state`, `checkpoints`, `hid`, `dsp`, `images`, `unmapped`
(`dsp` covers every DSP capture; a session without one never meets it). `INFO` results are never counted.

## The user folder

Every recording starts from `build/userbase-cfg`: a user folder with the 1.3.0 update installed, no save, and **the
config savegame** (`nand/data/<id>/sysdata/00010017/00000000/config`) that one default-mode run of the N0-29 runner
makes in a folder that has none. The runner gives that savegame a fixed console unique ID (`port/README.md`,
*Determinism*), and the game's save of G2 holds a value derived from it. N1's D37 gives a fresh folder of the native
runtime a random ID and says goldens and replays start from folders that already hold a config; so a recording's
`user-before/` holds the config savegame, and `record` warns (and writes `user_config_savegame: false` in the manifest)
when it does not. `build/userbase-cfg` was made by one short run of the N0-29 runner on a copy of `build/userbase`
(the same folder without the savegame); a G1 run from either folder gave the same 14 artifacts (the orchestrator's
check), and `golden.py check` from `userbase-cfg` passes (below). The comparison of user folders of the runner leaves
`log/` out (D49).

## The software keyboard

The runner's keyboard is **not** answered by the runner: the game opens it, and it stays open until a touch presses its
`OK` key (the config's user name is not entered). The sessions therefore type four letters with touches and press
`OK` at the bottom right of the touch screen. Typed text is part of the input script, so every runtime sees the same
touches. The native keyboard (N1-14) has to take the same touches.

Touch coordinates used (pixels of the 320x240 touch screen): the first row of letter keys at y = 115 and the second at
y = 136, `OK` at (236, 228), the `Confirm` button of the creation menu at (285, 183).

## First start

The user folder has no save, so every session starts with eight `A` presses the game asks for: five first-start
prompts, then a notice and the intro movie. The sessions answer them with `A` at frames 300, 450, 600, 750 and 900 (the
prompts), then 1050, 1200 and 1350 (the notice and the movie). The title is on screen from about frame 1360 and waits
for `A`. (`docs/native/wp/N0.md` once said "from game frame 1200 on": that is the intro movie without input; with the
presses above the title is at 1360.)

## Sessions

### G1: power-on to the title

Frames: 1700. Input: the eight `A` presses of [First start](#first-start). Checkpoints: `1450:0x00e64d5c` and
`1700:0x00e64d5c` (`aDemo`, the only scene of G1 that lasts more than 100 frames). Real scene changes: 2 (frame 2
`aGameSys`, frame 36 `aDemo`). Captures: `gpu:1500:3` (frames 1500 to 1502 of the title) and `dsp:1240:300`.
Recording: 64 s of host time, 341 MiB (the GPU capture is 138 MiB, the SVC trace 155 MiB).

The title is **silent**; the sound of G1 is before it (the measurement is in [History](#history-the-wip-recordings-before-n0-29):
non-silent PCM between game frames 300 and 1350, in six stretches, the last 1234 to 1350). The DSP capture starts at game
frame 1240, inside that last stretch, so its 300 DSP frames (game frames 1240 to 1283, DSP frame counters 4434 to 4733)
all fall inside the audio: 300 `REGIONS`, 300 `PCM` (all 300 non-silent) and 88 type-3 `SAMPLES` records, at most one
source enabled. The capture is not marked `:silent`. Two consequences for other work packages:

- non-silent PCM comes from `dsp-1240.mdsp` (the item of N0-06 that asked for it "at G1's title" could not hold: the
  title is silent);
- G1 never opens `ir:USER` (its IPC trace has no request of a session named `ir:`), so no type-2 (`IR`) records can appear
  in its HID capture.

### G2: new game to the first control

Frames: 9000. Checkpoints (one in each scene that lasts more than 100 frames): `1450:0x00e64d5c` (title),
`2500:0x00e6c138` and `3600:0x00e6c138` (creation), `5000:0x00e6c170` (village), `5500:0x00e655c8`, `8300:0x00e655c8` and
`9000:0x00e655c8` (opening): four distinct scene values. Real scene changes: 8 (frames 2, 36, 1652, 4955, 4957, 4958, 5084, 5086). Capture: `gpu:2500:3`, in character
creation, with the name already entered and the creation menu showing. Recording: 1198 s of host time, 1276 MiB in 309
files (the SVC trace is 1008 MiB, the GPU capture 139 MiB).

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

The scene changes of the recording, by frame and class: 2 `aGameSys`, 36 `aDemo`, 1652 `aNewGame`, 4955 `aDemo`, 4957
`aGameSys`, 4958 `aVillage`, 5084 `aGameSys`, 5086 `aQuest`. The steps at 4955, 4957 and 5084 are real states (the area
system pops the creation area, then `aDemo`, then `aVillage`), 1 to 3 frames long: they are part of the recorded
sequence and have no checkpoint. The first control is in the opening, whose area is `aQuest` (the screenshots of
frame 8400 and after show the controllable character): it is not in `aVillage`, which is on screen for 126 frames only,
before the opening.

### G6: the software keyboard again

Frames: 3700. Checkpoints: `1450:0x00e64d5c`, `2500:0x00e6c138`, `3700:0x00e6c138`. Real scene changes: 3 (frame 2
`aGameSys`, 36 `aDemo`, 1652 `aNewGame`). No capture (the spec asks for none). The first 3550 frames of G2: the keyboard is
used for the first name (frames 1800 to 2400) and again for the second (3000 to 3550), and the run stops on the creation
screen it was opened from. Recording: 306 s of host time, 590 MiB (the SVC trace is 533 MiB).

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
lists). Fog modes set: none (3 times). The HID capture of G1 has 1701 records, one per frame from 0 to 1700, with buttons
on 48 of them (`A` only). These are the same numbers as on the WIP recording: the interrupt delays are constants of the
GPU model.

## Time zone

Azahar's shared page turns the start date into the 3DS clock through the host's local time zone, so recordings are made
with `TZ=UTC0` in the child's environment (`record` and `check` set it; `manifest.json` has `"tz": "UTC0"`; the N0-29
runner sets it too).

**The Windows (mingw) runner honours `TZ`.** Three runs of the first 200 frames, dumping the shared page
(`--dump f100:1FF81000:0x100`) with `TZ=UTC0`, with `TZ` unset (this PC: UTC-8) and with `TZ=PST8PDT` gave a date
field in the shared page that is 28,800,000 ms (8 hours) lower for the last two than for the first. Keep `TZ=UTC0`: it
makes the date field of the shared page the same on every machine.

## Results

`golden.py record <S>` and `golden.py check <S> --repeat 2 --require svc,ipc,state,checkpoints,hid,images,unmapped`
(G1 also `dsp`), against the same N0-29 runner (`mh4u_runner.exe`, the Windows CI build of `wp/N0-29-runner-exact` at
`3aef9b8`), the code library `0004000000126300.dll`, `--cpu recomp`, `TZ=UTC0`, on the maintainer PC with nothing else
running:

| Session | `record` | Host time of one run | `check --repeat 2` (exit 0) |
|---|---|---|---|
| G1 | exit 0: 2/2 checkpoints, 2 real scene changes, 0 capture problems | 64 s | 29 passed, 0 failed, 0 skipped, 3 info |
| G6 | exit 0: 3/3 checkpoints, 3 real scene changes, 0 capture problems | 306 s | 23 passed, 0 failed, 0 skipped, 3 info |
| G2 | exit 0: 7/7 checkpoints (4 distinct scene values), 8 real scene changes, 0 capture problems | 1198 s | 26 passed, 0 failed, 0 skipped, 3 info |

The passes of each `check` are `svc`, `ipc`, `state`, `checkpoints`, `unmapped`, `hid`, `gpu-<start>` (G1, G2), `dsp-1240`
(G1), `images` and `shared-page` for each of the two runs against the recording, and the same comparisons (without
`checkpoints`) for run 2 against run 1; the 3 `INFO` lines are the `timing` traces, identical as well. The user folders
were not compared by `golden.py` (the orchestrator's own measurement of the N0-29 runner compared them file by file, `log/`
left out, D49).

Every `check` made three runs (the recording and the two repeats) and compared run 1 and run 2 each with the recording,
and run 2 with run 1, in the `same` profile: `svc`, `ipc`, `state` and `unmapped` identical apart from the header line,
`hid`, `dsp`, `gpu`, the screenshots and the shared-page dump identical by SHA-256, `timing` identical (information).
Nothing was loosened to make it pass: the comparison of `same` has no tolerance and no list.

## History: the WIP recordings (before N0-29)

The first recordings (the WIP branch `origin/wp/N0-09-golden-sessions`, head `d6e2871`, CI run 39075253705, native
`e6cd840`) were made with the reference runner before N0-26 and N0-29, and are replaced by the ones above (their shared
page held a random MAC address and starting tick). The measurements of that time led to D30 to D36 and are kept here.

### Reproducibility of the reference runner before N0-29

The reference runner was **nearly reproducible** on that PC, not exactly: two runs with the same arguments agreed in
scene, frame counter, quest id and stage loader state in every state record. They differed in the tick count `t` in 8 of
801 records by at most 562 ticks (about 2 microseconds), and in the SVC trace only in the main thread (its
`GetSystemTick` results differed by a few ticks at the first difference, and its call count by about 0.2%). The
measurements were made on an idle PC with `TZ=UTC0`, a fresh copy of the user folder per run, G1 as the input file,
`--frames 800`, `--state-trace` and `--trace-svc`:

| Measurement | Result |
|---|---|
| State trace, `--cpu recomp`, two runs | `scene`, `frame_counter`, `quest_id` and `stage_loader_state` agree in 801 of 801 records. The tick count `t` is equal in 793 of 801; the 8 that differ are around frame 471 and differ by at most 562 ticks, and the traces are tick-identical again at the last frame. `smain_delta` differs in 197 of 801 records (low bits of a float) |
| SVC trace, same two runs | 17 of 18 threads are identical call for call. Only the main thread differs: the first difference is at call 338025, in a loop that polls `GetSystemTick` (svc 0x28) where the returned tick differs by 8; its call count is 460698 against 461742 (about 0.2%) |
| SVC trace, another `recomp` pair (with `--trace-ipc` on) | the first difference is at call 25767 and the main thread's counts are 463524 against 461734 |
| IPC trace (`--trace-ipc`), `recomp` pair and `jit` pair | identical (`compare_ipc_traces.py` exits 0) |
| `--cpu jit`, two runs | the same picture as `recomp`: only the main thread differs in the SVC trace (equal call counts in that pair), IPC identical |
| Before and after N0-18 | the same noise as between two runs of one runner |
| `jit` against `recomp` (same arguments) | `state_diff.py` agrees (2 scene changes, within 2 frames); in the SVC trace 10 of 18 threads have different call counts (main 511886 against 463524; thread 9 11913 against 7275) and the main thread diverges at call 393; in the IPC trace the main thread differs at request 256 (header and descriptor entry 0 differ, a `gsp::Gpu` command in the same position) and thread 9 at request 827 |

- **IPC and load.** A reviewer's `golden.py check G1` made while other work was using the PC failed `ipc` in the main
  thread; the idle-PC pairs were identical.
- **Shared page.** Dumping 0x100 bytes at 0x1FF81000 at game frame 100 in three identical runs gave pages that differ in
  12 bytes: offsets 0x60 to 0x65 (the Wi-Fi MAC address, random per run) and, in the second date record, the low bytes of
  the date (0x40-0x41) and of the update tick (0x48-0x4B). N0-29 removed them.
- **Longer sessions.** For G2, an earlier sample had one `fs:USER` write with a different CRC and two threads with one
  request more or fewer.
- **`jit` against `recomp`** is a separate question from the repeatability of one backend: each backend repeats itself,
  but the two interleave threads differently, so "the jit and recomp traces compare equal" (the N0-04 and N0-21
  dump-check items) does not hold as written; N0-28 calibrates the `cross` profile for this.

`golden.py check` against the same runner on the WIP recordings, a sample of the runs made then:

| Session | Passed in the sample | Failed in the sample | Not stable between runs |
|---|---|---|---|
| G1 | state, checkpoints, hid, dsp-1500, images | svc | ipc (passed in the author's run, failed in a reviewer's re-run made under load) |
| G6 | ipc, state, checkpoints, hid | svc, images (13 of 130 screenshots differ) | ipc and images are one sample each |
| G2 | state, checkpoints, hid | svc, ipc, images (50 of 308 differ) | ipc and images are one sample each |

### The WIP recordings

The WIP sessions had checkpoints that could only say "still scene `0x00`" (the observable was the constant sApplet
byte), a G1 DSP capture at frame 1500 that was silent (the title), screenshots every 1000 emulated milliseconds (the
runner had no frame-keyed option) and a `check` that did not pass the `svc` comparison. G1: 1700 frames, 60 s, 337 MiB;
G2: 9000 frames, 1100 s, 1245 MiB (323 files); G6: 3700 frames, 282 s, 575 MiB. On the WIP G1 a DSP capture of 6000 DSP
frames from game frame 100 (`--frames 1720`) gave 236 type-3 records and 1198 non-silent PCM frames, all between game
frames 300 and 1350 in six stretches (300-327, 451-467, 601-625, 751-772, 901-918 and 1234-1350), at most one source
enabled at a time, and none from 1360 on or in game frames 1000 to 1087.
