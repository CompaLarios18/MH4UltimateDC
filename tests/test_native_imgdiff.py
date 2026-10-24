"""tools/native/imgdiff.py (N2b-17): ROADMAP section 6's image rule on synthetic PPMs only."""
from __future__ import annotations

import math
import random
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "native" / "imgdiff.py"
sys.path.insert(0, str(ROOT / "tools" / "native"))

import imgdiff  # noqa: E402


def ppm(width: int, height: int, pixel=(10, 20, 30), edits=None, comment: bool = False) -> bytes:
    """A P6 image of one colour; `edits` maps (x, y) to another colour."""
    rows = bytearray()
    for y in range(height):
        for x in range(width):
            rows += bytes((edits or {}).get((x, y), pixel))
    head = b"P6\n" + (b"# made by a test\n" if comment else b"") + f"{width} {height}\n255\n".encode()
    return head + bytes(rows)


def write(path: Path, data: bytes) -> Path:
    path.write_bytes(data)
    return path


def run_tool(*args) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True, check=False)


def block_edit(x0: int, y0: int, delta: int, size: int = 16, base=(10, 20, 30)) -> dict:
    return {(x, y): tuple(c + delta for c in base) for x in range(x0, x0 + size) for y in range(y0, y0 + size)}


# ── the thresholds are image.h's ─────────────────────────────────────────────

def header_constant(name: str) -> float:
    text = (ROOT / "native" / "src" / "gpu_capture" / "image.h").read_text(encoding="utf-8")
    m = re.search(rf"\b{name}\s*=\s*([0-9.]+)\s*;", text)
    assert m, f"{name} not found in image.h"
    return float(m.group(1))


def test_the_thresholds_equal_image_h() -> None:
    assert imgdiff.MIN_PSNR_DB == header_constant("kMinPsnrDb") == 40.0
    assert imgdiff.MAX_BLOCK_ERROR == header_constant("kMaxBlockError") == 8.0
    assert imgdiff.BLOCK_SIZE == int(header_constant("kBlockSize")) == 16


# ── the rule ─────────────────────────────────────────────────────────────────

def test_identical_images_pass_with_infinite_psnr() -> None:
    a = imgdiff.parse_ppm(ppm(40, 32))
    m = imgdiff.compare(a, a)
    assert math.isinf(m.psnr) and m.worst_block == 0 and m.passes
    assert m.text() == "psnr=inf worst_block=0.00 at=0,0 pass"


def test_one_wrong_pixel_passes_both_rules() -> None:
    a = imgdiff.parse_ppm(ppm(400, 240))
    b = imgdiff.parse_ppm(ppm(400, 240, edits={(200, 100): (255, 255, 255)}))
    m = imgdiff.compare(a, b)
    assert m.psnr >= 40.0 and m.worst_block <= 8.0 and m.passes
    assert (m.x, m.y) == (192, 96)              # the block that holds the pixel


def test_a_block_off_by_10_fails_the_block_rule_with_psnr_above_40() -> None:
    a = imgdiff.parse_ppm(ppm(400, 240))
    b = imgdiff.parse_ppm(ppm(400, 240, edits=block_edit(64, 48, 10)))
    m = imgdiff.compare(a, b)
    assert m.psnr > 40.0 and m.worst_block == pytest.approx(10.0) and not m.passes
    assert (m.x, m.y) == (64, 48)
    assert m.text().endswith("at=64,48 fail")


def test_a_block_off_by_8_passes_and_9_fails() -> None:
    a = imgdiff.parse_ppm(ppm(64, 64))
    assert imgdiff.compare(a, imgdiff.parse_ppm(ppm(64, 64, edits=block_edit(0, 0, 8)))).passes
    assert not imgdiff.compare(a, imgdiff.parse_ppm(ppm(64, 64, edits=block_edit(0, 0, 9)))).passes


def test_a_spread_error_fails_the_psnr_rule_with_every_block_under_8() -> None:
    # every pixel off by 7 on every channel: block mean 7 (passes), PSNR 20*log10(255/7) = 31.2 dB
    a = imgdiff.parse_ppm(ppm(48, 48))
    b = imgdiff.parse_ppm(ppm(48, 48, pixel=(17, 27, 37)))
    m = imgdiff.compare(a, b)
    assert m.worst_block == pytest.approx(7.0) and m.psnr == pytest.approx(20 * math.log10(255 / 7))
    assert not m.passes


def test_edge_blocks_are_averaged_over_the_pixels_they_have() -> None:
    # 20x20: the right and bottom blocks are 4 pixels wide/high; a fully wrong 4x4 corner block has mean 20
    a = imgdiff.parse_ppm(ppm(20, 20))
    b = imgdiff.parse_ppm(ppm(20, 20, edits=block_edit(16, 16, 20, size=4)))
    m = imgdiff.compare(a, b)
    assert m.worst_block == pytest.approx(20.0) and (m.x, m.y) == (16, 16) and not m.passes


def test_the_result_matches_a_direct_computation_on_random_images() -> None:
    rng = random.Random(7)
    w, h = 37, 21
    pa = bytes(rng.randrange(256) for _ in range(w * h * 3))
    pb = bytes(max(0, min(255, c + rng.randrange(-6, 7))) for c in pa)
    m = imgdiff.compare(imgdiff.Image(w, h, pa), imgdiff.Image(w, h, pb))
    squared = sum((x - y) ** 2 for x, y in zip(pa, pb))
    assert m.psnr == pytest.approx(10 * math.log10(255 * 255 / (squared / (w * h * 3))))
    worst = 0.0
    for by in range(0, h, 16):
        for bx in range(0, w, 16):
            total = count = 0
            for y in range(by, min(by + 16, h)):
                for x in range(bx, min(bx + 16, w)):
                    for c in range(3):
                        total += abs(pa[(y * w + x) * 3 + c] - pb[(y * w + x) * 3 + c])
                        count += 1
            worst = max(worst, total / count)
    assert m.worst_block == pytest.approx(worst)


# ── reading ──────────────────────────────────────────────────────────────────

def test_a_header_comment_is_allowed() -> None:
    image = imgdiff.parse_ppm(ppm(3, 2, comment=True))
    assert (image.width, image.height, len(image.rgb)) == (3, 2, 18)


@pytest.mark.parametrize("data", [b"", b"P6\n3 2\n", b"P3\n1 1\n255\n0 0 0\n", b"P6\n1 1\n65535\n\0\0\0\0\0\0",
                                  b"P6\n2 2\n255\n\0\0\0", b"P6\nx 2\n255\n"])
def test_unreadable_images_are_refused(data: bytes) -> None:
    with pytest.raises(imgdiff.ImageError):
        imgdiff.parse_ppm(data)


def test_different_sizes_cannot_be_compared() -> None:
    with pytest.raises(imgdiff.ImageError):
        imgdiff.compare(imgdiff.parse_ppm(ppm(4, 4)), imgdiff.parse_ppm(ppm(4, 5)))


# ── the command line ─────────────────────────────────────────────────────────

def test_exit_status_0_for_a_pair_that_passes(tmp_path: Path) -> None:
    a, b = write(tmp_path / "a.ppm", ppm(400, 240)), write(tmp_path / "b.ppm", ppm(400, 240, edits={(5, 5): (0, 0, 0)}))
    done = run_tool(a, b)
    assert done.returncode == 0 and re.fullmatch(r"psnr=\d+\.\d\d worst_block=\d+\.\d\d at=0,0 pass\n", done.stdout)


def test_exit_status_1_for_a_pair_that_fails(tmp_path: Path) -> None:
    a = write(tmp_path / "a.ppm", ppm(400, 240))
    b = write(tmp_path / "b.ppm", ppm(400, 240, edits=block_edit(32, 32, 10)))
    done = run_tool(a, b)
    assert done.returncode == 1 and done.stdout.strip().endswith("at=32,32 fail")


def test_exit_status_2_for_unreadable_or_different_size_images(tmp_path: Path) -> None:
    a = write(tmp_path / "a.ppm", ppm(8, 8))
    assert run_tool(a, tmp_path / "missing.ppm").returncode == 2
    assert run_tool(a, write(tmp_path / "junk.ppm", b"not an image")).returncode == 2
    done = run_tool(a, write(tmp_path / "c.ppm", ppm(8, 9)))
    assert done.returncode == 2 and "different sizes" in done.stderr and done.stdout == ""
    assert run_tool(a).returncode == 2


def test_the_module_function_reads_files(tmp_path: Path) -> None:
    a = write(tmp_path / "a.ppm", ppm(16, 16))
    b = write(tmp_path / "b.ppm", ppm(16, 16, pixel=(10, 20, 31)))
    m = imgdiff.compare_files(a, b)
    assert m.passes and m.worst_block == pytest.approx(1 / 3)
