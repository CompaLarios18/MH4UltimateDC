#include "input.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>
#include <SDL.h>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "config.h"
#include "core/frontend/input.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "hooks.h"
#include "kbm.h"

namespace App::Input {

namespace {

namespace NativeAnalog = Settings::NativeAnalog;
namespace NativeButton = Settings::NativeButton;

constexpr const char* ANY = "mh4u_any";
constexpr const char* MOUSE = "mh4u_mouse";

/// SetMuted: the in-game menu is up, and the game gets nothing.
std::atomic<bool> muted{false};

/// Pressed while any of its buttons is. Muted, it reads released, and a button held when the
/// mute ends stays released until it is let go (the B that closed the menu doesn't also roll).
class AnyButton final : public ::Input::ButtonDevice {
public:
    explicit AnyButton(std::vector<std::unique_ptr<::Input::ButtonDevice>> inner_)
        : inner(std::move(inner_)) {}
    bool GetStatus() const override {
        bool pressed = false;
        for (const auto& device : inner) {
            pressed = pressed || (device && device->GetStatus());
        }
        if (muted.load(std::memory_order_relaxed)) {
            held_over = true;
            return false;
        }
        if (held_over) {
            held_over = pressed;
            return false;
        }
        return pressed;
    }

private:
    std::vector<std::unique_ptr<::Input::ButtonDevice>> inner;
    mutable bool held_over = false;  // the emulation thread is the only reader
};

/// Whichever of its sticks is pushed furthest. Muted, it reads centred, and a stick pushed when
/// the mute ends reads centred until it has come back.
class AnyAnalog final : public ::Input::AnalogDevice {
public:
    explicit AnyAnalog(std::vector<std::unique_ptr<::Input::AnalogDevice>> inner_)
        : inner(std::move(inner_)) {}
    std::tuple<float, float> GetStatus() const override {
        const auto length = [](const std::tuple<float, float>& s) {
            return std::hypot(std::get<0>(s), std::get<1>(s));
        };
        std::tuple<float, float> status{};
        for (const auto& device : inner) {
            if (device) {
                const auto s = device->GetStatus();
                if (length(s) > length(status)) {
                    status = s;
                }
            }
        }
        if (muted.load(std::memory_order_relaxed)) {
            held_over = true;
            return {};
        }
        if (held_over) {
            held_over = length(status) > 0.25f;
            return {};
        }
        return status;
    }

private:
    std::vector<std::unique_ptr<::Input::AnalogDevice>> inner;
    mutable bool held_over = false;
};

/// "engine:mh4u_any,n:3,d0:<params>,d1:<params>,...": the inner parameters are serialized
/// again, which escapes them (Common::ParamPackage).
template <typename Device, typename Any>
class AnyFactory final : public ::Input::Factory<Device> {
public:
    std::unique_ptr<Device> Create(const Common::ParamPackage& params) override {
        std::vector<std::unique_ptr<Device>> inner;
        const int n = params.Get("n", 0);
        for (int i = 0; i < n; ++i) {
            const std::string p = params.Get(fmt::format("d{}", i), "");
            if (!p.empty()) {
                inner.push_back(::Input::CreateDevice<Device>(p));
            }
        }
        return std::make_unique<Any>(std::move(inner));
    }
};

/// Every binding goes through "mh4u_any", even a single one, so that SetMuted reaches it.
std::string Join(const std::vector<std::string>& parts) {
    Common::ParamPackage p{{"engine", ANY}};
    int n = 0;
    for (const std::string& part : parts) {
        if (!part.empty()) {
            p.Set(fmt::format("d{}", n++), part);
        }
    }
    if (n == 0) {
        return "";
    }
    p.Set("n", n);
    return p.Serialize();
}

// ── The mouse as the camera ──

/// The C-stick's push from the mouse, written by Tick and read by the stick, both on the
/// emulation thread (atomics so that neither has to be).
std::atomic<float> mouse_stick_x{0.0f}, mouse_stick_y{0.0f};

class MouseStick final : public ::Input::AnalogDevice {
public:
    std::tuple<float, float> GetStatus() const override {
        return {mouse_stick_x.load(std::memory_order_relaxed), mouse_stick_y.load(std::memory_order_relaxed)};
    }
};

class MouseStickFactory final : public ::Input::Factory<::Input::AnalogDevice> {
public:
    std::unique_ptr<::Input::AnalogDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<MouseStick>();
    }
};

Kbm::MouseCamera mouse;
std::chrono::steady_clock::time_point mouse_last{};

// ── Timed presses: the item bar's L+Y / L+A and the wheel's keys ──

Kbm::ItemScroll item_scroll(MACRO_KEY + NativeButton::L, MACRO_KEY + NativeButton::Y,
                            MACRO_KEY + NativeButton::A);
Kbm::Pulses pulses;

void Apply(const Kbm::KeyEvents& events) {
    for (const auto& [code, pressed] : events) {
        if (pressed) {
            InputCommon::GetKeyboard()->PressKey(code);
        } else {
            InputCommon::GetKeyboard()->ReleaseKey(code);
        }
    }
}

struct Controller {
    std::string guid;
    int port = 0;
    std::string name;
};

/// The first game controller SDL knows, and its port among controllers of the same kind (as
/// Azahar's SDL engine numbers them).
bool FirstController(Controller& out) {
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

/// A binding's keys ("F | Mouse Right"), each a keyboard device of Azahar's (the mouse's codes
/// are pressed on the same keyboard by the window).
std::vector<std::string> KeyParams(const std::string& spec) {
    std::vector<std::string> params;
    for (const int code : KeyCodes(spec)) {
        params.push_back(InputCommon::GenerateKeyboardParam(code));
    }
    return params;
}

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
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string StickKeysParam(const std::string& spec) {
    const auto parts = Common::SplitString(spec, ',');
    if (parts.size() < 4) {
        return "";
    }
    int codes[5] = {};
    for (std::size_t i = 0; i < 5 && i < parts.size(); ++i) {
        codes[i] = KeyCode(Common::StripSpaces(parts[i]));
    }
    if (!codes[0] && !codes[1] && !codes[2] && !codes[3]) {
        return "";
    }
    return InputCommon::GenerateAnalogParamFromKeys(codes[0], codes[1], codes[2], codes[3],
                                                    codes[4], 0.5f);
}

} // namespace

void Register() {
    ::Input::RegisterFactory<::Input::ButtonDevice>(
        ANY, std::make_shared<AnyFactory<::Input::ButtonDevice, AnyButton>>());
    ::Input::RegisterFactory<::Input::AnalogDevice>(
        ANY, std::make_shared<AnyFactory<::Input::AnalogDevice, AnyAnalog>>());
    ::Input::RegisterFactory<::Input::AnalogDevice>(MOUSE, std::make_shared<MouseStickFactory>());
}

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
        LOG_WARNING(Frontend, "no key called \"{}\"", name);
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
    for (const std::string& part : Common::SplitString(spec, '|')) {
        std::string name = Common::StripSpaces(part);
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
    auto& profile = Settings::values.current_input_profile;
    Controller pad;
    const bool have_pad = config.controller_auto && FirstController(pad);
    Common::ParamPackage pad_base;
    if (have_pad) {
        pad_base = Common::ParamPackage{{"engine", "sdl"}, {"guid", pad.guid},
                                        {"port", std::to_string(pad.port)}, {"api", "controller"}};
        controller_description = fmt::format("controller: {} (port {})", pad.name, pad.port);
    } else {
        controller_description = config.controller_auto ? "no controller found" : "controller off";
    }
    for (int i = 0; i < NativeButton::NumButtons; ++i) {
        if (!config.button_params[i].empty()) {
            profile.buttons[i] = Join({config.button_params[i]});
            continue;
        }
        std::string from_pad;
        // the Guide button opens the app's menu (ui/): it isn't the 3DS's HOME button here
        if (have_pad && i != NativeButton::Home) {
            const auto bind = InputCommon::GetControllerButtonBinds(pad_base, i);
            if (bind.Has("engine")) {
                from_pad = bind.Serialize();
            }
        }
        std::vector<std::string> parts = KeyParams(config.keys[i]);
        // the item bar's macro presses L and Y or A through codes of its own
        parts.push_back(InputCommon::GenerateKeyboardParam(MACRO_KEY + i));
        parts.push_back(from_pad);
        profile.buttons[i] = Join(parts);
    }
    for (int i = 0; i < NativeAnalog::NumAnalogs; ++i) {
        if (!config.stick_params[i].empty()) {
            profile.analogs[i] = Join({config.stick_params[i]});
            continue;
        }
        std::string from_pad;
        if (have_pad) {
            const auto bind = InputCommon::GetControllerAnalogBinds(pad_base, i);
            if (bind.Has("engine")) {
                from_pad = bind.Serialize();
            }
        }
        std::vector<std::string> parts{StickKeysParam(config.stick_keys[i]), from_pad};
        if (i == NativeAnalog::CStick) {
            // the mouse turns the camera (it reads centred while it isn't the camera)
            parts.push_back(Common::ParamPackage{{"engine", MOUSE}}.Serialize());
        }
        profile.analogs[i] = Join(parts);
    }
    // the mouse is the stylus (the window sends it as touches); no motion sensor
    profile.touch_device = "engine:emu_window";
    profile.motion_device = "engine:motion_emu,update_period:100,sensitivity:0.01,tilt_clamp:90.0";
}

std::string ControllerDescription() {
    return controller_description;
}

void SetMuted(bool on) {
    muted.store(on, std::memory_order_relaxed);
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
    if (Port::Hooks::CameraTurnActive()) {
        // the game's camera turns by the mouse itself (port/src/hooks.cpp): degrees a count
        const float degrees = static_cast<float>(std::clamp(config.mouse_sensitivity, 1, 100)) * 0.0024f;
        const float vertical = static_cast<float>(std::clamp(config.mouse_vertical, 10, 200)) / 100.0f;
        Port::Hooks::AddCameraTurn(static_cast<float>(dx) * degrees * (config.mouse_invert_x ? -1.0f : 1.0f),
                                   static_cast<float>(-dy) * degrees * vertical * (config.mouse_invert_y ? -1.0f : 1.0f));
        return;
    }
    mouse.Motion(dx, dy);
}

void MouseReset() {
    Port::Hooks::ClearCameraTurn();
    mouse.Reset();
    mouse_stick_x.store(0.0f, std::memory_order_relaxed);
    mouse_stick_y.store(0.0f, std::memory_order_relaxed);
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
    if (Port::Hooks::CameraTurnActive()) {
        mouse.Reset();  // the camera takes the mouse itself: no push on the C-stick
    }
    const auto [x, y] = mouse.Tick(dt, settings);
    mouse_stick_x.store(x, std::memory_order_relaxed);
    mouse_stick_y.store(y, std::memory_order_relaxed);
    Apply(pulses.Tick());
    Apply(item_scroll.Tick());
}

} // namespace App::Input
