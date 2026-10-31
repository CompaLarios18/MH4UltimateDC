# The capture player (N2-02)

`mh4n_capture_player` replays an `MGPU` capture ([`mgpu-format.md`](mgpu-format.md)) into any
`mh4n::gpu::Gpu` and checks the result against the capture: the memory each command wrote, the
register writes of each command list and the displayed screens of each frame. It times every
frame. The renderer of N2 is built and checked with it, without the runtime.

The logic is the module `native/src/gpu_capture/` (`mh4n_gpu_capture`); the program in
`native/tools/capture_player/` is only `main`. The tests are `native/tests/capture_player_test.cpp`;
every capture in them is made by a C++ writer in that file from synthetic data.

Getting the tool without a build: the Windows artifact `mh4n-runner-windows` of the CI job `native-windows`
([`CI.md`](CI.md)) holds `mh4n_capture_player.exe` next to the runner and the DLLs it needs.

```
mh4n_capture_player --gpu NAME [options] CAPTURE.mgpu
mh4n_capture_player --list          # the Gpus, wrappers, checks and options that are linked in
```

Exit status: 0 everything passed, 1 something failed, 2 the run could not complete (an unknown
Gpu or check, a Gpu that lacks what a check needs, a damaged capture or one that breaks the
ordering rules of `mgpu-format.md` section 2).

## What it does with each record

| Record | The player |
|---|---|
| `INIT` | calls `GpuDebug::LoadInitState` (the Gpu's blocks, section 4 of the format) before the first command; loads its own copies of the display registers (`GPU_REGS`, `LCD_REGS`) and of the PICA register file (`PICA_REGS`) |
| `MEM`, `CPU_PAGES` | write into the Gpu's physical memory (FCRAM, VRAM, DSP RAM) and into the capture's reference copy |
| `GX`, `MMIO`, `SWAP`, `FILL` | call `Execute`, `WriteReg`, `SetBufferSwap`, `SetColorFill`; the display copies follow them as `GPU::WriteReg`, `SetBufferSwap` and `SetColorFill` do |
| `PICA_REG`, `GPU_PAGES`, `IRQ` after one of those | the reference of that event: what the command should have written, decoded and signalled |
| `FLUSH` kinds 1, 3, 4 / kind 2 | `FlushRegion` / `InvalidateRegion` |
| `IRQ` PDC0, PDC1 | `VBlank(0)`, `VBlank(1)` |
| other `IRQ` | compared, in order and by id only, with what the Gpu signalled through `Host::SignalInterrupt` (the delays are ignored: D25) |
| `FRAME` | the frame's row: the displayed screens against `<n>_top.ppm` and `<n>_bottom.ppm`, the Gpu's counters and times, the checks' columns |

An event and the `PICA_REG`, `GPU_PAGES`, `IRQ` and `FLUSH` records that follow it form a group.
The player reads the whole group first, so the `replay` Gpu can find the reference of the event it
is executing (`CaptureHost::CurrentEvent`), then runs the event, then judges it, then hands the
rest of the group to the Gpu and the checks in file order.

## Modes and what is judged

- `--chained` (the default): the pages the Gpu wrote are compared with `GPU_PAGES` but kept, so
  errors build up as they would in the game. `--isolated`: after a command is judged, its
  `GPU_PAGES` are written into the Gpu's memory, so every command starts from the reference inputs.
- `--judge KINDS` (default `fill,transfer,copy,dma,cmdlist,frames`; also `cacheflush`, `all`,
  `none`). A command of a kind that is not judged is still sent to the Gpu, then its `GPU_PAGES`
  are applied in place of the Gpu's output and it is counted as skipped. Without `frames` the
  image columns are printed but nothing fails. An `MMIO` record is classified by the register it
  writes (memory-fill, display-transfer and command-buffer triggers, as `GPU::WriteReg`).
- Fills, transfers, copies and DMA: every page the capture's GPU wrote must hold the same bytes
  in the Gpu's memory (bit-exact), except the pages that are the GSP service's, below.
- **The GSP's interrupt queue is not the Gpu's.** `GPU_PAGES` are the pages that changed while
  `GPU::Execute` ran. With the reference's settings (D25) the GSP delivers an interrupt at once,
  inside that window, and writes it into its interrupt relay queue, which is in the GSP's shared
  memory: four queues of 0x40 bytes at the start of a page (`index`, `number_interrupts`,
  `error_code`, `config`, two missed-PDC counters, then 0x34 slots of one byte). So the page of
  that shared memory is in the `GPU_PAGES` of every command that signals an interrupt, and the
  first byte that differs from a Gpu's memory is byte 1, `number_interrupts`. A Gpu does not write
  it (the service does; the player has no GSP). The player leaves such a page out of the
  judgement when both hold: the capture's change of it (against the capture's own memory before
  the command) is exactly the command's interrupts, in order, pushed into one of the four queues
  as `GSP_GPU::ProcessPendingInterruptImpl` does (`diagnostics.h`: `ExplainedByInterruptQueue`),
  and the Gpu has not touched the page. The page is then put into the Gpu's memory as the
  service's write (in every mode), and counted in the summary line `pages of the GSP's interrupt
  relay queue ... not judged`. Any other change of such a page, a page the Gpu wrote, or a page
  whose command signalled no interrupt, is judged as before.
  The line counts listings, not distinct pages: every command that signals an interrupt lists the
  same page of the GSP's shared memory, once, in its `GPU_PAGES`, and the player counts it for
  every command of every kind (command lists too, and also when `--judge` leaves their kind out,
  because the page is looked for in every event). A page that one event lists twice is counted once
  on purpose (per event and address), so the totals by kind tell the readings apart. The line says
  the total, how many distinct pages that was, and the total by kind, first for the commands
  judged and then for the commands skipped (not in `--judge`): `... not judged): 12 (one for each
  command that signalled an interrupt into it, whatever its kind; 1 distinct page: 0x26c00000); in
  commands judged: transfer=6; in commands skipped (not in --judge): cmdlist=6`. At most four
  addresses are listed, then `, ...`; a kind with no listing is `none`. `other` is every command
  that is none of the kinds of `--judge` (an MMIO write that starts nothing, a buffer swap, a GX
  command of another id): the player passes it through whatever `--judge` says, so the line shows
  it under the commands judged, although the `commands judged:` line lists only the kinds.
  The 12 that the dump-check of N2-03 printed on both golden captures (with `--judge
  fill,transfer,copy,dma`) is expected to be the 6 transfers plus the 6 command lists, inferred
  from the code and from the golden counts transfer=6 and cmdlist=6, not two pages per transfer;
  re-running the dump-check confirms it, and the new line shows which reading holds (the totals by
  kind: transfer=6 and cmdlist=6 on 1 page for the first, transfer=12 on 2 or more pages for the
  second).
- Command lists: the colour buffers go through the image thresholds. The buffers judged are the
  ones the command list drew into (the colour-buffer registers, 0x117, 0x11D and 0x11E, at each
  draw trigger 0x22E and 0x22F), and the final one when the capture's GPU wrote into it. Before
  judging, the player calls `GpuDebug::WriteBackColorBuffers`. Pages the capture's GPU wrote
  outside every judged colour buffer (the depth buffer, for one) are counted
  (`command-list pages outside any judged colour buffer`) but not judged.
- `--check NAME` (repeatable): extra checks, below. They do not depend on `--judge`.
- `--skip-gas-frames`: a frame whose command lists set fog mode 7 (register 0x0E0, or draw with it
  still set) is reported, `gas-skipped`, but not judged. Azahar draws no gas.
- `--fail-fast` stops at the first failure.

### The image thresholds

A frame or a colour buffer passes when both hold (`docs/native/ROADMAP.md` section 6):
- PSNR of the R, G and B values is at least 40 dB (infinite when identical);
- no 16x16 block has a mean absolute error above 8/255, the mean taken over the block's pixels and
  its three channels (blocks at the right and bottom edges are smaller when the size is not a
  multiple of 16 and are averaged over the pixels they have).

Alpha is not compared. The table prints PSNR and the worst block per screen.

## What a failing fill, transfer, copy or DMA says

The first line is the summary (`transfer: 1 of 7 pages written by the capture's GPU differ; first
at 0x... (byte N of the page)`); the lines below it are indented and hold, without a byte of the
capture (addresses, sizes, offsets, counts and names only):

- `command:` the GX command (`GX DISPLAY_TRANSFER (id 3)`) or the register write that started it
  (`MMIO write of 0x... to 0x...`), then its decoded fields:
  - **display transfer** (flags as `Pica::DisplayTransferConfig`): input and output address (and
    the number in the command when it was a virtual address), input and output format by name
    (`RGBA8`, `RGB8`, `RGB565`, `RGB5A1`, `RGBA4`, `invalid(N)`), input size, output size as written
    and as scaled, tiled 8x8 or linear for each side (the output is tiled when the input is linear
    and swizzling is on, or the input is tiled and `dont_swizzle` is set), the output's row stride
    (tile-row stride when tiled) with `no gap`, the scaling mode, vertical flip, crop of input
    lines, `dont_swizzle`, the `block32` bit and the flags word. Bit 3 set on a display-transfer
    command is called out: Azahar then runs a texture copy with the registers as they were;
  - **texture copy** (the raw copy: bytes moved unchanged, line by line): addresses, line width and
    gap of the input and of the output in bytes, the size and the part of it that is copied;
  - **memory fill**: the unit, the width of its units (16, 24 or 32 bits), start and end, the
    control word (the fill value is not printed);
  - **DMA**: source, destination, size.
- `output region:` the range the command writes (as far as its fields tell: a tiled output is
  rounded up to whole 8x8 tiles, a texture copy includes its gaps), with its size and page count;
  `none` when Azahar refuses the command's fields.
- `pages:` how many pages the capture recorded (first and last address), how many of them lie in
  the output region, and how many pages of the region the Gpu changed (measured against the Gpu's
  memory just before the command; not measured when the region is over 64 MiB), and how many
  recorded pages were the GSP's queue.
- one `page 0x...:` line per differing page (at most eight, then `... and N more differing
  pages`): the number of bytes that differ, the offset of the first and of the last (never their
  values), whether the page is the first, a middle or the last of the pages the capture recorded,
  whether it is inside the output region (and which of its pages) or outside it, and a note when
  every difference is inside the first 0x100 bytes of the page, where the GSP keeps its queues.

Reading it: a page **outside the output region** is something the command's fields do not
explain (the GSP's memory, a second command's output, a decoding the player does not know); a page
**inside** it with the Gpu having written fewer pages than the capture recorded is a Gpu that
skipped part of the output; the first and last offsets show whether a whole page, a pixel or the
start of a page is wrong.

## The displayed screens

The software renderer's `LoadFBToScreenInfo` is reproduced from the player's copy of the display
registers: the framebuffer chosen by `active_fb`, the format, the stride, the column count from
the size register, the colour fill (`LCD_REGS`, both screens, set by `FILL` records and MMIO). The
reference PPMs come from the same renderer, so its quirks are kept:

- the image is `columns` wide and `stride / bytes per pixel` high; pixel (row x, column y) is
  memory pixel `y * stride_pixels + stride_pixels - x`. For x = 0 that is the first pixel of the
  next column and for the last column one pixel past the buffer (zero when the memory ends there);
- the left eye only.

`SWAP` addresses are converted with Azahar's `GPU::VirtualToPhysicalAddress` (`Host::VirtualToPhysical`
of the capture host: VRAM window, linear heap, addresses that already are physical). The format
document says the addresses are as the game wrote them, physical or virtual; either works, since a
physical address is left alone, except that a physical VRAM address (`0x18000000`...) falls in the old
linear-heap window, exactly as in Azahar.

**Assumptions about the recorder (N0-05), to check against a real capture:**
- the `FRAME` record of frame *n* is compared with `<n>_top.ppm` and `<n>_bottom.ppm` in the
  capture's frames directory, *n* being the record's frame number in decimal. The directory is
  `--frames-dir`, else `<capture>.frames`, else `<capture without extension>.frames`;
- that PPM shows the state of the display at the point of the `FRAME` record in the stream (the
  records before it), not the state after the following records. If the recorder takes the image
  at another moment, this is the one place to change (`Player::HandleFrame`);
- a missing reference image fails a judged frame.

`--dump-frames DIR` writes `<n>_top.ppm`, `<n>_bottom.ppm` (what the Gpu's memory displays) and
`<n>_top_diff.ppm`, `<n>_bottom_diff.ppm` (the absolute difference times four, clamped).

## Output

A table with a row per frame: `frame record result top_psnr top_blk bot_psnr bot_blk cpu_ms gpu_ms`,
then one column per counter the Gpu reported and one per column a check declared, then the
failures (`record N (frame F, GX SUBMIT_CMDLIST): message`), the number of commands judged and
skipped per kind, the kinds in `--judge` the capture has no command of (`commands not exercised`), and `result: PASS` or `FAIL (...)`. `cpu_ms` is the time spent inside the Gpu's
calls since the previous `FRAME`; `gpu_ms` comes from `GpuDebug::TakeFrameGpuTimeNs` (`-` when the
backend has no timestamps). Failure messages quote addresses, sizes and counts, never buffer
contents; a register check quotes register numbers and values.

## Adding to the player from your own module

Nothing in the player is edited. A module lists `mh4n_gpu_capture` in `DEPENDS` (for what it
includes from `gpu_capture/`) and registers:

```cpp
#include "gpu_capture/check.h"          // CaptureCheck, CheckFactory, CaptureHost
#include "mh4n/gpu_debug.h"             // GpuDebug, GpuWrapper, InitState, Counter

// A Gpu, by name: --gpu cpu-gx
MH4N_REGISTER(gpu::GpuFactory, "cpu-gx", [](gpu::Host& host, const Options& o) { ... });

// A wrapper, turned on by the flag of the same name: --queue
MH4N_OPTION("capture-player", "--queue", "", "run the Gpu behind the renderer thread");
MH4N_REGISTER(gpu::GpuWrapper, "queue",
              [](std::unique_ptr<gpu::Gpu> inner, gpu::Host& host, const Options& o) { ... });

// A check: --check my-check, with its own option and column
MH4N_OPTION("capture-player", "--my-limit", "N", "...");
MH4N_REGISTER(gpu_capture::CheckFactory, "my-check",
              [](const Options& o) { return std::make_unique<MyCheck>(...); });
```

- A **Gpu** that wants more than the memory check also derives from `gpu::GpuDebug`
  (`mh4n/gpu_debug.h`); the player finds it with `dynamic_cast`. Every member has a "not supported"
  default: `LoadInitState`, `TakeRegisterWrites` (the decoded writes since the last call; `nullopt`
  means the Gpu does not decode registers), `WriteBackColorBuffers`, `TakeFrameCounters`
  (named counters of the frame that ended: each name becomes a column) and `TakeFrameGpuTimeNs`.
- A **wrapper** is applied around the Gpu named by `--gpu` (and around the wrappers before it, in
  name order). It must implement `GpuDebug` too, forwarding to what it wraps, because the player
  only sees the outermost object.
- A **check** (`gpu_capture::CaptureCheck`) is told the Gpu, its `GpuDebug`, the host and the
  options in `Start`, sees every record in `OnRecord` (after the Gpu has run the group that record
  belongs to), every frame in `OnFrame` (where it fills the columns it declared in `Columns()`),
  and gets `Finish`. A non-empty return is a failure of that record, printed as
  `[check NAME] message`. `--check registers` is the one that ships: the Gpu's decoded register
  writes against `PICA_REG` (same registers, order and byte masks; the bytes under each mask).
- An **option** registers in the `capture-player` scope and is read from the `Options` the
  factories get.

### The two Gpus that ship

- `replay` writes the `GPU_PAGES` of the event it is executing, signals the interrupts the capture
  has and reports the capture's register writes. It must pass everything; it needs the capture
  host (`dynamic_cast<CaptureHost*>(&host)`).
- `none` does nothing. It must fail at the first fill, copy or draw, and passes with `--judge none`.

## Running behind the `queue` wrapper (N2-22)

`--queue` wraps the chosen GPU in the `queue` wrapper (`native/src/gpu/queue/`). The registered wrapper is **lockstep**: the renderer
thread makes every call to the wrapped GPU, but each call returns when it is done, because the player judges memory, interrupts and
registers right after each call. A run with `--queue` therefore shows that the wrapped GPU works when every call comes from one thread
that is not the player's, that the debug calls and the interrupts pass through, and that the GPU is destroyed on that thread. It does
**not** show asynchronous ordering on real work: the fake-GPU cases of `gpu_queue_test` cover that (calls held behind a gate, interrupt
order, the invalidate's wait, the frame counting and its cap).

The check the dump-check of N2-22 asks for is that three argument lines give the same result with and without `--queue` on every capture
of G1 to G4. The script below (needs `bash`: Git Bash or WSL on the PC) runs them, compares the exit status, stderr and stdout, and
leaves out the timing and counter columns by name (`cpu_ms`, `gpu_ms`, `translate_us`, `queue_wait_us`, `queue_frame_waits`; with
`--queue` three columns more appear). Save it as `build/logs/queue_compare.sh`, run it from the repository root (`P=` names another
player, for example the Windows artifact's), and read the verdicts:

```bash
#!/bin/bash
# Runs the capture player with and without --queue for three argument lines and compares the exit
# status, stderr and stdout; the timing and counter columns are left out by name.
P=${P:-build/native/bin/mh4n_capture_player}
cmp_table() { awk '
  /^ *frame +record +result/ { n=split($0,h); for(i=1;i<=n;i++) skip[i]=(h[i]~/^(cpu_ms|gpu_ms|translate_us|queue_wait_us|queue_frame_waits)$/); intable=1; next }
  intable && NF>0 && $1 ~ /^[0-9]+$/ { out=""; for(i=1;i<=NF;i++) if(!skip[i]) out=out" "h[i]"="$i; print out; next }
  { intable=0; print }' "$1"; }
c=$1; o=$2; t=$3; mkdir -p "$o"; i=0
while IFS= read -r args; do
  i=$((i+1))
  $P $args "$c" > "$o/$t.$i.off.txt" 2>&1; e0=$?
  $P $args --queue "$c" > "$o/$t.$i.on.txt" 2>&1; e1=$?
  if [ $e0 -ge 2 ] || [ $e1 -ge 2 ]; then echo "ERROR off=$e0 on=$e1  $args (see $o/$t.$i.off.txt)"; continue; fi
  if [ $e0 = $e1 ] && diff <(cmp_table "$o/$t.$i.off.txt") <(cmp_table "$o/$t.$i.on.txt") > "$o/$t.$i.diff"; then
    echo "SAME  exit=$e0  $args"
  else
    echo "DIFFERENT off=$e0 on=$e1  $args (see $o/$t.$i.diff)"
  fi
done <<'LIST'
--gpu cpu-gx --chained --judge fill,transfer,copy,dma --skip-gas-frames
--gpu pica-only --isolated --judge none --check registers
--gpu replay --chained --skip-gas-frames
LIST
```

```
for g in G1 G2 G3 G4; do for c in build/golden/$g/gpu-*.mgpu; do
  build/logs/queue_compare.sh "$c" build/logs/qcmp $g-$(basename "$c" .mgpu); done; done
```

Every line must say `SAME`. `ERROR` means the player could not run (a bad path, a glob that matched nothing: both runs exit 2 with the same
error), not a pass. `SAME exit=1` means both runs failed alike: on the real captures `cpu-gx` and `pica-only` are expected to exit 0, and
`replay` with its default judge needs the reference frames next to the capture (`<capture>.frames/`, `golden.md`), without which it exits 1 on
both sides. `DIFFERENT` names a file whose diff shows the line. The script was run on the synthetic captures `gpu_queue_test` writes
(`MH4N_QUEUE_TEST_KEEP=<dir> build/native/bin/gpu_queue_test -tc="*capture_player*"` keeps `good/scene.mgpu` and `bad/scene.mgpu`): `SAME` on
both, `ERROR` on a missing capture, and a reviewer proved it says `DIFFERENT` for a changed compared column, a different exit status and
an extra stderr line, and `SAME` for a change in a skipped column only.

## The gas table's dump check (N2-38)

`--gpu pica-only` reports the frame counter `pica_gas_lut_writes` (N2-38): the number of words written to `GPUREG_GAS_LUT_DATA`
(0x124) in that frame, a count only. The counter and the table (`PicaState.gas`) never print the table's values. N2-38's last
*Done when* line, over the real captures, needs the game files, so its row says `dump-check pending`. This is what to run
on a machine that has the recordings (a worktree has none: use the main checkout, where `build/golden` lives), in bash (Git Bash or WSL on
Windows), with a player built from the N2-38 commit or later (`cmake --build build/native --target mh4n_capture_player`, or the
Windows artifact built from it; point `P=` at it):

1. Run, from the main checkout's root:
   ```
   mkdir -p build/logs
   P=${P:-build/native/bin/mh4n_capture_player}
   for d in build/golden build/golden-n010 build/golden-n035; do
     for g in G1 G2 G3 G4; do
       found=0
       for c in $d/$g/gpu-*.mgpu; do
         [ -e "$c" ] || continue
         found=1
         $P --gpu pica-only --isolated --judge none --check registers "$c" 2>&1 \
           | tee "build/logs/gas-$(basename $d)-$g-$(basename "$c" .mgpu).txt"
         echo "$c exit=${PIPESTATUS[0]}" | tee -a build/logs/gas-exit-codes.txt
       done
       [ $found = 1 ] || echo "MISSING $d/$g" | tee -a build/logs/gas-exit-codes.txt
     done
   done
   ```
   The `echo` must stay directly after the `tee` pipeline (`PIPESTATUS` is the player's exit status only there). Do not add
   `--skip-gas-frames` (`--judge none` judges no frame) or `--queue`. The time is not measured (`unverified`); the captures are 3 to 10
   frames each.
2. **Required:** `build/golden/G1` to `G4` (the current recordings, `STATUS.md` N0-35: G1 `gpu-1500.mgpu`, G2 `gpu-2500.mgpu`,
   G3 `gpu-17400.mgpu`, G4 `gpu-2400.mgpu`; G3-perf is not part of G1 to G4). A `MISSING build/golden/G<n>` line is a failure of
   the procedure. **Extra, if present:** `build/golden-n010` (the first, older G3 and G4) and `build/golden-n035` (the interim folder of
   N0-35, which `STATUS.md` says was moved into `build/golden`): `MISSING` lines for these are expected.
3. **Pass:** `build/logs/gas-exit-codes.txt` has `exit=0` for every capture found, and each log ends with `result: PASS`. N2-04's
   dump check passed G1 and G2; G3 and G4 have not run with `pica-only` before. Any `exit=1` (a register mismatch) or `exit=2` (the run could not
   complete) is reported as blocked, with the capture path and the exit code: no retry with other flags.
4. **The count:** the G4 answer is the `pica_gas_lut_writes` column of the table in `build/logs/gas-golden-G4-gpu-2400.txt` (one row per
   frame, ten rows). To read it without printing anything else of the log:
   ```
   awk '/^ *frame +record +result/ {col=0; for(i=1;i<=NF;i++) if($i=="pica_gas_lut_writes") col=i; intable=1; next}
        intable && col && $1 ~ /^[0-9]+$/ {print "frame " $1 ": pica_gas_lut_writes=" $col; next}
        intable && NF==0 {intable=0}' build/logs/gas-golden-G4-gpu-2400.txt
   ```
   Any non-zero frame means the command lists inside the ten frames write the table (16 in a frame is a full rewrite). All
   zeros means the table was written before the capture started, or by a path the player does not decode: the `INIT` record carries no gas
   table, so the table stays zero for the draws and N2-10 must be told. On G1 to G3 a non-zero value is information, not a failure.
5. **Send back:** the lines of `build/logs/gas-exit-codes.txt` and, for G4, the ten numbers of step 4 (per directory found). Counts
   only: never paste or commit the logs, because a `--check registers` failure quotes register numbers and values taken from the capture.

## Limits of the checks

- What a run judges is what its capture contains. A capture without fills, copies or DMA says
  nothing about them: the summary line `commands judged:` counts what was judged by kind, and
  `commands not exercised` lists the selected kinds the capture has none of: **not exercised**, not
  verified.
- Only the pages the capture's GPU wrote are compared. A page the Gpu writes and the capture's GPU
  did not is not seen (comparing all of memory after every command would cost a pass over 140 MB
  each).
- A command list's colour buffer is compared whole. In `--chained` mode an earlier difference that
  is still in the buffer is reported again by each later draw into it; `--isolated` avoids that.
- Colour buffers are compared as RGB; alpha, depth and stencil are not judged.
- The Gpu's memory is the capture's: the reference copy of FCRAM and VRAM is a second copy, so a
  run needs about twice the capture's memory image (zero pages are never committed).
- Captures have no virtual-memory map, so virtual addresses are translated with Azahar's fixed
  windows. A capture of a game that maps the linear heap elsewhere is not supported.
