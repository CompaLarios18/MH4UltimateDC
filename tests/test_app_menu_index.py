"""port/app/ui/settings_index.cpp and menu_rules.cpp: the in-game menu's index (every tab, section and
row, the words its search matches, each row's help) and its rules (which rows are hidden or shown
disabled, and the notes that say why), compiled on their own with a small driver."""
from __future__ import annotations

import os
import re
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")
UI = os.path.join(APP, "ui")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "ui/menu_rules.h"
#include "ui/settings_index.h"

using namespace App::Ui;

static const char* KindName(Index::Kind kind) {
    switch (kind) {
    case Index::Kind::Setting: return "Setting";
    case Index::Kind::Key: return "Key";
    case Index::Kind::Action: return "Action";
    case Index::Kind::Block: return "Block";
    case Index::Kind::Link: return "Link";
    default: return "Fixed";
    }
}

static const char* Id(Index::RowId row) {
    return row == Index::RowId::Count ? "-" : Index::Get(row).id;
}

static bool On(const std::string& v) {
    return v == "1" || v == "on" || v == "true";
}

// rows | sections | normalize TEXT | search QUERY [ROW_ID=EXTRA...] | rules KEY=VALUE...
int main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "rows") {
        for (int i = 0; i < Index::ROW_COUNT; ++i) {
            const Index::Row& r = Index::ROWS[i];
            const auto row = static_cast<Index::RowId>(i);
            std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\t%s\n", r.id, r.section,
                        Index::TabName(Index::TabOf(row)), KindName(r.kind), r.label,
                        r.search_label ? r.search_label : "", r.help, r.keywords, r.parent, r.shown_when,
                        r.next_start ? 1 : 0, r.contextual ? 1 : 0, r.writes, Index::ListedLabel(row).c_str());
        }
        return 0;
    }
    if (mode == "sections") {
        for (int i = 0; i < Index::SECTION_COUNT; ++i) {
            const Index::Section& s = Index::SECTIONS[i];
            std::printf("%s\t%s\t%s\n", s.id, Index::TabName(s.tab), s.title);
        }
        return 0;
    }
    if (mode == "normalize") {
        std::printf("%s\n", Index::Normalize(argv[2]).c_str());
        return 0;
    }
    if (mode == "search") {
        std::vector<std::string> extra(Index::ROW_COUNT);
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto row = Index::Find(arg.substr(0, arg.find('=')));
            extra[static_cast<int>(row)] = arg.substr(arg.find('=') + 1);
        }
        std::printf("%s\n", Index::Searchable(argv[2]) ? "searchable" : "not searchable");
        for (const Index::Result& result : Index::Search(argv[2], extra)) {
            std::printf("%d\t%s\n", result.tier, Index::ROWS[result.row].id);
        }
        return 0;
    }
    Rules::Facts f;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string key = arg.substr(0, arg.find('=')), v = arg.substr(arg.find('=') + 1);
        if (key == "screens") f.screens = v;
        else if (key == "renderer") f.renderer = v;
        else if (key == "overlay_on") f.overlay_on = On(v);
        else if (key == "map_on") f.map_on = On(v);
        else if (key == "map_placed") f.map_placed = On(v);
        else if (key == "target_on") f.target_on = On(v);
        else if (key == "target_placed") f.target_placed = On(v);
        else if (key == "widescreen") f.widescreen = v;
        else if (key == "wide_render") f.wide_render = On(v);
        else if (key == "dither") f.dither = On(v);
        else if (key == "resolution") f.resolution = std::atoi(v.c_str());
        else if (key == "fps") f.fps = std::atoi(v.c_str());
        else if (key == "refresh_rate") f.refresh_rate = std::strtof(v.c_str(), nullptr);
        else if (key == "controller_mode") f.controller_mode = On(v);
        else if (key == "controller_auto") f.controller_auto = On(v);
        else if (key == "software_now") f.software_now = On(v);
        else if (key == "windows_now") f.windows_now = On(v);
        else if (key == "custom_now") f.custom_now = On(v);
        else if (key == "in_room") f.in_room = On(v);
        else if (key == "renderer_now") f.renderer_now = v;
        else if (key == "window_aspect") f.window_aspect = std::strtof(v.c_str(), nullptr);
        else if (key == "map_view_note") f.map_view_note = v;
        else if (key == "target_view_note") f.target_view_note = v;
        else if (key.rfind("buttons.", 0) == 0) f.buttons[std::atoi(key.c_str() + 8)] = v;
        else if (key.rfind("unbound.", 0) == 0) f.unbound[std::atoi(key.c_str() + 8)] = v;
        else { std::fprintf(stderr, "unknown fact %s\n", key.c_str()); return 2; }
    }
    const Rules::Verdict v = Rules::Evaluate(f);
    for (int i = 0; i < Index::ROW_COUNT; ++i) {
        if (v.hidden[i]) std::printf("hidden\t%s\n", Index::ROWS[i].id);
        if (v.disabled[i]) std::printf("disabled\t%s\n", Index::ROWS[i].id);
    }
    for (int i = 0; i < Index::SECTION_COUNT; ++i) {
        const Rules::Note& note = v.sections[i];
        if (!note.Empty()) std::printf("section\t%s\t%s\t%s\n", Index::SECTIONS[i].id, note.text.c_str(), Id(note.link));
    }
    for (const auto& [row, note] : v.rows) {
        std::printf("row\t%s\t%s\t%s\t%s\n", Id(row), note.text.c_str(), Id(note.link), note.amber ? "amber" : "dim");
    }
    if (!v.keys_top.Empty()) std::printf("keys_top\t%s\t%s\n", v.keys_top.text.c_str(), Id(v.keys_top.link));
    if (v.windows_entry_disabled) std::printf("windows_entry_disabled\n");
    if (v.hud_editor_off) std::printf("hud_editor_off\n");
    return 0;
}
"""

FIELDS = ("id", "section", "tab", "kind", "label", "search_label", "help", "keywords", "parent", "shown_when",
          "next_start", "contextual", "writes", "listed")


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("menu_index")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(UI, "settings_index.cpp"),
                    os.path.join(UI, "menu_rules.cpp"), "-o", str(exe)], check=True)
    return exe


def run(driver, *args: str) -> list[str]:
    out = subprocess.run([str(driver), *args], capture_output=True, text=True, check=True).stdout
    return out.splitlines()


@pytest.fixture(scope="module")
def rows(driver) -> list[dict[str, str]]:
    return [dict(zip(FIELDS, line.split("\t"))) for line in run(driver, "rows")]


@pytest.fixture(scope="module")
def sections(driver) -> list[tuple[str, str, str]]:
    return [tuple(line.split("\t")) for line in run(driver, "sections")]


def camel(dotted: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in re.split(r"[._]", dotted))


def enum_names(name: str) -> list[str]:
    with open(os.path.join(UI, "settings_index.h"), encoding="utf-8") as f:
        text = f.read()
    body = re.search(r"enum class %s : int \{(.*?)\};" % name, text, re.S).group(1)
    return [n for n in re.findall(r"\b([A-Z]\w*)\b", body) if n != "Count"]


# ── The index ──

def test_ids_unique_and_linked(rows, sections):
    ids = [r["id"] for r in rows]
    assert len(ids) == len(set(ids))
    section_ids = [s[0] for s in sections]
    assert len(section_ids) == len(set(section_ids))
    for r in rows:
        assert r["section"] in section_ids, r["id"]
        assert r["parent"] == "" or r["parent"] in ids, r["id"]
        # a row that can hide says when it shows
        assert (r["parent"] == "") == (r["shown_when"] == ""), r["id"]


def test_enums_follow_the_tables(rows, sections):
    assert enum_names("RowId") == [camel(r["id"]) for r in rows]
    assert enum_names("SectionId") == [camel(s[0]) for s in sections]


def test_rows_in_tab_order(rows):
    """The rows come tab by tab in the tab bar's order, each section's rows together."""
    tabs = ["Display", "Graphics", "Touch screen", "Controls", "Keys", "Gameplay", "Audio", "Online",
            "Top of the menu"]
    order = [tabs.index(r["tab"]) for r in rows]
    assert order == sorted(order)
    seen: list[str] = []
    for r in rows:
        if not seen or seen[-1] != r["section"]:
            assert r["section"] not in seen, f"{r['section']} comes back at {r['id']}"
            seen.append(r["section"])


def test_texts_ascii_and_short(rows, sections):
    for r in rows:
        for field in ("label", "search_label", "help", "keywords", "shown_when"):
            assert r[field].isascii(), (r["id"], field)
        help_text = re.sub(r"\{key:\w+\}", "X" * 10, r["help"])
        assert 0 < len(help_text) <= 140, (r["id"], len(help_text))
        assert len(r["label"]) <= 30, (r["id"], r["label"])
        assert "{" not in help_text and "}" not in help_text, r["id"]
    for _, _, title in sections:
        assert title.isascii()


# mh4u.ini's keys the menu writes (Menu::SetGame), as before the reorganisation
GAME_KEYS = {"fps", "refresh_rate", "widescreen", "wide_render", "hud_edges", "touch_overlay", "quest_overlay",
             "target_overlay", "kick", "target", "skip", "skip_end", "skip_end_hold", "skip_hint", "dither",
             "stereo_3d", "antialiasing", "bloom", "depth_of_field", "motion_blur", "radial_blur", "haze",
             "net_sync"}
# mh4u-app.ini's settings the menu changes: those of before, custom_textures and launcher, and the
# touch taps ([Controls] touch_tap_1 ... touch_tap_8)
APP_SETTINGS = {
    "[Display] fullscreen", "[Display] screens", "[Display] resolution", "[Display] linear_filter",
    "[Display] vsync", "[Display] renderer", "[Display] fps_counter", "[Display] custom_textures",
    "[Game] cpu_clock", "[Game] fast_forward_speed", "[Game] launcher", "[Audio] volume", "[Audio] output",
    "[Controls] input_mode", "[Controls] key_guides", "[Controls] mouse_sensitivity", "[Controls] mouse_vertical",
    "[Controls] mouse_invert_y", "[Controls] mouse_invert_x", "[Controls] mouse_deadzone", "[Controls] controller",
    "[Controls] fast_forward_hold", "[Controls] item_prev", "[Controls] item_next", "[Controls] stylus_key",
    "[Controls] touch_screen_key", "[Controls] key_guides_key", "[Controls] pause_key", "[Controls] fast_forward_key",
    "[Controls] circle_pad", "[Controls] c_stick", "[Controls] touch_tap_N",
    *(f"[Controls] button_{b}" for b in ("a", "b", "x", "y", "l", "r", "zl", "zr", "start", "select", "up", "down",
                                         "left", "right")),
}


def test_nothing_lost(rows):
    game, app = set(), set()
    for r in rows:
        writes = r["writes"]
        if writes.startswith("mh4u.ini "):
            game.add(writes.split(" ", 1)[1])
        elif writes.startswith("mh4u-app.ini "):
            app.add(writes.split(" ", 1)[1])
        else:
            assert writes == "", r["id"]
            assert r["kind"] in ("Action", "Link", "Fixed", "Block"), r["id"]
    assert game == GAME_KEYS
    assert app == APP_SETTINGS


def test_contextual_rows_are_second_places(rows):
    keys = {r["writes"] for r in rows if r["tab"] == "Keys" and r["kind"] == "Key"}
    contextual = [r for r in rows if r["contextual"] == "1"]
    assert {r["id"] for r in contextual} == {"touch.map_key", "gameplay.ff_key", "gameplay.pause_key"}
    for r in contextual:
        assert r["kind"] == "Key" and r["tab"] != "Keys"
        assert r["writes"] in keys, r["id"]
    # every other setting has one home: a field is written by one row, but the touch screen's corner
    # (its parts) and the quest views (theirs)
    homes: dict[str, list[str]] = {}
    for r in rows:
        if r["writes"] and r["contextual"] == "0" and r["tab"] != "Keys":
            homes.setdefault(r["writes"], []).append(r["id"])
    shared = {w: ids for w, ids in homes.items() if len(ids) > 1}
    assert set(shared) == {"mh4u.ini touch_overlay", "mh4u.ini quest_overlay", "mh4u.ini target_overlay"}


def test_next_start_rows(rows):
    assert {r["id"] for r in rows if r["next_start"] == "1"} == {
        "display.renderer", "display.launcher", "display.aspect", "display.wide_render", "graphics.custom_textures",
        "audio.output", "online.net_sync"}


# ── The search ──

IN_QUESTS = {"touch.map_only", "touch.map_key", "touch.hud_editor", "touch.map_size", "touch.map_where",
             "touch.map_x", "touch.map_y", "touch.target_view", "touch.target_size", "touch.target_where",
             "touch.target_x", "touch.target_y"}
FROM_PICTURE = {"graphics.dither", "graphics.stereo_3d", "graphics.antialiasing", "graphics.bloom",
                "graphics.depth_of_field", "graphics.motion_blur", "graphics.radial_blur", "graphics.haze"}
ONLINE_LINKS = {"online.join", "online.host", "online.lan", "online.lobby", "online.invite", "online.name",
                "online.leave"}


def search(driver, query: str, *extra: str) -> tuple[bool, list[tuple[int, str]]]:
    lines = run(driver, "search", query, *extra)
    found = [(int(tier), row) for tier, row in (line.split("\t") for line in lines[1:])]
    return lines[0] == "searchable", found


@pytest.mark.parametrize("query, expected", [
    ("fps", {"display.fps", "display.fps_counter", "keys.fixed_fps_counter"}),
    # the quest's map views, the map key; and Map a controller automatically, by its name
    ("map", IN_QUESTS | {"keys.touch_screen", "controls.controller_auto"}),
    ("the map", IN_QUESTS | {"keys.touch_screen", "controls.controller_auto"}),
    ("kick", {"gameplay.kick", "keys.zr"}),
    ("bloom", {"graphics.bloom"}),
    ("60", {"display.fps", "display.refresh_rate"}),
    ("opacity", {"touch.opacity"}),
    ("carving", {"gameplay.skip_end"}),
    ("textures", {"graphics.custom_textures"}),
    # what came from Game > Picture, and Draw the wider picture by its name
    ("picture", FROM_PICTURE | {"display.wide_render"}),
    ("multiplayer", ONLINE_LINKS | {"online.net_sync"}),
    ("16:9", {"display.aspect"}),
    ("vsync", {"display.vsync"}),
    ("quit", {"header.quit", "online.leave"}),
])
def test_search_tier_one(driver, query, expected):
    searchable, found = search(driver, query)
    assert searchable
    assert {row for _, row in found} == expected
    assert all(tier == 1 for tier, _ in found)


def test_search_in_the_menus_order(driver):
    _, found = search(driver, "fps")
    assert [row for _, row in found] == ["display.fps", "display.fps_counter", "keys.fixed_fps_counter"]


def test_search_the_keys_bound(driver):
    _, found = search(driver, "space", "keys.b=Space")
    assert found == [(1, "keys.b")]
    _, found = search(driver, "shift", "keys.r=Left Shift Mouse 4")
    assert found == [(1, "keys.r")]
    # a key bound now, not the one it had: without extra, no row is named by Space (only a help
    # text mentions spaces)
    assert all(tier == 2 for tier, _ in search(driver, "space")[1])


def test_search_help_only_is_tier_two(driver):
    _, found = search(driver, "misbehave")
    assert found == [(2, "display.cpu_clock")]


def test_search_nothing(driver):
    assert search(driver, "xyz") == (True, [])
    # results need a word of two letters or more (the menu doesn't search before)
    assert search(driver, "x")[0] is False
    assert search(driver, "the") == (False, [])
    assert search(driver, "a")[0] is False
    # the search field and Back aren't results
    assert "header.back" not in {row for _, row in search(driver, "back")[1]}


def test_search_old_names(driver):
    for old, row in (("glow over bright parts", "graphics.bloom"), ("focus blur", "graphics.depth_of_field"),
                     ("zoom blur", "graphics.radial_blur"), ("heat shimmer", "graphics.haze"),
                     ("shape of the top screen", "display.aspect"), ("up and down", "controls.mouse_vertical"),
                     ("map a controller on its own", "controls.controller_auto")):
        assert row in {r for _, r in search(driver, old)[1]}, old


def test_normalize(driver):
    assert run(driver, "normalize", "16:9")[0] == "16 9"
    assert run(driver, "normalize", "The C-stick, in quests")[0] == "c stick quests"


# ── The rules ──

def rules(driver, **facts) -> dict[str, object]:
    args = [f"{k.replace('__', '.')}={v}" for k, v in facts.items()]
    result: dict[str, object] = {"hidden": set(), "disabled": set(), "section": {}, "row": {}, "flags": set()}
    for line in run(driver, "rules", *args):
        kind, *rest = line.split("\t")
        if kind in ("hidden", "disabled"):
            result[kind].add(rest[0])
        elif kind == "section":
            result["section"][rest[0]] = (rest[1], rest[2])
        elif kind == "row":
            result["row"][rest[0]] = (rest[1], rest[2], rest[3])
        elif kind == "keys_top":
            result["keys_top"] = (rest[0], rest[1])
        else:
            result["flags"].add(kind)
    return result


# the menu as it comes: the overlay chosen and on, 16:9, the template's buttons all bound
BASE = dict(screens="overlay", overlay_on=1, map_on=1, target_on=1, widescreen="16:9", wide_render=1,
            fps=60, renderer="vulkan", renderer_now="Vulkan", buttons__0="ZR", buttons__1="ZL",
            buttons__2="ZR", buttons__3="ZL+ZR")


def test_rules_as_it_comes(driver):
    r = rules(driver, **BASE)
    assert r["disabled"] == set()
    assert r["section"] == {}
    assert r["row"] == {"display.renderer": ("In use: Vulkan.", "-", "dim")}
    assert r["hidden"] == {"touch.map_x", "touch.map_y", "touch.target_x", "touch.target_y"}
    assert r["flags"] == set()


def test_r1_screens_not_overlay(driver):
    r = rules(driver, **{**BASE, "screens": "side"})
    note = ("Only with Screens set to The game, the touch screen in a corner.", "touch.screens")
    assert r["section"]["touch.corner"] == note and r["section"]["touch.quests"] == note
    assert {"touch.overlay", "touch.corner", "touch.map_only", "touch.hud_editor"} <= r["disabled"]
    assert "hud_editor_off" in r["flags"]


def test_r2_overlay_off(driver):
    r = rules(driver, **{**BASE, "overlay_on": 0})
    assert r["section"] == {"touch.quests": ("Only with Touch screen over the game on.", "touch.overlay")}
    assert "touch.map_only" in r["disabled"] and "touch.overlay" not in r["disabled"]
    assert {"touch.corner", "touch.size", "touch.opacity"} <= r["hidden"]  # H1


def test_r3_windows_running(driver):
    r = rules(driver, **{**BASE, "windows_now": 1})
    note = ("From the next start: until then the touch screen has a window of its own.", "-")
    assert r["section"] == {"touch.corner": note, "touch.quests": note}
    assert r["disabled"] == {"touch.hud_editor"}


def test_r4_r5_the_hooks_sentences(driver):
    r = rules(driver, **BASE, map_view_note="No map panel.", target_view_note="No target panel.")
    assert r["section"] == {"touch.quests": ("No map panel.", "-")}
    assert "touch.target_view" not in r["row"]  # R4's note comes first
    assert r["disabled"] == set()  # the rows apply once the view can show
    r = rules(driver, **BASE, target_view_note="No target panel.")
    assert r["row"]["touch.target_view"] == ("No target panel.", "-", "amber")
    # the menu's own reasons come before the hooks' (the running layout may differ from the choice)
    r = rules(driver, **{**BASE, "screens": "stacked"}, map_view_note="No map panel.")
    assert r["section"]["touch.quests"][1] == "touch.screens"


def test_priorities(driver):
    # R2 over R3 over R4
    r = rules(driver, **{**BASE, "overlay_on": 0, "windows_now": 1}, map_view_note="No map panel.")
    assert r["section"]["touch.quests"][0] == "Only with Touch screen over the game on."
    r = rules(driver, **{**BASE, "windows_now": 1}, map_view_note="No map panel.")
    assert r["section"]["touch.quests"][0].startswith("From the next start")


def test_r6_r7_r8_renderer(driver):
    r = rules(driver, **{**BASE, "software_now": 1, "renderer_now": "Software", "renderer": "software"})
    assert {"graphics.resolution", "display.vsync"} <= r["disabled"]
    assert r["row"]["graphics.resolution"][1] == "display.renderer"
    assert r["row"]["display.vsync"] == ("Not with the software renderer.", "display.renderer", "amber")
    assert "windows_entry_disabled" in r["flags"]
    assert r["row"]["display.renderer"] == ("In use: Software.", "-", "dim")
    r = rules(driver, **{**BASE, "renderer": "software", "screens": "windows"})
    assert r["section"]["touch.screens"] == (
        "The software renderer has no second window, so the touch screen won't show.", "display.renderer")
    assert r["row"]["display.renderer"][0] == "In use: Vulkan; Software from the next start."


@pytest.mark.parametrize("widescreen, window, wider", [
    ("off", 1.7778, False), ("16:10", 1.7778, False), ("4:3", 1.7778, False), ("16:9", 1.7778, True),
    ("21:9", 1.7778, True), ("1.6", 1.7778, False), ("match", 16 / 9, True), ("match", 1.6, False),
    ("match", 0.5, False)])
def test_r9_r10_widescreen(driver, widescreen, window, wider):
    r = rules(driver, **{**BASE, "widescreen": widescreen, "window_aspect": window})
    if wider:
        assert not {"display.wide_render", "display.hud_edges"} & r["disabled"]
    else:
        assert {"display.wide_render", "display.hud_edges"} <= r["disabled"]
        assert r["row"]["display.wide_render"][1] == "display.aspect"
    r = rules(driver, **{**BASE, "widescreen": widescreen, "window_aspect": window, "wide_render": 0})
    if wider:
        assert r["disabled"] == {"display.hud_edges"}
        assert r["row"]["display.hud_edges"] == ("Only with Draw the wider picture on.", "display.wide_render",
                                                  "amber")


def test_r11_r12_controls(driver):
    r = rules(driver, **BASE, controller_mode=1)
    assert r["section"]["controls.mouse"][1] == "controls.input_mode"
    assert {"controls.mouse_sensitivity", "controls.mouse_deadzone", "controls.key_guides"} <= r["disabled"]
    assert r["row"]["controls.key_guides"][1] == "controls.input_mode"
    assert "keys_top" not in rules(driver, **BASE, controller_auto=1)
    assert rules(driver, **BASE, controller_auto=0)["keys_top"][1] == "controls.controller_auto"


@pytest.mark.parametrize("fps, hz, note", [(120, 60, True), (60, 0, False), (60, 60, False), (144, 59.94, True),
                                           (30, 60, False)])
def test_r13_refresh_rate(driver, fps, hz, note):
    r = rules(driver, **{**BASE, "fps": fps, "refresh_rate": hz})
    if note:
        text, link = r["section"]["display.frame_rate"]
        assert link == "display.refresh_rate"
        assert f"refreshes {hz:g} times a second" in text
        assert r["disabled"] == set()
    else:
        assert "display.frame_rate" not in r["section"]


def test_r14_r15_r18(driver):
    r = rules(driver, **BASE, in_room=1)
    assert r["section"]["gameplay.speed"][1] == "-"
    r = rules(driver, **BASE, unbound__0="ZR", unbound__3="ZL")
    assert r["row"]["gameplay.kick"] == ("ZR has no key or controller button: give it one in Keys.", "keys.zr",
                                         "amber")
    assert r["row"]["gameplay.skip_end"] == ("ZL + ZR need a key or controller button each: Keys.", "keys.zl",
                                             "amber")
    r = rules(driver, **{**BASE, "buttons__1": "UP"}, unbound__1="UP")
    assert r["row"]["gameplay.target"][0].startswith("D-pad up has no key")
    assert r["row"]["gameplay.target"][1] == "keys.dpad_up"
    assert "graphics.dither" in rules(driver, **BASE, dither=1, resolution=0)["row"]
    assert "graphics.dither" not in rules(driver, **BASE, dither=1, resolution=1)["row"]
    assert "graphics.dither" not in rules(driver, **BASE, dither=1, resolution=3, software_now=1)["row"]


def test_hidden_sub_settings(driver):
    r = rules(driver, **{**BASE, "map_on": 0})  # H2
    assert {"touch.map_key", "touch.map_size", "touch.target_view", "touch.target_size"} <= r["hidden"]
    assert "touch.hud_editor" not in r["hidden"]
    r = rules(driver, **{**BASE, "target_on": 0, "map_placed": 1})  # H3, H4
    assert {"touch.target_size", "touch.target_where"} <= r["hidden"]
    assert not {"touch.map_x", "touch.map_y"} & r["hidden"]
    r = rules(driver, **{**BASE, "buttons__3": ""})  # H5
    assert "gameplay.skip_end_hold" in r["hidden"]
