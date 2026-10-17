# Golden sessions

> **Status: skeleton (N0-09, phase 1).** The tool and its tests are done; the sessions are scaffolds
> (`draft: yes`) and nothing has been recorded yet. Sections marked **TODO (phase 2)** need the
> reference runner and the game.

A golden session is a frame-keyed input script plus the recording of what the reference runner (the Azahar-based
`mh4u_runner`, Old 3DS) did with it. Later work packages check a candidate runtime against the recordings
(`tools/native/golden.py check`). The recordings are game data: they live in `build/golden/<S>/` and never in the
repository.

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

Event times are frames (`f<N>`), never milliseconds, so a script means the same on every CPU backend and runtime.
Several events may share a line, separated by commas. The scripts are only button presses: they may be committed.

### A recording

| File | What |
|---|---|
| `user-before/`, `user-after/` | the user folder (and the save, if given) before the run, and as the run left it |
| `svc.txt`, `ipc.txt` | the SVC trace and the full IPC trace (`--trace-ipc-full`) |
| `state.txt`, `unmapped.txt` | the state trace and the unmapped-access report |
| `hid.mhid`, `dsp-<start>.mdsp` | the HID capture and each DSP capture |
| `gpu-<start>.mgpu` (with `.json` and `.frames/`) | each GPU capture |
| `shots/` | screenshots (`--shot-every`, default 1000 ms of emulated time) |
| `runner.log` | the runner's output |
| `manifest.json` | the runner and code library used (with SHA-256), the time zone, the arguments, the checkpoint results, the capture problems, and size and SHA-256 of every file above |

`check` runs into `build/golden/<S>/check/run<N>/` and writes `check/result.json`.

## Commands

```
golden.py list [--out DIR]
golden.py record S --runner RUN --game CXI --update CIA --user DIR [--code LIB] [--cpu jit|recomp] [--save DIR]
golden.py check  S --runner RUN --game CXI --update CIA [--code LIB]
                   [--candidate-runner RUN2] [--candidate-program PATH] [--candidate-code LIB]
                   [--candidate-args "..."] [--repeat N] [--ignore FILE]
```

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

## Time zone

Azahar's shared page turns the start date into the 3DS clock through the host's local time zone, so recordings should
be made on a UTC host. `record` and `check` start the runner with `TZ=UTC0` in the child's environment and
`manifest.json` has `"tz": "UTC0"`. **TODO (phase 2):** confirm that the Windows runner honours `TZ` (this PC is on
UTC-8): record twice and compare the clock-dependent state, or read the clock the game sees.

## The software keyboard

The runner's keyboard is Azahar's default one: it answers with the user name from the config (Azahar's default is
`AZAHAR`) and the button the configuration expects. The native keyboard answers the same way (N1-14).

## Sessions

### G1: power-on to the title

Power-on, no input, until "Press Start" is stable. Captures: `gpu` 3 frames and `dsp` 300 frames at the title.

**TODO (phase 2):** the real total, the checkpoints (title scene and its frame), the capture start (found with
`--shots`).

### G2: new game to the first control in the village

Title, new game, character creation with the name through the software keyboard, first control in the village. Capture:
`gpu` 3 frames in character creation.

**TODO (phase 2):** the input events, the checkpoints, the capture start.

### G6: the software keyboard again

The keyboard used a second time (the name, or the village chat). No capture required.

**TODO (phase 2):** the input events and the checkpoints.

## Numbers from `gcap.py summary` of G1's capture

**TODO (phase 2):** interrupt counts and delays (per interrupt id), GX command kinds and the frames covered, from
`gcap.py summary build/golden/G1/gpu-<start>.mgpu`. Do not paste anything else from the capture.
