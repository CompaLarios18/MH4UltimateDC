// A line over the game while the in-game menu is closed: "Press C / RT to skip" while the game
// offers to skip the wait after a quest is complete (port/src/hooks.h, SkipOffered).
#pragma once

#include <string>

namespace App {

struct Config;

namespace Ui {

/// The buttons to name in the hint now ("C / RT": the key `config` (mh4u-app.ini) gives the 3DS
/// button bound to the skip, and the controller's button for it when a controller is mapped),
/// or "" when there is no hint: the skip isn't offered, no button is bound to it, or mh4u.ini's
/// skip_hint is off. Cheap when there is none.
std::string HintButtons(const Config& config);

/// Draws "Press <buttons> to skip" at the bottom middle of the screen, in a Dear ImGui frame
/// (the menu's context): small and translucent, it takes no input. `flash`: its frame pulses.
void DrawHint(const std::string& buttons, bool flash);

} // namespace Ui
} // namespace App
