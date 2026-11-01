# Flat-memory coherence check (N0-14)

Evidence for principle 2 of the native port ([`../native/ROADMAP.md`](../native/ROADMAP.md)): the game's memory can be one
flat block that the CPU and the GPU both use in place, with no copy and no cache flush. Two things would break it, and
this check measures both:

- **(a)** the GPU uses data the CPU changed, and no cache flush covers it (so on the 3DS the data may never have reached
  the GPU, and the renderer would have to find out what changed by itself, for example by hashing);
- **(b)** the CPU reads something the GPU wrote (so the renderer would have to write its results back into guest memory
  where the CPU can see them).

**Status: the tools are written and tested on synthetic data; nothing has been measured.** The result tables below are
empty on purpose: the counts, the call sites and the conclusion are written by the session on the maintainer's PC, from the
game files and the golden recordings, which this repository does not have. Every table says "to be measured on the PC".

| | Tool | Input |
|---|---|---|
| (a) | `python3 tools/native/coherence.py capture FILE.mgpu ...` | the GPU captures of G1 to G4 (`build/golden/<S>/gpu-<start>.mgpu`) |
| (b) | `mh4u_runner --check-coherence FILE` then `python3 tools/native/coherence.py runlog FILE` | a run of each golden session |

Everything below `build/` is game data: captures, logs and reports with addresses stay there; this document holds counts
only.

## 1. Part (a): from a capture

A capture (`docs/native/mgpu-format.md`) has, before every GPU event (GX command, register write `MMIO`, `SWAP`, `FILL`),
a `CPU_PAGES` record with the pages whose contents differ from the shadow, that is, the pages that changed since the
previous diff; `FLUSH` records with the game's FlushDataCache, InvalidateDataCache and StoreDataCache requests (virtual
addresses, from the IPC recorder) and the regions of CacheFlush GX commands; and `GPU_PAGES` after a command (what the GPU
wrote). `coherence.py capture` streams the file (a capture is 140 to 175 MB) and, for each event:

1. takes the pages of its `CPU_PAGES` record and, for each, the bytes that differ from the page's previous contents
   (`MEM`, `CPU_PAGES` and `GPU_PAGES` applied in order, so a page the GPU wrote is compared with what the GPU wrote);
2. takes the `FLUSH` records recorded since the previous event's end (its `GPU_PAGES` position: the shadow is brought up to
   date there) up to this event, converts their virtual ranges to physical ones with the fixed mapping of Azahar's GPU
   (`VideoCore::GPU::VirtualToPhysicalAddress`: VRAM `0x1F000000`, the linear heap `0x14000000` and the new linear heap
   `0x30000000` onto FCRAM; the constants are in `coherence.py`), and counts how many of the changed bytes lie inside
   them. A CacheFlush command's own regions (recorded right after its `GX` record) count for its own `CPU_PAGES`;
3. classes each page as **full** (every changed byte covered), **partial** or **none**.

Reported per row (the GX command kinds of `gcap.GX_NAMES`, and `MMIO`, `SWAP`, `FILL` as rows of their own, because
`CPU_PAGES` precede them as well): events, pages, changed bytes, the share of the changed bytes covered, the page classes,
and the uncovered ranges grouped (consecutive pages). The coverage is shown for any flush, for each kind of flush alone
(`FlushDataCache`, `InvalidateDataCache`, `StoreDataCache`, `CacheFlushRegion`) and for the set principle 2 names
(`FlushDataCache` and the CacheFlush regions). A flush whose address maps nowhere in the fixed mapping (a heap address,
say) is counted apart (`maps nowhere`) and covers nothing: the game may flush linear memory through a heap alias, which
this cannot see.

**Approximations, all stated by the tool's output or here:**

- **The first interval.** The interval before the first event follows `MEM`: a flush the game made before the capture
  started is not in the file, so the first event's pages are reported as a row of their own, `(first interval)`, and read
  as an upper bound on what is uncovered.
- **A change after the flush.** `CPU_PAGES` is taken lazily, just before the next event, so a page in it changed at some
  point after the previous diff. A flush recorded in that interval is taken to cover the page, even if the CPU changed it
  after the flush; the capture cannot tell the two apart. The numbers therefore under-count the uncovered changes.
- **Who changed the page.** A page "changed by the CPU" is any page that changed outside a GPU command: the game's own
  stores, but also the kernel's and the services' (file reads into linear buffers, the camera, the DSP).
- **A flush's range, not the cache.** A flush counts as covering its virtual range; whether the data had reached memory is
  not observable here (the emulator has no cache).
- **The subset a command names (optional column).** "Named pages not fully covered" counts the pages of the uncovered set
  that the command's own words say it reads: the command list buffer (`SubmitCmdList`), the source of `RequestDma`, the
  input of `TextureCopy` (`size` bytes, an upper bound: the line gaps are ignored) and of `DisplayTransfer` (width x height x
  bytes per pixel from its flags); a fill reads nothing. A command list also reads the textures, vertices and shaders its
  register writes point at, and those addresses are in the `PICA_REG` records, not in the words, so this subset is a lower
  bound on what the GPU reads.

## 2. Part (b): `mh4u_runner --check-coherence FILE`

### Method

The runner keeps a shadow of all of FCRAM and VRAM (as `--capture-gpu` does, and with the capture's own page diff,
`GcapChangedPages`). Around every `GPU::Execute` and `GPU::WriteReg`:

1. **before the command**, the CPU's changes are taken into the shadow (pass 1), so that the diff after it holds only what
   the command changed;
2. **after it**, the diff (pass 2) is the set of pages the GPU changed. Each page that is not already marked is marked with
   `RasterizerMarkRegionCached(paddr, size, true)`, in runs. The runner keeps its own set of marked pages: Azahar hits
   `UNREACHABLE()` when a page that is already rasterizer-cached is marked again, or when a plain page is unmarked, and the
   software renderer marks nothing itself. A page the GPU wrote again while still marked stays marked and keeps the latest
   writer (counted as `rewritten`).

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
wrote the page. A **GPU-side** access (see next paragraph) is logged, once per mark, and **leaves the page marked**, so a
GX DMA that reads or writes a GPU-written page does not hide a later CPU read of it from the check. A page the GPU writes
again while it is still marked keeps its mark and gets the new writer (`rewritten`), and its GPU-side access is logged again.

**CPU or GPU.** Not every access through that path is the CPU's. GX `RequestDma` is executed in `GPU::Execute` with
`memory.CopyBlock` through the application's page table. An access made while the runner is inside a GPU hook (`Execute`,
`WriteReg`) is tagged `side=gpu` and counted apart; `side=cpu` is everything else that happens while the emulation loop
runs (recompiled code and the services it calls). Accesses by the runner itself between slices (the frame counter, dumps,
the state trace) and by other host threads are counted in the summary and not logged. Inside the hooks the runner reads the
frame counter through the process's page-table pointer, never through `memory.Read*`.

### Coverage: what cannot be watched

`PhysicalToVirtualAddressForRasterizer` maps FCRAM only to the linear heap (`0x14000000`, and `0x30000000` on a New 3DS
layout) and VRAM to `0x1F000000`. A page the GPU wrote can be unreachable through those windows in the game process (it
maps the memory only elsewhere), or reachable through them and through another address too (a shared memory block, the
application heap over the same FCRAM). Marking changes nothing for the second address. The runner reports both:

- after marking, it looks at the game process's page table: a page for which none of its windows became rasterizer-cached
  is logged as `watched=0` and grouped at the end as `UNWATCHED kind=nowindow`;
- at the start, every 600 game frames and at the end it scans the page table for pages that map FCRAM or VRAM outside the
  windows (`ALIAS` lines when the set changes); GPU-written pages in that set are `aliased=1` and grouped as
  `UNWATCHED kind=aliased`: a CPU access to those pages through the other address is invisible.

Partial coverage is a finding to report, not a failure of the run.

### The guest LR, and its limit

The line carries the running core's LR (`ctx->r[14]` on the recompiler), `guest=1` when a guest thread is running and
`lr=-` when none is. Recompiled functions keep their registers in C locals and write them back to `ctx` only at calls,
system calls, traps and returns (`tools/recomp/emit.py`, `_with_locals`: `RC_FLUSH` before each, `RC_RELOAD` after a call);
a function that never writes R14 never writes it back either. At a slow-path load or store `ctx->r[14]` is therefore the
value of the last write-back, which means:

- in a **leaf function**, or in any function **before its first call**, it is the return address left by the caller: the LR
  names the caller's call site (the usual case for `memcpy`-like code);
- **after the function has made a call** (a `bl` writes its return address to R14 and flushes it), it is the address just
  after that `bl`, a place **inside the same function**, not its caller; the same after a system call that followed a
  write to R14;
- it is **stale** when the function wrote R14 itself since the last write-back (`pop {..., lr}`, `mov lr, ...`,
  `ldr lr, ...`): `ctx` still holds the older value.

So no caller chain is recovered, and the LR names the caller only in the first two cases. To identify a site, take the
function that **contains** the LR (the function table of the recompiled code), not the LR itself: for a leaf it is the
caller of the function that accessed the memory, for the other cases it is the function that did. For a block copy or a
pointer taken by a service, the LR is the thread's at its system call (the stub's return address: the game's call site of
the service function). The recompiler is not changed for this.

### What the check does not see

- **A write unmarks the page.** After a CPU store to a page (even a few bytes) the page is plain again, so a later read of
  the rest of the page, which still holds GPU data, is not seen. The log has the size of every store; stores smaller than a
  page into a GPU-written page are therefore the place to look for a hidden dependency.
- **A GPU write that lands after the command returns.** The diff assumes the software renderer has finished its writes when
  `GPU::Execute` or `GPU::WriteReg` returns, as the capture does (untested until the dump, N0-05); a late write would show
  up as a CPU change in the next pre-pass and never be marked.
- **A raw pointer** (`Pointer`) unmarks one page and says nothing about the access made through it.
- **Aliases and windows** (above), and pages marked in another process's page table than the game's.
- **Reads that the hook does not reach**: none are known; every `RasterizerCachedMemory` case of `memory.cpp` has the hook
  (the tests check the patch for that). A kind of access that bypassed the path would make the check unable to support a
  conclusion for it.
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

A text file, one record per line, `TAG key=value ...`, numbers in hex written `0x` and eight digits, parsed by
`coherence.py runlog` (lines starting with `#` are skipped). First line:
`# coherence v1 <date> game=<title id> cpu=<jit|recomp|interp> new3ds=<on|off>`.

| Line | Fields | When |
|---|---|---|
| `ACCESS` | `n` (order), `f` (game frame), `t` (ticks since the run started), `side` (`cpu`, `gpu`), `kind` (table above), `paddr`, `vaddr`, `size`, `guest` (0 or 1), `lr` (hex or `-`), `in` (the GPU event the access was made inside, `-` for the CPU), `mark` (sequence number of the writing event), `mark_ev`, `mark_f`, `mark_t` (its kind, frame and ticks), `watched`, `aliased` | the first access to a marked page |
| `ALIAS` | `f`, then `runs`, or `vaddr`, `paddr`, `pages` per run | when the set of unwatchable runs changes |
| `EVENT` | `ev` (`RequestDma`, `SubmitCmdList`, `MemoryFill`, `DisplayTransfer`, `TextureCopy`, `CacheFlush`, `Mmio`, `Other`), `n`, `gpu_pages`, `marked`, `rewritten`, `unwatched`, `aliased` | end of run: totals per event kind |
| `UNWATCHED` | `kind` (`nowindow`, `aliased`), `paddr`, `pages`, `marks` | end of run: grouped ranges |
| `PENDING` | `pages` | end of run: pages still marked (the CPU never touched them) |
| `SUMMARY` | `first_frame`, `last_frame`, `events`, `gpu_pages`, `marked`, `rewritten`, `unwatched`, `aliased`, `accesses`, `cpu`, `gpu`, `pending`, `cpu_pages_changed`, `foreign` (an access to a page the checker had not marked), `no_window`, `outside_run`, `other_thread` | the last line; a log without it is from a run that did not finish |

The report groups the accesses by side and kind, by the GPU event that wrote the page, and the CPU's by LR (the call
sites), with the delay between the write and the first load.

## 3. Commands for the PC

Inputs are placeholders. `<golden>` is the shared checkout's `build/golden`; `<capture>` below is one of
`<golden>/G1/gpu-1500.mgpu`, `<golden>/G2/gpu-2500.mgpu`, `<golden>/G3/gpu-17400.mgpu`, `<golden>/G4/gpu-2400.mgpu`.
All outputs go to `<out>`, a folder under `build/`.

### (a) from the captures

```
python3 tools/native/coherence.py capture <golden>/G1/gpu-1500.mgpu <golden>/G2/gpu-2500.mgpu \
    <golden>/G3/gpu-17400.mgpu <golden>/G4/gpu-2400.mgpu --json <out>/coherence-a.json --md <out>/coherence-a.md --top 20
```

It reads each file once; the working set is the sparse page copy of FCRAM and VRAM (about as large as the capture).

### (b) the reference runner on each session

Run as `golden.py record` runs it (`docs/native/golden.md`), without the recorders and with `--check-coherence`:
`--new-3ds off --cpu recomp --code <code library> --install <update.cia> --user <user dir> --frames <the session's # frames>
--input-file tools/native/golden/<S>.txt`, plus `--init-time <the session's # init-time:>` when the file has that header
(G3 and G4 do, `1767268800.5`; G1 and G2 do not), with `TZ=UTC0`. `<user dir>` is a fresh copy, for each run, of the session's
`<golden>/<S>/user-before/` (G3 and G4 need the save, which `user-before/` has merged in). Frames: G1 1700, G2 9000,
G3 18000, G4 3000. The runner is `mh4u_runner.exe` from the Windows CI build of this branch (as `golden.md`'s commands
use) or `run.sh` from the `mh4u-runner-linux` artifact; `<game.cxi>` is the staged game, `<update.cia>` the 1.3.0 update.

PowerShell (Windows), once per session `<S>` (G1 shown; G3 and G4 add `--init-time 1767268800.5`):

```
$env:TZ = "UTC0"
Copy-Item -Recurse <golden>/G1/user-before <out>/G1-user
<runner> <game.cxi> --new-3ds off --cpu recomp --code <code library> --install <update.cia> `
    --user <out>/G1-user --frames 1700 --input-file tools/native/golden/G1.txt `
    --check-coherence <out>/G1-coherence.log
python tools/native/coherence.py runlog <out>/G1-coherence.log --json <out>/G1-coherence-b.json --md <out>/G1-coherence-b.md --top 20
```

Linux, the same with `TZ=UTC0 <runner> ...` (`<runner>` is `run.sh` of the artifact) and `cp -a`. The runner prints
`check-coherence: <n> accesses ...` when it ends; a log without a `SUMMARY` line means the run stopped early.

### Cost

Every GPU event costs two passes over FCRAM and VRAM (134 MB on an Old 3DS, `memcmp` of every page against the shadow, and
a copy of each page that differs), whatever the event changes: one before the command and one after. A standalone
`memcmp` loop over 134 MB takes about 18 to 22 ms on the 4-core development VM, so about 40 ms per GPU event there; the PC
will differ, and the emulation itself is on top. The number of events per game frame is not known until a capture is
summarised (`gcap.py summary` counts the GX commands; register writes are the `MMIO` records): at 20 events per frame
that is about 0.8 s per frame, at 100 about 4 s. By that arithmetic G1 (1700 frames) is minutes to an hour or two, G2 (9000)
hours, and **G3 (18000 frames) is the long one**: plan for most of a day, or run it unattended, and keep the PC from
sleeping. G4 (3000) is in between. These are estimates from the pass cost, not measurements; the first G1 run gives the real
figure, which belongs in the results below.

## 4. Results

**To be measured on the PC.** Do not fill these in from anywhere else. Counts, addresses of call sites and the conclusion
are written by the session that has the dump.

### 4.1 (a) Changes the CPU made that no flush covers, per capture

For each of G1, G2, G3, G4, one table (the capture's start frame and frame count as in `gcap.py summary`):

| Row (GX command or event) | Events | CPU pages | Changed bytes | Covered (any flush) | Covered (FlushDataCache + CacheFlush regions) | Pages none / partial / full (any) | Pages none / partial / full (principle 2's set) | Named pages not fully covered |
|---|---|---|---|---|---|---|---|---|
| (first interval) | to be measured on the PC | | | | | | | |
| REQUEST_DMA | to be measured on the PC | | | | | | | |
| SUBMIT_CMDLIST | to be measured on the PC | | | | | | | |
| MEMORY_FILL | to be measured on the PC | | | | | | | |
| DISPLAY_TRANSFER | to be measured on the PC | | | | | | | |
| TEXTURE_COPY | to be measured on the PC | | | | | | | |
| CACHE_FLUSH | to be measured on the PC | | | | | | | |
| MMIO | to be measured on the PC | | | | | | | |
| SWAP | to be measured on the PC | | | | | | | |
| FILL | to be measured on the PC | | | | | | | |
| total | to be measured on the PC | | | | | | | |

Changed bytes covered, per kind of flush alone:

| Row | FlushDataCache | InvalidateDataCache | StoreDataCache | CacheFlushRegion |
|---|---|---|---|---|
| total | to be measured on the PC | | | |

Flush records in the capture: FlushDataCache, InvalidateDataCache, StoreDataCache, CacheFlushRegion, and how many map
nowhere: to be measured on the PC.

Largest uncovered ranges (physical address, pages) per row: to be measured on the PC.

### 4.2 (b) What the CPU reads of what the GPU wrote, per session

For each of G1, G2, G3, G4 (frames, events and pages as in the log's `SUMMARY`):

| GPU event | Events | Pages changed | Marked | Rewritten | No window | Aliased |
|---|---|---|---|---|---|---|
| RequestDma | to be measured on the PC | | | | | |
| SubmitCmdList | to be measured on the PC | | | | | |
| MemoryFill | to be measured on the PC | | | | | |
| DisplayTransfer | to be measured on the PC | | | | | |
| TextureCopy | to be measured on the PC | | | | | |
| Mmio | to be measured on the PC | | | | | |

First access to a page the GPU wrote, by side and kind:

| Side / kind | Read | Write | WriteExclusive | BlockRead | BlockWrite | ZeroBlock | CopySource | Pointer |
|---|---|---|---|---|---|---|---|---|
| cpu | to be measured on the PC | | | | | | | |
| gpu | to be measured on the PC | | | | | | | |

CPU loads of GPU-written pages, by call site (the LR; with the function it falls in and the writing GPU event):

| LR | Accesses | Bytes | Kinds | Writing events | Function |
|---|---|---|---|---|---|
| to be measured on the PC | | | | | |

CPU stores to GPU-written pages (smaller than a page), by call site: to be measured on the PC.

Raw pointer call sites: to be measured on the PC.

Coverage (pages the checker could not watch): `UNWATCHED` ranges of kind `nowindow` and `aliased`: to be measured on the PC.

Run time of the check per session (the real figure for the cost estimate above): to be measured on the PC.

### 4.3 Conclusion

To be written on the PC: either principle 2 holds (no CPU load of GPU-written memory outside the call sites listed as
harmless, and every CPU change the GPU uses covered by a flush or by a mechanism the renderer provides), or the exceptions
the renderer must handle, each with its call site and its pages: for example ranges that need content hashing because the
CPU changes them with no flush, or a call site that reads back a GPU result so that the renderer must write it into guest
memory.

## 5. Provenance

`tools/native/coherence.py` takes its fixed virtual-to-physical mapping from Azahar's `GPU::VirtualToPhysicalAddress`
(`src/video_core/gpu.cpp`) and the windows' constants from `src/core/memory.h`, the GX command arguments from
`src/core/hle/service/gsp/gsp_command.h` and the display-transfer fields from `src/video_core/pica/regs_external.h`, all at
Azahar `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` (GPL-2.0-or-later). The runner's check reads `memory.h` and `memory.cpp`
(`RasterizerMarkRegionCached`, `PhysicalToVirtualAddressForRasterizer`, the `RasterizerCachedMemory` cases) and `gpu.cpp`
(`Execute`, `WriteReg`); those files are already named in the header of the runner's GPU capture section.
