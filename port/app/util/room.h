// Derived from Azahar src/network/room_member.h and src/network/room.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later, both): the reasons a room turns a
// member away or ends the connection (RoomMember::Error), the states of the connection
// (RoomMember::State, without Uninitialized) and the MAC address type (MacAddress), as the app's
// own types.
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (room_member.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (room.h)
//
// What the app's network room (multiplayer.h) knows of the runtime's room client. The runtime
// converts its own room types to these (runtime_azahar/azahar_network.cpp).
#pragma once

#include <array>

#include "util/types.h"

namespace App {

/// A console's 802.11 address.
using MacAddress = std::array<u8, 6>;

/// Why the connection to a room ended or was refused.
enum class RoomError {
    LostConnection,      ///< the connection closed
    HostKicked,          ///< the host removed this member
    Unknown,
    NameCollision,       ///< someone in the room has the nickname
    MacCollision,        ///< someone in the room has the preferred MAC address
    ConsoleIdCollision,  ///< someone in the room has the same console ID
    WrongVersion,        ///< the room speaks another version of the protocol
    WrongPassword,
    CouldNotConnect,     ///< nothing answered at the address
    RoomIsFull,
    HostBanned,
    PermissionDenied,    ///< a moderation request without the rights
    NoSuchUser,
};

/// Where the connection to a room stands.
enum class RoomState {
    Idle,       ///< not connected
    Joining,    ///< waiting for the room's answer
    Joined,     ///< in the room
    Moderator,  ///< in the room, with moderation rights
};

} // namespace App
