"""The in-game menu (port/app/ui/menu*.cpp) still writes every setting it wrote before its tabs were
reorganized, the same way: the mh4u.ini keys it sets (Menu::SetGame), the ones the hooks take while
the game runs (GAME_LIVE), and every app setting it changes saved when it closes (Menu::Closed)."""
from __future__ import annotations

import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "port", "app", "ui")

# mh4u.ini's keys the menu writes, as before the reorganisation
GAME_KEYS = {"fps", "refresh_rate", "widescreen", "wide_render", "hud_edges", "touch_overlay", "quest_overlay",
             "target_overlay", "kick", "target", "skip", "skip_end", "skip_end_hold", "skip_hint", "dither",
             "stereo_3d", "antialiasing", "bloom", "depth_of_field", "motion_blur", "radial_blur", "haze",
             "net_sync"}
# the ones that apply while the game runs (Port::Hooks::SetOption): all but widescreen,
# wide_render and net_sync
GAME_LIVE = GAME_KEYS - {"widescreen", "wide_render", "net_sync"}


def sources() -> str:
    text = ""
    for path in sorted(glob.glob(os.path.join(UI, "menu*.cpp"))):
        with open(path, encoding="utf-8") as f:
            text += f.read()
    return text


def header() -> str:
    with open(os.path.join(UI, "menu.h"), encoding="utf-8") as f:
        return f.read()


def test_set_game_keys():
    text = sources()
    literal = set(re.findall(r'SetGame\("(\w+)"', text))
    assert literal <= GAME_KEYS, literal - GAME_KEYS
    # keys passed on to SetGame(key, ...): a list of 3DS buttons, a quest view's rows, the effects'
    # loop, the editor's two views
    indirect = set(re.findall(r'DrawButtonList\(R::\w+, "(\w+)"\)', text))
    indirect |= set(re.findall(r'DrawPanelViewRows\("(\w+)"', text))
    indirect |= set(re.findall(r'std::pair\{R::Graphics\w+, "(\w+)"\}', text))
    indirect |= set(re.findall(r'PANEL_VIEW_KEYS\[\] = \{"(\w+)", "(\w+)"\}', text)[0])
    assert indirect <= GAME_KEYS, indirect - GAME_KEYS
    assert literal | indirect == GAME_KEYS, GAME_KEYS - (literal | indirect)


def test_game_live_unchanged():
    text = sources()
    block = re.search(r"GAME_LIVE = \{(.*?)\};", text, re.S).group(1)
    assert set(re.findall(r'"(\w+)"', block)) == GAME_LIVE


def test_every_app_setting_changed_and_saved():
    body = re.search(r"enum class AppSetting \{(.*?)\};", header(), re.S).group(1)
    settings = re.findall(r"\b([A-Z]\w*)\b", body)
    assert {"CustomTextures", "Launcher"} <= set(settings)
    text = sources()
    closed = re.search(r"void Menu::Closed\(\) \{(.*?)\n\}", text, re.S).group(1)
    for setting in settings:
        assert f"Changed(AppSetting::{setting})" in text, setting
        assert f"case AppSetting::{setting}:" in closed, setting


def test_closed_copies_the_fields():
    """What each AppSetting saves: today's fields, the two new ones and the touch taps."""
    text = sources()
    closed = re.search(r"void Menu::Closed\(\) \{(.*?)\n\}", text, re.S).group(1)
    closed = re.sub(r"//[^\n]*", "", closed)  # (a line in a comment isn't saved)
    copied = set(re.findall(r"file\.(\w+) = now\.(\w+);", closed))
    assert all(a == b for a, b in copied)
    fields = {a for a, _ in copied}
    assert fields == {
        "fullscreen", "screens", "resolution", "linear_filter", "vsync", "cpu_clock", "fast_forward_speed",
        "fast_forward_hold", "renderer", "volume", "audio_output", "fps_counter", "custom_textures", "launcher",
        "keys", "stick_keys", "controller_auto", "button_params", "stick_params", "input_mode",
        "mouse_sensitivity", "mouse_vertical", "mouse_invert_y", "mouse_invert_x", "mouse_deadzone", "item_prev",
        "item_next", "stylus_key", "touch_screen_key", "key_guides", "key_guides_key", "pause_key",
        "fast_forward_key", "touch_taps"}


def test_texts_ascii():
    """The menu's font has ASCII only (ui.cpp, AddFontDefaultVector): no curly quotes or arrows. With
    the touch taps' section and the map view's sentences (port/src/map_view.cpp), which it shows."""
    for path in sorted(glob.glob(os.path.join(UI, "menu*.cpp")) + [os.path.join(UI, "settings_index.cpp"),
                                                                    os.path.join(UI, "touch_taps.cpp"),
                                                                    os.path.join(ROOT, "port", "src",
                                                                                 "map_view.cpp")]):
        with open(path, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
                    assert literal.isascii(), f"{os.path.basename(path)}:{number}: {literal}"
