#include "input/input_mapper.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>
#include <utility>

#include <fmt/format.h>

#include "input/azahar_rules.h"
#include "input.h"
#include "kbm.h"
#include "util/buttons.h"
#include "util/log.h"
#include "util/strings.h"

namespace mh4n::app {

namespace {

namespace rules = azahar;

/// The key codes there are: SDL's scancodes (below 512), the mouse's buttons and the wheel
/// (App::Input::MOUSE_KEY, WHEEL_*), and the item bar's macro codes (MACRO_KEY + a button).
constexpr int kCodeCount = App::Input::MACRO_KEY + App::Button::Count;
constexpr std::size_t kPadButtonCount = 64;

/// Window.cpp's EchoOfController: Steam Input's desktop layouts send a key within a few
/// milliseconds of the controller's own event for it.
constexpr double kEchoWindowS = 0.150;

/// The 3DS buttons an InputState has: the PAD word's bits, and ZL and ZR on their own.
constexpr int kStateButtons[] = {App::Button::A,    App::Button::B,     App::Button::X,    App::Button::Y,
                                 App::Button::Up,   App::Button::Down,  App::Button::Left, App::Button::Right,
                                 App::Button::L,    App::Button::R,     App::Button::Start,
                                 App::Button::Select, App::Button::ZL,  App::Button::ZR};

u32 PadBit(int button) {
    switch (button) {
    case App::Button::A:
        return kPadA;
    case App::Button::B:
        return kPadB;
    case App::Button::X:
        return kPadX;
    case App::Button::Y:
        return kPadY;
    case App::Button::Up:
        return kPadUp;
    case App::Button::Down:
        return kPadDown;
    case App::Button::Left:
        return kPadLeft;
    case App::Button::Right:
        return kPadRight;
    case App::Button::L:
        return kPadL;
    case App::Button::R:
        return kPadR;
    case App::Button::Start:
        return kPadStart;
    case App::Button::Select:
        return kPadSelect;
    default:
        return 0;  // ZL and ZR are fields of their own
    }
}

/// One controller: what the layer says of it and what its events made of it.
struct Pad {
    PadInfo info;
    std::bitset<kPadButtonCount> buttons;
    std::array<int, rules::PadAxisId::Count> axes{};
};

/// Which controllers a binding reads, as Azahar's SDL engine picks them: by GUID and port.
struct Match {
    bool exact = false;  ///< the default binds: this controller (the picked one) alone
    std::int32_t id = 0;
    std::string guid;    ///< empty or "0": every controller
    int port = -1;       ///< with a GUID and a port that exists, that controller alone
    bool use_guid = true;
};

/// A way for a button to be pressed: a key, a controller's button, or one of its axes.
struct Source {
    enum class Kind { Key, PadButton, PadAxis };
    Kind kind = Kind::Key;
    int code = 0;  ///< Key: the code held
    Match match;   ///< the controllers a pad source reads
    int index = 0;           ///< PadButton: the button; PadAxis: the axis
    float threshold = 0.5f;  ///< PadAxis
    bool greater = true;     ///< PadAxis: pressed above the threshold, else below it
};

struct ButtonBinding {
    std::vector<Source> sources;
    bool held_over = false;  ///< let go once the mute ends: a button held when it ended stays released
};

struct StickSource {
    enum class Kind { Keys, Pad, Mouse };
    Kind kind = Kind::Keys;
    int codes[5] = {};  ///< up, down, left, right and the key that halves the push
    Match match;
    int axis_x = 0, axis_y = 1;
    float deadzone = 0.0f;
};

struct StickBinding {
    std::vector<StickSource> sources;
    bool held_over = false;
};

bool Contains(const std::vector<int>& codes, int code) {
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

}  // namespace

struct InputMapper::Impl {
    explicit Impl(InputMapper& owner_, IGamepadLayer* layer_) : owner(owner_), layer(layer_) {}

    InputMapper& owner;
    IGamepadLayer* layer;
    App::Config config;
    bool configured = false;

    // controllers
    std::vector<Pad> pads;
    std::optional<std::int32_t> picked;  ///< the controller the default binds read

    // keys
    std::bitset<kCodeCount> held;
    std::vector<int> stylus_keys, touch_keys, item_prev_keys, item_next_keys;
    int fullscreen_key = 0, fps_counter_key = 0, key_guides_key = 0;

    // bindings
    std::array<ButtonBinding, App::Button::Count> buttons;
    std::array<StickBinding, App::Analog::Count> sticks;
    std::set<std::string> logged_once;

    // the mode and the mouse
    bool last_used_keyboard = false;
    bool camera = false;  ///< the mouse is the camera
    bool focused = true;
    bool muted = false;
    bool camera_turn_active = false;
    double now = 0.0;
    std::optional<double> last_pad_event;
    std::optional<double> last_tick;
    App::Kbm::MouseCamera mouse;
    std::pair<float, float> mouse_push{0.0f, 0.0f};
    App::Kbm::ItemScroll item_scroll{App::Input::MACRO_KEY + App::Button::L, App::Input::MACRO_KEY + App::Button::Y,
                                     App::Input::MACRO_KEY + App::Button::A};
    App::Kbm::Pulses pulses;

    mh4n::InputState state;

    // ---- keys ----

    static bool ValidCode(int code) {
        return code > 0 && code < kCodeCount;
    }
    bool Held(int code) const {
        return ValidCode(code) && held[static_cast<std::size_t>(code)];
    }
    void Press(int code) {
        if (ValidCode(code)) {
            held.set(static_cast<std::size_t>(code));
        }
    }
    void Release(int code) {
        if (ValidCode(code)) {
            held.reset(static_cast<std::size_t>(code));
        }
    }
    void Apply(const App::Kbm::KeyEvents& events) {
        for (const auto& [code, pressed] : events) {
            if (pressed) {
                Press(code);
            } else {
                Release(code);
            }
        }
    }

    // ---- controllers ----

    void LogOnce(const std::string& message) {
        if (logged_once.insert(message).second) {
            APP_LOG_WARNING("{}", message);
        }
    }

    /// Asks the layer which controllers there are; the ones that stay keep their state.
    void RefreshPads() {
        std::vector<Pad> fresh;
        const std::vector<PadInfo> infos = layer ? layer->List() : std::vector<PadInfo>{};
        for (const PadInfo& info : infos) {
            const auto old = std::find_if(pads.begin(), pads.end(), [&](const Pad& p) { return p.info.id == info.id; });
            Pad pad = old != pads.end() ? *old : Pad{};
            pad.info = info;
            fresh.push_back(std::move(pad));
        }
        pads = std::move(fresh);
    }

    Pad* FindPad(std::int32_t id) {
        const auto it = std::find_if(pads.begin(), pads.end(), [&](const Pad& p) { return p.info.id == id; });
        return it == pads.end() ? nullptr : &*it;
    }

    /// The first controller (controller = auto), and the controller line input.cpp writes.
    void PickPad() {
        picked.reset();
        std::string line;
        if (config.controller_auto && !pads.empty()) {
            const PadInfo& info = pads.front().info;
            picked = info.id;
            line = fmt::format("controller: {} (port {})", info.name.empty() ? "controller" : info.name, info.port);
        } else {
            line = config.controller_auto ? "no controller found" : "controller off";
        }
        APP_LOG_INFO("{}", line);
    }

    /// The controllers a match reads, and whether it is one named by its port (Azahar's SDL
    /// engine then reads that one alone; else any of them presses a button, the furthest stick wins).
    std::vector<const Pad*> Resolve(const Match& match, bool& single) {
        std::vector<const Pad*> found;
        single = false;
        if (match.exact) {
            if (const Pad* pad = FindPad(match.id)) {
                found.push_back(pad);
            }
            single = true;
            return found;
        }
        const bool all = !match.use_guid || match.guid.empty() || match.guid == "0";
        for (const Pad& pad : pads) {
            if (all || pad.info.guid == match.guid) {
                found.push_back(&pad);
            }
        }
        if (match.use_guid && match.port >= 0 && static_cast<std::size_t>(match.port) < found.size()) {
            found = {found[static_cast<std::size_t>(match.port)]};
            single = true;
        }
        return found;
    }

    // ---- bindings ----

    Match ParamMatch(const rules::ParamPackage& p, const char* default_guid) const {
        Match match;
        const std::string maptype = p.Get("maptype", "guid+port");
        match.use_guid = maptype != "all";  // the controller API: "all" ignores the GUID
        match.guid = match.use_guid ? p.Get("guid", default_guid) : "";
        match.port = maptype == "guid+port" ? p.GetInt("port", -1) : -1;
        return match;
    }

    /// The sources of a button's parameter string ("engine:sdl,..." or "engine:keyboard,..."): what
    /// Azahar's profile holds when button_params replaces keys and controller altogether.
    void AddButtonParams(ButtonBinding& binding, const std::string& text) {
        const rules::ParamPackage p(text);
        const std::string engine = p.Get("engine", "");
        Source source;
        if (engine == "keyboard") {
            source.kind = Source::Kind::Key;
            source.code = p.GetInt("code", 0);
            binding.sources.push_back(source);
            return;
        }
        if (engine != "sdl") {
            LogOnce(fmt::format("button parameters with engine \"{}\" are not read here, ignored: {}", engine, text));
            return;
        }
        source.match = ParamMatch(p, "");
        const bool controller = p.Get("api", "joystick") == "controller";
        if (p.Has("hat")) {
            // a hat of the device: the directions of its first hat are the controller's D-pad
            const std::string direction = p.Get("direction", "");
            int button = -1;
            if (direction == "up") {
                button = rules::PadButtonId::DpadUp;
            } else if (direction == "down") {
                button = rules::PadButtonId::DpadDown;
            } else if (direction == "left") {
                button = rules::PadButtonId::DpadLeft;
            } else if (direction == "right") {
                button = rules::PadButtonId::DpadRight;
            }
            if (p.GetInt("hat", 0) != 0 || button < 0) {
                LogOnce(fmt::format("button parameters with a hat other than the D-pad's are ignored: {}", text));
                return;
            }
            source.kind = Source::Kind::PadButton;
            source.index = button;
        } else if (!controller) {
            LogOnce(fmt::format("button parameters for a raw joystick (api other than controller) are ignored: {}",
                                text));
            return;
        } else if (p.Has("axis")) {
            source.kind = Source::Kind::PadAxis;
            source.index = p.GetInt("axis", 0);
            source.threshold = p.GetFloat("threshold", 0.5f);
            source.greater = p.Get("direction", "") != "-";  // Azahar logs an error for any other and reads "+"
        } else {
            source.kind = Source::Kind::PadButton;
            source.index = p.GetInt("button", 0);
        }
        binding.sources.push_back(source);
    }

    void AddStickParams(StickBinding& binding, const std::string& text) {
        const rules::ParamPackage p(text);
        const std::string engine = p.Get("engine", "");
        if (engine != "sdl") {
            LogOnce(fmt::format("stick parameters with engine \"{}\" are not read here, ignored: {}", engine, text));
            return;
        }
        if (p.Get("api", "joystick") != "controller") {
            LogOnce(fmt::format("stick parameters for a raw joystick (api other than controller) are ignored: {}",
                                text));
            return;
        }
        StickSource source;
        source.kind = StickSource::Kind::Pad;
        source.match = ParamMatch(p, "0");
        source.axis_x = p.GetInt("axis_x", 0);
        source.axis_y = p.GetInt("axis_y", 1);
        source.deadzone = p.GetFloat("deadzone", 0.0f);
        binding.sources.push_back(source);
    }

    /// A stick's four keys and the one that halves it ("W,S,A,D,Left Shift"): app's StickKeysParam.
    static bool StickKeyCodes(const std::string& spec, int (&codes)[5]) {
        const auto parts = App::Util::SplitString(spec, ',');
        if (parts.size() < 4) {
            return false;
        }
        for (std::size_t i = 0; i < 5; ++i) {
            codes[i] = i < parts.size() ? App::Input::KeyCode(App::Util::StripSpaces(parts[i])) : 0;
        }
        return codes[0] || codes[1] || codes[2] || codes[3];
    }

    /// The profile of azahar_input.cpp's BuildInputProfile, as sources: params replace everything
    /// else; otherwise the keys, the item bar's macro code and the controller's bind.
    void BuildBindings() {
        for (const int i : kStateButtons) {
            ButtonBinding binding;
            binding.held_over = buttons[static_cast<std::size_t>(i)].held_over;
            const auto idx = static_cast<std::size_t>(i);
            if (!config.button_params[idx].empty()) {
                AddButtonParams(binding, config.button_params[idx]);
            } else {
                for (const int code : App::Input::KeyCodes(config.keys[idx])) {
                    Source key;
                    key.code = code;
                    binding.sources.push_back(key);
                }
                Source macro;
                macro.code = App::Input::MACRO_KEY + i;
                binding.sources.push_back(macro);
                // (the Guide button, Azahar's HOME, opens the app's menu: it is never a bind here)
                const rules::PadBind bind = rules::DefaultButtonBind(i);
                if (picked && bind.kind != rules::PadBind::Kind::None) {
                    Source pad;
                    pad.kind = bind.kind == rules::PadBind::Kind::Button ? Source::Kind::PadButton
                                                                         : Source::Kind::PadAxis;
                    pad.match.exact = true;
                    pad.match.id = *picked;
                    pad.index = bind.index;
                    pad.threshold = bind.threshold;
                    pad.greater = bind.greater;
                    binding.sources.push_back(pad);
                }
            }
            buttons[idx] = std::move(binding);
        }
        for (int i = 0; i < App::Analog::Count; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            StickBinding binding;
            binding.held_over = sticks[idx].held_over;
            if (!config.stick_params[idx].empty()) {
                AddStickParams(binding, config.stick_params[idx]);
            } else {
                StickSource keys;
                if (StickKeyCodes(config.stick_keys[idx], keys.codes)) {
                    keys.kind = StickSource::Kind::Keys;
                    binding.sources.push_back(keys);
                }
                const rules::StickAxes axes = rules::DefaultStickAxes(i);
                if (picked && axes.x >= 0) {
                    StickSource pad;
                    pad.kind = StickSource::Kind::Pad;
                    pad.match.exact = true;
                    pad.match.id = *picked;
                    pad.axis_x = axes.x;
                    pad.axis_y = axes.y;
                    binding.sources.push_back(pad);
                }
                if (i == App::Analog::CStick) {
                    StickSource mouse_source;
                    mouse_source.kind = StickSource::Kind::Mouse;  // the mouse turns the camera
                    binding.sources.push_back(mouse_source);
                }
            }
            sticks[idx] = std::move(binding);
        }
    }

    // ---- what the game reads ----

    bool Pressed(const Source& source) {
        if (source.kind == Source::Kind::Key) {
            return Held(source.code);
        }
        bool single = false;
        for (const Pad* pad : Resolve(source.match, single)) {
            if (source.kind == Source::Kind::PadButton) {
                if (source.index >= 0 && static_cast<std::size_t>(source.index) < kPadButtonCount &&
                    pad->buttons[static_cast<std::size_t>(source.index)]) {
                    return true;
                }
            } else if (source.index >= 0 && source.index < rules::PadAxisId::Count) {
                const float value = rules::AxisValue(pad->axes[static_cast<std::size_t>(source.index)]);
                if (source.greater ? value > source.threshold : value < source.threshold) {
                    return true;
                }
            }
        }
        return false;
    }

    std::pair<float, float> Push(const StickSource& source) {
        switch (source.kind) {
        case StickSource::Kind::Keys:
            return rules::KeyStick(Held(source.codes[0]), Held(source.codes[1]), Held(source.codes[2]),
                                   Held(source.codes[3]), Held(source.codes[4]));
        case StickSource::Kind::Mouse:
            return mouse_push;
        case StickSource::Kind::Pad: {
            bool single = false;
            std::vector<std::pair<int, int>> raws;
            for (const Pad* pad : Resolve(source.match, single)) {
                const auto axis = [&](int index) {
                    return index >= 0 && index < rules::PadAxisId::Count ? pad->axes[static_cast<std::size_t>(index)]
                                                                         : 0;
                };
                raws.emplace_back(axis(source.axis_x), axis(source.axis_y));
            }
            return rules::SdlAnalog(raws, single, source.deadzone);
        }
        }
        return {0.0f, 0.0f};
    }

    /// azahar_input.cpp's AnyButton and AnyAnalog over the sources of each binding.
    mh4n::InputState Evaluate() {
        mh4n::InputState out;
        for (const int i : kStateButtons) {
            ButtonBinding& binding = buttons[static_cast<std::size_t>(i)];
            bool pressed = false;
            for (const Source& source : binding.sources) {
                pressed = pressed || Pressed(source);
            }
            if (muted) {
                binding.held_over = true;
                pressed = false;
            } else if (binding.held_over) {
                binding.held_over = pressed;
                pressed = false;
            }
            if (!pressed) {
                continue;
            }
            if (i == App::Button::ZL) {
                out.zl = true;
            } else if (i == App::Button::ZR) {
                out.zr = true;
            } else {
                out.buttons |= PadBit(i);
            }
        }
        const auto length = [](const std::pair<float, float>& s) { return std::hypot(s.first, s.second); };
        for (int i = 0; i < App::Analog::Count; ++i) {
            StickBinding& binding = sticks[static_cast<std::size_t>(i)];
            std::pair<float, float> status{0.0f, 0.0f};
            for (const StickSource& source : binding.sources) {
                const std::pair<float, float> s = Push(source);
                if (length(s) > length(status)) {
                    status = s;
                }
            }
            if (muted) {
                binding.held_over = true;
                status = {0.0f, 0.0f};
            } else if (binding.held_over) {
                binding.held_over = length(status) > 0.25f;
                status = {0.0f, 0.0f};
            }
            const s16 x = rules::CirclePadRaw(status.first);
            const s16 y = rules::CirclePadRaw(status.second);
            if (i == App::Analog::CirclePad) {
                out.circle_x = x;
                out.circle_y = y;
            } else {
                out.cstick_x = x;
                out.cstick_y = y;
            }
        }
        return out;
    }

    /// Reads the bindings again; tells on_state when the result differs.
    void Recompute() {
        const mh4n::InputState fresh = Evaluate();
        if (fresh == state) {
            return;
        }
        state = fresh;
        if (owner.on_state) {
            owner.on_state(state);
        }
    }

    // ---- the keyboard and mouse mode (port/app/window.cpp) ----

    bool KeyboardMouseActive() const {
        const App::Input::Mode mode = App::Input::ParseMode(config.input_mode);
        return mode == App::Input::Mode::KeyboardMouse || (mode == App::Input::Mode::Auto && last_used_keyboard);
    }

    bool EchoOfController() const {
        return last_pad_event && now - *last_pad_event < kEchoWindowS;
    }

    bool IsHotkey(int code, unsigned mods) const {
        const bool alt_enter = code == App::Scancode::Return && (mods & App::Mod::Alt) != 0;
        return alt_enter || (code != 0 && (code == fullscreen_key || code == fps_counter_key || code == key_guides_key ||
                                           Contains(touch_keys, code)));
    }

    /// window.cpp's UpdateMouseMode: the camera only while the game has the mouse, not under the
    /// menu, not without the focus, not while the stylus key is held.
    void UpdateCamera() {
        bool stylus = false;
        for (const int code : stylus_keys) {
            stylus = stylus || Held(code);
        }
        const bool want = KeyboardMouseActive() && focused && !stylus && !muted;
        if (want == camera) {
            return;
        }
        camera = want;
        if (!want) {
            // the mouse's buttons let go as it stops being the camera, and the stick centred
            for (const int button : {App::MouseButton::Left, App::MouseButton::Middle, App::MouseButton::Right,
                                     App::MouseButton::X1, App::MouseButton::X2}) {
                Release(App::Input::MOUSE_KEY + button);
            }
            mouse.Reset();
            mouse_push = {0.0f, 0.0f};
            Recompute();
        }
    }

    /// input.cpp's MouseMotion (reimplemented here on the loop's clock and without Runtime(): keep in
    /// step with port/app/input.cpp).
    void MouseMotion(int dx, int dy) {
        if (camera_turn_active) {
            // the game's camera turns by the mouse itself: degrees a count
            const float degrees = static_cast<float>(std::clamp(config.mouse_sensitivity, 1, 100)) * 0.0024f;
            const float vertical = static_cast<float>(std::clamp(config.mouse_vertical, 10, 200)) / 100.0f;
            if (owner.on_camera_turn) {
                owner.on_camera_turn(
                    static_cast<float>(dx) * degrees * (config.mouse_invert_x ? -1.0f : 1.0f),
                    static_cast<float>(-dy) * degrees * vertical * (config.mouse_invert_y ? -1.0f : 1.0f));
            }
            return;
        }
        mouse.Motion(dx, dy);
    }

    /// input.cpp's Wheel: the item bar's scroll, or a key pressed for a moment (keep in step with
    /// port/app/input.cpp).
    void Wheel(int code) {
        if (Contains(item_prev_keys, code)) {
            item_scroll.Queue(-1);
        } else if (Contains(item_next_keys, code)) {
            item_scroll.Queue(1);
        } else {
            Apply(pulses.Start(code));
        }
    }

    bool OnEvent(const App::InputEvent& e) {
        using Type = App::InputEvent::Type;
        switch (e.type) {
        case Type::PadButton: {
            // a controller's button: what Steam Input may send as a key with it isn't the keyboard's
            if (e.down) {
                last_pad_event = now;
                last_used_keyboard = false;
            }
            if (Pad* pad = FindPad(e.pad); pad && e.button >= 0 && static_cast<std::size_t>(e.button) < kPadButtonCount) {
                pad->buttons.set(static_cast<std::size_t>(e.button), e.down);
            }
            Recompute();
            return true;
        }
        case Type::PadAxis: {
            if (std::abs(e.value) > 8000) {
                // (a stick too: Steam Input's desktop layouts send keys for the sticks as well)
                last_pad_event = now;
                if (std::abs(e.value) > 16000) {
                    last_used_keyboard = false;
                }
            }
            if (Pad* pad = FindPad(e.pad); pad && e.axis >= 0 && e.axis < rules::PadAxisId::Count) {
                pad->axes[static_cast<std::size_t>(e.axis)] = e.value;
            }
            Recompute();
            return true;
        }
        case Type::PadAdded:
        case Type::PadRemoved:
            // the first controller again, and what the player is told of it
            RefreshPads();
            PickPad();
            BuildBindings();
            Recompute();
            return true;
        case Type::KeyDown:
        case Type::KeyUp: {
            const bool down = e.type == Type::KeyDown;
            const bool hotkey = IsHotkey(e.scancode, e.mods);
            if (e.repeat) {
                return !hotkey;
            }
            if (down) {
                focused = true;
                // (the app's own keys don't count: F11 doesn't make a controller player's mouse the camera)
                if (!EchoOfController() && e.scancode != App::Scancode::Escape && e.scancode != fullscreen_key &&
                    e.scancode != fps_counter_key && e.scancode != key_guides_key &&
                    !Contains(stylus_keys, e.scancode)) {
                    last_used_keyboard = true;
                }
            }
            if (hotkey && down) {
                return false;  // the shell's: fullscreen, the frame counter, the quest map, the key guides
            }
            if (down) {
                Press(e.scancode);
            } else {
                Release(e.scancode);  // (a key let go is never left down, whatever it is now)
            }
            Recompute();
            return !hotkey;
        }
        case Type::MouseMotion:
            if (!camera) {
                return false;
            }
            if (!e.from_touch) {
                MouseMotion(e.dx, e.dy);
            }
            return true;
        case Type::MouseButton:
            if (e.down) {
                focused = true;
                if (camera) {
                    last_used_keyboard = true;
                }
            }
            if (!camera) {
                return false;
            }
            // the mouse's buttons are keys like any other (Mouse Left, ...)
            if (!e.from_touch) {
                const int code = App::Input::MOUSE_KEY + e.button;
                if (e.down) {
                    Press(code);
                } else {
                    Release(code);
                }
                Recompute();
            }
            return true;
        case Type::Wheel:
            if (!camera) {
                return false;
            }
            if (!e.from_touch) {
                if (e.wheel_y != 0) {
                    Wheel(e.wheel_y > 0 ? App::Input::WHEEL_UP : App::Input::WHEEL_DOWN);
                }
                if (e.wheel_x != 0) {
                    Wheel(e.wheel_x > 0 ? App::Input::WHEEL_RIGHT : App::Input::WHEEL_LEFT);
                }
                Recompute();
            }
            return true;
        case Type::FocusLost:
            // keys held while switching away would stay down
            focused = false;
            held.reset();
            Recompute();
            return true;
        default:
            return false;
        }
    }

    /// input.cpp's Tick, on the loop's clock (keep in step with port/app/input.cpp).
    void Tick(double now_s) {
        now = now_s;
        UpdateCamera();
        const double dt = last_tick ? now_s - *last_tick : 1.0 / 60.0;
        last_tick = now_s;
        App::Kbm::CameraSettings settings;
        settings.sensitivity = config.mouse_sensitivity;
        settings.vertical = config.mouse_vertical;
        settings.invert_y = config.mouse_invert_y;
        settings.deadzone = config.mouse_deadzone;
        if (camera_turn_active) {
            mouse.Reset();  // the camera takes the mouse itself: no push on the C-stick
        }
        mouse_push = mouse.Tick(dt, settings);
        Apply(pulses.Tick());
        Apply(item_scroll.Tick());
        Recompute();
    }

    void Configure(const App::Config& next) {
        config = next;
        stylus_keys = App::Input::KeyCodes(config.stylus_key);
        touch_keys = App::Input::KeyCodes(config.touch_screen_key);
        item_prev_keys = App::Input::KeyCodes(config.item_prev);
        item_next_keys = App::Input::KeyCodes(config.item_next);
        fullscreen_key = App::Input::KeyCode(config.fullscreen_key);
        fps_counter_key = App::Input::KeyCode(config.fps_counter_key);
        key_guides_key = App::Input::KeyCode(config.key_guides_key);
        RefreshPads();
        if (!configured) {
            // auto starts with the controller's way when there is a controller (a Steam Deck's, say)
            const App::Input::Mode mode = App::Input::ParseMode(config.input_mode);
            last_used_keyboard =
                mode == App::Input::Mode::KeyboardMouse || (mode == App::Input::Mode::Auto && pads.empty());
            configured = true;
        }
        PickPad();
        BuildBindings();
        Recompute();
    }
};

InputMapper::InputMapper(IGamepadLayer* pads) : impl_(std::make_unique<Impl>(*this, pads)) {}

InputMapper::~InputMapper() = default;

void InputMapper::Configure(const App::Config& config) {
    impl_->Configure(config);
}

bool InputMapper::OnEvent(const App::InputEvent& event) {
    return impl_->OnEvent(event);
}

void InputMapper::Tick(double now_s) {
    impl_->Tick(now_s);
}

mh4n::InputState InputMapper::State() const {
    return impl_->state;
}

void InputMapper::SetMuted(bool muted) {
    impl_->muted = muted;
    impl_->Recompute();
    impl_->UpdateCamera();
}

bool InputMapper::MouseIsStylus() const {
    return !impl_->camera;
}

bool InputMapper::KeyboardMouseActive() const {
    return impl_->KeyboardMouseActive();
}

bool InputMapper::WantsRelativeMouse() const {
    return impl_->camera;
}

void InputMapper::SetCameraTurnActive(bool active) {
    if (active && !impl_->camera_turn_active) {
        impl_->mouse.Reset();
        impl_->mouse_push = {0.0f, 0.0f};
        impl_->Recompute();
    }
    impl_->camera_turn_active = active;
}

}  // namespace mh4n::app
