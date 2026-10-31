// Why the quest's map view can't show, and what the app says about it. The map view is
// mh4u.ini's quest_overlay: in a quest the touch overlay's corner shows the map panel of the
// player's panel layout alone (hooks.cpp). It can't with another screen layout, with the touch
// overlay or quest_overlay off, with a code library too old for the quest panels' hook point,
// or with no map panel in the panels; and the touch-screen key can switch quests to the whole
// touch screen. Then the map's size and place, the HUD layout editor and the key change nothing
// on the screen, and the app says why: a sentence under the in-game menu's map settings (and the
// target camera's), and a short line on the screen when the key has nothing to switch. No Azahar
// here (hooks.cpp gathers the state), so it is tested on its own (tests/test_map_view.py).
#pragma once

#include <string>

namespace Port::MapView {

/// What the quest's small touch panels had the last time they ran (QuestHud::updatePanels).
struct Panels {
    bool seen = false;      ///< they have run since the game started: a quest was played
    bool in_quest = false;  ///< and that quest is still on
    int map_slot = -1;      ///< the map panel's slot (0, 1, 3 or 4), -1: none the map view can show
    int target_slot = -1;   ///< the target camera panel's (0-5), -1: none
};

/// What the map view needs.
struct State {
    /// The custom screen layout, the touch screen in the corner and in landscape (the app's
    /// screens = overlay): the only layout that shows a part of the touch screen.
    bool custom_layout = true;
    bool touch_overlay = true;  ///< touch_overlay on: the touch screen over the game in a corner
    bool quest_overlay = true;  ///< quest_overlay on: in quests that corner shows the map alone
    bool panels_hook = true;    ///< the code library has the hook point the panels are seen through
    bool switched = false;      ///< the touch-screen key switched quests to the whole touch screen
    Panels panels;
};

/// Why the map view can't show, the first that applies; None when it can (as far as the panels
/// seen so far tell: before a quest's panels have run, nothing is known of them).
enum class Block { None, Layout, TouchOverlay, QuestOverlay, PanelsHook, NoMapPanel, Switched };
Block Blocked(const State& state);

/// The sentence for the in-game menu's map settings and its HUD layout editor: why the map view
/// doesn't show and these settings change nothing, or "" when it shows.
std::string MapNote(const State& state);

/// The sentence for the target camera's view (`target_overlay` on): it shows beside the map's
/// only, and only with the target camera panel in the panels; "" when it shows, or when it's off.
std::string TargetNote(const State& state, bool target_overlay);

/// The touch-screen key, pressed: "" when it can switch quests between the map and the whole
/// touch screen, else a short line for the screen saying why there is nothing to switch.
std::string KeyNote(const State& state);

} // namespace Port::MapView
