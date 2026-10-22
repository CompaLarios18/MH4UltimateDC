// The app's input: the keyboard, the mouse and a controller at the same time on every 3DS button,
// a controller mapped on its own when one is plugged in, and the keys of the app's hotkeys. The
// keys and controllers are SDL's, which the executable's host knows (app_window.h, Host); the
// input profile that joins them is the runtime's (IRuntime::BuildInputProfile).
//
// Keyboard and mouse (a mode like Monster Hunter Rise's and World's on PC): the mouse turns the
// camera (it pushes the C-stick by how fast it moves), its buttons and wheel are keys like any
// other ("Mouse Left", "Wheel Up"), a 3DS button takes several keys ("F | Mouse Right"), and the
// wheel scrolls the item bar (L held with Y or A, the game's own way). The mouse's push, the
// camera turn and the timed key presses reach the game through IRuntime (SetMouseStick,
// AddCameraTurn, KeyDown and KeyUp).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace App {

struct Config;

namespace Input {

/// Key codes past SDL's scancodes (which end at 512) for the mouse: its buttons by SDL's numbers
/// (MOUSE_KEY + MouseButton::Left, ...), the wheel's four ways, and the codes the item-bar macro
/// presses (MACRO_KEY + an App::Button), joined to every button's binding.
inline constexpr int MOUSE_KEY = 0x1000;
inline constexpr int WHEEL_UP = 0x1010, WHEEL_DOWN = 0x1011, WHEEL_LEFT = 0x1012, WHEEL_RIGHT = 0x1013;
inline constexpr int MACRO_KEY = 0x1100;

/// Makes the runtime's input profile from the config's keys, the first controller found
/// (controller = auto) and any full parameters. Call again when a controller comes or goes,
/// then reload the input devices (IRuntime::ReloadInputDevices).
void BuildProfile(const Config& config);

/// The code for a key or mouse name ("W", "Left Shift", "F12", "Mouse Right", "Wheel Up"), 0
/// if there is no such key.
int KeyCode(const std::string& name);
/// The code of a mouse button or the wheel by its name ("Mouse Right"), 0 for any other name.
int MouseCode(const std::string& name);
/// The codes of a binding with alternatives, "F | Mouse Right" (unknown names left out).
std::vector<int> KeyCodes(const std::string& spec);
/// A binding's names, "F | Mouse Right" -> {"F", "Mouse Right"} (empty ones left out).
std::vector<std::string> KeyNames(const std::string& spec);
/// The name a code is kept by in mh4u-app.ini ("" if none).
std::string KeyName(int code);
/// A binding's first key ("" if none): what a hint over the game names.
std::string FirstKey(const std::string& spec);

/// A controller's port: how many devices before it (index `index` of `guids`, the GUID of every
/// joystick the host knows, controllers or not) have its GUID, as Azahar's SDL engine numbers
/// controllers of the same kind. For the host's Host::ListControllers.
int ControllerPort(const std::vector<std::string>& guids, std::size_t index);

/// A line for the log: the controller the profile uses, or that there is none.
std::string ControllerDescription();

/// While on (the in-game menu is up), every 3DS button and stick of the profile reads released
/// and centred; once off, a button still held from before reads released until it is let go,
/// and a stick still pushed reads centred until it comes back.
void SetMuted(bool on);

// ── Keyboard and mouse (the main window's events, on the emulation thread) ──

/// input_mode: auto (whichever was used last), controller, keyboard_mouse.
enum class Mode { Auto, Controller, KeyboardMouse };
Mode ParseMode(const std::string& value);
const char* ModeName(Mode mode);

/// The mouse moved this many counts while it is the camera: the game's camera turns by them
/// directly where the port's camera hook runs (degrees a count from the sensitivity, like a
/// PC game's mouse look), else they push the C-stick by the mouse's speed.
void MouseMotion(const Config& config, int dx, int dy);
/// The mouse stopped being the camera: the stick goes back to the centre at once.
void MouseReset();
/// The wheel turned while the mouse is the camera: `code` is WHEEL_UP, ... A wheel bound to the
/// item bar (item_prev / item_next) scrolls it; any other is pressed for a moment like a key.
void Wheel(const Config& config, int code);
/// Once a frame: the mouse's push on the C-stick from its speed, and the timed presses (the
/// item bar's L+Y / L+A, the wheel's).
void Tick(const Config& config);

} // namespace Input
} // namespace App
