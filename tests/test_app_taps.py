"""port/app/taps.cpp: the app's touch taps (mh4u-app.ini's touch_tap_1 to touch_tap_8, "Z @ 52 220
Mega potion"): keys that touch the touch screen at a point while they are held. How a tap's line is
read and written, and the rule that keeps a key on one input only: a tap's key leaves the other
bindings (a stick's direction keeps its place), the app's own keys stay theirs, and a key on two
taps stays on the first."""
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
#include <utility>
#include <vector>
#include "taps.h"

using namespace App::Taps;

static bool Starts(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// parse LINE: "keys<TAB>x<TAB>y<TAB>name", or "invalid"
// format KEYS X Y NAME: the tap's line
// withkey KEYS SLOT KEY, same A B: what they give
// scenario ITEM... ACTION: items tap=LINE, bind=NAME=KEYS, stick=NAME=KEYS, own=KEYS; the action
//   settle, give=TAP,SLOT,KEY, release=KEY or isown=KEY; then "log: ..." lines (settle), "had: 0/1"
//   (release), "own: 0/1" (isown), and the taps, bindings and sticks after it
int main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "parse") {
        Tap tap;
        if (!Parse(argv[2], tap)) {
            std::printf("invalid\n");
            return 0;
        }
        std::printf("%s\t%d\t%d\t%s\n", tap.keys.c_str(), tap.x, tap.y, tap.name.c_str());
        return 0;
    }
    if (mode == "format") {
        Tap tap;
        tap.keys = argv[2];
        tap.x = std::atoi(argv[3]);
        tap.y = std::atoi(argv[4]);
        tap.name = argv[5];
        std::printf("%s\n", Format(tap).c_str());
        return 0;
    }
    if (mode == "withkey") {
        std::printf("%s\n", WithKey(argv[2], std::atoi(argv[3]), argv[4]).c_str());
        return 0;
    }
    if (mode == "same") {
        std::printf("%d\n", Same(argv[2], argv[3]) ? 1 : 0);
        return 0;
    }
    std::vector<Tap> taps;
    std::vector<std::pair<std::string, std::string>> binds, sticks;
    Others others;
    std::string action;
    for (int i = 2; i < argc; ++i) {
        const std::string item = argv[i];
        if (Starts(item, "tap=")) {
            Tap tap;
            if (!Parse(item.substr(4), tap)) {
                std::printf("not a tap: %s\n", item.c_str());
                return 1;
            }
            taps.push_back(tap);
        } else if (Starts(item, "bind=") || Starts(item, "stick=")) {
            const bool stick = Starts(item, "stick=");
            const std::string rest = item.substr(stick ? 6 : 5);
            const std::size_t eq = rest.find('=');
            (stick ? sticks : binds).emplace_back(rest.substr(0, eq), rest.substr(eq + 1));
        } else if (Starts(item, "own=")) {
            others.own.push_back(item.substr(4));
        } else {
            action = item;
        }
    }
    for (auto& bind : binds) {
        others.bindings.emplace_back(bind.first, &bind.second);
    }
    for (auto& stick : sticks) {
        others.sticks.emplace_back(stick.first, &stick.second);
    }
    if (action == "settle") {
        for (const std::string& line : Settle(taps, others)) {
            std::printf("log: %s\n", line.c_str());
        }
    } else if (Starts(action, "give=")) {
        const std::string rest = action.substr(5);
        const std::size_t a = rest.find(','), b = rest.find(',', a + 1);
        Give(taps, std::atoi(rest.substr(0, a).c_str()), std::atoi(rest.substr(a + 1, b - a - 1).c_str()),
             rest.substr(b + 1), others);
    } else if (Starts(action, "release=")) {
        std::printf("had: %d\n", Release(taps, action.substr(8)) ? 1 : 0);
    } else if (Starts(action, "isown=")) {
        std::printf("own: %d\n", IsOwn(others, action.substr(6)) ? 1 : 0);
    }
    for (std::size_t i = 0; i < taps.size(); ++i) {
        std::printf("tap %d: %s\n", static_cast<int>(i + 1), Format(taps[i]).c_str());
    }
    for (const auto& bind : binds) {
        std::printf("bind %s: %s\n", bind.first.c_str(), bind.second.c_str());
    }
    for (const auto& stick : sticks) {
        std::printf("stick %s: %s\n", stick.first.c_str(), stick.second.c_str());
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("taps")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    # -Wall -Werror as the app is built (Azahar's CITRA_WARNINGS_AS_ERRORS)
    subprocess.run([CXX, "-std=c++17", "-Wall", "-Werror", "-I", APP, str(src), os.path.join(APP, "taps.cpp"),
                    "-o", str(exe)], check=True)
    return exe


def run(driver, *args):
    return subprocess.run([str(driver), *args], capture_output=True, text=True, check=True).stdout.splitlines()


def parse(driver, line):
    out = run(driver, "parse", line)[0]
    if out == "invalid":
        return None
    keys, x, y, name = out.split("\t")
    return keys, int(x), int(y), name


def scenario(driver, *items):
    return run(driver, "scenario", *items)


def test_a_tap_is_keys_a_point_and_a_name(driver):
    assert parse(driver, "Z @ 52 220") == ("Z", 52, 220, "")
    assert parse(driver, "F | Mouse Right @ 10 20 Mega potion") == ("F | Mouse Right", 10, 20, "Mega potion")
    # spaces don't count, and a comma can go between X and Y
    assert parse(driver, "  z|mouse right  @52,220  ") == ("z | mouse right", 52, 220, "")
    assert parse(driver, "Z @ 52 , 220 Map") == ("Z", 52, 220, "Map")


def test_the_point_stays_on_the_touch_screen(driver):
    assert parse(driver, "Z @ 320 240") == ("Z", 319, 239, "")
    assert parse(driver, "Z @ 99999999999999 0") == ("Z", 319, 0, "")


def test_a_tap_without_keys_keeps_its_point(driver):
    assert parse(driver, "@ 160 120") == ("", 160, 120, "")


def test_a_line_without_a_point_is_no_tap(driver):
    for line in ["Z", "Z @", "Z @ 52", "Z @ x 20", "Z @ 52 220px", "Z 52 220", "Z @ -5 20", ""]:
        assert parse(driver, line) is None, line


def test_an_at_sign_in_a_key_or_a_name(driver):
    # SDL names a keypad key "Keypad @": the point is after the first @ that has one
    assert parse(driver, "Keypad @ @ 1 2") == ("Keypad @", 1, 2, "")
    assert parse(driver, "Z @ 1 2 slot @ 3 4") == ("Z", 1, 2, "slot @ 3 4")


def test_format_writes_what_parse_reads(driver):
    assert run(driver, "format", "Z", "52", "220", "") == ["Z @ 52 220"]
    assert run(driver, "format", "", "160", "120", "") == ["@ 160 120"]
    assert run(driver, "format", "Z", "400", "-3", "  Map ") == ["Z @ 319 0 Map"]
    for line in ["Z @ 52 220", "F | Mouse Right @ 1 2 Mega potion", "@ 160 120", "Keypad @ @ 3 4 x"]:
        keys, x, y, name = parse(driver, line)
        assert run(driver, "format", keys, str(x), str(y), name) == [line]


def test_with_key_sets_a_column(driver):
    assert run(driver, "withkey", "F | Mouse Right", "0", "G") == ["G | Mouse Right"]
    assert run(driver, "withkey", "F | Mouse Right", "1", "") == ["F"]
    # taken out of the first column, the second key moves up
    assert run(driver, "withkey", "F | Mouse Right", "0", "") == ["Mouse Right"]
    assert run(driver, "withkey", "F", "1", "Z") == ["F | Z"]
    assert run(driver, "withkey", "", "1", "Z") == ["Z"]


def test_names_in_either_case_are_one_key(driver):
    assert run(driver, "same", "Left Shift", "left shift") == ["1"]
    assert run(driver, "same", "Z", "X") == ["0"]
    assert run(driver, "same", "", "") == ["0"]


def test_settle_the_bindings_give_up_the_taps_keys(driver):
    out = scenario(driver, "tap=f @ 1 2", "bind=button_a=F | Mouse Right", "bind=button_y=E", "settle")
    assert out == ["log: touch_tap_1 takes f from button_a", "tap 1: f @ 1 2", "bind button_a: Mouse Right",
                   "bind button_y: E"]


def test_settle_a_stick_direction_keeps_its_place(driver):
    out = scenario(driver, "tap=A @ 1 2", "tap=Left Shift @ 3 4", "stick=circle_pad=W,S,A,D,Left Shift",
                   "stick=c_stick=O,L,K,A", "settle")
    assert "stick circle_pad: W,S,,D" in out  # (no half-way key left: no fifth part)
    assert "stick c_stick: O,L,K," in out
    assert "log: touch_tap_2 takes Left Shift from circle_pad" in out


def test_settle_the_apps_own_keys_leave_the_taps(driver):
    out = scenario(driver, "tap=F11 | Z @ 1 2", "tap=escape @ 3 4", "tap=m @ 5 6", "own=Escape", "own=F11",
                   "own=F12 | M", "settle")
    assert [line for line in out if line.startswith("tap ")] == ["tap 1: Z @ 1 2", "tap 2: @ 3 4", "tap 3: @ 5 6"]
    assert "log: touch_tap_1: F11 is the app's own key, not a tap's" in out
    assert "log: touch_tap_3: m is the app's own key, not a tap's" in out


def test_settle_a_key_on_two_taps_stays_on_the_first(driver):
    out = scenario(driver, "tap=Z @ 1 2", "tap=X | z @ 3 4", "tap=Q | q @ 5 6", "settle")
    assert out == ["log: touch_tap_2: z is touch_tap_1's already", "tap 1: Z @ 1 2", "tap 2: X @ 3 4",
                   "tap 3: Q @ 5 6"]


def test_settle_with_nothing_to_change_says_nothing(driver):
    out = scenario(driver, "tap=Z @ 1 2 Potion", "bind=button_a=F", "stick=circle_pad=W,S,A,D", "own=F11",
                   "settle")
    assert out == ["tap 1: Z @ 1 2 Potion", "bind button_a: F", "stick circle_pad: W,S,A,D"]


def test_give_takes_the_key_from_every_other_input(driver):
    out = scenario(driver, "tap=Z @ 1 2", "tap=Q | E @ 3 4", "bind=button_zl=Q", "bind=item_prev=Wheel Up",
                   "stick=c_stick=O,L,K,Q", "give=0,1,q")
    assert out == ["tap 1: Z | q @ 1 2", "tap 2: E @ 3 4", "bind button_zl: ", "bind item_prev: Wheel Up",
                   "stick c_stick: O,L,K,"]


def test_give_puts_the_key_in_the_column_asked_for(driver):
    # Z was the first key: given to the second column, it takes X's place and is there once
    assert scenario(driver, "tap=Z | X @ 1 2", "give=0,1,Z") == ["tap 1: Z @ 1 2"]
    assert scenario(driver, "tap=@ 1 2", "give=0,1,Mouse 4") == ["tap 1: Mouse 4 @ 1 2"]


def test_give_to_a_tap_that_isnt_there_changes_nothing(driver):
    out = scenario(driver, "tap=Z @ 1 2", "bind=button_a=Z", "give=3,0,Z")
    assert out == ["tap 1: Z @ 1 2", "bind button_a: Z"]


def test_release_takes_a_key_off_the_taps(driver):
    out = scenario(driver, "tap=Z | X @ 1 2", "tap=x @ 3 4", "release=X")
    assert out == ["had: 1", "tap 1: Z @ 1 2", "tap 2: @ 3 4"]
    assert scenario(driver, "tap=Z @ 1 2", "release=Q") == ["had: 0", "tap 1: Z @ 1 2"]


def test_the_apps_own_keys_by_any_of_their_names(driver):
    assert scenario(driver, "own=F12 | M", "isown=m") == ["own: 1"]
    assert scenario(driver, "own=F12 | M", "own=", "isown=Z") == ["own: 0"]
