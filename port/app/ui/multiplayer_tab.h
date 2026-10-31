// The in-game menu's Online tab: the room you're in and who is in it, joining another one
// (an invite, an address, a lobby's list, the last room), hosting one with an invite to share,
// and leaving, all while the game runs (multiplayer.h does the work).
#pragma once

#include <functional>

namespace App::Ui {

class Layer;

/// In the menu's frame, on the Online tab. `settings_rows`, the menu's own rows (net sync), go
/// under the status line; `before_pages` runs just before the Join a room and Host a room buttons
/// (where the menu's LT and RT land).
void DrawMultiplayerTab(Layer& layer, const std::function<void()>& settings_rows = {},
                        const std::function<void()>& before_pages = {});

/// Shows Join a room (0) or Host a room (1), with the cursor on its button (the menu's search).
void ShowMultiplayerPage(int page);

} // namespace App::Ui
