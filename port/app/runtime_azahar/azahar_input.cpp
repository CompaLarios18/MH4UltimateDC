// AzaharRuntime: the 3DS's buttons and sticks as Azahar numbers and names them, the input profile
// it reads, and what a controller does for an input of it.
#include <string>

#include <SDL.h>
#include <fmt/format.h>

#include "common/param_package.h"
#include "common/settings.h"
#include "runtime_azahar/azahar_runtime.h"
#include "util/buttons.h"

namespace App::Azahar {

// the app numbers the 3DS's buttons and sticks (util/buttons.h) the way Azahar does. The two sides
// are different enumeration types, so they are compared as ints.
constexpr int V(auto e) {
    return static_cast<int>(e);
}
static_assert(V(Button::A) == V(Settings::NativeButton::A) &&
              V(Button::B) == V(Settings::NativeButton::B) &&
              V(Button::X) == V(Settings::NativeButton::X) &&
              V(Button::Y) == V(Settings::NativeButton::Y) &&
              V(Button::Up) == V(Settings::NativeButton::Up) &&
              V(Button::Down) == V(Settings::NativeButton::Down) &&
              V(Button::Left) == V(Settings::NativeButton::Left) &&
              V(Button::Right) == V(Settings::NativeButton::Right) &&
              V(Button::L) == V(Settings::NativeButton::L) &&
              V(Button::R) == V(Settings::NativeButton::R) &&
              V(Button::Start) == V(Settings::NativeButton::Start) &&
              V(Button::Select) == V(Settings::NativeButton::Select) &&
              V(Button::Debug) == V(Settings::NativeButton::Debug) &&
              V(Button::Gpio14) == V(Settings::NativeButton::Gpio14) &&
              V(Button::ZL) == V(Settings::NativeButton::ZL) &&
              V(Button::ZR) == V(Settings::NativeButton::ZR) &&
              V(Button::Home) == V(Settings::NativeButton::Home) &&
              V(Button::Power) == V(Settings::NativeButton::Power) &&
              V(Button::Count) == V(Settings::NativeButton::NumButtons));
static_assert(V(Analog::CirclePad) == V(Settings::NativeAnalog::CirclePad) &&
              V(Analog::CStick) == V(Settings::NativeAnalog::CStick) &&
              V(Analog::Count) == V(Settings::NativeAnalog::NumAnalogs));

namespace {

std::string ControllerButtonName(int button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
        return "A (bottom)";
    case SDL_CONTROLLER_BUTTON_B:
        return "B (right)";
    case SDL_CONTROLLER_BUTTON_X:
        return "X (left)";
    case SDL_CONTROLLER_BUTTON_Y:
        return "Y (top)";
    case SDL_CONTROLLER_BUTTON_BACK:
        return "Back";
    case SDL_CONTROLLER_BUTTON_GUIDE:
        return "Guide";
    case SDL_CONTROLLER_BUTTON_START:
        return "Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return "Left stick click";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return "Right stick click";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return "LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return "RB";
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return "D-pad up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return "D-pad down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return "D-pad left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return "D-pad right";
    default:
        return fmt::format("button {}", button);
    }
}

std::string ControllerAxisName(int axis) {
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
    case SDL_CONTROLLER_AXIS_LEFTY:
        return "Left stick";
    case SDL_CONTROLLER_AXIS_RIGHTX:
    case SDL_CONTROLLER_AXIS_RIGHTY:
        return "Right stick";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        return "LT";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        return "RT";
    default:
        return fmt::format("axis {}", axis);
    }
}

/// What a controller does for an input in the profile (Settings' parameters), or empty.
std::string ControllerName(const std::string& params) {
    const Common::ParamPackage p{params};
    const std::string engine = p.Get("engine", "");
    if (engine == "mh4u_any") { // the app's keyboard + controller pair (input.cpp)
        for (const char* side : {"b", "a"}) {
            if (std::string name = ControllerName(p.Get(side, "")); !name.empty()) {
                return name;
            }
        }
        return "";
    }
    if (engine == "analog_from_button") {
        return "";
    }
    if (engine == "keyboard" || engine.empty()) {
        return "";
    }
    if (engine != "sdl" && engine != "gcpad") {
        return engine;
    }
    if (p.Has("button")) {
        return ControllerButtonName(p.Get("button", -1));
    }
    if (p.Has("axis_x")) {
        return ControllerAxisName(p.Get("axis_x", -1));
    }
    if (p.Has("axis")) {
        return ControllerAxisName(p.Get("axis", -1));
    }
    if (p.Has("hat")) {
        return "D-pad (" + p.Get("direction", "") + ")";
    }
    return "controller";
}

} // namespace

const char* AzaharRuntime::ButtonSettingName(int button) const {
    return Settings::NativeButton::mapping[button];
}

const char* AzaharRuntime::StickSettingName(int stick) const {
    return Settings::NativeAnalog::mapping[stick];
}

std::string AzaharRuntime::ButtonBinding(int button) const {
    return Settings::values.current_input_profile.buttons[button];
}

std::string AzaharRuntime::StickBinding(int stick) const {
    return Settings::values.current_input_profile.analogs[stick];
}

std::string AzaharRuntime::DescribeControllerInput(const std::string& binding) const {
    return ControllerName(binding);
}

} // namespace App::Azahar
