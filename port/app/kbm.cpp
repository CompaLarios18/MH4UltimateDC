#include "kbm.h"

#include <algorithm>
#include <cmath>

namespace App::Kbm {

std::pair<float, float> MouseCamera::Tick(double dt, const CameraSettings& s) {
    dt = std::clamp(dt, 0.0005, 0.1);
    const double alpha = 1.0 - std::exp(-dt / 0.03);
    vx += (pending_x / dt - vx) * alpha;
    vy += (pending_y / dt - vy) * alpha;
    pending_x = pending_y = 0;
    const double full = 1500.0 * 50.0 / std::clamp(s.sensitivity, 1, 100);
    double x = vx / full;
    double y = -vy / full * std::clamp(s.vertical, 10, 200) / 100.0;  // the mouse up: the stick up
    if (s.invert_y) {
        y = -y;
    }
    const double length = std::hypot(x, y);
    if (length < 0.02) {
        return {0.0f, 0.0f};
    }
    // past the game's dead zone at once: a slow move turns the camera slowly instead of not at all
    const double dead = std::clamp(s.deadzone, 0, 60) / 100.0;
    const double pushed = dead + (1.0 - dead) * std::min(length, 1.0);
    return {static_cast<float>(x * pushed / length), static_cast<float>(y * pushed / length)};
}

void ItemScroll::Queue(int direction) {
    queued = std::clamp(queued + (direction < 0 ? -1 : 1), -5, 5);
}

int ItemScroll::Next() {
    const int code = queued < 0 ? y : a;
    queued += queued < 0 ? 1 : -1;
    return code;
}

KeyEvents ItemScroll::Tick() {
    if (phase == 0) {
        if (queued == 0) {
            return {};
        }
        phase = 1;
        count = 0;
        return {{l, true}};
    }
    if (++count < frames) {
        return {};
    }
    count = 0;
    switch (phase) {
    case 1:
        button = Next();
        phase = 2;
        return {{button, true}};
    case 2:
        phase = 3;
        return {{button, false}};
    default:
        if (queued != 0) {
            button = Next();
            phase = 2;
            return {{button, true}};
        }
        phase = 0;
        return {{l, false}};
    }
}

KeyEvents Pulses::Start(int code) {
    for (auto& [held_code, left] : held) {
        if (held_code == code) {
            left = frames;
            return {};
        }
    }
    held.emplace_back(code, frames);
    return {{code, true}};
}

KeyEvents Pulses::Tick() {
    KeyEvents events;
    for (auto& [code, left] : held) {
        if (--left <= 0) {
            events.emplace_back(code, false);
        }
    }
    held.erase(std::remove_if(held.begin(), held.end(), [](const auto& h) { return h.second <= 0; }), held.end());
    return events;
}

} // namespace App::Kbm
