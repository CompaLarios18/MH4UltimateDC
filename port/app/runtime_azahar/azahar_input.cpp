// AzaharRuntime: the 3DS's buttons and sticks as Azahar numbers and names them, the input profile
// it reads (and the engine that joins a key and a controller on one button), the keys the window
// sends, and what a controller does for an input of the profile.
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>

#include <SDL.h>
#include <fmt/format.h>

#include "common/param_package.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "config.h"
#include "core/core.h"
#include "core/frontend/input.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/ir/ir_rst.h"
#include "core/hle/service/ir/ir_user.h"
#include "core/hle/service/sm/sm.h"
#include "input.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
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

namespace NativeAnalog = Settings::NativeAnalog;
namespace NativeButton = Settings::NativeButton;

constexpr const char* ANY = "mh4u_any";

/// SetInputMuted: the in-game menu is up, and the game gets nothing.
std::atomic<bool> muted{false};

/// Pressed while either of two buttons is. Muted, it reads released, and a button held when the
/// mute ends stays released until it is let go (the B that closed the menu doesn't also roll).
class AnyButton final : public ::Input::ButtonDevice {
public:
    AnyButton(std::unique_ptr<::Input::ButtonDevice> a_, std::unique_ptr<::Input::ButtonDevice> b_)
        : a(std::move(a_)), b(std::move(b_)) {}
    bool GetStatus() const override {
        const bool pressed = (a && a->GetStatus()) || (b && b->GetStatus());
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
    std::unique_ptr<::Input::ButtonDevice> a, b;
    mutable bool held_over = false;  // the emulation thread is the only reader
};

/// Whichever of two sticks is pushed further. Muted, it reads centred, and a stick pushed when
/// the mute ends reads centred until it has come back.
class AnyAnalog final : public ::Input::AnalogDevice {
public:
    AnyAnalog(std::unique_ptr<::Input::AnalogDevice> a_, std::unique_ptr<::Input::AnalogDevice> b_)
        : a(std::move(a_)), b(std::move(b_)) {}
    std::tuple<float, float> GetStatus() const override {
        const auto sa = a ? a->GetStatus() : std::tuple<float, float>{};
        const auto sb = b ? b->GetStatus() : std::tuple<float, float>{};
        const auto length = [](const std::tuple<float, float>& s) {
            return std::hypot(std::get<0>(s), std::get<1>(s));
        };
        const auto status = length(sa) >= length(sb) ? sa : sb;
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
    std::unique_ptr<::Input::AnalogDevice> a, b;
    mutable bool held_over = false;
};

/// "engine:mh4u_any,a:<params>,b:<params>": the inner parameters are serialized again, which
/// escapes them (Common::ParamPackage).
template <typename Device, typename Any>
class AnyFactory final : public ::Input::Factory<Device> {
public:
    std::unique_ptr<Device> Create(const Common::ParamPackage& params) override {
        const auto inner = [&](const char* key) -> std::unique_ptr<Device> {
            const std::string p = params.Get(key, "");
            return p.empty() ? nullptr : ::Input::CreateDevice<Device>(p);
        };
        return std::make_unique<Any>(inner("a"), inner("b"));
    }
};

/// Every binding goes through "mh4u_any", even a single one, so that SetInputMuted reaches it.
std::string Join(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) {
        return "";
    }
    Common::ParamPackage p{{"engine", ANY}};
    if (!a.empty()) {
        p.Set("a", a);
    }
    if (!b.empty()) {
        p.Set("b", b);
    }
    return p.Serialize();
}

std::string KeyParam(const std::string& name) {
    const int code = App::Input::KeyCode(name);
    return code ? InputCommon::GenerateKeyboardParam(code) : "";
}

std::string StickKeysParam(const std::string& spec) {
    const auto parts = Common::SplitString(spec, ',');
    if (parts.size() < 4) {
        return "";
    }
    int codes[5] = {};
    for (std::size_t i = 0; i < 5 && i < parts.size(); ++i) {
        codes[i] = App::Input::KeyCode(Common::StripSpaces(parts[i]));
    }
    if (!codes[0] && !codes[1] && !codes[2] && !codes[3]) {
        return "";
    }
    return InputCommon::GenerateAnalogParamFromKeys(codes[0], codes[1], codes[2], codes[3],
                                                    codes[4], 0.5f);
}

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

void AzaharRuntime::InitInput() {
    InputCommon::Init();
    ::Input::RegisterFactory<::Input::ButtonDevice>(
        ANY, std::make_shared<AnyFactory<::Input::ButtonDevice, AnyButton>>());
    ::Input::RegisterFactory<::Input::AnalogDevice>(
        ANY, std::make_shared<AnyFactory<::Input::AnalogDevice, AnyAnalog>>());
}

void AzaharRuntime::ShutdownInput() {
    InputCommon::Shutdown();
}

void AzaharRuntime::BuildInputProfile(const Config& config, const ControllerInfo* controller) {
    auto& profile = Settings::values.current_input_profile;
    Common::ParamPackage pad_base;
    if (controller) {
        pad_base = Common::ParamPackage{{"engine", "sdl"}, {"guid", controller->guid},
                                        {"port", std::to_string(controller->port)},
                                        {"api", "controller"}};
    }
    for (int i = 0; i < NativeButton::NumButtons; ++i) {
        if (!config.button_params[i].empty()) {
            profile.buttons[i] = Join(config.button_params[i], "");
            continue;
        }
        std::string from_pad;
        // the Guide button opens the app's menu (ui/): it isn't the 3DS's HOME button here
        if (controller && i != NativeButton::Home) {
            const auto bind = InputCommon::GetControllerButtonBinds(pad_base, i);
            if (bind.Has("engine")) {
                from_pad = bind.Serialize();
            }
        }
        profile.buttons[i] = Join(KeyParam(config.keys[i]), from_pad);
    }
    for (int i = 0; i < NativeAnalog::NumAnalogs; ++i) {
        if (!config.stick_params[i].empty()) {
            profile.analogs[i] = Join(config.stick_params[i], "");
            continue;
        }
        std::string from_pad;
        if (controller) {
            const auto bind = InputCommon::GetControllerAnalogBinds(pad_base, i);
            if (bind.Has("engine")) {
                from_pad = bind.Serialize();
            }
        }
        profile.analogs[i] = Join(StickKeysParam(config.stick_keys[i]), from_pad);
    }
    // the mouse is the stylus (the window sends it as touches); no motion sensor
    profile.touch_device = "engine:emu_window";
    profile.motion_device = "engine:motion_emu,update_period:100,sensitivity:0.01,tilt_clamp:90.0";
}

void AzaharRuntime::ReloadInputDevices() {
    auto& system = Core::System::GetInstance();
    if (const auto hid = Service::HID::GetModule(system)) {
        hid->ReloadInputDevices();
    }
    if (const auto apt = Service::APT::GetModule(system)) {
        apt->GetAppletManager()->ReloadInputDevices();
    }
    auto& sm = system.ServiceManager();
    if (const auto ir_user = sm.GetService<Service::IR::IR_USER>("ir:USER")) {
        ir_user->ReloadInputDevices();
    }
    if (const auto ir_rst = sm.GetService<Service::IR::IR_RST>("ir:rst")) {
        ir_rst->ReloadInputDevices();
    }
}

void AzaharRuntime::SetInputMuted(bool on) {
    muted.store(on, std::memory_order_relaxed);
}

void AzaharRuntime::KeyDown(int scancode) {
    InputCommon::GetKeyboard()->PressKey(scancode);
}

void AzaharRuntime::KeyUp(int scancode) {
    InputCommon::GetKeyboard()->ReleaseKey(scancode);
}

void AzaharRuntime::ReleaseAllKeys() {
    InputCommon::GetKeyboard()->ReleaseAllKeys();
}

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
