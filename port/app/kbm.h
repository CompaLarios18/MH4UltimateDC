// The keyboard-and-mouse mode's arithmetic, on its own (no SDL, no Azahar) so that it can be
// tested: the mouse's speed as a push on the C-stick, and the timed presses that scroll the item
// bar (L held, then Y or A) and hold a wheel's key for a moment. input.cpp drives them once a
// frame.
#pragma once

#include <utility>
#include <vector>

namespace App::Kbm {

struct CameraSettings {
    int sensitivity = 50;  ///< 1-100: at 50, 1500 pixels a second push the stick all the way
    int vertical = 100;    ///< 10-200: up and down in % of left and right
    bool invert_y = false;
    int deadzone = 12;     ///< 0-60: the least push a moving mouse gives, % of the way
};

/// The mouse's speed, smoothed over a few hundredths of a second (the game reads the stick at
/// its own pace, and a push held only for a frame it doesn't read would be lost), as a push on
/// the stick: x to the right, y up (the mouse moved up pushes the stick up, unless inverted).
class MouseCamera {
public:
    void Motion(double dx, double dy) {
        pending_x += dx;
        pending_y += dy;
    }
    /// After `dt` seconds: the stick's push, each part -1..1 and the whole at most 1 long.
    std::pair<float, float> Tick(double dt, const CameraSettings& settings);
    void Reset() {
        pending_x = pending_y = vx = vy = 0;
    }

private:
    double pending_x = 0, pending_y = 0;  ///< pixels since the last tick
    double vx = 0, vy = 0;                ///< pixels a second, smoothed
};

/// A key pressed or let go: (code, pressed).
using KeyEvents = std::vector<std::pair<int, bool>>;

/// The item bar's scroll, the game's own way: L held, then Y (to the left) or A (to the right)
/// pressed and let go once for each scroll asked for, then L let go. Each step lasts `frames`
/// frames so that the game sees it; the codes are the ones to press for L, Y and A.
class ItemScroll {
public:
    ItemScroll(int l, int y, int a, int frames = 3) : l(l), y(y), a(a), frames(frames) {}
    /// One scroll more, to the left (`direction` < 0) or the right; at most 5 waiting either way.
    void Queue(int direction);
    /// A frame: the keys to press and let go now.
    KeyEvents Tick();
    bool Idle() const {
        return phase == 0 && queued == 0;
    }

private:
    int Next();

    int l, y, a, frames;
    int queued = 0;  ///< < 0: scrolls to the left still to do; > 0: to the right
    int phase = 0;   ///< 0 idle, 1 L held, 2 Y or A held as well, 3 Y or A let go (L still held)
    int count = 0;
    int button = 0;
};

/// Keys held for a few frames (a wheel's turn is a press without a release).
class Pulses {
public:
    explicit Pulses(int frames = 4) : frames(frames) {}
    /// Presses `code` (or holds it longer if it is still held from before).
    KeyEvents Start(int code);
    /// A frame: the keys whose time is up are let go.
    KeyEvents Tick();

private:
    int frames;
    std::vector<std::pair<int, int>> held;  ///< (code, frames left)
};

} // namespace App::Kbm
