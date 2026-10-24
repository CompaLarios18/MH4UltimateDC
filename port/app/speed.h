// The app's own pause and fast forward (mh4u-app.ini's pause_key and fast_forward_key), for
// playing alone: the game stops where it is (the picture and the in-game menu stay), or runs at
// fast_forward_speed (2x as it comes) through the long walks and gathering. In a room the others'
// games run on with this one (net sync would make them wait for it, and the game drops a player
// whose console stops), so both are off there and the keys only say why.
//
// What the keys did and what the main loop does about it (main.cpp: Azahar's temporary frame limit,
// vsync off while fast, the last picture drawn again while paused); no Azahar here, so the rules are
// tested on their own (tests/test_app_speed.py). One thread: the emulation thread's.
#pragma once

#include <cstdint>
#include <string>

namespace App::Speed {

class Control {
public:
    /// fast_forward_speed in % of the 3DS's (0: as fast as the PC can; otherwise 110 to 1000) and
    /// fast_forward_hold (the key held, or pressed once to start and once to stop).
    void Configure(int fast_percent, bool hold);

    /// The keys, as they go down and up; `now_ms` is a steady clock in milliseconds.
    void PauseKey(bool down, std::uint32_t now_ms);
    void FastForwardKey(bool down, std::uint32_t now_ms);

    /// Whether this PC plays in a room now (or is joining one): pause and fast forward stop, and
    /// the keys only say why while it does.
    void SetInRoom(bool in_room, std::uint32_t now_ms);

    bool Paused() const {
        return paused;
    }
    /// Fast forward is on (and the game isn't paused).
    bool Fast() const {
        return fast && !paused;
    }
    /// The speed the game runs at, in % of the 3DS's: -1 for the usual one (mh4u-app.ini's, no
    /// temporary limit), 0 for as fast as the PC can.
    int Limit() const;

    /// The line over the game: "Paused", "Fast forward 2x", for a few seconds why a key did
    /// nothing, or "" for nothing.
    std::string Badge(std::uint32_t now_ms) const;

    /// The speed as the badge says it: 200 -> "2x", 150 -> "1.5x", 0 -> "max".
    static std::string SpeedText(int percent);

private:
    void Notice(const char* text, std::uint32_t now_ms);

    int fast_percent = 200;
    bool hold = false;
    bool paused = false;
    bool fast = false;
    bool in_room = false;
    bool pause_down = false, fast_down = false;
    std::string notice;
    std::uint32_t notice_at = 0;
};

/// The app's one control (the window's keys, the main loop, the menu's badge).
Control& Get();

} // namespace App::Speed
