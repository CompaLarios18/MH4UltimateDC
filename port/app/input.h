// The app's input: the keyboard and a controller at the same time on every 3DS button, a
// controller mapped on its own when one is plugged in, and the keys of the app's hotkeys.
#pragma once

#include <string>

namespace App {

struct Config;

namespace Input {

/// Registers the input engine that joins two bindings ("mh4u_any": pressed when either is),
/// so that the keyboard and a controller both work. Call after InputCommon::Init().
void Register();

/// Settings::values.current_input_profile from the config's keys, the first controller found
/// (controller = auto) and any full parameters. Call again when a controller comes or goes,
/// then reload the input devices.
void BuildProfile(const Config& config);

/// The SDL scancode for a key name ("W", "Left Shift", "F12"), 0 if there is no such key.
int KeyCode(const std::string& name);

/// A line for the log: the controller the profile uses, or that there is none.
std::string ControllerDescription();

} // namespace Input
} // namespace App
