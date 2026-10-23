// Derived from Azahar src/input_common/sdl/sdl_impl.cpp, src/input_common/sdl/sdl_joystick.cpp,
// src/input_common/analog_from_button.cpp, src/common/param_package.cpp and
// src/core/hle/service/hid/hid.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// all five say "Licensed under GPLv2 or any later version"): the default controller binds
// (xinput_to_3ds_mapping, GetSDLControllerButtonBind, GetSDLControllerAnalogBindByGUID), the SDL
// devices' arithmetic (SDLJoystick::GetAxis and GetAnalog, SDLAnalog), the keyboard stick
// (Analog::GetStatus), the parameter strings (ParamPackage) and the circle pad's scale and rounding
// (MAX_CIRCLEPAD_POS, roundf).
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (sdl_impl.cpp)
// Copyright 2026 Citra Emulator Project / Azahar Emulator Project (sdl_joystick.cpp)
// Copyright Citra Emulator Project / Lime3DS Emulator Project (analog_from_button.cpp)
// Copyright 2017 Citra Emulator Project (param_package.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.cpp)
//
// See azahar_rules.h; docs/native/input.md says where each rule comes from.
#include "input/azahar_rules.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/buttons.h"
#include "util/strings.h"

namespace mh4n::app::azahar {

PadBind DefaultButtonBind(int app_button) {
    // xinput_to_3ds_mapping (the layout GetSDLControllerButtonBind uses unless the 3DS's A was
    // pressed on the controller's A during mapping, which the app's profile never does); the
    // triggers are the axes of GetSDLControllerButtonBind's ZL and ZR branches
    PadBind bind;
    const auto button = [&bind](int index) {
        bind.kind = PadBind::Kind::Button;
        bind.index = index;
    };
    const auto trigger = [&bind](int axis) {
        bind.kind = PadBind::Kind::Axis;
        bind.index = axis;
        bind.threshold = 0.5f;
        bind.greater = true;
    };
    switch (app_button) {
    case App::Button::A:
        button(PadButtonId::East);
        break;
    case App::Button::B:
        button(PadButtonId::South);
        break;
    case App::Button::X:
        button(PadButtonId::North);
        break;
    case App::Button::Y:
        button(PadButtonId::West);
        break;
    case App::Button::Up:
        button(PadButtonId::DpadUp);
        break;
    case App::Button::Down:
        button(PadButtonId::DpadDown);
        break;
    case App::Button::Left:
        button(PadButtonId::DpadLeft);
        break;
    case App::Button::Right:
        button(PadButtonId::DpadRight);
        break;
    case App::Button::L:
        button(PadButtonId::LeftShoulder);
        break;
    case App::Button::R:
        button(PadButtonId::RightShoulder);
        break;
    case App::Button::Start:
        button(PadButtonId::Start);
        break;
    case App::Button::Select:
        button(PadButtonId::Back);
        break;
    case App::Button::Home:
        button(PadButtonId::Guide);
        break;
    case App::Button::ZL:
        trigger(PadAxisId::TriggerLeft);
        break;
    case App::Button::ZR:
        trigger(PadAxisId::TriggerRight);
        break;
    default:
        break;  // Debug, Gpio14 and Power: SDL_CONTROLLER_BUTTON_INVALID
    }
    return bind;
}

StickAxes DefaultStickAxes(int app_analog) {
    if (app_analog == App::Analog::CirclePad) {
        return {PadAxisId::LeftX, PadAxisId::LeftY};
    }
    if (app_analog == App::Analog::CStick) {
        return {PadAxisId::RightX, PadAxisId::RightY};
    }
    return {};
}

s16 CirclePadRaw(float value) {
    // "These are rounded rather than truncated on actual hardware"
    return static_cast<s16>(std::roundf(value * kMaxCirclePadPosition));
}

float AxisValue(int raw) {
    return static_cast<float>(raw) / 32767.0f;
}

namespace {

// SDLJoystick::GetAnalog: the 3DS's y axis is SDL's inverted; a point outside the unit circle is
// normalized onto it
std::pair<float, float> Stick(int raw_x, int raw_y) {
    float x = AxisValue(raw_x);
    float y = -AxisValue(raw_y);
    float r = x * x + y * y;
    if (r > 1.0f) {
        r = std::sqrt(r);
        x /= r;
        y /= r;
    }
    return {x, y};
}

}  // namespace

std::pair<float, float> SdlAnalog(const std::vector<std::pair<int, int>>& raw_axes, bool single,
                                  float deadzone) {
    deadzone = std::clamp(deadzone, 0.0f, 0.99f);
    if (single && !raw_axes.empty()) {
        const auto [x, y] = Stick(raw_axes.front().first, raw_axes.front().second);
        const float r = std::sqrt((x * x) + (y * y));
        if (r > deadzone) {
            return {x / r * (r - deadzone) / (1 - deadzone), y / r * (r - deadzone) / (1 - deadzone)};
        }
        return {0.0f, 0.0f};
    }
    // more than one: whichever is pushed furthest
    float r_max = 0.0f, x_max = 0.0f, y_max = 0.0f;
    for (const auto& [raw_x, raw_y] : raw_axes) {
        const auto [x, y] = Stick(raw_x, raw_y);
        const float r = std::sqrt((x * x) + (y * y));
        if (r > r_max) {
            x_max = x;
            y_max = y;
            r_max = r;
        }
    }
    if (r_max > deadzone) {
        return {x_max / r_max * (r_max - deadzone) / (1 - deadzone),
                y_max / r_max * (r_max - deadzone) / (1 - deadzone)};
    }
    return {0.0f, 0.0f};
}

std::pair<float, float> KeyStick(bool up, bool down, bool left, bool right, bool modifier,
                                 float modifier_scale) {
    constexpr float SQRT_HALF = 0.707106781f;
    int x = 0, y = 0;
    if (right) {
        ++x;
    }
    if (left) {
        --x;
    }
    if (up) {
        ++y;
    }
    if (down) {
        --y;
    }
    const float coef = modifier ? modifier_scale : 1.0f;
    return {static_cast<float>(x) * coef * (y == 0 ? 1.0f : SQRT_HALF),
            static_cast<float>(y) * coef * (x == 0 ? 1.0f : SQRT_HALF)};
}

namespace {

constexpr char KEY_VALUE_SEPARATOR = ':';
constexpr char PARAM_SEPARATOR = ',';
constexpr char ESCAPE_CHARACTER = '$';
constexpr char EMPTY_PLACEHOLDER[] = "[empty]";

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

}  // namespace

ParamPackage::ParamPackage(const std::string& serialized) {
    if (serialized == EMPTY_PLACEHOLDER) {
        return;
    }
    for (const std::string& pair : App::Util::SplitString(serialized, PARAM_SEPARATOR)) {
        std::vector<std::string> key_value = App::Util::SplitString(pair, KEY_VALUE_SEPARATOR);
        if (key_value.size() != 2) {
            continue;  // Azahar logs "invalid key pair" and goes on
        }
        for (std::string& part : key_value) {
            part = ReplaceAll(part, "$0", std::string(1, KEY_VALUE_SEPARATOR));
            part = ReplaceAll(part, "$1", std::string(1, PARAM_SEPARATOR));
            part = ReplaceAll(part, "$2", std::string(1, ESCAPE_CHARACTER));
        }
        data.insert_or_assign(key_value[0], key_value[1]);
    }
}

bool ParamPackage::Has(const std::string& key) const {
    return data.find(key) != data.end();
}

std::string ParamPackage::Get(const std::string& key, const std::string& fallback) const {
    const auto it = data.find(key);
    return it == data.end() ? fallback : it->second;
}

int ParamPackage::GetInt(const std::string& key, int fallback) const {
    const auto it = data.find(key);
    if (it == data.end()) {
        return fallback;
    }
    try {
        return std::stoi(it->second);
    } catch (const std::logic_error&) {
        return fallback;
    }
}

float ParamPackage::GetFloat(const std::string& key, float fallback) const {
    const auto it = data.find(key);
    if (it == data.end()) {
        return fallback;
    }
    try {
        return std::stof(it->second);
    } catch (const std::logic_error&) {
        return fallback;
    }
}

}  // namespace mh4n::app::azahar
