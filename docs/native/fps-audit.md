# The 60/120 fps audit (N3-22)

ROADMAP section 6 asks for the same session at 60 and at 120 fps to give the same game, "positions and times within the
tolerance the audit sets". `tools/native/fps_audit.py` plays a golden session twice on the native runner, headless, and
compares the two runs in six items. This note is the method, the tables the first run of it made on the maintainer's PC
(2026-10-31, base `283f6b5`), the measured spread D70's numbers come from, and the verdict on `--io-latency none` for D71.
Only counts, frames, times and shifts are written here: no memory contents, no game text, nothing of a save.

**Outcome: needs-decision.** The tool is done and tested; the audit does not pass, in any mode, for any of G1 to G4, and two
findings are not a matter of tolerance (*What the runs found*): the scripted G3 does not reach the Arena at 120 fps on the
native runtime although the reference does (a deterministic run reproduces it, so it is not noise), and the game's clock
is 4 s ahead of its frame count from the first movie on at 120 fps, on the reference as well, which puts the audio of a
game frame seconds apart. D70's numbers are proposed below; D70 and D71 are the orchestrator's to mark Decided.

## Using it

```
python tools/native/fps_audit.py scale tools/native/golden/G3.txt --factor 2 -o OUT.txt      # observable_probe.py's scale
python tools/native/fps_audit.py run S --runner build/native/bin/mh4u_native_runner.exe --program build/native-game \
    --code <library> --user-before build/golden/S/user-before --io-latency azahar --out build/fps/S \
    --path <llvm-mingw bin> --lock build/locks/emulator [--dsp START:FRAMES] [--runner-arg=--deterministic ...]
python tools/native/fps_audit.py compare build/fps/S [--require position,quest_time,dsp,writes] [--position-until 15862] \
    [--fs-write-mask FILE]
```

`run` plays S at `--set 0:fps=60` with the session and at `--set 0:fps=120` with the session scaled by 2 (every frame key,
hold and checkpoint frame doubled, milliseconds unchanged: `observable_probe.py scale`), each from a fresh copy of
`user-before`, with `TZ=UTC0`, the session's `# init-time`, `--state-trace` (the observable file of the repository plus N3-21's
four lines as `ignore`, written to `OUT/observable.txt`; `config/**` is not edited), `--trace-ipc --trace-ipc-full` and one
`--capture-dsp` per DSP capture (the start scaled). `--dsp START:FRAMES` adds a window (G2, G3 and G4 have none of their
own); `--runner-arg=ARG` passes an option to the runner as it is (`{out}` is the run's folder: this is how the deterministic
mode is asked for). The emulator lock folder is waited for, created and removed. Everything lands in `OUT/r60/`, `OUT/r120/`
and `OUT/meta.json`. `compare` reads that folder and writes `OUT/compare.txt` and `OUT/compare.json` (every number of the
tables below); exit 0 when no item fails, 1 when one does, 2 for a usage problem or an unreadable run.

The runner needs the llvm-mingw `bin` folder on `PATH` (`--path`), or it waits on a hidden missing-DLL dialog.

## The items

Before the six, `runs` checks that both runs finished: exit status 0 and a state trace that reaches the session's last
frame. A run that hangs must not read as a pass. The tolerances are D70's first guesses, options with those defaults
(`--time-frames`, `--position-relative`, `--position-absolute`, `--quest-seconds`, `--max-shift`); nothing was retuned.
An item with nothing to compare is `n/a` (reported, not a failure) unless `--require` names it (D34).

| # | Item | Rule |
|---|---|---|
| 1 | `scenes` | the sequence of real scene changes (`state_diff.real_scene_changes`) is the same in values and order; no change in either run is a failure |
| 2 | `times` | each change's game time, frame / fps, within 4 frames at 60 fps (66.7 ms); the table also has the game clock (the trace's `t=`) at the two records |
| 3 | `checkpoints` | at each checkpoint of the session (frame F at 60 fps, 2F at 120) the `eq` observables of the observable file are equal, and the scene is the checkpoint's |
| 4 | `observables` | every second of game time, in the hunt only (the scene aQuest: the chains name whatever sits there elsewhere, N3-21; `--hunt-scene any` lifts it): the position within 2 % of the path length of the 60 fps hunter in the last second plus 0.05 units; the quest counter within 1 s (60 units a second, N3-21). A counter that never moves compares nothing (`n/a`). The rows also have the distance with 4 frames of time slack, as information |
| 5 | `dsp` | `capfmt.diff_dsp_aligned` with `--max-shift 48` on each capture pair (D51); the detail line gives the game clock offset between the runs at the window's first frame, in DSP frames |
| 6 | `writes` | the `fs:USER` file Writes, per thread, in order and in full (offset, size, result, CRC-32 of the data), the lines of `fs_write_masks.txt` of the session (the base name: `G3` for `G3-x2`) zeroed first (`compare_ipc_traces.load`); a mask line that matches no Write is a failure |

## What the runs found

1. **The game's clock is ahead of its frame count at 120 fps, from the first movie on.** At the same game time
   (frame / fps) the clock of the 120 fps run is 4.0 s later in real time and 4 to 9 s later in the deterministic mode
   in the first minutes (22 s by 270 s of G3), and N3-21's reference runs show the same (+4.2 s at 15 s of game time,
   +17 s at 224 s). It is a fact about the game (the frame counter runs slower than 120 a second while a movie or a load
   is on, it seems), not about the native runtime. It is why item 5 cannot hold after the boot, and why the scene changes
   after the intro movie are 100 to 300 ms earlier at 120 fps in frames over fps.
2. **G3 does not reach the Arena at 120 fps on the native runtime, and the reference does.** In the deterministic mode
   (`--deterministic`, the `_native_det` library) the native 60 fps run is the reference's, frame for frame: all 18,001
   state records equal in every observable, the tick offset never more than 5,825 ticks (22 us), the ten scene changes at
   the reference's frames. The 120 fps run equals the reference's (N3-21's run) in every observable up to frame 26,920,
   where the reference leaves the village and the native runtime never does (6 real scene changes, the run ends in the
   village; seeds 1, 2 and 3 of `--seed` give the same). The session is N3-21's, byte for byte (the scaled one too). The
   ticks of the native 120 fps run drift from the reference's from 55.5 s of game time on, in the long talk of the
   village (0.4 ms at 55.5 s, 3.7 ms at 75 s, 37 ms at 153 s, 2.5 s at 220 s, 5.2 s at 300 s). The inputs reach the game
   as scripted (the HID capture shows the circle pad stretches of the walk at their scaled frames). The fix is a hook,
   which the orchestrator opens; the candidate is the quest-loading race.
3. **Real time is a coin flip on G3, in both directions.** Ten real-time runs (five pairs; the runner is paced to real
   time): the 60 fps run reaches the hunt in 3 and the 120 fps run in 3, and in four of the five pairs exactly one of the
   two does not. Two of the four failing runs were slow ones (host time 454 s and 408 s against 305 s). N1-26 measured
   10 of 10 at 60 fps on a quiet PC. So a single real-time pair cannot gate G3: the deterministic mode is the gate (a
   pair takes 13 s for G1 and G4, 30 s for G2 and 75 s for G3 of host time).
4. **The hunter's position is as unrepeatable between two runs at one rate as between two rates.** At 60 fps two
   native runs end the 10 s walk (frame 14350, 600 frames) 98 units apart; two runs at 120 fps end it 1,280 units apart;
   the reference's own 60 fps run (N3-21) ends it 840 units from a native 60 fps run. Over the 65 seconds of the hunt two
   native 60 fps runs are up to 2,460 units apart (median 100), two 120 fps runs up to 2,233 (median 1,206), and a 60 fps
   run against a 120 fps run up to 1,959 to 5,603 (the table below). The quest counter between two 60 fps runs is within
   0.20 s, between two 120 fps runs up to 15.8 s (the talk that pauses it starts at other times).
5. **`--io-latency none` hangs the deterministic mode at boot.** G1 and G4 with `--deterministic --io-latency none`
   write no state record and spin (G1 killed after 2 minutes and 123 s of CPU, G4 timed out at 100 s per rate; with
   `azahar` the same runs take 5 s and 4 s). So `none` has real-time runs only.
6. **G3 and G4 need the same two write masks as G2.** The Write of 81,408 bytes at offset 0 differs between the rates in
   bytes 0 to 7 and 280 to 287 and nowhere else (the checksum and the clock-derived words of `fs_write_masks.txt`'s G2
   lines; the 120 fps run reads the clock at other frames). With `none` the clock readings fall in the same second and
   the Write is equal without a mask.

## Tables

Runs of 2026-10-31 on the maintainer's PC, the native runner and code library, game folder `build/native-game`, a copy of the
recording's `user-before` per run (G3 and G4 with the maintainer's save), `# init-time` of the session where it has one.
Host seconds are for the 60 and the 120 fps run. Mode: **det** is `--deterministic` with the `_native_det` library, **rt**
the real-time `--native` library.

Items by session, `--io-latency azahar` (what D70 gates). Counts are of the 60 fps run and the 120 fps run.

| Session | Mode | host s | scenes | times (limit 66.7 ms) | checkpoints | observables | dsp (limit 48) | writes |
|---|---|---|---|---|---|---|---|---|
| G1 | det | 5 + 6 | pass (2 changes) | **FAIL** 275.0 ms | pass (2) | n/a | **FAIL** +57 | pass (11) |
| G1 | rt | 31 + 35 | pass (2) | **FAIL** 91.7 ms | pass (2) | n/a | pass +17 | pass (11) |
| G2 | det | 10 + 20 | pass (8) | **FAIL** 300.0 ms (3 of 8 over) | pass (7) | position **FAIL** (1 jump), quest n/a | **FAIL** +1262 | pass (13, 1 masked) |
| G2 | rt | 153 + 157 | pass (8) | **FAIL** 133.3 ms (3 of 8) | pass (7) | position **FAIL** (1 jump), quest n/a | **FAIL** +671 | pass (13, 1 masked) |
| G4 | det | 4 + 9 | pass (6) | **FAIL** 316.7 ms (4 of 6) | pass (4) | n/a | **FAIL** no buffer in common | pass (2, 1 masked by the proposed lines) |
| G4 | rt | 53 + 57 | pass (6) | **FAIL** 158.3 ms (4 of 6) | pass (4) | n/a | **FAIL** +646 | pass (2, 1 masked by the proposed lines) |
| G3 | det | 25 + 49 | **FAIL** (10 changes, 6) | n/a | **FAIL** (4 of 9) | n/a | **FAIL** no buffer in common | pass (2, 1 masked by the proposed lines) |
| G3 | rt, 3 pairs | 303 to 408 + 307 to 455 | **FAIL** in all three (one run of each pair stays in the village) | n/a | **FAIL** | n/a | **FAIL** | pass (2, 1 masked) |

G2's position failure is one second of 63: the hunter in the opening jumps 673.6 units, and the jump falls in another
second's sample at the two rates (more than 4 frames apart: the slack column does not clear it); the other 62 seconds
are equal to 0.05 units. Its quest counter does not move in the opening (n/a).

Scene changes, game time of the 120 fps run minus the 60 fps run in ms (frame / fps; negative: earlier at 120 fps), the
game clock offset beside it, for `azahar`. The changes are, in order, aGameSys, aDemo, then the ones named in the sessions.

| Session | mode | differences in ms, in order | game clock offsets, s |
|---|---|---|---|
| G1 | det | -16.7, -275.0 | -0.03, -0.11 |
| G1 | rt | -16.7, -91.7 | -0.13, -0.21 |
| G2 | det | -16.7, -275.0, -16.7, -41.7, -58.3, -66.7, -283.3, -300.0 | -0.03, -0.11, +4.26, +6.82, +6.81, +6.79, +6.75, +6.73 |
| G2 | rt | -16.7, -91.7, -33.3, -41.7, -58.3, -66.7, -116.7, -133.3 | -0.02, -0.09, +4.16, +4.08, +4.06, +4.06, +4.02, +4.01 |
| G4 | det | -16.7, -316.7, -33.3, -125.0, -141.7, -150.0 | -0.03, -0.14, +4.21, +4.14, +4.13, +4.11 |
| G4 | rt | -16.7, -158.3, -33.3, -108.3, -125.0, -133.3 | -0.03, -0.15, +4.00, +3.90, +3.89, +3.88 |
| G3, reference (N3-21), for comparison | | -16.7, -316.7, -33.3, -125.0, -141.7, -150.0, -33.3, -50.0, -58.3, -75.0 | -0.03, -0.14, +4.21, +4.14, +4.13, +4.11, +17.4, +17.3, +17.7, +17.7 |
| G3, native det, first six | | the same six as the reference, to the frame | |

The deterministic G4 and G3 rows are the reference's (the frames 42, 46, 903, 1802, 1576, 3137, 1578, 3139, 1579, 3140 of
N3-21 are the native run's too). The changes over 66.7 ms are the first aDemo (the intro movie's start), the three after
the intro movie (aDemo, aGameSys, aVillage of G3 and G4; in G2 the same three are 41.7 to 66.7 ms) and, in G2, the two of the
opening.

`--io-latency none` (reported). Items and the numbers that differ from `azahar`:

| Session | Mode | scenes | times | checkpoints | observables | dsp | writes |
|---|---|---|---|---|---|---|---|
| G1 | det | the runner hangs at boot (no state record in 150 s of CPU) | | | | | |
| G4 | det | the runner hangs at boot (timed out at 100 s per rate, no state record) | | | | | |
| G1 | rt | pass (2) | **FAIL** 100.0 ms | pass | n/a | pass +9 | pass (11) |
| G2 | rt | pass (8) | **FAIL** 166.7 ms (4 of 8) | pass (7) | position **FAIL** (the same jump), quest n/a | **FAIL** +648 | pass (13, 1 masked) |
| G4 | rt | pass (6) | **FAIL** 166.7 ms (4 of 6) | pass | n/a | **FAIL** +640 | pass (2; 1 masked by the proposed lines, equal without) |
| G3 | rt, pair 1 | **FAIL** (60 fps run stays in the village, 6 changes) | n/a | **FAIL** | n/a | **FAIL** | pass |
| G3 | rt, pair 2 | pass (10 changes in both) | **FAIL** 1,016.7 ms (7 of 10 over) | pass (9) | position **FAIL**, quest **FAIL** (16.95 s) | **FAIL** (+28.9 s of clock) | pass |

Real-time G3 runs, every run (the hunt = 10 real scene changes): `azahar` pair 1: 60 yes, 120 no; pair 2: yes, no (the 120 run
took 454 s of host time); pair 3: no (408 s), yes; `none` pair 1: no, yes; pair 2: yes, yes.

Hunt observables from the real-time runs of G3 that reached the hunt. The `azahar` pairs never had both runs in the hunt,
so the first two rows pair the 60 fps run of one pair with the 120 fps run of another (two independent runs, which is
what the tool compares anyway); the third row is one `none` pair; the reference's row is N3-21's:

| Pair | scene changes over 66.7 ms | largest time difference | position, seconds over / of | largest distance | quest counter, seconds over 1 s / of | largest quest difference |
|---|---|---|---|---|---|---|
| 60 fps run of pair 2 with the 120 fps run of pair 3 | 5 of 10 | 150.0 ms | 55 / 64 | 2,234 units | 53 / 65 | 16.55 s |
| 60 fps run of pair 1 with the 120 fps run of pair 3 | 4 of 10 | 141.7 ms | 55 / 64 | 1,959 units | 53 / 65 | 16.35 s |
| `none` pair 2 | 7 of 10 | 1,016.7 ms | 61 / 64 | 5,603 units | 65 / 65 | 16.95 s |
| reference, N3-21 (G3) | 5 of 10 | 316.7 ms | 59 / 63 | 3,627 units | 19 / 65 | 1.07 s |

## The spread D70's numbers come from, and the proposal

D70 (Open) says: same scene sequence; each change within 4 frames at 60 fps; equal `eq` values at the checkpoints; the
position within 2 % of the last second's travel plus 0.05 units; the quest clock within 1 s; the DSP stream by D51's rule;
`fs:` Writes by D52's rule. What the table above measured, item by item, with the proposal for each (the numbers are the
orchestrator's to settle; none of the tool's defaults was changed):

1. **Scene sequence.** Equal in G1, G2 and G4 in all runs, real time and deterministic, `azahar` and `none`. Different in G3
   at 120 fps in the deterministic mode, reproducibly. Proposal: keep "the same sequence, never tolerated", **gate it in the
   deterministic mode** (reproducible, seconds long) and report real time; G3 at 120 fps stays `needs-decision` until the hook
   is found (finding 2).
2. **Time of a scene change.** The measured differences are 16.7 ms (the first change) to 316.7 ms; the largest of the
   deterministic runs, the reference's own, is 19 frames at 60 fps (G4 and G3's first aDemo); G2's opening gives 17 and
   18 frames in the deterministic mode. The real-time runs of G1, G2 and G4 stay at 10 frames (166.7 ms) or less; the real-time
   G3 hunt pair of `none` has 61 frames (1,016.7 ms), which is host noise (finding 3). All the changes over 4 frames are the
   first aDemo, the three after the intro movie and the opening of G2. Proposal: **20 frames at 60 fps (333.3 ms)**, D50's
   rule (the largest measured plus one) applied to the deterministic gate, which the real-time runs of G1, G2 and G4 are
   inside.
3. **Checkpoints.** The `eq` observables are equal at every checkpoint of every run in which the scene sequence is equal.
   Proposal: unchanged.
4. **Position and quest clock.** The spec's numbers cannot be met, and not because of the rate: two runs at the same rate
   differ by as much (finding 4: 2,460 and 2,233 units; 0.20 s and 15.8 s), and the reference's run of N3-21 has 3,627
   units and 1.07 s. The deterministic G3 does not reach the hunt at 120 fps, so the one measurement that is free of host
   noise cannot be made. Proposal: **position and quest clock are reported, not gated**, until a deterministic pair reaches
   the hunt at both rates; then the numbers of that pair, with the same-rate pair of the deterministic mode (which is
   zero by construction) as the floor, are the table to settle them from. The tool's rows (`compare.json`) have the
   per-second values for both.
5. **DSP stream.** The window of G1 (before the clock offset arises) gives shifts of +9 and +17 DSP frames in real time and
   +57 in the deterministic mode (the game clock offset at the window is +0.41, +0.33 and +0.61 s); every window after the
   intro movie gives shifts of 640 to 4,552 DSP frames or no buffer in common, with the clock offset at the window 4 to
   22 s (finding 1). Proposal: gate the item **on windows taken where the clock offset between the runs is under 1 s (G1's
   `dsp:1240:300`) with `--max-shift 58`** (+57 measured plus one, D50's rule), and report the windows after it with
   their offset; the audio of one game frame at 120 fps is as far from the 60 fps audio as the clock is.
6. **`fs:` Writes.** Equal by D52's rule in every run once the two ranges of `fs_write_masks.txt` that G2 has are also given
   to G3 and G4. Proposed lines for `config/native/fs_write_masks.txt`, the reason as G2's:
   ```
   G3 81408 0   0 8     block 0 of the user slot: the byte-sum checksum of the plaintext (as the G2 line)
   G3 81408 0 280 16    blocks 35 and 36 of the user slot: the clock-derived words (as the G2 line)
   G4 81408 0   0 8     block 0 of the user slot: the byte-sum checksum of the plaintext (as the G2 line)
   G4 81408 0 280 16    blocks 35 and 36 of the user slot: the clock-derived words (as the G2 line)
   ```
   The runs used a copy with these four lines added (`build/fps/fs_write_masks_n322.txt`); the differing bytes without
   masks are only 0 to 7 and 280 to 287, in every pair.

## D71: `--io-latency none` for players

D71 (already marked Decided, 2026-10-21) says `none` can be offered only if the audit shows it passes every gated item,
otherwise `azahar` for every player. **`none` does not pass.** In real time it fails `times` in G1, G2 and G4 as `azahar`
does (100.0, 166.7 and 166.7 ms against 91.7, 133.3 and 158.3 ms), fails `dsp` after the boot as `azahar` does, and on G3
it is no more stable (one pair has the 60 fps run stay in the village, the other reaches the hunt with the largest
scene-time difference of the whole audit, 1,016.7 ms, and a 16.95 s quest difference). It passes `writes` without the masks
where `azahar` needs them, which is the clock readings falling in the same second and not a stability gain. And in the
deterministic mode, the one that gates, it does not run at all: `--deterministic --io-latency none` makes the runner spin at
boot (G1 and G4). Verdict: **keep `azahar` for every player** (option (a)); the audit gives no reason for (b) or (c).
The hang of `--deterministic` with `none` is for the orchestrator to open as a defect of the file system's reply
scheduling (the replies are deferred to the scheduler thread in that mode).

## Repeating it

Every run of this note is `fps_audit.py run S ... --out build/fps/<tag>` followed by `compare`; the lock
(`--lock build/locks/emulator`) and the llvm-mingw `--path` are the only environment. Real time: the `--native`
library `build/n3-17-user/recomp/0004000000126300_native.dll`. Deterministic: the `_native_det` library
`build/game-det/recomp/0004000000126300_native_det.dll` with `--runner-arg=--deterministic` (and `--runner-arg=--seed
--runner-arg=N`). DSP windows added to the sessions that have none: G2 `--dsp 6000:300`, G3 `--dsp 16000:300` (the hunt) or
`6000:300` (the village), G4 `--dsp 2000:300`. For G3, `compare --require position,quest_time,dsp --position-until 15862`
(15862 is N3-21's last frame before the first attack press moves the hunter). Do not run other work on the PC during
real-time runs: the guest clock follows the host's.
