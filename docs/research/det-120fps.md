# G3 at 120 fps in the deterministic mode (N3-32)

N3-22's audit found that the native runtime in the deterministic mode does not take G3 out of the village at 120 fps,
although the reference does with the same scaled session (`fps-audit.md`, *What the runs found*, finding 2). This note
is how the cause was found, what it was, the fix, and the numbers of the runs that show it. Only frames, ticks, counts,
times, distances and shifts are written here: nothing of the traces' values, the game or a save.

**Outcome.** The cause is in the native runtime, not in the game: the deterministic mode's slice was 1,145,774 ticks,
and Azahar's `MAX_SLICE_LENGTH` is `BASE_CLOCK_RATE_ARM11 / 234` in integer arithmetic, 268,111,856 / 234 =
**1,145,777**. A thread-safe event (Azahar's `ScheduleEvent(..., thread_safe_mode)`) is due `MAX_SLICE_LENGTH * 2` after
its core's clock, so every `nwm::UDS` asynchronous signal came 6 ticks early on core 1. That is the residual N1-26 and
N0-39 recorded and left unexplained (`golden.md`; `config/native/timeline_allow.txt`). At 60 fps the timeline closed up
again after each such event; at 120 fps the fourth one, in frame 6,591, cut a stretch of core 0 6 ticks short and
moved core 0's timeline for good, and from there the game's own frame timing (`smain_delta`, from frame 6,592) drifted
until the village-to-Arena change at frame 26,920 did not happen. With the slice fixed, the native deterministic G3 at
120 fps equals the reference record for record over the whole session and reaches the hunt, and the scheduler timelines
of G1, G3 and G4 at 60 fps and of G3 at 120 fps equal the reference's event for event, with no allowed difference left.

## Method and commands

All runs on the maintainer's PC, one emulator at a time (the lock folder `build/locks/emulator`), the llvm-mingw `bin`
folder on `PATH`. Paths under `build/` are the PC clone's (`C:\NoJales\mh4u-native\build\`).

- **Step 0, the reference repeats at 120 fps.** N3-22 had already repeated it exactly as the spec asks (N3-21's
  `run_ref.ps1` command line, `build/fps/G3-ref120-r4`). Checked again here:
  `python tools/native/state_diff.py --order-only --min-changes 10 build/n3-21/r3/state.txt build/fps/G3-ref120-r4/state.txt`
  and the same with `--tolerance 0`. Step 1's reference run (the same command line plus `--trace-timing`) is a third
  repeat and was checked the same way.
- **Step 1 (a), the reference's timeline:** `build/n3-21/run_ref.ps1 -Out build/n3-32/ref120 -Session
  build/n3-21/sessions/G3-x2.txt -Frames 36000 -Observable build/n3-21/obs_n321.txt -Fps 120 -Extra
  --shots,<out>/shots,--shot-every,100000000,--draw-window,100,--trace-timing,<out>/timing.txt` (151 s of host time).
- **Step 1 (b), the native timeline:** the native runner of the batch's base and the `_native_det` library with the
  arguments `fps_audit.py` gives its 120 fps run (`runner_args` and `child_env` of the merged N3-22 tool: the scaled
  session, a fresh copy of G3's `user-before`, `--set 0:fps=120`, `--io-latency azahar`, `--deterministic`, its
  `--state-trace`, `--trace-ipc`), `--frames 27000`, and `MH4N_TRACE_EVENTS=build/n3-32/nat120/events.txt` (39 s).
- **Step 1 (c), the 60 fps baseline:** the same native run at 60 fps with the unscaled session
  (`build/n3-32/nat60-base`) against the recording's `build/golden/G3/timing.txt`.
- **Step 2:** `python tools/native/timeline_check.py --reference <timing.txt> --candidate <events.txt> --show N` for both
  pairs, then the state traces' `t=` frame by frame around the first difference that the 60 fps pair does not have.
  `--trace-svc` was not needed: the timelines named the event.
- **After the fix:** the same native runs with the fixed runner (`build/n3-32/nat120-fix`, all 36,000 frames, and
  `build/n3-32/nat60-fix`), then the package's *Done when* runs (below).

## Step 0: the reference at 120 fps repeats

| Run | real scene changes | frames of the changes | against N3-21's `r3`, `--order-only --min-changes 10` | `--tolerance 0` |
|---|---|---|---|---|
| N3-21's `r3` | 10 | 2, 46, 1802, 3137, 3139, 3140, 26920, 26922, 28207, 28209 | | |
| N3-22's `G3-ref120-r4` | 10 | the same | exit 0 (12 scene changes, 10 real) | equal: 12 scene changes within 0 frames |
| this package's `ref120` (with `--trace-timing`) | 10 | the same | exit 0 | equal: within 0 frames; all 36,001 records equal in every value and every tick |

So the reference is exact at 120 fps: three runs, the same 36,001 records, tick for tick. The game reaching the Arena
at 120 fps is the game's behaviour on the reference's clock, and the native run that does not reach it has another clock.

## The first divergence

The baseline at 60 fps (G3, the native runner of the base against the recording): 841,314 of 841,395 events equal,
81 differences, every one at one of the 31 `UDS::handle_async_event_signals_event` events of core 1 (6 ticks early) or
the few events of core 0 right after it; the timeline is equal again after each.

At 120 fps (the base against `build/n3-32/ref120`): 232,258 of 1,096,491 events equal, then nothing. The first three
differences are again the `nwm` events alone (event 116,339 at tick 9,895,324,426, frame 3,492; events 178,518 and
178,519 at tick 14,226,171,087, frame 5,410), each 6 ticks early, the timeline equal after them. The fourth is where they
part for good:

| | reference | native (base) |
|---|---|---|
| event 210,797: `UDS::handle_async_event_signals_event`, core 1 | tick 17,078,952,141, late 0 | tick 17,078,952,135, late 0 |
| event 210,798: `ThreadWakeupCallback_0`, core 0 | tick 17,078,958,014, late 4,833 | tick 17,078,958,008, late 4,827 |
| every later event of core 0 | | 6 ticks early, then further off |

Ticks are counted from the clock's start, as in `timing.txt`. The event falls in frame 6,591 (the state traces' `t=`);
the next frame, 6,592, is the first whose `smain_delta` differs, which is where N3-22 saw the drift start. The event
was asked for from a thread of core 0 (a `nwm::UDS` request) at tick 17,076,660,587 of core 1's clock in the native run
(the `stretch` line of the events file, frame 6,589), so it was due at that tick plus 2,291,548; the reference's
is due 2,291,554 after the same moment, 6 ticks later.

## The cause

`ScheduleThreadSafe` (`native/src/kernel/deterministic.cpp`) puts the event at `CoreTicks(core) + 2 *
kDeterministicSliceTicks`, as Azahar's `Timing::ScheduleEvent` does with `thread_safe_mode` (`cycles_into_future =
max(MAX_SLICE_LENGTH * 2, cycles_into_future)`, on the core's `GetTicks()`). `kDeterministicSliceTicks` was 1,145,774;
Azahar's `core_timing.h` at the pinned commit has `static constexpr int MAX_SLICE_LENGTH = BASE_CLOCK_RATE_ARM11 / 234`,
which is 1,145,777. Twice the 3 ticks is the 6 of every `nwm` event of G1, G3 and G4.

Why it shows at 120 fps only: the event is the next event of core 1, so it limits the stretch of core 0 that runs over
it (RunLoop's `max_slice` is the shortest time to an event of either core). In frame 6,591 the next event of core 0, a
thread's wake-up, is due at the same tick in both runs (tick minus lateness: 17,078,953,181), but it ran 6 ticks earlier
with 6 ticks less lateness: core 0's clock was 6 ticks behind the reference's from that stretch on, and everything core
0 scheduled after it was counted from that clock (the next wake-ups are 6 ticks early with equal lateness). At 60 fps,
and at the first three events at 120 fps, the timelines met again after the event; why the fourth kept the 6 ticks
(what core 0 was doing in that stretch) was not looked into further, because the cause of the 6 ticks was found. The
game's frame timing is computed from `svcGetSystemTick`, so a few ticks became the drift N3-22 measured (milliseconds by
75 s, seconds by 220 s) and then another frame at the village's exit.

The candidates of the spec, checked against this:

1. **The refresh rate.** The native and the reference set the same VBlank period (2,234,265 ticks, the truncated
   268,111,856 / 120), and the VBlank events of the two timelines are equal from the first one to event 210,796; ruled out.
2. **The frame limiter's waits** (the sleeps and the spin on the system tick). They are where the 6 ticks grew, not
   where they came from: every wake-up of core 0 is equal up to event 210,797.
3. **The timer events.** This is the cause: a kernel timer event (the thread-safe `nwm` signal) on core 1, 6 ticks early
   because the slice it is counted in was 3 ticks short.
4. **The file replies' delays** (`--io-latency none` at 120 fps): not run; the timelines named the event before it was
   needed, and with the fix the run with `azahar` equals the reference throughout.
5. **The quest-loading race** (`QuestFlow::startQuest`'s hold): ruled out for this divergence. The runs part at frame
   6,591, 170 s of game time before the quest loads, and with the fix the native run loads the quest at the reference's
   frames (28,207 and 28,209) with the hook as it is.

## The fix

`native/src/kernel/deterministic.h`: `kDeterministicSliceTicks = kTicksPerSecond / 234` (1,145,777, with a
`static_assert`), the same integer division as Azahar's. It is the cap of each stretch of RunLoop, the length a core with
no event is given, and the delay of the thread-safe events, so all three are now Azahar's. The default of `--slice-us`
(4274) still means exactly this value; 4274 us is 133 ticks more. No change to the code library: the `_native_det`
library charges cycles only and does not hold the slice, so it is not rebuilt.

Tests (`native/tests/determinism_test.cpp`): the slice is written out as 1,145,777 (`kAzaharSlice`, from Azahar's header,
not from the code under test) in every case that used 1,145,774, and a new case, *a thread-safe event asked for from
core 0 is due 2,291,554 ticks after core 1's clock*, reproduces the mechanism. Against the base the file fails 10 of its
45 cases (the new one, the thread-safe event's exact tick, the order of thread-safe events, the stretch lengths, the
first slice of the periodic-events case, the seeded stretch's longest); with the fix all pass.
`native/tests/nwm_test.cpp` (not this package's) checks the old tick of the connection status event and fails with the
fix until its literal is changed (proposed in the package's report).

## The runs after the fix

Timelines (`timeline_check.py`, no allow file):

| Pair | base | fixed |
|---|---|---|
| G3 at 120 fps, native against `build/n3-32/ref120` | 232,258 of 1,096,491 equal (frame 27,000 of the native run) | 1,096,491 of 1,096,491 equal (all 36,000 frames) |
| G3 at 60 fps, native against the recording | 841,314 of 841,395 equal, 81 differences | 841,395 of 841,395 equal |
| G4, `golden.py check --timeline` | 143,290 of 143,294 (4 allowed) | 143,294 of 143,294 equal (the four allow lines match nothing) |
| G1, `golden.py check --timeline` | | 78,578 of 78,578 equal |

State traces of the native deterministic runs against the reference's (every record, every compared value; the tick
offset from frame 1 on, besides the constant 486,920 ticks at frame 0):

| Pair | records equal | largest tick offset from frame 1 |
|---|---|---|
| 120 fps: `build/fps/G3-det-n332/r120` against `build/n3-32/ref120` | 36,001 of 36,001 | 5,825 |
| 120 fps, base: `build/fps/G3-det/r120` against N3-21's `r3` | 6,630 of 36,001 (`smain_delta` from frame 6,592; the scene, quest, loader and quest-counter lines from 26,920; the position lines from 28,406) | 1,392,414,076 |
| 60 fps: `build/fps/G3-det-n332/r60` against N3-21's `r2` | 18,001 of 18,001 | 5,893 (as at the base) |
| `--seed 1`, `2`, `3` against the unseeded run, at 60 and at 120 fps | all, every tick | 0 |

The offset of 5,825 ticks is the one SendSyncRequest by which the two runners' frame records are taken apart; it is
there from frame 1 at both rates.

Scene changes at 120 fps (the frames are the same in the reference, the fixed native run and the three seeds):
2, 46, 1802, 3137, 3139, 3140, 26920, 26922, 28207, 28209 (10 real changes; the base native run had the first six only).

Tick offset, reference minus native, at the frames of N3-22's times (game time = frame / 120) and at each scene change:

| Frame | game time | base (`G3-det/r120` against `r3`), ticks | fixed (`G3-det-n332/r120` against `ref120`), ticks |
|---|---|---|---|
| 6,592 | 54.9 s | 5,831 | 5,825 |
| 6,660 | 55.5 s | 13,135 | 5,825 |
| 9,000 | 75.0 s | -497,908 | 5,825 |
| 18,360 | 153.0 s | 4,305,838 | 5,825 |
| 26,400 | 220.0 s | -663,991,133 | 5,825 |
| 36,000 | 300.0 s | -1,390,355,249 | 5,825 |
| 2, 46, 1802, 3137, 3139, 3140 | | 5,825 each | 5,825 each |
| 26,920, 26,922 | 224.3 s | -780,395,320, -782,066,662 (no change in the base run) | 5,825 each |
| 28,207, 28,209 | 235.1 s | -827,991,429, -828,569,360 (no change in the base run) | 5,825 each |

(A single record's tick moves with the point at which the frame watcher sees the counter change, so the base column is
noisy frame to frame; its trend is N3-22's.)

## The deterministic pair at 60 and 120 fps (for D70)

`python tools/native/fps_audit.py run G3 ... --io-latency azahar --out build/fps/G3-det-n332 --deterministic` (the merged
N3-22 tool of the batch, which has `--deterministic` and `--seed` as options), then `compare`. The six items and `runs`:

| Item | result | numbers |
|---|---|---|
| runs | pass | 60 fps: exit 0, 24.0 s, 18,001 records; 120 fps: exit 0, 44.8 s, 36,001 records |
| scenes | pass | 10 real scene changes in each run, the same order; frames at 60 fps 2, 42, 903, 1576, 1578, 1579, 13462, 13464, 14107, 14109 |
| times | fail (not gated) | 5 of 10 over 66.7 ms; differences in ms, in order: 16.7, 316.7, 33.3, 125.0, 141.7, 150.0, 33.3, 50.0, 58.3, 75.0; game clock offsets in s: -0.025, -0.144, +4.206, +4.139, +4.126, +4.113, +17.365, +17.344, +17.689, +17.672 |
| checkpoints | pass | 9 of 9 equal |
| observables | fail (not gated) | position: 63 seconds compared (game time 238 to 300 s), 59 over, largest distance 3,627.2 units (57 over with 4 frames of slack), first over at 240 s; up to frame 15,862 (`--position-until 15862`): 27 seconds, 23 over, largest 840.0 units. Quest counter: 65 seconds compared (236 to 300 s), 19 over 1 s, largest 1.067 s at 246 s |
| dsp | n/a | no DSP capture in the session |
| writes | fail without masks, pass with N3-22's four proposed lines | 2 Writes on 1 thread at each rate; Write #0 (81,408 bytes) differs without masks; with `--fs-write-mask build/fps/fs_write_masks_n322.txt` equal in order and in full, 1 masked |

The same three gated items pass for `--seed 1`, `2` and `3` (`build/fps/G3-det-n332-s1` to `-s3`), with the same numbers
in every item: the seeded runs are the unseeded one, record for record, at both rates.

These are the numbers N3-22 asked for to settle D70's position and quest clock: a deterministic pair that reaches the
hunt at both rates, free of host noise. They equal the reference's own pair (N3-21: scene changes over 66.7 ms 5 of 10,
largest 316.7 ms; position 59 of 63 seconds over, 3,627 units; quest counter 19 of 65 over, 1.07 s), because each native
run is now the reference's run of its rate.

## What is not covered

- `--io-latency none` at 120 fps in the deterministic mode was not run (candidate 4 was not needed).
- The real-time runs at 120 fps are untouched: the slice and the thread-safe delay are the deterministic mode's only
  (`LockstepOf`); the real-time mode delivers the `nwm` signal at once and uses `--slice-us` (1000 us).
- Finding 1 of N3-22 (the game's clock ahead of its frame count at 120 fps during the movie and loads) is the reference's
  behaviour too and is not changed: it is the source of the `times` and `observables` numbers above.
