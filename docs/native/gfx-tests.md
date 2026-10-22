# Synthetic GX programs

Our own 3DS programs that drive the GPU through the GSP the way a game does, with pictures whose
pixels are known exactly: an oracle for the GX path (memory fills, display transfers, texture copies),
the display registers (buffer swaps) and presentation, that needs no game data. It runs on the
reference runner now and on the native runner once the renderer can take screenshots (N2b-12 to
N2b-14), and the same expected images judge both. Work package N2b-15; `tests/test_native_gfx.py` is
the test, `port/tests/gsp.h` the helper and `port/tests/gfx_gx.c` the program. A PICA draw program
(`gfx_draw.c`) joins them in N2b-16 and adds its section here.

## `gsp.h`

For bare test programs (C, freestanding, `svc.h`): `GspGpu` is the spec's `gsp::Gpu`.

| Function | What it does |
|---|---|
| `gsp_open` | `srv:` to `gsp::Gpu`, a one-shot event, `RegisterInterruptRelayQueue` (0x13), the shared block mapped at 0x10002000, `AcquireRight` (0x16) |
| `gsp_alloc_linear` | `svcControlMemory` with the linear flag: memory the GPU can reach (the linear heap starts at 0x30000000 under Azahar's ELF loader) |
| `gsp_drain`, `gsp_wait(id)`, `gsp_wait_vblank_count(n)` | take interrupts out of the relay queue in shared memory; wait for one of a kind (`PSC0`, `PPF`, `PDC0`...), or until the program has seen `n` VBlanks |
| `gsp_queue` and `gsp_memory_fill`, `gsp_display_transfer`, `gsp_texture_copy` | write a command into the shared GX queue and, when the queue was empty, `TriggerCmdReqQueue` (0x0C); Azahar processes the queued commands during that call and signals the interrupts before it returns |
| `gsp_flush` | `FlushDataCache` (0x08) |
| `gsp_swap_buffers` | the way libctru does it: write the framebuffer update into the shared block and set its dirty flag; the GSP applies it at the screen's next VBlank (PDC0 top, PDC1 bottom) |
| `gsp_set_buffer_swap` | `SetBufferSwap` (0x05), applied at once |
| `gsp_write_regs` | `WriteHWRegs` (0x01) with a static buffer |

The layouts are 3dbrew's *GSP Services* and *GSP Shared Memory*: per GSP thread, a 0x40-byte
interrupt relay queue at `thread * 0x40` (index, count, error, config, two missed counters, 0x34
slots), two 0x40-byte framebuffer updates at `0x200 + (2 * thread + screen) * 0x40` (index, dirty
flag, two 0x1C-byte framebuffer infos), and a 0x200-byte command queue at `0x800 + thread * 0x200`
(index, count, status, 15 commands of 8 words); written from the public documentation and Azahar's
behaviour, nothing copied from libctru. `gfx_gx.c` shows a buffer on the bottom screen in all three ways: the shared block (step 1), the
`SetBufferSwap` request (step 6) and the registers (step 7).

## `gfx_gx`: what each step draws

A screen is stored rotated: the top screen (400x240 as shown) is 400 stored lines of 240 pixels, the
bottom (320x240) 320 lines of 240. Transfers therefore move 240x400 images, and the screenshot shows
stored line `y` as column `y` and pixel `x` of it as row `x`.

The program first draws three test cards with the CPU into linear memory (tiled, RGBA8): a 240x400 card
(red grows with x, green with y, blue is a 16-pixel checkerboard), the same card on a 480x800 grid with
each pixel four times and the red channel's two low bits replaced by `x%2 + 2*(y%2)` (so a correct box
filter gives red + 1, not the pixel itself), and the bottom screen's texture (320 rows of 240 RGB565
pixels, each followed by 32 bytes of 0xA5 that must not reach the screen). Then nine steps; each
waits for a VBlank, prints `gfx_gx: step N <name> vblank V` (V: the VBlanks seen), does its work,
swaps, and holds its picture until the V+60th VBlank:

| # | Name | GX work | Top screen | Bottom screen |
|---|---|---|---|---|
| 1 | `fills` | one fill command with two buffers (a 32-bit value `0x2090E0FF` and a 24-bit pattern `12 34 56`), one PSC0; a fill with a 16-bit value `0xA945`; a display transfer RGBA8 to RGB8 | the 32-bit colour, (0x20, 0x90, 0xE0) | the 16-bit fill as RGB565, (173, 40, 41); shown through the shared block |
| 2 | `rgb565` | the same source transferred to RGB565 | (0x21, 0x92, 0xE7): the colour with the low bits dropped and refilled | |
| 3 | `pattern` | the 24-bit source transferred to RGB8 | three colours: the 3-byte pattern over 4-byte pixels repeats with the pixel number mod 3, so pixel `n` holds the pattern from byte `4n` | |
| 4 | `pattern565` | the 24-bit source transferred to RGB565 | the same three colours quantised | |
| 5 | `flip` | the card transferred to RGB565 with the vertical flip, into the second top buffer | the flipped card | |
| 6 | `scale` | the 480x800 card transferred to RGB8 with the 2x downscale (the output registers hold the size before it) | the card with red `(r & 0xFC) + 1` | |
| 7 | `texcopy` | a texture copy of the bottom texture, 30 units of 16 bytes per row with a gap of 2 in the source, contiguous into the second bottom buffer; shown with the `SetBufferSwap` request | step 6's card | the texture card |
| 8 | `swap` | none: both screens swap in one frame (the top through the shared block, the bottom by writing its framebuffer registers with `WriteHWRegs`) | step 5's buffer | step 1's buffer |
| 9 | `alternate` | none: the top screen swaps between its two buffers on every VBlank, 120 times, and the program prints `gfx_gx: flip vblanks 00000078` | after VBlank j of the step: step 6's buffer for odd j, step 5's for even j | step 1's |

Every transfer and fill is followed by a wait for its interrupt (PSC0 or PPF); the program ends with
`gfx_gx: gsp error 00000000` (the first failing GSP result, 0 for none) and `gfx_gx: done`.

One detail of the display that the expected images follow, because Azahar's software renderer
(`LoadFBToScreenInfo`, the reference of `native/src/gpu_capture/image.h`'s `BuildScreen`) does it: row 0
of column `y` shows the first pixel of stored line `y + 1`, not of line `y` (it reads pixel number
`y * stride + stride - x`), so the last column's row 0 shows the pixel after the buffer's end. That
pixel is whatever the memory holds there: zero after the program's clearing fills (step 1), except in
steps 2 and 4, where an RGB565 picture (192000 bytes) overwrote the start of an RGB8 one (288000) and the
pixel is made of two bytes of that older picture. A renderer that draws the display differently shows
one wrong pixel per screen here, and that is what the test would report.

## `tests/test_native_gfx.py`

The test holds a model of the GPU's operations (the fills and the display transfer and texture copy of
Azahar's `sw_blitter.cpp`, the colour conversions of `common/color.h`, the screen read-out above), runs
the program's nine steps on byte arrays and so computes the ten expected screenshots (top and
bottom). Always run, without a runner: the program builds and recompiles; the model's results agree with
numbers worked out by hand (the fill colours, the quantisation of RGB565, the three colours of the
pattern, the card's red); the `#define`s of the program are the model's constants; the shot schedule
(below); the variable handling.

With a runner it runs the program three times: the first prints the lines the schedule comes from, the
other two take the shots. It checks that the printed lines are the ones above (the steps 61 VBlanks
apart), that the three runs print the same lines, that the two with shots produce identical files, and
that every screenshot, top and bottom, is equal pixel for pixel to the model's image. When both
runners ran, it also checks that their shots are identical.

### How the shot times come from the printed lines

The program has no game frame counter, so the shots are in milliseconds, and each step holds long
enough for one in its middle.

1. VBlank number `j` happens `j * 4481136 / 268111856` s after the GPU starts (the ARM11 clock and
   Azahar's `FRAME_TICKS`, 59.83 Hz, 16.7137 ms), which is the runner's emulated time.
2. The program waits for a VBlank before each step and holds until the 60th after it, so step `n` starts
   at `V_n = V_1 + 61 * (n - 1)`; the test fails if the printed `V` do not follow that. Its picture is
   on the screen from VBlank `V_n + 1` to `V_n + 61`.
3. The first shot goes at `V_1 + 31.5` frames (the middle of the hold, half a frame from each VBlank,
   which matters for step 9) and the others every 61 frames: with the usual `V_1 = 4`, 593.3 ms and
   1019.53 ms. The options take whole milliseconds, so the test passes `--shots-from 591
   --shot-every 1020` for that case (the interval rounded up, the start moved back by half the drift
   the rounding adds over the ten shots: each shot is within 2.5 ms of its ideal time). The first
   run's `V_1` comes from the run itself, so a runner whose start-up differs gets its own times.
4. The tenth shot is 61 frames after the ninth, still in step 9 (120 VBlanks long): step 9's two
   shots are at VBlank 31 and 92 of the step, one showing each buffer.

## Running it

Reference runner only (the Linux artifact of `native.yml`, fetched as `docs/native/CI.md` says, or any
build of `port/`; it runs the program with `--cpu jit --new-3ds off`):

```
MH4U_RUNNER=build/runner/run.sh MH4N_GFX_REQUIRE=reference python3 -m pytest -rs tests/test_native_gfx.py
```

Native runner (when its GPU takes screenshots; the code library of the program is built by the test):

```
MH4N_RUNNER=build/native/bin/mh4u_native_runner MH4N_GFX_GPU=renderer MH4N_GFX_REQUIRE=both \
  MH4U_RUNNER=build/runner/run.sh python3 -m pytest -rs tests/test_native_gfx.py
```

`MH4N_GFX_GPU` is the native runner's `--gpu` (`renderer` once N2b-12 to N2b-14 are in; `null-display`
takes shots but draws nothing). Without a runner the runner tests skip, and `-rs` says which variable
was missing; `MH4N_GFX_REQUIRE` (`reference`, `native` or `both`, comma separated) turns the skip of the
named runner into a failure, so a CI job cannot go green by not running them. The native runner is
called as `runner ELF --code LIB --gpu NAME --ms 20000 --user DIR --shots DIR --shots-from MS
--shot-every MS`.

To look at a picture: the shots are PPM files; the test's temporary folder is deleted after the run, so
run the program by hand (`python3 port/tests/build_svc_test.py --program gfx_gx -o build/gfx`, then the
runner with `--shots`).

Adding a step: change `gfx_gx.c` and the model's `expected_shots` together; `test_constants_match_the_program`
keeps the numbers the two share in step.
