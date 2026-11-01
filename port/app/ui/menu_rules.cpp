#include "ui/menu_rules.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace App::Ui::Rules {

namespace {

using R = RowId;
using S = SectionId;

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

/// The renderer's name as the menu shows it.
std::string RendererName(const std::string& value) {
    const std::string v = Lower(value);
    return v == "opengl" ? "OpenGL" : v == "software" ? "Software" : "Vulkan";
}

/// "60", "59.94": a refresh rate as a number of times a second.
std::string Number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
    return text;
}

} // namespace

const Note* Verdict::RowNote(RowId row) const {
    for (const auto& [r, note] : rows) {
        if (r == row) {
            return &note;
        }
    }
    return nullptr;
}

bool IsOverlay(const std::string& screens) {
    return screens != "single" && screens != "stacked" && screens != "side" && screens != "windows";
}

float Aspect(const std::string& widescreen, float window_aspect) {
    const std::string v = Lower(widescreen);
    if (v == "match" || v == "auto" || v == "screen") {
        // as the hooks resolve it: the window's shape when it is wider than tall, else off
        return window_aspect > 1.0f ? window_aspect : 0.0f;
    }
    if (const auto colon = v.find(':'); colon != std::string::npos) {
        const float w = std::strtof(v.c_str(), nullptr);
        const float h = std::strtof(v.c_str() + colon + 1, nullptr);
        return h > 0.0f ? w / h : 0.0f;
    }
    const float aspect = std::strtof(v.c_str(), nullptr);
    return aspect > 0.0f ? aspect : 0.0f;
}

RowId KeyRowOf(const std::string& button) {
    static constexpr std::pair<const char*, RowId> rows[] = {
        {"A", R::KeysA},          {"B", R::KeysB},          {"X", R::KeysX},         {"Y", R::KeysY},
        {"L", R::KeysL},          {"R", R::KeysR},          {"ZL", R::KeysZl},       {"ZR", R::KeysZr},
        {"START", R::KeysStart},  {"SELECT", R::KeysSelect}, {"UP", R::KeysDpadUp},  {"DOWN", R::KeysDpadDown},
        {"LEFT", R::KeysDpadLeft}, {"RIGHT", R::KeysDpadRight}};
    for (const auto& [name, row] : rows) {
        if (button == name) {
            return row;
        }
    }
    return R::Count;
}

std::string ButtonText(const std::string& buttons) {
    std::string text;
    std::size_t start = 0;
    while (start <= buttons.size()) {
        std::size_t end = buttons.find('+', start);
        if (end == std::string::npos) {
            end = buttons.size();
        }
        std::string part = buttons.substr(start, end - start);
        part = part == "UP"      ? "D-pad up"
               : part == "DOWN"  ? "D-pad down"
               : part == "LEFT"  ? "D-pad left"
               : part == "RIGHT" ? "D-pad right"
                                 : part;
        text += (text.empty() ? "" : " + ") + part;
        start = end + 1;
    }
    return text;
}

Verdict Evaluate(const Facts& f) {
    Verdict v;
    const auto hide = [&v](RowId row) { v.hidden[static_cast<int>(row)] = true; };
    const auto disable = [&v](RowId row) { v.disabled[static_cast<int>(row)] = true; };
    const auto disable_section = [&](SectionId section) {
        for (int i = 0; i < Index::ROW_COUNT; ++i) {
            if (Index::SectionOf(static_cast<RowId>(i)) == section) {
                disable(static_cast<RowId>(i));
            }
        }
    };
    // one note a section: the first rule to give one wins (the rules below go in priority order)
    const auto section_note = [&v](SectionId section, const Note& note) {
        if (v.sections[static_cast<int>(section)].Empty()) {
            v.sections[static_cast<int>(section)] = note;
        }
    };
    const auto row_note = [&v](RowId row, const std::string& text, RowId link, bool amber) {
        v.rows.push_back({row, Note{text, link, amber}});
    };

    // ── H1-H5: the sub-settings of a switch or list right above them ──
    if (!f.overlay_on) { // H1
        hide(R::TouchCorner);
        hide(R::TouchSize);
        hide(R::TouchOpacity);
    }
    if (!f.map_on) { // H2
        for (const RowId row : {R::TouchMapKey, R::TouchMapSize, R::TouchMapWhere, R::TouchMapX, R::TouchMapY,
                                R::TouchTargetView, R::TouchTargetSize, R::TouchTargetWhere, R::TouchTargetX,
                                R::TouchTargetY}) {
            hide(row);
        }
    }
    if (!f.target_on) { // H3
        for (const RowId row : {R::TouchTargetSize, R::TouchTargetWhere, R::TouchTargetX, R::TouchTargetY}) {
            hide(row);
        }
    }
    if (!f.map_placed) { // H4
        hide(R::TouchMapX);
        hide(R::TouchMapY);
    }
    if (!f.target_placed) {
        hide(R::TouchTargetX);
        hide(R::TouchTargetY);
    }
    if (f.buttons[static_cast<int>(Buttons::End)].empty()) { // H5
        hide(R::GameplaySkipEndHold);
    }

    // ── Touch screen: R1, R2, R3 (the layout), R4, R5 (the player's panels, #54), R7 ──
    const bool overlay = IsOverlay(f.screens);
    const bool windows_wait = f.windows_now && overlay; // R3
    // R3 the other way: a window of its own chosen, from the next start; the corner runs until then
    const bool corner_until_next = f.screens == "windows" && !f.windows_now && f.custom_now;
    const bool corner = overlay || corner_until_next;
    if (!corner) { // R1
        disable_section(S::TouchCorner);
        disable_section(S::TouchQuests);
        const Note note{"Only with Screens set to \"The game, the touch screen in a corner\".", R::TouchScreens};
        section_note(S::TouchCorner, note);
        section_note(S::TouchQuests, note);
    } else if (!f.overlay_on) { // R2
        disable_section(S::TouchQuests);
        section_note(S::TouchQuests, Note{"Only with Touch screen over the game on.", R::TouchOverlay});
    }
    if (windows_wait) { // R3
        const Note note{"From the next start: until then the touch screen has a window of its own."};
        section_note(S::TouchCorner, note);
        section_note(S::TouchQuests, note);
    }
    if (corner_until_next && Lower(f.renderer) != "software") { // R3 (with the software renderer, R7 says why not)
        const Note note{"Until the next start: then the touch screen has a window of its own."};
        section_note(S::TouchCorner, note);
        section_note(S::TouchQuests, note);
    }
    // Edit HUD layout: with the overlay running (R1, R2, R3, and while a change of the screens is
    // still on its way to the renderer)
    if (!corner || !f.overlay_on || windows_wait || !f.custom_now) {
        v.hud_editor_off = true;
        disable(R::TouchHudEditor);
    }
    const bool layout_ok = corner && f.overlay_on && !windows_wait;
    const bool map_note = layout_ok && f.map_on && !f.map_view_note.empty();
    if (map_note) { // R4: the rows stay enabled, they apply once the view can show
        section_note(S::TouchQuests, Note{f.map_view_note});
    }
    if (layout_ok && !map_note && !v.Hidden(R::TouchTargetView) && !f.target_view_note.empty()) { // R5
        row_note(R::TouchTargetView, f.target_view_note, R::Count, true);
    }
    if (Lower(f.renderer) == "software") { // R7
        v.windows_entry_disabled = true;
        if (f.screens == "windows") {
            section_note(S::TouchScreens,
                         Note{"The software renderer has no second window, so the touch screen won't show.",
                              R::DisplayRenderer});
        }
    }

    // ── Display, Graphics: R6, R8, R9, R10, R13, R18 ──
    if (f.software_now) { // R6
        disable(R::GraphicsResolution);
        disable(R::DisplayVsync);
        row_note(R::GraphicsResolution, "The software renderer draws at the 3DS's own resolution.",
                 R::DisplayRenderer, true);
        row_note(R::DisplayVsync, "Not with the software renderer.", R::DisplayRenderer, true);
    }
    { // R8: the one chosen as the app started, and another running, didn't start (main.cpp tries the
      // others then); the next start does what this one did
        const std::string chosen = RendererName(f.renderer);
        const bool fell_back = chosen != f.renderer_now && Lower(f.renderer) == Lower(f.renderer_started);
        row_note(R::DisplayRenderer,
                 chosen == f.renderer_now ? "In use: " + f.renderer_now + "."
                 : fell_back ? "In use: " + f.renderer_now + " (" + chosen + " didn't start: see mh4u_log.txt)."
                             : "In use: " + f.renderer_now + "; " + chosen + " from the next start.",
                 R::Count, fell_back);
    }
    // the aspect ratio and Draw the wider picture apply from the next start; HUD at the screen's
    // edges at once, to the picture the game draws now, and from the next start to the one chosen
    const bool wider = Aspect(f.widescreen, f.window_aspect) > 5.0f / 3.0f + 0.01f;
    const bool wide_next = wider && f.wide_render;
    if (!wider) { // R9
        disable(R::DisplayWideRender);
        row_note(R::DisplayWideRender, "Only with an aspect ratio wider than 5:3: 16:9, 21:9 or 32:9.",
                 R::DisplayAspect, true);
    }
    if (!f.wide_now && !wide_next) { // R9, R10
        disable(R::DisplayHudEdges);
        if (wider) {
            row_note(R::DisplayHudEdges, "Only with Draw the wider picture on.", R::DisplayWideRender, true);
        }
    } else if (!f.wide_now) {
        row_note(R::DisplayHudEdges, "Applies from the next start, when the game draws the wider picture.", R::Count,
                 false);
    }
    if (f.refresh_rate > 0.0f && f.refresh_rate < static_cast<float>(f.fps)) { // R13
        const std::string hz = Number(f.refresh_rate);
        section_note(S::DisplayFrameRate, Note{"The screen refreshes " + hz + " times a second, so the game shows "
                                               "at most " + hz + " frames a second.",
                                               R::DisplayRefreshRate});
    }
    if (f.dither && f.resolution != 1 && !f.software_now) { // R18
        row_note(R::GraphicsDither, "At this internal resolution the dither pattern shows as a fine grid: off is "
                                    "cleaner.",
                 R::Count, false);
    }

    // ── Controls, Keys: R11, R12 ──
    if (f.controller_mode) { // R11
        disable_section(S::ControlsMouse);
        disable(R::ControlsKeyGuides);
        section_note(S::ControlsMouse, Note{"With Play with set to Controller, the mouse is the stylus: these don't "
                                            "apply.",
                                            R::ControlsInputMode});
        row_note(R::ControlsKeyGuides, "They show only while you play with the keyboard and mouse.",
                 R::ControlsInputMode, true);
    }
    if (!f.controller_auto) { // R12
        v.keys_top = Note{"No controller is mapped automatically, so the Controller column is empty.",
                          R::ControlsControllerAuto};
    }

    // ── Gameplay: R14, R15 ──
    if (f.in_room) { // R14
        section_note(S::GameplaySpeed,
                     Note{"In a room, pause and fast forward are off: the others' games run on with yours."});
    }
    static constexpr RowId LISTS[] = {R::GameplayKick, R::GameplayTarget, R::GameplaySkip, R::GameplaySkipEnd};
    for (int i = 0; i < static_cast<int>(Buttons::Count); ++i) { // R15
        const std::string& buttons = f.buttons[i];
        const std::string& unbound = f.unbound[i];
        if (buttons.empty() || unbound.empty()) {
            continue;
        }
        const bool combo = buttons.find('+') != std::string::npos;
        row_note(LISTS[i],
                 combo ? ButtonText(buttons) + " need a key or controller button each: Keys."
                       : ButtonText(buttons) + " has no key or controller button: give it one in Keys.",
                 KeyRowOf(unbound), true);
    }
    return v;
}

} // namespace App::Ui::Rules
