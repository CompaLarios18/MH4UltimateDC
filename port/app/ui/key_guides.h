// The key guides: while the game is played with the keyboard and mouse, the keys for what the
// hunter does drawn small over the game (mh4u-app.ini's [Controls] key_guides, a corner or off),
// each with the 3DS button the game's own prompts name, and the touch taps' keys with what they
// touch. key_guides_key (F1) and the menu's Controls tab show and hide them.
#pragma once

#include <string>

namespace App {
struct Config;
}

namespace App::Ui {

/// The corners key_guides takes, "off" first (the frame-rate counter's).
inline constexpr const char* KEY_GUIDES_PLACES[] = {"off", "top-left", "top-right", "bottom-left",
                                                    "bottom-right"};

/// Draws the guides in the corner `place`, from the keys `config` binds, in a Dear ImGui frame
/// (the menu's context); they take no input.
void DrawKeyGuides(const Config& config, const std::string& place);

} // namespace App::Ui
