# The 60/120 fps audit (N3-22)

ROADMAP section 6 asks for the same session at 60 and at 120 fps to give the same game, "positions and times within the
tolerance the audit sets". `tools/native/fps_audit.py` plays a golden session twice on the native runner, headless, and
compares the two runs in six items. This note is the method, the tables the first run of it made on the maintainer's PC
(2026-10-31, base `283f6b5`), the measured spread D70's numbers come from, and the verdict on `--io-latency none` for D71.
Only counts, frames, times and shifts are written here: no memory contents, no game text, nothing of a save.

**Outcome: needs-decision.** The tool is done and tested; the audit does not pass, in any mode, for any of G1 to G4, and two
findings are not a matter of tolerance (*What the runs found*): the scripted G3 does not reach the Arena at 120 fps on the
native runtime although the reference does (twice, identically; a deterministic native run reproduces the failure, so it
is not noise), and the game's clock is 4 s ahead of its frame count from the intro movie on at 120 fps, on the reference
as well, which moves the audio of a game frame by hundreds of DSP frames. D70's numbers are proposed below; D70 and D71
are the orchestrator's to mark Decided.

**The PC during the runs.** The real-time runs of 2026-10-31 (every row marked **rt**, and every `none` row except the
deterministic ones) were taken while the PC was busy with the orchestrator's builds; the host times of 454 s and 408 s
against 305 s for G3 show it. In the real-time mode the guest clock follows the host's, so those rows carry host load
and are qualified as such below. The deterministic runs (**det**) do not depend on the host (time is the cycles
charged). The second reference run of G3 at 120 fps and the timed G1 det-none pair were made later with no build, test
or other run on the PC (the maintainer's desktop programs were open: an animated wallpaper used about 1.3 of the 12
logical processors).

## Using it

```
python tools/native/fps_audit.py scale tools/native/golden/G3.txt --factor 2 -o OUT.txt      # observable_probe.py's scale
python tools/native/fps_audit.py run S --runner build/native/bin/mh4u_native_runner.exe --program build/native-game \
    --code <library> --user-before build/golden/S/user-before --io-latency azahar --out build/fps/S \
    --path <llvm-mingw bin> --lock build/locks/emulator [--deterministic [--seed N]] [--dsp START:FRAMES] \
    [--timeout SECONDS] [--runner-arg=ARG ...]
python tools/native/fps_audit.py compare build/fps/S [--require position,quest_time,dsp,writes] [--position-until 15862] \
    [--fs-write-mask FILE]
```

`run` plays S at `--set 0:fps=60` with the session and at `--set 0:fps=120` with the session scaled by 2 (every frame key,
hold and checkpoint frame doubled, milliseconds unchanged: `observable_probe.py scale`), each from a fresh copy of
`user-before`, with `TZ=UTC0`, the session's `# init-time`, `--state-trace` (the observable file of the repository plus N3-21's
four lines as `ignore`, written to `OUT/observable.txt`; `config/**` is not edited), `--trace-ipc --trace-ipc-full` and one
`--capture-dsp` per DSP capture (the start scaled). `--deterministic` (with the `_native_det` library) and `--seed N` ask
for the runner's deterministic mode; `meta.json` records the mode and `compare` prints it in its first line, so a
deterministic and a real-time verdict cannot be confused (a `meta.json` written before the option existed, with
`--runner-arg=--deterministic`, reads as deterministic too). `--dsp START:FRAMES` adds a window (G2, G3 and G4 have none of
their own); `--runner-arg=ARG` passes another option to the runner as it is (`{out}` is the run's folder). `--timeout` is
per run, one hour by default (the slowest run of this note took 455 s). The emulator lock folder is waited for, created
and removed. Everything lands in `OUT/r60/`, `OUT/r120/` and `OUT/meta.json`; `--force` deletes OUT first only when it
holds nothing but what `run` writes. `compare` reads that folder and writes `OUT/compare.txt` and `OUT/compare.json`
(every number of the tables below); exit 0 when no item fails, 1 when one does, 2 for a usage problem or an unreadable
run.

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
| 4 | `observables` | every second of game time, in the hunt only (the scene aQuest: the chains name whatever sits there elsewhere, N3-21; `--hunt-scene any` lifts it): the position within 2 % of the path length of the 60 fps hunter in the last second plus 0.05 units; the quest counter within 1 s (60 units a second, N3-21). A counter that is constant in both runs compares nothing (`n/a`) unless its two constants are more than 1 s apart, which fails. The rows also have the distance with 4 frames of time slack, as information |
| 5 | `dsp` | `capfmt.diff_dsp_aligned` with `--max-shift 48` on each capture pair (D51). The detail line also gives the game clock offset between the runs at the window's first frame, in DSP frames, as information beside the measured shift: the two are not the same (finding 1) |
| 6 | `writes` | the `fs:USER` file Writes, per thread, in order and in full (offset, size, result, CRC-32 of the data), the lines of `fs_write_masks.txt` of the session (the base name: `G3` for `G3-x2`) zeroed first (`compare_ipc_traces.load`); a mask line that matches no Write is a failure |

## What the runs found

1. **The game's clock is ahead of its frame count at 120 fps, from the intro movie on.** At the same game time
   (frame / fps) the clock of the 120 fps run is about 4.0 s later in real time and 4 to 9 s later in the deterministic
   mode in the first minutes (22 s by 270 s of G3), and N3-21's reference runs show the same (+4.2 s at 15 s of game time,
   +17 s at 224 s). The offset arises between the first aDemo (the intro movie's start, -0.14 s) and the next change
   (+4.2 s). It is on the reference too, so it is not the native runtime's. *Inference, not measured:* the frame counter
   advances by less than 120 per second of the clock while a movie or a load is on. This contradicts the premise of
   N3-22's trap that a movie's game time does not change with the rate: in frames over fps the scene changes after the
   movie are only 100 to 300 ms apart, but on the game's clock the intro movie lasts about 4 s longer at 120 fps. It is
   why item 5 cannot hold after the boot, and why the scene changes after the intro movie are 100 to 300 ms earlier at
   120 fps in frames over fps. **The audio does not move by the clock offset.** The measured DSP shift against the clock
   offset at the window's start, in DSP frames: G1 +9 of +85 (11 %, `none`), +17 of +68 (25 %, rt), +57 of +125 (46 %,
   det); G4 +640 of +833, +646 of +824 (77 %, 78 %); G2 +648 of +823, +671 of +812, +1262 of +1488 (79 %, 83 %, 85 %); G3's
   hunt window (det) +4552 of +4462 (102 %). So the shift is 11 to 102 % of the clock offset: neither the same game frame
   (100 %) nor the same moment of the clock (0 %), and 77 to 102 % after the intro movie.
2. **G3 does not reach the Arena at 120 fps on the native runtime, and the reference does.** In the deterministic mode
   (`--deterministic`, the `_native_det` library) the native 60 fps run is the reference's, frame for frame: all 18,001
   state records equal in every observable (N3-21's run r2), the tick offset at most 5,893 ticks (22 us) from frame 1 on,
   plus a constant 486,920 ticks (1.8 ms) at frame 0, the ten scene changes at the reference's frames. The 120 fps run
   against the reference's (N3-21's run r3): the compared lines (`scene`, `applet_state`, `frame_counter`, `quest_id`,
   `stage_loader_state`) and N3-21's `quest_time` are equal up to frame 26,919, and N3-21's three position lines up to
   28,405; `applet_state` and `frame_counter` never differ. The ignored `smain_delta` differs from frame 6,592 (54.9 s of
   game time), where the ticks start to drift: the tick offset is at most 5,825 ticks before that (the same 486,920 at
   frame 0), then 0.4 ms at 55.5 s, 3.7 ms at 75 s, 37 ms at 153 s, 2.5 s at 220 s, 5.2 s at 300 s. At frame 26,920
   the reference leaves the village and the native runtime never does (6 real scene changes, the run ends in the
   village; seeds 1, 2 and 3 of `--seed` give the same frames). The session is N3-21's, byte for byte (the scaled one too).
   The inputs reach the game as scripted (the HID capture shows the circle pad stretches of the walk at their scaled
   frames). **The reference side, repeated.** The reference had been run at 120 fps once (N3-21's r3), and its exactness
   at 120 fps had never been measured (N0-29 measured it at 60). One more reference run was made for this note,
   exactly as N3-21's "Repeating it" (the reference runner of `build/runner-win-n035`, `--cpu recomp`, the golden library,
   a fresh copy of G3's `user-before`, `--init-time 1767268800.5`, `--set 0:fps=120`, N3-21's `G3-x2.txt`, `--state-trace`
   with N3-21's observable file, `--shots DIR --shot-every 100000000 --draw-window 100`), under the emulator lock with
   the PC otherwise idle: it reaches the Arena again, 10 real scene changes at frames 2, 46, 1802, 3137, 3139, 3140,
   26920, 26922, 28207 and 28209, and its state trace equals r3's in all 36,001 records, every tick and every value
   (157 s of host time against 235 s). **The cause is open.** The divergence starts as a tick drift of the native
   deterministic runtime against the reference in the long talk of the village, about 170 s of game time before the
   Arena and 180 s before the quest loads. The quest-loading race (the game behaviour N3-22's trap names) is one
   candidate; native timing is the other, and the reference reaching the Arena twice with the same session points at
   native timing first (an inference). Either way it is the orchestrator's to open (a hook if it is the game's own
   behaviour, a native-runtime defect otherwise).
3. **Real time is a coin flip on G3, in both directions, on a loaded PC.** Ten real-time runs (five pairs; the runner is
   paced to real time), all taken while the PC was busy with other builds: the 60 fps run reaches the hunt in 3 and the
   120 fps run in 3, and in four of the five pairs exactly one of the two does not. Two of the four failing runs were
   slow ones (host time 454 s and 408 s against 305 s). N1-26 measured 10 of 10 at 60 fps on an idle PC. So this says
   that a single real-time pair cannot gate G3 **on a loaded PC**; how often G3 fails in real time on an idle PC was not
   measured here and should be before D70 relies on it. The deterministic mode is the gate proposed for G3 because it
   does not depend on the host at all (a pair takes 13 s for G1 and G4, 30 s for G2 and 75 s for G3 of host time).
4. **On a loaded PC the hunter's position is as unrepeatable between two runs at one rate as between two rates.**
   Measured while the PC was busy with other builds: at 60 fps two native runs end the 10 s walk (frame 14350, 600 frames)
   98 units apart; two runs at 120 fps end it 1,280 units apart; the reference's own 60 fps run (N3-21) ends it 840 units
   from a native 60 fps run. Over the 65 seconds of the hunt two native 60 fps runs are up to 2,460 units apart (median
   100), two 120 fps runs up to 2,233 (median 1,206), and a 60 fps run against a 120 fps run up to 1,959 to 5,603 (the
   table below). The quest counter between two 60 fps runs is within 0.20 s, between two 120 fps runs up to 15.8 s (the
   talk that pauses it starts at other times). These spreads carry the host's load and are not a basis for D70's numbers.
5. **`--io-latency none` hangs the deterministic mode at boot.** G1 and G4 with `--deterministic --io-latency none`
   write no state record and spin. With the tool's own timeout: G4 (`--timeout 100`) and G1 (`--timeout 300`) time out at
   both rates with no state record, `compare` verdict FAIL (runs); G1's IPC trace stops at request 23, an `fs:USER`
   OpenFile, at both rates. With `azahar` the same runs take 5 s and 4 s. A first G1 pair (`build/fps/G1-det-none`) was
   killed by hand after about 2 minutes at 60 fps, before `run` wrote its `meta.json`, so `compare` cannot read it; the
   timed pair (`build/fps/G1-det-none-t300`) replaces it. G2 and G3 were not run with `none` in the deterministic mode
   (the hang comes at boot, before any input of a session, so they would hang too: an inference). So `none` has
   real-time runs only.
6. **G3 and G4 need the same two write masks as G2.** The Write of 81,408 bytes at offset 0 differs between the rates in
   bytes 0 to 7 and 280 to 287 and nowhere else (the checksum and the clock-derived words of `fs_write_masks.txt`'s G2
   lines; the 120 fps run reads the clock at other frames). With `none` the clock readings fall in the same second and
   the Write is equal without a mask.

## Tables

Runs of 2026-10-31 on the maintainer's PC, the native runner and code library, game folder `build/native-game`, a copy of the
recording's `user-before` per run (G3 and G4 with the maintainer's save), `# init-time` of the session where it has one.
Host seconds are for the 60 and the 120 fps run. Mode: **det** is `--deterministic` with the `_native_det` library, **rt**
the real-time `--native` library. **Every rt row of this note was measured while the PC was busy with other builds**
(*The PC during the runs*): its times, shifts and spreads include host load; the det rows do not depend on it.

Items by session, `--io-latency azahar` (what D70 gates). Counts are of the 60 fps run and the 120 fps run.

| Session | Mode | host s | scenes | times (limit 66.7 ms) | checkpoints | observables | dsp (limit 48) | writes |
|---|---|---|---|---|---|---|---|---|
| G1 | det | 5 + 6 | pass (2 changes) | **FAIL** 275.0 ms | pass (2) | n/a | **FAIL** +57 | pass (11) |
| G1 | rt (PC loaded) | 31 + 35 | pass (2) | **FAIL** 91.7 ms | pass (2) | n/a | pass +17 | pass (11) |
| G2 | det | 10 + 20 | pass (8) | **FAIL** 300.0 ms (3 of 8 over) | pass (7) | position **FAIL** (1 jump), quest n/a | **FAIL** +1262 | pass (13, 1 masked) |
| G2 | rt (PC loaded) | 153 + 157 | pass (8) | **FAIL** 133.3 ms (3 of 8) | pass (7) | position **FAIL** (1 jump), quest n/a | **FAIL** +671 | pass (13, 1 masked) |
| G4 | det | 4 + 9 | pass (6) | **FAIL** 316.7 ms (4 of 6) | pass (4) | n/a | **FAIL** no buffer in common | pass (2, 1 masked by the proposed lines) |
| G4 | rt (PC loaded) | 53 + 57 | pass (6) | **FAIL** 158.3 ms (4 of 6) | pass (4) | n/a | **FAIL** +646 | pass (2, 1 masked by the proposed lines) |
| G3 | det | 25 + 49 | **FAIL** (10 changes, 6) | n/a | **FAIL** (4 of 9) | n/a | **FAIL** no buffer in common | pass (2, 1 masked by the proposed lines) |
| G3 | rt (PC loaded), 3 pairs | 303 to 408 + 307 to 455 | **FAIL** in all three (one run of each pair stays in the village) | n/a | **FAIL** | n/a | **FAIL** | pass (2, 1 masked) |

G2's position failure is one second of 63: the hunter in the opening jumps 673.6 units, and the jump falls in another
second's sample at the two rates (more than 4 frames apart: the slack column does not clear it); the other 62 seconds
are equal to 0.05 units. Its quest counter does not move in the opening, with the same constant in both runs (n/a).

Scene changes, game time of the 120 fps run minus the 60 fps run in ms (frame / fps; negative: earlier at 120 fps), the
game clock offset beside it, for `azahar`. The changes are, in order, aGameSys, aDemo, then the ones named in the sessions.
The rt rows were measured on the loaded PC.

| Session | mode | differences in ms, in order | game clock offsets, s |
|---|---|---|---|
| G1 | det | -16.7, -275.0 | -0.03, -0.11 |
| G1 | rt (PC loaded) | -16.7, -91.7 | -0.13, -0.21 |
| G2 | det | -16.7, -275.0, -16.7, -41.7, -58.3, -66.7, -283.3, -300.0 | -0.03, -0.11, +4.26, +6.82, +6.81, +6.79, +6.75, +6.73 |
| G2 | rt (PC loaded) | -16.7, -91.7, -33.3, -41.7, -58.3, -66.7, -116.7, -133.3 | -0.02, -0.09, +4.16, +4.08, +4.06, +4.06, +4.02, +4.01 |
| G4 | det | -16.7, -316.7, -33.3, -125.0, -141.7, -150.0 | -0.03, -0.14, +4.21, +4.14, +4.13, +4.11 |
| G4 | rt (PC loaded) | -16.7, -158.3, -33.3, -108.3, -125.0, -133.3 | -0.03, -0.15, +4.00, +3.90, +3.89, +3.88 |
| G3, reference (N3-21), for comparison | | -16.7, -316.7, -33.3, -125.0, -141.7, -150.0, -33.3, -50.0, -58.3, -75.0 | -0.03, -0.14, +4.21, +4.14, +4.13, +4.11, +17.4, +17.3, +17.7, +17.7 |
| G3, native det, first six | | the same six as the reference, to the frame | |

The deterministic G4 and G3 rows are the reference's (the frames 42, 46, 903, 1802, 1576, 3137, 1578, 3139, 1579, 3140 of
N3-21 are the native run's too). The changes over 66.7 ms are the first aDemo (the intro movie's start), the three after
the intro movie (aDemo, aGameSys, aVillage of G3 and G4; in G2 the same three are 41.7 to 66.7 ms) and, in G2, the two of the
opening; on the reference, also G3's last change (aQuest, 75.0 ms).

`--io-latency none` (reported). Items and the numbers that differ from `azahar`. The rt rows were measured on the loaded
PC; the det rows with the tool's timeout:

| Session | Mode | scenes | times | checkpoints | observables | dsp | writes |
|---|---|---|---|---|---|---|---|
| G1 | det | **FAIL** (runs): the runner hangs at boot; `run --timeout 300` timed out at both rates, no state record | | | | | |
| G4 | det | **FAIL** (runs): the runner hangs at boot; `run --timeout 100` timed out at both rates, no state record | | | | | |
| G1 | rt (PC loaded) | pass (2) | **FAIL** 100.0 ms | pass | n/a | pass +9 | pass (11) |
| G2 | rt (PC loaded) | pass (8) | **FAIL** 166.7 ms (4 of 8) | pass (7) | position **FAIL** (the same jump), quest n/a | **FAIL** +648 | pass (13, 1 masked) |
| G4 | rt (PC loaded) | pass (6) | **FAIL** 166.7 ms (4 of 6) | pass | n/a | **FAIL** +640 | pass (2; 1 masked by the proposed lines, equal without) |
| G3 | rt (PC loaded), pair 1 | **FAIL** (60 fps run stays in the village, 6 changes) | n/a | **FAIL** | n/a | **FAIL** | pass |
| G3 | rt (PC loaded), pair 2 | pass (10 changes in both) | **FAIL** 1,016.7 ms (7 of 10 over) | pass (9) | position **FAIL**, quest **FAIL** (16.95 s) | **FAIL** (+28.9 s of clock) | pass |

Real-time G3 runs, every run, all on the loaded PC (the hunt = 10 real scene changes): `azahar` pair 1: 60 yes, 120 no;
pair 2: yes, no (the 120 run took 454 s of host time); pair 3: no (408 s), yes; `none` pair 1: no, yes; pair 2: yes, yes.

Hunt observables from the real-time runs of G3 that reached the hunt (loaded PC). The `azahar` pairs never had both runs in
the hunt, so the first two rows pair the 60 fps run of one pair with the 120 fps run of another (two independent runs,
which is what the tool compares anyway); the third row is one `none` pair; the reference's row is N3-21's:

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
   at 120 fps in the deterministic mode, reproducibly; in real time G3 failed in four of five pairs on the loaded PC.
   Proposal: keep "the same sequence, never tolerated", and **gate it in both modes for G1, G2 and G4** (it passed in
   every run of the three), and in the deterministic mode for G3; G3's real-time sequence is reported, not gated, until it
   is measured on an idle PC. G3 at 120 fps stays `needs-decision` until finding 2's cause is found.
2. **Time of a scene change.** The changes over 4 frames are always the same ones, and systematic (the deterministic mode
   gives the same frames run after run; G3's with three seeds): the first aDemo, the three after the intro movie, G2's
   opening and, on the reference, G3's aQuest. Every other change is 1 to 3 frames. Proposal: **4 frames (66.7 ms) by
   default, with named exceptions at the largest measured value plus one frame (D50's rule)**, taken from the
   deterministic runs, which carry no host noise:

   | Change | largest measured (det; the reference for G3's changes the native det run does not reach) | proposed limit |
   |---|---|---|
   | the first aDemo (the intro movie's start), every session | 316.7 ms (19 frames, G4 and G3) | 333.3 ms (20 frames) |
   | the three after the intro movie (aDemo, aGameSys, aVillage), every session | 150.0 ms (9 frames, G4 and G3) | 166.7 ms (10 frames) |
   | G2's two changes of the opening | 300.0 ms (18 frames) | 316.7 ms (19 frames) |
   | G3's aGameSys and aQuest of the quest load | 75.0 ms (4.5 frames, the reference only: the native det run does not reach them) | 91.7 ms (5.5 frames), to be confirmed from the first native deterministic pair that reaches them |
   | every other change | 50.0 ms (3 frames) | 66.7 ms (4 frames, D70's first guess) |

   Every real-time run of G1, G2 and G4 (loaded PC) is inside these limits too (largest: 166.7 ms on the first aDemo of
   G4 and on G2's opening, 133.3 ms after the intro movie, 33.3 ms elsewhere). A blanket 20 frames is not proposed: it would loosen the other changes,
   now at 1 to 3 frames, five times. The exceptions sit where the clock offset of finding 1 arises (the movie, the loads),
   so they may become one rule once finding 1's mechanism is measured rather than inferred; the tool would need a
   per-change tolerance for them (today it has one number, `--time-frames`).
3. **Checkpoints.** The `eq` observables are equal at every checkpoint of every run in which the scene sequence is equal.
   Proposal: unchanged.
4. **Position and quest clock.** The spec's numbers cannot be met, and not because of the rate: on the loaded PC two runs
   at the same rate differ by as much (finding 4: 2,460 and 2,233 units; 0.20 s and 15.8 s), and the reference's run of
   N3-21 has 3,627 units and 1.07 s. The deterministic G3 does not reach the hunt at 120 fps, so the one measurement that is
   free of host noise cannot be made. Proposal: **position and quest clock are reported, not gated, as an explicitly open
   part of D70**, with its trigger: a deterministic pair that reaches the hunt at both rates; then the numbers of that pair,
   with the same-rate pair of the deterministic mode (zero by construction) as the floor, are the table to settle them
   from. The tool's rows (`compare.json`) have the per-second values for both. What it means for ROADMAP section 6: its
   high-frame-rate rule asks for "positions and times within the tolerance the audit sets"; until the trigger, the gate
   covers the scene sequence, the scene times and the checkpoints at 120 fps, but **not the hunter's position or the
   quest's clock**: a 120 fps build that moves the hunter elsewhere or runs the quest clock at another rate would pass.
   So D70 cannot be fully Decided on this note: the scene, time, checkpoint and write parts can, the position and quest
   clock part stays Open with that trigger.
5. **DSP stream.** The window of G1 (before the clock offset arises) gives shifts of +9 and +17 DSP frames in real time
   (loaded PC) and +57 in the deterministic mode; every window after the intro movie gives shifts of 640 to 4,552 DSP frames
   or no buffer in common, with the clock offset at the window 4 to 22 s (finding 1). The shift is measured on the DSP
   frame counters, so it does not depend on where the window starts: a window that starts where the 120 fps run's clock
   equals the 60 fps window's (or a wider window) gives overlap where there is none now (G3 and G4 det, "no buffer in
   common"), but not a smaller shift, which after the intro movie is 77 to 102 % of the clock offset (finding 1). Proposal:
   **gate G1's boot window** (`dsp:1240:300`) with D51's 48 in real time (+9 and +17), and in the deterministic mode at
   **58** (+57 measured plus one, D50's rule; one sample, so a named exception rather than a new bound); **report every
   window after the intro movie**, with its offset, as an open part of D70 to settle after finding 1 is explained (a
   clock-aligned or wider window is the tool change that would let those windows compare at all).
6. **`fs:` Writes.** Equal by D52's rule in every run once the two ranges of `fs_write_masks.txt` that G2 has are also given
   to G3 and G4. Proposed lines for `config/native/fs_write_masks.txt` (the orchestrator's file), the reason as G2's:
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
otherwise `azahar` for every player. **Neither `none` nor `azahar` passes the audit** (times and dsp fail in real time with
both, and G3 fails with both), so the audit does not tell the two apart: **there is no evidence for `none`, and option (a),
`azahar` for every player, stands.** The real-time differences between the two (100.0, 166.7 and 166.7 ms against 91.7,
133.3 and 158.3 ms on the scene times; G3's pair with 1,016.7 ms) were measured on the loaded PC and are within the
run-to-run noise of that PC (finding 3, finding 4), so they are not evidence either way. `none` passes `writes` without
the masks where `azahar` needs them, which is the clock readings falling in the same second and not a stability gain. The
hang of `--deterministic --io-latency none` at boot (finding 5) is a defect of a runner mode players never use (the
deterministic mode is a test mode), not evidence about `none` for players; it is for the orchestrator to open as a defect
of the file system's reply scheduling in that mode (the replies are deferred to the scheduler thread there; the IPC trace
stops at an `fs:USER` OpenFile).

## Repeating it

Every run of this note is `fps_audit.py run S ... --out build/fps/<tag>` followed by `compare`; the lock
(`--lock build/locks/emulator`) and the llvm-mingw `--path` are the only environment. Real time: the `--native`
library `build/n3-17-user/recomp/0004000000126300_native.dll`. Deterministic: the `_native_det` library
`build/game-det/recomp/0004000000126300_native_det.dll` with `--deterministic` (the runs of 2026-10-31 passed it as
`--runner-arg=--deterministic`, and the seed as `--runner-arg=--seed --runner-arg=N`; `compare` reads both as the
deterministic mode). DSP windows added to the sessions that have none: G2 `--dsp 6000:300`, G3 `--dsp 16000:300` (the hunt)
or `6000:300` (the village), G4 `--dsp 2000:300`. For G3, `compare --require position,quest_time,dsp --position-until 15862`
(15862 is N3-21's last frame before the first attack press moves the hunter). The timed G1 det-none pair:
`run G1 ... --io-latency none --deterministic --timeout 300 --out build/fps/G1-det-none-t300`. Do not run other work on the
PC during real-time runs: the guest clock follows the host's (the real-time rows of this note did not meet that).

The record-by-record numbers of finding 2 (records equal, first difference per observable, tick offsets) are not an item
of the tool: they compare a native state trace with a reference one at the same frame, every value and the `t=` ticks
(the native det traces `build/fps/G3-det/r60/state.txt` and `r120/state.txt` against N3-21's `build/n3-21/r2` and `r3`,
and r3 against the new reference run `build/fps/G3-ref120-r4`). The helper scripts that did it print frames and counts
only and stay outside the repository (`build/fps/an_*.py` and the session's scratch folder). The second reference run is
N3-21's `build/n3-21/run_ref.ps1` with `-Session build/n3-21/sessions/G3-x2.txt -Frames 36000 -Observable
build/n3-21/obs_n321.txt -Fps 120 -Extra --shots,<out>/shots,--shot-every,100000000,--draw-window,100`, which creates and
removes the emulator lock itself.
