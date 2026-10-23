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
>
> **Status (N0-10).** The sessions that need the maintainer's save are G3 (the Arena hunt), G4 (a cutscene with gas) and
> G3-perf (G3 at 21:9), recorded the same way from a user folder with the save merged in (`--save`); see
> [The save](#the-save), [`ir:USER`](#iruser-d36) and the sessions [G3](#g3-the-arena-hunt), [G4](#g4-a-cutscene-with-gas),
> [G3-perf](#g3-perf-g3-at-219) and [G7](#g7-a-downloaded-quest-not-made). **G7 could not be made** (the board that lists the
> downloaded quests lists none), and **none of the sessions opens `ir:USER`**: the reference runner's HID capture kills
> the game when the Circle Pad Pro option is on, so the sessions run from a copy of the save with it off.

## Files

| Path | What |
|---|---|
| `tools/native/golden/<S>.txt` | the session: header lines and input events (the runner's `--input-file` format): G1, G2, G6 (no save), G3, G3-perf, G4 (the save), G7 (a draft) |
| `tools/native/golden.py` | `list`, `record`, `check` |
| `build/golden/<S>/` | one recording (the shared checkout's `build/`, not a worktree's) |

### Session file

```
# name: <id>                    must equal the file name
# needs: none | save
# frames: <total>               the run stops here (--frames)
# checkpoints: <frame>:<scene>, ...     scene = the observable `scene` value (0x.. or decimal)
# captures: gpu:<start>:<frames>, dsp:<start>:<frames>[:silent], ...    (gpu at most 30 frames)
# env: NAME=VALUE               optional, any number: an environment variable of the runner (G3-perf)
# draft: yes                    optional: placeholder numbers; `record` refuses the file
f600:START                      events: f<frame>:BUTTON[:hold], f<frame>:TOUCH:x:y[:hold], f<frame>:CIRCLE:x:y[:hold]
```

Event times are frames (`f<N>`, the game's frame counter), never milliseconds, so a script means the same on every CPU
backend and runtime. Several events may share a line, separated by commas. The scripts are only button presses and
touches: they may be committed. A checkpoint `<frame>:<scene>` holds when the first state-trace record at or after that
frame has that `scene`. `:silent` marks a DSP capture that is expected to be silent (a capture that is not marked must
have sound, see [Capture problems](#capture-problems)).

`# env:` lines (N0-10) are the start-up options of a session that the port reads from the environment (`MH4U_WIDESCREEN`,
`MH4U_WIDE_RENDER`, ...: it reads them once at start-up, so the runner's `--set` cannot change them, it answers "not an
option that can change while the game runs"). `record` and `check` give every run of the session those variables (over the
shell's, with `TZ=UTC0` kept) whatever runner it is (the reference, or a candidate), and the manifest keeps them
(`session.env`). `check` compares the session file's lines with the manifest's and stops with exit status 2 when they
differ (a session edited since `record`, or a recording made without them: record again), so a recording and a check can
never differ in them by an edited file or a forgotten command-line option; a manifest without the key counts as having
none. Only `MH4U_*` names are accepted (a session file sets the port's options and nothing else: not `PATH`, not
`PYTHONPATH`), each at most once, and `TZ` is refused.

`# init-time: SECONDS[.FRACTION]` (N0-35, D83; none by default, at most once) is the start date of the runners' clock, seconds
since the Unix epoch, UTC. D83 infers (from N0-36) that the game seeds its random generator from the Unix second of its first clock
reading, and that the runners' whole-second default (1767268800) puts that reading a few milliseconds from a second
boundary, where each runtime falls on a different side. A start date with a sub-second part (for example `1767268800.5`) is
meant to put it in the middle of a second on all of them, so that the same seed is drawn, which N0-35 Part B checks on G4 first. Grammar: SECONDS is
1 to 10 digits, FRACTION a dot and 1 to 6 digits, nothing else (no sign, space, exponent, hex, `.5` or `5.`); range
946684800 (2000-01-01 00:00:00 UTC) to 4102444799.999999 (2099-12-31 23:59:59.999999 UTC), both included; anything else is a
session-file error, as a bad `# env:` line is. The canonical form (seconds without leading zeros, fraction without trailing
zeros, no dot for a zero fraction: `0946684800` is `946684800`, `1767268800.500000` is `1767268800.5`) is what the session
stores, so two spellings of one date are one date. `record` and `check` give `--init-time <canonical>` to every runner they
start (the recording, the `--repeat` runs, the `--candidate-args` runs and the native runner); the manifest keeps it
(`session.init_time`) and `check` stops with exit status 2 when the session file's value differs from the manifest's (a header
edited, added or removed since `record`; a manifest without the key counts as none). A `--init-time` in `--candidate-args` (or
`--runner-args`) that disagrees with the header is a usage error (exit 2), an equal one is harmless. A session without the
header gets no `--init-time` from golden.py and runs exactly as before (a `--init-time` in its `--candidate-args` passes
through untouched); G1, G2 and G6 keep their whole-second date. No session has this header yet (G1 to G7 have none; G3,
G3-perf, G4, G7 and G8 get theirs when they are recorded again in N0-35 Part B).

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
golden.py check  S [--runner RUN --game CXI] [--update CIA] [--code LIB]
                   [--candidate-runner RUN2] [--candidate-program PATH] [--candidate-code LIB]
                   [--candidate-args "..."] [--repeat N] [--profile same|cross|native] [--fallback]
                   [--require NAMES] [--ignore FILE]... [--observable FILE]
                   [--tolerance N] [--min-margin N] [--max-surplus N] [--max-shift N]     # the cross and native numbers
```

The recordings of this document were made with (PowerShell, from the worktree; `TZ` is set by `golden.py`):

```
python tools/native/golden.py record <S> --runner build/runner-win-n029/mh4u-port-windows/mh4u_runner.exe `
    --game baserom/juego.cxi --update baserom/update_v130.cia --user build/userbase-cfg `
    --code build/game/recomp/0004000000126300.dll --cpu recomp --out build/golden
python tools/native/golden.py check <S> --repeat 2 --require svc,ipc,state,checkpoints,hid,images,unmapped `
    --runner ... --game ... --update ... --code ... --out build/golden        # G1 also ,dsp
```

The sessions that need the save (N0-10) were recorded and checked from the main checkout, with the worktree's `golden.py`
and session files and `--out` pointing at the main checkout's `build/golden`, the same options and:

```
python tools/native/golden.py record G3 <the options above> --user build/userbase-cfg --save build/n0-10-save-cpp-off
python tools/native/golden.py check G3 <the options above> --repeat 2 --require svc,ipc,state,checkpoints,hid   # no --save
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
- for G2: fewer than 2 real scene changes in `state.txt`, or checkpoints that hold fewer than 3 distinct scene values;
- *(N0-10)* for G3 and G3-perf: checkpoints that hold fewer than 5 distinct scene values (the intro, the continue screen,
  the village, the Arena's equipment screen, the hunt); for G4: no GPU capture that sets fog mode 7 (gas), the `fog modes
  set` of `gcap.py summary`.

### Who runs, and what is compared

| `check` given | Who runs | Profile |
|---|---|---|
| nothing | RUN with the recording's arguments | `same` |
| `--candidate-args "..."` only | RUN with the recording's whole argument list (`--new-3ds off`, `--install`, every recorder, `--count-unmapped`, `--trace-timing`, the screenshots and the shared-page dump) plus those options; `--code` only when the run's `--cpu` (the one in `--candidate-args`, else the recording's) is `recomp` | `same` unless `--candidate-args` changes `--cpu`, then `cross` |
| `--candidate-runner RUN2` (with `--candidate-program PATH`, `--candidate-code LIB`) | RUN2 with its own program and library and only the options both runners share: `--user`, `--frames`, `--input-file`, `--set`, `--trace-svc`, `--trace-ipc`, `--trace-ipc-full`, `--state-trace`, `--observable`, `--capture-hid`, `--capture-dsp`, plus `--candidate-args` | `cross` (`native` with `--profile native`) |

`--profile same|cross|native` forces one (`native` is never the default, section **`native`** below). Three usage
rules of `check` decide which of the options above are required (all of them a usage error, exit 2, when broken):

- `--runner` and `--game` are required **unless** a candidate runner runs the check: with no `--candidate-runner` a
  check needs both, because the reference runner runs again. (`--update` is needed too when the recording was made
  with `--install`, and `--code` for `--cpu recomp`; neither is needed when `--candidate-runner` runs.)
- `--candidate-runner` needs a program: `--candidate-program PATH`, or else `--game` (the program the reference ran).
  With `--candidate-program` it needs no `--game` and no `--runner` at all (N1-21: the native runner is started with
  its own game folder).
- `--candidate-program` and `--candidate-code` need `--candidate-runner`.

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
| `state` | `state_diff.py --tolerance 4 --min-changes <the recording's real scene changes>`: the real scene changes in the same order, to the same scene, up to **4 frames** apart (D50; D33 said 2); skipped when the recording has none: nothing to align. The result prints the tolerance |
| `state-margin` | D50's guard, with `cross_model_report.py`'s logic: every input event of the session script that follows a real scene change comes at least **60 frames** after the last real scene change **on both sides**. Runs with `state` (`--require state` covers it) and prints the largest scene-change shift and the smallest margin of each side |
| `checkpoints` | as in `same` |
| `ipc` | `compare_ipc_traces.py --ignore config/native/ipc_time_dependent.txt` (plus every `--ignore FILE`) `--fs-reads-multiset --max-surplus 16 --fs-write-mask config/native/fs_write_masks.txt --fs-write-mask-session <S>` (D52): per thread the `fs:USER/file` Reads as a multiset of (offset, size, result, CRC of the data read) with at most 16 reads more on one side; every other `fs:` request in order and in full; a Write by the CRC of its data with the byte ranges of the session's mask lines zeroed. The result prints the reads and the masks used |
| `svc` | `compare_svc_traces.py --normalize --outputs --cross-model config/native/svc_cross_model.txt` (N0-25) |
| `svc-full`, `images` | run and printed as `INFO`, never gated (the image threshold comes in N2b) |
| `hid` | `capfmt.py diff-hid` |
| `dsp-<start>` | `capfmt.py diff-dsp --align --max-shift 48` (D51): the candidate aligned on the first buffer change of the first enabled source, the shift constant and at most **48** DSP frames, every source status block equal after it; the PCM samples are not compared. The result prints the shift |

`comparators.md` (sections 7 to 9) describes each rule, what it still catches and what it cannot. The numbers of the
cross profile (`--tolerance`, `--min-margin`, `--max-surplus`, `--max-shift`) default to those of D50, D51 and D52 and
can be changed **in the cross and native profiles only**; with the `same` profile they are a usage error, like
`--ignore`, so a same-model check can never be loosened. `config/native/fs_write_masks.txt` must exist for a cross or
native check (usage error otherwise); it is edited by the orchestrator, and a line of the recording's session that
matches no Write of the two traces makes the `ipc` comparison fail (a stale mask never passes silently).

`--ignore FILE` (repeatable) adds IPC ignore files to the `cross` and `native` profiles only; with the `same` profile
it is a usage error, so a same-model check can never be loosened by a list.

**`native`** (D76; N0-33; the native runtime in real time against the reference. Asked for with `--profile native`,
never the default: `--candidate-runner` alone is `cross`). Its game clock follows the host while the reference charges
the modelled 268 MHz, so the scenes change earlier, the DSP counter runs hundreds of frames apart and a streaming
reader chunks its reads differently (`comparators.md`, section 10). It is the `cross` profile with these differences
and nothing else loosened (the `golden.py` constants `NATIVE_TOLERANCE`, `NATIVE_MAX_SHIFT`, `NATIVE_FS_READS_COALESCE`
and `NATIVE_FS_READS_PREFIX`, with D76 named in the comment):

| Comparison | Rule, where it differs from `cross` |
|---|---|
| `state` | `--tolerance 24` (D76) instead of 4: a real scene change may fall up to **24 frames** apart, in the same order and to the same scene |
| `state-margin` | unchanged: every input at least **60 frames** after the last real scene change on both sides (D50), so a 24-frame shift can never move an input into another scene |
| `ipc` | `--fs-reads-coalesce --fs-reads-prefix` added: the `fs:USER/file` Reads of one thread and one file session whose byte ranges touch are merged into one Read (CRC-32 combined) before the multiset is built, and a run that goes on further than the other side's counts as contained when the other side's is its verified prefix; the surplus limit stays 16, the Writes and every other `fs:` request as in `cross` (D52's masks, `config/native/fs_write_masks.txt`) |
| `dsp-<start>` | `--max-shift 300` (D76) instead of 48, and an **odd** shift compares the reference's region *r* with the candidate's region *1 − r* |
| everything else | as `cross` (`svc` on the cross-model list, `hid`, `checkpoints`, `svc-full` and `images` as information) |

The profile's number is printed in each result (`within 24 frames`, `largest scene-change shift 15 frames (at most
24)`, `shift -205 DSP frames`, `after merging touching reads: 46 and 46`) and the last line names it
(`(profile native)`). `--tolerance`, `--min-margin`, `--max-surplus` and `--max-shift` still apply to it. The native
runtime is not repeatable from run to run (the host's timing), so a native check is run more than once and says how often
it passes (`comparators.md`, section 10, lists what was measured); `--repeat` compares the runs with each other in the
`same` profile, which a native runtime does not pass, so run the check several times instead.

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

## The save

The sessions that `needs: save` (G3, G4, G3-perf, G7) start from the same user folder with the maintainer's save merged
over it (`record --save DIR`, copied into `user-before/` after the user folder, so a `check` of them needs no save
option). The staged save is in the user-folder layout: the game's save slots
(`sdmc/Nintendo 3DS/<id>/<id>/title/00040000/00126300/data/00000001/{system,user1}`) and its extdata
(`.../extdata/00000000/00001263/user/{card1-3,cardbox,otomo1,quest1-5}`); `golden.py` never writes into it.

**The recordings use a copy of it with the Circle Pad Pro option off** (`build/n0-10-save-cpp-off`), because the reference
runner cannot record the game with the option on (see [`ir:USER`](#iruser-d36)). The copy differs from the staged save in
one file, `system` (512 bytes: the settings slot; the game writes it again, so 508 of its bytes differ), and was made by the
game itself, from the staged save, with a script of touches that switches the option off in the main menu's Options:

```
f300:A:f6,f450:A:f6,f600:A:f6,f750:A:f6,f900:A:f6     the first five presses of the sessions (the run reaches the main menu with these alone)
f1070:TOUCH:103:100:f6        Options (the main menu's third button, bottom screen)
f1250:TOUCH:200:73:f6         the Circle Pad Pro row
f1450:A:f6                    "Use the Nintendo 3DS Circle Pad Pro attachment?" has No selected: A sets it to Off
f1650:A:f6                    closes the notice
f1850:TOUCH:290:222:f6        Back (to the main menu): the game has written `system`
```

To rebuild the copy (the script above in a file, `TZ=UTC0`, the N0-29 reference runner; about two minutes):

1. `D` = a copy of `build/userbase-cfg` with the staged save merged over it (`build/n0-10-save/*` copied into `D`).
2. `mh4u_runner <game>.cxi --new-3ds off --cpu recomp --code build/golden-lib/0004000000126300.dll --install
   <update 1.3.0>.cia --user D --frames 2400 --input-file <the script>`: the options of `golden.py record` that matter,
   and **no** `--capture-hid` (with the option on, a run with it dies in the Circle Pad Pro's IR service at boot,
   see below, before the option can be switched; the other recorders and the screenshots do not change what the game writes).
3. `D`'s `system` (it is at the same path as in the staged save, `sdmc/Nintendo 3DS/<id>/<id>/title/00040000/00126300/data/
   00000001/system`) replaces the one of a copy of the staged save; every other file is the staged save's, untouched.

That recipe was run again from scratch (measured, after the copy was made) and gives the same twelve files byte for byte
as `build/n0-10-save-cpp-off`: the run changes none of the other eleven, and its `system` is identical to the copy's. Only
`system` differs from the staged save (the 508 of 512 bytes noted above). Every later run starts from the copy, so the script is not
part of any session. A recording's `user-before/` holds the copy, which is what a native replay of the session starts from.

## `ir:USER` (D36)

**None of G3, G4, G3-perf or G7 opens `ir:USER`, and none can with this reference runner.** D36 wanted one golden session
whose HID capture has type-2 (IR) records; it cannot be had, for this reason:

- The maintainer's save has the Circle Pad Pro option **on**. With it on the game opens `ir:USER` while it boots (about
  frame 199: Initialize with a 0x1000-byte shared block at 0x08DF0000, Finalize, Initialize again, 8 requests then a second
  `CreateMemoryBlock` of the same address and size), and keeps polling it (47,000 `ir:USER` requests in the first 11,400
  frames).
- With `--capture-hid` that second `CreateMemoryBlock` fails with `0xE0A01BF5` and the game dies with a fatal error
  (`err:f`, status 17 of the runner, the SVC trace ends with that call): the runner's HID capture
  (`HidIpcRecorded`, `port/src/runner.cpp`) keeps a reference to the first block it sees handed to `ir:USER`, so the
  block the game freed would still be mapped at 0x08DF0000 when the game asks for it again (this cause is a reading of
  the runner's code that no test has confirmed; what was measured is that the game dies with the capture and the option
  both on, and not with either one off). The same run **without**
  `--capture-hid` goes on (both calls succeed in its SVC trace). Switching the option on from inside the game (the village's
  Options, page 3, "Circle Pad Pro") with the capture on gives the same fatal error at that moment, and with the option off
  there is no `ir:USER` request at all (0 in more than 9,000 frames).
- `golden.py record` always records the HID, so a session cannot open `ir:USER` without a runner fix; the recordings use the
  save with the option off ([The save](#the-save)). The capture check of `golden.py` stays as N0-09 made it: a recording whose
  IPC trace has an `ir:USER` request needs a type-2 record in its HID capture from that request on (a test covers it), so the
  day the runner is fixed a session that opens it is checked.
- To get D36's session: fix the capture (hold the block weakly, or copy what it needs on Initialize; a change of
  `port/src/runner.cpp` and a new reference runner, which would have to be checked again against N0-29's exactness), then
  record G3 again from the staged save as it is (the option on) and `golden.md` says so. Not done here: it is not in this
  WP's files.

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

### G3: the Arena hunt

Needs the save ([The save](#the-save): the copy with the Circle Pad Pro option off). Frames: 18000. Save → village → the
Arena counter's quest "Grudge Match: Yian Kut-Ku" → the Arena with a preset loadout → 3,891 frames (about 65 s) of the hunt, from the load at f14109 to the end.
Checkpoints (one in each scene that lasts more than 100 frames, five distinct values): `600:0x00e64d5c` (`aDemo`, the
intro movies), `1400:0x00e6c778` (`aContinue`, the continue screen), `2500`, `8000` and `13000:0x00e6c170` (`aVillage`),
`13700:0x00e650bc` (`aArena`, the equipment screen), `15000`, `17000` and `18000:0x00e655c8` (`aQuest`, the hunt). Real
scene changes: 10 (frames 2 `aGameSys`, 42 `aDemo`, 903 `aContinue`, 1576 `aDemo`, 1578 `aGameSys`, 1579 `aVillage`,
13462 `aGameSys`, 13464 `aArena`, 14107 `aGameSys`, 14109 `aQuest`; the steps of 1 to 3 frames have no checkpoint).
Capture: `gpu:17400:10`, 10 frames with the Yian Kut-Ku on screen (it is in view from about frame 16740 to 17800); fog mode
5 (plain fog) is set 10 times and gas never. Recording: 3,363 s of host time (56 minutes), 3,114 MiB in 624 files (the SVC
trace is 2,696 MiB, the GPU capture 166 MiB). The comments of `G3.txt` explain each stretch:

| Frames | What |
|---|---|
| 300 to 1500 | nine `A`: the main menu and the character select |
| 1579 to about 2900 | the village's arrival scene plays by itself (the cutscene G4 captures) |
| 3300 to 8460 | `A` every 120 frames through the caravan leader's talk (about 5,000 frames); the press at 8460 ends it, control at about 8466 |
| 8694 to 9044 | touches: the world map button, the Gathering Hall and its confirmation |
| 9494 to 11244 | circle pad from the hall's door to the quest counter's desk |
| 11444 to 12119 | `A` every 75 frames: the Arena counter's talk; "Select this Quest?" has *Yes* selected, the press at 12044 posts the quest and 12119 closes the notice |
| 12250 to 13100 | circle pad to the Quest Gate |
| 13300, 13650 | `A`: "Depart on Quest", then the Arena's equipment screen |
| 13850, 13910 | `LEFT`, then `A`: "Use this equipment?" has *No* selected |
| about 14109 | the quest has loaded |
| 14350 | circle pad forward-right for 600 frames |
| 15050 to 17840 | `A` every 90 frames: the caravan leader's explanation of the target camera, then the swings of the hunt |

The two traps of the spec hold in it: the script never presses `B` (right after the quest is accepted it would leave the
quest unposted), and the equipment prompt is answered with `LEFT`, then `A`. A press after f8460 would start the talk
again (the leader's idle talk), and one after f12119 would start the counter's talk again ("you've already got a Quest?",
then a "Cancel the current Quest?" step that the script does not want): both were seen in the exploration, so the
sequence in the table is the one that works, and its frames cannot be moved by more than a press.

### G4: a cutscene with gas

Needs the save. Frames: 3000: the same nine `A` as G3, and nothing after them; the village's arrival scene plays by itself
and is the cutscene (it runs from f1579, the start of the village, to about f2900; the GPU capture is inside it, and the
mode-7 writes of `gcap.py summary` are what shows it uses gas). Checkpoints: `600:0x00e64d5c`, `1400:0x00e6c778`, `2500` and `3000:0x00e6c170`.
Real scene changes: 6 (frames 2, 42, 903, 1576, 1578, 1579: as G3's first six). Capture: `gpu:2400:10`, ten frames inside the
cutscene, **`gcap.py summary` shows fog mode 7 on every one of them**:

```
fog modes set:  fog: 30   gas: 10   none: 50
```

(per frame: one write of mode 7, three of plain fog and five with fog off), 59 `GX` commands (20 `SUBMIT_CMDLIST`, 20
`DISPLAY_TRANSFER`, 19 `CACHE_FLUSH`), 20 `PICA_REG`, 40 `GPU_PAGES`, 60 `IRQ`; the `P3D` interrupts of the command lists
were given delays of 51,145 to 5,123,392 ns (median 2,554,218), the heavy command lists of the gas scene. `golden.py record`
fails ("never set fog mode 7") when no GPU capture of G4 sets mode 7, so a G4 that lost the gas is never kept; the same
check is in the tests with a synthetic capture. In the exploration a capture at frame 3500, in the talk that follows
the scene, set plain fog only (no mode 7), and the one in the scene itself, at frame 2400, is the one kept. Recording:
369 s of host time, 570 MiB in 124 files (the SVC trace is 359 MiB, the GPU capture 165 MiB).

### G3-perf: G3 at 21:9

Needs the save. G3 with the top screen rendered at the monitor's aspect ratio, for the GPU budget (N2-19): the script is
`G3.txt` with three `# env:` lines, `MH4U_WIDESCREEN=3440:1440` and `MH4U_WIDE_RENDER=1` (the spec's `--set
0:widescreen=3440:1440 --set 0:wide_render=1` as the port reads them: at start-up, from the environment; the runner's `--set`
refuses both) and `MH4U_DEPTH_OF_FIELD=off`, and a GPU capture of 30 frames in the fight, `gpu:17300:30`. Its inputs are G3's
line for line, and so are its checkpoints and its scene changes: **the render width does not move the frame at which
anything happens** (the recording's ten scene changes fall at the frames of G3's). The top screen's screenshots are 576
pixels wide (3440/1440 of 240, rounded to whole 8-pixel tiles). Real scene changes: 10. Capture: `gpu:17300:30`, 30 frames
with the Yian Kut-Ku in view (`gcap.py summary`: 179 `GX`, 60 `PICA_REG`, 239 `CPU_PAGES`, 120 `GPU_PAGES`, fog mode 5 on 30 draws
and none on 60, no gas); the display transfer of the widened top screen was given **185,185 ns** (G3's: 92,592) and the
`P3D` interrupts 203,049 to 16,068,479 ns (median 8,015,431). Recording: 4,287 s of host time (71 minutes), 3,330 MiB in 664
files (the SVC trace is 2,781 MiB, the GPU capture 248 MiB).

**Why the depth-of-field filter is off.** With `wide_render` on and the filter on (the default), the reference runner dies
in the village's arrival scene, at about frame 1900, whatever the save and whether or not the screens are being drawn for
screenshots: `HW.Memory <Error> GetPhysMemRegionInfo: Unknown GetPhysMemRegionInfo @ 0X000000 at PC 0X56D9F4`, then an
access violation of the runner (status -1073741819). Three runs gave it (two saves, with and without the draw window); with
`MH4U_DEPTH_OF_FIELD=off` (the port's own option for the cutscenes' blur, `docs/PLAYING.md`) the same script ran to its end.
The cause is in the runner or the port (`port/src/`; a hypothesis from these three runs: the widened render targets
against the filter's pass), not in the game or in this script; N2b's G3-perf (N2b-18, N2b-20) starts from this recording and runs with the filter off as recorded.

### G7: a downloaded quest (not made)

**Not recorded: `tools/native/golden/G7.txt` is a draft** (`# draft: yes`, which `record` refuses) that keeps the route to the
board. The spec wants a downloaded quest from the maintainer's extdata (`quest1` to `quest5`) entered and 30 seconds of play
on it; the board that lists downloaded quests, the **Event Quests** board of the quest counter (the Low Questatrix's desk:
three boards on the touch screen, HR 1+, Guild Quests and Event Quests), answers **"(No Quests available.)"**:

- The game does read the files for it: touching Event Quests makes it list the extdata folder and read four files whole
  (`quest1`, `quest2`, `quest3` and `quest5` in the trace that has the file names; 328,704 bytes each: an `fs:USER`
  `OpenFile` then a `Read` of the whole file, one after the other) within one second of emulated time, and then it prints
  that line. The five files are also opened at boot, with the guild cards and the Palico file.
- The same answer comes from the staged save with the Circle Pad Pro option on and from the copy with it off (two runs
  that reach the board at different frames).
- The "Guild Quests" board of the same counter is a different list (the pool of quests obtained through StreetPass and
  expeditions), and the title screen's `DLC` button asks "Connect to the Internet to
  access downloadable content?", not for the downloaded quests.
- What stops the list is, most likely, the clock: event quests are distributed for a limited period, and the reference runner
  starts every run at 2026-01-01 12:00 UTC (a constant of `port/src/runner.cpp`, D30). That is a reading of the game's
  behaviour, not a measurement: nothing in this WP's files can change the runner's date (`--async-io` starts at the host's date,
  which is 2026 as well and makes the run unrepeatable).

So G7 needs a decision outside N0-10: a runner option for the start date (so that a session can start at a date in the
distribution period of one of the five quests, which a run of the game with the files could find out), or the statement that
downloaded quests are not part of what the native runtime is checked against. The draft's route
(`f300` to `f10894` as G3, then `f11340` to `f11880` circle pad along the desk until the `A` prompt names the Low Questatrix
and not Arena Bambina, `A` every 90 frames, `A` at `f12460`, touch at `f12700`) is what the next attempt starts from.

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

### Results of N0-10 (the sessions with the save)

`golden.py record <S> ... --save build/n0-10-save-cpp-off`, then `golden.py check <S> --repeat 2 --require
svc,ipc,state,checkpoints,hid` (the `same` profile; `images`, `unmapped`, the GPU capture and the shared page are compared
and gated too, as above), the same runner, library, clock and PC as the table above, the maintainer's PC with nothing else
using the emulator (one process at a time) and some compiles and test runs going on beside it, which do not change what a run
of this runner does (the clock of a run is emulated):

| Session | `record` | Host time of one run | `check --repeat 2` (exit 0) |
|---|---|---|---|
| G4 | exit 0: 4/4 checkpoints, 6 real scene changes, gas in the capture, 0 capture problems | 368 s | 26 passed, 0 failed, 0 skipped, 3 info; 729 s for the check |
| G3 | exit 0: 9/9 checkpoints (5 distinct scene values), 10 real scene changes, 0 capture problems | 3,363 s | 26 passed, 0 failed, 0 skipped, 3 info; 6,717 s for the check |
| G3-perf | exit 0: 9/9 checkpoints (5 distinct scene values), 10 real scene changes, 0 capture problems | 4,287 s | 26 passed, 0 failed, 0 skipped, 3 info; 8,302 s for the check |
| G7 | not made (a draft: [G7](#g7-a-downloaded-quest-not-made)) | | |

The passes are the same nine comparisons per run as G2's (`svc`, `ipc`, `state`, `checkpoints`, `unmapped`, `hid`, `gpu-<start>`,
`images`, `shared-page` against the recording, and the runs against each other without `checkpoints`), and the three `INFO`
lines are the identical `timing` traces. Run 1 and run 2 were each identical to the recording and to each other.

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
