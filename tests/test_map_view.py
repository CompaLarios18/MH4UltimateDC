"""port/src/map_view.cpp: why the quest's map view (quest_overlay) can't show, and what to say
about it: the in-game menu's sentence for the map's settings and the target camera's, and what
the touch-screen key does, with the line it shows when it has nothing to switch (issue #54: a
panel layout without the map panel left the corner on the whole touch screen, and nothing said
why)."""
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

// KEY=VALUE...: the state (layout swapped upright touch quest hook switched seen in_quest map
// target, and target_overlay), then five lines: Blocked() as a number, MapNote, TargetNote, and
// the key pressed: its line, and the switch after it (0 or 1)
int main(int argc, char** argv) {
    State s;
    bool target_overlay = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string key = arg.substr(0, arg.find('='));
        const int value = std::atoi(arg.c_str() + arg.find('=') + 1);
        if (key == "layout") s.custom_layout = value != 0;
        else if (key == "swapped") s.swapped = value != 0;
        else if (key == "upright") s.upright = value != 0;
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
    const KeyPress press = PressKey(s);
    std::printf("%d\n%s\n%s\n%s\n%d\n", static_cast<int>(Blocked(s)), MapNote(s).c_str(),
                TargetNote(s, target_overlay).c_str(), press.note.c_str(), press.switched ? 1 : 0);
    return 0;
}
"""

NONE, LAYOUT, TOUCH_OVERLAY, QUEST_OVERLAY, PANELS_HOOK, NO_MAP_PANEL, SWITCHED = range(7)

# the default panel layout (Type A) in a quest: the map in slots 0-1, the target camera in 2
IN_QUEST = {"seen": 1, "in_quest": 1, "map": 0, "target": 2}

# issue #54's panels: the target camera, Gestures, Combo List, the kick and item shortcuts
NO_MAP = {"seen": 1, "in_quest": 1, "map": -1, "target": 0}

# what each setting that keeps the map from showing is, and what the menu's sentence and the
# key's line say of it
SETTINGS = {
    LAYOUT: ({"layout": 0}, "(Touch screen > Screens)", "Esc > Touch screen"),
    TOUCH_OVERLAY: ({"touch": 0}, "Azahar's own custom layout", "Esc > Touch screen"),
    QUEST_OVERLAY: ({"quest": 0}, '"Only the map in quests" is off', '"Only the map in quests" is off'),
    PANELS_HOOK: ({"hook": 0}, "build it again", "build it again"),
}


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("map_view")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", SRC, str(src), os.path.join(SRC, "map_view.cpp"), "-o", str(exe)],
                   check=True)
    return exe


def run(driver, **state):
    out = subprocess.run([str(driver), *(f"{k}={int(v)}" for k, v in state.items())], capture_output=True,
                         text=True, check=True).stdout
    return out.split("\n")[:5]


def view(driver, **state):
    """Blocked(), the menu's sentence for the map, the target camera's, and the key's line."""
    block, map_note, target_note, key_note, _ = run(driver, **state)
    return int(block), map_note, target_note, key_note


def switched_after_press(driver, **state):
    """Whether quests show the whole touch screen after a press of the key."""
    return run(driver, **state)[4] == "1"


def test_shows_with_the_map_panel(driver):
    assert view(driver, **IN_QUEST) == (NONE, "", "", "")


def test_before_any_quest_nothing_is_known(driver):
    # the panels haven't run yet: no note, and the key switches as before
    assert view(driver) == (NONE, "", "", "")
    assert view(driver, map=-1, target=-1) == (NONE, "", "", "")
    assert switched_after_press(driver)


def test_panels_without_the_map(driver):
    block, map_note, target_note, key_note = view(driver, **NO_MAP)
    assert block == NO_MAP_PANEL
    assert map_note == ("Your touch screen's panels have no map now (Customize Panels in the quest menu): the map "
                        "view shows the map panel, which is two slots tall, so until it's back the corner shows the "
                        "whole touch screen and these settings change nothing.")
    # the target camera's view goes with the map's
    assert "beside the map's only" in target_note
    # the key says why it changes nothing, briefly
    assert key_note == "No map to switch to: your touch screen's panels have none (Customize Panels in the quest menu)"
    assert len(key_note) < 100


def test_after_the_quest_the_last_one_is_meant(driver):
    _, map_note, _, key_note = view(driver, **{**NO_MAP, "in_quest": 0})
    assert "had no map in your last quest" in map_note
    assert "last quest" in key_note


def test_settings_that_keep_the_map_from_showing(driver):
    for block, (state, in_map_note, in_key_note) in SETTINGS.items():
        got = view(driver, **IN_QUEST, **state)
        assert got[0] == block, state
        assert in_map_note in got[1], state
        assert got[2] == "The target camera's view shows beside the map's only, so it doesn't show either."
        # what the key can't do is switch to the map alone: the whole touch screen may well be there
        assert got[3].startswith("No map to switch to: ") and in_key_note in got[3], state


def test_swapped_or_upright_screens_show_the_touch_screen_whole(driver):
    # the custom layout crops the touch screen only in the corner, in landscape
    for state in ({"swapped": 1}, {"upright": 1}):
        block, map_note, _, key_note = view(driver, **IN_QUEST, **state)
        assert block == LAYOUT, state
        assert "(Touch screen > Screens)" in map_note and "Esc > Touch screen" in key_note, state


def test_the_menu_places_named_are_the_menus():
    # the sentences name the in-game menu's tab, its settings and the Keys row as the menu draws them
    with open(os.path.join(ROOT, "port", "app", "ui", "settings_index.cpp"), encoding="utf-8") as f:
        index = f.read()
    for name in ("Touch screen", "Screens", "Touch screen over the game", "Only the map in quests",
                 "Map / whole touch screen"):
        assert f'"{name}"' in index, name


def test_the_first_reason_is_the_one_told(driver):
    # the screens before the corner, the corner before the option, the option before the panels
    assert view(driver, **NO_MAP, layout=0, touch=0)[0] == LAYOUT
    assert view(driver, **NO_MAP, touch=0, quest=0)[0] == TOUCH_OVERLAY
    assert view(driver, **NO_MAP, quest=0, hook=0)[0] == QUEST_OVERLAY
    assert view(driver, **NO_MAP, hook=0)[0] == PANELS_HOOK


def test_switched_to_the_whole_touch_screen(driver):
    block, map_note, target_note, key_note = view(driver, **IN_QUEST, switched=1)
    assert block == SWITCHED
    assert "the Map / whole touch screen key" in map_note and "Press it again for the map." in map_note
    assert "beside the map's only" in target_note
    # the key switches back: nothing to say on the screen
    assert key_note == ""


def test_the_key_switches_when_it_can(driver):
    assert switched_after_press(driver, **IN_QUEST)
    assert not switched_after_press(driver, **IN_QUEST, switched=1)


def test_no_map_panel_comes_before_the_switch(driver):
    # switching back wouldn't show a map: the panels are what to change
    block, map_note, _, key_note = view(driver, **NO_MAP, switched=1)
    assert block == NO_MAP_PANEL
    assert "Customize Panels" in map_note and key_note.startswith("No map to switch to: ")


def test_with_nothing_to_switch_the_key_asks_for_the_map(driver):
    # a switch flipped while nothing showed it kept quests on the whole touch screen once the map
    # could show again (the map panel put back, say): the press sets it to the map instead
    for switched in (0, 1):
        assert not switched_after_press(driver, **NO_MAP, switched=switched)
        assert not switched_after_press(driver, **{**NO_MAP, "in_quest": 0}, switched=switched)
        for state, _, _ in SETTINGS.values():
            assert not switched_after_press(driver, **IN_QUEST, **state, switched=switched), state


def test_target_camera_needs_its_panel(driver):
    _, map_note, target_note, key_note = view(driver, **{**IN_QUEST, "target": -1})
    assert (map_note, key_note) == ("", "")
    assert target_note.startswith("Your touch screen's panels have no target camera now")
    assert "had no target camera in your last quest" in view(driver, **{**IN_QUEST, "target": -1,
                                                                        "in_quest": 0})[2]
    # turned off: nothing to explain
    assert view(driver, **{**IN_QUEST, "target": -1}, target_overlay=0)[2] == ""
    assert view(driver, **NO_MAP, target_overlay=0)[2] == ""
