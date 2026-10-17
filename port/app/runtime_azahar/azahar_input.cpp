// AzaharRuntime: the 3DS's buttons and sticks as Azahar numbers and names them, the input profile
// it reads, and what a controller does for an input of it.
#include <string>

#include <SDL.h>
#include <fmt/format.h>

#include "common/param_package.h"
#include "common/settings.h"
#include "runtime_azahar/azahar_runtime.h"

namespace App::Azahar {

// the app numbers the 3DS's buttons and sticks (buttons.h) the way Azahar does
static_assert(Button::A == Settings::NativeButton::A && Button::B == Settings::NativeButton::B &&
              Button::X == Settings::NativeButton::X && Button::Y == Settings::NativeButton::Y &&
              Button::Up == Settings::NativeButton::Up && Button::Down == Settings::NativeButton::Down &&
              Button::Left == Settings::NativeButton::Left && Button::Right == Settings::NativeButton::Right &&
              Button::L == Settings::NativeButton::L && Button::R == Settings::NativeButton::R &&
              Button::Start == Settings::NativeButton::Start && Button::Select == Settings::NativeButton::Select &&
              Button::Debug == Settings::NativeButton::Debug && Button::Gpio14 == Settings::NativeButton::Gpio14 &&
              Button::ZL == Settings::NativeButton::ZL && Button::ZR == Settings::NativeButton::ZR &&
              Button::Home == Settings::NativeButton::Home && Button::Power == Settings::NativeButton::Power &&
              Button::Count == Settings::NativeButton::NumButtons);
static_assert(Analog::CirclePad == Settings::NativeAnalog::CirclePad &&
              Analog::CStick == Settings::NativeAnalog::CStick &&
              Analog::Count == Settings::NativeAnalog::NumAnalogs);

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
