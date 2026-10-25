#!/usr/bin/env python3
"""Compare two PPM screenshots with ROADMAP section 6's rule (N2b-17).

The rule is `CompareImages` of native/src/gpu_capture/image.cpp, in Python: a pair passes when the PSNR over the
R, G and B values is at least 40 dB and no 16x16 block has a mean absolute error above 8/255 (blocks at the
right and bottom edges are smaller when the size is not a multiple of 16; their mean is over the pixels they
have), so a missing effect cannot hide in the average.  The thresholds are read from `image.h` by a test.

    imgdiff.py A.ppm B.ppm

prints `psnr=<dB> worst_block=<0-255> at=<x>,<y> pass|fail` (the PSNR is `inf` for identical images; `at` is the
top-left pixel of the worst block) and exits 0 when the pair passes, 1 when it fails and 2 when an image cannot be
read or the sizes differ.  Other tools import `read_ppm`, `compare` and `Metrics`.  Standard library only.
"""
from __future__ import annotations

import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence

MIN_PSNR_DB = 40.0          # kMinPsnrDb
MAX_BLOCK_ERROR = 8.0       # kMaxBlockError, out of 255
BLOCK_SIZE = 16             # kBlockSize
MAX_SIDE = 16384            # ReadPpm's limit


class ImageError(Exception):
    """An image that cannot be read, or two images that cannot be compared (exit status 2)."""


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    rgb: bytes          # 3 bytes per pixel, row-major, top row first


@dataclass(frozen=True)
class Metrics:
    psnr: float             # dB over R, G and B; infinity when identical
    worst_block: float      # the largest block mean of |a - b| over R, G and B, out of 255
    x: int                  # that block's top-left pixel
    y: int

    @property
    def passes(self) -> bool:
        return self.psnr >= MIN_PSNR_DB and self.worst_block <= MAX_BLOCK_ERROR

    def text(self) -> str:
        psnr = "inf" if math.isinf(self.psnr) else f"{self.psnr:.2f}"
        return (f"psnr={psnr} worst_block={self.worst_block:.2f} at={self.x},{self.y} "
                f"{'pass' if self.passes else 'fail'}")


def parse_ppm(data: bytes, name: str = "image") -> Image:
    """A binary PPM (P6, maxval 255); comments in the header are allowed."""
    pos = 0
    tokens: list[bytes] = []
    while len(tokens) < 4:
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        if start == pos:
            raise ImageError(f"{name}: truncated PPM header")
        tokens.append(data[start:pos])
    if tokens[0] != b"P6":
        raise ImageError(f"{name}: not a binary PPM (P6)")
    try:
        width, height, maxval = (int(t) for t in tokens[1:])
    except ValueError:
        raise ImageError(f"{name}: malformed PPM header") from None
    if maxval != 255 or not (0 < width <= MAX_SIDE) or not (0 < height <= MAX_SIDE):
        raise ImageError(f"{name}: unsupported PPM ({width}x{height}, maxval {maxval})")
    pos += 1                    # the single whitespace byte after maxval
    size = width * height * 3
    if len(data) - pos < size:
        raise ImageError(f"{name}: truncated PPM pixel data")
    return Image(width, height, bytes(data[pos:pos + size]))


def read_ppm(path: str | Path) -> Image:
    try:
        data = Path(path).read_bytes()
    except OSError as error:
        raise ImageError(f"cannot read {path}: {error.strerror or error}") from None
    return parse_ppm(data, str(path))


def compare(a: Image, b: Image) -> Metrics:
    """`CompareImages`: PSNR over the three channels and the worst 16x16 block mean."""
    if (a.width, a.height) != (b.width, b.height):
        raise ImageError(f"different sizes: {a.width}x{a.height} and {b.width}x{b.height}")
    w, h = a.width, a.height
    row_bytes = w * 3
    blocks_x = (w + BLOCK_SIZE - 1) // BLOCK_SIZE
    blocks_y = (h + BLOCK_SIZE - 1) // BLOCK_SIZE
    squared = 0
    worst, wx, wy = 0.0, 0, 0
    for by in range(blocks_y):
        sums = [0] * blocks_x
        rows = min(BLOCK_SIZE, h - by * BLOCK_SIZE)
        for y in range(by * BLOCK_SIZE, by * BLOCK_SIZE + rows):
            ra = a.rgb[y * row_bytes:(y + 1) * row_bytes]
            rb = b.rgb[y * row_bytes:(y + 1) * row_bytes]
            if ra == rb:
                continue
            diffs = [abs(p - q) for p, q in zip(ra, rb)]
            squared += sum(d * d for d in diffs)
            step = BLOCK_SIZE * 3
            for bx in range(blocks_x):
                sums[bx] += sum(diffs[bx * step:(bx + 1) * step])
        for bx in range(blocks_x):
            cols = min(BLOCK_SIZE, w - bx * BLOCK_SIZE)
            mean = sums[bx] / (cols * rows * 3.0)
            if mean > worst:
                worst, wx, wy = mean, bx * BLOCK_SIZE, by * BLOCK_SIZE
    mse = squared / (w * h * 3.0)
    psnr = math.inf if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    return Metrics(psnr, worst, wx, wy)


def compare_files(path_a: str | Path, path_b: str | Path) -> Metrics:
    return compare(read_ppm(path_a), read_ppm(path_b))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if args in (["-h"], ["--help"]):
        print(__doc__.strip())
        return 0
    if len(args) != 2:
        print("usage: imgdiff.py A.ppm B.ppm", file=sys.stderr)
        return 2
    try:
        metrics = compare_files(args[0], args[1])
    except ImageError as error:
        print(f"imgdiff: {error}", file=sys.stderr)
        return 2
    print(metrics.text())
    return 0 if metrics.passes else 1


if __name__ == "__main__":
    sys.exit(main())
