# Observables for the 60/120 fps audit: the hunter's position and the quest's clock

N3-21 (docs/native/wp/N3.md; ROADMAP section 6's high-frame-rate rule: "positions and times within the tolerance
the audit sets"). N3-22 compares G3 at 60 fps with the scaled G3 at 120 fps; besides scenes it needs two values:
where the hunter is and how much of the quest's time has gone. This note records how they were found on the reference
runner, the lines, and what the validation rules of the spec say about them.

**Outcome: done, with two rules decided by the orchestrator.** Both values were found and behave as what they are.
Taken literally, the rules of the spec did not fit the game (a countdown that pauses; a hunter that swings when
attacking), and the first round of this package ended `needs-decision` for that reason. The two rules below were
decided by the orchestrator under the maintainer's delegation, 2026-10-31; the tool implements them and, on the
saved traces of the four runs, prints `valid` for both values at 60 and at 120 fps (*Validation*). The three numbers of
the spec (60 frames, magnitudes under 100000, 1 %) were not touched, and the audit's tolerances stay D70's, still Open
(N3 rule 7). Only addresses, offsets, counts, frames and rates are written here: no memory contents, no game text,
nothing of the save.

The rules, as the package applies them:

- **Position.** The stillness-and-walk rule of the spec, unchanged in its numbers, applies to the part of the hunt
  **before the first attack press**. An attack is not circle-pad input and moves the hunter by itself, so the
  stretch after it has no stillness to check. The frame that ends the part is given on the command line (`--to`) and
  the tool prints the restriction it used in its verdict line (`valid (hunt restricted by --to 15862)`). Without
  `--to` the rule is applied to the whole hunt, and the position, which swings after the first attack, is `invalid`.
  Neither `G3.txt` nor any golden file is edited, and the session notes (`# hit:`) are not used.
- **Quest time.** The counter is cut into **running stretches** (it moves in every second of game time) and
  **pauses** (it does not move at all). A running stretch shorter than **5 s** of game time is a blip and is ignored (a
  pause that only a blip separated from another is one pause). The rule passes when there is at least one running
  stretch and every one stays within **1 %** of its own straight line (of its own change). The pauses are reported
  (their number and frames), never an error. The rate per second of game time of every running stretch is printed and
  **not asserted**: that it agrees between 60 and 120 fps is N3-22's comparison.

All runs: Old 3DS (`--new-3ds off`), `--cpu recomp`, the library of the golden recordings
(`build/golden-lib/0004000000126300.dll`, sha e95eae1f), the reference runner of `build/runner-win-n035`, G3's
`# init-time`, a fresh copy of G3's `user-before` (the maintainer's save, Circle Pad Pro off) for every run. Frames
are game frames (the `u32` at `0x010571d4`).

## The lines

Written as proposed `config/native/observable.txt` lines, `ignore` (recorded, never compared by `state_diff.py`;
N3-22 decides what it compares). `[[[A]+o1]+o2]` reads the 4 bytes at `[[A]+o1]+o2`, so the chain below is: the
`sPlayer` instance, its player unit, a field of the unit.

```
[[[0x00fb83e8]+0xa98]+0xe48]  4 hunter_x    ignore
[[[0x00fb83e8]+0xa98]+0xe4c]  4 hunter_y    ignore
[[[0x00fb83e8]+0xa98]+0xe50]  4 hunter_z    ignore
[[0x00fbeffc]+0x803c]         4 quest_time  ignore
```

| Line | What it is |
|---|---|
| `0x00fb83e8` | `g_sPlayerInstance` (`config/symbols.txt`): the `sPlayer` instance (size 0xad0, `mt_classes.tsv`) |
| `sPlayer+0xa98` | the only heap pointer in the `sPlayer` instance: the player unit, a `uPlayer`-sized object (0xace0 bytes; its vtable is a code address) |
| `unit+0xe48 .. +0xe50` | three IEEE floats, the hunter's position. A second copy of the same three values is at `unit+0xd00 .. +0xd08`; the other candidate triples in the unit (26 in all) are overlapping windows over these words and values derived from them |
| `0x00fbeffc` | a global that holds the pointer to a quest object (vtable `0x00e659bc`; instance in the heap) |
| `quest+0x803c` | a `u32` countdown of the quest's time, **60 units per second of game time** (measured below), at 60 and at 120 fps |

Readable from frame 14243 (position, 60 fps; 28406 at 120) and from before the quest (the countdown, from 14100) to the
end of the session. As N3's trap says, the chain is validated in the hunt only: it names whatever sits there in other areas.

Rejected as `quest_time`: the game's millisecond clock (`0x010e66c0`, and the copies of it in the heap) is linear within
the rule's 1 % at 60 fps (0.44 %) but not at 120 fps (1.23 %), and it is the emulated clock, not the quest's: it would
agree between the two rates whatever the game did with its own time. The frame counters (1 per frame, and the 30 Hz
ones) run at twice the amount per second at 120 fps by construction.

## Method

1. **Static starting points.** `g_sPlayerInstance` and `sPlayer` from `config/symbols.txt` and `mt_classes.tsv` (sizes
   of `sPlayer` and `uPlayer`); `docs/research/camera.md` for what the player unit is (its input object at `+0xe30`, the
   camera rig that follows it). The Ghidra exports (`build/export/*.tsv`) hold function names, callers and strings and
   no code, so no field of the unit could be read from them; the fields were found in memory.
2. **Dumps.** One G3 run at 60 fps with `--dump f<N>:08000000:0x4000000` (the 64 MiB application heap) and
   `--dump f<N>:00100000:0x1000000` (the 16 MiB of globals) at 17 frames of the hunt: 14130, 14250, 14340 (no pad input),
   14420, 14500, 14600, 14700, 14800, 14900 (the circle pad held, G3's walk from f14350 for 600 frames), 14980, 15040
   (no input), 15500, 16000, 16500, 17000, 17500, 17900. 34 files, 1.3 GiB, in `build/n3-21/r1/shots`.
3. **Position:** `observable_probe.py find-position` with `--still 14250,14340;14980,15040` and `--moving
   14420,14500,14600,14700,14800,14900`: the triples of consecutive floats (magnitude under 100000 in every dump) that are
   equal in each still group and change in each step of the walk. 295 triples in the heap, 26 of them inside the player
   unit (found from `sPlayer`: its pointer to the unit). The triple at `unit+0xe48` is the one with an `x` and `z` that
   move steadily and a `y` that stays on flat ground for most of the walk; the others in the unit are copies and
   derived positions (the same values, or offsets from them).
4. **Quest time:** `find-counter` over the same dumps (1 % straight line): 20 words in the heap and the globals (17
   and 3), all of them frame counters or clocks of the emulated time, and none that counts down. Because a quest's countdown stops during
   cutscenes and talks, `find-monotone --down --min-moving 6` (never turns back, moves in at least 6 of the 16 gaps)
   gives 8 words in the heap, and one of them, `quest+0x803c`, moves in 13 gaps by about one unit per frame and is
   flat in the others. The pointer to its object came from the words of the globals that point at an object that
   contains the address.
5. **State traces.** Both candidates were then recorded at every frame (`--state-trace` with the lines above added to
   `config/native/observable.txt`'s): G3 at `--set 0:fps=60` with the session, and at `--set 0:fps=120` with the
   scaled one (`observable_probe.py scale G3.txt --factor 2 -o G3-x2.txt`, which `golden.py`'s parser accepts). To
   run in 2 and 4 minutes of host time instead of one hour, the runs use `--shots DIR --shot-every 100000000
   --draw-window 100`: the software renderer draws nothing and nothing about the game changes. At 60 fps the whole state
   trace (18,001 records) equals the one recorded in `build/golden/G3` in every tick and every value they share.

Everything is repeatable with the commands at the end.

## The reference at 120 fps follows the scaled session

The reference runner plays the scaled session. Real scene changes of the two runs (10 each, the same order), in game
time (frame over fps):

| Scene change (the class the new scene is) | 60 fps: frame, s | 120 fps: frame, s | Difference |
|---|---|---|---|
| `aGameSys` | 2, 0.03 | 2, 0.02 | -0.02 s |
| `aDemo` | 42, 0.70 | 46, 0.38 | -0.32 s |
| `aContinue` | 903, 15.05 | 1802, 15.02 | -0.03 s |
| `aDemo` | 1576, 26.27 | 3137, 26.14 | -0.12 s |
| `aGameSys` | 1578, 26.30 | 3139, 26.16 | -0.14 s |
| `aVillage` | 1579, 26.32 | 3140, 26.17 | -0.15 s |
| `aGameSys` | 13462, 224.37 | 26920, 224.33 | -0.03 s |
| `aArena` | 13464, 224.40 | 26922, 224.35 | -0.05 s |
| `aGameSys` | 14107, 235.12 | 28207, 235.06 | -0.06 s |
| `aQuest` | 14109, 235.15 | 28209, 235.07 | -0.08 s |

The hunt is reached at 120 fps, so both candidates could be recorded there (the quest-loading race of the native
runtime did not show on the reference in this one run). Host time: 128 s at 60 fps and 235 s at 120 fps.

## Validation

`observable_probe.py validate --trace STATE --session S --rule position|quest_time` with the two rules above. The
inputs are the saved state traces of the earlier runs (no new run was made for this round): G3 at 60 fps with `G3.txt`,
and G3 at 120 fps with `G3-x2.txt` (what `scale` writes). `--settle` is 0 in every run (the margin is not used).

| Run | Rule | Command line (after the trace and session) | Output line |
|---|---|---|---|
| 60 fps, G3 | `position` | `--rule position --to 15862` | `valid (hunt restricted by --to 15862)`: hunt frames 14243 to 15862 (1620 records); 2 stretches without movement input constant, 1 with the circle pad held changes |
| 120 fps, G3-x2 | `position` | `--rule position --to 31542` | `valid (hunt restricted by --to 31542)`: hunt frames 28406 to 31542 (3137 records); 2 stretches constant, 1 changes |
| 60 fps | `quest_time` | `--rule quest_time` | `valid (running stretches 2, pauses 2)`: hunt frames 14109 to 18000 (64.85 s of game time) |
| 120 fps | `quest_time` | `--rule quest_time` | `valid (running stretches 2, pauses 2)`: hunt frames 28209 to 36000 (64.93 s) |

At 60 fps the same `position` run on the copy of the triple at `unit+0xd00` (`--names hunter_x_b,hunter_y_b,hunter_z_b`)
is `valid` too, with the same counts, and so is the one at 120 fps. Without `--to` both `position` runs are `invalid`,
as the rule says they must be: at 60 fps, "the position changes inside the stretch 14950..18000 without movement input
(first at frame 15863)"; at 120 fps, "inside 29900..36000 (first at frame 31543)".

What the numbers say, for both rates:

- **Position, the part before the first attack press.** From the hunt's first readable frame the position is constant
  until the walk (f14350 on, 600 frames; f28700 on, 1200 frames at 120), changes during it, and is constant again
  after the hunter stops (f14933, 17 frames before the hold ends; 929 frames, 15.5 s of game time, of talk follow).
  The session then presses `A` every 90 frames (180 at 120 fps) from f15050, first to page through a caravan leader's
  explanation (no hunter motion) and then as swings: 3 frames after the first press that is an attack, `A` at f15860,
  the position moves by itself for about 40 frames, and it does so again after later presses. That press is found from
  the trace, not from the session text (the presses are all `A`): at 120 fps it is `A` at f31540, again 3 frames before
  the first move (f31543). `--to` is the press frame plus 2 (15862, 31542). The attack press falls 90 frames of the
  60 fps clock (1.5 s) earlier at 120 fps, which is the same shift as the end of the long pause of the countdown
  (below): the talk ends 1.5 s of game time sooner at 120 fps.
- **Quest time.** It pauses for the first 2 s of the hunt, runs, pauses for 15 s (the explanation of the target camera)
  and runs to the end. The tool prints, per running stretch, the frames, length, rate and largest distance from its
  straight line:

  | 60 fps | frames | seconds | rate per second | distance from the line |
  |---|---|---|---|---|
  | pause | 14109..14229 | 2.0 | | |
  | running | 14251..14933 | 11.4 | -60.44 | 0.508 % |
  | pause | 14949..15849 | 15.0 | | |
  | running | 15862..18000 | 35.6 | -60.17 | 0.051 % |

  | 120 fps | frames | seconds | rate per second | distance from the line |
  |---|---|---|---|---|
  | pause | 28209..28329 | 1.0 | | |
  | running | 28415..29609 | 9.9 | -62.61 | 0.779 % |
  | pause | 29649..31449 | 15.0 | | |
  | running | 31542..36000 | 37.1 | -60.46 | 0.113 % |

  Both running stretches of each run are well over the 5 s minimum and within 1 % of their own line, so both rates pass;
  the margin is 0.779 % at worst (the first stretch at 120 fps). No `--settle` was needed. The rates are printed and
  not judged: the long stretches are -60.17 and -60.46 per second, 0.5 % apart, and the short ones -60.44 and -62.61
  (the first running stretch at 120 fps starts later and is shorter).

Observations for N3-22 (not results of this package): after the first attack press the hunter's position at matched
times differs between the two runs by hundreds of units, well outside a tolerance of 2 % of the distance travelled in
the last second plus 0.05 units (the 120 fps hunter stops earlier and elsewhere), and the quest's pauses fall at
different game times: the countdown first moves 142 frames (2.4 s) after the quest loads at 60 fps and 206 frames
(1.7 s) at 120 fps, and the long pause ends at game time 264.4 s at 60 fps and 262.9 s at 120. The countdown's values
at matched game times (every 3 s from the start of the hunt, 22 samples) differ by 64 units at most (1.07 s of game
time), and by 28 to 38 units (0.47 to 0.63 s) from 265 s on. Both are game behaviour at 120 fps, to be tabulated by the
audit.

## What N3-22 decides

This package validates the two observables; it does not compare the two rates, which is the audit's. It leaves N3-22:

1. **The position after the first attack press.** The comparison at matched times after it is N3-22's, which tabulates
   it: the stillness rule stops at the first attack, and the observation above (hundreds of units apart at 120 fps) is
   input to that table, not a verdict.
2. **The rate agreement of the quest time.** Whether the rate per second of game time of the running stretches agrees
   between 60 and 120 fps, and how the pauses compare (matched times within 1 s of game time, as N3-22's item 4 says).
3. **The lines.** The four lines above are proposals as `ignore` for N3-22 to add to `observable.txt` at run time
   (`config/**` is the orchestrator's; this package did not edit it, nor `G3.txt`, nor any golden file).

The tool keeps `validate --settle N` (frames left out at the start of each stretch of the position rule; not used) and
`segments` (the cut into running stretches and pauses, of every length).

## Repeating it

```
python tools/native/observable_probe.py scale tools/native/golden/G3.txt --factor 2 -o build/n3-21/sessions/G3-x2.txt
# a copy of G3's user-before per run; build/n3-21/obs_n321.txt is config/native/observable.txt plus the four lines
mh4u_runner juego.cxi --new-3ds off --cpu recomp --code <library> --install update_v130.cia --user <copy> \
    --frames 18000 --input-file G3.txt --init-time 1767268800.5 --set 0:fps=60 --state-trace s60.txt \
    --observable obs_n321.txt --shots <dir> --shot-every 100000000 --draw-window 100          # 36000, G3-x2.txt, fps=120
# the validation: --to is the frame before the first attack press moves the hunter (15862 at 60 fps, 31542 at 120)
python tools/native/observable_probe.py validate --trace s60.txt --session G3.txt --rule position --to 15862
python tools/native/observable_probe.py validate --trace s120.txt --session G3-x2.txt --rule position --to 31542
python tools/native/observable_probe.py validate --trace s60.txt --session G3.txt --rule quest_time
python tools/native/observable_probe.py validate --trace s120.txt --session G3-x2.txt --rule quest_time
python tools/native/observable_probe.py segments --trace s60.txt --session G3.txt   # every stretch, whatever its length
# the hunt for the lines (dumps from a run with --dump f<N>:08000000:0x4000000 and f<N>:00100000:0x1000000)
python tools/native/observable_probe.py find-position DUMPS --base 0x08000000 --still 14250,14340;14980,15040 --moving 14420,14500,14600,14700,14800,14900
python tools/native/observable_probe.py find-counter DUMPS --base 0x08000000 --frames 14130,14250,...
python tools/native/observable_probe.py find-monotone DUMPS --base 0x08000000 --frames 14130,14250,... --down
```

(With the maintainer's default flags the same runs take about an hour at 60 fps: the `--draw-window` form skips the
software renderer and keeps the trace identical.) The emulator is exclusive: the lock folder
`build/locks/emulator` was created before each run and removed after.
