// When a setting of the in-game menu can't take effect, and why: from what the menu has chosen and
// what runs now, which rows are hidden (the sub-settings of a switch that is off), which are shown
// disabled, and the one-line notes that say why with a link to what blocks them. No Dear ImGui and
// no Azahar here (tests/test_app_menu_index.py compiles it with the index); the menu gathers the
// facts for the tab it draws and draws what Evaluate says.
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "ui/settings_index.h"

namespace App::Ui::Rules {

using Index::RowId;
using Index::SectionId;

/// The lists of 3DS buttons in Gameplay.
enum class Buttons { Kick, Target, Skip, End, Count };

struct Facts {
    // ── Chosen: mh4u.ini with the menu's changes, mh4u-app.ini's settings ──
    std::string screens = "overlay";  ///< overlay, single, stacked, side, windows
    std::string renderer = "vulkan";  ///< from the next start: vulkan, opengl, software
    bool overlay_on = false;          ///< touch_overlay isn't off
    bool map_on = true;               ///< quest_overlay isn't off
    bool map_placed = false;          ///< Map position: Where you put it
    bool target_on = true;            ///< target_overlay isn't off
    bool target_placed = false;
    std::string widescreen = "off";   ///< as written, lower case
    bool wide_render = false;
    bool dither = false;
    int resolution = 0;               ///< mh4u-app.ini's; 0: as large as the window needs
    int fps = 30;                     ///< 30 when the file has 0 or less
    float refresh_rate = 0.0f;        ///< 0: auto
    bool controller_mode = false;     ///< Play with: Controller
    bool controller_auto = true;
    /// The 3DS buttons each list has chosen ("ZR", "ZL+ZR"; "" for None), and the first of them
    /// with no key and no controller button ("" when each has one)
    std::array<std::string, static_cast<int>(Buttons::Count)> buttons, unbound;

    // ── Running now ──
    bool software_now = false;        ///< the software renderer
    bool windows_now = false;         ///< the touch screen in a window of its own
    bool custom_now = true;           ///< the custom layout (the overlay's)
    bool in_room = false;
    std::string renderer_now = "Vulkan";
    float window_aspect = 16.0f / 9.0f; ///< for widescreen = match

    // ── The hooks' sentences (#54: Port::Hooks::MapViewNote, TargetViewNote), on Touch screen and
    // in the editor only; "" when the views can show ──
    std::string map_view_note, target_view_note;
};

struct Note {
    std::string text;
    RowId link = RowId::Count; ///< the row its Go to link goes to (Count: none)
    bool amber = true;         ///< false: dim, it only informs
    bool Empty() const {
        return text.empty();
    }
};

struct Verdict {
    std::array<bool, Index::ROW_COUNT> hidden{};   ///< not drawn: the switch above is off
    std::array<bool, Index::ROW_COUNT> disabled{}; ///< drawn disabled: a note says why
    std::array<Note, Index::SECTION_COUNT> sections; ///< one under a section's title
    std::vector<std::pair<RowId, Note>> rows;        ///< under a row's control
    Note keys_top;                                   ///< at the top of Keys
    bool windows_entry_disabled = false; ///< Screens' "in a window of its own" can't be chosen
    bool hud_editor_off = false;         ///< Edit HUD layout can't open

    const Note* RowNote(RowId row) const;
    bool Hidden(RowId row) const {
        return hidden[static_cast<int>(row)];
    }
    bool Disabled(RowId row) const {
        return disabled[static_cast<int>(row)];
    }
    const Note& SectionNote(SectionId section) const {
        return sections[static_cast<int>(section)];
    }
};

Verdict Evaluate(const Facts& facts);

/// Whether a screens value is the overlay (the game, the touch screen in a corner): any value but
/// single, stacked, side and windows, as the app's settings read it.
bool IsOverlay(const std::string& screens);

/// A widescreen value's aspect ratio ("16:9", "1.7778"; match, auto and screen: the window's); 0
/// for off.
float Aspect(const std::string& widescreen, float window_aspect);

/// The Keys row of a 3DS button as mh4u.ini names it ("ZR", "UP"), RowId::Count for none.
RowId KeyRowOf(const std::string& button);

/// A 3DS button as the menu shows it: "ZR", "D-pad up"; a combo "ZL + ZR".
std::string ButtonText(const std::string& buttons);

} // namespace App::Ui::Rules
