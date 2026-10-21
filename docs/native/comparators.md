# The comparators: what each rule compares, and what it still catches

Written by N0-25 (D30, D31, D34) and extended by N0-32 (D50, D51, D52: sections 7 to 9). The tools: `tools/compare_svc_traces.py` (system calls), `tools/native/state_diff.py` (game state), `tools/compare_ipc_traces.py` (IPC), `tools/native/capfmt.py` (the HID and DSP captures), `tools/native/cross_model_report.py` (N0-28: the report of the cross profile's numbers), `tools/native/golden.py` (N0-09: runs them). The trace formats are in [`wp/N0.md`](wp/N0.md) (*Rules shared by the N0 packages*) and `tools/native/tracefmt.py`. Every comparator prints only service names, SVC numbers, register names, result codes, thread names and counts: its output can be quoted. The traces themselves are game data and stay in `build/`.

## 1. The two profiles (D31)

| Profile | Compares | Svc comparison | State comparison |
|---|---|---|---|
| `same`: two runs of one backend (`--cpu recomp` twice; later the native runtime's deterministic mode twice) | exact; no tick tolerance | all calls, in order, with `--normalize --outputs`. Fallback only, if N0-29 cannot remove the tick jitter (D30): `--collapse-polling config/native/svc_polling.txt` | `state_diff.py --tolerance 0 --min-changes <recorded>` |
| `cross`: two timing models (`jit` against `recomp`, the reference against the native runtime) | what a different clock cannot change | only the calls of `config/native/svc_cross_model.txt` (`--cross-model`); the full trace is information | `state_diff.py --tolerance 4 --min-changes` and the margin guard of 60 frames (D50, section 9); IPC per thread with `config/native/ipc_time_dependent.txt`, the `fs:USER/file` Reads as multisets and the masked Writes (D52, section 7); HID; DSP aligned on its first buffer change (D51, section 8) |

Once N0-26 and N0-29 are done, a `same` comparison needs no rule: the text traces are identical apart from their first line.

## 2. `compare_svc_traces.py --collapse-polling FILE`

`FILE` (`config/native/svc_polling.txt`) lists the calls whose result depends only on the clock and that change nothing, one per line, `<svc hex>  # reason`. It holds `0x28` (GetSystemTick) and nothing else. Use it with `--normalize --outputs`.

In each thread, a maximal run of consecutive calls of one listed svc becomes one record. Two records match when:

- both are runs of the same svc and the first call of each was made from the same place (`pc` and `lr`);
- the places of the shorter run (the `pc` and `lr` of its calls) are all places of the longer one; runs of one length must have the same places (a longer run is a shorter one plus more iterations of the loop, which may pass places the shorter one never reached);
- nothing else is compared: not the lengths (they are printed: per thread, the number of runs and the longest run on each side), not the registers or `t=` of the calls in a run. The registers of a polling call carry what the call before it left, for example those of a `SleepThread` that just returned.

Every other call compares as without the flag, in order: svc, `pc`, `lr`, the arguments, and with `--outputs` the results. `--in4` is not added to the collapse: in every `SleepThread` call `r6` holds the high word of the last GetSystemTick result, which is not an argument of the call and differs between runs.

**What it still catches.** A polling call changes nothing, so a run can only matter through the code that makes it and through the calls around it, and both are compared: a run on one side with no run at that place on the other (the next record is a run on one side and a call on the other); a run that starts somewhere else; a shorter run that passes a place the longer one never does; a non-polling call missing, added, moved, or with another argument or result between two runs (the two runs then merge on one side and are two records on the other). What it cannot catch: the number of iterations of a polling loop (that is the clock), and a difference in the registers of a polling call.

**What it does not cover.** The `SleepThread` durations that the game derives from the clock (whole milliseconds). They are compared like any call and still differ, see the measurement below. The fallback can therefore pass only if N0-26 or N0-29 remove that difference.

## 3. `compare_svc_traces.py --cross-model FILE`

`FILE` (`config/native/svc_cross_model.txt`) lists the calls compared between two timing models, one per line: `<svc hex> in=<regs> out=<regs>  # reason`. The registers listed are the ones that ARE compared (`config/native/svc_unused_registers.txt` lists the ones that are ignored). `in=` takes `r0`-`r6` (`r4`-`r6` come from the trace's `in4=` field) and `out=` takes `r0`-`r3`. The option implies `--normalize` and cannot be combined with `--collapse-polling`, `--in4` or `--ignore-args`.

- **Calls.** The criterion: calls that create, map, unmap, duplicate or close kernel objects or memory, or start or end a thread, and whose result depends neither on time nor on another thread's progress. The file holds ControlMemory, CreateThread, ExitThread, CreateMutex, CreateSemaphore, CreateEvent, CreateTimer, CreateMemoryBlock, MapMemoryBlock, UnmapMemoryBlock, CreateAddressArbiter, CloseHandle, DuplicateHandle and ConnectToPort, each with its reason. Never waits, sleeps, signals, arbitration, the clock or SendSyncRequest. The registers follow the SVC wrapper (Azahar's `svc_wrapper.h`, as `native/src/kernel/svc.cpp` documents it): the result is `r0`, the values an output pointer receives follow in `r1` up, and the inputs keep their positions with the output pointers' slots skipped, so the slot of an output pointer (`r0` of CreateEvent, for one) holds a stack address the caller left and is never listed.
- **Kernel-chosen addresses are never compared.** Which address an allocation gets depends on the order in which threads allocate, which differs between timing models. For ControlMemory that is the operation (`r0`), the size (`r3`) and the permissions (`r4`), and the result code; not `r1` and `r2` (addresses taken from an earlier result, or a hint) and not the address it returns. For CreateMemoryBlock and MapMemoryBlock: sizes, permissions, the handles and the result, not the address, because a trace cannot tell a constant of the game's code from one returned by an earlier call. A thread's argument and stack top (CreateThread `r2`, `r3`) are heap addresses and are not compared either; the entry point, priority and processor id are.
- **Compared per call.** `svc`, `pc`, `lr` and the result code (`out r0`) always; the listed inputs; the listed outputs with `--outputs`. Handles are compared by their normalised name. Each thread's sequence is filtered to the listed calls and compared in order, with `--prefix` as in the plain comparison.
- **Names across models.** Threads are `main`, and otherwise `<parent name>/<entry>#<k>`, where k counts the threads that parent created with that entry, in its own order (from the `parent=` of the `# thread` lines). `tracefmt.py`'s global `<entry>#<k>` can swap two threads with the same entry created by different parents when the interleaving differs; the per-parent name cannot, because one parent creates its threads in program order. Handles are `<kind>#<creating thread>.<k>`, k counting per creating thread and kind from 0, so creations interleaved differently across threads keep their names. A handle that came in an IPC reply is `?` (this tool does not read the IPC trace).
- **Output.** Per thread: the number of compared calls on each side and, for information, each side's total number of calls.

**What it still catches.** A creation, map, unmap, duplicate, close, thread start or thread end that one model does and the other does not, or does at another place, with another size, permission, operation, priority, entry point or processor id, or with another result code; a thread that one model never creates (its parent's CreateThread is missing, and the thread's own listed calls too); the order of one thread's own listed calls. What it cannot catch: anything about waits, sleeps, signals, arbitration, SendSyncRequest and the clock (those run a different number of times in two timing models: the IPC trace, the state checkpoints, HID and DSP cover what they cause); which address the kernel handed out; a thread that makes no listed call (it is only visible through its parent's CreateThread).

## 4. `state_diff.py --min-changes N`

`state_diff.py` aligns two state traces on the scene byte (`scene`): the k-th scene change of one with the k-th of the other, and compares, at each aligned change, the scene reached, the frame (within `--tolerance`) and the `eq` observables of `config/native/observable.txt`. It counts the first record as a scene change, so a trace whose scene never changes agreed with itself (D34, M7).

`--min-changes N` (default 0: no minimum) exits 3 and prints how many it aligned when fewer than N **real** scene changes were aligned in the reference trace. A real scene change is a record whose `scene` is readable (not `-`) and differs from the previous readable `scene`; the first record, a change from `-` to a value (every trace starts with `scene=-`, then a value) and a change to `-` are not real. The alignment and the statuses 0 and 1 are unchanged; a divergence wins over 3. The final line prints the number of real changes.

**What "state passed" means.** `state_diff.py` compares the `eq` values only at the aligned scene-change records. `stage_loader_state` and `quest_id` are therefore not checked inside a scene, and G1, G2 and G6 have no quest, so there is no `quest_id` to check. A passed state comparison means: the scene sequence, the frames of its changes within the tolerance, and the values of the `eq` observables at those records. No more.

## 5. Exit statuses (svc and state; the IPC and capture tools have their own, below)

| Status | `compare_svc_traces.py` | `state_diff.py` |
|---|---|---|
| 0 | every thread matches | the traces agree (and at least `--min-changes` real changes) |
| 1 | a thread differs (the first difference per thread is printed; with `--collapse-polling` or `--cross-model` also every record that differs at the same position, up to 20 per thread, with the names of the fields) | a divergence (the first is printed) |
| 2 | a trace or an option file cannot be read, holds no system call, or an option is wrong | bad input |
| 3 | nothing was compared: after the `--cross-model` filter neither trace has a call to compare (D34) | fewer than `--min-changes` real scene changes were aligned |

A difference wins over 3. `golden.py check` (N0-09) uses the same four statuses.

## 6. What was measured (N0-25, on the saved traces of 2026-10-17; numbers only)

**Polling, G1 to frame 800, Old 3DS, `--cpu recomp`.** Each trace has 18 threads (17 numbered and main). In every pair the main thread has 91546 records after the collapse and the other threads have the same number of records on both sides; polling runs: 1986 in main and 2894 in the thread that makes the other polling calls, 4880 in all.

| Pair | Calls in main (ref vs cand) | Collapsed records | `--collapse-polling --normalize --outputs` |
|---|---|---|---|
| `n018/A1` and `A2` (current reference runner) | 460698 vs 461742 | 91546 vs 91546 | exit 0 |
| `n004/R1` and `R2` (recomp) | 463524 vs 461734 | 91546 vs 91546 | exit 1: 5 of 91546 records differ |
| `n018/B1` and `B2` (runner from before N0-18) | 463264 vs 461766 | 91546 vs 91546 | exit 1: 5 of 91546 records differ |

The 5 records of the two failing pairs are the same positions in both (main thread records 16844, 21156, 23312, 25339, 27495), all `SleepThread` (`0x0A`), differing in `r0` (the duration) and `r2` (a leftover that follows from it) and in the same two registers of the result: 7 ms against 6, 5 against 4, 4 against 3, 3 against 2, 2 against 1. The tool prints each of them with its position and the names of the registers. No exception is added for them. Main has 381 `SleepThread` records; the other 376 match.

Of the 4880 polling runs in each pair, 4879 (A), 4878 (B) and 4879 (R) have the same set of places on both sides; the others (1, 2 and 1) differ only in that the longer run passes more places (in A1 and A2, a run of 2 calls against one of 4), and all 4880 start at the same place and have the places of the shorter run among those of the longer.

**Cross-model, `n004/R1` (recomp) against `n004/J1` (jit), same arguments, `--outputs --cross-model config/native/svc_cross_model.txt`: exit 0.** 111 calls compared on each side: ControlMemory 2, CreateThread 17, CreateMutex 1, CreateEvent 5, MapMemoryBlock 2, CreateAddressArbiter 1, CloseHandle 81, DuplicateHandle 1, ConnectToPort 1 (CreateSemaphore, CreateTimer, CreateMemoryBlock, UnmapMemoryBlock and ExitThread do not occur in the first 800 frames). Their threads: main 84, `main/<entry>#1` 1 and `main/<entry>#13` 26; the other 15 make no listed call. In all, 492581 calls (R1) against 545737 (J1); the totals per thread differ in 10 of the 18 threads. No register of the 111 pairs differs, listed or not, so the exclusions of the addresses are not exercised by this pair: they follow from the allocation order, not from a measured difference. `R1` against `R2` (same model) also gives no difference in the 111.

**State, the same traces.** The 800-frame traces never leave the first scene: `state_diff.py` counts 2 scene changes (the first record and the change from `-`) and 0 real ones; with `--min-changes 1` every saved pair (`n018` A, B; `n004` R, J) exits 3.


## 7. `compare_ipc_traces.py`: the `fs:` rules of D52 (`--fs-reads-multiset`, `--fs-write-mask`)

Two timing models cannot be expected to make the same number of reads, or to interleave two readers the same way, but **what a game reads from a file follows the file, and what it writes is its state** (D52). The cross profile of `golden.py` and the native runtime's checks (N1-21, N1-26) run `compare_ipc_traces.py` with these options; no `fs:` command goes in `config/native/ipc_time_dependent.txt`.

- **`--fs-reads-multiset [--max-surplus N]`** (default 16). Per thread, the Read requests of the file sessions of `fs:USER` (`fs:USER/file`, header `0x080200c2`) are taken out of the thread's list and compared as a multiset of **(offset, size, result, CRC-32 of the data the reply wrote)**, order ignored. The multiset of the side with fewer reads must be contained in the other side's, and the other may hold at most `--max-surplus` more. So five reads fewer (a reader cut short by the end of the run or a scripted skip) and two reads in another order pass; a surplus of 17, a read of another offset or size, a read that returned other data, or a read with another result fails, and so does a thread that reads on one side only. (The result is part of the key on top of the three values of D52: it can only make the rule stricter.) The report names the offsets, sizes, result and CRC of the first reads the other side lacks, never the data.
- **Everything else of the `fs:` sessions is compared in order and in full**: with the option, a request of any `fs:` session (a Write above all) must also have the same header and **normal parameters** in the request and in the reply (an offset, a size, flags, a path's size, a handle's number) as its counterpart, besides the session, header, result and descriptors (the CRC-32 of a Write's data is a descriptor). The words after the normal parameters are translate parameters (descriptors, handles, the address of a buffer, which the process chooses and a different clock can change), so they are not compared. Requests of services that are not `fs:` are compared as before (the plain comparison reads no word).
- **`--fs-write-mask FILE --fs-write-mask-session S`**. `FILE` is `config/native/fs_write_masks.txt`: one line per byte range, `<session> <write-size> <write-offset> <mask-offset> <mask-size>  <reason>`, written by the orchestrator when a work package proves a range depends on the clock (N0-31 proved the two ranges of G2's save). A Write of a file session whose size and file offset are those of a line of session `S` is compared by the CRC-32 of its data **with the masked ranges set to zero**, on both sides; a byte that differs outside a masked range still fails. The trace must hold the Write's data (`--trace-ipc-full`: `data=`). The lines of other sessions are not looked at; `golden.py` passes the session of the recording.
- **Errors, never a pass** (exit 2): a mask file that is missing, is not UTF-8 text, holds no line or a malformed line (a missing reason, a negative number, a range that is empty or outside the Write); a line of session `S` that matches no Write of either trace; a Write that a line applies to whose trace has no data for it, or whose data does not match the CRC-32 the trace recorded for it. `--max-surplus` without `--fs-reads-multiset`, one of the two mask options without the other, and a negative number are usage errors (also exit 2).
- The output ends with `fs reads: ...` (threads that read, reads on each side, the largest difference) and `fs write masks: ...` (lines of the session and Writes they applied to); `golden.py` prints them with the result of `ipc`.

**What it still catches.** A read that returns other bytes, another offset or size, or fails; more than 16 reads beyond the other side (a loop that reads in a runaway); a thread that reads on one side only; every `fs:` command that is not a Read, in its order and in its normal parameters; a Write with any byte different outside the masked ranges, or with a different size, offset or flags. **What it cannot catch.** The order of the reads of one thread (D52 drops it), and how many reads a thread makes up to the surplus; a difference inside a masked range (that is what the mask is for: the reason of each line says why).

## 8. `capfmt.py diff-dsp --align [--max-shift 48]`: the DSP capture across timing models (D51)

`diff-dsp` keys on the DSP's own frame counter, which is the model's clock: on `jit` the same audio stream plays a constant number of DSP frames later than on `recomp` (22 in G1, N0-28). `--align` compares the content:

1. the **first enabled source** is the lowest-numbered source that is enabled, in the region the DSP reads, in the first frame of the reference that has one;
2. the first change of that source's buffer id in the reference is looked for in the candidate **by its id** (a capture may start in the middle of a stream: the candidate's first changes can be buffers the reference started after), and the difference of the two DSP frame counters is the **shift**;
3. the shift must be at most `--max-shift` DSP frames (default 48, about 233 ms) in either direction;
4. it must be **constant**: every other buffer change of that source inside the overlap (one frame in from each end) has its counterpart, with the same id, at the shift within 1 frame, and the candidate has no change the reference lacks;
5. after the shift, every source status block of both regions of every overlapping DSP frame (enabled, dirty, sync count, buffer position, current and last buffer id) must be equal. The PCM samples and the sample data are not compared across models (the native runtime has no mixer);
6. the shift is printed, with the buffer and frames it was found on, the numbers compared and the largest deviation, on success and on failure. Exit 0 equal, 1 any failure above, 2 a file that is not a DSP capture; `--max-shift` without `--align`, a negative bound and `--align` with `--frames` are usage errors.

When neither capture has an enabled source, or the source never changes buffer in either, there is nothing to align on: the comparison is the plain one (shift 0, keyed on the counter) and says so; if only one side has an enabled source, or only one side has buffer changes, or no buffer id is in both, it fails. On G1's two captures (the recording and the first `jit` run): shift +22, 293 DSP frames and 14064 status blocks compared, all equal, 85 buffer changes at the shift with a deviation of 0 frames; `--max-shift 10` exits 1.

## 9. The cross profile's state comparison and its margin guard (D50)

The cross profile runs `state_diff.py --tolerance 4 --min-changes <the recording's real scene changes>` (the same profile stays at `--tolerance 0`): a real scene change may fall up to 4 frames apart on the two models, to the same scene and in the same order. On top of it, `golden.py` adds the result `state-margin`, with `cross_model_report.py`'s logic: every input event of the session script that follows a real scene change must come **at least 60 frames** after the last real scene change on **both** sides, so that a shift of a scene change can never move an input into another scene. An input before the first real scene change has no margin. Both numbers can be changed in the cross profile only (`golden.py check --tolerance N --min-margin N`); `--require state` covers `state-margin`. The result prints the largest shift and the smallest margin of each side. The numbers are D50's: measured on 13 real scene changes in G1, G2 and G6 (largest shift 3, smallest margin 91; `docs/native/cross-model.md`).
