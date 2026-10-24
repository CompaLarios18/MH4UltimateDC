# Cross-model calibration: jit against recomp

Written by N0-28 (D31, D33, D35), and brought up to date in a fix round after the orchestrator settled the first round's findings in D50, D51 and D52 (2026-10-21). The tool is `tools/native/cross_model_report.py`; the runs are `golden.py check <S> --candidate-args "--cpu jit" --profile cross` on G1, G2 and G6. This document keeps frames, counts, service names and request headers only: the traces, screenshots and user folders it was made from are game data and stay in `build/`. Section 11 (N2b-17, 2026-10-24) calibrates the screenshots: which frames two timing models show alike.

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

## 9. The quest sessions G3 and G4 (N0-30)

Written by N0-30 (2026-10-22). **Status: `needs-decision`.** The scene and input rules of D50 hold on both sessions, the SVC and HID comparisons pass, and two IPC commands that carry clock values are proposed as entries; but the cross comparison of the `fs:` traffic does not pass, and the traps of N0-28 and N0-30 forbid calibrating an `fs:` difference away. The decision is the orchestrator's ([section 9.5](#95-what-needs-a-decision)).

### 9.1 What was run

`golden.py check <S> --candidate-args "--cpu jit" --profile cross --ignore build/cross-model/ipc_time_dependent-quests.txt --require state,checkpoints,ipc,svc,hid` for G3 and G4: the N0-29 reference runner with `--cpu jit` against the recordings of N0-10 (their `user-before/` already holds the save copy, so no `--save`), `config/native/ipc_time_dependent.txt` with the 18 entries of N0-28 and, on top of it, the file of 9.4. The recordings were hard-linked into `build/cross-model/rec/<S>/` as in N0-28. Host time of the jit run: about 580 s (G4) and 5,580 s (G3); a whole `check` about 19 minutes (G4) and 2 h 32 min (G3). The IPC comparison was repeated offline on the saved traces with the options of the cross profile (`--fs-reads-multiset --max-surplus 16`) to list every thread, because `golden.py` reports only the first difference per thread.

| Comparison | G4 | G3 |
|---|---|---|
| `svc` (cross-model list) | pass (115 compared calls per side) | pass (197 per side) |
| `state` | pass: 6 real scene changes | pass: 10 real scene changes |
| `state-margin` (D50: 4 frames, 60 frames) | pass: largest shift 1 frame, smallest input margin 147 (reference) and 148 (candidate) | pass: largest shift 1 frame, smallest input margin 147 and 148 |
| `checkpoints` | pass | pass |
| `hid` | pass | pass |
| `ipc` | **fail** | **fail** |
| `svc-full`, `images` | info, never gated: 54 of 92 screenshots differ | info: 491 of 592 differ |
| exit status | 1 | 1 |

### 9.2 Scene changes and input margins (D50): the rule holds

Both sessions have the scene sequence of G1, G2 and G6 and the same scene values on both sides; **jit is not later than recomp here, it is up to 1 frame earlier or later**. G4's six real changes fall at +0, +1, -1, -1, -1, -1 frames; G3's ten at +0, +1, -1, -1, -1, -1, +1, +1, -1, -1 (G4's are G3's first six). The largest shift is **1 frame**, against 3 in G2 and D50's tolerance of 4. Every scripted input comes at least **147 frames** after the last real scene change on both sides (G4: 9 events, smallest 147/148 at frame 1050; G3: 111 events, the same smallest; the three that follow the real change at frame 13464 have margins from 186 on the reference and 185 on the candidate). G3's many `A` presses in talks and menus are all at least that far from a real scene change, so the margin guard needed no loosening. The tables are in the appendix.

The sessions agree on more than D50 asks: all checkpoints and the HID captures are equal, and the SVC comparison of N0-25 compares 115 (G4) and 197 (G3) calls per side, equal.

### 9.3 IPC: what is the same, what differs

With the 18 entries of N0-28 hidden, `golden.py` stops each thread at its first difference. Aligning every thread of the two traces (G4: 10 threads, G3: 20) gives these differences, and only these:

| # | Where | G4 | G3 | Class |
|---|---|---|---|---|
| 1 | main thread, request 222 (`fs:USER` header `0x080d0144`, ControlArchive, in the save-loading start-up, before frame 3) | 1 request | 1 request | `fs:`: the CRC-32 of the one-byte *input* buffer differs (the output byte is equal on both sides: the reference's input is a non-zero byte, the candidate's is 0). Every other request of the main thread (547 of 548 in G4, 687 of 688 in G3) is equal |
| 2 | thread `#14` (`nwm::UDS`), `0x001d0044` BeginHostingNetwork and `0x00100042` SetApplicationData | 1 + 1 requests of 22 | 11 + 11 requests of 237 (every call of the two commands) | not `fs:`: clock values in the request buffer, see 9.4 |
| 3 | thread `#3` (`fs:USER/file` Reads, the asset loader), around its request 205 (the village's arrival scene, about frame 1950) | 275 against 276 Reads; 6 Reads of the reference (the side with fewer) have no equal on the candidate | the same (583 against 584) | `fs:`: two loads are made in **different pieces**: sizes 3280 and 131072 against 560 and 79617 at the same file position, and a 2185-byte piece at another file position; surplus 1, but the multiset rule asks for containment and the Reads are not contained |
| 4 | thread `#12` (`fs:USER/file` Reads, the 8192-byte streaming reader of 5.2) | 264 against 261 Reads: passes the multiset rule (surplus 3, three Reads out of place) | 2317 against 2289 Reads: **fails**, surplus 28 (at most 16); 379 Reads of the reference and 351 of the candidate have no equal on the other side. They are 8192-byte pieces in a few contiguous ranges of the same file (5 ranges on the reference, 4 on the candidate; the largest, 322 Reads and 2.6 MB, on each side) at **different file positions**, not the same ranges in another order | `fs:`: the reader streams other parts of the file on the two backends |
| 5 | thread `#17` (the movie stream of 5.2) | 580 against 571 Reads | 580 against 571 | `fs:`: 9 fewer Reads on jit at the scripted skip, inside the multiset rule (D52, surplus 9) |
| 6 | thread `#5` (`fs:USER/file` Reads) | 115 against 114 | 214 against 213 | `fs:`: one 124,610-byte Read less on jit, inside the rule (surplus 1) |

The other threads (G3 has 20) match request for request. **No difference of a new kind appears in G3's quest part (frames 13,000 to 14,100) outside thread `#12`:** the main thread's single difference and the loader `#3`'s (its requests 205 to 210 and one more at 221) are at the same positions in G4 and G3, and the UDS calls differ in every call of both sessions. The Y2R, DSP and GPU commands of the 18 entries differ in count as in N0-28 (G4: Y2R set-ups 234 against 230, GPU queue triggers 18,108 against 36,090) and are hidden by them.

Rows 1, 3 and 4 are `fs:` differences that the cross-profile rules of D52 do not absorb, and D52 says no `fs:` command is ever an entry. Rows 5 and 6 pass the rule.

### 9.4 The entries proposed (`build/cross-model/ipc_time_dependent-quests.txt`)

Two entries, both for commands that are not `fs:`, each with a measured dependence on the clock. Offsets and counts only:

| Entry | What the pair shows | Why it depends on time |
|---|---|---|
| `nwm::UDS 0x001d0044` | BeginHostingNetwork: 11 requests in G3, 1 differing in G4; the 264-byte network structure differs in the same two bytes (offsets 70 and 71) in all 12 requests, and in two more bytes (260 and 261) in 8 of the 11 requests of G3 | the 16-bit value at 70 to 71 is a different number on each call and different on the two backends (11 BeginHostingNetwork calls of G3: 11 values per side, none equal across sides), which is how the game's random generator, seeded from its first clock reading (the same mechanism N0-34 found in G2's save), behaves; the 16-bit field at 260 to 261 is a counter of emulated seconds: it rises from 0x16c0 to 0x17ad over the 11 calls on both sides, a few units apart (one unit is about 2.7e8 ticks, one second of the 3DS clock). The reply is a result code only |
| `nwm::UDS 0x00100042` | SetApplicationData: the 200-byte beacon data differs in offsets 6 and 7 (the same random value) and, in 9 of 11 requests, 196 and 197 (the second counter) | the same two fields as above, read from the same state. The reply is a result code only |

The random-value half of this is an inference from the pattern and from N0-34, not a proof: no `GetSystemTick` result of the UDS thread equals the value, and the thread's own SVC calls do not hold it (it comes from another thread's state). The counter half is plain time. Both entries leave the `nwm::UDS` thread comparing every other command in full. The orchestrator decides whether this level of evidence is enough to merge them (the alternative is to compare those two commands with the two fields masked, a rule the plan lacks).

With the two entries and the 18, the offline comparison of the cross profile still exits 1 on both sessions: G4 fails on thread `main` (row 1) and thread `#3` (row 3); G3 on `main`, `#3` and `#12` (row 4). Nothing else fails.

### 9.5 What needs a decision

1. **Row 1, the ControlArchive input byte.** One byte of an input buffer of an `fs:USER` command differs in the start-up with the save: the reference's input is non-zero and the output is the same. It looks like uninitialised guest stack that the game passes along (the buffer is on the main thread's stack), left over from the earlier, timing-different run of the code, and not like data read or written; but this work package did not find where the byte comes from, and its trap forbids treating an `fs:` difference as time-dependent. G1, G2 and G6 start without the save and never show it. Options: a rule for an `fs:` command whose input buffer is smaller than N bytes, a finding of the byte's origin (an N0-31-style investigation), or a fix if it is a backend bug.
2. **Rows 3 and 4, different pieces and different positions of the streams.** The loader `#3` reads one resource in pieces of 3280 and 131072 bytes on recomp and 560 and 79617 on jit, and the streaming reader `#12` reads other parts of its file on the two backends (351 pieces of 2.6 MB each side in G3). The first looks like the quest-loading race of `docs/research/system-services.md` (which resource is requested at a moment depends on how many frames have run), the second like a stream (music, by its pattern) that is at another position when the run ends or when a scene starts. D52's multiset needs equal pieces; N0-33's `--fs-reads-coalesce --fs-reads-prefix` (the `native` profile) merges touching pieces and might absorb row 3, but it is not part of the `cross` profile this WP was told to use, and neither option has been tried on these traces here. The orchestrator must say whether the `cross` profile is to take these options (and which surplus), whether the streams of G3 get a rule, or whether G3's and G4's cross calibration is replaced by a `native`-profile calibration.
3. **The two UDS entries (9.4)** if the evidence is enough.

D50's numbers (4 frames, 60 frames) need no change. `golden.py check G3`/`G4 ... --require state,checkpoints,ipc,svc,hid` exits 0 only after 1 and 2 are settled.

## 10. The quest sessions G4 and G8 on the recordings at the fractional date (N0-30, second pass)

Written by N0-30 (2026-10-23). **Status: G4 passes, G8 does not: `needs-decision`.** G3 is not part of this pass (D90) and G7 was dropped (D91). The reference runner of N0-35 with `--cpu jit` was checked against the recordings of N0-35 (`# init-time: 1767268800.5`, recomp, `user-before/` already holding the save copy), once per session: `golden.py check <S> --candidate-args "--cpu jit" --profile cross --ignore config/native/ipc_time_dependent.txt --require state,checkpoints,ipc,svc,hid`. The recordings were copied to `build/cross-model/rec2/<S>/` so that the `check/` folders of `build/golden` stay as they are. The existing ignore file holds the 18 entries of N0-28, the three of D82 and the two of D86; no new entry was added for G4.

| Comparison | G4 | G8 |
|---|---|---|
| `svc` (cross-model list) | pass (115 compared calls per side) | pass (115 per side) |
| `state` | pass: 6 real scene changes | pass: 6 real scene changes |
| `state-margin` (D50: 4 frames, 60 frames) | pass: largest shift 1 frame, smallest input margin 147 (reference) and 148 (candidate) | pass: largest shift 1 frame, smallest input margin 147 and 147 |
| `checkpoints` | pass | pass |
| `hid` | pass | pass |
| `ipc` | **pass** with the existing ignore file | **fail** (two differences, 10.3) |
| `svc-full`, `images` | info, never gated: 50 of 92 screenshots differ | info: 55 of 102 differ |
| exit status | **0** | 1 |

### 10.1 Scene changes and input margins (D50): the rule holds

Real scene changes (frames on the reference and the candidate, difference candidate minus reference):

| # | G4 reference | G4 candidate | G4 diff | G8 reference | G8 candidate | G8 diff |
|---|---|---|---|---|---|---|
| 1 | 2 | 2 | +0 | 2 | 2 | +0 |
| 2 | 42 | 43 | +1 | 42 | 43 | +1 |
| 3 | 903 | 902 | -1 | 1053 | 1053 | +0 |
| 4 | 1576 | 1575 | -1 | 1726 | 1725 | -1 |
| 5 | 1578 | 1577 | -1 | 1728 | 1727 | -1 |
| 6 | 1579 | 1578 | -1 | 1729 | 1728 | -1 |

The scene values are the same on both sides in every row. The largest shift is **1 frame** in both sessions (D50 allows 4). Both sessions have 9 scripted inputs (frames 300, 450, 600, 750, 900, 1050, 1200, 1350, 1500). G4: the last real change before the inputs at 1050 to 1500 is at frame 903 (reference) and 902 (candidate), margins 147 to 597 and 148 to 598. G8: the last real change before 1200 to 1500 is at 1053 on both sides, margins 147 to 447 on both. Every input is at least **147 frames** after the last real scene change on both sides (D50's minimum is 60). D50's numbers need no change.

### 10.2 fs coverage (D83, D85)

| | G4 | G8 |
|---|---|---|
| `fs:USER/file` Reads, reference / candidate | 1277 / 1267 | 1294 / 1289 |
| largest difference of one thread's Read count | 9 on the movie stream's thread (at most 16) | 4 on thread `#18` |
| bytes only the reference covers | 26,624 in 2 ranges | 274,251 in 5 ranges |
| bytes only the candidate covers | 0 | 84,658 in 3 ranges |
| most one-sided bytes of one thread and file session | 18,432 (reference, the movie stream's thread; the bound of D85 is 20,480) | reference 133,257 and candidate 84,658, both on the loader thread `#3`; thread `#5`: reference 124,610 |
| result | **pass**: the loader threads match exactly, nothing but the movie stream differs | **fail**: loader threads `#3` and `#5` differ |

G4 is the calibrated kind that D83 expected: the first pass's differences in the main thread's ControlArchive byte, the loader `#3` and the streaming reader `#12` are gone, and the one-sided bytes are the 18,432 of the movie stream's skip. G8 does not reach it.

### 10.3 G8: what differs in the IPC

With the 23 entries of the ignore file, the offline comparison of the two traces (cross profile options, every thread listed) shows these differences and no other:

| # | Where | G8 | Class |
|---|---|---|---|
| 1 | the `ir:USER` poll thread (`#17` in `golden.py`'s naming), commands `0x000d0042` and `0x00190040` | the thread alternates the two commands; both runs agree request for request up to request 13,448 (header, result, descriptor); the reference stops there and the candidate makes 6 more (3 of each). Totals over the session: 6725 and 6724 on recomp, 6728 and 6727 on jit; every other `ir:USER` command (`Initialize`, the event and shared-memory set-up, 2 or 1 requests each) is equal | not `fs:`: a poll whose count at the end of the run follows the clock, the same kind as D86's `cfg:u 0x00010082` (97 to 99 requests). Proposed as two entries below |
| 2 | loader thread `#3` (`fs:USER/file` Reads), around request 205 | 275 against 276 Reads (surplus 1); 5 Reads of the reference have no equal on the candidate (offset 1,019,089,312 sizes 12 and 131,072; offset 1,019,089,324 size 3,280; offset 811,370,608 sizes 12 and 2,185); reference-only 133,257 bytes, candidate-only 84,658 | `fs:`: the same two resources as the first pass's row 3, loaded in other pieces (3,280 and 131,072 against the candidate's pieces at the same positions). With `--fs-reads-coalesce --fs-reads-prefix` (the native profile's options, tried offline only) the multiset still differs on 4 runs (131,072 and 3,292 at offset 1,019,089,312; 2,185 and 92 at 811,370,608), and the one-sided bytes do not change |
| 3 | thread `#5` (`fs:USER/file` Reads) | 115 against 114 Reads; one 124,610-byte Read of the reference has no equal on the candidate | `fs:`: as the first pass's row 6, but there it was inside the rule (surplus 1) and here the one-sided bytes (124,610 against 20,480) fail N0-37's coverage bound |

The main thread matches request for request (528 against 528), as do the other threads; thread `#12` (285 against 284 Reads) and thread `#18` (575 against 571) pass. G8 is the first session with `ir:USER` open: **the jit and recomp runs agree on every `ir:USER` request that both make** (the sequence of commands, results and descriptors, including the 6725 polls), and the only difference is the 3 extra poll pairs at the tail.

### 10.4 Entries proposed (`build/cross-model/ipc_time_dependent-quests2.txt`)

Two entries, for commands that are not `fs:`:

| Entry | What the pair shows | Why it depends on time |
|---|---|---|
| `ir:USER 0x000d0042` | 6725 requests on recomp, 6728 on jit, one thread; all requests before the tail equal | the game's poll loop on the `ir:USER` shared block runs once per period of emulated time and the number of passes before the scripted end of the run follows the clock; the reply is a result code only |
| `ir:USER 0x00190040` | 6724 and 6727 requests, the command that alternates with the one above | the same loop, the same cause; the reply is a result code only |

With these two entries added, the offline comparison of G8 still exits 1: only threads `#3` and `#5` fail, both on `fs:` Reads (10.3 rows 2 and 3). G4 needs no entry.

### 10.5 Conclusion, and what needs a decision

- **D50 holds** on both sessions: largest shift 1 frame, smallest margin 147 frames; no change to the tolerance or the minimum margin.
- **G4 meets Done-when** (`golden.py check G4 ... --require state,checkpoints,ipc,svc,hid` exits 0 with `config/native/ipc_time_dependent.txt` as it is). D83's remedy worked on it: the loader and streaming differences of the first pass are gone, and the largest one-sided figure, 18,432 bytes, is the movie stream's, under D85's bound of 20,480.
- **D52 and D85 on G8:** the check does not exit 0. The two `ir:USER` entries fix row 1, but the loader thread `#3` and thread `#5` of G8 read other pieces or other resources on the two backends at the fractional date, 133,257 and 124,610 bytes one-sided against the bound of 20,480. An `fs:` difference is never an entry, and the fractional date does not remove it here. The decision is the orchestrator's: whether G8 is judged with the native-profile options (offline they do not absorb it), whether the cause on G8 (the staged save with the Circle Pad Pro option on, a different start-up from G4's) needs an N0-36-style investigation, or whether G8's cross calibration is dropped as G3's was (D90) and G8 is judged by the native mode (N1-25, N1-26) and D86's rule.
- Neither session needed a change to `config/**` beyond the two `ir:USER` entries above, and the orchestrator decides whether to merge them.

## 11. The screenshots across timing models (N2b-17)

Written by N2b-17 (2026-10-24). **Status: decided (D58, settled by the orchestrator).** The gate is built and calibrated on G1, G2, G6 and G4: the `cross` profile holds a candidate's screenshots to the frames at which jit and recomp already agree with a margin (PSNR at least 42 dB and no 16x16 block above 7.0/255 on both screens), 81 frames. Two long scenes (G4 903 to 1575, G2 5086 to 9000) have no such frame and are not judged by the jit-calibrated gate. No threshold of ROADMAP section 6 was changed. [Section 11.6](#116-the-decision-d58) has the rule.

### 11.1 What was run

`golden.py check <S> --candidate-args "--cpu jit" --profile cross --ignore config/native/ipc_time_dependent.txt` for G1, G6, G4 and G2, one at a time under the emulator lock, with the reference runner of the fractional-date work (N0-35) and its recomp library, on hard links of the recordings (`build/cross-model/images/rec/<S>/`, so that `check/` did not land in `build/golden/`). The recordings of G1, G2 and G6 are N0-09's (the N0-29 runner, no start date), G4's is N0-35's (`# init-time: 1767268800.5`). Host time of a whole `check`: 118 s (G1), 514 s (G6), 683 s (G4) and 1969 s (G2). Then `python tools/native/cross_model_report.py --images --session tools/native/golden/<S>.txt --reference <recording> --candidate <recording>/check/run1 --proposal build/cross-model/image_frames.txt` (with `--not-judged 903-1575` for G4 and `--not-judged 5086-9000` for G2, see 11.6), which compares the top and bottom screenshot of every frame (every 60 frames from frame 300) with `imgdiff.py`'s rule and writes the frames where both screens pass with the margin of D58 (`--margin-db 2.0` and `--margin-block 1.0` are the defaults; `0` and `0` give the plain rule). The count of screenshot files that differ is N0-28's (G1: 19 of 48 files), so the n035 runner's jit run shows the same pictures as N0-28's n029 run. The tables in `build/cross-model/images/images-<S>.md` are the tool's output; this section keeps frames and numbers.

The rule is ROADMAP section 6's, as `CompareImages` (`native/src/gpu_capture/image.h`): PSNR over R, G and B at least **40 dB**, and no 16x16 block with a mean error above **8/255** (blocks at the edges over the pixels they have). `tools/native/imgdiff.py` is the same arithmetic in Python and a test reads the three constants from `image.h`. The gate (`golden.py check --profile cross`) judges the listed frames with these thresholds. The calibration lists only the frames that pass with **2.0 dB and 1.0 (of 255) to spare**, that is PSNR at least **42 dB** and a worst block of at most **7.0** on both screens (both inclusive): the margin selects the calibrated frames, it does not change the thresholds.

### 11.2 The numbers

| Session | Screenshot frames | Frames proposed (margin: 42 dB, block 7.0) | Of them, both screens identical | Lowest PSNR / highest block among them | Pass the plain rule (40 dB, 8/255) | Narrow, dropped by the margin | Frames that fail the plain rule |
|---|---|---|---|---|---|---|---|
| G1 | 24 (300 to 1680) | **22** | 11 | 42.2 dB / 6.6 | 23 | 1260 | 1 (frame 1320: 39.4 dB, block 6.5, top screen) |
| G6 | 57 (300 to 3660) | **21** | 11 | 42.2 dB / 6.6 | 24 | 1260, 1620, 3360 | 33 (the worst 18.3 dB; 28 of them have a block above 16) |
| G4 | 46 (300 to 3000) | **10** | 5 | 46.0 dB / 3.3 | 10 | none | 36 (the worst 11.8 dB; 32 have a block above 16) |
| G2 | 146 (300 to 9000) | **28** | 17 | 42.2 dB / 6.6 | 31 | 1260, 1620, 3360 | 115 (the worst 14.1 dB; 73 have a block above 16) |

81 frames in all. The bottom screen (menus) is the same on both models in most frames; the top screen (the 3D scene) is what moves. The first scene of G1, G6 and G2 is the same session start and gives the same numbers in all three. The frames that passed narrowly are the ones the margin drops: 1260 in G1 (40.2 dB), and 1260, 1620 (bottom screen: 41.7 dB, block 7.2) and 3360 (top screen: 41.7 dB) in G6 and G2; none lies within 0.15 dB or 0.1 of the margin's boundary, so rounding does not decide any frame. Frame 1320 of G1, G6 and G2 fails the plain rule by 0.6 dB. The thresholds are the plan's and were not touched.

### 11.3 Per scene

A scene is the interval between two real scene changes of the recording's state trace (the same definition as section 3; the first two, frames 0 to 1 and 2 to 35 or 41, are the start-up and have no screenshot). Image frames are the screenshot frames proposed (both screens pass with the margin of 11.1); a range `a-b (n)` is n frames 60 apart.

**G1**

| # | scene | frames | length | screenshot frames | proposed | image frames |
|---|---|---|---|---|---|---|
| 3 | 0x00e64d5c | 36 to 1700 | 1665 | 24 | 22 | 300-1200 (16), 1380-1680 (6) |

**G6**

| # | scene | frames | length | screenshot frames | proposed | image frames |
|---|---|---|---|---|---|---|
| 3 | 0x00e64d5c | 36 to 1651 | 1616 | 23 | 20 | 300-1200 (16), 1380-1560 (4) |
| 4 | 0x00e6c138 | 1652 to 3700 | 2049 | 34 | 1 | 3060 |

**G4**

| # | scene | frames | length | screenshot frames | proposed | image frames |
|---|---|---|---|---|---|---|
| 3 | 0x00e64d5c | 42 to 902 | 861 | 11 | 9 | 300-480 (4), 660-900 (5) |
| 4 | 0x00e6c778 | 903 to 1575 | 673 | 11 | **0** | none (not judged by the jit-calibrated gate) |
| 7 | 0x00e6c170 | 1579 to 3000 | 1422 | 24 | 1 | 1800 |

(G4's scenes 5 and 6, frames 1576 to 1578, are the three consecutive real scene changes of section 10.1.)

**G2**

| # | scene | frames | length | screenshot frames | proposed | image frames |
|---|---|---|---|---|---|---|
| 3 | 0x00e64d5c | 36 to 1651 | 1616 | 23 | 20 | 300-1200 (16), 1380-1560 (4) |
| 4 | 0x00e6c138 | 1652 to 4954 | 3303 | 55 | 6 | 3060, 4020-4260 (5) |
| 7 | 0x00e6c170 | 4958 to 5083 | 126 | 2 | 2 | 4980-5040 (2) |
| 9 | 0x00e655c8 | 5086 to 9000 | 3915 | 66 | **0** | none (not judged by the jit-calibrated gate) |

(G2's scenes 1, 2, 5, 6 and 8 last 1 to 34 frames and have no screenshot.)

### 11.4 The two scenes without an image frame

| Scene | Top screen: PSNR median (min to max), block median | Bottom screen | Screens that pass alone |
|---|---|---|---|
| G4 #4 (frames 903 to 1575, 11 screenshots) | 29.9 dB (23.4 to 35.8), 22.2 | identical in all 11 | top 0, bottom 11 |
| G2 #9 (frames 5086 to 9000, 66 screenshots) | 28.2 dB (14.1 to identical), 12.7 | passes in 63 of 66 (lowest 24.9 dB) | top 3, bottom 63 |

In both scenes the top screen is far from passing at every frame (G4: the closest is 35.8 dB with a block of 12.9; the 3 frames of G2 at which the top screen is identical are 5100, 5160 and 5220, where the bottom screen fails). The scenes are the cutscene of G4 and the quest part of G2 (the area system left the village at frame 5084), so both are moving 3D scenes in which the same game frame is a different moment on the two clocks (section 3: jit is up to 3 frames behind, and every frame of motion is a different picture).

### 11.5 G3, G8 and G3-perf: no image frames

**G3 and G8 get no image frames from this work package.** Their jit runs are not calibration runs: D90 and D93 record that the jit run chooses other resources than recomp and never leaves the village, so the pictures are of other scenes (G3: 491 of 592 files differ in the first pass of section 9; G4's jit run does pass, D93). The image gate of G3 and G8 will come from the native deterministic mode when a renderer exists (N2b-19 to N2b-21). **G3-perf** has no image gate (rule 9): its widened top screen exists only where the port's hooks run, and they run only on the recomp backend. `config/native/image_frames.txt` therefore lists no frame for G3, G8 or G3-perf, and `images` is skipped for them (exit 3 under `--require images`).

### 11.6 The decision (D58)

1. **The rule.** The `cross` profile holds a candidate's screenshots to **the frames at which jit and recomp already agree, with a margin**: the frames at which the calibration run's top and bottom screenshots both have a PSNR of at least **42 dB** and no 16x16 block above **7.0/255** (2.0 dB and 1.0 over ROADMAP section 6's 40 dB and 8/255; both bounds inclusive). They are listed per session in `config/native/image_frames.txt` (81 frames: G1 22, G6 21, G4 10, G2 28; the orchestrator writes the file from `build/cross-model/image_frames.txt`, which `cross_model_report.py --images` produced with its defaults). At those frames the candidate's pictures are compared with the recording's with ROADMAP section 6's thresholds (`golden.py check`, comparison `images`); the margin only selects the calibrated frames. A frame in the candidate's `shots/gas.txt` is skipped; every other screenshot is information; the `same` profile keeps SHA-256 and the `native` profile keeps images as information (`--image-frames` outside the cross profile is a usage error). A session with no calibration (G3, G8, G3-perf, section 11.5) has no frames and `images` is skipped. The list is made again with `cross_model_report.py --images` whenever a session is recorded again (a new recording moves the frames).
2. **The coverage rule is "at least 3 image frames and one in every scene longer than 300 frames", amended for two scenes.** G4 scene 4 (frames 903 to 1575) and G2 scene 9 (frames 5086 to 9000) are not judged by the jit-calibrated gate: the top screens never agree there between jit and recomp (11.4). `cross_model_report.py --images` is told so with `--not-judged 903-1575` (G4) and `--not-judged 5086-9000` (G2); with that option its exit status is 0 and the output names the scene as not judged, and any other shortfall still exits 1. Without it those two scenes exit 1.
3. **The plan for those two scenes (not built here).** The native deterministic mode shares the recording's clock, so it will judge every screenshot of the recording (minus the gas frames) by the same rule when the renderer exists (N2b-19 to N2b-21).
4. **Not adopted.** Gating each screen on its own (listing `<session> <frame> top|bottom`, option (b) of the first proposal).

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


## Appendix: the report tool on the quest sessions G4 and G3 (N0-30)

The tool's output on the runs of section 9, with the defaults of D50 (tolerance 4, minimum margin 60); both exit 0.

### G4: jit (candidate) against recomp (reference)

#### Real scene changes (6 on the reference, 6 on the candidate; tolerance 4 frames)

| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |
|---|---|---|---|---|---|---|
| 1 | 0x00e6c100 | 0x00e6c100 | 2 | 2 | +0 | yes |
| 2 | 0x00e64d5c | 0x00e64d5c | 42 | 43 | +1 | yes |
| 3 | 0x00e6c778 | 0x00e6c778 | 903 | 902 | -1 | yes |
| 4 | 0x00e64d5c | 0x00e64d5c | 1576 | 1575 | -1 | yes |
| 5 | 0x00e6c100 | 0x00e6c100 | 1578 | 1577 | -1 | yes |
| 6 | 0x00e6c170 | 0x00e6c170 | 1579 | 1578 | -1 | yes |

The largest difference is 1 frame.

#### Input events (9; minimum margin 60 frames)

| frame | input | last scene change (reference) | last scene change (candidate) | margin (reference) | margin (candidate) | margin difference |
|---|---|---|---|---|---|---|
| 300 | A | 42 | 43 | 258 | 257 | -1 |
| 450 | A | 42 | 43 | 408 | 407 | -1 |
| 600 | A | 42 | 43 | 558 | 557 | -1 |
| 750 | A | 42 | 43 | 708 | 707 | -1 |
| 900 | A | 42 | 43 | 858 | 857 | -1 |
| 1050 | A | 903 | 902 | 147 | 148 | +1 |
| 1200 | A | 903 | 902 | 297 | 298 | +1 |
| 1350 | A | 903 | 902 | 447 | 448 | +1 |
| 1500 | A | 903 | 902 | 597 | 598 | +1 |

- smallest margin on the reference: 147 frames (the event at frame 1050)
- smallest margin on the candidate: 148 frames (the event at frame 1050)
- every input event is at least 60 frames after the last real scene change on both sides

#### SVC calls per thread (cross-model names)

| thread | calls (reference) | calls (candidate) | difference | the SVC numbers that differ most |
|---|---|---|---|---|
| main | 2369799 | 2624196 | +254397 | 0x28 GetSystemTick 1141657/1374736, 0x37 1185687/1207168, 0x22 ArbitrateAddress 11877/11280 |
| main/0010D8D4#0 | 1 | 1 | +0 | - |
| main/0010D8D4#1 | 9 | 9 | +0 | - |
| main/0010D8D4#10 | 3 | 3 | +0 | - |
| main/0010D8D4#11 | 44678 | 44641 | -37 | 0x28 GetSystemTick 11291/11280, 0x32 SendSyncRequest 851/841, 0x22 ArbitrateAddress 45/39 |
| main/0010D8D4#12 | 1317 | 1313 | -4 | 0x32 SendSyncRequest 264/261, 0x0A SleepThread 1043/1042 |
| main/0010D8D4#13 | 263 | 263 | +0 | - |
| main/0010D8D4#14 | 15741 | 12774 | -2967 | 0x28 GetSystemTick 13371/10751, 0x0A SleepThread 2300/1954, 0x22 ArbitrateAddress 7/6 |
| main/0010D8D4#14/0010D8D4#0 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#1 | 3028 | 2431 | -597 | 0x24 WaitSynchronization1 3026/2429 |
| main/0010D8D4#14/0010D8D4#2 | 3028 | 2431 | -597 | 0x24 WaitSynchronization1 3026/2429 |
| main/0010D8D4#15 | 3136 | 3127 | -9 | 0x0A SleepThread 3132/3123 |
| main/0010D8D4#16 | 3133 | 3124 | -9 | 0x0A SleepThread 3132/3123 |
| main/0010D8D4#17 | 1726 | 1699 | -27 | 0x22 ArbitrateAddress 1145/1127, 0x32 SendSyncRequest 580/571 |
| main/0010D8D4#18 | 2515 | 2469 | -46 | 0x0A SleepThread 1276/1226, 0x22 ArbitrateAddress 1238/1242 |
| main/0010D8D4#19 | 114 | 141 | +27 | 0x22 ArbitrateAddress 113/140 |
| main/0010D8D4#2 | 530 | 531 | +1 | 0x0A SleepThread 528/529 |
| main/0010D8D4#20 | 3 | 3 | +0 | - |
| main/0010D8D4#21 | 981 | 793 | -188 | 0x0A SleepThread 978/790 |
| main/0010D8D4#22 | 964 | 775 | -189 | 0x0A SleepThread 959/771, 0x22 ArbitrateAddress 4/3 |
| main/0010D8D4#3 | 10819 | 10756 | -63 | 0x37 9840/9790, 0x0A SleepThread 378/360, 0x22 ArbitrateAddress 326/330 |
| main/0010D8D4#4 | 1584 | 1669 | +85 | 0x0C 555/587, 0x0A SleepThread 479/505, 0x0B 479/505 |
| main/0010D8D4#5 | 233 | 231 | -2 | 0x22 ArbitrateAddress 118/117, 0x32 SendSyncRequest 115/114 |
| main/0010D8D4#6 | 1 | 1 | +0 | - |
| main/0010D8D4#7 | 1 | 1 | +0 | - |
| main/0010D8D4#8 | 15941 | 16274 | +333 | 0x37 6626/6848, 0x0A SleepThread 3313/3424 |
| main/0010D8D4#9 | 27115 | 44470 | +17355 | 0x32 SendSyncRequest 3165/21143, 0x22 ArbitrateAddress 5635/5042, 0x19 ClearEvent 9157/9142 |
| **all 27 threads** | 2506672 | 2774135 | +267463 | 19 of 27 threads differ |

#### IPC requests per command

33855 requests on the reference, 51789 on the candidate, 97 distinct commands; 19 of them with a different count on at least one thread.

| service | header | requests (reference) | requests (candidate) | difference | threads that differ (reference/candidate) |
|---|---|---|---|---|---|
| dsp::DSP | 0x00130082 | 851 | 841 | -10 | main/0010D8D4#11 851/841 |
| fs:USER/file | 0x080200c2 | 1278 | 1266 | -12 | main/0010D8D4#12 264/261, main/0010D8D4#17 580/571, main/0010D8D4#3 275/276, main/0010D8D4#5 115/114 |
| gsp::Gpu | 0x00080082 | 9436 | 9470 | +34 | main 9436/9470 |
| gsp::Gpu | 0x000c0000 | 18108 | 36090 | +17982 | main 14943/14947, main/0010D8D4#9 3165/21143 |
| y2r:u | 0x00010040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00030040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00050040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00070040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x000d0040 | 235 | 231 | -4 | main 235/231 |
| y2r:u | 0x00100102 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00110102 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00120102 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00180102 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x001a0040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x001c0040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x001e0100 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00210040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00220040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00260000 | 234 | 230 | -4 | main 234/230 |

### G3: jit (candidate) against recomp (reference)

#### Real scene changes (10 on the reference, 10 on the candidate; tolerance 4 frames)

| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |
|---|---|---|---|---|---|---|
| 1 | 0x00e6c100 | 0x00e6c100 | 2 | 2 | +0 | yes |
| 2 | 0x00e64d5c | 0x00e64d5c | 42 | 43 | +1 | yes |
| 3 | 0x00e6c778 | 0x00e6c778 | 903 | 902 | -1 | yes |
| 4 | 0x00e64d5c | 0x00e64d5c | 1576 | 1575 | -1 | yes |
| 5 | 0x00e6c100 | 0x00e6c100 | 1578 | 1577 | -1 | yes |
| 6 | 0x00e6c170 | 0x00e6c170 | 1579 | 1578 | -1 | yes |
| 7 | 0x00e6c100 | 0x00e6c100 | 13462 | 13463 | +1 | yes |
| 8 | 0x00e650bc | 0x00e650bc | 13464 | 13465 | +1 | yes |
| 9 | 0x00e6c100 | 0x00e6c100 | 14107 | 14106 | -1 | yes |
| 10 | 0x00e655c8 | 0x00e655c8 | 14109 | 14108 | -1 | yes |

The largest difference is 1 frame.

#### Input events (111; minimum margin 60 frames)

| frame | input | last scene change (reference) | last scene change (candidate) | margin (reference) | margin (candidate) | margin difference |
|---|---|---|---|---|---|---|
| 300 | A | 42 | 43 | 258 | 257 | -1 |
| 450 | A | 42 | 43 | 408 | 407 | -1 |
| 600 | A | 42 | 43 | 558 | 557 | -1 |
| 750 | A | 42 | 43 | 708 | 707 | -1 |
| 900 | A | 42 | 43 | 858 | 857 | -1 |
| 1050 | A | 903 | 902 | 147 | 148 | +1 |
| 1200 | A | 903 | 902 | 297 | 298 | +1 |
| 1350 | A | 903 | 902 | 447 | 448 | +1 |
| 1500 | A | 903 | 902 | 597 | 598 | +1 |
| 3300 | A | 1579 | 1578 | 1721 | 1722 | +1 |
| 3420 | A | 1579 | 1578 | 1841 | 1842 | +1 |
| 3540 | A | 1579 | 1578 | 1961 | 1962 | +1 |
| 3660 | A | 1579 | 1578 | 2081 | 2082 | +1 |
| 3780 | A | 1579 | 1578 | 2201 | 2202 | +1 |
| 3900 | A | 1579 | 1578 | 2321 | 2322 | +1 |
| 4020 | A | 1579 | 1578 | 2441 | 2442 | +1 |
| 4140 | A | 1579 | 1578 | 2561 | 2562 | +1 |
| 4260 | A | 1579 | 1578 | 2681 | 2682 | +1 |
| 4380 | A | 1579 | 1578 | 2801 | 2802 | +1 |
| 4500 | A | 1579 | 1578 | 2921 | 2922 | +1 |
| 4620 | A | 1579 | 1578 | 3041 | 3042 | +1 |
| 4740 | A | 1579 | 1578 | 3161 | 3162 | +1 |
| 4860 | A | 1579 | 1578 | 3281 | 3282 | +1 |
| 4980 | A | 1579 | 1578 | 3401 | 3402 | +1 |
| 5100 | A | 1579 | 1578 | 3521 | 3522 | +1 |
| 5220 | A | 1579 | 1578 | 3641 | 3642 | +1 |
| 5340 | A | 1579 | 1578 | 3761 | 3762 | +1 |
| 5460 | A | 1579 | 1578 | 3881 | 3882 | +1 |
| 5580 | A | 1579 | 1578 | 4001 | 4002 | +1 |
| 5700 | A | 1579 | 1578 | 4121 | 4122 | +1 |
| 5820 | A | 1579 | 1578 | 4241 | 4242 | +1 |
| 5940 | A | 1579 | 1578 | 4361 | 4362 | +1 |
| 6060 | A | 1579 | 1578 | 4481 | 4482 | +1 |
| 6180 | A | 1579 | 1578 | 4601 | 4602 | +1 |
| 6300 | A | 1579 | 1578 | 4721 | 4722 | +1 |
| 6420 | A | 1579 | 1578 | 4841 | 4842 | +1 |
| 6540 | A | 1579 | 1578 | 4961 | 4962 | +1 |
| 6660 | A | 1579 | 1578 | 5081 | 5082 | +1 |
| 6780 | A | 1579 | 1578 | 5201 | 5202 | +1 |
| 6900 | A | 1579 | 1578 | 5321 | 5322 | +1 |
| 7020 | A | 1579 | 1578 | 5441 | 5442 | +1 |
| 7140 | A | 1579 | 1578 | 5561 | 5562 | +1 |
| 7260 | A | 1579 | 1578 | 5681 | 5682 | +1 |
| 7380 | A | 1579 | 1578 | 5801 | 5802 | +1 |
| 7500 | A | 1579 | 1578 | 5921 | 5922 | +1 |
| 7620 | A | 1579 | 1578 | 6041 | 6042 | +1 |
| 7740 | A | 1579 | 1578 | 6161 | 6162 | +1 |
| 7860 | A | 1579 | 1578 | 6281 | 6282 | +1 |
| 7980 | A | 1579 | 1578 | 6401 | 6402 | +1 |
| 8100 | A | 1579 | 1578 | 6521 | 6522 | +1 |
| 8220 | A | 1579 | 1578 | 6641 | 6642 | +1 |
| 8340 | A | 1579 | 1578 | 6761 | 6762 | +1 |
| 8460 | A | 1579 | 1578 | 6881 | 6882 | +1 |
| 8694 | TOUCH | 1579 | 1578 | 7115 | 7116 | +1 |
| 8894 | TOUCH | 1579 | 1578 | 7315 | 7316 | +1 |
| 9044 | TOUCH | 1579 | 1578 | 7465 | 7466 | +1 |
| 9494 | CIRCLE | 1579 | 1578 | 7915 | 7916 | +1 |
| 9914 | CIRCLE | 1579 | 1578 | 8335 | 8336 | +1 |
| 10294 | CIRCLE | 1579 | 1578 | 8715 | 8716 | +1 |
| 10604 | CIRCLE | 1579 | 1578 | 9025 | 9026 | +1 |
| 10894 | CIRCLE | 1579 | 1578 | 9315 | 9316 | +1 |
| 11444 | A | 1579 | 1578 | 9865 | 9866 | +1 |
| 11519 | A | 1579 | 1578 | 9940 | 9941 | +1 |
| 11594 | A | 1579 | 1578 | 10015 | 10016 | +1 |
| 11669 | A | 1579 | 1578 | 10090 | 10091 | +1 |
| 11744 | A | 1579 | 1578 | 10165 | 10166 | +1 |
| 11819 | A | 1579 | 1578 | 10240 | 10241 | +1 |
| 11894 | A | 1579 | 1578 | 10315 | 10316 | +1 |
| 11969 | A | 1579 | 1578 | 10390 | 10391 | +1 |
| 12044 | A | 1579 | 1578 | 10465 | 10466 | +1 |
| 12119 | A | 1579 | 1578 | 10540 | 10541 | +1 |
| 12250 | CIRCLE | 1579 | 1578 | 10671 | 10672 | +1 |
| 12510 | CIRCLE | 1579 | 1578 | 10931 | 10932 | +1 |
| 12850 | CIRCLE | 1579 | 1578 | 11271 | 11272 | +1 |
| 13300 | A | 1579 | 1578 | 11721 | 11722 | +1 |
| 13650 | A | 13464 | 13465 | 186 | 185 | -1 |
| 13850 | LEFT | 13464 | 13465 | 386 | 385 | -1 |
| 13910 | A | 13464 | 13465 | 446 | 445 | -1 |
| 14350 | CIRCLE | 14109 | 14108 | 241 | 242 | +1 |
| 15050 | A | 14109 | 14108 | 941 | 942 | +1 |
| 15140 | A | 14109 | 14108 | 1031 | 1032 | +1 |
| 15230 | A | 14109 | 14108 | 1121 | 1122 | +1 |
| 15320 | A | 14109 | 14108 | 1211 | 1212 | +1 |
| 15410 | A | 14109 | 14108 | 1301 | 1302 | +1 |
| 15500 | A | 14109 | 14108 | 1391 | 1392 | +1 |
| 15590 | A | 14109 | 14108 | 1481 | 1482 | +1 |
| 15680 | A | 14109 | 14108 | 1571 | 1572 | +1 |
| 15770 | A | 14109 | 14108 | 1661 | 1662 | +1 |
| 15860 | A | 14109 | 14108 | 1751 | 1752 | +1 |
| 15950 | A | 14109 | 14108 | 1841 | 1842 | +1 |
| 16040 | A | 14109 | 14108 | 1931 | 1932 | +1 |
| 16130 | A | 14109 | 14108 | 2021 | 2022 | +1 |
| 16220 | A | 14109 | 14108 | 2111 | 2112 | +1 |
| 16310 | A | 14109 | 14108 | 2201 | 2202 | +1 |
| 16400 | A | 14109 | 14108 | 2291 | 2292 | +1 |
| 16490 | A | 14109 | 14108 | 2381 | 2382 | +1 |
| 16580 | A | 14109 | 14108 | 2471 | 2472 | +1 |
| 16670 | A | 14109 | 14108 | 2561 | 2562 | +1 |
| 16760 | A | 14109 | 14108 | 2651 | 2652 | +1 |
| 16850 | A | 14109 | 14108 | 2741 | 2742 | +1 |
| 16940 | A | 14109 | 14108 | 2831 | 2832 | +1 |
| 17030 | A | 14109 | 14108 | 2921 | 2922 | +1 |
| 17120 | A | 14109 | 14108 | 3011 | 3012 | +1 |
| 17210 | A | 14109 | 14108 | 3101 | 3102 | +1 |
| 17300 | A | 14109 | 14108 | 3191 | 3192 | +1 |
| 17390 | A | 14109 | 14108 | 3281 | 3282 | +1 |
| 17480 | A | 14109 | 14108 | 3371 | 3372 | +1 |
| 17570 | A | 14109 | 14108 | 3461 | 3462 | +1 |
| 17660 | A | 14109 | 14108 | 3551 | 3552 | +1 |
| 17750 | A | 14109 | 14108 | 3641 | 3642 | +1 |
| 17840 | A | 14109 | 14108 | 3731 | 3732 | +1 |

- smallest margin on the reference: 147 frames (the event at frame 1050)
- smallest margin on the candidate: 148 frames (the event at frame 1050)
- every input event is at least 60 frames after the last real scene change on both sides

#### SVC calls per thread (cross-model names)

| thread | calls (reference) | calls (candidate) | difference | the SVC numbers that differ most |
|---|---|---|---|---|
| main | 17894935 | 18244895 | +349960 | 0x28 GetSystemTick 3160504/3342117, 0x37 14501179/14670005, 0x22 ArbitrateAddress 71468/70538 |
| main/0010D8D4#0 | 1 | 1 | +0 | - |
| main/0010D8D4#1 | 9 | 9 | +0 | - |
| main/0010D8D4#10 | 3 | 3 | +0 | - |
| main/0010D8D4#11 | 256927 | 256903 | -24 | 0x32 SendSyncRequest 7616/7546, 0x18 SignalEvent 62112/62127, 0x19 ClearEvent 62112/62127 |
| main/0010D8D4#12 | 8347 | 8320 | -27 | 0x32 SendSyncRequest 2317/2289, 0x22 ArbitrateAddress 6/7 |
| main/0010D8D4#13 | 263 | 263 | +0 | - |
| main/0010D8D4#14 | 160898 | 160776 | -122 | 0x28 GetSystemTick 138728/138625, 0x0A SleepThread 21452/21437, 0x22 ArbitrateAddress 52/48 |
| main/0010D8D4#14/0010D8D4#0 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#1 | 3028 | 2431 | -597 | 0x24 WaitSynchronization1 3026/2429 |
| main/0010D8D4#14/0010D8D4#10 | 3228 | 3428 | +200 | 0x24 WaitSynchronization1 3226/3426 |
| main/0010D8D4#14/0010D8D4#11 | 3228 | 3428 | +200 | 0x24 WaitSynchronization1 3226/3426 |
| main/0010D8D4#14/0010D8D4#12 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#13 | 2830 | 3628 | +798 | 0x24 WaitSynchronization1 2828/3625, 0x22 ArbitrateAddress 0/1 |
| main/0010D8D4#14/0010D8D4#14 | 2830 | 3627 | +797 | 0x24 WaitSynchronization1 2828/3625 |
| main/0010D8D4#14/0010D8D4#15 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#16 | 3428 | 3029 | -399 | 0x24 WaitSynchronization1 3426/3027 |
| main/0010D8D4#14/0010D8D4#17 | 3428 | 3029 | -399 | 0x24 WaitSynchronization1 3426/3027 |
| main/0010D8D4#14/0010D8D4#18 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#19 | 2829 | 2430 | -399 | 0x24 WaitSynchronization1 2827/2428 |
| main/0010D8D4#14/0010D8D4#2 | 3028 | 2431 | -597 | 0x24 WaitSynchronization1 3026/2429 |
| main/0010D8D4#14/0010D8D4#20 | 2829 | 2430 | -399 | 0x24 WaitSynchronization1 2827/2428 |
| main/0010D8D4#14/0010D8D4#21 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#22 | 2011 | 2397 | +386 | 0x24 WaitSynchronization1 2009/2395 |
| main/0010D8D4#14/0010D8D4#23 | 2002 | 2388 | +386 | 0x24 WaitSynchronization1 2000/2386 |
| main/0010D8D4#14/0010D8D4#24 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#25 | 3497 | 2898 | -599 | 0x24 WaitSynchronization1 3495/2896 |
| main/0010D8D4#14/0010D8D4#26 | 3492 | 2891 | -601 | 0x24 WaitSynchronization1 3490/2889 |
| main/0010D8D4#14/0010D8D4#27 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#28 | 3428 | 2430 | -998 | 0x24 WaitSynchronization1 3426/2428 |
| main/0010D8D4#14/0010D8D4#29 | 3428 | 2430 | -998 | 0x24 WaitSynchronization1 3426/2428 |
| main/0010D8D4#14/0010D8D4#3 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#30 | 5 | 5 | +0 | - |
| main/0010D8D4#14/0010D8D4#31 | 1087 | 2666 | +1579 | 0x24 WaitSynchronization1 1086/2665 |
| main/0010D8D4#14/0010D8D4#32 | 1087 | 2666 | +1579 | 0x24 WaitSynchronization1 1086/2665 |
| main/0010D8D4#14/0010D8D4#4 | 3625 | 2430 | -1195 | 0x24 WaitSynchronization1 3623/2428 |
| main/0010D8D4#14/0010D8D4#5 | 3625 | 2430 | -1195 | 0x24 WaitSynchronization1 3623/2428 |
| main/0010D8D4#14/0010D8D4#6 | 9 | 9 | +0 | - |
| main/0010D8D4#14/0010D8D4#7 | 2429 | 3625 | +1196 | 0x24 WaitSynchronization1 2427/3623 |
| main/0010D8D4#14/0010D8D4#8 | 2429 | 3625 | +1196 | 0x24 WaitSynchronization1 2427/3623 |
| main/0010D8D4#14/0010D8D4#9 | 9 | 9 | +0 | - |
| main/0010D8D4#15 | 18175 | 18163 | -12 | 0x0A SleepThread 18171/18159 |
| main/0010D8D4#16 | 18172 | 18160 | -12 | 0x0A SleepThread 18171/18159 |
| main/0010D8D4#17 | 1726 | 1699 | -27 | 0x22 ArbitrateAddress 1145/1127, 0x32 SendSyncRequest 580/571 |
| main/0010D8D4#18 | 2515 | 2469 | -46 | 0x0A SleepThread 1276/1226, 0x22 ArbitrateAddress 1238/1242 |
| main/0010D8D4#19 | 114 | 141 | +27 | 0x22 ArbitrateAddress 113/140 |
| main/0010D8D4#2 | 3042 | 3043 | +1 | 0x0A SleepThread 3040/3041 |
| main/0010D8D4#20 | 3 | 3 | +0 | - |
| main/0010D8D4#21 | 981 | 793 | -188 | 0x0A SleepThread 978/790 |
| main/0010D8D4#22 | 964 | 775 | -189 | 0x0A SleepThread 959/771, 0x22 ArbitrateAddress 4/3 |
| main/0010D8D4#23 | 3 | 3 | +0 | - |
| main/0010D8D4#24 | 1168 | 793 | -375 | 0x0A SleepThread 1165/790 |
| main/0010D8D4#25 | 1150 | 775 | -375 | 0x0A SleepThread 1146/771 |
| main/0010D8D4#26 | 3 | 3 | +0 | - |
| main/0010D8D4#27 | 793 | 1167 | +374 | 0x0A SleepThread 790/1164 |
| main/0010D8D4#28 | 776 | 1149 | +373 | 0x0A SleepThread 771/1145, 0x22 ArbitrateAddress 4/3 |
| main/0010D8D4#29 | 3 | 3 | +0 | - |
| main/0010D8D4#3 | 20704 | 20643 | -61 | 0x37 18607/18557, 0x0A SleepThread 888/870, 0x22 ArbitrateAddress 626/632 |
| main/0010D8D4#30 | 1043 | 1106 | +63 | 0x0A SleepThread 1040/1103 |
| main/0010D8D4#31 | 1025 | 1089 | +64 | 0x0A SleepThread 1021/1084, 0x22 ArbitrateAddress 3/4 |
| main/0010D8D4#32 | 3 | 3 | +0 | - |
| main/0010D8D4#33 | 918 | 1169 | +251 | 0x0A SleepThread 915/1166 |
| main/0010D8D4#34 | 901 | 1152 | +251 | 0x0A SleepThread 896/1147 |
| main/0010D8D4#35 | 3 | 3 | +0 | - |
| main/0010D8D4#36 | 1106 | 981 | -125 | 0x0A SleepThread 1103/978 |
| main/0010D8D4#37 | 1088 | 963 | -125 | 0x0A SleepThread 1084/959 |
| main/0010D8D4#38 | 3 | 3 | +0 | - |
| main/0010D8D4#39 | 917 | 793 | -124 | 0x0A SleepThread 914/790 |
| main/0010D8D4#4 | 3034 | 3208 | +174 | 0x0C 1068/1130, 0x0A SleepThread 915/971, 0x0B 915/971 |
| main/0010D8D4#40 | 901 | 775 | -126 | 0x0A SleepThread 896/771, 0x22 ArbitrateAddress 4/3 |
| main/0010D8D4#41 | 3 | 3 | +0 | - |
| main/0010D8D4#42 | 660 | 783 | +123 | 0x0A SleepThread 657/780 |
| main/0010D8D4#43 | 642 | 764 | +122 | 0x0A SleepThread 638/760 |
| main/0010D8D4#44 | 3 | 3 | +0 | - |
| main/0010D8D4#45 | 1128 | 939 | -189 | 0x0A SleepThread 1125/936 |
| main/0010D8D4#46 | 1110 | 921 | -189 | 0x0A SleepThread 1106/917 |
| main/0010D8D4#47 | 3 | 3 | +0 | - |
| main/0010D8D4#48 | 1106 | 793 | -313 | 0x0A SleepThread 1103/790 |
| main/0010D8D4#49 | 1089 | 775 | -314 | 0x0A SleepThread 1084/771, 0x22 ArbitrateAddress 4/3 |
| main/0010D8D4#5 | 431 | 429 | -2 | 0x22 ArbitrateAddress 217/216, 0x32 SendSyncRequest 214/213 |
| main/0010D8D4#50 | 2 | 2 | +0 | - |
| main/0010D8D4#51 | 368 | 863 | +495 | 0x0A SleepThread 366/861 |
| main/0010D8D4#52 | 352 | 846 | +494 | 0x0A SleepThread 348/843, 0x22 ArbitrateAddress 4/3 |
| main/0010D8D4#6 | 1 | 1 | +0 | - |
| main/0010D8D4#7 | 1 | 1 | +0 | - |
| main/0010D8D4#8 | 93221 | 96941 | +3720 | 0x37 38146/40626, 0x0A SleepThread 19073/20313 |
| main/0010D8D4#9 | 161744 | 268771 | +107027 | 0x32 SendSyncRequest 18320/126298, 0x22 ArbitrateAddress 35053/34124, 0x19 ClearEvent 54185/54174 |
| **all 87 threads** | 18727402 | 19187897 | +460495 | 59 of 87 threads differ |

#### IPC requests per command

187777 requests on the reference, 295567 on the candidate, 97 distinct commands; 19 of them with a different count on at least one thread.

| service | header | requests (reference) | requests (candidate) | difference | threads that differ (reference/candidate) |
|---|---|---|---|---|---|
| dsp::DSP | 0x00130082 | 7616 | 7546 | -70 | main/0010D8D4#11 7616/7546 |
| fs:USER/file | 0x080200c2 | 3738 | 3701 | -37 | main/0010D8D4#12 2317/2289, main/0010D8D4#17 580/571, main/0010D8D4#3 583/584, main/0010D8D4#5 214/213 |
| gsp::Gpu | 0x00080082 | 63655 | 63630 | -25 | main 63655/63630 |
| gsp::Gpu | 0x000c0000 | 108212 | 216194 | +107982 | main 89892/89896, main/0010D8D4#9 18320/126298 |
| y2r:u | 0x00010040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00030040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00050040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00070040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x000d0040 | 235 | 231 | -4 | main 235/231 |
| y2r:u | 0x00100102 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00110102 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00120102 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00180102 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x001a0040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x001c0040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x001e0100 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00210040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00220040 | 234 | 230 | -4 | main 234/230 |
| y2r:u | 0x00260000 | 234 | 230 | -4 | main 234/230 |
