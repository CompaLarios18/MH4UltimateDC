// Small helpers the in-game menu's files share (ui/menu*.cpp only).
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <imgui.h>

namespace App::Ui::MenuParts {

/// A note that says why something can't apply, today's notice colour.
inline const ImVec4 NOTE_COLOUR(1.0f, 0.75f, 0.45f, 1.0f);
/// Why the last key wasn't taken.
inline const ImVec4 CAPTURE_COLOUR(1.0f, 0.6f, 0.4f, 1.0f);

/// Gameplay's lists of 3DS buttons, as mh4u.ini names them (OFF: none; written "off"), and End
/// the quest's combinations; RefreshTexts names what presses each, in the same order.
inline constexpr const char* BUTTONS[] = {"OFF", "A",  "B",     "X",  "Y",    "L",    "R",
                                          "ZL",  "ZR", "START", "UP", "DOWN", "LEFT", "RIGHT"};
inline constexpr const char* COMBOS[] = {"OFF", "ZL+ZR", "L+R", "ZL+R", "L+ZR", "ZL+ZR+START"};

inline bool IsOn(const std::string& v) {
    return v == "1" || v == "on" || v == "yes" || v == "true";
}

inline bool IsOff(const std::string& v) {
    return v == "0" || v == "off" || v == "no" || v == "false";
}

inline std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline std::string Upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

/// mh4u.ini's value of a list of 3DS buttons as the menu's lists name it, upper case and without
/// spaces as the hooks read it ("zl + zr" -> "ZL+ZR"), "" for none.
inline std::string ButtonsOf(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
                value.end());
    value = Upper(value);
    return value.empty() || IsOff(Lower(value)) || value == "NONE" ? "" : value;
}

// The menu's shortcuts, Ctrl+F for the search and Ctrl+Tab for the tabs, as a Mac's own apps have
// them there: Dear ImGui reads Cmd as Ctrl on macOS (io.ConfigMacOSXBehaviors), so the search is
// Cmd+F, and the tabs take the Control key itself (ImGuiMod_Super there), Cmd+Tab being the system's.
#ifdef __APPLE__
inline constexpr ImGuiKeyChord TABS_MOD = ImGuiMod_Super;
inline constexpr const char* SEARCH_HINT = "Search (Cmd+F)";
inline constexpr const char* PAGE_KEYS = "Enter: choose   Esc: back   Ctrl+Tab: tabs   Cmd+F: search";
#else
inline constexpr ImGuiKeyChord TABS_MOD = ImGuiMod_Ctrl;
inline constexpr const char* SEARCH_HINT = "Search (Ctrl+F)";
inline constexpr const char* PAGE_KEYS = "Enter: choose   Esc: back   Ctrl+Tab: tabs   Ctrl+F: search";
#endif

/// Esc or a controller's B in a question (Dear ImGui's B doesn't close those itself).
inline bool CancelPressed() {
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

} // namespace App::Ui::MenuParts
