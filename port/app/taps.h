// Touch taps: keys that touch the touch screen at a point while they are held, for a touch panel
// or an item shortcut without the stylus. mh4u-app.ini's touch_tap_1 to touch_tap_8 keep them as
// "Z @ 52 220 Mega potion": the keys as a 3DS button takes them, the point in the touch screen's
// pixels, and a name for the key guides. A key is on one input only, and the rule that keeps it so
// between the taps and the app's other bindings is here too.
//
// input.cpp hands the taps to Azahar's touch-from-button map, which the console's touch screen
// reads while the stylus (the mouse, a finger) isn't touching it. No Azahar here, so the rules are
// tested on their own (tests/test_app_taps.py).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace App::Taps {

/// touch_tap_1 to touch_tap_8.
inline constexpr int MAX = 8;
/// The touch screen's size in pixels.
inline constexpr int WIDTH = 320, HEIGHT = 240;

struct Tap {
    std::string keys;    ///< as a 3DS button's: "Z", "F | Mouse Right"; empty: none yet
    int x = WIDTH / 2;   ///< 0-319, from the touch screen's left edge
    int y = HEIGHT / 2;  ///< 0-239, from its top edge
    std::string name;    ///< what it touches, for the key guides ("Mega potion"); may be empty
};

/// A tap from its line, "KEYS @ X Y [NAME]" (X and Y apart by spaces or a comma, each kept to the
/// screen); false when the line has no point.
bool Parse(const std::string& line, Tap& tap);
/// A tap's line: "Z @ 52 220 Mega potion", "@ 160 120" for one without keys.
std::string Format(const Tap& tap);

/// A binding's key names: "F | Mouse Right" -> {"F", "Mouse Right"} (empty ones left out).
std::vector<std::string> Names(const std::string& keys);
/// And back, empty names left out: {"F", "", "Mouse Right"} -> "F | Mouse Right".
std::string Join(const std::vector<std::string>& names);
/// Whether two names are one key (SDL takes a key's name in either case).
bool Same(const std::string& a, const std::string& b);
/// `keys` with its `slot`th key (the menu's columns) set to `key`; an empty `key` takes that one
/// out, and the keys after it move up.
std::string WithKey(const std::string& keys, int slot, const std::string& key);

/// The app's other settings with keys, for the rule. The bindings (a 3DS button's, the item bar's,
/// the stylus key: "F | Mouse Right") and the sticks ("W,S,A,D,Left Shift") give a key up to a
/// tap, each named as in mh4u-app.ini; the app's own keys ("Escape", "F11", "F12 | M", ...) stay
/// theirs.
struct Others {
    std::vector<std::pair<std::string, std::string*>> bindings;
    std::vector<std::pair<std::string, std::string*>> sticks;
    std::vector<std::string> own;
};

/// Whether `key` is one of the app's own keys.
bool IsOwn(const Others& others, const std::string& key);

/// One key, one input, for taps as a settings file has them: a tap's key that is one of the app's
/// own leaves the tap, a key on two taps stays on the first, and the bindings and sticks give up
/// the taps' keys (a stick's direction keeps its place, empty). A line for the log per change.
std::vector<std::string> Settle(std::vector<Tap>& taps, Others& others);

/// `key` becomes tap `tap`'s `slot`th key (the menu's capture) and leaves every other input it was
/// on. Nothing happens for a tap that isn't there.
void Give(std::vector<Tap>& taps, int tap, int slot, const std::string& key, Others& others);

/// `key` went to another input (the menu's capture): it leaves the taps. True if one had it.
bool Release(std::vector<Tap>& taps, const std::string& key);

} // namespace App::Taps
