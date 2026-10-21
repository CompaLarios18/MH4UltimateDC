"""port/app/kbm.cpp: the keyboard-and-mouse mode's arithmetic. The mouse's speed becomes a push
on the C-stick (the camera), and the wheel scrolls the item bar the game's own way: L held, then
Y (left) or A (right) pressed and let go, each step held long enough for the game to see it."""
from __future__ import annotations

import math
import os
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "kbm.h"

using namespace App::Kbm;

// camera SENS VERTICAL INVERT DEADZONE DT DX,DY...: one tick per DX,DY, "x y" per tick
// scroll N...: the item bar's scrolls queued (-1 left, 1 right), then ticks until idle: a line
//              per tick with its events, "+code" pressed and "-code" let go
// pulse: a wheel's key started twice, ticks until let go
int main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "camera") {
        CameraSettings s;
        s.sensitivity = std::atoi(argv[2]);
        s.vertical = std::atoi(argv[3]);
        s.invert_y = std::atoi(argv[4]) != 0;
        s.deadzone = std::atoi(argv[5]);
        const double dt = std::atof(argv[6]);
        MouseCamera camera;
        for (int i = 7; i < argc; ++i) {
            double dx = 0, dy = 0;
            std::sscanf(argv[i], "%lf,%lf", &dx, &dy);
            camera.Motion(dx, dy);
            const auto [x, y] = camera.Tick(dt, s);
            std::printf("%.6f %.6f\n", x, y);
        }
        return 0;
    }
    if (mode == "scroll") {
        ItemScroll scroll(1, 2, 3);  // L, Y, A
        for (int i = 2; i < argc; ++i) {
            scroll.Queue(std::atoi(argv[i]));
        }
        for (int frame = 0; frame < 200 && !(frame > 0 && scroll.Idle()); ++frame) {
            for (const auto& [code, pressed] : scroll.Tick()) {
                std::printf("%c%d ", pressed ? '+' : '-', code);
            }
            std::printf("\n");
        }
        return 0;
    }
    Pulses pulses(4);
    for (const auto& [code, pressed] : pulses.Start(7)) std::printf("%c%d ", pressed ? '+' : '-', code);
    std::printf("\n");
    for (int frame = 0; frame < 10; ++frame) {
        if (frame == 1) {
            for (const auto& [code, pressed] : pulses.Start(7)) std::printf("%c%d ", pressed ? '+' : '-', code);
        }
        for (const auto& [code, pressed] : pulses.Tick()) std::printf("%c%d ", pressed ? '+' : '-', code);
        std::printf("\n");
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("kbm")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(APP, "kbm.cpp"), "-o", str(exe)],
                   check=True)
    return exe


def camera(driver, *moves, sens=50, vertical=100, invert=0, deadzone=0, dt=1 / 60):
    out = subprocess.run([str(driver), "camera", str(sens), str(vertical), str(invert), str(deadzone), str(dt),
                          *(f"{dx},{dy}" for dx, dy in moves)], capture_output=True, text=True, check=True).stdout
    return [tuple(float(v) for v in line.split()) for line in out.splitlines()]


def events(driver, *args):
    out = subprocess.run([str(driver), *args], capture_output=True, text=True, check=True).stdout
    return [line.split() for line in out.splitlines()]


def test_still_mouse_leaves_the_stick_centred(driver):
    assert camera(driver, (0, 0), (0, 0), deadzone=12) == [(0.0, 0.0), (0.0, 0.0)]


def test_steady_move_settles_on_its_speed(driver):
    # 12.5 pixels a frame at 60 frames a second: 750 px/s, half of sensitivity 50's 1500
    ticks = camera(driver, *[(12.5, 0)] * 30)
    assert ticks[-1][0] == pytest.approx(0.5, abs=0.01)
    assert ticks[-1][1] == 0.0
    # a higher sensitivity pushes further for the same speed
    assert camera(driver, *[(12.5, 0)] * 30, sens=100)[-1][0] == pytest.approx(1.0, abs=0.01)


def test_mouse_up_pushes_the_stick_up_unless_inverted(driver):
    assert camera(driver, *[(0, -10)] * 30)[-1][1] > 0.3
    assert camera(driver, *[(0, -10)] * 30, invert=1)[-1][1] < -0.3
    # up and down at half the speed of left and right
    half = camera(driver, *[(0, -10)] * 30, vertical=50)[-1][1]
    assert half == pytest.approx(camera(driver, *[(0, -10)] * 30)[-1][1] / 2, abs=0.01)


def test_push_is_never_longer_than_the_stick(driver):
    for x, y in camera(driver, *[(400, -400)] * 10, sens=100):
        assert math.hypot(x, y) <= 1.0 + 1e-6


def test_small_moves_get_past_the_dead_zone(driver):
    x, _ = camera(driver, *[(1, 0)] * 30, deadzone=12)[-1]
    assert 0.12 < x < 0.2
    assert camera(driver, *[(1, 0)] * 30, deadzone=0)[-1][0] < 0.08


def test_the_turn_is_kept_when_the_mouse_stops(driver):
    # a flick is spread over the next frames, not lost: the stick comes back to the centre soon
    ticks = camera(driver, (60, 0), *[(0, 0)] * 30)
    assert ticks[0][0] > 0
    assert sum(1 for x, _ in ticks if x > 0) >= 3
    assert ticks[-1] == (0.0, 0.0)


def test_scroll_holds_l_around_y_or_a(driver):
    lines = events(driver, "scroll", "-1")
    flat = [(i, e) for i, line in enumerate(lines) for e in line]
    assert [e for _, e in flat] == ["+1", "+2", "-2", "-1"]  # L, Y, Y up, L up
    frames = [i for i, _ in flat]
    # each step lasts at least 3 frames, so a game at 30 fps in a 60 Hz loop sees it
    assert all(b - a >= 3 for a, b in zip(frames, frames[1:]))


def test_scrolls_in_a_row_keep_l_held(driver):
    seq = [e for line in events(driver, "scroll", "1", "1", "1") for e in line]
    assert seq == ["+1", "+3", "-3", "+3", "-3", "+3", "-3", "-1"]


def test_scrolls_the_other_way_cancel(driver):
    lines = events(driver, "scroll", "1", "-1")
    assert [e for line in lines for e in line] == []


def test_scrolls_queue_at_most_five(driver):
    seq = [e for line in events(driver, "scroll", *["-1"] * 9) for e in line]
    assert seq.count("+2") == 5


def test_wheel_pulse_is_let_go_after_a_few_frames(driver):
    lines = events(driver, "pulse")
    assert lines[0] == ["+7"]
    # started again while held: no second press, held 4 frames from then
    flat = [(i, e) for i, line in enumerate(lines) for e in line]
    assert [e for _, e in flat] == ["+7", "-7"]
    assert flat[1][0] == 1 + 1 + 4 - 1  # released on the 4th tick after the restart
