# Cross-model calibration: jit against recomp

Written by N0-28 (D31, D33, D35), and brought up to date in a fix round after the orchestrator settled the first round's findings in D50, D51 and D52 (2026-10-21). The tool is `tools/native/cross_model_report.py`; the runs are `golden.py check <S> --candidate-args "--cpu jit" --profile cross` on G1, G2 and G6. This document keeps frames, counts, service names and request headers only: the traces, screenshots and user folders it was made from are game data and stay in `build/`.

## Summary

**N0-28 is done: the measurement, the report tool and the time-dependent entries are delivered, and what its first run found is settled and handed on.** The two backends run the game correctly, each repeats itself byte for byte, and most of what the cross profile gates agrees. The first round ended `needs-decision` on three findings (a fourth was "everything else passes"); the orchestrator settled them in D50 to D52 and in the specs of N0-28, N0-31 and N0-32 (`docs/native/wp/N0.md`):

| # | Finding of the first round | Settled by | Who builds it, and which part |
|---|---|---|---|
| 1 | Two real scene changes of G2 fall **3 frames** apart (5084 against 5087, 5086 against 5089), over D33's 2. Every other real scene change (11 of 13) is 0 to 2 frames apart; every scripted input comes at least 91 frames after the last real scene change on both sides ([section 3](#3-real-scene-changes-and-input-margins-d33-and-d50)) | **D50**: the cross profile's scene tolerance is **4 frames** (the `same` profile stays at 0), with a guard that every scripted input comes at least **60 frames** after the last real scene change on both sides | **N0-28 (this work package)**: `cross_model_report.py` has `--tolerance` (default 4) and `--min-margin` (default 60) and exits 1 when either is violated. **N0-32**: `golden.py check --profile cross` uses the same two numbers by default, in its report and in its exit status. Checked here against the tables: the tool exits 0 on G1, G2 and G6, the largest shift is 3 and the smallest margin 91 |
| 2 | The cross-profile DSP comparison of G1 fails: the two captures hold the **same audio stream** shifted by a constant 22 DSP frames (every one of 14064 compared source-status blocks is equal after the shift) ([section 6](#6-the-dsp-capture-of-g1)) | **D51**: align the candidate on the first buffer-id change of the first enabled source, the shift constant and at most 48 DSP frames, every source status block equal after it; the PCM samples are not compared across models | **N0-32**: `capfmt.py diff-dsp --align [--max-shift 48]` and its use by the cross profile |
| 3a | After the 18 time-dependent entries, the movie's reader thread sends 5 fewer `fs:` Read requests on jit (G1, G2, G6) and one reader thread's reads come in another order (G6, G2) ([section 5.2](#52-what-remained-and-is-not-an-entry)) | **D52**: `fs:USER/file` Reads are compared per thread as a multiset of (offset, size, CRC of the data read), order ignored, the smaller contained in the larger with a surplus of at most 16 reads; no `fs:` command is ever an entry | **N0-32**: `compare_ipc_traces.py --fs-reads-multiset [--max-surplus 16]` and its use by the cross profile |
| 3b | **The save write of G2 holds 24 different bytes** (offsets 0 to 7 and 280 to 295 of 81,408) ([section 7](#7-the-save-write-of-g2)) | **D52**: a Write is compared in full, including the CRC of the written bytes, except the byte ranges that are proved to be derived from time | **N0-31**: finds what the 24 bytes are, and proposes the ranges for `config/native/fs_write_masks.txt` (or reports a backend bug). **N0-32**: `compare_ipc_traces.py --fs-write-mask FILE` and its use by the cross profile with that file |
| 4 | Everything else passes: the cross-model SVC comparison (`svc`), the state comparison on G1 and G6, the checkpoints and the HID capture on all three ([section 4](#4-svc-calls)) | nothing to settle | nothing |

The 18 IPC entries that **are** tied to time are now in `config/native/ipc_time_dependent.txt` (the orchestrator merged them from [section 5.1](#51-the-18-entries-now-in-confignativeipc_time_dependenttxt)). `golden.py check <S> --candidate-args "--cpu jit" --profile cross ... --require checkpoints,ipc,svc,hid` exiting 0 for the three sessions was moved to N0-32, because it needs the rules of D51 and D52: with the 18 entries alone everything passes except G1's DSP capture (a constant shift), the `fs:` reads of the movie and of one reader, and G2's save write.

## 1. What was run

- Runner: the N0-29 runner (`mh4u_runner.exe`, the Windows CI build of `wp/N0-29-runner-exact` at `3aef9b8`), the recordings of G1, G2 and G6 made with it (`docs/native/golden.md`), Old 3DS, `TZ=UTC0`, the user folder `build/userbase-cfg`.
- Each session: `golden.py check <S> --candidate-args "--cpu jit" --profile cross` (the reference runner again, with `--cpu jit` and no code library), then again with `--ignore build/cross-model/ipc_time_dependent.txt --require checkpoints,ipc,svc,hid,state` (G1 also `dsp`). The outputs are in `build/cross-model/`: the recordings were hard-linked into `build/cross-model/rec/<S>/` so that `check`'s own output folder (`<S>/check/`) did not land in `build/golden/`.
- Host time of one jit run on the maintainer PC with nothing else running: 88 s (G1), 412 s (G6) and 2021 s (G2) (recomp: 64 s, 306 s and 1198 s, `docs/native/golden.md`). A whole `check` takes longer (G1 129 s, G6 554 s, G2 2297 s) because of the comparisons.
- **jit repeats itself exactly**, as N0-29's gates promised: the SVC, IPC and state traces and the HID capture of the jit runs of G1, G6 and G2 made for the first and for the second pass are identical by SHA-256 (the same N0-29 property in the second backend). So every difference below is a difference of the two backends' clocks or a bug, not noise.

### Results of `golden.py check` (cross profile), the first round: history

This table is what the first round measured, with D33's 2 frames and before D51 and D52. It is the history the findings come from, **not** today's expected result: under D50 the state comparison of G2 passes (its largest shift is 3, see [section 3](#3-real-scene-changes-and-input-margins-d33-and-d50)), and the `dsp-1240` and `ipc` rows are what N0-32 builds the rules for. The expected result of the three sessions is the `exit 0` of N0-32's *Done when*.

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

(The state comparison of `check` stops at the first divergence; the report tool of [section 3](#3-real-scene-changes-and-input-margins-d33-and-d50) lists all eight real changes and shows that the next one, to `aQuest`, is 3 frames apart as well.)

## 2. The report tool

`python tools/native/cross_model_report.py --session tools/native/golden/<S>.txt --reference build/golden/<S> --candidate build/cross-model/rec/<S>/check/run1 [--observable FILE] [--tolerance N] [--min-margin N] [--no-svc] [--no-ipc]` prints markdown: each real scene change (N0-25's definition: between two readable scene values, so the `-` start is not one) with its frame on each side and the difference; each input event of the session script with the last real scene change at or before it on each side and the margin (the event's frame minus that frame), and a line that says whether every margin reaches the minimum; each thread's number of SVC calls under the cross-model names of N0-25 (`main`, `<parent name>/<entry>#<k>`), with the SVC numbers that differ most; and, for information, the IPC commands whose per-thread request counts differ.

The two limits are D50's: `--tolerance` (default 4) and `--min-margin` (default 60). It exits 1 when a real scene change is more than `--tolerance` frames apart, goes to another scene or exists on one side only, **or** when any input event that follows a real scene change has a margin below `--min-margin` on either side; each problem is named on stderr (for a margin: the event's frame and kind, the margin and the side) and the report is still written. An input before the first real scene change has no margin and is not checked. It exits 2 for bad input (a missing file, a trace that cannot be read, a negative number). `docs/native/golden.md` and `docs/native/comparators.md` describe the profiles it reports on; N0-30 and the native runtime's calibrations use the same tool, and `golden.py check --profile cross` takes its two numbers from D50 (N0-32).

## 3. Real scene changes and input margins (D33 and D50)

The real scene changes, each side's frame, and the difference (candidate minus reference; jit is never earlier). The threshold was D33's 2 frames when this was measured; the measurement is what D50 settled on (4 frames and a margin guard of 60), and [section 3.1](#31-d50s-numbers-checked-against-these-tables) checks D50's numbers against the same runs.

| Session | Real changes | Differences (frames), in order | Largest |
|---|---|---|---|
| G1 | 2 | +0, +2 | 2 |
| G6 | 3 | +0, +2, +1 | 2 |
| G2 | 8 | +0, +2, +1, +1, +1, +1, **+3**, **+3** | **3** |

Over the three sessions 13 real scene changes were measured; 11 are within D33's 2 frames and two, both in G2, are 3 frames apart. Those two are the changes at which the area system leaves the village and enters the opening (the `aGameSys` step at 5084 and `aQuest` at 5086 on recomp): the shift is +1 from the creation (frame 1653) up to the village (4959), and +3 from there to the end of the session. This matches what N0-27 saw (`docs/research/scene-observable.md`: "3"), now with the exact N0-29 runner and the two runs repeating themselves. D33 did not allow widening the threshold from inside the work package, so the first round stopped here with this table; D50 then settled the numbers.

What the shift does to the session's inputs (the margin is the input's frame minus the frame of the last real scene change on that side; the smallest margin is the closest an input comes after a scene change):

| Session | Input events | Smallest margin, reference | Smallest margin, candidate | The margins differ by |
|---|---|---|---|---|
| G1 | 8 | 264 (event at 300) | 262 | -2 |
| G6 | 23 | 148 (event at 1800) | 147 | -2 up to frame 1650, -1 after |
| G2 | 52 | 92 (event at 5050) | 91 | -2, -1, then -3 from frame 5200 on |

Every input of the three sessions comes **at least 91 frames** after the last real scene change on both backends, thirty times the largest shift. The scripts' inputs therefore cannot land in a different scene because of a shift of 3 frames; that is the figure the decision on the threshold needed (the game's continuous stability), and the guard of D50. The emulated time at the last frame differs by 31.4 million ticks (7 frames of 4.48 million ticks) in G1, 35.8 million in G6 and 49.3 million in G2 (jit is later in all three): the jit run needs more emulated time for the same game frames, which is why the margins differ by a few frames.

The tables of the three sessions are in [the appendix](#appendix-the-report-tool-on-g1-g6-and-g2).

### 3.1 D50's numbers checked against these tables

D50 takes 4 frames (the measured largest, 3, plus one) and a margin guard of 60 frames (the measured smallest, 91, with room). The report tool was run again, with its new defaults (`--tolerance 4`, `--min-margin 60`), on the saved runs of the first round (the recordings of `build/golden/<S>` against `build/cross-model/rec/<S>/check/run1`, no emulator run):

| Session | Exit status | Real scene changes (reference / candidate) | Largest shift | Smallest margin, reference | Smallest margin, candidate | Input events below 60 |
|---|---|---|---|---|---|---|
| G1 | 0 | 2 / 2 | 2 | 264 | 262 | 0 of 8 |
| G6 | 0 | 3 / 3 | 2 | 148 | 147 | 0 of 23 |
| G2 | 0 | 8 / 8 | **3** | 92 | **91** | 0 of 52 |

The tool exits 0 on all three, so D50's numbers hold on the measured runs: the largest shift of the three sessions is 3 (one frame under the tolerance), the smallest margin 91 (31 frames over the guard), the same scene sequence on both sides. The two G2 changes that stopped the first round (5084 against 5087 and 5086 against 5089) are now "within". The shift the numbers must absorb is bounded the way the first round saw it: jit needs more emulated time for the same game frames, 31 to 49 million ticks over a whole session, a few frames, with every scripted input far from a scene change. The numbers are for these three sessions; N0-30 calibrates the quest sessions with the same tool.

## 4. SVC calls

The cross-model SVC comparison (`--cross-model config/native/svc_cross_model.txt`, N0-25) **passes on all three sessions**: every compared call (thread creations, memory and handle operations) is the same in both backends, in the same place, with the same result. Compared calls per side: 128 (G1), 144 (G6) and 164 (G2), the same on both sides. So the recompiler's behaviour matches Azahar's own on every call the list covers, and the list needed no change.

The totals are not gated and differ, as N0-25 measured on G1 to frame 800: the full SVC call counts differ in 14 of 21 threads of G1 (17 of 25 in G2 and G6), by thread and call as the tables below show. The differences are polling and waiting: the main thread's `GetSystemTick` (0x28) count is 846638 against 630582 in G1, its `SleepThread` (0x0A) count 897 against 762, and the threads that sleep (`0x0A`) run a few more or fewer iterations; thread `#9` sends 11907 `SendSyncRequest` calls on jit against 1727 on recomp in G1 (all of them `TriggerCmdReqQueue`, [section 5](#5-ipc-calibration)). Nothing in them is a count of something the game keeps: they are loops that wait, sleep, poll or send requests, which is what a different clock changes.

## 5. IPC calibration

The method: `compare_ipc_traces.py` reports only the first difference per thread, so each pair was also aligned thread by thread (per-parent thread names, a request being its session, header, result and descriptors) to list every difference. The commands that differ in count or place were then tied to time one by one; the entries were added to `build/cross-model/ipc_time_dependent.txt` and the check run again, until what remained could not be tied to time.

### 5.1 The 18 entries, now in `config/native/ipc_time_dependent.txt`

The orchestrator merged the 18 entries below into `config/native/ipc_time_dependent.txt` (it holds exactly these 18 lines, each with its reason); the working copy of the first round stays in `build/cross-model/ipc_time_dependent.txt`. Each of the 18 entries was needed (without it the comparison of that command fails in at least one session), and with all of them in place the main thread and every thread but the `fs:` readers of [5.2](#52-what-remained-and-is-not-an-entry) match request for request in all three sessions.

| Entry | What the pair shows (reference / candidate requests) | Why it depends on time |
|---|---|---|
| `gsp::Gpu 0x000c0000` | TriggerCmdReqQueue: thread `#9` 1727 / 11907 (G1), 3742 / 25922 (G6), 9078 / 63058 (G2); the main thread 8489 / 8491, 18484 / 18486, 44972 / 44974. In thread `#9` the candidate's list is the reference's plus more requests of the same command; in the main thread the command moves among the other requests | a queue trigger sent from a loop (the ticks and SVC counts of thread `#9` are those of a polling loop), so how many are sent in a run follows the model's clock. The reply is a result code only |
| `gsp::Gpu 0x00080082` | FlushDataCache: the main thread 3669 / 3672 (G1), 19719 / 19712 (G6), 35664 / 35468 (G2) | the main thread flushes inside its frame loop, which polls the clock (`GetSystemTick`, 846638 / 630582 calls in G1), so the number of flushes between two other requests follows the clock (the main thread's first difference, at its request 256 in all three sessions, is a flush and a queue trigger in another order). With this entry and the one above hidden, the main thread's other requests are equal |
| `dsp::DSP 0x00130082` | FlushDataCache of the audio thread (`#11`): 294 / 292 (G1), 671 / 669 (G6), 1544 / 1538 (G2); the candidate's list is a prefix of the reference's | presumably one flush per audio buffer handed to the DSP: how many buffers were handed over when the movie was skipped and when the run ended follows the DSP clock, which is shifted in jit ([section 6](#6-the-dsp-capture-of-g1)). The candidate's list is cut short in all three sessions |
| `y2r:u` 15 headers: `0x00010040 0x00030040 0x00050040 0x00070040 0x000d0040 0x00100102 0x00110102 0x00120102 0x00180102 0x001a0040 0x001c0040 0x001e0100 0x00210040 0x00220040 0x00260000` | the intro movie's conversions: 232 / 230 requests of each (233 / 231 for `0x000d0040`) in G1, G6 and G2 | one set-up per movie frame, 232 on recomp against 230 on jit. The scripts skip the movie with an `A` at frame 1350; the last conversion of both runs is right before it (recomp 0.5 million ticks, jit 0.4 million ticks before the press), the movie started 40.8 million ticks later on jit while the press came 30.8 million ticks later, so jit played the movie 9.8 million ticks (2.2 frames at 4.4 million ticks each) shorter: the count is the movie's playing time. Replies carry results only |

The entries hide those commands from both traces in every later check, the native runtime's included. They are what the pair shows and no more; the 15 `y2r:u` lines are one fact (the movie's frame count) and could be one entry if the file allowed a service-wide line (it does not).

### 5.2 What remained, and is not an entry

With the 18 entries hidden (as measured in the first round), `compare_ipc_traces.py` exits 1 on these threads (names are the comparator's `<entry>#<k>`; the entry address is the game's thread start):

| Session | Thread | Difference | Commands |
|---|---|---|---|
| G1, G6, G2 | `0010D8D4#17` | 576 reference requests, 571 candidate; the candidate's list is a **prefix** of the reference's | `fs:USER/file` 0x080200c2 (Read of 2048 bytes), probably the intro movie's stream: it stops 5 requests short on jit |
| G6, G2 | `0010D8D4#12` | same number of requests (188 in G6; 563 / 562 in G2), but **two reads of 8192 bytes come earlier** on jit: one 8 positions earlier (request 25 on recomp, 17 on jit), one 1 position earlier (29 / 28); in G2 the thread also ends one 8192-byte read short | `fs:USER/file` 0x080200c2, a reader that interleaves reads of 8192 bytes with small ones (48 and 420 bytes) |
| G2 | `0010D8D4#13` | the same 224 requests, request 192 differs in its data | `fs:USER/file` 0x08030102 (Write, 81408 bytes): the save, [section 7](#7-the-save-write-of-g2) |

The first row is consistent with the measurement of the `y2r:u` entries: the reads of that thread stop at the scripted skip (the last one 7.7 million ticks after the press on recomp and 7.8 million on jit), and jit played the movie 2.2 frames shorter, so 5 reads of 2048 bytes (10 KiB) were not made. That it is the movie's stream is an inference from those times, not something the traces name. The second row is an order difference inside one thread, with the same set of requests: it is plausibly a race between that reader and the threads that feed it, which the clock decides, but this work package did not trace the cause, and neither its spec nor DECISIONS.md has a rule for it. The third row is not tied to time. **None of the three is an entry**: the trap of N0-28 forbids an `fs:` command in `ipc_time_dependent.txt`, and a line there would hide every `fs:USER/file` Read of both traces anyway (the entry format has no thread or file, only the session and the header).

**Resolved by D52** (the first round listed three options: a prefix rule, an order-insensitive comparison, or totals only). D52 took the second: in the cross profile, per thread, the `fs:USER/file` Reads are compared as a multiset of (offset, size, CRC of the data read), order ignored, the smaller multiset contained in the larger with a surplus of at most 16 reads (the first two rows: 5 reads short, and two reads in another order); the Writes are compared in full (the third row, [section 7](#7-the-save-write-of-g2)); and no `fs:` command is an entry. N0-32 builds the Read rule (`compare_ipc_traces.py --fs-reads-multiset [--max-surplus 16]`). The first row, the movie, would also disappear if the sessions' scripts were keyed to the game and not to frame 1350, but they are frame-keyed by design (`docs/native/golden.md`).

## 6. The DSP capture of G1

`golden.py check G1` fails `dsp-1240` (`capfmt.py diff-dsp`: "the reference starts 15 DSP frames earlier; they are skipped", then every source status of the overlap differs): the capture starts at game frame 1240, and at that frame the DSP frame counter is 4434 on recomp and 4449 on jit.

Analysis of the two captures (300 DSP frames each, two regions, one source enabled per region):

- the audio stream is the same: the sequence of buffer ids of source 0 in each region changes in the same pattern in both captures (the same intervals of 2 and 4 DSP frames between buffer changes);
- the same buffer id comes at a DSP frame that is later on jit by a **constant**: 22 DSP frames in region 0 for 87 of 87 buffers, 22 in region 1 for 85 of 86 buffers (21 for one);
- after shifting the candidate by 22 DSP frames, all 14064 source-status blocks that overlap are **equal** (enabled, dirty, sync count, buffer position, current and last buffer id): 0 differ.

So the two backends play the same sound; it begins 22 DSP frames (about 107 ms, 28.8 million ticks) later on jit, and `diff-dsp` keys on the DSP frame counter, which is the model's own time. The emulated time at game frame 1240 is 31.4 million ticks later on jit (24 DSP frames), the same order. This is tied to time and no entry or option of this work package covers a DSP capture: `capfmt.py` belongs to N0-06. **Resolved by D51** (the first round listed two options: align the candidate on the first buffer change, or leave `dsp` information-only): D51 took the first. In the cross profile the capture is compared after aligning the candidate on the first change of the first enabled source's buffer id, the shift a constant number of DSP frames (every later buffer change at that shift within 1) and at most 48 (jit against recomp measured 22), and after it the source status blocks must all be equal; the printed shift is part of the result and the PCM samples are not compared across models. N0-32 builds it (`capfmt.py diff-dsp --align [--max-shift 48]`).

## 7. The save write of G2

At game frame 4917 (reference) and 4919 (candidate), thread `#13` writes the new game's save with one `Write` (`fs:USER/file`, 81408 bytes at offset 0). The two requests carry the same header, the same size and the same offset; of the 81408 bytes **24 differ, in two ranges: offsets 0 to 7 and 280 to 295**. The write's buffer has CRC-32 `0x4bc7a915` on recomp and `0x2cf8030a` on jit. The 24 bytes are not a copy of the clock: none of the six 32-bit words at those offsets equals the first result register of any `GetSystemTick` call of its own run's SVC trace, and the two runs' values differ by numbers that are not small. A hash, a random value seeded from the clock or a save identifier are all possible; which one it is needs the game's save code, which this work package does not read. Each backend repeats its own value (N0-29's gates, the recordings' user folders), so the cause is either a value derived from timing or a backend difference. Never an entry: it is save data.

What this means for later work: a native replay of G2 compared in the cross profile will show the same 24 bytes whenever its clock differs from the reference's, so someone has to find out what they are before the `fs:` comparison of G2 is calibrated.

**Resolved by D52, in two parts.** The rule: a Write is compared in full, including the CRC of the written bytes, except the byte ranges that are proved to be derived from time, which go into `config/native/fs_write_masks.txt` with their reason; a difference outside a masked range is a failure, and a difference that is not derived from time is a backend bug, not a mask. **N0-31** takes the investigation: what the 24 bytes (offsets 0 to 7 and 280 to 295 of the 81,408-byte Write) are, shown on dumps of both backends' buffers, and the ranges to put in the mask file (or the statement that it is a bug). **N0-32** takes the comparator: `compare_ipc_traces.py --fs-write-mask FILE` zeroes the masked ranges before it compares the CRC of the written bytes, and the cross profile uses it. Until N0-31 is merged the 24 bytes stay unexplained; nothing in this document claims more than the measurement above.

## 8. Conclusion for D50, and what is handed on

1. **D50 (4 frames and a margin of 60).** Measured on 13 real scene changes in three sessions: 0 to 2 frames apart for 11 and 3 frames for the two at the end of G2's village; every scripted input at least 91 frames after the last real scene change on both backends; the same scene sequence on both sides; the shift is jit running later in emulated time (31 to 49 million ticks over the whole session), not scenes in another order. With D50's defaults the report tool exits 0 on G1, G2 and G6 ([section 3.1](#31-d50s-numbers-checked-against-these-tables)): the largest shift is 3 (the tolerance is 4) and the smallest margin 91 (the guard is 60). D33's 2 frames stopped the first round; they are replaced for the cross profile by D50, and the `same` profile stays at 0. The threshold is not widened again without a new decision.
2. **DSP in the cross profile (D51)**: a constant shift of the DSP counter with equal content, aligned and bounded at 48 DSP frames. Built by N0-32.
3. **`fs:` across models (D52)**: Reads as a per-thread multiset (surplus at most 16), Writes in full with only the proved time-derived ranges masked. N0-31 finds what the 24 bytes of G2's save are and proposes the mask; N0-32 builds the two rules.
4. **Entries**: the 18 of [5.1](#51-the-18-entries-now-in-confignativeipc_time_dependenttxt) are in `config/native/ipc_time_dependent.txt`. They do not make a `check` pass alone; N0-32's *Done when* is the `exit 0` of the cross profile on the three sessions with them, the rules of 2 and 3, and the mask file.

What this calibration confirms: the SVC cross-model list holds on 128 to 164 compared calls per session ([section 4](#4-svc-calls)); the state comparison and the checkpoints hold with D50's numbers; HID matches in all three; the time-dependent IPC commands are few (the GPU queue, the cache flush, the movie, the audio flush) and the ones that remained were all `fs:` reads and a write, which D52 gives rules and not entries.

## Appendix: the report tool on G1, G6 and G2

The tool's output on the saved runs of the first round, with its defaults of D50 (tolerance 4, minimum margin 60); the SVC and IPC counts are those of the first round and do not depend on the two limits.

### G1: jit (candidate) against recomp (reference)

#### Real scene changes (2 on the reference, 2 on the candidate; tolerance 4 frames)

| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |
|---|---|---|---|---|---|---|
| 1 | 0x00e6c100 | 0x00e6c100 | 2 | 2 | +0 | yes |
| 2 | 0x00e64d5c | 0x00e64d5c | 36 | 38 | +2 | yes |

The largest difference is 2 frames.

#### Input events (8; minimum margin 60 frames)

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
- every input event is at least 60 frames after the last real scene change on both sides

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

#### Real scene changes (3 on the reference, 3 on the candidate; tolerance 4 frames)

| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |
|---|---|---|---|---|---|---|
| 1 | 0x00e6c100 | 0x00e6c100 | 2 | 2 | +0 | yes |
| 2 | 0x00e64d5c | 0x00e64d5c | 36 | 38 | +2 | yes |
| 3 | 0x00e6c138 | 0x00e6c138 | 1652 | 1653 | +1 | yes |

The largest difference is 2 frames.

#### Input events (23; minimum margin 60 frames)

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
- every input event is at least 60 frames after the last real scene change on both sides

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

#### Real scene changes (8 on the reference, 8 on the candidate; tolerance 4 frames)

| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |
|---|---|---|---|---|---|---|
| 1 | 0x00e6c100 | 0x00e6c100 | 2 | 2 | +0 | yes |
| 2 | 0x00e64d5c | 0x00e64d5c | 36 | 38 | +2 | yes |
| 3 | 0x00e6c138 | 0x00e6c138 | 1652 | 1653 | +1 | yes |
| 4 | 0x00e64d5c | 0x00e64d5c | 4955 | 4956 | +1 | yes |
| 5 | 0x00e6c100 | 0x00e6c100 | 4957 | 4958 | +1 | yes |
| 6 | 0x00e6c170 | 0x00e6c170 | 4958 | 4959 | +1 | yes |
| 7 | 0x00e6c100 | 0x00e6c100 | 5084 | 5087 | +3 | yes |
| 8 | 0x00e655c8 | 0x00e655c8 | 5086 | 5089 | +3 | yes |

The largest difference is 3 frames.

#### Input events (52; minimum margin 60 frames)

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
- every input event is at least 60 frames after the last real scene change on both sides

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
