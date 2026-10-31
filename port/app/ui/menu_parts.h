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

/// Esc or a controller's B in a question (Dear ImGui's B doesn't close those itself).
inline bool CancelPressed() {
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

} // namespace App::Ui::MenuParts
