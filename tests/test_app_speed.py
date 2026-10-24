"""port/app/speed.cpp: the app's pause and fast forward for playing alone (pause_key,
fast_forward_key, fast_forward_speed, fast_forward_hold in mh4u-app.ini): what the keys do, the
speed the main loop runs the game at, and why the keys do nothing in a room."""
from __future__ import annotations

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
#include <string>
#include "speed.h"

// argv: SPEED HOLD, then steps, each printing "paused<TAB>limit<TAB>badge":
//   p+ p- f+ f- (the keys down and up), room / alone, wait:MS (the clock moves on)
int main(int argc, char** argv) {
    App::Speed::Control c;
    c.Configure(std::atoi(argv[1]), std::atoi(argv[2]) != 0);
    unsigned now = 1000;
    for (int i = 3; i < argc; ++i) {
        const std::string step = argv[i];
        if (step == "p+") c.PauseKey(true, now);
        else if (step == "p-") c.PauseKey(false, now);
        else if (step == "f+") c.FastForwardKey(true, now);
        else if (step == "f-") c.FastForwardKey(false, now);
        else if (step == "room") c.SetInRoom(true, now);
        else if (step == "alone") c.SetInRoom(false, now);
        else if (step.rfind("wait:", 0) == 0) now += std::atoi(step.c_str() + 5);
        std::printf("%d\t%d\t%s\n", c.Paused() ? 1 : 0, c.Limit(), c.Badge(now).c_str());
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("speed")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(APP, "speed.cpp"), "-o", str(exe)],
                   check=True)
    return exe


def run(driver, speed, hold, *steps):
    out = subprocess.run([str(driver), str(speed), "1" if hold else "0", *steps], capture_output=True, text=True,
                         check=True).stdout
    rows = []
    for line in out.splitlines():
        paused, limit, badge = (line.split("\t") + [""])[:3]
        rows.append((paused == "1", int(limit), badge))
    return rows


def test_pause_key_toggles_on_press(driver):
    rows = run(driver, 200, False, "p+", "p-", "p+", "p-")
    assert [r[0] for r in rows] == [True, True, False, False]
    assert rows[0][2] == "Paused"
    assert rows[3][2] == ""


def test_held_pause_key_doesnt_repeat(driver):
    # key repeat, or a second down event without an up: still one press
    rows = run(driver, 200, False, "p+", "p+", "p+")
    assert [r[0] for r in rows] == [True, True, True]


def test_fast_forward_toggle(driver):
    rows = run(driver, 200, False, "f+", "f-", "f+", "f-")
    assert [r[1] for r in rows] == [200, 200, -1, -1]
    assert rows[0][2] == "Fast forward 2x"


def test_fast_forward_hold(driver):
    rows = run(driver, 300, True, "f+", "f-")
    assert [r[1] for r in rows] == [300, -1]
    assert rows[0][2] == "Fast forward 3x"


def test_pause_stops_fast_forward_until_unpaused(driver):
    rows = run(driver, 200, False, "f+", "f-", "p+", "p-", "p+", "p-")
    assert rows[2] == (True, -1, "Paused")
    # back from the pause: fast forward still on
    assert rows[4] == (False, 200, "Fast forward 2x")


def test_fast_forward_ends_a_pause(driver):
    rows = run(driver, 200, False, "p+", "p-", "f+")
    assert rows[-1] == (False, 200, "Fast forward 2x")


def test_unlimited_and_odd_speeds(driver):
    assert run(driver, 0, False, "f+")[0][1:] == (0, "Fast forward max")
    assert run(driver, 150, False, "f+")[0][1:] == (150, "Fast forward 1.5x")
    assert run(driver, 125, False, "f+")[0][1:] == (125, "Fast forward 1.25x")
    # below 110% isn't fast forward: the least is 110
    assert run(driver, 50, False, "f+")[0][1] == 110
    assert run(driver, 5000, False, "f+")[0][1] == 1000


def test_keys_do_nothing_in_a_room_and_say_why(driver):
    rows = run(driver, 200, False, "room", "p+", "p-", "f+", "f-")
    assert all(not r[0] and r[1] == -1 for r in rows)
    assert "not in a room" in rows[1][2]
    assert "not in a room" in rows[3][2]


def test_hold_key_in_a_room_stays_off(driver):
    rows = run(driver, 200, True, "room", "f+", "alone")
    assert rows[1][1] == -1
    # leaving the room doesn't start it while the key is still held from before
    assert rows[2][1] == -1


def test_joining_a_room_ends_pause_and_fast_forward(driver):
    rows = run(driver, 200, False, "f+", "f-", "p+", "room")
    assert rows[-1][0] is False and rows[-1][1] == -1
    assert rows[-1][2].startswith("In a room")
    # and once out of the room again the keys work
    rows = run(driver, 200, False, "room", "alone", "p+")
    assert rows[-1][0] is True


def test_notice_fades(driver):
    rows = run(driver, 200, False, "room", "p+", "wait:2000", "wait:2000")
    assert "not in a room" in rows[2][2]
    assert rows[3][2] == ""
