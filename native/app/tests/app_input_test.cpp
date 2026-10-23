// Derived from Azahar src/input_common/sdl/sdl_impl.cpp, src/input_common/sdl/sdl_joystick.cpp,
// src/input_common/analog_from_button.cpp, src/common/param_package.cpp and
// src/core/hle/service/hid/hid.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// all five say "Licensed under GPLv2 or any later version"): the default controller binds
// (xinput_to_3ds_mapping and the trigger and stick branches of SDLState::GetSDLControllerButtonBind
// and GetSDLControllerAnalogBindByGUID) restated in the table below, the circle pad's limit and
// rounding (MAX_CIRCLEPAD_POS), the SDL devices' value arithmetic, the keyboard stick's diagonal
// and modifier, and the parameter strings the tests feed to button_params and stick_params.
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (sdl_impl.cpp)
// Copyright 2026 Citra Emulator Project / Azahar Emulator Project (sdl_joystick.cpp)
// Copyright Citra Emulator Project / Lime3DS Emulator Project (analog_from_button.cpp)
// Copyright 2017 Citra Emulator Project (param_package.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.cpp)
//
// N3-07: the input of mh4u-native (native/app/input, docs/native/input.md) on synthetic
// App::InputEvents and a fake gamepad layer: the default pad binds against the table restated
// here (and, when MH4N_AZAHAR_DIR names a checkout of Azahar's sources, against Azahar's own
// tables), keys and pad together, the halving key, button_params and stick_params, the mute and
// what is held over it, the modes, the stylus key, the wheel over Ticks, the mouse as the camera,
// the raw range, and controllers that come and go. Synthetic data only.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "app_window.h"
#include "config.h"
#include "input.h"
#include "input/azahar_rules.h"
#include "input/input_mapper.h"
#include "util/buttons.h"
#include "util/tests/fake_runtime.h"

namespace {

using mh4n::InputState;
using mh4n::app::IGamepadLayer;
using mh4n::app::InputMapper;
using mh4n::app::PadInfo;
namespace az = mh4n::app::azahar;

// SDL3's gamepad numbers are the ones InputEvent carries (SDL2's): the module's table relies on it
static_assert(SDL_GAMEPAD_BUTTON_SOUTH == az::PadButtonId::South && SDL_GAMEPAD_BUTTON_EAST == az::PadButtonId::East &&
              SDL_GAMEPAD_BUTTON_WEST == az::PadButtonId::West && SDL_GAMEPAD_BUTTON_NORTH == az::PadButtonId::North &&
              SDL_GAMEPAD_BUTTON_BACK == az::PadButtonId::Back && SDL_GAMEPAD_BUTTON_GUIDE == az::PadButtonId::Guide &&
              SDL_GAMEPAD_BUTTON_START == az::PadButtonId::Start &&
              SDL_GAMEPAD_BUTTON_LEFT_STICK == az::PadButtonId::LeftStick &&
              SDL_GAMEPAD_BUTTON_RIGHT_STICK == az::PadButtonId::RightStick &&
              SDL_GAMEPAD_BUTTON_LEFT_SHOULDER == az::PadButtonId::LeftShoulder &&
              SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER == az::PadButtonId::RightShoulder &&
              SDL_GAMEPAD_BUTTON_DPAD_UP == az::PadButtonId::DpadUp &&
              SDL_GAMEPAD_BUTTON_DPAD_DOWN == az::PadButtonId::DpadDown &&
              SDL_GAMEPAD_BUTTON_DPAD_LEFT == az::PadButtonId::DpadLeft &&
              SDL_GAMEPAD_BUTTON_DPAD_RIGHT == az::PadButtonId::DpadRight);
static_assert(SDL_GAMEPAD_AXIS_LEFTX == az::PadAxisId::LeftX && SDL_GAMEPAD_AXIS_LEFTY == az::PadAxisId::LeftY &&
              SDL_GAMEPAD_AXIS_RIGHTX == az::PadAxisId::RightX && SDL_GAMEPAD_AXIS_RIGHTY == az::PadAxisId::RightY &&
              SDL_GAMEPAD_AXIS_LEFT_TRIGGER == az::PadAxisId::TriggerLeft &&
              SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == az::PadAxisId::TriggerRight);

constexpr std::int32_t kPad1 = 10, kPad2 = 11, kPad3 = 12;

/// The controllers a test plugs in and out.
struct FakePads final : IGamepadLayer {
    std::vector<PadInfo> pads;
    std::vector<PadInfo> List() override {
        return pads;
    }
};

PadInfo Pad(std::int32_t id, const char* guid, int port, const char* name) {
    PadInfo info;
    info.id = id;
    info.guid = guid;
    info.port = port;
    info.name = name;
    return info;
}

int Key(const char* name) {
    const int code = App::Input::KeyCode(name);
    REQUIRE_MESSAGE(code != 0, "no key called " << name);
    return code;
}

/// The mapper under test, with the fake controllers, the log and what its callbacks said.
struct Rig {
    fake::Env env;
    FakePads fake_pads;
    App::Config config = App::Defaults();
    std::unique_ptr<InputMapper> mapper;
    std::vector<InputState> states;
    std::vector<std::pair<float, float>> turns;
    double now = 1.0;

    Rig(std::vector<PadInfo> pads = {}, void (*tweak)(App::Config&) = nullptr) {
        fake_pads.pads = std::move(pads);
        if (tweak) {
            tweak(config);
        }
        Start();
    }
    void Start() {
        mapper = std::make_unique<InputMapper>(&fake_pads);
        mapper->on_state = [this](const InputState& s) { states.push_back(s); };
        mapper->on_camera_turn = [this](float yaw, float pitch) { turns.emplace_back(yaw, pitch); };
        mapper->Configure(config);
    }

    InputState State() const {
        return mapper->State();
    }
    mh4n::u32 Buttons() const {
        return mapper->State().buttons;
    }
    void Tick(double dt = 1.0 / 60.0) {
        now += dt;
        mapper->Tick(now);
    }
    void Ticks(int n) {
        for (int i = 0; i < n; ++i) {
            Tick();
        }
    }

    bool Down(int scancode, unsigned mods = 0, bool repeat = false) {
        App::InputEvent e;
        e.type = App::InputEvent::Type::KeyDown;
        e.scancode = scancode;
        e.mods = mods;
        e.repeat = repeat;
        return mapper->OnEvent(e);
    }
    bool Up(int scancode) {
        App::InputEvent e;
        e.type = App::InputEvent::Type::KeyUp;
        e.scancode = scancode;
        return mapper->OnEvent(e);
    }
    bool PadButton(std::int32_t pad, int button, bool down) {
        App::InputEvent e;
        e.type = App::InputEvent::Type::PadButton;
        e.pad = pad;
        e.button = button;
        e.down = down;
        return mapper->OnEvent(e);
    }
    bool PadAxis(std::int32_t pad, int axis, int value) {
        App::InputEvent e;
        e.type = App::InputEvent::Type::PadAxis;
        e.pad = pad;
        e.axis = axis;
        e.value = value;
        return mapper->OnEvent(e);
    }
    bool PadEvent(App::InputEvent::Type type, std::int32_t pad) {
        App::InputEvent e;
        e.type = type;
        e.pad = pad;
        return mapper->OnEvent(e);
    }
    bool Motion(int dx, int dy, bool from_touch = false) {
        App::InputEvent e;
        e.type = App::InputEvent::Type::MouseMotion;
        e.dx = dx;
        e.dy = dy;
        e.from_touch = from_touch;
        return mapper->OnEvent(e);
    }
    bool Mouse(int button, bool down, bool from_touch = false) {
        App::InputEvent e;
        e.type = App::InputEvent::Type::MouseButton;
        e.button = button;
        e.down = down;
        e.from_touch = from_touch;
        return mapper->OnEvent(e);
    }
    bool Wheel(int x, int y) {
        App::InputEvent e;
        e.type = App::InputEvent::Type::Wheel;
        e.wheel_x = x;
        e.wheel_y = y;
        return mapper->OnEvent(e);
    }
    bool Other(App::InputEvent::Type type) {
        App::InputEvent e;
        e.type = type;
        return mapper->OnEvent(e);
    }
};

std::vector<PadInfo> OnePad() {
    return {Pad(kPad1, "03000000deadbeef0000000000000001", 0, "Pad A")};
}

// ── Azahar's tables, restated (docs/native/input.md has the provenance) ──

// Azahar's xinput_to_3ds_mapping (sdl_impl.cpp), in the order of Settings::NativeButton (which the
// app's App::Button repeats): the SDL button each 3DS button reads on a controller of the Xbox
// layout. -1 is SDL_CONTROLLER_BUTTON_INVALID. SDL2's numbers: A 0, B 1, X 2, Y 3, BACK 4, GUIDE 5,
// START 6, LEFTSHOULDER 9, RIGHTSHOULDER 10, DPAD_UP 11, DPAD_DOWN 12, DPAD_LEFT 13, DPAD_RIGHT 14.
constexpr int kXinputTo3ds[18] = {
    1,   // A: SDL_CONTROLLER_BUTTON_B
    0,   // B: SDL_CONTROLLER_BUTTON_A
    3,   // X: SDL_CONTROLLER_BUTTON_Y
    2,   // Y: SDL_CONTROLLER_BUTTON_X
    11,  // Up
    12,  // Down
    13,  // Left
    14,  // Right
    9,   // L: SDL_CONTROLLER_BUTTON_LEFTSHOULDER
    10,  // R: SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
    6,   // Start
    4,   // Select: SDL_CONTROLLER_BUTTON_BACK
    -1,  // Debug
    -1,  // Gpio14
    -1,  // ZL (the trigger axis below)
    -1,  // ZR
    5,   // Home: SDL_CONTROLLER_BUTTON_GUIDE (the app leaves it to its menu)
    -1,  // Power
};
// GetSDLControllerButtonBind: ZL is axis SDL_CONTROLLER_AXIS_TRIGGERLEFT (4), ZR TRIGGERRIGHT (5),
// direction "+", threshold 0.5. GetSDLControllerAnalogBindByGUID: the circle pad is axes LEFTX (0) and
// LEFTY (1), the C-stick RIGHTX (2) and RIGHTY (3).
constexpr int kZlAxis = 4, kZrAxis = 5;
// hid.cpp: MAX_CIRCLEPAD_POS = 0x9A, "rounded rather than truncated"
constexpr int kCirclePadMax = 0x9A;

mh4n::u32 BitOf(int button) {
    using namespace mh4n;
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
        return 0;
    }
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST_CASE("every default pad bind: the 3DS button each controller button presses") {
    for (int button = 0; button < 12; ++button) {
        CAPTURE(button);
        Rig rig(OnePad());
        const int sdl_button = kXinputTo3ds[button];
        REQUIRE(sdl_button >= 0);
        CHECK(rig.PadButton(kPad1, sdl_button, true));
        CHECK(rig.Buttons() == BitOf(button));
        CHECK_FALSE(rig.State().zl);
        CHECK_FALSE(rig.State().zr);
        CHECK(rig.PadButton(kPad1, sdl_button, false));
        CHECK(rig.State() == InputState{});
    }
}

TEST_CASE("the triggers are ZL and ZR past half way, and nothing else is pressed") {
    Rig rig(OnePad());
    // 16383 / 32767 = 0.49998 (below 0.5), 16384 / 32767 = 0.50002 (above)
    rig.PadAxis(kPad1, kZlAxis, 16383);
    CHECK_FALSE(rig.State().zl);
    rig.PadAxis(kPad1, kZlAxis, 16384);
    CHECK(rig.State().zl);
    CHECK_FALSE(rig.State().zr);
    CHECK(rig.Buttons() == 0);
    rig.PadAxis(kPad1, kZlAxis, 0);
    rig.PadAxis(kPad1, kZrAxis, 32767);
    CHECK(rig.State().zr);
    CHECK_FALSE(rig.State().zl);
    rig.PadAxis(kPad1, kZrAxis, 0);
    CHECK(rig.State() == InputState{});
}

TEST_CASE("the buttons with no bind and the Guide button press nothing") {
    Rig rig(OnePad());
    for (const int button : {az::PadButtonId::Guide, az::PadButtonId::LeftStick, az::PadButtonId::RightStick, 15, 16,
                             17, 18, 19, 20}) {
        CAPTURE(button);
        rig.PadButton(kPad1, button, true);
        CHECK(rig.State() == InputState{});
        rig.PadButton(kPad1, button, false);
    }
    // (a button number outside what the mapper keeps is not an error either)
    rig.PadButton(kPad1, 500, true);
    rig.PadAxis(kPad1, 500, 32767);
    CHECK(rig.State() == InputState{});
}

TEST_CASE("the default bind table is Azahar's: the module's, the restated one and Azahar's own text") {
    // the module against the restated table
    for (int button = 0; button < App::Button::Count; ++button) {
        CAPTURE(button);
        const az::PadBind bind = az::DefaultButtonBind(button);
        if (button == App::Button::ZL || button == App::Button::ZR) {
            CHECK(bind.kind == az::PadBind::Kind::Axis);
            CHECK(bind.index == (button == App::Button::ZL ? kZlAxis : kZrAxis));
            CHECK(bind.threshold == 0.5f);
            CHECK(bind.greater);
        } else if (kXinputTo3ds[button] >= 0) {
            CHECK(bind.kind == az::PadBind::Kind::Button);
            CHECK(bind.index == kXinputTo3ds[button]);
        } else {
            CHECK(bind.kind == az::PadBind::Kind::None);
        }
    }
    CHECK(az::DefaultStickAxes(App::Analog::CirclePad).x == 0);
    CHECK(az::DefaultStickAxes(App::Analog::CirclePad).y == 1);
    CHECK(az::DefaultStickAxes(App::Analog::CStick).x == 2);
    CHECK(az::DefaultStickAxes(App::Analog::CStick).y == 3);
    CHECK(az::kMaxCirclePadPosition == kCirclePadMax);

    // the restated table against Azahar's text, when its sources are at hand (the pinned commit in
    // port/azahar/COMMIT; ~/azahar in the cloud)
    const char* dir = std::getenv("MH4N_AZAHAR_DIR");
    if (!dir || !*dir) {
        MESSAGE("MH4N_AZAHAR_DIR is not set: the restated table is not compared with Azahar's sources");
        return;
    }
    const std::string root = std::string(dir) + "/src/";
    const std::string sdl_impl = ReadFile(root + "input_common/sdl/sdl_impl.cpp");
    const std::string hid = ReadFile(root + "core/hle/service/hid/hid.cpp");
    const std::string joystick = ReadFile(root + "input_common/sdl/sdl_joystick.cpp");
    const std::string analog_from_button = ReadFile(root + "input_common/analog_from_button.cpp");
    REQUIRE_MESSAGE(!sdl_impl.empty(), "MH4N_AZAHAR_DIR has no src/input_common/sdl/sdl_impl.cpp");
    REQUIRE(!hid.empty());
    REQUIRE(!joystick.empty());
    REQUIRE(!analog_from_button.empty());

    // xinput_to_3ds_mapping = {{ ... }}
    const std::size_t table = sdl_impl.find("xinput_to_3ds_mapping");
    REQUIRE(table != std::string::npos);
    const std::size_t open = sdl_impl.find("{{", table);
    const std::size_t close = sdl_impl.find("}}", open);
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    const std::string body = sdl_impl.substr(open, close - open);
    const std::regex name_re(R"(SDL_CONTROLLER_BUTTON_([A-Z_0-9]+))");
    const std::vector<std::pair<std::string, int>> sdl2 = {
        {"A", 0},          {"B", 1},          {"X", 2},          {"Y", 3},         {"BACK", 4},
        {"GUIDE", 5},      {"START", 6},      {"LEFTSTICK", 7},  {"RIGHTSTICK", 8}, {"LEFTSHOULDER", 9},
        {"RIGHTSHOULDER", 10}, {"DPAD_UP", 11}, {"DPAD_DOWN", 12}, {"DPAD_LEFT", 13}, {"DPAD_RIGHT", 14},
        {"INVALID", -1}};
    std::vector<int> azahar_table;
    for (std::sregex_iterator it(body.begin(), body.end(), name_re), end; it != end; ++it) {
        const std::string name = (*it)[1];
        const auto found = std::find_if(sdl2.begin(), sdl2.end(), [&](const auto& p) { return p.first == name; });
        REQUIRE_MESSAGE(found != sdl2.end(), "an SDL button the test does not know: " << name);
        azahar_table.push_back(found->second);
    }
    REQUIRE(azahar_table.size() == 18);
    for (int i = 0; i < 18; ++i) {
        CAPTURE(i);
        CHECK(azahar_table[static_cast<std::size_t>(i)] == kXinputTo3ds[i]);
    }
    // the triggers and the sticks, the threshold and the direction, as GetSDLControllerButtonBind and
    // GetSDLControllerAnalogBindByGUID write them
    CHECK(std::regex_search(sdl_impl, std::regex(R"(Values::ZL\)\s*\{\s*params\.Set\("axis", SDL_CONTROLLER_AXIS_TRIGGERLEFT\))")));
    CHECK(std::regex_search(sdl_impl, std::regex(R"(Values::ZR\)\s*\{\s*params\.Set\("axis", SDL_CONTROLLER_AXIS_TRIGGERRIGHT\))")));
    CHECK(std::regex_search(sdl_impl, std::regex(R"(params\.Set\("direction", "\+"\);\s*params\.Set\("threshold", 0\.5f\);)")));
    CHECK(std::regex_search(sdl_impl, std::regex(R"(CirclePad\) \{\s*button_bind_x = SDL_CONTROLLER_AXIS_LEFTX;\s*button_bind_y = SDL_CONTROLLER_AXIS_LEFTY;)")));
    CHECK(std::regex_search(sdl_impl, std::regex(R"(CStick\) \{\s*button_bind_x = SDL_CONTROLLER_AXIS_RIGHTX;\s*button_bind_y = SDL_CONTROLLER_AXIS_RIGHTY;)")));
    // the numbers of SDL's own enumerations, which SDL2 and SDL3 share
    CHECK(az::PadAxisId::TriggerLeft == 4);
    CHECK(az::PadAxisId::TriggerRight == 5);
    // hid.cpp: the limit and the rounding
    CHECK(std::regex_search(hid, std::regex(R"(MAX_CIRCLEPAD_POS = 0x9A;)")));
    CHECK(hid.find("std::roundf(circle_pad_x_f * MAX_CIRCLEPAD_POS)") != std::string::npos);
    CHECK(hid.find("std::roundf(circle_pad_y_f * MAX_CIRCLEPAD_POS)") != std::string::npos);
    // sdl_joystick.cpp: an axis is the raw value over 32767, and y is turned over for the 3DS
    CHECK(joystick.find("32767.0f") != std::string::npos);
    CHECK(joystick.find("y = -y;") != std::string::npos);
    // analog_from_button.cpp: the diagonal and the modifier's default
    CHECK(analog_from_button.find("SQRT_HALF = 0.707106781f") != std::string::npos);
    CHECK(analog_from_button.find("params.Get(\"modifier_scale\", 0.5f)") != std::string::npos);
}

TEST_CASE("keys and the pad work together on one button") {
    Rig rig(OnePad());
    const int f = Key("F");
    rig.Down(f);
    CHECK(rig.Buttons() == mh4n::kPadA);
    rig.PadButton(kPad1, az::PadButtonId::East, true);  // the 3DS's A on the pad
    CHECK(rig.Buttons() == mh4n::kPadA);
    rig.Up(f);
    CHECK(rig.Buttons() == mh4n::kPadA);  // the pad still holds it
    rig.PadButton(kPad1, az::PadButtonId::East, false);
    CHECK(rig.Buttons() == 0);
    // a button of the keyboard and another of the pad at once
    rig.Down(Key("Space"));  // B
    rig.PadButton(kPad1, az::PadButtonId::DpadUp, true);
    CHECK(rig.Buttons() == (mh4n::kPadB | mh4n::kPadUp));
    // the on_state callback was told of every change and of nothing else
    CHECK(rig.states.size() == 4);
    for (std::size_t i = 1; i < rig.states.size(); ++i) {
        CHECK_FALSE(rig.states[i] == rig.states[i - 1]);
    }
}

TEST_CASE("a key bound with several alternatives, and the mouse's buttons only while it is the camera") {
    Rig rig;  // no controller: the keyboard and mouse's way
    rig.Tick();
    REQUIRE(rig.mapper->WantsRelativeMouse());
    rig.Mouse(App::MouseButton::Right, true);  // "F | Mouse Right": the 3DS's A
    CHECK(rig.Buttons() == mh4n::kPadA);
    rig.Down(Key("F"));
    rig.Mouse(App::MouseButton::Right, false);
    CHECK(rig.Buttons() == mh4n::kPadA);
    rig.Up(Key("F"));
    CHECK(rig.Buttons() == 0);
    // "Left Ctrl | Mouse Middle": L, and "Left Shift | Mouse 4": R
    rig.Mouse(App::MouseButton::Middle, true);
    rig.Mouse(App::MouseButton::X1, true);
    CHECK(rig.Buttons() == (mh4n::kPadL | mh4n::kPadR));
}

TEST_CASE("the circle pad from keys, with the halving key, and the left stick") {
    Rig rig(OnePad(), [](App::Config& c) { c.stick_keys[App::Analog::CirclePad] = "W,S,A,D,Left Shift"; });
    const int w = Key("W"), a = Key("A"), s = Key("S"), d = Key("D"), shift = Key("Left Shift");
    rig.Down(w);
    CHECK(rig.State().circle_x == 0);
    CHECK(rig.State().circle_y == kCirclePadMax);
    rig.Down(shift);  // halves it (and is the 3DS's R as well)
    CHECK(rig.State().circle_y == 77);
    CHECK((rig.Buttons() & mh4n::kPadR) != 0);
    rig.Up(shift);
    rig.Down(d);  // a diagonal is shortened to the unit circle: 0.7071 * 154 = 108.9
    CHECK(rig.State().circle_x == 109);
    CHECK(rig.State().circle_y == 109);
    rig.Down(shift);  // and halved: 0.3536 * 154 = 54.4
    CHECK(rig.State().circle_x == 54);
    CHECK(rig.State().circle_y == 54);
    rig.Up(shift);
    rig.Up(w);
    rig.Up(d);
    rig.Down(a);
    CHECK(rig.State().circle_x == -kCirclePadMax);
    rig.Up(a);
    rig.Down(s);
    CHECK(rig.State().circle_y == -kCirclePadMax);
    // both keys of one axis cancel
    rig.Down(w);
    CHECK(rig.State().circle_y == 0);
    rig.Up(w);
    rig.Up(s);
    CHECK(rig.State() == InputState{});
}

TEST_CASE("without a fifth key the shift does not halve the stick") {
    Rig rig(OnePad());  // the default "W,S,A,D"
    rig.Down(Key("W"));
    rig.Down(Key("Left Shift"));
    CHECK(rig.State().circle_y == kCirclePadMax);
}

TEST_CASE("the C-stick from its keys and the circle pad from both: the furthest push wins") {
    Rig rig(OnePad());
    // O, L, K and ; are the C-stick's up, down, left and right
    const int semicolon = Key(";");
    rig.Down(Key("O"));
    rig.Down(semicolon);
    CHECK(rig.State().cstick_x == 109);
    CHECK(rig.State().cstick_y == 109);
    CHECK(rig.State().circle_x == 0);
    rig.Up(Key("O"));
    rig.Up(semicolon);
    // the key W (full) against the left stick pushed down half way: the key is further
    rig.Down(Key("W"));
    rig.PadAxis(kPad1, az::PadAxisId::LeftY, 16384);
    CHECK(rig.State().circle_y == kCirclePadMax);
    // the stick pushed further than the key: the stick
    rig.Up(Key("W"));
    rig.Down(Key("S"));  // down, full
    rig.PadAxis(kPad1, az::PadAxisId::LeftY, -32768);  // up, full
    CHECK(rig.State().circle_y == -kCirclePadMax);     // equal pushes: the keys (listed first) stay
    rig.Up(Key("S"));
    CHECK(rig.State().circle_y == kCirclePadMax);      // the stick alone
}

TEST_CASE("the raw range of the sticks: the HID scale, rounded, at full and half pushes") {
    Rig rig(OnePad());
    const auto circle = [&](int x, int y) {
        rig.PadAxis(kPad1, az::PadAxisId::LeftX, x);
        rig.PadAxis(kPad1, az::PadAxisId::LeftY, y);
        return std::make_pair(static_cast<int>(rig.State().circle_x), static_cast<int>(rig.State().circle_y));
    };
    // SDL's y grows downwards, the 3DS's upwards
    CHECK(circle(32767, 0) == std::make_pair(154, 0));
    CHECK(circle(-32768, 0) == std::make_pair(-154, 0));  // -32768 / 32767 is past 1: cut to the circle
    CHECK(circle(0, 32767) == std::make_pair(0, -154));
    CHECK(circle(0, -32768) == std::make_pair(0, 154));
    CHECK(circle(16384, 0) == std::make_pair(77, 0));  // 0.50002 * 154 = 77.002
    CHECK(circle(-16384, 0) == std::make_pair(-77, 0));
    CHECK(circle(16383, 0) == std::make_pair(77, 0));  // 0.49998 * 154 = 76.998: rounded, not truncated
    CHECK(circle(0, -16384) == std::make_pair(0, 77));
    // rounding is to the nearest and not truncated: 0.9 * 154 = 138.6 -> 139 (29490 / 32767 = 0.89998)
    CHECK(circle(29490, 0) == std::make_pair(139, 0));
    // both axes at full: the point is cut back onto the unit circle, 0.7071 * 154 = 108.9
    CHECK(circle(32767, -32767) == std::make_pair(109, 109));
    CHECK(circle(0, 0) == std::make_pair(0, 0));
    // the right stick is the C-stick, the same scale
    rig.PadAxis(kPad1, az::PadAxisId::RightX, 32767);
    CHECK(rig.State().cstick_x == 154);
    rig.PadAxis(kPad1, az::PadAxisId::RightX, 0);
    rig.PadAxis(kPad1, az::PadAxisId::RightY, 16384);
    CHECK(rig.State().cstick_y == -77);
    // both at once are cut back to the unit circle first: (1, 0.5) / 1.118 -> (0.894, 0.447) * 154
    rig.PadAxis(kPad1, az::PadAxisId::RightX, 32767);
    CHECK(rig.State().cstick_x == 138);
    CHECK(rig.State().cstick_y == -69);
    rig.PadAxis(kPad1, az::PadAxisId::RightX, 0);
    rig.PadAxis(kPad1, az::PadAxisId::RightY, 0);
    // a push no larger than a hundredth of the range is a few units: the scale has no dead zone
    rig.PadAxis(kPad1, az::PadAxisId::RightX, 1000);
    rig.PadAxis(kPad1, az::PadAxisId::RightY, 0);
    CHECK(rig.State().cstick_x == 5);  // 0.0305 * 154 = 4.7
    // the module's own function agrees
    CHECK(az::CirclePadRaw(1.0f) == 154);
    CHECK(az::CirclePadRaw(-1.0f) == -154);
    CHECK(az::CirclePadRaw(0.5f) == 77);
    CHECK(az::CirclePadRaw(-0.5f) == -77);
    CHECK(az::CirclePadRaw(0.003f) == 0);
}

TEST_CASE("button_params: Azahar's sdl and keyboard parameters replace the keys and the pad") {
    Rig rig(OnePad(), [](App::Config& c) {
        const std::string g = "03000000deadbeef0000000000000001";
        // A: the controller's button 0 (SDL_CONTROLLER_BUTTON_A)
        c.button_params[App::Button::A] = "engine:sdl,guid:" + g + ",port:0,api:controller,button:0";
        // B: the left trigger past 0.5
        c.button_params[App::Button::B] = "engine:sdl,guid:" + g + ",port:0,api:controller,axis:4,direction:+,threshold:0.5";
        // X: the left stick's x below -0.5 (a negative threshold, direction "-")
        c.button_params[App::Button::X] = "engine:sdl,guid:" + g + ",port:0,api:controller,axis:0,direction:-,threshold:-0.5";
        // Y: the D-pad as a hat
        c.button_params[App::Button::Y] = "engine:sdl,guid:" + g + ",port:0,hat:0,direction:up";
        // L: a key (SDL scancode of J)
        c.button_params[App::Button::L] = "engine:keyboard,code:" + std::to_string(App::Input::KeyCode("J"));
    });
    // (the keys and the pad's default binds of these buttons are gone)
    rig.Down(Key("F"));
    rig.PadButton(kPad1, az::PadButtonId::East, true);
    CHECK(rig.Buttons() == 0);
    rig.Up(Key("F"));
    rig.PadButton(kPad1, az::PadButtonId::East, false);

    rig.PadButton(kPad1, az::PadButtonId::South, true);
    CHECK(rig.Buttons() == mh4n::kPadA);
    rig.PadButton(kPad1, az::PadButtonId::South, false);

    rig.PadAxis(kPad1, az::PadAxisId::TriggerLeft, 20000);
    CHECK(rig.Buttons() == mh4n::kPadB);
    CHECK(rig.State().zl);  // (ZL keeps its own default bind: the same trigger)
    rig.PadAxis(kPad1, az::PadAxisId::TriggerLeft, 0);

    rig.PadAxis(kPad1, az::PadAxisId::LeftX, -16000);  // -0.488: not below -0.5
    CHECK((rig.Buttons() & mh4n::kPadX) == 0);
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, -17000);  // -0.519
    CHECK((rig.Buttons() & mh4n::kPadX) != 0);
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 0);

    rig.PadButton(kPad1, az::PadButtonId::DpadUp, true);
    CHECK((rig.Buttons() & mh4n::kPadY) != 0);
    CHECK((rig.Buttons() & mh4n::kPadUp) != 0);  // Up keeps its own default bind
    rig.PadButton(kPad1, az::PadButtonId::DpadUp, false);

    rig.Down(Key("J"));
    CHECK(rig.Buttons() == mh4n::kPadL);
    rig.Up(Key("J"));
    CHECK(rig.Buttons() == 0);

    // the key of a button with parameters, and its macro code, no longer press it
    rig.Down(Key("Left Ctrl"));
    CHECK(rig.Buttons() == 0);
    // the other buttons keep keys and pad
    rig.Down(Key("Return"));
    CHECK(rig.Buttons() == mh4n::kPadStart);
}

TEST_CASE("button_params name a controller by GUID and port, any other engine is logged once and ignored") {
    Rig rig({Pad(kPad1, "aaaa", 0, "First"), Pad(kPad2, "bbbb", 0, "Second"), Pad(kPad3, "bbbb", 1, "Third")},
            [](App::Config& c) {
                c.button_params[App::Button::A] = "engine:sdl,guid:bbbb,port:1,api:controller,button:0";
                c.button_params[App::Button::B] = "engine:sdl,guid:bbbb,api:controller,button:0";
                c.button_params[App::Button::X] = "engine:gcpad,port:0,button:1";
                c.button_params[App::Button::Y] = "engine:sdl,guid:aaaa,port:0,api:joystick,button:1";
                c.button_params[App::Button::L] = "engine:sdl,maptype:all,api:controller,button:1";
                c.button_params[App::Button::R] = "not a parameter string";
            });
    // the default pad is the first controller; A reads port 1 of GUID bbbb: the third
    rig.PadButton(kPad2, 0, true);
    CHECK(rig.Buttons() == mh4n::kPadB);  // B: any controller of the GUID; A: not the second one
    rig.PadButton(kPad2, 0, false);
    rig.PadButton(kPad3, 0, true);
    CHECK(rig.Buttons() == (mh4n::kPadA | mh4n::kPadB));
    rig.PadButton(kPad3, 0, false);
    rig.PadButton(kPad1, 0, true);  // the first controller: neither A (bbbb) nor B (bbbb)
    CHECK(rig.Buttons() == 0);
    rig.PadButton(kPad1, 0, false);
    // maptype all: any controller
    rig.PadButton(kPad1, 1, true);
    CHECK(rig.Buttons() == mh4n::kPadL);
    rig.PadButton(kPad1, 1, false);
    // the gcpad and the raw joystick parameters and the line that is no parameter: never pressed
    rig.PadButton(kPad1, 1, true);
    rig.PadButton(kPad1, 2, true);
    CHECK((rig.Buttons() & (mh4n::kPadX | mh4n::kPadY | mh4n::kPadR)) == 0);
    // logged once, however often the settings are read
    const auto count = [&](const std::string& part) {
        std::scoped_lock lock{rig.env.log_mutex};
        return std::count_if(rig.env.logged.begin(), rig.env.logged.end(),
                             [&](const std::string& line) { return line.find(part) != std::string::npos; });
    };
    CHECK(count("engine \"gcpad\"") == 1);
    CHECK(count("raw joystick") == 1);
    CHECK(count("engine \"\"") == 1);  // the line that is no parameter string has no engine
    rig.mapper->Configure(rig.config);
    rig.mapper->Configure(rig.config);
    CHECK(count("engine \"gcpad\"") == 1);
    CHECK(count("raw joystick") == 1);
}

TEST_CASE("stick_params: an sdl stick with a dead zone replaces the keys and the pad; others are ignored") {
    Rig rig(OnePad(), [](App::Config& c) {
        c.stick_params[App::Analog::CStick] = "engine:sdl,guid:0,port:0,api:controller,axis_x:0,axis_y:1,deadzone:0.5";
        c.stick_params[App::Analog::CirclePad] = "engine:analog_from_button,up:engine$0keyboard$1code$04";
    });
    // the C-stick is the left stick through a dead zone of 0.5: half way is inside it
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 16384);
    CHECK(rig.State().cstick_x == 0);
    // three quarters: (0.75 - 0.5) / (1 - 0.5) = 0.5 of the way
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 24575);
    CHECK(rig.State().cstick_x == 77);
    // the right stick and the C-stick's keys no longer count
    rig.PadAxis(kPad1, az::PadAxisId::RightX, 32767);
    rig.Down(Key("O"));
    CHECK(rig.State().cstick_y == 0);
    // the circle pad had parameters of an engine that is not read: it reads centred, keys included
    rig.Down(Key("W"));
    rig.PadAxis(kPad1, az::PadAxisId::LeftY, 32767);
    CHECK(rig.State().circle_x == 0);
    CHECK(rig.State().circle_y == 0);
}

TEST_CASE("mute: nothing is pressed, and what is held when it ends stays released until let go") {
    Rig rig(OnePad());
    const int f = Key("F");
    rig.Down(f);                                                  // A
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 32767);               // the circle pad pushed
    rig.PadAxis(kPad1, az::PadAxisId::RightY, -32768);             // the C-stick pushed
    rig.PadAxis(kPad1, az::PadAxisId::TriggerRight, 32767);        // ZR
    REQUIRE(rig.Buttons() == mh4n::kPadA);
    REQUIRE(rig.State().circle_x == 154);
    REQUIRE(rig.State().zr);

    rig.mapper->SetMuted(true);
    CHECK(rig.State() == InputState{});
    // a key pressed while muted reads released too
    rig.Down(Key("Space"));
    CHECK(rig.State() == InputState{});

    rig.mapper->SetMuted(false);
    CHECK(rig.State() == InputState{});  // A, Space, the sticks and ZR are all still held: still nothing
    rig.Tick();
    CHECK(rig.State() == InputState{});

    // the stick comes back and is pushed again: now it reads
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 0);
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 32767);
    CHECK(rig.State().circle_x == 154);
    // the C-stick comes back only part way (a quarter of the range is where it counts as back)
    rig.PadAxis(kPad1, az::PadAxisId::RightY, -10000);
    CHECK(rig.State().cstick_y == 0);
    rig.PadAxis(kPad1, az::PadAxisId::RightY, -8000);  // 0.244: below a quarter
    CHECK(rig.State().cstick_y == 0);                  // (this read ended the hold over: the next one reads)
    rig.PadAxis(kPad1, az::PadAxisId::RightY, -32768);
    CHECK(rig.State().cstick_y == 154);
    // buttons: let go, then pressed again
    rig.Up(f);
    rig.Down(f);
    CHECK((rig.Buttons() & mh4n::kPadA) != 0);
    rig.PadAxis(kPad1, az::PadAxisId::TriggerRight, 0);
    CHECK_FALSE(rig.State().zr);
    rig.PadAxis(kPad1, az::PadAxisId::TriggerRight, 32767);
    CHECK(rig.State().zr);
    // Space was pressed while muted and is still down: it stays released until let go
    CHECK((rig.Buttons() & mh4n::kPadB) == 0);
    rig.Up(Key("Space"));
    rig.Down(Key("Space"));
    CHECK((rig.Buttons() & mh4n::kPadB) != 0);

    // muted twice and unmuted with nothing held: nothing is held over
    rig.mapper->SetMuted(true);
    rig.Up(f);
    rig.Up(Key("Space"));
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 0);
    rig.PadAxis(kPad1, az::PadAxisId::RightY, 0);
    rig.PadAxis(kPad1, az::PadAxisId::TriggerRight, 0);
    rig.mapper->SetMuted(false);
    rig.Down(f);
    CHECK(rig.Buttons() == mh4n::kPadA);
}

TEST_CASE("modes: auto follows the last device used, the pad against the keyboard") {
    Rig rig;  // no controller: auto starts with the keyboard and mouse
    rig.Tick();
    CHECK(rig.mapper->WantsRelativeMouse());
    CHECK_FALSE(rig.mapper->MouseIsStylus());

    // a controller comes and its button is pressed: the controller's way, the mouse the stylus again
    rig.fake_pads.pads = OnePad();
    CHECK(rig.PadEvent(App::InputEvent::Type::PadAdded, kPad1));
    rig.Tick();
    CHECK(rig.mapper->WantsRelativeMouse());  // (plugging one in changes nothing by itself)
    CHECK(rig.PadButton(kPad1, az::PadButtonId::South, true));
    rig.Tick();
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());
    CHECK(rig.mapper->MouseIsStylus());
    // the mouse's events are the shell's now: touches
    CHECK_FALSE(rig.Motion(5, 5));
    CHECK_FALSE(rig.Mouse(App::MouseButton::Left, true));
    CHECK_FALSE(rig.Wheel(0, 1));
    CHECK(rig.Buttons() == mh4n::kPadB);  // only the pad's button: the mouse press was no X
    rig.Tick();
    // moving the mouse alone does not change the mode (port/app/window.cpp: only keys and buttons do)
    for (int i = 0; i < 3; ++i) {
        rig.Motion(50, 0);
        rig.Tick();
    }
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());
    CHECK(rig.State().cstick_x == 0);

    // Steam Input's keys that come with a controller's event are not the keyboard's: within 150 ms
    rig.Down(Key("W"), 0);
    rig.Tick();
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());
    rig.Up(Key("W"));
    // a key later: the keyboard's way
    rig.Tick(0.2);
    rig.Down(Key("W"));
    rig.Tick();
    CHECK(rig.mapper->WantsRelativeMouse());
    rig.Up(Key("W"));

    // the app's own keys and the stylus key do not make the mouse the camera
    rig.PadButton(kPad1, az::PadButtonId::South, false);
    rig.PadButton(kPad1, az::PadButtonId::South, true);
    rig.Tick(0.2);
    REQUIRE_FALSE(rig.mapper->WantsRelativeMouse());
    for (const char* name : {"F11", "F3", "F1", "Left Alt", "Escape"}) {
        CAPTURE(name);
        rig.Down(Key(name));
        rig.Tick();
        CHECK_FALSE(rig.mapper->WantsRelativeMouse());
        rig.Up(Key(name));
    }

    // a stick: between 8000 and 16000 only marks the echo window, past 16000 it switches
    rig.Tick(0.2);
    rig.Down(Key("W"));
    rig.Up(Key("W"));
    rig.Tick();
    REQUIRE(rig.mapper->WantsRelativeMouse());
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 10000);  // between 8000 and 16000: marks, does not switch
    rig.Tick();
    CHECK(rig.mapper->WantsRelativeMouse());
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 7000);  // below 8000: nothing
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 16500);
    rig.Tick();
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());

    // the settings read again in the middle of play keep the mode the player is in
    rig.mapper->Configure(rig.config);
    rig.Tick();
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());
}

TEST_CASE("modes: auto starts with the controller's way when there is a controller") {
    Rig rig(OnePad());
    rig.Tick();
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());
    rig.Tick(0.2);
    rig.Down(Key("W"));
    rig.Tick();
    CHECK(rig.mapper->WantsRelativeMouse());
}

TEST_CASE("modes: controller never gives the camera, keyboard_mouse always") {
    {
        Rig rig({}, [](App::Config& c) { c.input_mode = "controller"; });
        rig.Down(Key("W"));
        rig.Mouse(App::MouseButton::Left, true);
        rig.Tick();
        CHECK_FALSE(rig.mapper->WantsRelativeMouse());
        CHECK(rig.mapper->MouseIsStylus());
        CHECK_FALSE(rig.Motion(10, 10));
    }
    {
        Rig rig(OnePad(), [](App::Config& c) { c.input_mode = "keyboard_mouse"; });
        rig.Tick();
        CHECK(rig.mapper->WantsRelativeMouse());
        rig.PadButton(kPad1, az::PadButtonId::South, true);  // (the controller still plays)
        rig.PadAxis(kPad1, az::PadAxisId::LeftX, 32767);
        rig.Tick();
        CHECK(rig.mapper->WantsRelativeMouse());
        CHECK(rig.Buttons() == mh4n::kPadB);
        CHECK(rig.State().circle_x == 154);
    }
}

TEST_CASE("the stylus key, the menu and the focus take the mouse from the camera") {
    Rig rig;
    rig.Tick();
    REQUIRE(rig.mapper->WantsRelativeMouse());
    rig.Mouse(App::MouseButton::Left, true);  // "Mouse Left | V": X
    REQUIRE(rig.Buttons() == mh4n::kPadX);
    const int alt = Key("Left Alt");
    rig.Down(alt);
    rig.Tick();
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());
    CHECK(rig.mapper->MouseIsStylus());
    CHECK(rig.Buttons() == 0);  // the mouse's buttons are let go as it stops being the camera
    CHECK_FALSE(rig.Motion(3, 3));
    CHECK_FALSE(rig.Mouse(App::MouseButton::Left, true));
    CHECK_FALSE(rig.Mouse(App::MouseButton::Left, false));
    CHECK(rig.Buttons() == 0);
    rig.Up(alt);
    rig.Tick();
    CHECK(rig.mapper->WantsRelativeMouse());
    CHECK(rig.Motion(3, 3));

    // the menu: the mouse is its own at once, and the camera comes back when it is gone
    rig.mapper->SetMuted(true);
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());
    rig.mapper->SetMuted(false);
    rig.Tick();
    CHECK(rig.mapper->WantsRelativeMouse());

    // the window loses the focus: keys let go, the mouse is free until the player acts in it again
    rig.Down(Key("W"));
    REQUIRE(rig.State().circle_y == kCirclePadMax);
    CHECK(rig.Other(App::InputEvent::Type::FocusLost));
    CHECK(rig.State() == InputState{});
    rig.Tick();
    CHECK_FALSE(rig.mapper->WantsRelativeMouse());
    CHECK_FALSE(rig.Motion(30, 0));
    rig.Tick();
    CHECK(rig.State().cstick_x == 0);
    rig.Down(Key("W"));
    rig.Tick();
    CHECK(rig.mapper->WantsRelativeMouse());
}

TEST_CASE("touch-screen mouse events are no mouse; the app's hotkeys are the shell's") {
    Rig rig;
    rig.Tick();
    REQUIRE(rig.mapper->WantsRelativeMouse());
    // events that come from a touch (SDL's touch mouse): not the mouse's buttons or motion
    CHECK(rig.Mouse(App::MouseButton::Left, true, true));
    CHECK(rig.Buttons() == 0);
    CHECK(rig.Motion(100, 0, true));
    rig.Tick();
    CHECK(rig.State().cstick_x == 0);

    // F11, F3, F1, M and F12 (touch_screen_key "F12 | M") and Alt+Enter are the shell's
    for (const char* name : {"F11", "F3", "F1", "M", "F12"}) {
        CAPTURE(name);
        CHECK_FALSE(rig.Down(Key(name)));
        CHECK_FALSE(rig.Up(Key(name)));
    }
    CHECK_FALSE(rig.Down(Key("Return"), App::Mod::Alt));
    CHECK(rig.Buttons() == 0);
    // Return alone is the 3DS's Start
    CHECK(rig.Down(Key("Return")));
    CHECK(rig.Buttons() == mh4n::kPadStart);
    rig.Up(Key("Return"));
    // other events are not input
    CHECK_FALSE(rig.Other(App::InputEvent::Type::Resized));
    CHECK_FALSE(rig.Other(App::InputEvent::Type::Touch));
    CHECK_FALSE(rig.Other(App::InputEvent::Type::Text));
    CHECK_FALSE(rig.Other(App::InputEvent::Type::Quit));
    CHECK_FALSE(rig.Other(App::InputEvent::Type::Other));
    // a key's repeat changes nothing
    rig.Down(Key("Space"));
    rig.Up(Key("Space"));
    CHECK(rig.Down(Key("Space"), 0, true));
    CHECK(rig.Buttons() == 0);
}

TEST_CASE("the wheel scrolls the item bar: L, then Y or A, one step at a time over Ticks") {
    using namespace mh4n;
    Rig rig;  // the keyboard and mouse's way
    rig.Tick();
    REQUIRE(rig.mapper->WantsRelativeMouse());
    CHECK(rig.Wheel(0, 1));  // Wheel Up is item_prev: L and Y
    std::vector<u32> seen;
    for (int i = 0; i < 12; ++i) {
        rig.Tick();
        seen.push_back(rig.Buttons());
    }
    const u32 L = kPadL, Y = kPadY, A = kPadA;
    // L alone for the 3 frames the game needs to see it, with Y for 3, L alone for 3, then let go
    CHECK(seen == std::vector<u32>{L, L, L, L | Y, L | Y, L | Y, L, L, L, 0, 0, 0});

    // Wheel Down (item_next): L and A
    rig.Wheel(0, -1);
    seen.clear();
    for (int i = 0; i < 10; ++i) {
        rig.Tick();
        seen.push_back(rig.Buttons());
    }
    CHECK(seen == std::vector<u32>{L, L, L, L | A, L | A, L | A, L, L, L, 0});

    // two notches in a row: L stays down between the two Y presses
    rig.Wheel(0, 1);
    rig.Wheel(0, 1);
    seen.clear();
    for (int i = 0; i < 16; ++i) {
        rig.Tick();
        seen.push_back(rig.Buttons());
    }
    CHECK(seen == std::vector<u32>{L, L, L, L | Y, L | Y, L | Y, L, L, L, L | Y, L | Y, L | Y, L, L, L, 0});

    // at most 5 wait: 7 notches are 5 scrolls
    for (int i = 0; i < 7; ++i) {
        rig.Wheel(0, 1);
    }
    int y_presses = 0;
    u32 previous = 0;
    for (int i = 0; i < 60; ++i) {
        rig.Tick();
        y_presses += ((rig.Buttons() & Y) && !(previous & Y)) ? 1 : 0;
        previous = rig.Buttons();
    }
    CHECK(y_presses == 5);
    CHECK(rig.Buttons() == 0);
}

TEST_CASE("a wheel bound to a button is pressed for a few frames; a wheel off the camera does nothing") {
    using namespace mh4n;
    Rig rig({}, [](App::Config& c) {
        c.keys[App::Button::Select] = "Wheel Right";
        c.item_prev = "Wheel Left";
        c.item_next = "";
    });
    rig.Tick();
    CHECK(rig.Wheel(1, 0));
    CHECK(rig.Buttons() == kPadSelect);  // pressed at once
    rig.Tick();
    rig.Tick();
    rig.Tick();
    CHECK(rig.Buttons() == kPadSelect);
    rig.Tick();  // the fourth frame lets it go
    CHECK(rig.Buttons() == 0);
    // another notch while it is held keeps it down longer
    rig.Wheel(1, 0);
    rig.Tick();
    rig.Tick();
    rig.Wheel(1, 0);
    rig.Tick();
    rig.Tick();
    rig.Tick();
    CHECK(rig.Buttons() == kPadSelect);
    rig.Tick();
    CHECK(rig.Buttons() == 0);
    // Wheel Left is the item bar's now: L, then Y
    rig.Wheel(-1, 0);
    rig.Tick();
    CHECK(rig.Buttons() == kPadL);
    rig.Ticks(20);
    CHECK(rig.Buttons() == 0);
    // an unbound wheel code does nothing (Wheel Down is neither item_next nor on a button)
    rig.Wheel(0, -1);
    rig.Ticks(5);
    CHECK(rig.Buttons() == 0);

    // the wheel is the camera's: with the controller's way the shell has it
    Rig pad_rig(OnePad());
    pad_rig.Tick();
    CHECK_FALSE(pad_rig.Wheel(0, 1));
    pad_rig.Ticks(5);
    CHECK(pad_rig.Buttons() == 0);
}

TEST_CASE("the mouse pushes the C-stick by how fast it moves, and lets go as it stops") {
    Rig rig;
    rig.Tick();
    REQUIRE(rig.mapper->WantsRelativeMouse());
    // a quick flick to the right
    CHECK(rig.Motion(60, 0));
    rig.Tick();
    CHECK(rig.State().cstick_x == 154);
    CHECK(rig.State().cstick_y == 0);
    CHECK(rig.State().circle_x == 0);
    // it fades once the mouse stops
    int ticks = 0;
    while (rig.State().cstick_x != 0 && ticks < 60) {
        rig.Tick();
        ++ticks;
    }
    CHECK(rig.State().cstick_x == 0);
    CHECK(ticks < 30);
    // the mouse up pushes the stick up; down, down
    rig.Motion(0, -60);
    rig.Tick();
    CHECK(rig.State().cstick_y == 154);
    rig.Ticks(40);
    rig.Motion(0, 60);
    rig.Tick();
    CHECK(rig.State().cstick_y == -154);
    rig.Ticks(40);
    CHECK(rig.State().cstick_y == 0);

    // a slow move pushes less, but past the game's dead zone (12 %): 0.12 * 154 = 18.5 and up
    rig.Motion(10, 0);
    rig.Tick();
    CHECK(rig.State().cstick_x > 18);
    CHECK(rig.State().cstick_x < 154);
    rig.Ticks(40);

    // the vertical setting and the inversion
    rig.config.mouse_invert_y = true;
    rig.mapper->Configure(rig.config);
    rig.Motion(0, -60);
    rig.Tick();
    CHECK(rig.State().cstick_y == -154);
    rig.Ticks(40);

    // the mouse stops being the camera: the stick is centred at once
    rig.config.mouse_invert_y = false;
    rig.mapper->Configure(rig.config);
    rig.Motion(60, 0);
    rig.Tick();
    REQUIRE(rig.State().cstick_x == 154);
    rig.Down(Key("Left Alt"));
    rig.Tick();
    CHECK(rig.State().cstick_x == 0);
    rig.Up(Key("Left Alt"));
    rig.Tick();
    CHECK(rig.State().cstick_x == 0);

    // the mouse's push joins the C-stick's keys and the right stick: the furthest wins
    rig.Down(Key("O"));  // up, full
    rig.Motion(60, 0);   // right, full: equal lengths, the keys (listed first) stay
    rig.Tick();
    CHECK(rig.State().cstick_x == 0);
    CHECK(rig.State().cstick_y == 154);
}

TEST_CASE("with the camera turn active the mouse turns the camera by degrees and pushes no stick") {
    Rig rig;
    rig.Tick();
    REQUIRE(rig.mapper->WantsRelativeMouse());
    rig.mapper->SetCameraTurnActive(true);
    CHECK(rig.Motion(10, -5));
    REQUIRE(rig.turns.size() == 1);
    // 50 (sensitivity) * 0.0024 = 0.12 degrees a count; the mouse up is up
    CHECK(rig.turns[0].first == doctest::Approx(1.2f));
    CHECK(rig.turns[0].second == doctest::Approx(0.6f));
    rig.Tick();
    CHECK(rig.State().cstick_x == 0);
    CHECK(rig.State().cstick_y == 0);
    // the settings: sensitivity, the vertical share and the inversions
    rig.config.mouse_sensitivity = 100;
    rig.config.mouse_vertical = 200;
    rig.config.mouse_invert_x = true;
    rig.config.mouse_invert_y = true;
    rig.mapper->Configure(rig.config);
    rig.Motion(10, -5);
    REQUIRE(rig.turns.size() == 2);
    CHECK(rig.turns[1].first == doctest::Approx(-2.4f));            // 10 * 0.24, inverted
    CHECK(rig.turns[1].second == doctest::Approx(-2.4f));           // 5 * 0.24 * 2.0, inverted
    rig.config.mouse_sensitivity = 1;
    rig.config.mouse_vertical = 10;
    rig.config.mouse_invert_x = false;
    rig.config.mouse_invert_y = false;
    rig.mapper->Configure(rig.config);
    rig.Motion(100, 100);
    REQUIRE(rig.turns.size() == 3);
    CHECK(rig.turns[2].first == doctest::Approx(0.24f));           // 100 * 0.0024
    CHECK(rig.turns[2].second == doctest::Approx(-0.024f));        // -100 * 0.0024 * 0.1
    // the camera turn is not asked when it is not the camera
    rig.Down(Key("Left Alt"));
    rig.Tick();
    rig.Motion(10, 10);
    CHECK(rig.turns.size() == 3);
    rig.Up(Key("Left Alt"));
    rig.Tick();
    // and off again: the C-stick takes the push
    rig.config.mouse_sensitivity = 50;
    rig.config.mouse_vertical = 100;
    rig.mapper->Configure(rig.config);
    rig.mapper->SetCameraTurnActive(false);
    rig.Motion(60, 0);
    rig.Tick();
    CHECK(rig.State().cstick_x == 154);
    CHECK(rig.turns.size() == 3);
    // a motion pending when it goes on is dropped
    rig.Ticks(40);
    rig.Motion(60, 0);
    rig.mapper->SetCameraTurnActive(true);
    rig.Tick();
    CHECK(rig.State().cstick_x == 0);
}

TEST_CASE("controllers that come and go: the first one plays, and the log says which") {
    Rig rig({Pad(kPad1, "aaaa", 0, "Pad A"), Pad(kPad2, "bbbb", 0, "Pad B")});
    CHECK(rig.env.Logged("controller: Pad A (port 0)"));
    rig.Tick();
    // the second controller's events are not the game's
    rig.PadButton(kPad2, az::PadButtonId::East, true);
    CHECK(rig.Buttons() == 0);
    rig.PadButton(kPad2, az::PadButtonId::East, false);
    rig.PadButton(kPad1, az::PadButtonId::East, true);
    CHECK(rig.Buttons() == mh4n::kPadA);

    // the first one is unplugged while a button is held: released, and the second takes over
    rig.fake_pads.pads = {Pad(kPad2, "bbbb", 0, "Pad B")};
    CHECK(rig.PadEvent(App::InputEvent::Type::PadRemoved, kPad1));
    CHECK(rig.Buttons() == 0);
    CHECK(rig.env.Logged("controller: Pad B (port 0)"));
    rig.PadButton(kPad1, az::PadButtonId::East, true);  // (an event of the removed one: nothing)
    CHECK(rig.Buttons() == 0);
    rig.PadButton(kPad2, az::PadButtonId::East, true);
    CHECK(rig.Buttons() == mh4n::kPadA);
    rig.PadButton(kPad2, az::PadButtonId::East, false);

    // a third comes in: the first of the list still plays
    rig.fake_pads.pads.push_back(Pad(kPad3, "bbbb", 1, "Pad C"));
    rig.PadEvent(App::InputEvent::Type::PadAdded, kPad3);
    rig.PadButton(kPad3, az::PadButtonId::East, true);
    CHECK(rig.Buttons() == 0);
    rig.PadButton(kPad3, az::PadButtonId::East, false);
    CHECK(rig.env.Logged("controller: Pad B (port 0)"));

    // the second goes: the third takes over, as port 1 of its kind
    rig.fake_pads.pads = {Pad(kPad3, "bbbb", 1, "Pad C")};
    rig.PadEvent(App::InputEvent::Type::PadRemoved, kPad2);
    CHECK(rig.env.Logged("controller: Pad C (port 1)"));
    rig.PadButton(kPad3, az::PadButtonId::East, true);
    CHECK(rig.Buttons() == mh4n::kPadA);
    rig.PadAxis(kPad3, az::PadAxisId::LeftX, 32767);
    CHECK(rig.State().circle_x == 154);

    // the last goes: nothing is held, and the line says so
    rig.fake_pads.pads.clear();
    rig.PadEvent(App::InputEvent::Type::PadRemoved, kPad3);
    CHECK(rig.State() == InputState{});
    CHECK(rig.env.Logged("no controller found"));
    // and a new one comes: its state starts released
    rig.fake_pads.pads = {Pad(kPad2, "bbbb", 0, "")};
    rig.PadEvent(App::InputEvent::Type::PadAdded, kPad2);
    CHECK(rig.env.Logged("controller: controller (port 0)"));  // a controller with no name
    CHECK(rig.State() == InputState{});
    rig.PadButton(kPad2, az::PadButtonId::South, true);
    CHECK(rig.Buttons() == mh4n::kPadB);
}

TEST_CASE("controller = off: no default binds, and the line says so; parameters still read controllers") {
    Rig rig(OnePad(), [](App::Config& c) {
        c.controller_auto = false;
        c.button_params[App::Button::Start] = "engine:sdl,guid:0,api:controller,button:6";
    });
    CHECK(rig.env.Logged("controller off"));
    rig.PadButton(kPad1, az::PadButtonId::East, true);
    rig.PadAxis(kPad1, az::PadAxisId::LeftX, 32767);
    CHECK(rig.State() == InputState{});
    rig.PadButton(kPad1, az::PadButtonId::Start, true);
    CHECK(rig.Buttons() == mh4n::kPadStart);
}

TEST_CASE("the settings read again keep what is held and re-bind keys") {
    Rig rig(OnePad());
    rig.Down(Key("F"));
    REQUIRE(rig.Buttons() == mh4n::kPadA);
    rig.config.keys[App::Button::A] = "G";
    rig.mapper->Configure(rig.config);
    CHECK(rig.Buttons() == 0);  // F is held but is not A's key now
    rig.Down(Key("G"));
    CHECK(rig.Buttons() == mh4n::kPadA);
    // an unknown key name is logged and leaves the others
    rig.config.keys[App::Button::A] = "no such key | H";
    rig.mapper->Configure(rig.config);
    CHECK(rig.env.Logged("no key called"));
    rig.Down(Key("H"));
    CHECK(rig.Buttons() == mh4n::kPadA);
}

TEST_CASE("the item bar's macro codes press the buttons too (the mouse's push and the scroll share the bind)") {
    // the macro's codes are the bind's: L held by the scroll reads as L whatever the keys are
    Rig rig({}, [](App::Config& c) { c.keys[App::Button::L] = "Q"; });
    rig.Tick();
    rig.Wheel(0, 1);
    rig.Tick();
    CHECK(rig.Buttons() == mh4n::kPadL);
}

TEST_CASE("the SDL3 layer lists a gamepad with its GUID, port and name") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    REQUIRE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    auto layer = mh4n::app::MakeSdlGamepads();
    CHECK(layer->List().empty());

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Synthetic Pad";
    const SDL_JoystickID first = SDL_AttachVirtualJoystick(&desc);
    REQUIRE_MESSAGE(first != 0, SDL_GetError());
    const SDL_JoystickID second = SDL_AttachVirtualJoystick(&desc);
    REQUIRE_MESSAGE(second != 0, SDL_GetError());

    const std::vector<PadInfo> pads = layer->List();
    REQUIRE(pads.size() == 2);
    CHECK(pads[0].id == static_cast<std::int32_t>(first));
    CHECK(pads[1].id == static_cast<std::int32_t>(second));
    CHECK(pads[0].name == "Synthetic Pad");
    CHECK_FALSE(pads[0].guid.empty());
    CHECK(pads[0].guid == pads[1].guid);  // two of a kind
    CHECK(pads[0].port == 0);
    CHECK(pads[1].port == 1);
    // the host's own list agrees
    const std::vector<App::ControllerInfo> host = App::Host::ListControllers();
    REQUIRE(host.size() == 2);
    CHECK(host[0].guid == pads[0].guid);
    CHECK(host[1].port == 1);

    // one goes away
    SDL_DetachVirtualJoystick(first);
    const std::vector<PadInfo> left = layer->List();
    REQUIRE(left.size() == 1);
    CHECK(left[0].id == static_cast<std::int32_t>(second));
    SDL_DetachVirtualJoystick(second);
    CHECK(layer->List().empty());
    layer.reset();
    SDL_Quit();
}
