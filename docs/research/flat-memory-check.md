# Flat-memory coherence check (N0-14)

Evidence for principle 2 of the native port ([`../native/ROADMAP.md`](../native/ROADMAP.md)): the game's memory can be one
flat block that the CPU and the GPU both use in place, with no copy and no cache flush. Two things would break it, and
this check measures both:

- **(a)** the GPU uses data the CPU changed, and no cache flush covers it (so on the 3DS the data may never have reached
  the GPU, and the renderer would have to find out what changed by itself, for example by hashing);
- **(b)** the CPU reads something the GPU wrote (so the renderer would have to write its results back into guest memory
  where the CPU can see them).

**Status: measured.** Both parts ran on the maintainer's PC: part (b) on 2026-10-31, local time (the last session, G3,
ended at 00:44 on 2026-11-01), and the read-based coverage of part (a) on 2026-11-01. The answer (4.3): nothing measured
contradicts principle 2, within the limits that 4.3 lists.

- (a) covers a window of 3 or 10 frames in each of the four golden sessions
  ([`../native/golden.md`](../native/golden.md)). There every byte the GPU read after a CPU change had a cache flush
  recorded before the read. For 44 / 0 / 42 / 6,942 bytes of vertex and index data that flush was recorded in the
  change's own interval, so whether the store came first is unknown. The windows hold no texture upload.
- (b) covers the whole of the four sessions. No CPU load of a watched page the GPU had written was logged. The one
  GPU-written page the check could not watch is the GSP shared memory, which the game reads by design as its channel to
  the GSP service; it is not render output.

| | Tool | Input |
|---|---|---|
| (a) | `python3 tools/native/coherence.py capture FILE.mgpu ... --alias-log LOG` | the GPU captures of G1 to G4 (`build/golden/<S>/gpu-<start>.mgpu`) and the log of a part (b) run (`LOG`) |
| (b) | `mh4u_runner --check-coherence FILE` then `python3 tools/native/coherence.py runlog FILE` | a run of each golden session |

Everything below `build/` is game data: captures, logs and reports stay there. This document holds counts, hashes and
addresses only: no game bytes and no game text.

## 1. Part (a): from a capture

A capture (`docs/native/mgpu-format.md`) has three kinds of record that matter here:

- before every GPU event (a GX command, a register write `MMIO`, `SWAP`, `FILL`), a `CPU_PAGES` record with the pages
  whose contents differ from the shadow, that is, the pages that changed since the previous diff;
- `FLUSH` records: the game's `FlushDataCache`, `InvalidateDataCache` and `StoreDataCache` requests (virtual addresses,
  from the IPC recorder) and the regions of `CacheFlush` GX commands;
- after a command, a `GPU_PAGES` record: what the GPU wrote.

`coherence.py capture` streams the file (a capture is 140 to 175 MB) once and gives two answers from that pass:

- the **same-interval rule** looks at each event's own interval and counts every changed byte;
- the **read-based coverage** follows each changed byte to the GPU read that uses it.

One class of the read-based coverage is also called **same interval**: a byte the GPU read whose only flush was recorded
in the change's own interval. Both look at the flushes recorded in the change's own interval, but the rule counts every
changed byte such a flush covers, read by the GPU or not, and the class counts only the bytes a read took (4.1 compares
the two).

### The same-interval rule

For each event, the tool:

1. takes the pages of its `CPU_PAGES` record and, for each, the bytes that differ from the page's previous contents
   (`MEM`, `CPU_PAGES` and `GPU_PAGES` applied in order, so a page the GPU wrote is compared with what the GPU wrote);
2. takes the `FLUSH` records recorded since the previous event's end (its `GPU_PAGES` position: the shadow is brought up to
   date there) up to this event, converts their virtual ranges to physical ones with the fixed mapping of Azahar's GPU
   (`VideoCore::GPU::VirtualToPhysicalAddress`: VRAM `0x1f000000`, the linear heap `0x14000000` and the new linear heap
   `0x30000000` onto FCRAM; the constants are in `coherence.py`), and counts how many of the changed bytes lie inside
   them. A `CacheFlush` command's own regions (recorded right after its `GX` record) count for its own `CPU_PAGES`;
3. classes each page as **full** (every changed byte covered), **partial** or **none**.

Reported per row (one row per GX command kind of `gcap.GX_NAMES`, and `MMIO`, `SWAP`, `FILL` as rows of their own,
because `CPU_PAGES` precede them as well): events, pages, changed bytes, the share of the changed bytes covered, the page
classes, and the uncovered ranges grouped (consecutive pages). A page is counted once per event whose `CPU_PAGES` holds
it, so the page counts are page changes, not distinct pages. The coverage is shown for any flush, for each kind of flush
alone (`FlushDataCache`, `InvalidateDataCache`, `StoreDataCache`, `CacheFlushRegion`) and for the set principle 2 names
(`FlushDataCache` and the `CacheFlush` regions). A flush whose address maps nowhere in the fixed mapping (a heap
address, say) is counted apart (`maps nowhere`) and covers nothing here: the game may flush linear memory through a heap
alias, which this rule cannot see (the read-based coverage maps it through the alias runs).

**Named pages.** The column "named pages not fully covered" counts the pages of the uncovered set that the command's own
words say it reads: the command list buffer (`SubmitCmdList`), the source of `RequestDma`, the input of `TextureCopy`
(`size` bytes, an upper bound: the line gaps are ignored) and of `DisplayTransfer` (width x height x bytes per pixel from
its flags); a fill reads nothing. A command list also reads the textures, vertices and shaders its register writes point
at, and those addresses are in the `PICA_REG` records, not in the words, so this subset is a lower bound on what the GPU
reads.

**Why the rule reports most changes uncovered.** It counts every changed byte, read by the GPU or not, and most of the
bytes the GPU does not read are heap, stack and data: in the four captures, 91 / 83 / 89 / 64% of them lie in alias
regions (4.1). It also credits a flush only in the change's own interval. The game writes the next frame's command list
before `SWAP` and flushes it with a `CacheFlush` command just before the `SubmitCmdList`, a later interval, so the rule
calls those bytes uncovered. The read-based coverage has neither problem.

### Read-based coverage

The same pass asks, for each byte the GPU reads: did the CPU change it earlier in the capture, and was a flush recorded
between the change and the read? (`_ReadCoverage` in `coherence.py`, key `read_coverage` of the report.)

- **Intervals and changes.** Interval k is the time before event k (the `GX`, `MMIO`, `SWAP` and `FILL` records,
  numbered from 0). Every byte that differs in the `CPU_PAGES` record before event k is a change of interval k. A later
  change to the same byte replaces the earlier one; a GPU write to a byte with a pending change ends that change (the
  CPU's value is gone from memory).
- **Flushes.** A `FLUSH` record covers the pending changes in its range: with **certain** order when it is recorded in a
  later interval than the change, and only **in the same interval** when it is recorded in the change's own interval (the
  capture cannot tell whether the store or the flush came first). A `CacheFlush` command's regions are placed at that
  command: certain for the changes of earlier intervals, same interval for those of its own `CPU_PAGES`. A flush by an
  address in the GPU's windows maps through them, as in the same-interval rule. A flush by an address outside them is
  mapped through the alias runs of `--alias-log` (the game flushes by the address it uses), and covers nothing without
  them. The alias map is the union of the log's scans, so two runs could map one address to different memory (a remap
  between two scans); a flush covers no byte of such an address, while a DMA source reads from both.
- **Reads.** At each event the GPU reads the addresses the command's words name (the command list of `SubmitCmdList`,
  the source of `RequestDma`, the input of `TextureCopy` and of `DisplayTransfer`). For a `SubmitCmdList` it also reads
  what a replay of the PICA registers (the `INIT` record, then the `PICA_REG` records) says its draws and jumps read:
  each enabled texture unit with its mip chain (six faces for a cube texture), the vertex span of each attribute loader
  (`[vmin, vmax]`, taken from the index data for an indexed draw), the index buffer, and chained command buffers. A named
  address outside the GPU's windows is passed through as a physical address for a command list or a transfer (as Azahar
  does) and followed through the alias runs for a DMA source (Azahar copies it through the game's page table); bytes that
  map to no memory the capture holds are not modelled. An event's reads come before the flushes and GPU writes recorded
  after it.
- **Not modelled:** the colour and depth buffers a draw loads from memory, and the screen the display reads at `SWAP`. A
  change that only those read counts as not read.
- **Upper bounds.** The read ranges are upper bounds: every enabled unit, even if no stage samples it; the whole mip
  chain; the full span of each loader.
- **Classes.** The first read of a changed byte decides its class: **certain**, **same interval** or **not covered** (no
  flush between the change and the read). A later read of the same unchanged byte is not counted again. A pair is one
  page changed in one interval and read by one event; pages and pairs are counted in the worst class of their bytes.
  The changes of the first interval can have no earlier flush in the file; their uncovered bytes are reported apart.
- **Regions.** With `--alias-log`, the `ALIAS` runs of a part (b) log (their union; a run seen in several scans counts
  once) split FCRAM into regions. An alias region is memory the game reaches through an address that the GPU's fixed
  mapping does not translate (code, heap, stack, data, shared memory, thread-local storage), named by the run's virtual
  address. `FCRAM linear` is the rest, which the game reaches through the linear windows; VRAM is a region of its own.
  Each changed byte ends in one of: read by the GPU, replaced by a later change, overwritten by the GPU, or still pending
  at the end (in a read range of the capture but changed after its last read, or never in one). Per region the report
  also counts the changed bytes any flush covered from the change's interval to the end (an upper bound).
- **Counts.** Draws, texture reads, chained-buffer jumps, `SubmitCmdList` commands with `do_flush` (word 7) nonzero
  (counted, not credited as a flush: Azahar ignores the word), and `CacheFlush` commands whose region is exactly the
  buffer of the next `SubmitCmdList`.

### Approximations

All are stated by the tool's output or here. Each says which answer it concerns.

- **The first interval (both).** The interval before the first event follows `MEM`: a flush the game made before the
  capture started is not in the file. The rule reports the first event's pages as a row of their own,
  `(first interval)`, to be read as an upper bound on what is uncovered; the read-based coverage reports the uncovered
  bytes changed in that interval apart.
- **A change after the flush (both).** `CPU_PAGES` is taken lazily, just before the next event, so a page in it changed
  at some point after the previous diff. The rule takes a flush recorded in that interval to cover the page, even if the
  CPU changed it after the flush; the capture cannot tell the two apart, so the rule under-counts the uncovered changes.
  The read-based coverage reports these bytes as **same interval** instead.
- **Who changed the page (both).** A page "changed by the CPU" is any page that changed outside a GPU command: the game's
  own stores, but also the kernel's and the services' (file reads into linear buffers, the camera, the DSP).
- **A flush's range, not the cache (both).** A flush counts as covering its virtual range; whether the data had reached
  memory is not observable here (the emulator has no cache).
- **Only gsp::Gpu flushes (both).** The IPC recorder keeps the game's `FlushDataCache`, `InvalidateDataCache` and
  `StoreDataCache` requests to gsp::Gpu; other flushes (`svcFlushProcessDataCache`, the DSP service's) are not in the
  capture. A change covered only by such a flush counts as not covered.
- **The alias map comes from another run (read-based).** `--alias-log` takes the alias runs of a part (b) run, not of
  the golden run that wrote the capture: same game, same boot, same layout assumed. In 4.1 it is consistent with the
  captures: the code and read-only data runs never change, no flush maps nowhere, and no GPU read took a changed byte
  in an alias region. The 8 runs of the measured logs do not overlap, virtually or physically.

## 2. Part (b): `mh4u_runner --check-coherence FILE`

### Method

The runner keeps a shadow of all of FCRAM and VRAM (as `--capture-gpu` does, and with the capture's own page diff,
`GcapChangedPages`). Around every `GPU::Execute` and `GPU::WriteReg`:

1. **before the command**, the CPU's changes are taken into the shadow (pass 1), so that the diff after it holds only what
   the command changed;
2. **after it**, the diff (pass 2) is the set of pages the GPU changed. Each page that is not already marked is marked with
   `RasterizerMarkRegionCached(paddr, size, true)`, in runs. The runner keeps its own set of marked pages: Azahar hits
   `UNREACHABLE()` when a page that is already rasterizer-cached is marked again, or when a plain page is unmarked, and the
   software renderer marks nothing itself. A page the GPU changed again while still marked stays marked and keeps the
   latest writer (counted as `rewritten`).

A marked page has a null host pointer in the page table, so a virtual access to it (the recompiled code's slow path, the
interpreter, Dynarmic's callbacks, a kernel or service handler) takes the `RasterizerCachedMemory` case of Azahar's
`memory.cpp`. The runner's patch (`azahar.patch`, `Memory::SetCachedAccessObserver`) calls a function pointer there, a null
test unless the option is on, from **every** such case, and tells which one it was:

| Kind in the log | Where in `memory.cpp` | Meaning |
|---|---|---|
| `Read`, `Write`, `WriteExclusive` | `Read<T>`, `Write<T>`, `WriteExclusive<T>` (each has a watchpoint twin) | a single load or store: the CPU's slow path, or a service reading one word |
| `BlockRead`, `BlockWrite` | `ReadBlockImpl`, `WriteBlockImpl` | a copy by the kernel or an HLE service (IPC buffers, file reads), or by the GPU's DMA |
| `ZeroBlock` | `ZeroBlock` | a clear by the kernel or a service |
| `CopySource` | `CopyBlock` (the source side; its destination goes through `WriteBlock`) | a copy inside guest memory, including GX `RequestDma` |
| `Pointer` | both `GetPointer` overloads | a raw host pointer is handed out: the size is 0, and what the caller does with it (how many bytes, which direction) is not seen |

The checker logs a **CPU-side** access (below) and **unmarks exactly the page that was accessed**, after logging it (a
block access calls the hook once per page). So each mark gives at most one CPU line: the first CPU access since the GPU
wrote the page. A **GPU-side** access (see next paragraph) is logged at most once per mark and **leaves the page
marked**, so a GX DMA that reads or writes a GPU-written page does not hide a later CPU read of it from the check. A page
the GPU changes again while it is still marked keeps its mark and gets the new writer (`rewritten`), and its next
GPU-side access is logged again.

**CPU or GPU.** Not every access through that path is the CPU's. GX `RequestDma` is executed in `GPU::Execute` with
`memory.CopyBlock` through the application's page table. An access made while the runner is inside a GPU hook (`Execute`,
`WriteReg`) is tagged `side=gpu` and counted apart; `side=cpu` is everything else that happens while the emulation loop
runs (recompiled code and the services it calls). Accesses by the runner itself between slices (the frame counter, dumps,
the state trace) and by other host threads are counted in the summary and not logged. Inside the hooks the runner reads the
frame counter through the process's page-table pointer, never through `memory.Read*`.

### Coverage: what cannot be watched

`PhysicalToVirtualAddressForRasterizer` maps every page of FCRAM to both linear windows, `0x14000000` and `0x30000000`,
whatever the console model, and VRAM to `0x1f000000`; the extra FCRAM of a New 3DS maps only to `0x30000000`. The game,
run with `--new-3ds off`, uses the `0x30000000` window (4.2 has an example). A page the GPU wrote can be unreachable
through those windows in the game process (it maps the memory only elsewhere), or reachable through them and through
another address too (a shared memory block, the application heap over the same FCRAM). Marking changes nothing for the
second address. The runner reports both:

- after marking, it looks at the game process's page table: a page for which none of its windows became rasterizer-cached
  is logged as `watched=0` and grouped at the end as `UNWATCHED kind=nowindow`;
- at the start, every 600 game frames and at the end it scans the page table for pages that map FCRAM or VRAM outside the
  windows (`ALIAS` lines when the set changes); GPU-written pages in that set are `aliased=1` and grouped as
  `UNWATCHED kind=aliased`: a CPU access to those pages through the other address is invisible;
- the `aliased` flag is taken when the page is marked, from the latest scan, and never looked at again (`runner.cpp`:
  `mark.aliased = alias_[index]` at marking). A page marked before its alias first shows up in a scan stays
  `aliased=0`, so `kind=aliased` can under-count. In the measured runs the scan at frame 0 ran before the game mapped its
  heap and its shared memory blocks (the `ALIAS` sets of frames 0 and 600 differ). The flag matters only for a page that
  a window reaches as well: a page with no window in the game's page table is counted as `nowindow` at its mark,
  whatever the alias set (4.2 has the one such page).

Partial coverage is a finding to report, not a failure of the run.

### The guest LR, and its limit

The line carries the running core's LR, `guest=1` when a guest thread is running and `lr=-` when none is. In code that
runs in the interpreter it is the live LR. On the recompiler it is `ctx->r[14]`, and recompiled functions keep their
registers in C locals: they write them back to `ctx` only at calls, system calls, traps, returns and yields
(`tools/recomp/emit.py`, `_with_locals`: `RC_FLUSH` before each, `RC_RELOAD` after a call, a system call or a yield; a
yield is when the budget check at the start of a basic block runs out, `RC_TICK` in `tools/recomp/runtime/recomp.h`).
A function that never writes R14 never writes it back, and after a call the caller reloads R14 from `ctx`, so it holds
whatever the callee left there. At a slow-path load or store the LR is therefore one of these (a Thumb caller's return
address has bit 0 set):

- **The return address of the call that led to the access**, when R14 still holds it: the access is in the function
  that call entered (a leaf, or a function before its first call), or in code that function reached by branches
  without a call (a tail `b`, or running into the next function). The function that contains the LR made the call.
- **The address just after a call the accessing function made**, when the callee left R14 alone or restored it before
  returning (`pop {..., lr}` then `bx lr`). The function that contains the LR made the access, or reached the code
  that did by branches without a call.
- **A place inside a function that had returned before the access**, when that callee returned with its own value in
  R14 (a non-leaf that returns with `pop {..., pc}`, so its own last call set R14), which may lie in a function it
  called in turn. The access was made above it in the call chain: by its caller, by a function further up if that
  caller returned without writing R14 again, or by code one of them reached by branches without a call.
- **An older value, or one that is not a return address**, when a function wrote R14 itself (this one, or a callee
  that then returned with `pop {..., pc}`), with `pop {..., lr}`, `mov lr, ...` or `ldr lr, ...`: `ctx` keeps the older
  value until the next write-back (a system call or a yield, say), and then holds the new one, which may be a scratch
  value outside the code or one that happens to fall inside a function.

So the LR is a lead, not a call chain: the LR alone cannot tell these cases apart, and reading the code at and after the
LR (the function table of the recompiled code gives the function that contains it) is what narrows them down. For a
block copy or a pointer taken by a service during a system call, the LR is the thread's at that system call, as the
cases above give it at the `svc`: when the `svc` is in a leaf stub that never writes R14 (299 of the 352 functions with
`svc 0x32` in the game's code library), it is the stub's return address, the game's call site of the service function;
when the `svc` follows a call in the same function, it is what that callee left. Work a service does later, at a
wake-up or on another host thread, carries another thread's LR or is counted as `other_thread`. The recompiler is not
changed for this.

### What the check does not see

- **A write unmarks the page.** After a CPU store to a page (even a few bytes) the page is plain again, so a later read of
  the rest of the page, which still holds GPU data, is not seen. The log has the size of every store; stores smaller than a
  page into a GPU-written page are therefore the place to look for a hidden dependency (4.2 has the one site).
- **A GPU write that lands after the command returns.** The diff assumes the software renderer has finished its writes when
  `GPU::Execute` or `GPU::WriteReg` returns, as the capture does (untested until the dump, N0-05); a late write would show
  up as a CPU change in the next pre-pass and never be marked.
- **A raw pointer** (`Pointer`) unmarks one page and says nothing about the access made through it.
- **Aliases and windows** (above), and pages marked in another process's page table than the game's. In the measured runs
  one GPU-written page has no window: the GSP shared memory (4.2).
- **Reads that bypass the page table.** HLE code that reads guest memory by physical address or through a backing pointer
  never reaches the hook: the DSP HLE's sample buffers (`audio_core/hle/source.cpp`), the AAC decoder
  (`audio_core/hle/aac_decoder.cpp`) and the kernel's shared-memory backing that the services use
  (`core/hle/kernel/shared_memory.cpp`). These are service-side consumers and none is known to read GPU output, but the
  check cannot rule it out.
- **The Luma alias path.** `Read<T>` and `Write<T>` serve an address with bit 31 set straight from FCRAM when the page
  pointer is null (a Luma3DS mapping), with no hook. A retail game does not use these addresses.
- **Reads that the hook does not reach** through the page table: none are known. Every `RasterizerCachedMemory` data
  case of `memory.cpp` has the hook, 12 call sites in 9 functions (the two `GetPointer` overloads counted apart), checked
  against Azahar `5f3306d`'s file with the patch applied (the repository's test counts the hooks in the patch). A kind of
  access that bypassed the path would make the check unable to support a conclusion for it.
- **An access that straddles a page boundary.** `Read<T>`, `Write<T>` and `WriteExclusive<T>` look up the page of the first
  byte only, so the hook and the unmark concern that page; the next page's bytes are read or written through the
  contiguous host mapping without being looked at. The recompiler's slow path (`ARM_Recomp::SlowRead`/`SlowWrite`)
  splits such an access into byte accesses, so each page is hooked there; other CPU backends are not checked for this.
  The block cases are per page.

### Options

Software renderer (the runner's only one); drawing is forced on for the whole run and `disable_right_eye_render` is off, as
for `--capture-gpu` (or the GPU writes it looks for are skipped). `--check-coherence`, `--capture-gpu` and `--perf-log`
share `Pica::g_capture_hooks`; together they are a usage error (exit status 1), like `--perf-log` with `--capture-gpu`.

### The log

A text file, one record per line, `TAG key=value ...`, addresses in hex written `0x` and eight digits, other numbers in
decimal, parsed by
`coherence.py runlog` (lines starting with `#` are skipped). First line:
`# coherence v1 <date> game=<title id> cpu=<jit|recomp|interp> new3ds=<on|off>`.

| Line | Fields | When |
|---|---|---|
| `ACCESS` | `n` (order), `f` (game frame), `t` (ticks since the run started), `side` (`cpu`, `gpu`), `kind` (table above), `paddr`, `vaddr`, `size` (decimal; for a block access, the part in this page), `guest` (0 or 1), `lr` (hex or `-`), `in` (the GPU event the access was made inside, `-` for the CPU), `mark` (sequence number of the writing event), `mark_ev`, `mark_f`, `mark_t` (its kind, frame and ticks), `watched`, `aliased` | the first access to a marked page |
| `ALIAS` | `f`, then `runs`, or `vaddr`, `paddr`, `pages` per run | when the set of unwatchable runs changes |
| `EVENT` | `ev` (`RequestDma`, `SubmitCmdList`, `MemoryFill`, `DisplayTransfer`, `TextureCopy`, `CacheFlush`, `Mmio`, `Other`; `Mmio` is a register write, the `MMIO` record of a capture), `n`, `gpu_pages`, `marked`, `rewritten`, `unwatched`, `aliased` | end of run: totals per event kind |
| `UNWATCHED` | `kind` (`nowindow`, `aliased`), `paddr`, `pages`, `marks` | end of run: grouped ranges of pages |
| `PENDING` | `pages` | end of run: pages still marked (the CPU never touched them) |
| `SUMMARY` | `first_frame`, `last_frame`, `events`, `gpu_pages`, `marked`, `rewritten`, `unwatched`, `aliased`, `accesses`, `cpu`, `gpu`, `pending`, `cpu_pages_changed` (pages pass 1 found changed, summed over the events), then the accesses the checker passed over: `foreign` (to a page the checker had not marked), `no_window` (to an address the checker cannot translate, outside the windows), `outside_run` (made by the runner between slices), `other_thread` (made by another host thread) | the last line; a log without it is from a run that did not finish |

`UNWATCHED kind=nowindow` counts pages; `no_window` in `SUMMARY` counts accesses.

The report groups the accesses by side and kind, by the GPU event that wrote the page, and the CPU's by LR (the call
sites), with the delay between the write and the first load.

## 3. Commands for the PC

Inputs are placeholders. `<golden>` is the shared checkout's `build/golden`; all outputs go to `<out>`, a folder under
`build/`. Part (b) comes first: part (a) takes its alias runs from a part (b) log.

### (b) the reference runner on each session

Run as `golden.py record` runs it (`docs/native/golden.md`), without the recorders and with `--check-coherence`:
`--new-3ds off --cpu recomp --code <code library> --install <update.cia> --user <user dir> --frames <the session's # frames>
--input-file tools/native/golden/<S>.txt`, plus `--init-time <the session's # init-time:>` when the file has that header
(G3 and G4 do, `1767268800.5`; G1 and G2 do not), with `TZ=UTC0`. `<user dir>` is a fresh copy, for each run, of the session's
`<golden>/<S>/user-before/` (G3 and G4 need the save, which `user-before/` has merged in). Frames: G1 1700, G2 9000,
G3 18000, G4 3000. The runner is `mh4u_runner.exe` from the Windows CI build of this branch (as `golden.md`'s commands
use) or `run.sh` from the `mh4u-runner-linux` artifact; `<game.cxi>` is the staged game, `<update.cia>` the 1.3.0 update.
The measured runs (section 5 has the runner and the library) were made by a driver script that does this for G1, G2, G4
and G3 in turn; it is kept with its log in `build/logs` (`n0-14-b`).

PowerShell (Windows), once per session `<S>` (G1 shown; G3 and G4 add `--init-time 1767268800.5`):

```
$env:TZ = "UTC0"
Copy-Item -Recurse <golden>/G1/user-before <out>/G1-user
<runner> <game.cxi> --new-3ds off --cpu recomp --code <code library> --install <update.cia> `
    --user <out>/G1-user --frames 1700 --input-file tools/native/golden/G1.txt `
    --check-coherence <out>/G1-coherence.log
python tools/native/coherence.py runlog <out>/G1-coherence.log --json <out>/G1-coherence-b.json --md <out>/G1-coherence-b.md --top 20
```

Linux, the same with `TZ=UTC0 <runner> ...` (`<runner>` is `run.sh` of the artifact), `cp -a` and `python3`. The runner
prints `check-coherence: <n> accesses ...` when it ends; a log without a `SUMMARY` line means the run stopped early.

### (a) from the captures, after (b)

PowerShell (on Linux, `python3` and `\` at the line ends):

```
python tools/native/coherence.py capture <golden>/G1/gpu-1500.mgpu <golden>/G2/gpu-2500.mgpu `
    <golden>/G3/gpu-17400.mgpu <golden>/G4/gpu-2400.mgpu --alias-log <out>/G1-coherence.log `
    --json <out>/coherence-a2.json --md <out>/coherence-a2.md --top 20
```

`--alias-log` takes the `ALIAS` runs of a part (b) log; the log of any session will do, since the four measured logs have
the same `ALIAS` lines. Without it the read-based coverage treats FCRAM as one region and does not map DMA sources or
flushes outside the GPU's windows. The report has the same-interval rule's rows and, per capture, `read_coverage`. It
reads each file once; the working set is the sparse page copy of FCRAM and VRAM (about as large as the capture).

### Cost

The game makes about 6 GPU events per frame, and with the check on each took 34 to 55 ms of wall time, emulation
included: G1 took about 6 minutes and G3, the longest, 1.6 h (4.2 has the measured table). Each GPU event costs two
passes over FCRAM and VRAM (134 MB on an Old 3DS), whatever it changes. Part (a) takes seconds (about 2 s for the four
captures). The estimate written before the runs (about 40 ms per event for the two passes on the development VM, with
the emulation on top; 20 to 100 events per frame; most of a day for G3) cannot be checked for the cost of the passes:
the measured times include the emulation, and there is no run without the check to subtract. Its clear error was the
number of events per frame.

## 4. Results

Measured on the maintainer's PC. This section has the counts, the call sites and the conclusion; the per-row and
per-access detail (uncovered ranges, every access line) stays in `build/n0-14`: `coherence-a2.md` and `.json` for (a),
`<S>-coherence.log`, `<S>-coherence-b.md` and `.json` for (b).

### 4.1 (a) What the GPU read of the CPU's changes, per capture

Captures: G1 frames 1500 to 1502 and G2 frames 2500 to 2502 (3 frames, 23 events each), G3 frames 17400 to 17409 and
G4 frames 2400 to 2409 (10 frames, 79 events each). Alias map: the 8 runs of `G1-coherence.log`.

What the capture windows hold: per frame two `SubmitCmdList`, two `DisplayTransfer`, two `CacheFlush` and two `SWAP`
(the first event of each window is a `SubmitCmdList`, so one `CacheFlush` fewer). They hold no `RequestDma`,
`TextureCopy`, `MemoryFill`, `MMIO` or `FILL`: texture uploads and their flushes are not exercised here. The CPU changed
no VRAM byte, and the colour buffers, the depth buffer and the screens the captures set are all in VRAM, so the reads
the model leaves out (colour and depth loads, the screen at `SWAP`) could not have taken a CPU change in these windows.

Read-based coverage (bytes are changed bytes the GPU read; a page or a pair is counted in the worst class of its bytes):

| | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| bytes the GPU read after a CPU change | 44 | 2,972 | 81,510 | 432,223 |
| of them covered with certain order | 0 | 2,972 (command lists) | 81,468 (command lists) | 425,281 (command lists) |
| of them covered in the same interval (order unknown) | 44 (vertex) | 0 | 42 (vertex) | 6,942 (vertex 6,184, index 758) |
| of them not covered | 0 | 0 | 0 | 0 |
| pairs: certain / same interval / not covered | 0 / 4 / 0 | 9 / 0 / 0 | 136 / 9 / 0 | 260 / 27 / 0 |
| pages: certain / same interval / not covered | 0 / 2 / 0 | 9 / 0 / 0 | 30 / 2 / 0 | 60 / 6 / 0 |
| pages read (all modelled reads) | 463 | 665 | 1,315 | 910 |

No byte changed in the first interval was read uncovered, no texture byte was changed and then read, and no chained
command buffer was jumped to. Every command list the GPU read after a change was flushed by the `CacheFlush` command just
before its `SubmitCmdList`, recorded in a later interval than the change: certain order. The vertex and index bytes
were covered only by a `FlushDataCache` recorded in the change's own interval, before the top screen's `SWAP`: the
capture cannot tell whether those stores came before the flush.

The same-interval rule, totals (per row in `coherence-a2.md`):

| | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| changed bytes | 3,699 | 14,955 | 276,381 | 620,157 |
| all changed bytes covered by a flush in the change's own interval | 54 (1.5%) | 0 | 46 (0.02%) | 7,477 (1.2%) |
| page changes (a page once per event): none / partial / full | 251 / 0 / 3 | 398 / 0 / 0 | 2,616 / 0 / 10 | 2,583 / 10 / 20 |

The rule's covered bytes are the read-based **same interval** bytes plus 10 / 0 / 4 / 535 bytes that a flush covered
but no read took before the capture ended (the flushed part of the pending bytes in a read range, below); a byte-level
comparison of the two answers on the four captures gives exactly these two sets. Principle 2's set gives the same
numbers (only `FlushDataCache` and `CacheFlush` regions were recorded), and every covered byte was covered by a
`FlushDataCache`. No changed page lies in the input a command's own words name, in the command's own interval: the
named-pages column has no page in any capture. The rule reads near zero for the reasons in section 1 (*Why the rule
reports most changes uncovered*).

Changed bytes by region:

| region | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| FCRAM linear | 373 | 5,003 | 102,250 | 499,790 |
| alias regions, all | 3,326 | 9,952 | 174,131 | 120,367 |
| - heap, `0x08000000` | 1,306 | 7,876 | 163,651 | 111,328 |
| - main thread's stack, `0x0ffc0000` | 1,360 | 1,386 | 7,606 | 6,463 |
| - program data, `0x00ec0000` | 483 | 499 | 2,171 | 1,863 |
| - GSP shared memory, `0x10002000` | 138 | 140 | 548 | 536 |
| - hid shared memory, `0x10000000` | 22 | 34 | 94 | 94 |
| - thread-local storage, `0x1ff82000` | 17 | 17 | 61 | 83 |
| VRAM | 0 | 0 | 0 | 0 |
| total | 3,699 | 14,955 | 276,381 | 620,157 |

The alias regions hold 90 / 67 / 63 / 19% of the changed bytes, and 91 / 83 / 89 / 64% of those the GPU did not read.
The code and read-only data runs (`0x00100000`, `0x00ded000`) never change. Where the changed bytes that the GPU did not
read ended:

| | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| replaced by a later CPU change | 2,672 | 7,101 | 95,346 | 99,078 |
| overwritten by the GPU (all in the GSP shared memory) | 5 | 5 | 21 | 21 |
| pending at the end, in a read range but changed after its last read | 10 | 1,477 | 9,258 | 47,113 |
| pending at the end, never in a read range | 968 | 3,400 | 90,246 | 41,722 |

The pending bytes in a read range may be data for a read after the capture ended (the capture shows none); 10 / 0 / 4 /
535 of them were flushed by then. Of the bytes never in a read range, 123 / 210 / 8,666 / 16,824 are in FCRAM linear,
the only region in which the GPU read a changed byte in these captures, and no flush covered any of them; what they are
(CPU-only data, DSP buffers, file data) the capture cannot tell.

Counts:

| | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| draws (of them indexed) | 155 (0) | 794 (63) | 3,650 (870) | 1,390 (575) |
| texture reads (units and cube faces) | 289 | 1,492 | 6,410 | 1,995 |
| `SubmitCmdList` (of them with `do_flush` set) | 6 (0) | 6 (0) | 20 (0) | 20 (0) |
| `CacheFlush` (of them flushing exactly the buffer of the next `SubmitCmdList`) | 5 (5) | 5 (5) | 19 (19) | 19 (19) |
| `FlushDataCache` records | 9 | 9 | 40 | 40 |

The `CacheFlush` region records are one per `CacheFlush` command. Chained command buffer jumps, `InvalidateDataCache`
and `StoreDataCache` records: none. No flush maps nowhere or goes through an alias run (so none named an address two
runs map to different memory), and no named read lies outside the GPU's windows.

### 4.2 (b) What the CPU reads of what the GPU wrote, per session

The four runs went from 2026-10-31 21:56 to 2026-11-01 00:44, local time, one after the other (G1, G2, G4, G3); each
ended with exit status 0 and a `SUMMARY` line.

GPU events, from `<S>-coherence-b.json` (events / pages changed / pages marked):

| GPU event | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| `RequestDma` | 22 / 110 / 69 | 56 / 472 / 255 | 218 / 1,392 / 392 | 114 / 770 / 392 |
| `SubmitCmdList` | 3,398 / 288,083 / 302 | 17,998 / 2,495,497 / 426 | 35,998 / 6,760,015 / 426 | 5,998 / 900,175 / 426 |
| `DisplayTransfer` | 3,397 / 28,017 / 257 | 17,997 / 240,868 / 257 | 35,997 / 627,849 / 257 | 5,997 / 108,641 / 257 |
| `CacheFlush` | 3,399 / 0 / 0 | 17,999 / 0 / 0 | 35,999 / 0 / 0 | 5,999 / 0 / 0 |
| `Mmio` | 68 / 0 / 0 | 68 / 0 / 0 | 68 / 0 / 0 | 68 / 0 / 0 |
| total | 10,284 / 316,210 / 628 | 54,118 / 2,736,837 / 938 | 108,280 / 7,389,256 / 1,075 | 18,176 / 1,009,586 / 1,075 |

No `MemoryFill`, `TextureCopy` or `Other` event. Rewritten (pages changed again while still marked) is pages changed
minus marked in every row. Unwatched with no window: 1 page in each session (the `SubmitCmdList` row); aliased: 0
everywhere. The log gives only the total of `Mmio` events; by inference they are the 68 register writes
(`WriteHWRegs` and `WriteHWRegsWithMask`) the game sends to gsp::Gpu in each golden `ipc.txt`, all in frame 0 (the last
at tick 50,658,926; frame 1 starts at tick 135,660,302 in the golden `state.txt`).

First accesses to a page the GPU wrote, by side and kind:

| | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| accesses | 19 | 162 | 844 | 265 |
| cpu `Write` | 0 | 1 | 1 | 1 |
| cpu `Read`, `BlockRead`, `CopySource` (loads) | 0 | 0 | 0 | 0 |
| cpu `Pointer` | 0 | 0 | 0 | 0 |
| cpu, other kinds | 0 | 0 | 0 | 0 |
| gpu `BlockWrite` | 19 | 161 | 843 | 264 |
| gpu, other kinds | 0 | 0 | 0 | 0 |
| passed over (`foreign`, `no_window`, `outside_run`, `other_thread`; section 2, *The log*) | 0 | 0 | 0 | 0 |

**CPU loads of GPU-written pages: none logged** on a watched page in the four sessions. The one GPU-written page the
check could not watch, the GSP shared memory, is read by the game by design (*Coverage*, below). **Raw pointers: none.**

**CPU stores: one in each of G2, G3 and G4**, none in G1. Each is a 4-byte `Write` with LR `0x00bfab90` into the page at
physical `0x200a1000`: in G2 at frame 1802, to the page's first byte (virtual `0x300a1000`); in G3 and G4 at frame 904,
to physical `0x200a1008` (virtual `0x300a1008`). The page was marked by a `DisplayTransfer` in frame 0 (mark 71, at the
same tick in the three sessions), and the GPU did not write it between then and the store (the line names that mark as
the page's latest writer).

- **The site.** `0x00bfab90` is the return address of the `bl` at `0x00bfab8c` in `f_00bfa7ec` (from `0x00bfa7ec` up to
  `0x00bfb8fc` in the function table) to `f_00bf60b8`, a leaf that never writes R14; neither function has a tail
  branch. So the store was most likely made in `f_00bf60b8` or in `f_00bfa7ec` after that call returned (section 2: the
  LR cannot tell which). If `f_00bfa7ec` returned after that call without writing R14 again (it returns with a `pop`
  that loads PC, and on some paths it uses LR as a scratch register after the call, from `0x00bfac50`), the store may
  also have been made further up the call chain; the paths after the call were not traced. `f_00bfa7ec` holds PICA
  command headers (of registers such as `0x0227`, `0x0245`, `0x025e`), so it builds command packets; it and its leaf are
  most likely the game's command-list encoder. Where their stores land was not traced.
- **The page.** In the captures the game's two command buffers start at virtual `0x3008d990` and `0x3018e7c0` (the
  `SubmitCmdList` addresses; each holds a frame's two lists back to back, and the two buffers alternate). The page lies
  `0x13670` bytes into the first, so a list reaches it only in a frame whose lists are longer than that: in the G3 and
  G4 captures they are, and the CPU writes the page every other frame; in the G1 and G2 captures they are not. The
  layout at frames 904 and 1802, when the stores were made, is not captured.
- **What it means.** Most likely the game writing a command word into its command list, in memory that a frame-0
  `DisplayTransfer` had written once. No load of a GPU result was logged: no load of the page was logged before the
  store. After the store the page is no longer watched until the GPU writes it again. In G3 and G4, a later GPU write
  would have marked it again, and the CPU's next store (every other frame in the captures) would have logged a second
  line; there is none. A read of the rest of the page right after the store would not be seen (section 2). In the
  captures the page's reader is the GPU, through `SubmitCmdList`, after a `CacheFlush` of the list.

**GPU-side accesses:** 19 / 161 / 843 / 264 (1,287 in all), every one a `BlockWrite` made inside a `RequestDma`, to VRAM
between physical `0x18154880` and `0x184c0000` (the bytes the lines name; a block access is logged per page, so each
`size` is the part of the DMA in that page), on 15 / 115 / 276 / 194 distinct pages, with LR `0x0056cd64`. That LR is
the return address of the call in `f_0056cc80` to `f_0056d9dc`, a leaf stub that sends gsp::Gpu command `0x000c`
(`TriggerCmdReqQueue`) with svc `0x32`: the game's call site that submits GX commands. The accessed page had last been
written by a `RequestDma` in 1,284 cases and by a `SubmitCmdList` in 3 (G3 2, G4 1). No `CopySource` was logged: no
DMA read was the first GPU-side access to a page since the last GPU write that changed it, so no DMA read-back of render
output was seen. (A GPU-side access is logged at most once after each GPU write that changed the page, so a DMA read
that came after another GPU-side access to the page, for example after a DMA that rewrote it with the same bytes, is
not seen.) By start offset:

- 19 / 53 / 207 / 111 accesses start mid-page: the page already held something else. 19 / 42 / 169 / 90 of them come
  within one frame of the page's previous GPU write: a second DMA into the same page soon after the first. The rest come
  later (up to 5012 / 11705 / 1565 frames later in G2 / G3 / G4), into a page written long before. Among them are the
  three accesses to a page a `SubmitCmdList` wrote last, which start exactly where a colour buffer ends in the layout
  the captures show: `0x184a5400` after the 64x128 RGBA8 buffer at `0x1849d400` (G3 and G4, frame 1771, page written
  at frame 1768), and `0x18194f00` after the 256x256 RGBA8 buffer at `0x18154f00` (G3, frame 13493, page written at
  frame 1788). Those frames are not captured.
- 0 / 108 / 636 / 153 accesses start on a page boundary, at least 59 / 664 / 760 frames (G2 / G3 / G4) after the page's
  previous GPU write: DMAs that refill VRAM an earlier DMA filled.

The accesses come in bursts. With gaps of at most 5 frames grouped, G3's fall at frames 1, 69-76, 907-917, 1634, 1655,
1662-1669, 1716-1748, 1763-1773, 9184-9187, 9199-9202, 9212-9214, 13493-13523, 14159-14172, 14214 and 14242; G4's are
G3's groups up to frame 1773.

**Coverage:** `UNWATCHED kind=nowindow`, `0x26c00000`, 1 page, 1 mark, in all four sessions; no `kind=aliased`.

- **What the page is.** The GSP shared memory: Azahar's gsp::Gpu creates 0x1000 bytes of shared memory in the BASE
  region (`gsp_gpu.cpp`), which starts at FCRAM + `0x06c00000` in memory layout 0 (the Old 3DS layout with 64 MB for
  the application; `core/hle/kernel/memory.cpp`). The game maps it at `0x10002000` in frame 0
  (`RegisterInterruptRelayQueue`, then `svcMapMemoryBlock` at tick 49,548,949 in the golden G1 recording).
- **Its flag.** It shows up in the `ALIAS` lines only from frame 600, and it was marked before that scan, so its flag is
  `aliased=0` (the stale flag of section 2); it is counted as `nowindow` either way.
- **Who changes it.** During a `SubmitCmdList` or a `DisplayTransfer`, Azahar's HLE GSP pushes an interrupt into the
  thread's interrupt relay queue at the start of the page (`gsp_interrupt.h`: the count at byte `0x1`, the slots from
  `0xc` to `0x3f`): P3D when a command list ends, PPF when a transfer ends. In the G4 capture the page is in the
  `GPU_PAGES` of all 20 `SubmitCmdList` and all 20 `DisplayTransfer` commands and of none of the 19 `CacheFlush`
  commands. Outside GPU commands the page changes too: the queue's index and count at `0x0` and `0x1` and more slot
  bytes, the framebuffer information from `0x200`, and the GX command queue from `0x800`. The slots and the count
  change because Azahar pushes PDC0 and PDC1 at each VBlank (one of each per frame in the captures' `IRQ` records). The
  index is moved by the queue's reader: Azahar's push never writes it.
- **What the port must do.** The page is the game's channel to the GSP service, not render output. The port's GSP
  replacement must keep this contract in guest memory with plain CPU writes: push the completion interrupts (P3D after
  a command list, PPF after a transfer, DMA after a `RequestDma`, PSC0 or PSC1 after a `MemoryFill`, which these
  sessions do not use) and PDC0 and PDC1 at each VBlank, and read the GX command queue and the framebuffer information.
  The renderer needs no write-back for it.

**Pending:** 628 / 937 / 1,074 / 1,074 pages were still marked at the end: every mark except the one page the CPU store
unmarked in G2, G3 and G4.

**Run time:**

| | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| frames | 1700 | 9000 | 18000 | 3000 |
| GPU events per frame | 6.05 | 6.01 | 6.02 | 6.06 |
| wall time | 350 s | 2,911 s | 5,811 s (1.6 h) | 1,004 s |
| per frame | 0.206 s | 0.323 s | 0.323 s | 0.335 s |
| per GPU event | 34.0 ms | 53.8 ms | 53.7 ms | 55.2 ms |

The times include the emulation and the software renderer. Another job on the PC (a check of another package, under a
minute) ran during the last minutes of the G4 run (23:04 to 23:05; G4 ended at 23:07).

**Validity.** Each run played its golden session:

- the final emulated time equals the golden recording's: 31,672.5 / 154,718.6 / 304,072.1 / 52,932.3 ms;
- every (frame, ticks) pair in the log (the `f`/`t` and `mark_f`/`mark_t` of the `ACCESS` lines) is consistent with the
  golden `state.txt` (its tick lies after the golden first sighting of the frame before and no later than that of the
  frame after): 38 of 38, 324 of 324, 1,688 of 1,688 and 530 of 530;
- the emulator's message sequence is the golden run's (40 / 49 / 107 / 54 messages), with no `UNREACHABLE`, assertion or
  interpreter message;
- the code library is the paged build (`rc_build_mode` 0), whose generated code reaches guest memory only through the
  `rd*`/`wr*` accessors (1,616,418 call sites; no direct pointer). For a page whose host pointer is null, as a marked
  page's is, they take the slow path, `ARM_Recomp::SlowRead`/`SlowWrite`, which calls `memory.Read*`/`Write*` and so
  reaches the hook;
- every `RasterizerCachedMemory` data case of `memory.cpp` has the hook (12 call sites, section 2).

### 4.3 Conclusion

Nothing measured contradicts principle 2. Part (a) covers the four capture windows (3 and 10 frames: command lists,
vertex and index data, no texture upload); part (b) covers the whole of the four sessions.

- **(a)** In the captured windows every byte the GPU read after a CPU change had a flush recorded before the read: with
  certain order for the command lists, each flushed by the `CacheFlush` before its `SubmitCmdList`, and in the same
  interval, order unknown, for 44 / 0 / 42 / 6,942 bytes of vertex and index data. No range was found that needs
  content hashing, provided the renderer reads vertex and index data from guest memory at every draw (below).
- **(b)** No CPU load of a watched GPU-written page was logged, and no raw pointer to one was taken. The one CPU store
  in each of G2, G3 and G4 is most likely the game reusing, for its command list, memory that a frame-0
  `DisplayTransfer` once wrote; no load of GPU output was logged there. The one GPU-written page the check could not
  watch, the GSP shared memory, is read by the game by design: it is a service contract that the port's GSP
  replacement must keep with plain CPU writes, not render output, and it needs no renderer write-back.

Limits:

- The windows of (a) are short: 3 and 10 frames per capture, against 1700 to 18000 frames in (b).
- The captures hold no `RequestDma` or `TextureCopy` (the (b) runs have 22 / 56 / 218 / 114 `RequestDma`), so (a) says
  nothing about texture uploads and their flushes; no texture byte was changed and then read.
- For the same-interval bytes (vertex and index data, 6,942 bytes at most in a capture) the capture cannot tell whether
  the store came before the flush.
- Flushes outside gsp::Gpu (`svcFlushProcessDataCache`, the DSP service's) are not recorded; they matter only for
  changes no modelled read took.
- The alias map of (a) comes from a (b) run, not from the golden runs that made the captures.
- (b) covers the code paths of the four golden sessions only: 31.7 s, 154.7 s, 304.1 s and 52.9 s of emulated time,
  about 9 minutes in all. What each session plays is in [`../native/golden.md`](../native/golden.md); parts of the game
  they do not reach (another menu or quest type, a feature that reads a rendered image back) are untested.
- The limits of (b) (section 2): HLE code that reads by physical pointer (the DSP HLE, the AAC decoder, the kernel's
  shared-memory backing) and the Luma alias path are not seen; a store unmarks its page; GPU writes that land after
  `Execute` or `WriteReg` return are untested; the `aliased` flag can be stale; a GPU-side access is logged at most
  once per mark (4.2).

For the renderer:

- **Invalidate a `RequestDma` destination by byte range.** All 1,287 GPU-side accesses are DMA writes into VRAM the GPU
  had written before (by a DMA, or, three times, by a command list, apparently the tail of a colour buffer), and
  page-aligned DMAs refill VRAM an earlier DMA filled, in bursts many frames apart. The command names the range, so no
  hashing is needed: whatever the renderer cached from those bytes (presumably textures) is dropped when a DMA rewrites
  them. Azahar invalidates the destination region the same way.
- **VRAM is packed byte-tight.** Two DMA destinations start exactly where a colour buffer ends, consistent with the
  layout the captures show, so invalidation works by bytes, and any write-back of colour buffers into guest VRAM must
  not be page-granular, or it would overwrite what a DMA put in the shared page.
- **Read vertex and index data at every draw.** Do not cache them until the next flush: for up to 6,942 bytes per
  capture the flush may have come before the store. Azahar's OpenGL renderer copies the vertex and index arrays from
  guest memory at every draw (`gl_rasterizer.cpp`, `AccelerateDrawBatch`).

Possible follow-up: captures over windows that contain `RequestDma` (for example at the frames where the (b) runs show
DMA bursts), to test texture uploads and their flushes in (a).

## 5. Provenance

`tools/native/coherence.py` takes these from Azahar, all at `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6`
(GPL-2.0-or-later; the tool's header lists them):

- the fixed virtual-to-physical mapping, from `GPU::VirtualToPhysicalAddress` (`src/video_core/gpu.cpp`), and the
  windows' constants, from `src/core/memory.h`;
- the GX command arguments, from `src/core/hle/service/gsp/gsp_command.h`, and the display-transfer fields, from
  `src/video_core/pica/regs_external.h`;
- for the read model: the command buffer registers a `SubmitCmdList` sets (`gpu.cpp`); the positions and fields of the
  PICA registers a draw or a command buffer jump reads (`src/video_core/pica/regs_internal.h`, `regs_texturing.h`,
  `regs_pipeline.h`) and what those triggers do (`pica_core.cpp`); the vertex span of a draw
  (`src/video_core/rasterizer_accelerated.cpp`); the texture units a draw binds
  (`src/video_core/renderer_opengl/gl_rasterizer.cpp`, `src/video_core/rasterizer_cache/rasterizer_cache.h`); and the
  bits per pixel and the mip levels of a texture (`src/video_core/rasterizer_cache/pixel_format.h`, `utils.cpp`,
  `surface_params.cpp`).

The runner's check reads `memory.h` and `memory.cpp` (`RasterizerMarkRegionCached`,
`PhysicalToVirtualAddressForRasterizer`, the `RasterizerCachedMemory` cases) and `gpu.cpp` (`Execute`, `WriteReg`); those
files are already named in the header of the runner's GPU capture section.

The findings of sections 2 and 4 also rest on these Azahar files, read at the same commit (facts only, no code copied):

- `src/core/memory.cpp`: both linear windows, the Luma path;
- `src/core/hle/service/gsp/gsp_gpu.cpp` and `gsp_interrupt.h`: the shared memory, its layout, and when interrupts are
  pushed;
- `src/core/hle/kernel/memory.cpp`: the memory regions;
- `src/video_core/gpu.cpp` and `src/video_core/pica/pica_core.cpp`: `RequestDma`, and the interrupts each command
  signals;
- `src/video_core/renderer_opengl/gl_rasterizer.cpp`: the vertex and index copy at every draw;
- the files with physical-pointer readers named in section 2.

The part (b) runs used `mh4u_runner.exe` from the `mh4u-port-windows` artifact of GitHub Actions run 41273011879
(workflow `windows.yml`, branch `native`, head commit `4a70af0bca8df606334c343eba486350969ca0f9`; sha256 prefix
d57964c6) and the code library `build/golden-lib-v6/recomp/0004000000126300.dll` (the paged build, `rc_build_mode` 0;
sha256 prefix 6952d2a7), not the golden recordings' library (sha256 prefix e95eae1f). The checks of 4.2 (identical
final emulated times and message sequences, every logged (frame, ticks) pair consistent with the golden trace) show
they behave the same on these sessions. Part (a) read the golden captures `build/golden/<S>/gpu-<start>.mgpu`, with the
alias runs of `build/n0-14/G1-coherence.log`.
