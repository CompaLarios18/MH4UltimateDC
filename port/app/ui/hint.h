// Lines over the game while the in-game menu is closed, once a quest is complete
// (port/src/hooks.h): which buttons tap the touch screen's Skip and which, held, end the
// wait; the hold's progress; and, online, who hasn't asked to end it yet. And for a few
// seconds after the touch-screen key changed nothing, why.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace App {

struct Config;

namespace Ui {

struct Hint {
    std::string skip;      ///< the keys / controller buttons that skip ("C / RT"), or ""
    std::string end;       ///< those held to end the wait ("Q+C / LT+RT"), or ""
    float hold = 0.0f;     ///< 0..1 of the hold, 1 once asked
    bool requested = false;          ///< this player asked to end the wait
    std::vector<std::string> waiting_for;  ///< online: port players who haven't asked yet
    bool Empty() const {
        return skip.empty() && end.empty();
    }
};

/// What to show now: nothing (Empty()) when the skip isn't offered, no button is bound, or
/// mh4u.ini's skip_hint is off. The key names come from `config` (mh4u-app.ini) and the
/// controller mapping. Cheap when there is nothing.
Hint CurrentHint(const Config& config);

/// Draws the hint at the bottom middle of the screen, in a Dear ImGui frame (the menu's
/// context): small and translucent, it takes no input. `flash`: its frame pulses.
void DrawHint(const Hint& hint, bool flash);

/// What presses mh4u.ini's 3DS buttons `buttons` ("ZR", "ZL+ZR"): "C / RT", "Q+C / LT+RT"; the
/// keys or the controller's buttons alone when only they have them all; "" when one of the
/// buttons has neither (the in-game menu's lists of 3DS buttons, and its note that says so).
std::string ButtonInputs(const Config& config, const std::string& buttons);

/// The touch-screen key (mh4u-app.ini's touch_screen_key) or a controller's right-stick click,
/// pressed: in a quest it switches the corner between the map and the whole touch screen
/// (Port::Hooks::PressQuestMapKey). When it can't (a panel layout without the map panel, say),
/// TouchScreenNotice says why for a few seconds. The window's events, on the emulation thread.
void TouchScreenKey(std::uint32_t now_ms);

/// Why the touch-screen key changed nothing, while that is news; "" otherwise. `now_ms`: the
/// same clock as TouchScreenKey's (SDL's ticks).
std::string TouchScreenNotice(std::uint32_t now_ms);

/// Draws a notice at the top middle of the screen in a Dear ImGui frame, small; it takes no
/// input. `below_badge`: under the pause and fast-forward badge, which is up as well.
void DrawNotice(const std::string& text, bool below_badge);

} // namespace Ui
} // namespace App
