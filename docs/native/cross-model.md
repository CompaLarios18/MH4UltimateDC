# Cross-model calibration: jit against recomp

Written by N0-28 (D31, D33, D35). The tool is `tools/native/cross_model_report.py`; the runs are `golden.py check <S> --candidate-args "--cpu jit" --profile cross` on G1, G2 and G6. This document keeps frames, counts, service names and request headers only: the traces, screenshots and user folders it was made from are game data and stay in `build/`.

## Summary

**N0-28 ends `needs-decision`.** The two backends run the game correctly, each repeats itself byte for byte, and most of what the cross profile gates agrees: but four findings cannot be settled inside this work package.

| # | Finding | Where | What it needs |
|---|---|---|---|
| 1 | Two real scene changes of G2 fall **3 frames** apart (5084 against 5087, 5086 against 5089), over D33's 2. Every other real scene change (11 of 13) is 0 to 2 frames apart | [section 3](#3-real-scene-changes-and-input-margins-d33) | D33: the threshold is not widened here |
| 2 | The cross-profile DSP comparison of G1 fails: the two captures hold the **same audio stream** shifted by a constant 22 DSP frames (every one of 14064 compared source-status blocks is equal after the shift) | [section 6](#6-the-dsp-capture-of-g1) | a rule for DSP in the cross profile (`capfmt.py` is N0-06's) |
| 3 | After the 18 time-dependent entries below, three `fs:` differences remain: the movie's reader thread sends 5 fewer requests on jit (G1, G2, G6), one reader thread's requests come in another order in G6 and G2, and **the save write of G2 holds 24 different bytes** | [section 5](#5-ipc-calibration) and [section 7](#7-the-save-write-of-g2) | the trap of this work package: an `fs:` command is never proposed as time-dependent, and a difference that cannot be tied to time stops it |
| 4 | Everything else passes: the cross-model SVC comparison (`svc`), the state comparison on G1 and G6, the checkpoints and the HID capture on all three | [section 4](#4-svc-calls) | nothing |

The 18 IPC entries that **are** tied to time are proposed in [section 5.1](#51-the-entries-proposed-for-configipc_time_dependenttxt); they do not make `golden.py check` pass by themselves.

## 1. What was run

- Runner: the N0-29 runner (`mh4u_runner.exe`, the Windows CI build of `wp/N0-29-runner-exact` at `3aef9b8`), the recordings of G1, G2 and G6 made with it (`docs/native/golden.md`), Old 3DS, `TZ=UTC0`, the user folder `build/userbase-cfg`.
- Each session: `golden.py check <S> --candidate-args "--cpu jit" --profile cross` (the reference runner again, with `--cpu jit` and no code library), then again with `--ignore build/cross-model/ipc_time_dependent.txt --require checkpoints,ipc,svc,hid,state` (G1 also `dsp`). The outputs are in `build/cross-model/`: the recordings were hard-linked into `build/cross-model/rec/<S>/` so that `check`'s own output folder (`<S>/check/`) did not land in `build/golden/`.
- Host time of one jit run on the maintainer PC with nothing else running: 88 s (G1), 412 s (G6) and 2021 s (G2) (recomp: 64 s, 306 s and 1198 s, `docs/native/golden.md`). A whole `check` takes longer (G1 129 s, G6 554 s, G2 2297 s) because of the comparisons.
- **jit repeats itself exactly**, as N0-29's gates promised: the SVC, IPC and state traces and the HID capture of the jit runs of G1, G6 and G2 made for the first and for the second pass are identical by SHA-256 (the same N0-29 property in the second backend). So every difference below is a difference of the two backends' clocks or a bug, not noise.

### Results of `golden.py check` (cross profile)

| Comparison | G1 | G6 | G2 |
|---|---|---|---|
| `svc` (cross-model list) | pass | pass | pass |
| `state` (`--min-changes` = the recording's real scene changes) | pass | pass | **fail**: `state_diff.py` stops at its scene change #8 (counting the first record as #0; the real change to the `aGameSys` step), reference frame 5084, candidate frame 5087 |
| `checkpoints` | pass | pass | pass |
| `hid` | pass | pass | pass |
| `dsp-1240` | **fail** | - | - |
| `ipc`, first run | fail | fail | fail |
| `ipc`, with the 18 entries | fail (`fs:`, 1 thread) | fail (`fs:`, 2 threads) | fail (`fs:`, 3 threads) |
| `svc-full`, `images` | info (never gated): 19 of 48 screenshots differ | info: 57 of 114 | info: 140 of 292 |
| exit status | 1 | 1 | 1 |

(The state comparison of `check` stops at the first divergence; the report tool of [section 3](#3-real-scene-changes-and-input-margins-d33) lists all eight real changes and shows that the next one, to `aQuest`, is 3 frames apart as well.)

## 2. The report tool

`python tools/native/cross_model_report.py --session tools/native/golden/<S>.txt --reference build/golden/<S> --candidate build/cross-model/rec/<S>/check/run1 [--observable FILE] [--tolerance N] [--no-svc] [--no-ipc]` prints markdown: each real scene change (N0-25's definition: between two readable scene values, so the `-` start is not one) with its frame on each side and the difference; each input event of the session script with the last real scene change at or before it on each side and the margin (the event's frame minus that frame); each thread's number of SVC calls under the cross-model names of N0-25 (`main`, `<parent name>/<entry>#<k>`), with the SVC numbers that differ most; and, for information, the IPC commands whose per-thread request counts differ. It exits 1 when a real scene change is more than `--tolerance` frames (default 2) apart, goes to another scene, or exists on one side only (the report is still written), and 2 for bad input. `docs/native/golden.md` and `docs/native/comparators.md` describe the profiles it reports on; N0-30 and the native runtime's calibrations use the same tool.

## 3. Real scene changes and input margins (D33)

The real scene changes, each side's frame, and the difference (candidate minus reference; jit is never earlier):

| Session | Real changes | Differences (frames), in order | Largest |
|---|---|---|---|
| G1 | 2 | +0, +2 | 2 |
| G6 | 3 | +0, +2, +1 | 2 |
| G2 | 8 | +0, +2, +1, +1, +1, +1, **+3**, **+3** | **3** |

Over the three sessions 13 real scene changes were measured; 11 are within D33's 2 frames and two, both in G2, are 3 frames apart. Those two are the changes at which the area system leaves the village and enters the opening (the `aGameSys` step at 5084 and `aQuest` at 5086 on recomp): the shift is +1 from the creation (frame 1653) up to the village (4959), and +3 from there to the end of the session. This matches what N0-27 saw (`docs/research/scene-observable.md`: "3"), now with the exact N0-29 runner and the two runs repeating themselves. **D33 does not allow widening the threshold here, so the work package stops.**

What the shift does to the session's inputs (the margin is the input's frame minus the frame of the last real scene change on that side; the smallest margin is the closest an input comes after a scene change):

| Session | Input events | Smallest margin, reference | Smallest margin, candidate | The margins differ by |
|---|---|---|---|---|
| G1 | 8 | 264 (event at 300) | 262 | -2 |
| G6 | 23 | 148 (event at 1800) | 147 | -2 up to frame 1650, -1 after |
| G2 | 52 | 92 (event at 5050) | 91 | -2, -1, then -3 from frame 5200 on |

Every input of the three sessions comes **at least 91 frames** after the last real scene change on both backends, thirty times the largest shift. The scripts' inputs therefore cannot land in a different scene because of a shift of 3 frames; that is the figure the orchestrator needs for D33's criterion (the game's continuous stability). The emulated time at the last frame differs by 31.4 million ticks (7 frames of 4.48 million ticks) in G1, 35.8 million in G6 and 49.3 million in G2 (jit is later in all three): the jit run needs more emulated time for the same game frames, which is why the margins differ by a few frames.

The tables of the three sessions are in [the appendix](#appendix-the-report-tool-on-g1-g6-and-g2).

## 4. SVC calls

The cross-model SVC comparison (`--cross-model config/native/svc_cross_model.txt`, N0-25) **passes on all three sessions**: every compared call (thread creations, memory and handle operations) is the same in both backends, in the same place, with the same result. Compared calls per side: 128 (G1), 144 (G6) and 164 (G2), the same on both sides. So the recompiler's behaviour matches Azahar's own on every call the list covers, and the list needed no change.

The totals are not gated and differ, as N0-25 measured on G1 to frame 800: the full SVC call counts differ in 14 of 21 threads of G1 (17 of 25 in G2 and G6), by thread and call as the tables below show. The differences are polling and waiting: the main thread's `GetSystemTick` (0x28) count is 846638 against 630582 in G1, its `SleepThread` (0x0A) count 897 against 762, and the threads that sleep (`0x0A`) run a few more or fewer iterations; thread `#9` sends 11907 `SendSyncRequest` calls on jit against 1727 on recomp in G1 (all of them `TriggerCmdReqQueue`, [section 5](#5-ipc-calibration)). Nothing in them is a count of something the game keeps: they are loops that wait, sleep, poll or send requests, which is what a different clock changes.

## 5. IPC calibration

The method: `compare_ipc_traces.py` reports only the first difference per thread, so each pair was also aligned thread by thread (per-parent thread names, a request being its session, header, result and descriptors) to list every difference. The commands that differ in count or place were then tied to time one by one; the entries were added to `build/cross-model/ipc_time_dependent.txt` and the check run again, until what remained could not be tied to time.

### 5.1 The entries proposed for `config/native/ipc_time_dependent.txt`

Each of the 18 entries was needed (without it the comparison of that command fails in at least one session), and with all of them in place the main thread and every thread but the `fs:` readers of [5.2](#52-what-remains-and-is-not-proposed) match request for request in all three sessions.

| Entry | What the pair shows (reference / candidate requests) | Why it depends on time |
|---|---|---|
| `gsp::Gpu 0x000c0000` | TriggerCmdReqQueue: thread `#9` 1727 / 11907 (G1), 3742 / 25922 (G6), 9078 / 63058 (G2); the main thread 8489 / 8491, 18484 / 18486, 44972 / 44974. In thread `#9` the candidate's list is the reference's plus more requests of the same command; in the main thread the command moves among the other requests | a queue trigger sent from a loop (the ticks and SVC counts of thread `#9` are those of a polling loop), so how many are sent in a run follows the model's clock. The reply is a result code only |
| `gsp::Gpu 0x00080082` | FlushDataCache: the main thread 3669 / 3672 (G1), 19719 / 19712 (G6), 35664 / 35468 (G2) | the main thread flushes inside its frame loop, which polls the clock (`GetSystemTick`, 846638 / 630582 calls in G1), so the number of flushes between two other requests follows the clock (the main thread's first difference, at its request 256 in all three sessions, is a flush and a queue trigger in another order). With this entry and the one above hidden, the main thread's other requests are equal |
| `dsp::DSP 0x00130082` | FlushDataCache of the audio thread (`#11`): 294 / 292 (G1), 671 / 669 (G6), 1544 / 1538 (G2); the candidate's list is a prefix of the reference's | presumably one flush per audio buffer handed to the DSP: how many buffers were handed over when the movie was skipped and when the run ended follows the DSP clock, which is shifted in jit ([section 6](#6-the-dsp-capture-of-g1)). The candidate's list is cut short in all three sessions |
| `y2r:u` 15 headers: `0x00010040 0x00030040 0x00050040 0x00070040 0x000d0040 0x00100102 0x00110102 0x00120102 0x00180102 0x001a0040 0x001c0040 0x001e0100 0x00210040 0x00220040 0x00260000` | the intro movie's conversions: 232 / 230 requests of each (233 / 231 for `0x000d0040`) in G1, G6 and G2 | one set-up per movie frame, 232 on recomp against 230 on jit. The scripts skip the movie with an `A` at frame 1350; the last conversion of both runs is right before it (recomp 0.5 million ticks, jit 0.4 million ticks before the press), the movie started 40.8 million ticks later on jit while the press came 30.8 million ticks later, so jit played the movie 9.8 million ticks (2.2 frames at 4.4 million ticks each) shorter: the count is the movie's playing time. Replies carry results only |

The entries hide those commands from both traces in every later check, the native runtime's included. They are what the pair shows and no more; the 15 `y2r:u` lines are one fact (the movie's frame count) and could be one entry if the file allowed a service-wide line (it does not).

### 5.2 What remains, and is not proposed

With the 18 entries hidden, `compare_ipc_traces.py` exits 1 on these threads (names are the comparator's `<entry>#<k>`; the entry address is the game's thread start):

| Session | Thread | Difference | Commands |
|---|---|---|---|
| G1, G6, G2 | `0010D8D4#17` | 576 reference requests, 571 candidate; the candidate's list is a **prefix** of the reference's | `fs:USER/file` 0x080200c2 (Read of 2048 bytes), probably the intro movie's stream: it stops 5 requests short on jit |
| G6, G2 | `0010D8D4#12` | same number of requests (188 in G6; 563 / 562 in G2), but **two reads of 8192 bytes come earlier** on jit: one 8 positions earlier (request 25 on recomp, 17 on jit), one 1 position earlier (29 / 28); in G2 the thread also ends one 8192-byte read short | `fs:USER/file` 0x080200c2, a reader that interleaves reads of 8192 bytes with small ones (48 and 420 bytes) |
| G2 | `0010D8D4#13` | the same 224 requests, request 192 differs in its data | `fs:USER/file` 0x08030102 (Write, 81408 bytes): the save, [section 7](#7-the-save-write-of-g2) |

The first row is consistent with the measurement of the `y2r:u` entries: the reads of that thread stop at the scripted skip (the last one 7.7 million ticks after the press on recomp and 7.8 million on jit), and jit played the movie 2.2 frames shorter, so 5 reads of 2048 bytes (10 KiB) were not made. That it is the movie's stream is an inference from those times, not something the traces name. The second row is an order difference inside one thread, with the same set of requests: it is plausibly a race between that reader and the threads that feed it, which the clock decides, but this work package did not trace the cause, and neither its spec nor DECISIONS.md has a rule for it. The third row is not tied to time. **None of the three is an entry**: the trap of N0-28 forbids an `fs:` command in `ipc_time_dependent.txt`, and a line there would hide every `fs:USER/file` Read of both traces anyway (the entry format has no thread or file, only the session and the header).

Options for the orchestrator (decisions, not made here): a prefix rule for a listed reader thread (the comparator's `--prefix` exists, but is one-sided and for every thread); an order-insensitive comparison of one thread's reads of one file (a multiset, with the same counts and CRCs); or accept the `fs:` reads of the movie and the loader as not comparable across models and compare their totals only. The first row, the movie, would also disappear if the sessions' scripts were keyed to the game and not to frame 1350, but they are frame-keyed by design (`docs/native/golden.md`).

## 6. The DSP capture of G1

`golden.py check G1` fails `dsp-1240` (`capfmt.py diff-dsp`: "the reference starts 15 DSP frames earlier; they are skipped", then every source status of the overlap differs): the capture starts at game frame 1240, and at that frame the DSP frame counter is 4434 on recomp and 4449 on jit.

Analysis of the two captures (300 DSP frames each, two regions, one source enabled per region):

- the audio stream is the same: the sequence of buffer ids of source 0 in each region changes in the same pattern in both captures (the same intervals of 2 and 4 DSP frames between buffer changes);
- the same buffer id comes at a DSP frame that is later on jit by a **constant**: 22 DSP frames in region 0 for 87 of 87 buffers, 22 in region 1 for 85 of 86 buffers (21 for one);
- after shifting the candidate by 22 DSP frames, all 14064 source-status blocks that overlap are **equal** (enabled, dirty, sync count, buffer position, current and last buffer id): 0 differ.

So the two backends play the same sound; it begins 22 DSP frames (about 107 ms, 28.8 million ticks) later on jit, and `diff-dsp` keys on the DSP frame counter, which is the model's own time. The emulated time at game frame 1240 is 31.4 million ticks later on jit (24 DSP frames), the same order. This is tied to time and no entry or option of this work package covers a DSP capture: `capfmt.py` belongs to N0-06. Options: align the candidate on the first buffer change before comparing (a rule in `diff_dsp`), or leave `dsp` information-only in the cross profile (D31 gates it).

## 7. The save write of G2

At game frame 4917 (reference) and 4919 (candidate), thread `#13` writes the new game's save with one `Write` (`fs:USER/file`, 81408 bytes at offset 0). The two requests carry the same header, the same size and the same offset; of the 81408 bytes **24 differ, in two ranges: offsets 0 to 7 and 280 to 295**. The write's buffer has CRC-32 `0x4bc7a915` on recomp and `0x2cf8030a` on jit. The 24 bytes are not a copy of the clock: none of the six 32-bit words at those offsets equals the first result register of any `GetSystemTick` call of its own run's SVC trace, and the two runs' values differ by numbers that are not small. A hash, a random value seeded from the clock or a save identifier are all possible; which one it is needs the game's save code, which this work package does not read. Each backend repeats its own value (N0-29's gates, the recordings' user folders), so the cause is either a value derived from timing or a backend difference. Never an entry: it is save data.

What this means for later work: a native replay of G2 compared in the cross profile will show the same 24 bytes whenever its clock differs from the reference's, so someone has to find out what they are before the `fs:` comparison of G2 is calibrated.

## 8. Conclusion for D33, and what is asked

1. **D33 (the 2-frame threshold).** Measured on 13 real scene changes in three sessions: 0 to 2 frames apart for 11 and 3 frames for the two at the end of G2's village. The work package stops as D33 requires. The figures to decide with: the largest shift is 3 frames; every scripted input comes at least 91 frames after the last real scene change on both backends; the shift is jit running later in emulated time (31 to 49 million ticks over the whole session), not scenes in another order (every scene sequence is the same on both sides).
2. **DSP in the cross profile**: a constant shift of the DSP counter, with equal content; a rule is needed.
3. **`fs:` differences across models**: the reader thread's prefix and order, the save's 24 bytes. Not time entries; they need a rule or an investigation.
4. **Entries**: the 18 of [5.1](#51-the-entries-proposed-for-configipc_time_dependenttxt) are tied to time and proposed, to be added once the three points above are settled (a partial list does not make any `check` pass).

What this calibration confirms: the SVC cross-model list holds on 128 to 164 compared calls per session ([section 4](#4-svc-calls)); the state comparison and the checkpoints are sound as soon as D33 is decided; HID matches in all three; the time-dependent IPC commands are few (the GPU queue, the cache flush, the movie, the audio flush) and the ones that remain are all `fs:` reads and a write.

## Appendix: the report tool on G1, G6 and G2

### G1: jit (candidate) against recomp (reference)

#### Real scene changes (2 on the reference, 2 on the candidate; tolerance 2 frames)

| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |
|---|---|---|---|---|---|---|
| 1 | 0x00e6c100 | 0x00e6c100 | 2 | 2 | +0 | yes |
| 2 | 0x00e64d5c | 0x00e64d5c | 36 | 38 | +2 | yes |

The largest difference is 2 frames.

#### Input events (8)

| frame | input | last scene change (reference) | last scene change (candidate) | margin (reference) | margin (candidate) | margin difference |
|---|---|---|---|---|---|---|
| 300 | A | 36 | 38 | 264 | 262 | -2 |
| 450 | A | 36 | 38 | 414 | 412 | -2 |
| 600 | A | 36 | 38 | 564 | 562 | -2 |
| 750 | A | 36 | 38 | 714 | 712 | -2 |
| 900 | A | 36 | 38 | 864 | 862 | -2 |
| 1050 | A | 36 | 38 | 1014 | 1012 | -2 |
| 1200 | A | 36 | 38 | 1164 | 1162 | -2 |
| 1350 | A | 36 | 38 | 1314 | 1312 | -2 |

- smallest margin on the reference: 264 frames (the event at frame 300)
- smallest margin on the candidate: 262 frames (the event at frame 300)

#### SVC calls per thread (cross-model names)

| thread | calls (reference) | calls (candidate) | difference | the SVC numbers that differ most |
|---|---|---|---|---|
| main | 1017886 | 814936 | -202950 | 0x28 GetSystemTick 846638/630582, 0x37 146695/160057, 0x0A SleepThread 897/762 |
| main/0010D8D4#0 | 1 | 1 | +0 | - |
| main/0010D8D4#1 | 9 | 9 | +0 | - |
| main/0010D8D4#10 | 3 | 3 | +0 | - |
| main/0010D8D4#11 | 26691 | 26755 | +64 | 0x18 SignalEvent 6381/6396, 0x19 ClearEvent 6381/6396, 0x22 ArbitrateAddress 27/42 |
| main/0010D8D4#12 | 626 | 628 | +2 | 0x0A SleepThread 624/626 |
| main/0010D8D4#13 | 259 | 259 | +0 | - |
| main/0010D8D4#14 | 314 | 315 | +1 | 0x0A SleepThread 312/313 |
| main/0010D8D4#15 | 1851 | 1858 | +7 | 0x0A SleepThread 1847/1854 |
| main/0010D8D4#16 | 1848 | 1855 | +7 | 0x0A SleepThread 1847/1854 |
| main/0010D8D4#17 | 1714 | 1699 | -15 | 0x22 ArbitrateAddress 1137/1127, 0x32 SendSyncRequest 576/571 |
| main/0010D8D4#18 | 2474 | 2463 | -11 | 0x0A SleepThread 1278/1228, 0x22 ArbitrateAddress 1195/1234 |
| main/0010D8D4#19 | 96 | 149 | +53 | 0x22 ArbitrateAddress 95/148 |
| main/0010D8D4#2 | 317 | 319 | +2 | 0x0A SleepThread 315/317 |
| main/0010D8D4#3 | 1954 | 1945 | -9 | 0x0A SleepThread 124/117, 0x22 ArbitrateAddress 71/69 |
| main/0010D8D4#4 | 369 | 386 | +17 | 0x0A SleepThread 113/119, 0x0B 113/119, 0x0C 127/133 |
| main/0010D8D4#5 | 65 | 65 | +0 | - |
| main/0010D8D4#6 | 1 | 1 | +0 | - |
| main/0010D8D4#7 | 1 | 1 | +0 | - |
| main/0010D8D4#8 | 10131 | 10287 | +156 | 0x37 4486/4590, 0x0A SleepThread 2243/2295 |
| main/0010D8D4#9 | 15445 | 25516 | +10071 | 0x32 SendSyncRequest 1727/11907, 0x22 ArbitrateAddress 3161/3054, 0x19 ClearEvent 5278/5277 |
| **all 21 threads** | 1082055 | 889450 | -192605 | 14 of 21 threads differ |

#### IPC requests per command

18932 requests on the reference, 29080 on the candidate, 80 distinct commands; 19 of them with a different count on at least one thread.

| service | header | requests (reference) | requests (candidate) | difference | threads that differ (reference/candidate) |
|---|---|---|---|---|---|
| dsp::DSP | 0x00130082 | 294 | 292 | -2 | main/0010D8D4#11 294/292 |
| fs:USER/file | 0x080200c2 | 674 | 669 | -5 | main/0010D8D4#17 576/571 |
| gsp::Gpu | 0x00080082 | 3669 | 3672 | +3 | main 3669/3672 |
| gsp::Gpu | 0x000c0000 | 10216 | 20398 | +10182 | main 8489/8491, main/0010D8D4#9 1727/11907 |
| y2r:u | 0x00010040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00030040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00050040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00070040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x000d0040 | 233 | 231 | -2 | main 233/231 |
| y2r:u | 0x00100102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00110102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00120102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00180102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001a0040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001c0040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001e0100 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00210040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00220040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00260000 | 232 | 230 | -2 | main 232/230 |

### G6: jit (candidate) against recomp (reference)

#### Real scene changes (3 on the reference, 3 on the candidate; tolerance 2 frames)

| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |
|---|---|---|---|---|---|---|
| 1 | 0x00e6c100 | 0x00e6c100 | 2 | 2 | +0 | yes |
| 2 | 0x00e64d5c | 0x00e64d5c | 36 | 38 | +2 | yes |
| 3 | 0x00e6c138 | 0x00e6c138 | 1652 | 1653 | +1 | yes |

The largest difference is 2 frames.

#### Input events (23)

| frame | input | last scene change (reference) | last scene change (candidate) | margin (reference) | margin (candidate) | margin difference |
|---|---|---|---|---|---|---|
| 300 | A | 36 | 38 | 264 | 262 | -2 |
| 450 | A | 36 | 38 | 414 | 412 | -2 |
| 600 | A | 36 | 38 | 564 | 562 | -2 |
| 750 | A | 36 | 38 | 714 | 712 | -2 |
| 900 | A | 36 | 38 | 864 | 862 | -2 |
| 1050 | A | 36 | 38 | 1014 | 1012 | -2 |
| 1200 | A | 36 | 38 | 1164 | 1162 | -2 |
| 1350 | A | 36 | 38 | 1314 | 1312 | -2 |
| 1500 | A | 36 | 38 | 1464 | 1462 | -2 |
| 1650 | A | 36 | 38 | 1614 | 1612 | -2 |
| 1800 | A | 1652 | 1653 | 148 | 147 | -1 |
| 1950 | TOUCH | 1652 | 1653 | 298 | 297 | -1 |
| 2000 | TOUCH | 1652 | 1653 | 348 | 347 | -1 |
| 2050 | TOUCH | 1652 | 1653 | 398 | 397 | -1 |
| 2100 | TOUCH | 1652 | 1653 | 448 | 447 | -1 |
| 2400 | TOUCH | 1652 | 1653 | 748 | 747 | -1 |
| 2600 | TOUCH | 1652 | 1653 | 948 | 947 | -1 |
| 2800 | A | 1652 | 1653 | 1148 | 1147 | -1 |
| 3000 | A | 1652 | 1653 | 1348 | 1347 | -1 |
| 3250 | TOUCH | 1652 | 1653 | 1598 | 1597 | -1 |
| 3300 | TOUCH | 1652 | 1653 | 1648 | 1647 | -1 |
| 3350 | TOUCH | 1652 | 1653 | 1698 | 1697 | -1 |
| 3550 | TOUCH | 1652 | 1653 | 1898 | 1897 | -1 |

- smallest margin on the reference: 148 frames (the event at frame 1800)
- smallest margin on the candidate: 147 frames (the event at frame 1800)

#### SVC calls per thread (cross-model names)

| thread | calls (reference) | calls (candidate) | difference | the SVC numbers that differ most |
|---|---|---|---|---|
| main | 3564667 | 3176047 | -388620 | 0x28 GetSystemTick 1591347/1174433, 0x37 1914441/1942991, 0x0A SleepThread 1616/1277 |
| main/0010D8D4#0 | 1 | 1 | +0 | - |
| main/0010D8D4#1 | 9 | 9 | +0 | - |
| main/0010D8D4#10 | 3 | 3 | +0 | - |
| main/0010D8D4#11 | 54813 | 54895 | +82 | 0x18 SignalEvent 13318/13336, 0x19 ClearEvent 13318/13336, 0x24 WaitSynchronization1 13319/13337 |
| main/0010D8D4#12 | 1491 | 1493 | +2 | 0x0A SleepThread 1297/1299 |
| main/0010D8D4#13 | 259 | 259 | +0 | - |
| main/0010D8D4#14 | 653 | 654 | +1 | 0x0A SleepThread 651/652 |
| main/0010D8D4#15 | 3877 | 3882 | +5 | 0x0A SleepThread 3873/3878 |
| main/0010D8D4#16 | 3890 | 3895 | +5 | 0x0A SleepThread 3873/3878 |
| main/0010D8D4#17 | 1714 | 1699 | -15 | 0x22 ArbitrateAddress 1137/1127, 0x32 SendSyncRequest 576/571 |
| main/0010D8D4#18 | 2474 | 2463 | -11 | 0x0A SleepThread 1278/1228, 0x22 ArbitrateAddress 1195/1234 |
| main/0010D8D4#19 | 96 | 149 | +53 | 0x22 ArbitrateAddress 95/148 |
| main/0010D8D4#2 | 657 | 658 | +1 | 0x0A SleepThread 655/656 |
| main/0010D8D4#20 | 69 | 69 | +0 | - |
| main/0010D8D4#21 | 10269 | 10247 | -22 | 0x0A SleepThread 10266/10244 |
| main/0010D8D4#22 | 69 | 70 | +1 | 0x22 ArbitrateAddress 3/4 |
| main/0010D8D4#23 | 9403 | 9412 | +9 | 0x0A SleepThread 9400/9409 |
| main/0010D8D4#3 | 2242 | 2233 | -9 | 0x0A SleepThread 124/117, 0x22 ArbitrateAddress 88/86 |
| main/0010D8D4#4 | 414 | 430 | +16 | 0x0A SleepThread 125/131, 0x0B 125/131, 0x0C 145/150 |
| main/0010D8D4#5 | 67 | 67 | +0 | - |
| main/0010D8D4#6 | 1 | 1 | +0 | - |
| main/0010D8D4#7 | 1 | 1 | +0 | - |
| main/0010D8D4#8 | 21007 | 21178 | +171 | 0x37 9070/9184, 0x0A SleepThread 4535/4592 |
| main/0010D8D4#9 | 33004 | 55291 | +22287 | 0x32 SendSyncRequest 3742/25922, 0x22 ArbitrateAddress 6649/6754, 0x19 ClearEvent 11306/11307 |
| **all 25 threads** | 3711150 | 3345106 | -366044 | 17 of 25 threads differ |

#### IPC requests per command

47754 requests on the reference, 69892 on the candidate, 81 distinct commands; 19 of them with a different count on at least one thread.

| service | header | requests (reference) | requests (candidate) | difference | threads that differ (reference/candidate) |
|---|---|---|---|---|---|
| dsp::DSP | 0x00130082 | 671 | 669 | -2 | main/0010D8D4#11 671/669 |
| fs:USER/file | 0x080200c2 | 1047 | 1042 | -5 | main/0010D8D4#17 576/571 |
| gsp::Gpu | 0x00080082 | 19719 | 19712 | -7 | main 19719/19712 |
| gsp::Gpu | 0x000c0000 | 22226 | 44408 | +22182 | main 18484/18486, main/0010D8D4#9 3742/25922 |
| y2r:u | 0x00010040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00030040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00050040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00070040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x000d0040 | 233 | 231 | -2 | main 233/231 |
| y2r:u | 0x00100102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00110102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00120102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00180102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001a0040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001c0040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001e0100 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00210040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00220040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00260000 | 232 | 230 | -2 | main 232/230 |

### G2: jit (candidate) against recomp (reference)

#### Real scene changes (8 on the reference, 8 on the candidate; tolerance 2 frames)

| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |
|---|---|---|---|---|---|---|
| 1 | 0x00e6c100 | 0x00e6c100 | 2 | 2 | +0 | yes |
| 2 | 0x00e64d5c | 0x00e64d5c | 36 | 38 | +2 | yes |
| 3 | 0x00e6c138 | 0x00e6c138 | 1652 | 1653 | +1 | yes |
| 4 | 0x00e64d5c | 0x00e64d5c | 4955 | 4956 | +1 | yes |
| 5 | 0x00e6c100 | 0x00e6c100 | 4957 | 4958 | +1 | yes |
| 6 | 0x00e6c170 | 0x00e6c170 | 4958 | 4959 | +1 | yes |
| 7 | 0x00e6c100 | 0x00e6c100 | 5084 | 5087 | +3 | no |
| 8 | 0x00e655c8 | 0x00e655c8 | 5086 | 5089 | +3 | no |

#### Input events (52)

| frame | input | last scene change (reference) | last scene change (candidate) | margin (reference) | margin (candidate) | margin difference |
|---|---|---|---|---|---|---|
| 300 | A | 36 | 38 | 264 | 262 | -2 |
| 450 | A | 36 | 38 | 414 | 412 | -2 |
| 600 | A | 36 | 38 | 564 | 562 | -2 |
| 750 | A | 36 | 38 | 714 | 712 | -2 |
| 900 | A | 36 | 38 | 864 | 862 | -2 |
| 1050 | A | 36 | 38 | 1014 | 1012 | -2 |
| 1200 | A | 36 | 38 | 1164 | 1162 | -2 |
| 1350 | A | 36 | 38 | 1314 | 1312 | -2 |
| 1500 | A | 36 | 38 | 1464 | 1462 | -2 |
| 1650 | A | 36 | 38 | 1614 | 1612 | -2 |
| 1800 | A | 1652 | 1653 | 148 | 147 | -1 |
| 1950 | TOUCH | 1652 | 1653 | 298 | 297 | -1 |
| 2000 | TOUCH | 1652 | 1653 | 348 | 347 | -1 |
| 2050 | TOUCH | 1652 | 1653 | 398 | 397 | -1 |
| 2100 | TOUCH | 1652 | 1653 | 448 | 447 | -1 |
| 2400 | TOUCH | 1652 | 1653 | 748 | 747 | -1 |
| 2600 | TOUCH | 1652 | 1653 | 948 | 947 | -1 |
| 2800 | A | 1652 | 1653 | 1148 | 1147 | -1 |
| 3000 | A | 1652 | 1653 | 1348 | 1347 | -1 |
| 3250 | TOUCH | 1652 | 1653 | 1598 | 1597 | -1 |
| 3300 | TOUCH | 1652 | 1653 | 1648 | 1647 | -1 |
| 3350 | TOUCH | 1652 | 1653 | 1698 | 1697 | -1 |
| 3550 | TOUCH | 1652 | 1653 | 1898 | 1897 | -1 |
| 3750 | TOUCH | 1652 | 1653 | 2098 | 2097 | -1 |
| 3950 | A | 1652 | 1653 | 2298 | 2297 | -1 |
| 4150 | A | 1652 | 1653 | 2498 | 2497 | -1 |
| 4300 | A | 1652 | 1653 | 2648 | 2647 | -1 |
| 4450 | A | 1652 | 1653 | 2798 | 2797 | -1 |
| 4600 | A | 1652 | 1653 | 2948 | 2947 | -1 |
| 4750 | A | 1652 | 1653 | 3098 | 3097 | -1 |
| 4900 | A | 1652 | 1653 | 3248 | 3247 | -1 |
| 5050 | A | 4958 | 4959 | 92 | 91 | -1 |
| 5200 | A | 5086 | 5089 | 114 | 111 | -3 |
| 5350 | A | 5086 | 5089 | 264 | 261 | -3 |
| 5500 | A | 5086 | 5089 | 414 | 411 | -3 |
| 5650 | A | 5086 | 5089 | 564 | 561 | -3 |
| 5800 | A | 5086 | 5089 | 714 | 711 | -3 |
| 5950 | A | 5086 | 5089 | 864 | 861 | -3 |
| 6100 | A | 5086 | 5089 | 1014 | 1011 | -3 |
| 6250 | A | 5086 | 5089 | 1164 | 1161 | -3 |
| 6400 | A | 5086 | 5089 | 1314 | 1311 | -3 |
| 6550 | A | 5086 | 5089 | 1464 | 1461 | -3 |
| 6700 | A | 5086 | 5089 | 1614 | 1611 | -3 |
| 6850 | A | 5086 | 5089 | 1764 | 1761 | -3 |
| 7000 | A | 5086 | 5089 | 1914 | 1911 | -3 |
| 7150 | A | 5086 | 5089 | 2064 | 2061 | -3 |
| 7300 | A | 5086 | 5089 | 2214 | 2211 | -3 |
| 7450 | A | 5086 | 5089 | 2364 | 2361 | -3 |
| 7600 | A | 5086 | 5089 | 2514 | 2511 | -3 |
| 7750 | A | 5086 | 5089 | 2664 | 2661 | -3 |
| 7900 | A | 5086 | 5089 | 2814 | 2811 | -3 |
| 8700 | CIRCLE | 5086 | 5089 | 3614 | 3611 | -3 |

- smallest margin on the reference: 92 frames (the event at frame 5050)
- smallest margin on the candidate: 91 frames (the event at frame 5050)

#### SVC calls per thread (cross-model names)

| thread | calls (reference) | calls (candidate) | difference | the SVC numbers that differ most |
|---|---|---|---|---|
| main | 6687785 | 6302440 | -385345 | 0x28 GetSystemTick 2276078/1823374, 0x37 4288701/4356575, 0x0A SleepThread 2133/1836 |
| main/0010D8D4#0 | 1 | 1 | +0 | - |
| main/0010D8D4#1 | 9 | 9 | +0 | - |
| main/0010D8D4#10 | 3 | 3 | +0 | - |
| main/0010D8D4#11 | 128614 | 128735 | +121 | 0x18 SignalEvent 31551/31579, 0x19 ClearEvent 31551/31579, 0x24 WaitSynchronization1 31552/31580 |
| main/0010D8D4#12 | 3640 | 3642 | +2 | 0x0A SleepThread 3069/3072, 0x32 SendSyncRequest 563/562 |
| main/0010D8D4#13 | 356 | 356 | +0 | - |
| main/0010D8D4#14 | 1544 | 1546 | +2 | 0x0A SleepThread 1542/1544 |
| main/0010D8D4#15 | 9210 | 9213 | +3 | 0x0A SleepThread 9206/9209 |
| main/0010D8D4#16 | 9223 | 9226 | +3 | 0x0A SleepThread 9206/9209 |
| main/0010D8D4#17 | 1714 | 1699 | -15 | 0x22 ArbitrateAddress 1137/1127, 0x32 SendSyncRequest 576/571 |
| main/0010D8D4#18 | 2474 | 2463 | -11 | 0x0A SleepThread 1278/1228, 0x22 ArbitrateAddress 1195/1234 |
| main/0010D8D4#19 | 96 | 149 | +53 | 0x22 ArbitrateAddress 95/148 |
| main/0010D8D4#2 | 1548 | 1550 | +2 | 0x0A SleepThread 1546/1548 |
| main/0010D8D4#20 | 69 | 69 | +0 | - |
| main/0010D8D4#21 | 10269 | 10247 | -22 | 0x0A SleepThread 10266/10244 |
| main/0010D8D4#22 | 69 | 70 | +1 | 0x22 ArbitrateAddress 3/4 |
| main/0010D8D4#23 | 9403 | 9412 | +9 | 0x0A SleepThread 9400/9409 |
| main/0010D8D4#3 | 10062 | 10048 | -14 | 0x0A SleepThread 701/689, 0x22 ArbitrateAddress 286/284 |
| main/0010D8D4#4 | 1492 | 1562 | +70 | 0x0A SleepThread 467/491, 0x0B 467/491, 0x0C 514/537 |
| main/0010D8D4#5 | 297 | 297 | +0 | - |
| main/0010D8D4#6 | 1 | 1 | +0 | - |
| main/0010D8D4#7 | 1 | 1 | +0 | - |
| main/0010D8D4#8 | 48986 | 50489 | +1503 | 0x37 20656/21658, 0x0A SleepThread 10328/10829 |
| main/0010D8D4#9 | 80673 | 134651 | +53978 | 0x32 SendSyncRequest 9078/63058, 0x22 ArbitrateAddress 17116/17106, 0x19 ClearEvent 27239/27243 |
| **all 25 threads** | 7007539 | 6677879 | -329660 | 17 of 25 threads differ |

#### IPC requests per command

97068 requests on the reference, 150812 on the candidate, 82 distinct commands; 19 of them with a different count on at least one thread.

| service | header | requests (reference) | requests (candidate) | difference | threads that differ (reference/candidate) |
|---|---|---|---|---|---|
| dsp::DSP | 0x00130082 | 1544 | 1538 | -6 | main/0010D8D4#11 1544/1538 |
| fs:USER/file | 0x080200c2 | 1627 | 1621 | -6 | main/0010D8D4#12 563/562, main/0010D8D4#17 576/571 |
| gsp::Gpu | 0x00080082 | 35664 | 35468 | -196 | main 35664/35468 |
| gsp::Gpu | 0x000c0000 | 54050 | 108032 | +53982 | main 44972/44974, main/0010D8D4#9 9078/63058 |
| y2r:u | 0x00010040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00030040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00050040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00070040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x000d0040 | 233 | 231 | -2 | main 233/231 |
| y2r:u | 0x00100102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00110102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00120102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00180102 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001a0040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001c0040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x001e0100 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00210040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00220040 | 232 | 230 | -2 | main 232/230 |
| y2r:u | 0x00260000 | 232 | 230 | -2 | main 232/230 |
