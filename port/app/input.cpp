#include "input.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include <SDL.h>
#include <fmt/format.h>

#include "config.h"
#include "kbm.h"
#include "runtime.h"
#include "util/buttons.h"
#include "util/log.h"
#include "util/strings.h"

namespace App::Input {

namespace {

/// The first game controller SDL knows, and its port among controllers of the same kind (as
/// Azahar's SDL engine numbers them).
bool FirstController(ControllerInfo& out) {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        out.guid = guid;
        out.port = 0;
        for (int j = 0; j < i; ++j) {
            char other[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(j), other, sizeof(other));
            out.port += out.guid == other ? 1 : 0;
        }
        const char* name = SDL_GameControllerNameForIndex(i);
        out.name = name ? name : "controller";
        return true;
    }
    return false;
}

std::string controller_description = "no controller";

struct NamedCode {
    const char* name;
    int code;
};
constexpr NamedCode MOUSE_NAMES[] = {
    {"Mouse Left", MOUSE_KEY + SDL_BUTTON_LEFT},   {"Mouse Middle", MOUSE_KEY + SDL_BUTTON_MIDDLE},
    {"Mouse Right", MOUSE_KEY + SDL_BUTTON_RIGHT}, {"Mouse 4", MOUSE_KEY + SDL_BUTTON_X1},
    {"Mouse 5", MOUSE_KEY + SDL_BUTTON_X2},        {"Wheel Up", WHEEL_UP},
    {"Wheel Down", WHEEL_DOWN},                    {"Wheel Left", WHEEL_LEFT},
    {"Wheel Right", WHEEL_RIGHT},
};

bool SameName(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// ── The mouse as the camera ──

Kbm::MouseCamera mouse;
std::chrono::steady_clock::time_point mouse_last{};

// ── Timed presses: the item bar's L+Y / L+A and the wheel's keys ──

Kbm::ItemScroll item_scroll(MACRO_KEY + Button::L, MACRO_KEY + Button::Y, MACRO_KEY + Button::A);
Kbm::Pulses pulses;

/// Presses and lets go the keys of the events, through the runtime's keyboard.
void Apply(const Kbm::KeyEvents& events) {
    for (const auto& [code, pressed] : events) {
        if (pressed) {
            Runtime().KeyDown(code);
        } else {
            Runtime().KeyUp(code);
        }
    }
}

} // namespace

int KeyCode(const std::string& name) {
    if (name.empty()) {
        return 0;
    }
    for (const auto& [mouse_name, code] : MOUSE_NAMES) {
        if (SameName(name, mouse_name)) {
            return code;
        }
    }
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    if (code == SDL_SCANCODE_UNKNOWN) {
        APP_LOG_WARNING("no key called \"{}\"", name);
        return 0;
    }
    return static_cast<int>(code);
}

int MouseCode(const std::string& name) {
    for (const auto& [mouse_name, code] : MOUSE_NAMES) {
        if (SameName(name, mouse_name)) {
            return code;
        }
    }
    return 0;
}

std::vector<std::string> KeyNames(const std::string& spec) {
    std::vector<std::string> names;
    for (const std::string& part : Util::SplitString(spec, '|')) {
        std::string name = Util::StripSpaces(part);
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
    }
    return names;
}

std::vector<int> KeyCodes(const std::string& spec) {
    std::vector<int> codes;
    for (const std::string& name : KeyNames(spec)) {
        if (const int code = KeyCode(name)) {
            codes.push_back(code);
        }
    }
    return codes;
}

std::string KeyName(int code) {
    for (const auto& [name, mouse_code] : MOUSE_NAMES) {
        if (code == mouse_code) {
            return name;
        }
    }
    if (code <= 0 || code >= SDL_NUM_SCANCODES) {
        return "";
    }
    return SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
}

std::string FirstKey(const std::string& spec) {
    const auto names = KeyNames(spec);
    return names.empty() ? "" : names.front();
}

void BuildProfile(const Config& config) {
    ControllerInfo pad;
    const bool have_pad = config.controller_auto && FirstController(pad);
    if (have_pad) {
        controller_description = fmt::format("controller: {} (port {})", pad.name, pad.port);
    } else {
        controller_description = config.controller_auto ? "no controller found" : "controller off";
    }
    Runtime().BuildInputProfile(config, have_pad ? &pad : nullptr);
}

std::string ControllerDescription() {
    return controller_description;
}

void SetMuted(bool on) {
    Runtime().SetInputMuted(on);
}

Mode ParseMode(const std::string& value) {
    if (value == "controller") {
        return Mode::Controller;
    }
    if (value == "keyboard_mouse" || value == "keyboard" || value == "mouse") {
        return Mode::KeyboardMouse;
    }
    return Mode::Auto;
}

const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Controller:
        return "controller";
    case Mode::KeyboardMouse:
        return "keyboard_mouse";
    default:
        return "auto";
    }
}

void MouseMotion(const Config& config, int dx, int dy) {
    if (Runtime().CameraTurnActive()) {
        // the game's camera turns by the mouse itself (the port's camera hook): degrees a count
        const float degrees = static_cast<float>(std::clamp(config.mouse_sensitivity, 1, 100)) * 0.0024f;
        const float vertical = static_cast<float>(std::clamp(config.mouse_vertical, 10, 200)) / 100.0f;
        Runtime().AddCameraTurn(
            static_cast<float>(dx) * degrees * (config.mouse_invert_x ? -1.0f : 1.0f),
            static_cast<float>(-dy) * degrees * vertical * (config.mouse_invert_y ? -1.0f : 1.0f));
        return;
    }
    mouse.Motion(dx, dy);
}

void MouseReset() {
    Runtime().ClearCameraTurn();
    mouse.Reset();
    Runtime().SetMouseStick(0.0f, 0.0f);
}

void Wheel(const Config& config, int code) {
    const auto has = [code](const std::string& spec) {
        const auto codes = KeyCodes(spec);
        return std::find(codes.begin(), codes.end(), code) != codes.end();
    };
    if (has(config.item_prev)) {
        item_scroll.Queue(-1);
    } else if (has(config.item_next)) {
        item_scroll.Queue(1);
    } else {
        Apply(pulses.Start(code));
    }
}

void Tick(const Config& config) {
    const auto now = std::chrono::steady_clock::now();
    const double dt = mouse_last == std::chrono::steady_clock::time_point{}
                          ? 1.0 / 60.0
                          : std::chrono::duration<double>(now - mouse_last).count();
    mouse_last = now;
    Kbm::CameraSettings settings;
    settings.sensitivity = config.mouse_sensitivity;
    settings.vertical = config.mouse_vertical;
    settings.invert_y = config.mouse_invert_y;
    settings.deadzone = config.mouse_deadzone;
    if (Runtime().CameraTurnActive()) {
        mouse.Reset();  // the camera takes the mouse itself: no push on the C-stick
    }
    const auto [x, y] = mouse.Tick(dt, settings);
    Runtime().SetMouseStick(x, y);
    Apply(pulses.Tick());
    Apply(item_scroll.Tick());
}

} // namespace App::Input
