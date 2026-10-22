"""The native runner's screenshots (N2b-14): --shots, --shot-every and --shots-from.

Runs our own program port/tests/io_test.c (it draws a known picture into the top screen's
framebuffer with the CPU: a green band, a red left half, a blue right half) on the native runner
with `--gpu null-display`, the same picture and the same pixel checks as
tests/test_port.py::test_runner_scripted_input_and_screenshots makes of the reference runner.

Needs MH4N_RUNNER (the native runner, build/native/bin/mh4u_native_runner) and the freestanding 3DS
toolchain of port/tests (clang, ld.lld, llvm-objcopy, llvm-readelf) to build the program and its
code library; the whole file skips without the variable. Synthetic data only; no game files.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "port", "tests"))

NEEDED_TOOLS = ["clang", "ld.lld", "llvm-objcopy", "llvm-readelf"]
RUNNER = os.environ.get("MH4N_RUNNER", "")

pytestmark = pytest.mark.skipif(not RUNNER, reason="native runner not set (MH4N_RUNNER)")


@pytest.fixture(scope="module")
def program(tmp_path_factory):
    """(io_test's ELF, the native code library built from it)."""
    missing = [t for t in NEEDED_TOOLS if not shutil.which(t)]
    if missing:
        pytest.fail(f"MH4N_RUNNER is set but the 3DS toolchain is missing: {missing}")
    import build_svc_test
    from recomp import build as rbuild
    out = str(tmp_path_factory.mktemp("io_test"))
    elf = build_svc_test.build(out, "io_test")
    lib = rbuild.build(out, 2, "O1", rbuild.native_defines(True, False), "mh4u_code_native.so",
                       [rbuild.DLL_GLUE], extra_flags=["-fvisibility=hidden"])
    return elf, lib


def run(program, work, *args, check=True):
    elf, lib = program
    cmd = [RUNNER, elf, "--code", lib, "--user", os.path.join(work, "user"), *args]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
    if check:
        assert r.returncode == 0, (r.returncode, r.stdout[-3000:])
    return r


def read_ppm(path):
    data = open(path, "rb").read()
    magic, size, maxval, pixels = data.split(b"\n", 3)
    width, height = map(int, size.split())
    assert (magic, maxval) == (b"P6", b"255")
    assert len(pixels) == width * height * 3
    return width, height, pixels


def shots_of(directory):
    return sorted(f for f in os.listdir(directory) if f.endswith(".ppm"))


def test_help_lists_the_options():
    out = subprocess.run([RUNNER, "--help"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         timeout=60).stdout
    for option in ("--shots", "--shots-from", "--shot-every"):
        assert any(line.split()[:1] == [option] for line in out.splitlines()), option


def test_default_schedule_and_the_picture(program, tmp_path):
    """Every 1000 ms from 1000 ms, named by the time; the picture of io_test (test_port's checks).
    io_test polls 150 times, 16.7 ms apart, and exits at about 2517 ms: the shots at 1000 and 2000."""
    shots = tmp_path / "shots"
    run(program, str(tmp_path), "--gpu", "null-display", "--ms", "3500", "--shots", str(shots))
    assert shots_of(shots) == [f"000{t}000_{s}.ppm" for t in (1, 2) for s in ("bottom", "top")]
    width, height, pixels = read_ppm(shots / "0001000_top.ppm")
    assert (width, height) == (400, 240)

    def pixel(x, y):
        i = (y * width + x) * 3
        return tuple(pixels[i:i + 3])
    assert pixel(100, 5) == (0, 255, 0) and pixel(100, 120) == (255, 0, 0) and pixel(300, 120) == (0, 0, 255)
    bw, bh, _ = read_ppm(shots / "0001000_bottom.ppm")
    assert (bw, bh) == (320, 240)
    # the later shot of an unchanging picture is the same file
    assert (shots / "0002000_top.ppm").read_bytes() == (shots / "0001000_top.ppm").read_bytes()
    # no gas without the renderer; the file is there, empty
    assert (shots / "gas.txt").read_text() == ""


def test_shots_from_and_shot_every_in_milliseconds(program, tmp_path):
    shots = tmp_path / "shots"
    run(program, str(tmp_path), "--gpu", "null-display", "--ms", "1300", "--shots", str(shots),
        "--shots-from", "500", "--shot-every", "500")
    assert shots_of(shots) == ["0000500_bottom.ppm", "0000500_top.ppm", "0001000_bottom.ppm", "0001000_top.ppm"]
    assert read_ppm(shots / "0000500_top.ppm")[:2] == (400, 240)


def test_a_shot_after_the_end_of_the_run_is_not_taken(program, tmp_path):
    shots = tmp_path / "shots"
    run(program, str(tmp_path), "--gpu", "null-display", "--ms", "800", "--shots", str(shots))
    assert shots_of(shots) == []
    assert (shots / "gas.txt").exists()


def test_a_program_without_the_frame_counter_has_no_frame_keyed_shots(program, tmp_path):
    """io_test does not map the game's frame counter (frame 0 all along): `fN` never comes due."""
    shots = tmp_path / "shots"
    run(program, str(tmp_path), "--gpu", "null-display", "--ms", "1200", "--shots", str(shots),
        "--shot-every", "f30")
    assert shots_of(shots) == []


def test_shots_need_a_gpu_that_shows_its_screens(program, tmp_path):
    shots = tmp_path / "shots"
    r = run(program, str(tmp_path), "--ms", "500", "--shots", str(shots), check=False)   # the default GPU: null
    assert r.returncode == 2
    assert "null-display" in r.stdout and "--shots" in r.stdout
    assert not shots.exists()


@pytest.mark.parametrize("args", [["--shot-every", "f0"], ["--shot-every", "often"], ["--shots-from", "soon"]])
def test_a_bad_schedule_ends_with_status_2(program, tmp_path, args):
    r = run(program, str(tmp_path), "--gpu", "null-display", "--ms", "500", "--shots", str(tmp_path / "s"),
            *args, check=False)
    assert r.returncode == 2
    assert args[0] in r.stdout
