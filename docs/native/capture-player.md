# The capture player (N2-02)

`mh4n_capture_player` replays an `MGPU` capture ([`mgpu-format.md`](mgpu-format.md)) into any
`mh4n::gpu::Gpu` and checks the result against the capture: the memory each command wrote, the
register writes of each command list and the displayed screens of each frame. It times every
frame. The renderer of N2 is built and checked with it, without the runtime.

The logic is the module `native/src/gpu_capture/` (`mh4n_gpu_capture`); the program in
`native/tools/capture_player/` is only `main`. The tests are `native/tests/capture_player_test.cpp`;
every capture in them is made by a C++ writer in that file from synthetic data.

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
  in the Gpu's memory (bit-exact).
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
document says they are physical; either works, since a physical address is left alone, except that a
physical VRAM address (`0x18000000`...) falls in the old linear-heap window, exactly as in Azahar.

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
skipped per kind, and `result: PASS` or `FAIL (...)`. `cpu_ms` is the time spent inside the Gpu's
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

## Limits of the checks

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
