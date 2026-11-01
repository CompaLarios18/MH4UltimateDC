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
are the orchestrator's to mark Decided. *(2026-10-31, N3-32: the first of the two, finding 2, was native timing and is
fixed: the deterministic G3 now reaches the Arena at 120 fps as the reference does. Finding 1 stands; D70 stays Open
until the audit is run again on an idle PC.)* *(2026-10-31, later: run again, *Runs on an idle PC*. D70 is **Decided**
on it: the proposal below, with G3's real-time sequence and observables reported, and the position and quest clock
gated in the deterministic mode at the reference pair's own values (`DECISIONS.md`, D70). D71 stands. The tool's
defaults are still D70's first guesses until N3-34.)*

**The PC during the runs.** The real-time runs of 2026-10-31 (every row marked **rt**, and every `none` row except the
deterministic ones) were taken while the PC was busy with the orchestrator's builds; the host times of 454 s and 408 s
against 305 s for G3 show it. In the real-time mode the guest clock follows the host's, so those rows carry host load
and are qualified as such below. The deterministic runs (**det**) do not depend on the host (time is the cycles
charged). The second reference run of G3 at 120 fps and the timed G1 det-none pair were made later with no build, test
or other run on the PC (the maintainer's desktop programs were open: an animated wallpaper used about 1.3 of the 12
logical processors).
*(2026-10-31, later)* Every row was run again on an idle PC at native `4a6eaf4`: *Runs on an idle PC* below.

## Using it

```
python tools/native/fps_audit.py scale tools/native/golden/G3.txt --factor 2 -o OUT.txt      # observable_probe.py's scale
python tools/native/fps_audit.py run S --runner build/native/bin/mh4u_native_runner.exe --program build/native-game \
    --code <library> --user-before build/golden/S/user-before --io-latency azahar --out build/fps/S \
    --path <llvm-mingw bin> --lock build/locks/emulator [--deterministic [--seed N]] [--dsp START:FRAMES] \
    [--timeout SECONDS] [--runner-arg=ARG ...]
python tools/native/fps_audit.py compare build/fps/S [--require ITEMS] [--position-until FRAME] [--hunt-scene VALUE|any] \
    [--fs-write-mask FILE|none] [--time-frames N] [--position-relative R] [--position-absolute UNITS] \
    [--quest-seconds S] [--quest-units U] [--max-shift N] [--brief]
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
(every number of the tables below) into it.

The runner needs the llvm-mingw `bin` folder on `PATH` (`--path`), or it waits on a hidden missing-DLL dialog.

**`compare` gives D70's verdict with no option** (N3-34; *The items* below describe the tool before it, with D70's first
guesses as defaults): exit 0 when every **gated** item passes, 1 when one fails, 2 for a usage problem or an unreadable
run. The second line of `compare.txt` names the rules applied, the third the bounds; each item line says `gated` or
`reported`; a reported item is printed with its numbers and, when it fails, named on the verdict line (`verdict: pass
(reported: dsp); 6 gated, 1 reported`). The verdict line counts the gated items only. Which items gate follows from
`meta.json`'s session, `mode` and `io_latency`:

| The pair | gated | reported |
|---|---|---|
| `--io-latency none`, either mode | nothing | every item (D71) |
| `azahar`, real time, G1, G2 and G4 | runs, scenes, times, checkpoints, dsp, writes | observables |
| `azahar`, real time, G3 | runs, writes | scenes, times, checkpoints, observables, dsp |
| `azahar`, deterministic, G1 to G4 | every item | none |

In both modes `dsp` gates only G1's boot window (`dsp:1240:300` of `G1.txt`); every other window (G2's 6000, G3's 16000,
G4's 2000) is reported, with the game clock's offset at its start. A session the tool does not name (G6, G8) takes G1's
rules (its DSP windows are reported) and the 4-frame limit for every scene change; N3-26 names its sessions if they need
other rules.

The defaults are D70's numbers; each option overrides one of them, for a look at other values (a verdict with an
override is not D70's verdict, and none of these numbers is retuned to make a session pass):

| What | Default (D70) | Option |
|---|---|---|
| a scene change's time, frames at 60 fps | 4 (66.7 ms), except, by session and change number (from 0, as `compare.txt` numbers them): #1 of G1 to G4 (the first aDemo) 20 (333.3 ms); #3 to #5 of G2, G3 and G4 (the three into the village) 10 (166.7 ms); G2's #6 and #7 (the opening) 19 (316.7 ms); G3's #8 and #9 (aGameSys and aQuest of the quest load) 5.5 (91.7 ms). `compare.txt` prints each change's limit beside its difference | `--time-frames N`: one limit for every change, and the verdict line says so |
| the hunter's position, each second in the hunt | at most 3,700 units, no part relative to the last second's travel | `--position-absolute`, `--position-relative` |
| the quest clock, each second in the hunt | at most 1.09 s (60 counter units a second) | `--quest-seconds` (`--quest-units`) |
| the DSP shift (D51) | 48 DSP frames in real time, 58 in the deterministic mode | `--max-shift` |
| items that must have something to compare (an `n/a` fails) | G3: position, quest_time, dsp; the others none | `--require` replaces it (`--require ""` for none) |
| the position up to a frame (the walk before the first attack, reported in the observables' rows) | G3: 15862; the others none | `--position-until` |
| the scene the position and the quest clock are compared in | aQuest, the hunt | `--hunt-scene` |
| the `fs:` write masks | `config/native/fs_write_masks.txt` | `--fs-write-mask FILE` or `none` |

`compare.json` has `"gated"` for each item, each scene change's `limit_frames`, `limit_ms` and `over`, the rules
(`rules`), the bounds used (`tolerances`), `require`, `position_until`, `verdict` and `reported_failures`. `--brief`
prints the item lines only on the terminal; `compare.txt` always has the details.

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
   det); G4 +640 of +833, +646 of +824 (77 %, 78 %); G2 +648 of +823, +671 of +812, +1262 of +1488 (79 %, 83 %, 85 %). So the shift is 11 to 85 % of the clock offset: neither
   the same game frame (100 %) nor the same moment of the clock (0 %), and 77 to 85 % after the intro movie. (G3's det
   window at frame 16000, +4552 of +4462, is left out: there the 120 fps run is still in the village (finding 2), so the
   two windows hold different scenes.)
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
   *(2026-10-31, N3-32)* **Found and fixed: native timing, not the quest-loading race.** The deterministic mode's slice
   was 1,145,774 ticks, 3 short of Azahar's `MAX_SLICE_LENGTH` (`BASE_CLOCK_RATE_ARM11 / 234`, 1,145,777), so every
   thread-safe `nwm::UDS` event came 6 ticks early on core 1; at 120 fps the fourth one, in frame 6,591, cut a stretch of
   core 0 short and core 0's clock stayed behind from there, which is the drift from frame 6,592 above. With the slice
   set to 1,145,777 (`docs/research/det-120fps.md`), the native deterministic G3 at 120 fps equals a third reference run
   at 120 fps (`build/n3-32/ref120`, itself equal to r3) in all 36,001 state records, every value, and reaches the Arena
   and the hunt at the reference's frames, with seeds 1, 2 and 3 too; the 60 fps run still equals r2 (18,001 records);
   the scheduler timelines of G1, G3 and G4 equal the recordings' in every event. The deterministic pair
   (`fps_audit.py run G3 ... --deterministic`, `build/fps/G3-det-n332`) passes `runs`, `scenes` (10 real changes in each
   run) and `checkpoints` (9 of 9); `times` and `observables` fail with the numbers of the reference's own pair (N3-21),
   because finding 1 is the reference's behaviour too, and `writes` passes with the proposed masks. Still open: the other
   rows of this note were not run again, and D70 stays Open until the audit is run again on an idle PC.
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

## Runs on an idle PC (2026-10-31, native 4a6eaf4)

Every row of *Tables* again, with nothing else of the port running on the PC, at native `4a6eaf4` (N3-32's slice fix,
N3-33's `--deterministic --io-latency none` fix, and PR #64's `RC_HOST_VERSION` 6). The runner was built from that tree
as the native-windows job builds it (`-DCMAKE_BUILD_TYPE=RelWithDebInfo -DMH4N_WERROR=ON`, llvm-mingw clang, the
`mh4u_native_runner` target, `build/n3-22b/b`), and both code libraries again with `tools/build_game.py --dump
baserom/update_v130.cia --native --no-mods` (`build/n3-22b/lib-rt`, 7 min) and the same with `--deterministic`
(`build/n3-22b/lib-det`, 7 min): the libraries of *Repeating it* were built for `RC_HOST_VERSION` 5, which a runner of
this base refuses (not tried here); a short G1 pair checked that it accepts both new ones. Then
`fps_audit.py run` and `compare` as *Repeating it* says, but with `--deterministic` for the det rows and with the new
libraries and timeouts, one pair at a time under the emulator lock, the masks copy
`build/fps/fs_write_masks_n322.txt`, into new folders `build/fps/<S>-det-idle`, `<S>-det-none-idle`, `<S>-rt-idle[-rN]`
and `<S>-rt-none-idle` (the deterministic runs before the real-time ones; nothing else of the port ran during the
real-time ones; other programs: *The PC's load*). Per-run timeout 900 s (det) and 1,800 s (rt): no run timed out, every
run exited 0.

**The PC's load.** The total CPU over 20 s before each real-time pair: G1 21.3 %; G3 pairs 1 to 5: 21.1, 22.2, 21.5,
20.8, 24.4 %; G2 19.7, G4 19.9 %; `none`: G1 20.3, G2 19.5, G4 18.8 %. That is the maintainer's desktop (the animated
wallpaper, Chrome, Steam, the graphics driver's panel) with no build, test or other run, and about 20 % rather than the
about 11 % (1.3 of the 12 logical processors, *The PC during the runs* above) of the reference run earlier that day.
Sampled every 10 s from the end of G3's pair 5 to the last pair: 25.3 % on average with no runner (18 samples, in the
20 s gaps between pairs and after the last one; the first after each run includes the run's end and `compare`, up to
40.8 %), 34.8 % with the runner (94 samples, at most 45.1 %). Against those five pairs' pre-run loads (19.6 % on
average) the runner adds about 15 %, about 1.8 of the 12 logical processors. **Two interruptions.** An unrelated
installer of the maintainer's ran during the 120 fps run of G3's pair 4. Its times are not in the logs: they were read
by hand during the session. Its processes started at about 18:41 local time, about 112 s into that run (the long talk
in the village; the run lasted from 18:39:19 to 18:44:28 by its files' times), and took every processor from about
18:44, the run's last ~30 s. The load was not sampled during pair 4 (the samples are taken before each pair). What the
logs show is the next sample, at 18:45:10, 20 s after pair 4 and its `compare` ended: 100 %; and the run took 309.4 s
against 306.5 s for the other four 120 fps runs. Pair 5 was held while the load was 100 % and then while a game and an
antivirus scan ran (18 minutes in all), and started at 24.4 % once the game had closed. So every real-time run below is
idle except pair 4's 120 fps run, which is marked.

Host seconds of the real-time runs are now steady: G3 303.1 to 303.4 at 60 fps and 306.5 at 120 fps (pair 4's 309.4),
G2 152.7 to 153.3 and 156.5 to 157.1, G4 52.3 to 52.4 and 56.2 to 56.4, G1 30.7 to 30.8 and 34.8 to 34.9.

Items by session, `--io-latency azahar`. Counts are of the 60 fps run and the 120 fps run.

| Session | Mode | host s | scenes | times (limit 66.7 ms) | checkpoints | observables | dsp (limit 48) | writes |
|---|---|---|---|---|---|---|---|---|
| G1 | det | 2.5 + 4.6 | pass (2 changes) | **FAIL** 275.0 ms | pass (2) | n/a | **FAIL** +57 | pass (11) |
| G1 | rt | 30.8 + 34.9 | pass (2) | **FAIL** 91.7 ms | pass (2) | n/a | pass +15 | pass (11) |
| G2 | det | 10.2 + 21.7 | pass (8) | **FAIL** 300.0 ms (3 of 8 over) | pass (7) | position **FAIL** (1 jump), quest n/a | **FAIL** +1262 | pass (13, 1 masked) |
| G2 | rt | 153.3 + 157.1 | pass (8) | **FAIL** 125.0 ms (3 of 8) | pass (7) | position **FAIL** (the jump), quest n/a | **FAIL** +683 | pass (13, 1 masked) |
| G4 | det | 4.5 + 8.5 | pass (6) | **FAIL** 316.7 ms (4 of 6) | pass (4) | n/a | **FAIL** no buffer in common | pass (2, 1 masked) |
| G4 | rt | 52.4 + 56.4 | pass (6) | **FAIL** 133.3 ms (4 of 6) | pass (4) | n/a | **FAIL** +651 | pass (2, 1 masked) |
| G3 | det, seed 0 and `--seed 1` | 25.9 + 48.5, 25.3 + 48.5 | pass (10) | **FAIL** 316.7 ms (5 of 10) | pass (9) | position **FAIL** (59 of 63 s), quest **FAIL** (19 of 65 s) | **FAIL** +3765 | pass (2, 1 masked) |
| G3 | rt, 5 pairs | 303.1 to 303.4 + 306.5 to 309.4 | **FAIL** in 4 (one run of the pair stays in the village); pass in 1 (both stay, 6 changes) | n/a in the 4; **FAIL** 133.3 ms (4 of 6) in pair 3 | **FAIL** (4 of 9) in all five | **FAIL** in all five (`--require`: no second in the hunt in both runs) | **FAIL** in all five | pass (2, 1 masked) in all five |

The two deterministic G3 pairs have equal state traces, byte for byte (seed 0 against seed 1, at each rate), and their
ten scene changes are N3-21's reference frames. G2's position failure is the same one second of the opening jump
(673.6 units) in every G2 row.

**G3 in real time on the idle PC: 4 runs of 10 reach the hunt** (10 real scene changes), and no pair has both:

| Pair | 60 fps run | 120 fps run |
|---|---|---|
| 1 | hunt | village (6 changes) |
| 2 | hunt | village |
| 3 | village | village |
| 4 | village | hunt (partly under the installer's load, by unlogged times: *The PC's load*) |
| 5 | hunt | village |

So 3 of 5 at 60 fps and 1 of 5 at 120 fps; of the four fully idle 120 fps runs, none reached the hunt. Every run that
stays in the village has the same six changes; the five 120 fps runs have them at the same frames (2, 38, 1802, 3154,
3156, 3157), and the five 60 fps runs at frames 2, 27 or 28, 902 or 903, and 1579 to 1586 for the three after the intro
movie; the three 60 fps runs that reach the hunt have its four changes at the same frames (13463, 13465, 14107, 14109).

`--io-latency none` (reported, D71):

| Session | Mode | host s | scenes | times | checkpoints | observables | dsp | writes |
|---|---|---|---|---|---|---|---|---|
| G1 | det | 2.5 + 4.6 | pass (2) | **FAIL** 266.7 ms | pass (2) | n/a | **FAIL** +57 | pass (11) |
| G4 | det | 4.2 + 8.7 | pass (6) | **FAIL** 2,650.0 ms (5 of 6) | pass (4) | n/a | **FAIL** no buffer in common | pass (2, 1 masked) |
| G1 | rt | 30.7 + 34.8 | pass (2) | **FAIL** 116.7 ms | pass (2) | n/a | pass +13 | pass (11) |
| G2 | rt | 152.7 + 156.5 | pass (8) | **FAIL** 150.0 ms (3 of 8) | pass (7) | position **FAIL** (the jump), quest n/a | **FAIL** +638 | pass (13, 1 masked) |
| G4 | rt | 52.3 + 56.2 | pass (6) | **FAIL** 166.7 ms (4 of 6) | pass (4) | n/a | **FAIL** +633 | pass (2; 1 masked, equal without) |

The deterministic G1 and G4 with `none` now run to the end (N3-33). The deterministic G4 with `none` fails the times by
2.65 s: its 60 fps run ends the intro movie at frame 903 (15.05 s, where every G3 and G4 `azahar` 60 fps run ends it:
902 in G3's pair 4; 1802, 15.02 s, at 120 fps) and its 120 fps run at frame 1502 (12.52 s), where G4's real-time `none`
run ends it (frames 753 and 1502). G2 ends it later, at frame 1652 or 1653 (3302 or 3303 at 120 fps), in every G2 row.

Scene changes, game time of the 120 fps run minus the 60 fps run in ms (frame / fps), the game clock offset beside it.
G3 in real time has no pair with both runs in the hunt; its rows pair the 60 fps run of one pair with pair 4's 120 fps
run (folders `build/fps/mix-idle-60rN-120r4`, made as the `mix-*` folders of the loaded runs).

| Session | mode | differences in ms, in order | game clock offsets, s |
|---|---|---|---|
| G1 | det | -16.7, -275.0 | -0.03, -0.11 |
| G1 | rt | -16.7, -91.7 | -0.01, -0.09 |
| G2 | det | -16.7, -275.0, -16.7, -41.7, -58.3, -66.7, -283.3, -300.0 | -0.03, -0.11, +4.26, +6.82, +6.81, +6.79, +6.75, +6.73 |
| G2 | rt | -16.7, -91.7, -33.3, -33.3, -50.0, -58.3, -108.3, -125.0 | -0.02, -0.09, +4.13, +4.04, +4.03, +4.02, +3.99, +3.97 |
| G4 | det | -16.7, -316.7, -33.3, -125.0, -141.7, -150.0 | -0.03, -0.14, +4.21, +4.14, +4.13, +4.11 |
| G4 | rt | -16.7, -133.3, -33.3, -100.0, -116.7, -125.0 | -0.02, -0.13, +3.99, +3.89, +3.88, +3.87 |
| G3 | det | -16.7, -316.7, -33.3, -125.0, -141.7, -150.0, -33.3, -50.0, -58.3, -75.0 | -0.03, -0.14, +4.21, +4.14, +4.13, +4.11, +17.37, +17.34, +17.69, +17.67 |
| G3 | rt, 60 fps of pair 1 or 2 with 120 fps of pair 4 | -16.7, -133.3, -33.3, -100.0, -116.7, -125.0, -50.0, -66.7, -66.7, -83.3 | -0.02, -0.12, +4.03, +3.94, +3.93, +3.92, +3.60, +3.58, +3.57, +3.55 |
| G3 | rt, 60 fps of pair 5 with 120 fps of pair 4 | -16.7, -150.0, -33.3, -33.3, -50.0, -58.3, -50.0, -66.7, -66.7, -83.3 | -0.03, -0.15, +4.03, +3.95, +3.93, +3.92, +3.53, +3.51, +3.50, +3.48 |
| G1 | det, `none` | -16.7, -266.7 | -0.02, -0.10 |
| G4 | det, `none` | -16.7, -308.3, -2,533.3, -2,625.0, -2,641.7, -2,650.0 | -0.02, -0.14, +1.69, +1.65, +1.63, +1.62 |
| G1 | rt, `none` | -16.7, -116.7 | -0.01, -0.10 |
| G2 | rt, `none` | -16.7, -116.7, -25.0, -33.3, -50.0, -58.3, -133.3, -150.0 | -0.02, -0.10, +4.06, +3.91, +3.89, +3.89, +3.82, +3.81 |
| G4 | rt, `none` | -16.7, -166.7, -33.3, -91.7, -108.3, -116.7 | -0.04, -0.17, +3.98, +3.89, +3.88, +3.87 |

The deterministic rows equal those of the loaded runs to the 0.1 ms (G3's now has all ten changes, the reference's).
Against the per-change limits proposed below (item 2 of the proposal: 333.3 ms for the first aDemo, 166.7 ms for the
three after the intro movie, 316.7 ms for G2's opening, 91.7 ms for G3's aGameSys and aQuest, 66.7 ms for every other
change), every `azahar` row, deterministic and real time, is inside; so are the real-time `none` rows; the deterministic
G4 with `none` is not (2,533.3 to 2,650.0 ms).

DSP shift against the game clock offset at the window's start, in DSP frames: G1 (the boot window) +15 of +106 (rt), +13
of +98 (rt `none`), +57 of +125 and +57 of +124 (det, both latencies); G2 at frame 6000 +683 of +814 (84 %, rt), +638 of
+810 (79 %, rt `none`), +1262 of +1488 (85 %, det); G4 at frame 2000 +651 of +822 (79 %, rt), +633 of +821 (77 %, rt
`none`), no buffer in common in the det runs (+1006 and +1008 of clock offset); **G3 at frame 16000 (the hunt) +3765 of
+3767 (100 %, det)**, and in real-time pair 3, where both runs are still in the village there, +708 of +708 (100 %); the
other four real-time G3 pairs have no buffer in common or other buffer changes (one run in the hunt, the other not).

Hunt observables of G3 (the hunt = the scene aQuest; per second of game time, D70's first guesses as the limits;
`compare --position-until 15862` gives the walk before the first attack press, in the hunt too):

| Pair | scene changes over 66.7 ms | largest time difference | position, seconds over / of | largest distance (median) | walk to frame 15862, largest | quest counter, seconds over 1 s / of | largest quest difference (median) |
|---|---|---|---|---|---|---|---|
| det, seed 0 (and `--seed 1`, equal) | 5 of 10 | 316.7 ms | 59 / 63 | 3,627.2 units (949.4) | 840.0 units | 19 / 65 | 1.07 s (0.62 s) |
| rt, 60 fps of pair 1 with 120 fps of pair 4 | 5 of 10 | 133.3 ms | 55 / 64 | 2,277.7 units (958.8) | 958.8 units | 53 / 65 | 10.53 s (1.52 s) |
| rt, 60 fps of pair 2 with 120 fps of pair 4 | 5 of 10 | 133.3 ms | 55 / 64 | 1,660.7 units (66.4) | 76.6 units | 42 / 65 | 14.05 s (8.70 s) |
| rt, 60 fps of pair 5 with 120 fps of pair 4 | 2 of 10 | 150.0 ms | 55 / 64 | 2,615.7 units (862.2) | 862.2 units | 53 / 65 | 10.35 s (1.35 s) |
| reference, N3-21 (G3), for comparison | 5 of 10 | 316.7 ms | 59 / 63 | 3,627 units | | 19 / 65 | 1.07 s |

The deterministic pair is the reference's pair, value for value (N3-32 found the same). Pair 4's 120 fps run is the one
partly under load (*The PC's load*).

**The same-rate spread on the idle PC.** Two 60 fps runs of G3 that reach the hunt, compared as a pair (folders
`build/fps/same60-idle-rA-rB`: the second run's frame numbers doubled so that `compare` reads it as the 120 fps side;
values and clock untouched):

| 60 fps runs | scenes | largest time difference | position, seconds over / of | largest distance (median) | walk to frame 15862, largest | quest counter, seconds over 1 s / of | largest quest difference (median) |
|---|---|---|---|---|---|---|---|
| pair 1 with pair 2 | pass (10) | 0.0 ms | 53 / 64 | 2,603.2 units (893.0) | 893.0 units | 52 / 65 | 15.55 s (15.53 s) |
| pair 1 with pair 5 | pass (10) | 66.7 ms | 53 / 64 | 1,042.1 units (101.1) | 101.1 units | 0 / 65 | 0.18 s (0.17 s) |
| pair 2 with pair 5 | pass (10) | 66.7 ms | 53 / 64 | 2,598.3 units (796.4) | 796.4 units | 52 / 65 | 15.38 s (15.37 s) |

The 120 fps spread in the hunt cannot be measured: one 120 fps run reached it. Before the hunt the five 120 fps runs
have their six scene changes at the same frames (above). The quest counter of two 60 fps runs is either within 0.18 s or
15.4 to 15.6 s apart (pair 2's run against the other two): the talk that pauses it starts at another time, as on the
loaded PC.

**What changed from the loaded runs.**

1. The deterministic rows are unchanged to the 0.1 ms, the frame and the DSP frame (G1, G2, G4), and G3's is now the
   reference's: 10 scene changes at both rates, 9 of 9 checkpoints, the hunt reached, seeds 0 and 1 equal (N3-32).
2. `--deterministic --io-latency none` no longer hangs (N3-33): G1 and G4 finish in 2.5 + 4.6 s and 4.2 + 8.7 s. Its G4
   pair fails the times by 2.65 s, because the 60 fps run ends the intro movie where G3's and G4's `azahar` 60 fps runs
   do (frame 903; 902 in G3's pair 4; 1802 at 120 fps) and the 120 fps run where G4's real-time `none` run does (frame
   1502).
3. Real-time host times are steady (G3 303.1 to 303.4 + 306.5, against 303 to 408 + 307 to 455 loaded), and the
   real-time `azahar` scene times are a little closer: largest G1 91.7, G2 125.0, G4 133.3 ms against 91.7, 133.3,
   158.3 ms loaded (`none`: 116.7, 150.0, 166.7 against 100.0, 166.7, 166.7). Every `azahar` row, both modes, and
   every real-time `none` row is inside the per-change limits of the proposal.
4. G3 in real time is not more repeatable on the idle PC: 4 of 10 runs reach the hunt (3 of 5 at 60 fps, 1 of 5 at
   120 fps, that one partly under load), against 6 of 10 on the loaded PC (3 and 3, `azahar` and `none` pairs; `azahar`
   alone: 3 of 6, 2 of 3 at 60 fps and 1 of 3 at 120 fps), and no pair of five has both runs in the hunt. N1-26's 10 of
   10 at 60 fps on an idle PC was not reproduced at this base.
5. The same-rate spread is as large on the idle PC: two 60 fps runs up to 2,603 units apart in the hunt (2,460 loaded),
   and up to 15.55 s on the quest counter (0.20 s loaded, 15.8 s between two loaded 120 fps runs). The position and
   quest clock in real time depend on the run, not mainly on the host's extra load.
6. The DSP windows after the intro movie give the same 77 to 85 % of the clock offset in G2 and G4; G3's window at frame
   16000, measurable now in the deterministic mode, gives 100 % (+3765 of +3767), as does real-time pair 3's (both runs
   in the village there).
7. The writes are unchanged: with the proposed G3 and G4 masks every pair passes; G4 with `none` is equal without them.

## The spread D70's numbers come from, and the proposal

D70 (Open) says: same scene sequence; each change within 4 frames at 60 fps; equal `eq` values at the checkpoints; the
position within 2 % of the last second's travel plus 0.05 units; the quest clock within 1 s; the DSP stream by D51's rule;
`fs:` Writes by D52's rule. What the table above measured, item by item, with the proposal for each (the numbers are the
orchestrator's to settle; none of the tool's defaults was changed):

1. **Scene sequence.** Equal in G1, G2 and G4 in all runs, real time and deterministic, `azahar` and `none`. Different in G3
   at 120 fps in the deterministic mode, reproducibly; in real time G3 failed in four of five pairs on the loaded PC.
   Proposal: keep "the same sequence, never tolerated", and **gate it in both modes for G1, G2 and G4** (it passed in
   every run of the three), and in the deterministic mode for G3; G3's real-time sequence is reported, not gated, until it
   is measured on an idle PC. G3 at 120 fps stays `needs-decision` until finding 2's cause is found. *(2026-10-31, N3-32:
   found and fixed, see finding 2; the deterministic G3 pair has the same 10 real scene changes at both rates.)*
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
   | G3's aGameSys and aQuest of the quest load | 75.0 ms (4.5 frames, the reference only: the native det run does not reach them) | 91.7 ms (5.5 frames), to be confirmed from the first native deterministic pair that reaches them *(2026-10-31, N3-32: that pair, `build/fps/G3-det-n332`, measures the reference's values, 75.0 ms the largest)* |
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
   clock part stays Open with that trigger. *(2026-10-31, N3-32: the trigger is met. The deterministic pair
   `build/fps/G3-det-n332` reaches the hunt at both rates: position 59 of 63 seconds over, largest 3,627.2 units; quest
   counter 19 of 65 seconds over 1 s, largest 1.067 s, the numbers of the reference's own pair. Settling them is D70's,
   which stays Open until the audit is run again on an idle PC.)*
5. **DSP stream.** The window of G1 (before the clock offset arises) gives shifts of +9 and +17 DSP frames in real time
   (loaded PC) and +57 in the deterministic mode; every window after the intro movie gives shifts of 640 to 4,552 DSP frames
   or no buffer in common, with the clock offset at the window 4 to 22 s (finding 1). The shift is measured on the DSP
   frame counters, so it does not depend on where the window starts: a window that starts where the 120 fps run's clock
   equals the 60 fps window's (or a wider window) gives overlap where there is none now (G3 and G4 det, "no buffer in
   common"), but not a smaller shift, which after the intro movie is 77 to 85 % of the clock offset (finding 1). Proposal:
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
or `6000:300` (the village), G4 `--dsp 2000:300`. For G3 the runs of 2026-10-31 gave `compare --require
position,quest_time,dsp --position-until 15862` (15862 is N3-21's last frame before the first attack press moves the
hunter); since N3-34 those are G3's defaults, and `compare DIR` with no option gives D70's verdict for every session
(*Using it*). The timed G1 det-none pair: `run G1 ... --io-latency none --deterministic --timeout 300 --out
build/fps/G1-det-none-t300`.

**A real-time pair counts only on an idle PC (D70):** the PC's total CPU load before the pair at most **25 %**, measured
as *Runs on an idle PC* did (the total over the 20 s before the pair), with no build, test or other run of the port and
no other heavy program (an installer, a game, an antivirus scan) during the pair; the guest clock follows the host's. A
pair over 25 % before it, or interrupted during it, does not count: wait for the load to drop and run it again (as pair 5
of *Runs on an idle PC* was held), and say in the notes which pair was affected (as pair 4's 120 fps run is marked). The
real-time rows of *Tables* did not meet this rule. The deterministic mode does not depend on the host.

`compare` writes `compare.txt` and `compare.json` into the folder it reads. To judge recorded runs again without
touching their record, copy each run folder first (`robocopy build\fps\<tag> build\<wp>\<tag> /E`) and run `compare` on
the copy, as N3-34 did with N3-22's idle folders in `build/n3-34/`.

The record-by-record numbers of finding 2 (records equal, first difference per observable, tick offsets) are not an item
of the tool: they compare a native state trace with a reference one at the same frame, every value and the `t=` ticks
(the native det traces `build/fps/G3-det/r60/state.txt` and `r120/state.txt` against N3-21's `build/n3-21/r2` and `r3`,
and r3 against the new reference run `build/fps/G3-ref120-r4`). The helper scripts that did it print frames and counts
only and stay outside the repository (`build/fps/an_*.py` and the session's scratch folder). The second reference run is
N3-21's `build/n3-21/run_ref.ps1` with `-Session build/n3-21/sessions/G3-x2.txt -Frames 36000 -Observable
build/n3-21/obs_n321.txt -Fps 120 -Extra --shots,<out>/shots,--shot-every,100000000,--draw-window,100`, which creates and
removes the emulator lock itself.
