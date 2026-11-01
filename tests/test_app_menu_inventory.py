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


def test_layout_editor_key_lines():
    """Edit HUD layout's keys, the controller's and the keyboard's: as many lines (the toolbar's buttons
    don't move when the other is used), each 41 characters at most, which fit its 26 em at every font
    size (a longer one broke a key from what it does)."""
    with open(os.path.join(UI, "menu_editor.cpp"), encoding="utf-8") as f:
        text = f.read()
    literals = r'((?:"(?:[^"\\]|\\.)*"\s*)+)'
    pad, keyboard = re.search(r"const char\* keys = layer\.PadLast\(\)\s*\?\s*" + literals + r":\s*" + literals + ";",
                              text).groups()
    lines = [("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', group))).split("\\n") for group in (pad, keyboard)]
    assert len(lines[0]) == len(lines[1])
    for line in lines[0] + lines[1]:
        assert 0 < len(line) <= 41, line


def test_sliders_take_no_typed_value_from_a_controller():
    """Every slider of the app (the menu's tabs, the touch taps, Online, the launcher's screens) takes
    no value typed in while a controller is in use: a controller's A held 0.6 s would ask for one,
    with no keyboard at hand. The one being typed already keeps its field: Menu::SliderFlags(id), or
    the slider's own id checked with Nav::TypingValue or ImGui::TempInputIsActive just before it. The
    launcher's screens go through Launcher::Ui::SliderInt, with the controller's state (s.pad)."""
    for path in sorted(glob.glob(os.path.join(ROOT, "port", "app", "launcher", "*.cpp"))):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for call in re.findall(r"\bUi::SliderInt\(([^;]*)\);", text):
            assert call.rsplit(",", 1)[1].strip() == "s.pad", f"{os.path.basename(path)}: {call}"
    found = 0
    for path in sorted(glob.glob(os.path.join(UI, "*.cpp")) +
                       glob.glob(os.path.join(ROOT, "port", "app", "launcher", "*.cpp"))):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        last_end = 0
        for match in re.finditer(r"ImGui::Slider\w*\(", text):
            depth, end = 1, match.end()
            while depth:
                depth += {"(": 1, ")": -1}.get(text[end], 0)
                end += 1
            call = text[match.start():end]
            slider_id = call[len(match.group()):].split(",")[0].strip()
            # what comes before it, back to the slider before (whose own check doesn't count)
            before = text[max(last_end, match.start() - 600):match.start()]
            own = (f"Nav::TypingValue({slider_id})" in before or
                   f"ImGui::TempInputIsActive(ImGui::GetID({slider_id}))" in before)
            where = f"{os.path.basename(path)}:{text.count(chr(10), 0, match.start()) + 1}"
            assert f"SliderFlags({slider_id})" in call or (own and "NoInput" in before + call), where
            last_end = end
            found += 1
    # the menu's two helpers, the quest views' size, the taps' point, Online's players, the launcher's helper
    assert found >= 6


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
