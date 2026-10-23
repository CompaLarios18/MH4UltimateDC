// The rules of Azahar's input that mh4u-native keeps without Azahar's input engine (N3-07): the
// default controller binds, the arithmetic of its SDL devices, the keyboard stick, the circle pad's
// scale in HID's shared memory and its parameter strings. Plain functions and tables, no SDL: the
// mapper (input_mapper.h) puts them together and docs/native/input.md says where each comes from.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::app::azahar {

/// A controller's buttons and axes as App::InputEvent carries them: SDL2's numbering, which SDL3's
/// SDL_GAMEPAD_BUTTON_* and SDL_GAMEPAD_AXIS_* keep for these (SDL3 names the face buttons by
/// position: SOUTH is SDL2's A, EAST its B, WEST its X, NORTH its Y).
namespace PadButtonId {
inline constexpr int Invalid = -1;
inline constexpr int South = 0;  // SDL2's A, the bottom face button
inline constexpr int East = 1;   // B, right
inline constexpr int West = 2;   // X, left
inline constexpr int North = 3;  // Y, top
inline constexpr int Back = 4;
inline constexpr int Guide = 5;
inline constexpr int Start = 6;
inline constexpr int LeftStick = 7;
inline constexpr int RightStick = 8;
inline constexpr int LeftShoulder = 9;
inline constexpr int RightShoulder = 10;
inline constexpr int DpadUp = 11;
inline constexpr int DpadDown = 12;
inline constexpr int DpadLeft = 13;
inline constexpr int DpadRight = 14;
}  // namespace PadButtonId

namespace PadAxisId {
inline constexpr int LeftX = 0;
inline constexpr int LeftY = 1;
inline constexpr int RightX = 2;
inline constexpr int RightY = 3;
inline constexpr int TriggerLeft = 4;
inline constexpr int TriggerRight = 5;
inline constexpr int Count = 8;  // the axes the mapper keeps per controller
}  // namespace PadAxisId

/// What a controller does for a 3DS button: a button, or an axis past a threshold.
struct PadBind {
    enum class Kind { None, Button, Axis };
    Kind kind = Kind::None;
    int index = -1;          ///< the button's or axis' number
    float threshold = 0.5f;  ///< Axis: the value (-1..1) the button is pressed past
    bool greater = true;     ///< Axis: pressed above the threshold ("+"), below it ("-")
};

/// The default bind of a 3DS button (an App::Button) on a controller, Azahar's layout for an Xbox
/// style pad: the 3DS's A on the pad's right face button, B on the bottom one, X on the top, Y on
/// the left; ZL and ZR on the triggers. None for the buttons a controller has no bind for. The
/// Guide button is Azahar's bind for HOME: the table has it, the app leaves it to its menu.
PadBind DefaultButtonBind(int app_button);

/// The axes of a controller's stick for a 3DS stick (an App::Analog): the left stick for the
/// circle pad, the right for the C-stick.
struct StickAxes {
    int x = -1;
    int y = -1;
};
StickAxes DefaultStickAxes(int app_analog);

/// HID's calibrated limit of the circle pad: a stick pushed all the way reads this far.
inline constexpr int kMaxCirclePadPosition = 0x9A;

/// A stick's push (-1..1, y up) as HID's shared memory has it, rounded (not truncated).
s16 CirclePadRaw(float value);

/// An axis' value as the SDL engine reads it: the raw value over 32767.
float AxisValue(int raw);

/// The SDL engine's analog device over the raw x and y axes of the controllers it matched
/// (`raw_axes`, in order): y flipped for the 3DS, a push past the unit circle cut back to it, and
/// the dead zone (0..0.99) taken off. `single` (a port was named): that controller's stick; else
/// the one pushed furthest.
std::pair<float, float> SdlAnalog(const std::vector<std::pair<int, int>>& raw_axes, bool single,
                                  float deadzone);

/// The "analog from button" device: a stick from four keys and a modifier that scales it
/// (diagonals shortened so the push stays on the unit circle).
std::pair<float, float> KeyStick(bool up, bool down, bool left, bool right, bool modifier,
                                 float modifier_scale = 0.5f);

/// Azahar's parameter strings ("engine:sdl,guid:...,port:0,api:controller,button:1"): pairs
/// separated by commas, a key and its value by a colon, with "$0", "$1" and "$2" for a colon, a
/// comma and a dollar sign inside a piece. Ones that are no pair are left out.
class ParamPackage {
public:
    explicit ParamPackage(const std::string& serialized);
    bool Has(const std::string& key) const;
    std::string Get(const std::string& key, const std::string& fallback) const;
    /// The value as a number; the fallback when it is missing or not one.
    int GetInt(const std::string& key, int fallback) const;
    float GetFloat(const std::string& key, float fallback) const;

private:
    std::map<std::string, std::string> data;
};

}  // namespace mh4n::app::azahar
