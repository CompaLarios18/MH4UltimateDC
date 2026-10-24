#include "speed.h"

#include <algorithm>
#include <cstdio>

namespace App::Speed {

namespace {
constexpr std::uint32_t NOTICE_MS = 3000;
constexpr const char* IN_ROOM = "Pause and fast forward are for playing alone: not in a room";
} // namespace

void Control::Configure(int percent, bool hold_key) {
    fast_percent = percent <= 0 ? 0 : std::clamp(percent, 110, 1000);
    hold = hold_key;
    if (hold && !fast_down) {
        fast = false;
    }
}

void Control::Notice(const char* text, std::uint32_t now_ms) {
    notice = text;
    notice_at = now_ms;
}

void Control::PauseKey(bool down, std::uint32_t now_ms) {
    const bool pressed = down && !pause_down;
    pause_down = down;
    if (!pressed) {
        return;
    }
    if (in_room) {
        Notice(IN_ROOM, now_ms);
        return;
    }
    paused = !paused;
    notice.clear();
}

void Control::FastForwardKey(bool down, std::uint32_t now_ms) {
    const bool pressed = down && !fast_down;
    fast_down = down;
    if (in_room) {
        if (pressed) {
            Notice(IN_ROOM, now_ms);
        }
        fast = false;
        return;
    }
    if (hold) {
        fast = down;
    } else if (pressed) {
        fast = !fast;
    } else {
        return;
    }
    if (fast) {
        // fast forward goes on from a pause too: it is what the player asked for now
        paused = false;
    }
    notice.clear();
}

void Control::SetInRoom(bool now_in_room, std::uint32_t now_ms) {
    if (now_in_room == in_room) {
        return;
    }
    in_room = now_in_room;
    if (in_room && (paused || fast)) {
        paused = false;
        fast = false;
        Notice("In a room: the game runs at its usual speed", now_ms);
    }
}

int Control::Limit() const {
    return Fast() ? fast_percent : -1;
}

std::string Control::SpeedText(int percent) {
    if (percent <= 0) {
        return "max";
    }
    char text[16];
    if (percent % 100 == 0) {
        std::snprintf(text, sizeof(text), "%dx", percent / 100);
    } else if (percent % 10 == 0) {
        std::snprintf(text, sizeof(text), "%d.%dx", percent / 100, percent / 10 % 10);
    } else {
        std::snprintf(text, sizeof(text), "%d.%02dx", percent / 100, percent % 100);
    }
    return text;
}

std::string Control::Badge(std::uint32_t now_ms) const {
    if (paused) {
        return "Paused";
    }
    if (fast) {
        return "Fast forward " + SpeedText(fast_percent);
    }
    if (!notice.empty() && now_ms - notice_at < NOTICE_MS) {
        return notice;
    }
    return "";
}

Control& Get() {
    static Control control;
    return control;
}

} // namespace App::Speed
