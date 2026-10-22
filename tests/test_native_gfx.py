# Derived from Azahar src/video_core/renderer_software/sw_blitter.cpp, src/common/color.h,
# src/video_core/utils.h, src/video_core/renderer_software/renderer_software.cpp and
# src/video_core/pica/regs_external.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later;
# each says "Licensed under GPLv2 or any later version"): the model of the GPU's memory fills, display
# transfer and texture copy, the colour conversions, the Morton tiling and the read-out of a screen
# restate those functions in Python, and the transfer flag bits are those of regs_external.h.
# Copyright 2023 Citra Emulator Project (sw_blitter.cpp)
# Copyright 2014 Citra Emulator Project (color.h, utils.h)
# Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (renderer_software.cpp)
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (regs_external.h)
"""Synthetic GX programs (N2b-15): port/tests/gfx_gx.c drives the GPU through the GSP the way a game
does (memory fills, display transfers, a texture copy with a gap, buffer swaps of both screens,
VBlank waits), and this test checks every picture it holds against an image computed here.

- Always: the program builds and recompiles; the model that computes the expected images agrees
  with numbers worked out by hand; the shot schedule is derived from printed lines as documented.
- With MH4U_RUNNER (the reference runner, `--cpu jit --new-3ds off`): the program prints the lines
  of docs/native/gfx-tests.md, three runs of it agree, and each screenshot equals the model's image pixel for pixel.
- With MH4N_RUNNER and MH4N_GFX_GPU (the native runner and its --gpu): the same checks; and when
  both ran, the two runs' shots are identical.
- MH4N_GFX_REQUIRE=reference (or native, or both; comma separated) turns the skip of that runner's
  tests into a failure.

How the shot times come from the program's lines (docs/native/gfx-tests.md has the picture): the
program prints, for each of its 9 steps, "gfx_gx: step N <name> vblank V", V being the number of
VBlanks it has seen when the step starts. A step holds its picture until the V+60th VBlank, and the
next step waits for one more, so the steps start 61 VBlanks apart and the picture of step N is on the
screen from VBlank V+1 to V+61. VBlank number j happens j * FRAME_TICKS / CLOCK after the GPU
starts, which is the runner's emulated time (checked: a shot at j + 0.5 frames shows the picture of
the VBlank j, and step 9 depends on it). The test takes the first run's V of step 1,
puts the first shot at V + 31.5 frames (the middle of the hold, half a frame from either VBlank),
and the next ones every 61 frames: one shot per step, and the 10th shot, 61 frames later, is in the
last step too (it lasts 120 VBlanks). That step alternates the top screen between two buffers on every
VBlank, so the two shots in it show different buffers, which is what the expected images say."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PORT_TESTS = os.path.join(ROOT, "port", "tests")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, PORT_TESTS)

PROGRAM = "gfx_gx"
NEEDED_TOOLS = ["clang", "ld.lld", "llvm-objcopy", "llvm-readelf"]
RUN_MS = 20000               # a run ends when the program exits (about 9.2 s of emulated time)
DEBUG_LINE = re.compile(r"OutputDebugString:\d+: (.*)$")
ANSI = re.compile(r"\x1b\[[0-9;]*m")

# ── the VBlank clock ─────────────────────────────────────────────────────────────────────────────
# Azahar's VideoCore::FRAME_TICKS and the ARM11 clock: the 3DS's 59.83 Hz.
FRAME_TICKS = 4481136
CLOCK = 268111856
FRAME_MS = FRAME_TICKS * 1000 / CLOCK
STEPS = ["fills", "rgb565", "pattern", "pattern565", "flip", "scale", "texcopy", "swap", "alternate"]
STEP_FRAMES = 61             # 60 held VBlanks and the one the next step waits for
SHOT_FRAME = 31.5            # the first shot, in frames after the first step's printed VBlank
SHOT_COUNT = 10              # nine steps, and a second one in the last (120 VBlanks)
FLIP_VBLANKS = 120

STEP_LINE = re.compile(r"gfx_gx: step (\d) (\w+) vblank ([0-9a-f]{8})$")


def shot_schedule(lines: list[str]) -> tuple[int, int, list[int]]:
    """(first shot in ms, interval in ms, the VBlank count V of each step) from a run's lines. The
    steps must start STEP_FRAMES apart, or the regular schedule of --shots-from and --shot-every
    cannot hit the middle of each hold. The options take whole milliseconds, so the interval is
    1020 ms for the 1019.53 of 61 frames, and the first shot is moved back by half of the drift the
    rounding adds over the shots: every shot is then within 2.5 ms of the middle of its hold's frame."""
    starts = []
    for line in lines:
        m = STEP_LINE.fullmatch(line)
        if m:
            assert int(m.group(1)) == len(starts) + 1 and m.group(2) == STEPS[len(starts)], line
            starts.append(int(m.group(3), 16))
    assert len(starts) == len(STEPS), f"{len(starts)} step lines in {lines}"
    assert all(b - a == STEP_FRAMES for a, b in zip(starts, starts[1:])), f"steps start at {starts}"
    every = round(STEP_FRAMES * FRAME_MS)
    drift = every - STEP_FRAMES * FRAME_MS
    return round((starts[0] + SHOT_FRAME) * FRAME_MS - drift * (SHOT_COUNT - 1) / 2), every, starts


# ── which runners, and what MH4N_GFX_REQUIRE says ────────────────────────────────────────────────
def required() -> set[str]:
    """The runners MH4N_GFX_REQUIRE names: reference, native, both (comma or space separated)."""
    parts = {p for p in re.split(r"[,\s]+", os.environ.get("MH4N_GFX_REQUIRE", "").strip().lower()) if p}
    unknown = parts - {"reference", "native", "both"}
    if unknown:
        raise ValueError(f"MH4N_GFX_REQUIRE: unknown value(s) {sorted(unknown)}; use reference, native or both")
    if "both" in parts:
        parts = (parts - {"both"}) | {"reference", "native"}
    return parts


def skip_or_fail(kind: str, reason: str):
    """A runner (or, for kind "any", what every runner needs) that is not there: a skip, or a failure
    when MH4N_GFX_REQUIRE names the runner (any runner, for "any")."""
    try:
        wanted = bool(required()) if kind == "any" else kind in required()
    except ValueError as e:
        pytest.fail(str(e))
    if wanted:
        pytest.fail(f"MH4N_GFX_REQUIRE names the {kind} runner, but {reason}")
    pytest.skip(reason)


def have_toolchain() -> bool:
    return all(shutil.which(t) for t in NEEDED_TOOLS)


# ── pixel formats (Azahar's common/color.h, as native/src/gpu_capture/image.h documents them) ─────
RGBA8, RGB8, RGB565, RGB5A1, RGBA4 = range(5)
BPP = {RGBA8: 4, RGB8: 3, RGB565: 2, RGB5A1: 2, RGBA4: 2}


def c5(v): return (v << 3) | (v >> 2)
def c6(v): return (v << 2) | (v >> 4)
def c4(v): return (v << 4) | v


def decode_pixel(fmt: int, b, o: int = 0) -> tuple[int, int, int, int]:
    """(r, g, b, a) of the pixel at b[o:]."""
    if fmt == RGBA8:
        return b[o + 3], b[o + 2], b[o + 1], b[o]
    if fmt == RGB8:
        return b[o + 2], b[o + 1], b[o], 255
    p = b[o] | (b[o + 1] << 8)
    if fmt == RGB565:
        return c5((p >> 11) & 31), c6((p >> 5) & 63), c5(p & 31), 255
    if fmt == RGB5A1:
        return c5((p >> 11) & 31), c5((p >> 6) & 31), c5((p >> 1) & 31), 255 * (p & 1)
    if fmt == RGBA4:
        return c4((p >> 12) & 15), c4((p >> 8) & 15), c4((p >> 4) & 15), c4(p & 15)
    raise ValueError(f"format {fmt}")


def encode_pixel(fmt: int, r: int, g: int, b: int, a: int) -> bytes:
    if fmt == RGBA8:
        return bytes((a, b, g, r))
    if fmt == RGB8:
        return bytes((b, g, r))
    if fmt == RGB565:
        p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    elif fmt == RGB5A1:
        p = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7)
    elif fmt == RGBA4:
        p = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4)
    else:
        raise ValueError(f"format {fmt}")
    return bytes((p & 255, p >> 8))


# ── tiling: 8x8 Morton blocks (Azahar's video_core/utils.h) ───────────────────────────────────────
XLUT = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YLUT = (0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A)


def morton(x: int, y: int) -> int:
    return XLUT[x & 7] + YLUT[y & 7]


def tiled_index(x: int, y: int, width: int) -> int:
    """The number of the pixel (x, y) of an image `width` pixels wide stored in 8x8 tiles."""
    return (y & ~7) * width + (x & ~7) * 8 + morton(x, y)


# ── the GPU's memory operations (Azahar's sw_blitter.cpp), on byte arrays ─────────────────────────
def memory_fill(buf: bytearray, start: int, end: int, mode: int, value: int) -> None:
    """mode 16, 24 or 32 (bits); 24 writes the bytes value, value >> 8, value >> 16 over and over."""
    if mode == 24:
        pat = bytes((value & 255, (value >> 8) & 255, (value >> 16) & 255))
        for p in range(start, end, 3):
            buf[p:p + 3] = pat
    elif mode == 32:
        n = (end - start) // 4
        buf[start:start + 4 * n] = (value & 0xFFFFFFFF).to_bytes(4, "little") * n
    else:
        pat = (value & 0xFFFF).to_bytes(2, "little")
        for p in range(start, end, 2):
            buf[p:p + 2] = pat


def display_transfer(src: bytearray, dst: bytearray, in_w: int, in_h: int, out_w: int, out_h: int,
                     flags: int) -> None:
    """Tiled input, linear output (the way a game moves a rendered frame to the screen), with the
    vertical flip and the two downscales; the output registers hold the size before the downscale."""
    flip, linear_in, crop, raw, no_swizzle = flags & 1, flags >> 1 & 1, flags >> 2 & 1, flags >> 3 & 1, flags >> 5 & 1
    assert not (linear_in or crop or raw or no_swizzle), "this model does only tiled input, linear output"
    ifmt, ofmt, scaling = flags >> 8 & 7, flags >> 12 & 7, flags >> 24 & 3
    assert scaling <= 2
    hshift, vshift = (1 if scaling else 0), (1 if scaling == 2 else 0)
    ow, oh = out_w >> hshift, out_h >> vshift
    sbpp, dbpp = BPP[ifmt], BPP[ofmt]
    stride = in_w * sbpp
    taps = 1 << (hshift + vshift)        # pixels averaged: 1, 2 (x) or 4 (the next three in memory)
    for y in range(oh):
        iy = y << vshift
        oy = oh - y - 1 if flip else y
        for x in range(ow):
            ix = x << hshift
            so = (morton(ix, iy) + (ix & ~7) * 8) * sbpp + (iy & ~7) * stride
            px = [decode_pixel(ifmt, src, so + k * sbpp) for k in range(taps)]
            r, g, b, a = (sum(p[c] for p in px) // taps for c in range(4))
            do = (x + oy * ow) * dbpp
            dst[do:do + dbpp] = encode_pixel(ofmt, r, g, b, a)


def texture_copy(src: bytearray, dst: bytearray, size: int, in_word: int, out_word: int) -> None:
    """Rows of (width * 16) bytes with (gap * 16) bytes skipped after each, in and out; a gap of 0
    is contiguous. in_word and out_word are (gap << 16) | width."""
    remaining = size & ~15
    in_gap, out_gap = (in_word >> 16) * 16, (out_word >> 16) * 16
    in_width = remaining if in_gap == 0 else (in_word & 0xFFFF) * 16
    out_width = remaining if out_gap == 0 else (out_word & 0xFFFF) * 16
    sp = dp = 0
    rem_in, rem_out = in_width, out_width
    while remaining > 0:
        n = min(rem_in, rem_out, remaining)
        dst[dp:dp + n] = src[sp:sp + n]
        sp += n
        dp += n
        rem_in -= n
        rem_out -= n
        remaining -= n
        if rem_in == 0:
            rem_in = in_width
            sp += in_gap
        if rem_out == 0:
            rem_out = out_width
            dp += out_gap


# ── the displayed screens (Azahar's renderer_software.cpp LoadFBToScreenInfo) ─────────────────────
class Image:
    def __init__(self, width: int, height: int, rgb: bytes):
        self.width, self.height, self.rgb = width, height, bytes(rgb)

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        i = (y * self.width + x) * 3
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]

    def __eq__(self, other) -> bool:
        return (self.width, self.height, self.rgb) == (other.width, other.height, other.rgb)


def show_screen(mem: bytearray, fmt: int, stride: int, lines: int) -> Image:
    """The picture of a screen stored as `lines` lines of `stride` bytes: line y is column y of the
    image, pixel x of it is row x, and Azahar reads pixel number (y * stride_px + stride_px - x), so
    row 0 shows the first pixel of the next line and the last column's row 0 the pixel after the
    buffer (the memory model holds it; it reads as 0 beyond)."""
    bpp = BPP[fmt]
    px = stride // bpp
    out = bytearray(lines * px * 3)
    for y in range(lines):
        base = y * px
        for x in range(px):
            o = (base + px - x) * bpp
            r, g, b, _ = decode_pixel(fmt, mem, o) if o + bpp <= len(mem) else (0, 0, 0, 0)
            i = (x * lines + y) * 3
            out[i] = r
            out[i + 1] = g
            out[i + 2] = b
    return Image(lines, px, out)


def parse_ppm(data: bytes) -> Image:
    magic, size, maxval, pixels = data.split(b"\n", 3)
    w, h = map(int, size.split())
    assert magic == b"P6" and maxval == b"255" and len(pixels) == w * h * 3
    return Image(w, h, pixels)


# ── the program's parameters (gfx_gx.c's defines are read back by test_constants_match_the_program) ─
C_DEFINES = ("TOP_SIZE", "BOT_SIZE", "SRC_BYTES", "BOT_BYTES", "FILL32", "FILL24", "FILL16",
             "TEX_PITCH", "TEX_ROW", "PAD_BYTE", "HOLD_VBLANKS", "FLIP_VBLANKS",
             "TOP_A", "TOP_B", "BOT_A", "BOT_B", "LIN_SRC_A", "LIN_SRC_B", "LIN_SRC_C", "LIN_SRC_D",
             "LIN_TEX", "LIN_SIZE")
TOP_SIZE, BOT_SIZE = 0x48000, 0x28000
SRC_BYTES = 240 * 400 * 4
BOT_BYTES = 240 * 320 * 2
FILL32, FILL24, FILL16 = 0x2090E0FF, 0x00563412, 0xA945
TEX_PITCH, TEX_ROW, PAD_BYTE = 512, 480, 0xA5
COLOR32 = (0x20, 0x90, 0xE0)          # FILL32 as R, G, B
COLOR16 = (173, 40, 41)               # FILL16 (r5 21, g6 10, b5 5) as 8-bit components


def card_rgb(x: int, y: int) -> tuple[int, int, int]:
    """The 240x400 test card: red grows with x, green with y, blue is a 16-pixel checkerboard."""
    return x * 255 // 239, y * 255 // 399, 255 if ((x >> 4) + (y >> 4)) & 1 else 0


def draw_card() -> bytearray:
    buf = bytearray(SRC_BYTES)
    for y in range(400):
        for x in range(240):
            r, g, b = card_rgb(x, y)
            o = tiled_index(x, y, 240) * 4
            buf[o:o + 4] = bytes((255, b, g, r))
    return buf


def draw_card_2x() -> bytearray:
    """The 480x800 card: the 240x400 card's pixels four times over, red with its two low bits
    cleared plus x%2 + 2*(y%2), so that the box filter's average (r + 1) is not the pixel itself."""
    buf = bytearray(4 * SRC_BYTES)
    for y in range(800):
        for x in range(480):
            r, g, b = card_rgb(x >> 1, y >> 1)
            r = (r & 0xFC) + (x & 1) + 2 * (y & 1)
            o = tiled_index(x, y, 480) * 4
            buf[o:o + 4] = bytes((255, b, g, r))
    return buf


def draw_texture() -> bytearray:
    """The bottom screen's texture: 320 rows of 240 RGB565 pixels and 32 bytes of padding."""
    buf = bytearray(320 * TEX_PITCH)
    for y in range(320):
        row = bytearray()
        for x in range(240):
            r5, g6, b5 = x * 31 // 239, y * 63 // 319, 31 if ((x >> 4) ^ (y >> 4)) & 1 else 0
            row += ((r5 << 11) | (g6 << 5) | b5).to_bytes(2, "little")
        buf[y * TEX_PITCH:(y + 1) * TEX_PITCH] = bytes(row) + bytes([PAD_BYTE]) * (TEX_PITCH - TEX_ROW)
    return buf


def expected_shots() -> list[tuple[Image, Image]]:
    """(top, bottom) for each of the SHOT_COUNT shots, by running the program's steps on the model."""
    top_a, top_b, bot_a, bot_b = (bytearray(n) for n in (TOP_SIZE, TOP_SIZE, BOT_SIZE, BOT_SIZE))
    src_a, src_b, src_c = (bytearray(SRC_BYTES) for _ in range(3))
    src_c = draw_card()
    src_d = draw_card_2x()
    tex = draw_texture()
    shots: list[tuple[Image, Image]] = []

    def top(buf, fmt):
        return show_screen(buf, fmt, 240 * BPP[fmt], 400)

    def bottom(buf):
        return show_screen(buf, RGB565, 480, 320)

    F = lambda i, o, f: (i << 8) | (o << 12) | f          # the transfer flags
    # step 1: zeros in the display buffers, then the sources, the bottom screen's 16-bit fill, a transfer
    for buf, size in ((top_a, TOP_SIZE), (top_b, TOP_SIZE), (bot_a, BOT_SIZE), (bot_b, BOT_SIZE)):
        memory_fill(buf, 0, size, 32, 0)
    memory_fill(src_a, 0, SRC_BYTES, 32, FILL32)
    memory_fill(src_b, 0, SRC_BYTES, 24, FILL24)
    memory_fill(bot_a, 0, BOT_BYTES, 16, FILL16)
    display_transfer(src_a, top_a, 240, 400, 240, 400, F(RGBA8, RGB8, 0))
    shots.append((top(top_a, RGB8), bottom(bot_a)))
    # step 2: the same source as RGB565
    display_transfer(src_a, top_a, 240, 400, 240, 400, F(RGBA8, RGB565, 0))
    shots.append((top(top_a, RGB565), bottom(bot_a)))
    # step 3: the 24-bit pattern as RGB8
    display_transfer(src_b, top_a, 240, 400, 240, 400, F(RGBA8, RGB8, 0))
    shots.append((top(top_a, RGB8), bottom(bot_a)))
    # step 4: the pattern as RGB565
    display_transfer(src_b, top_a, 240, 400, 240, 400, F(RGBA8, RGB565, 0))
    shots.append((top(top_a, RGB565), bottom(bot_a)))
    # step 5: the card as RGB565, flipped, into the second buffer
    display_transfer(src_c, top_b, 240, 400, 240, 400, F(RGBA8, RGB565, 1))
    shots.append((top(top_b, RGB565), bottom(bot_a)))
    # step 6: the 480x800 card downscaled to RGB8
    display_transfer(src_d, top_a, 480, 800, 480, 800, F(RGBA8, RGB8, 0) | (2 << 24))
    shots.append((top(top_a, RGB8), bottom(bot_a)))
    # step 7: the texture copied (30 units of 16 bytes per row, a gap of 2) into the other bottom buffer
    texture_copy(tex, bot_b, BOT_BYTES, (2 << 16) | 30, 30)
    shots.append((top(top_a, RGB8), bottom(bot_b)))
    # step 8: both screens swapped: top back to the flipped card, bottom back to the fill
    shots.append((top(top_b, RGB565), bottom(bot_a)))
    # step 9: after VBlank j (j counted from the step's first) the top screen shows TOP_A for odd j
    # and TOP_B for even j; the shots are at j = 31 and j = 92
    shots.append((top(top_a, RGB8), bottom(bot_a)))
    shots.append((top(top_b, RGB565), bottom(bot_a)))
    return shots


_expected_cache: list[tuple[Image, Image]] = []


def expected() -> list[tuple[Image, Image]]:
    if not _expected_cache:
        _expected_cache.extend(expected_shots())
    return _expected_cache


# ── running ───────────────────────────────────────────────────────────────────────────────────────
class Run:
    def __init__(self, lines: list[str], shots: dict[str, bytes]):
        self.lines = lines
        self.shots = shots     # file name -> bytes, in the order of the names (the times)


def debug_lines(text: str) -> list[str]:
    return [m.group(1) for line in ANSI.sub("", text).splitlines() if (m := DEBUG_LINE.search(line))]


def run_program(kind: str, runner: str, elf: str, lib: str | None, work: str, name: str,
                schedule: tuple[int, int] | None) -> Run:
    user = os.path.join(work, f"user_{name}")
    if kind == "reference":
        cmd = [runner, elf, "--cpu", "jit", "--new-3ds", "off", "--ms", str(RUN_MS), "--user", user]
    else:
        cmd = [runner, elf, "--code", lib, "--gpu", os.environ["MH4N_GFX_GPU"], "--ms", str(RUN_MS),
               "--user", user]
    shots_dir = os.path.join(work, f"shots_{name}")
    if schedule:
        cmd += ["--shots", shots_dir, "--shots-from", str(schedule[0]), "--shot-every", str(schedule[1])]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900)
    assert r.returncode == 0, (r.returncode, r.stdout[-3000:])
    shots = {}
    if schedule and os.path.isdir(shots_dir):
        for f in sorted(os.listdir(shots_dir)):
            if f.endswith(".ppm"):
                shots[f] = open(os.path.join(shots_dir, f), "rb").read()
    return Run(debug_lines(r.stdout), shots)


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    """The program's ELF, and (when the native runner needs it) the code library built from it."""
    if not have_toolchain():
        # nothing can run without the program: a failure when any runner is required
        skip_or_fail("any", "clang/lld toolchain not installed")
    import build_svc_test
    out = str(tmp_path_factory.mktemp("gfx_gx"))
    elf = build_svc_test.build(out, PROGRAM)
    libs: dict[str, str] = {}

    def native_library() -> str:
        if "native" not in libs:
            from recomp import build as rbuild
            libs["native"] = rbuild.build(out, 2, "O1", rbuild.native_defines(True, False),
                                          "mh4u_code_native.so", [rbuild.DLL_GLUE],
                                          extra_flags=["-fvisibility=hidden"])
        return libs["native"]
    return elf, native_library, out


def runs_of(kind: str, built, tmp_path_factory) -> list[Run]:
    """Three runs of the program on one runner: the first prints the lines the schedule is worked
    out from, the other two take the shots (and must repeat each other exactly)."""
    elf, native_library, _ = built
    var = "MH4U_RUNNER" if kind == "reference" else "MH4N_RUNNER"
    runner = os.environ.get(var)
    if not runner:
        skip_or_fail(kind, f"{kind} runner not set ({var})")
    if kind == "native" and not os.environ.get("MH4N_GFX_GPU"):
        skip_or_fail(kind, "MH4N_GFX_GPU (the native runner's --gpu) not set")
    lib = native_library() if kind == "native" else None
    work = str(tmp_path_factory.mktemp(f"run_{kind}"))
    first = run_program(kind, runner, elf, lib, work, "1", None)
    from_ms, every_ms, _ = shot_schedule(first.lines)
    return [first] + [run_program(kind, runner, elf, lib, work, str(i), (from_ms, every_ms)) for i in (2, 3)]


@pytest.fixture(scope="module")
def reference(built, tmp_path_factory):
    return runs_of("reference", built, tmp_path_factory)


@pytest.fixture(scope="module")
def native(built, tmp_path_factory):
    return runs_of("native", built, tmp_path_factory)


# ── tests that need no runner ─────────────────────────────────────────────────────────────────────
def test_program_builds_and_recompiles(tmp_path):
    if not have_toolchain():
        pytest.skip("clang/lld toolchain not installed")
    import build_svc_test
    assert PROGRAM in build_svc_test.programs()
    assert os.path.exists(build_svc_test.build(str(tmp_path), PROGRAM))


def c_defines() -> dict[str, int]:
    """The #defines of gfx_gx.c (a constant may name an earlier one: TOP_B is VRAM + 0x100000u)."""
    values: dict[str, int] = {}
    with open(os.path.join(PORT_TESTS, PROGRAM + ".c"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"#define (\w+)\s+(.*?)\s*(?:/\*.*)?$", line.rstrip())
            if not m:
                continue
            expr = re.sub(r"(?<=[0-9a-fA-F])u\b", "", m.group(2))
            expr = re.sub(r"\b[A-Z_][A-Z0-9_]*\b", lambda n: str(values[n.group(0)]) if n.group(0) in values else n.group(0), expr)
            if re.fullmatch(r"[0-9a-fA-Fx+*() ]+", expr):
                values[m.group(1)] = eval(expr)   # noqa: S307  (digits, + * and parentheses only)
    return values


def test_constants_match_the_program():
    """The numbers the model uses are the ones in gfx_gx.c."""
    d = c_defines()
    mine = {"TOP_SIZE": TOP_SIZE, "BOT_SIZE": BOT_SIZE, "SRC_BYTES": SRC_BYTES, "BOT_BYTES": BOT_BYTES,
            "FILL32": FILL32, "FILL24": FILL24, "FILL16": FILL16, "TEX_PITCH": TEX_PITCH,
            "TEX_ROW": TEX_ROW, "PAD_BYTE": PAD_BYTE, "FLIP_VBLANKS": FLIP_VBLANKS, "HOLD_VBLANKS": STEP_FRAMES - 1}
    for name, value in mine.items():
        assert d[name] == value, name
    assert {n: d[n] for n in ("TOP_A", "TOP_B", "BOT_A", "BOT_B")} == {
        "TOP_A": 0x1F000000, "TOP_B": 0x1F100000, "BOT_A": 0x1F200000, "BOT_B": 0x1F300000}
    assert d["TOP_SIZE"] <= 0x100000 and d["BOT_SIZE"] <= 0x100000
    # the linear sources do not overlap, and fit in the block
    spans = sorted([(d["LIN_SRC_A"], SRC_BYTES), (d["LIN_SRC_B"], SRC_BYTES), (d["LIN_SRC_C"], SRC_BYTES),
                    (d["LIN_SRC_D"], 4 * SRC_BYTES), (d["LIN_TEX"], 320 * TEX_PITCH)])
    assert all(a + n <= b for (a, n), (b, _) in zip(spans, spans[1:]))
    assert spans[-1][0] + spans[-1][1] <= d["LIN_SIZE"]
    assert all(a % 8 == 0 for a, _ in spans)      # the GPU's address registers hold address >> 3
    assert 240 * 400 * 3 + 16 <= TOP_SIZE and BOT_BYTES + 16 <= BOT_SIZE
    text = open(os.path.join(PORT_TESTS, PROGRAM + ".c"), encoding="utf-8").read()
    assert re.findall(r'begin_step\("gfx_gx: step (\d) (\w+) vblank"\)', text) == [
        (str(n + 1), name) for n, name in enumerate(STEPS)]


def test_morton_order_is_the_documented_tile():
    """The 8x8 tile of video_core/utils.h: 0 1 4 5 16 17 20 21 along the bottom row, 2 3 6 7 ... above."""
    assert [morton(x, 0) for x in range(8)] == [0, 1, 4, 5, 16, 17, 20, 21]
    assert [morton(0, y) for y in range(8)] == [0, 2, 8, 10, 32, 34, 40, 42]
    assert sorted(morton(x, y) for y in range(8) for x in range(8)) == list(range(64))
    assert tiled_index(8, 0, 240) == 64 and tiled_index(0, 8, 240) == 8 * 240 and tiled_index(7, 7, 240) == 63


def test_colour_conversions():
    assert decode_pixel(RGB565, encode_pixel(RGB565, 0x20, 0x90, 0xE0, 255)) == (0x21, 0x92, 0xE7, 255)
    assert decode_pixel(RGB565, (0xA945).to_bytes(2, "little")) == COLOR16 + (255,)
    assert decode_pixel(RGBA8, (FILL32).to_bytes(4, "little")) == (0x20, 0x90, 0xE0, 0xFF)
    assert encode_pixel(RGB8, 1, 2, 3, 4) == bytes((3, 2, 1)) and decode_pixel(RGB8, bytes((3, 2, 1))) == (1, 2, 3, 255)
    for fmt in (RGB8, RGB565, RGB5A1, RGBA4, RGBA8):     # encode(decode(x)) is a fixed point
        raw = bytes(range(11, 11 + BPP[fmt]))
        assert encode_pixel(fmt, *decode_pixel(fmt, raw)) == encode_pixel(fmt, *decode_pixel(fmt, encode_pixel(fmt, *decode_pixel(fmt, raw))))


def test_fills_texture_copy_and_transfer_models():
    buf = bytearray(12)
    memory_fill(buf, 0, 12, 24, FILL24)
    assert buf == bytes((0x12, 0x34, 0x56)) * 4
    memory_fill(buf, 4, 8, 32, 0x01020304)
    assert buf[4:8] == bytes((4, 3, 2, 1)) and buf[:4] == bytes((0x12, 0x34, 0x56, 0x12))
    memory_fill(buf, 0, 4, 16, 0xA945)
    assert buf[:4] == bytes((0x45, 0xA9, 0x45, 0xA9))
    # a copy with a gap: 3 rows of 16 bytes read from a pitch of 32
    src = bytearray(b"".join(bytes([i]) * 16 + b"\xFF" * 16 for i in range(3)))
    dst = bytearray(48)
    texture_copy(src, dst, 48, (1 << 16) | 1, 1)
    assert dst == b"".join(bytes([i]) * 16 for i in range(3))
    # a copy with a gap on the output side
    dst = bytearray(96)
    texture_copy(bytearray(b"".join(bytes([i + 1]) * 16 for i in range(3))), dst, 48, 1, (1 << 16) | 1)
    assert dst == b"".join(bytes([i + 1]) * 16 + bytes(16) for i in range(3))[:80] + bytes(16)
    # a transfer of one 8x8 tile of uniform colour: RGBA8 -> RGB8
    tile = bytearray((FILL32).to_bytes(4, "little") * 64)
    out = bytearray(8 * 8 * 3)
    display_transfer(tile, out, 8, 8, 8, 8, (RGBA8 << 8) | (RGB8 << 12))
    assert out == bytes((0xE0, 0x90, 0x20)) * 64


def test_the_downscale_averages_four_pixels():
    """ScaleXY: each output pixel is the mean (rounded down) of four consecutive pixels in memory,
    the 2x2 block: here red 0, 1, 2, 3 and green 4 times 255 give red 1 (6 // 4) and green 255."""
    src = bytearray(8 * 8 * 4)
    for y in range(8):
        for x in range(8):
            o = tiled_index(x, y, 8) * 4
            src[o:o + 4] = bytes((255, 0, 255, (x & 1) + 2 * (y & 1)))
    out = bytearray(4 * 4 * 3)
    display_transfer(src, out, 8, 8, 8, 8, (RGBA8 << 8) | (RGB8 << 12) | (2 << 24))
    assert out == bytes((0, 255, 1)) * 16


def colours(image: Image) -> dict[bytes, int]:
    """How many pixels of each colour."""
    counts: dict[bytes, int] = {}
    for i in range(0, len(image.rgb), 3):
        counts[image.rgb[i:i + 3]] = counts.get(image.rgb[i:i + 3], 0) + 1
    return counts


def test_known_pictures():
    """Images worked out by hand, not by the model. Azahar shows, in the last column, row 0 of the
    stored line after the buffer's end; the program leaves zeros there, so it is black."""
    shots = expected()
    assert len(shots) == SHOT_COUNT
    top0, bottom0 = shots[0]
    assert (top0.width, top0.height, bottom0.width, bottom0.height) == (400, 240, 320, 240)
    # step 1: the 32-bit colour (RGBA8 -> RGB8 loses nothing), the bottom's 16-bit fill
    assert colours(top0) == {bytes(COLOR32): 400 * 240 - 1, bytes(3): 1} and top0.pixel(399, 0) == (0, 0, 0)
    assert colours(bottom0) == {bytes(COLOR16): 320 * 240 - 1, bytes(3): 1} and bottom0.pixel(319, 0) == (0, 0, 0)
    # step 2: RGB565 loses the low bits: 0x20 0x90 0xE0 -> 0x21 0x92 0xE7. The pixel after the buffer's
    # end (192000 bytes) is now inside step 1's RGB8 data: its bytes E0 90 read as the 565 value 0x90E0
    assert colours(shots[1][0]) == {bytes((0x21, 0x92, 0xE7)): 400 * 240 - 1, bytes((0x94, 0x1C, 0x00)): 1}
    # step 3: the 24-bit fill writes 12 34 56 over and over, so the 4-byte pixel n (n = the number in
    # the tile order) holds the pattern from byte 4n: n % 3 = 0: 12 34 56 12, 1: 34 56 12 34, 2: 56 12 34 56,
    # and RGBA8 reads (a, b, g, r) from the bytes, giving three colours, one third of the pixels each
    pattern = colours(shots[2][0])
    assert set(pattern) == {bytes((0x12, 0x56, 0x34)), bytes((0x34, 0x12, 0x56)), bytes((0x56, 0x34, 0x12)), bytes(3)}
    assert pattern[bytes(3)] == 1 and all(abs(n - 32000) <= 1 for k, n in pattern.items() if k != bytes(3))
    # step 4: the same three colours, quantised to 5-6-5 bits: the pixel types give (2, 21, 6),
    # (6, 4, 10) and (10, 13, 2), as 5, 6 and 5-bit values
    p565 = colours(shots[3][0])
    quantised = {bytes((c5(0x12 >> 3), c6(0x56 >> 2), c5(0x34 >> 3))), bytes((c5(0x34 >> 3), c6(0x12 >> 2), c5(0x56 >> 3))),
                 bytes((c5(0x56 >> 3), c6(0x34 >> 2), c5(0x12 >> 3)))}
    assert quantised <= set(p565) and len(p565) <= 5
    # step 5's picture and step 6's are different cards; step 8 and the alternating step's second
    # shot show step 5's buffer again, the alternating step's first shot step 6's
    flipped, scaled = shots[4][0], shots[5][0]
    assert flipped != scaled and shots[7][0] == flipped and shots[9][0] == flipped and shots[8][0] == scaled
    # step 7 changes only the bottom screen; the texture's padding never reaches it
    assert shots[6][0] == scaled and shots[6][1] != shots[5][1]
    assert bytes((PAD_BYTE,)) * 3 not in colours(shots[6][1])
    assert shots[7][1] == shots[0][1] == shots[9][1]


def test_the_scaled_card_is_the_card_with_red_plus_one():
    """Step 6's downscale of the doubled card gives the 240x400 card, red as r & 0xFC plus 1."""
    scaled = expected()[5][0]
    # the picture's pixel (col y, row x) is the stored pixel (x_card = row..) of the card; red is
    # a function of the stored x, which is the picture's row, up to the one-pixel shift of row 0
    for row in (1, 50, 120, 239):
        r, g, b = scaled.pixel(200, row)
        card_x = 240 - row                  # Azahar's stored pixel number 240 - x
        want_r = (card_x * 255 // 239 & 0xFC) + 1
        assert r == want_r, (row, r, want_r)


def test_shot_schedule_comes_from_the_printed_lines():
    lines = ["gfx_gx: start", "gfx_gx: ready"]
    for n, name in enumerate(STEPS):
        lines.append(f"gfx_gx: step {n + 1} {name} vblank {4 + 61 * n:08x}")
    lines += ["gfx_gx: flip vblanks 00000078", "gfx_gx: gsp error 00000000", "gfx_gx: done"]
    first, every, starts = shot_schedule(lines)
    assert starts == [4 + 61 * n for n in range(9)]
    assert every == 1020 and first == 591          # (4 + 31.5) frames = 593.3 ms, less 2.3 ms
    ideal = [(4 + 61 * n + 31.5) * FRAME_MS for n in range(SHOT_COUNT)]
    assert all(abs(first + n * every - t) < 2.5 for n, t in enumerate(ideal))
    assert abs(1000 / FRAME_MS - 59.83) < 0.002          # the spec's 59.83 Hz
    with pytest.raises(AssertionError):
        shot_schedule(lines[:3] + lines[4:])         # a step missing
    with pytest.raises(AssertionError):
        shot_schedule([line.replace(f"{4 + 61 * 3:08x}", f"{4 + 61 * 3 + 1:08x}") for line in lines])   # a step late


def test_require_variable(monkeypatch):
    monkeypatch.delenv("MH4N_GFX_REQUIRE", raising=False)
    assert required() == set()
    for value, want in (("reference", {"reference"}), ("native", {"native"}), ("both", {"reference", "native"}),
                        ("Reference,native", {"reference", "native"}), ("", set())):
        monkeypatch.setenv("MH4N_GFX_REQUIRE", value)
        assert required() == want
    monkeypatch.setenv("MH4N_GFX_REQUIRE", "everything")
    with pytest.raises(ValueError):
        required()


def test_a_missing_runner_skips_or_fails(monkeypatch):
    monkeypatch.delenv("MH4N_GFX_REQUIRE", raising=False)
    with pytest.raises(pytest.skip.Exception):
        skip_or_fail("reference", "not set")
    monkeypatch.setenv("MH4N_GFX_REQUIRE", "reference")
    with pytest.raises(pytest.fail.Exception):
        skip_or_fail("reference", "not set")
    with pytest.raises(pytest.skip.Exception):
        skip_or_fail("native", "not set")           # only the reference one is required
    with pytest.raises(pytest.fail.Exception):
        skip_or_fail("any", "no toolchain")
    monkeypatch.setenv("MH4N_GFX_REQUIRE", "both")
    with pytest.raises(pytest.fail.Exception):
        skip_or_fail("native", "not set")


# ── tests that need a runner ──────────────────────────────────────────────────────────────────────
def check_lines(lines: list[str]) -> None:
    assert lines[:2] == ["gfx_gx: start", "gfx_gx: ready"], lines
    _, _, starts = shot_schedule(lines)
    assert starts[0] >= 1
    assert lines[-3:] == ["gfx_gx: flip vblanks %08x" % FLIP_VBLANKS, "gfx_gx: gsp error 00000000",
                          "gfx_gx: done"], lines
    assert len(lines) == 2 + len(STEPS) + 3, lines


def compare_images(got: Image, want: Image, what: str) -> None:
    assert (got.width, got.height) == (want.width, want.height), f"{what}: {got.width}x{got.height}, expected {want.width}x{want.height}"
    if got == want:
        return
    bad = [(i // 3 % got.width, i // 3 // got.width) for i in range(0, len(got.rgb), 3)
           if got.rgb[i:i + 3] != want.rgb[i:i + 3]]
    x, y = bad[0]
    pytest.fail(f"{what}: {len(bad)} of {got.width * got.height} pixels differ, the first at ({x}, {y}): "
                f"got {got.pixel(x, y)}, expected {want.pixel(x, y)}")


def check_run(runs: list[Run]) -> None:
    first, second, third = runs
    check_lines(first.lines)
    assert second.lines == first.lines and third.lines == first.lines, "the printed lines differ between runs"
    assert second.shots == third.shots, "the shots of two identical runs differ"
    tops = [n for n in second.shots if n.endswith("_top.ppm")]
    bottoms = [n for n in second.shots if n.endswith("_bottom.ppm")]
    assert len(tops) >= SHOT_COUNT and len(bottoms) >= SHOT_COUNT, sorted(second.shots)
    for k, (want_top, want_bottom) in enumerate(expected()):
        for names, want, label in ((tops, want_top, "top"), (bottoms, want_bottom, "bottom")):
            path = names[k]
            compare_images(parse_ppm(second.shots[path]), want, f"shot {k + 1} ({path}), {label} screen")


def test_reference_runner_prints_the_lines_and_shows_the_pictures(reference):
    check_run(reference)


def test_native_runner_prints_the_lines_and_shows_the_pictures(native):
    check_run(native)


def test_the_runners_show_the_same_shots(reference, native):
    """The shots of the first SHOT_COUNT times, top and bottom, byte for byte (the names carry each
    runner's own times, so the files are paired in order)."""
    def shots(runs: list[Run]) -> list[bytes]:
        return [runs[1].shots[n] for n in sorted(runs[1].shots)][:2 * SHOT_COUNT]
    ref, nat = shots(reference), shots(native)
    assert len(ref) == len(nat) == 2 * SHOT_COUNT
    for i, (a, b) in enumerate(zip(ref, nat)):
        assert a == b, f"shot {i // 2 + 1}, {'top' if i % 2 else 'bottom'} screen, differs between the runners"
