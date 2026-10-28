// A lobby's list of rooms (GET <lobby>/lobby: server/lobby/mh4u_lobby.py, or any lobby that
// speaks Azahar's web API), for the launcher's Online screen.
#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace Launcher::Lobby {

struct Room {
    std::string name, description, owner, address, game;
    u16 port = 0;
    u64 game_id = 0;
    u32 max_players = 0;
    bool password = false;
    std::vector<std::string> players;  ///< nicknames
    bool mh4u = false;                 ///< set up for the player's Monster Hunter 4 Ultimate (its region)
};

struct List {
    bool ok = false;
    std::string error;
    std::vector<Room> rooms;  ///< MH4U's first, then by name
};

constexpr u64 MH4U_USA = 0x0004000000126300;
constexpr u64 MH4U_EUROPE = 0x0004000000126100;

/// The MH4U a room is set up for, from the player's game (its program id, 0 for none yet):
/// Europe's for Europe's game, else USA's.
constexpr u64 RoomGame(u64 program_id) {
    return program_id == MH4U_EUROPE ? MH4U_EUROPE : MH4U_USA;
}

/// The rooms a lobby lists, those for `game` (the player's MH4U, by program id) first. Blocks for
/// a few seconds at most: run it on a worker thread.
List Fetch(const std::string& lobby_url, u64 game = MH4U_USA);

/// The JSON a lobby answers with, as rooms (for tests and Fetch). A room for the other region's
/// MH4U counts as another game's (whether the regions play together isn't known yet), named with
/// its region.
List Parse(const std::string& json, u64 game = MH4U_USA);

/// A lobby address as typed ("lobby.example.org:24872") with its scheme ("http://...").
std::string NormalizeUrl(const std::string& url);

} // namespace Launcher::Lobby
