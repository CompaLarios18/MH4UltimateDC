"""port/src/map_view.cpp: why the quest's map view (quest_overlay) can't show, and what the app
says about it: the in-game menu's sentence for the map's settings and the target camera's, and
the line the touch-screen key shows when it has nothing to switch (issue #54: a panel layout
without the map panel left the corner on the whole touch screen, and nothing said why)."""
from __future__ import annotations

import os
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "port", "src")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdio>
#include <cstdlib>
#include <string>
#include "map_view.h"

using namespace Port::MapView;

// KEY=VALUE...: the state (layout touch quest hook switched seen in_quest map target, and
// target_overlay), then four lines: Blocked() as a number, MapNote, TargetNote, KeyNote
int main(int argc, char** argv) {
    State s;
    bool target_overlay = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string key = arg.substr(0, arg.find('='));
        const int value = std::atoi(arg.c_str() + arg.find('=') + 1);
        if (key == "layout") s.custom_layout = value != 0;
        else if (key == "touch") s.touch_overlay = value != 0;
        else if (key == "quest") s.quest_overlay = value != 0;
        else if (key == "hook") s.panels_hook = value != 0;
        else if (key == "switched") s.switched = value != 0;
        else if (key == "seen") s.panels.seen = value != 0;
        else if (key == "in_quest") s.panels.in_quest = value != 0;
        else if (key == "map") s.panels.map_slot = value;
        else if (key == "target") s.panels.target_slot = value;
        else if (key == "target_overlay") target_overlay = value != 0;
        else return 2;
    }
    std::printf("%d\n%s\n%s\n%s\n", static_cast<int>(Blocked(s)), MapNote(s).c_str(),
                TargetNote(s, target_overlay).c_str(), KeyNote(s).c_str());
    return 0;
}
"""

NONE, LAYOUT, TOUCH_OVERLAY, QUEST_OVERLAY, PANELS_HOOK, NO_MAP_PANEL, SWITCHED = range(7)

# the default panel layout (Type A) in a quest: the map in slots 0-1, the target camera in 2
IN_QUEST = {"seen": 1, "in_quest": 1, "map": 0, "target": 2}

# issue #54's panels: the target camera, Gestures, Combo List, the kick and item shortcuts
NO_MAP = {"seen": 1, "in_quest": 1, "map": -1, "target": 0}


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("map_view")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", SRC, str(src), os.path.join(SRC, "map_view.cpp"), "-o", str(exe)],
                   check=True)
    return exe


def view(driver, **state):
    out = subprocess.run([str(driver), *(f"{k}={int(v)}" for k, v in state.items())], capture_output=True,
                         text=True, check=True).stdout
    block, map_note, target_note, key_note = out.split("\n")[:4]
    return int(block), map_note, target_note, key_note


def test_shows_with_the_map_panel(driver):
    assert view(driver, **IN_QUEST) == (NONE, "", "", "")


def test_before_any_quest_nothing_is_known(driver):
    # the panels haven't run yet: no note, and the key switches as before
    assert view(driver) == (NONE, "", "", "")
    assert view(driver, map=-1, target=-1) == (NONE, "", "", "")


def test_panels_without_the_map(driver):
    block, map_note, target_note, key_note = view(driver, **NO_MAP)
    assert block == NO_MAP_PANEL
    assert map_note == ("Your touch screen's panels have no map now (Customize Panels in the quest menu): the map "
                        "view shows the map panel, which is two slots tall, so until it's back the corner shows the "
                        "whole touch screen and these settings change nothing.")
    # the target camera's view goes with the map's
    assert "beside the map's only" in target_note
    # the key says why it changes nothing, briefly
    assert key_note.startswith("No map to show: ") and "Customize Panels" in key_note
    assert len(key_note) < 100


def test_after_the_quest_the_last_one_is_meant(driver):
    _, map_note, _, key_note = view(driver, **{**NO_MAP, "in_quest": 0})
    assert "had no map in your last quest" in map_note
    assert "last quest" in key_note


def test_settings_that_keep_the_map_from_showing(driver):
    cases = {
        LAYOUT: ({"layout": 0}, "(Display)", "Esc > Display"),
        TOUCH_OVERLAY: ({"touch": 0}, "Azahar's own custom layout", "Esc > Game > Touch screen"),
        QUEST_OVERLAY: ({"quest": 0}, "In quests, only the map is off", "In quests, only the map is off"),
        PANELS_HOOK: ({"hook": 0}, "build it again", "build it again"),
    }
    for block, (state, in_map_note, in_key_note) in cases.items():
        got = view(driver, **IN_QUEST, **state)
        assert got[0] == block, state
        assert in_map_note in got[1], state
        assert got[2] == "The target camera's view shows beside the map's only, so it doesn't show either."
        assert got[3].startswith("No map to show: ") and in_key_note in got[3], state


def test_the_first_reason_is_the_one_told(driver):
    # the screens before the corner, the corner before the option, the option before the panels
    assert view(driver, **NO_MAP, layout=0, touch=0)[0] == LAYOUT
    assert view(driver, **NO_MAP, touch=0, quest=0)[0] == TOUCH_OVERLAY
    assert view(driver, **NO_MAP, quest=0, hook=0)[0] == QUEST_OVERLAY
    assert view(driver, **NO_MAP, hook=0)[0] == PANELS_HOOK


def test_switched_to_the_whole_touch_screen(driver):
    block, map_note, target_note, key_note = view(driver, **IN_QUEST, switched=1)
    assert block == SWITCHED
    assert "Press it again for the map." in map_note
    assert "beside the map's only" in target_note
    # the key switches back: nothing to say on the screen
    assert key_note == ""


def test_no_map_panel_comes_before_the_switch(driver):
    # switching back wouldn't show a map: the panels are what to change
    block, map_note, _, key_note = view(driver, **NO_MAP, switched=1)
    assert block == NO_MAP_PANEL
    assert "Customize Panels" in map_note and key_note.startswith("No map to show: ")


def test_target_camera_needs_its_panel(driver):
    _, map_note, target_note, key_note = view(driver, **{**IN_QUEST, "target": -1})
    assert (map_note, key_note) == ("", "")
    assert target_note.startswith("Your touch screen's panels have no target camera now")
    assert "had no target camera in your last quest" in view(driver, **{**IN_QUEST, "target": -1,
                                                                        "in_quest": 0})[2]
    # turned off: nothing to explain
    assert view(driver, **{**IN_QUEST, "target": -1}, target_overlay=0)[2] == ""
    assert view(driver, **NO_MAP, target_overlay=0)[2] == ""
