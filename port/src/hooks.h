// Native code around game functions: the port's hooks on the hook points that the code
// library was recompiled with (config/hooks.txt).
#pragma once

#include <string>
#include <vector>

#include "core/frontend/top_screen.h"

struct RCodeInfo;

namespace Port::Hooks {

/// Installs the port's hooks on a freshly loaded code library. Options come from
/// "mh4u.ini" in Azahar's user directory (`key = value` lines: fps, widescreen,
/// trace_frames), and environment variables override them (MH4U_FPS, MH4U_WIDESCREEN,
/// MH4U_TRACE_FRAMES). Returns lines for the log: options in effect and hooks the library
/// has no hook point for (it was built with an older hooks.txt).
std::vector<std::string> Install(const RCodeInfo& info);

/// Changes one of the options above while the game runs (the runner's --set, for tests: one
/// player of a session at another frame rate once everyone is in, say). Only options that
/// the hooks read as the game runs can change: fps, refresh_rate, dither, render_flags_set,
/// render_flags_clear, kick, target, quest_overlay, hud_edges, the screen filters (bloom,
/// motion_blur, depth_of_field, radial_blur, haze) and the trace_* options. Returns false
/// for any other key.
bool SetOption(const std::string& key, const std::string& value);

/// In quests, switches the touch overlay between the map alone (quest_overlay) and the whole
/// touch screen (for the chat or the item shortcuts), until switched back. Safe to call from
/// any thread; it takes effect a moment later.
void ToggleQuestMap();

/// How many of the port's hook points the code library loaded last didn't have (it was built
/// with an older config/hooks.txt, so the options behind them don't work until it is rebuilt).
int MissingHookPoints();

/// The touch_overlay option: "top-right 30 85" (corner, height in % of the window's, opacity
/// in %), "on" (top right, 30%, 85%) or "off".
Layout::ScreenOverlay ParseOverlay(std::string value);

/// The quest_overlay option: "map" (while a quest's touch panels are up, the touch overlay
/// shows the map panel alone, at its own scale), "map 40" (the map 40% of the window's
/// height) or "off". Returns that height as a fraction, 0 for the overlay's scale, < 0 off.
float ParseQuestOverlay(std::string value);

/// The touch overlay `overlay` cut down to the quest's map in touch-panel slot `slot` (0-2
/// the left column from the top, 3-5 the right one), `height` times the window's height
/// (0: at the overlay's own scale).
Layout::ScreenOverlay MapOverlay(const Layout::ScreenOverlay& overlay, int slot, float height);

/// What the touch overlay shows in a quest (quest_overlay), worked out once a frame. The map
/// alone while the quest's small touch panels are up; the whole touch screen while the quest
/// menu or a big panel is on it (both take taps), when the panels have no map, and once the
/// panels have been gone from the touch screen for a while (the quest is over). Through
/// shorter breaks, a change of area or a cutscene, and while someone talks (the panels stay
/// on the touch screen but don't run), it keeps what it shows, so that the corner doesn't jump
/// between the two. A change shows once it has lasted a moment. Times are emulated seconds,
/// so the frame rate makes no difference.
class QuestOverlayChoice {
public:
    static constexpr double SETTLE = 0.1;  // a change shows once it has lasted this long
    static constexpr double HOLD = 5.0;    // without the panels this long: out of the quest
    static constexpr int FREE = -2;        // `forced`: no slot forced

    /// One frame at `time`: `panels` whether the small panels ran, with the map in `map_slot`
    /// (-1 none); `map_drawn` whether the map panel was drawn on the touch screen all the same;
    /// `touch_ui` whether the quest menu or a big panel was up. `forced` overrides the choice
    /// (-1: the whole touch screen). Returns true when Shown() changes.
    bool Update(double time, bool panels, int map_slot, bool map_drawn, bool touch_ui, int forced = FREE);

    /// The map's slot the overlay shows, or -1 for the whole touch screen.
    int Shown() const {
        return shown;
    }

private:
    int shown = -1;
    int next = -1;           // what the frames since `next_since` asked for
    double next_since = 0;
    bool panels_seen = false;
    double panels_last = 0;  // when the small panels last ran or were drawn
};

} // namespace Port::Hooks
