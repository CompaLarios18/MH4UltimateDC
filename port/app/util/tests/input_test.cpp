// The app's input (input.cpp) against a fake runtime and a fake SDL: which controller the input
// profile is made for and how the log line names it, key names (with the mouse's and the lists of
// alternatives), the mute that goes to the runtime, and the keyboard-and-mouse mode's calls to it
// (the mouse's stick, the camera turn, the wheel's and the item bar's key presses). The SDL calls the file makes are defined here, so no SDL library is linked; only its
// headers are needed. Synthetic data only.
// Built with -DMH4U_APP_TESTS=ON (port/app/CMakeLists.txt) or by hand against doctest, {fmt}
// and the SDL2 headers.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <SDL.h>

#include "config.h"
#include "input.h"
#include "util/tests/fake_runtime.h"

namespace {

struct FakePad {
    bool is_controller = true;
    const char* guid = "";
    const char* name = nullptr;
};
std::vector<FakePad> pads;

/// The fake SDL's guid: the text is kept in the bytes.
SDL_JoystickGUID GuidOf(const char* text) {
    SDL_JoystickGUID guid{};
    std::strncpy(reinterpret_cast<char*>(guid.data), text, sizeof(guid.data) - 1);
    return guid;
}

} // namespace

extern "C" {
int SDL_NumJoysticks(void) {
    return static_cast<int>(pads.size());
}
SDL_bool SDL_IsGameController(int index) {
    return pads.at(static_cast<std::size_t>(index)).is_controller ? SDL_TRUE : SDL_FALSE;
}
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int index) {
    return GuidOf(pads.at(static_cast<std::size_t>(index)).guid);
}
void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char* text, int size) {
    std::strncpy(text, reinterpret_cast<const char*>(guid.data), static_cast<std::size_t>(size) - 1);
    text[size - 1] = '\0';
}
const char* SDL_GameControllerNameForIndex(int index) {
    return pads.at(static_cast<std::size_t>(index)).name;
}
SDL_Scancode SDL_GetScancodeFromName(const char* name) {
    if (std::strcmp(name, "W") == 0) {
        return SDL_SCANCODE_W;
    }
    if (std::strcmp(name, "F12") == 0) {
        return SDL_SCANCODE_F12;
    }
    if (std::strcmp(name, "F") == 0) {
        return SDL_SCANCODE_F;
    }
    return SDL_SCANCODE_UNKNOWN;
}
const char* SDL_GetScancodeName(SDL_Scancode code) {
    return code == SDL_SCANCODE_W ? "W" : code == SDL_SCANCODE_F12 ? "F12" : "";
}
}

using namespace App;
using fake::Env;

TEST_CASE("key names are SDL's, and an unknown one is 0 with a warning") {
    Env env;
    CHECK(Input::KeyCode("W") == SDL_SCANCODE_W);
    CHECK(Input::KeyCode("F12") == SDL_SCANCODE_F12);
    CHECK(Input::KeyCode("") == 0);
    CHECK_FALSE(env.Logged("no key called"));
    CHECK(Input::KeyCode("Hyper") == 0);
    CHECK(env.Logged("no key called \"Hyper\""));
}

TEST_CASE("without a controller the profile is made for the keys alone") {
    Env env;
    pads.clear();
    const Config config = Defaults();
    Input::BuildProfile(config);
    CHECK(Input::ControllerDescription() == "no controller found");
    REQUIRE(env.runtime.profiles.size() == 1);
    CHECK_FALSE(env.runtime.profiles[0].controller_given);
}

TEST_CASE("the first controller SDL knows is the profile's") {
    Env env;
    pads = {{false, "joystick-guid", "Wheel"}, {true, "pad-guid", "Pad A"}, {true, "other-guid", "Pad B"}};
    const Config config = Defaults();
    Input::BuildProfile(config);
    CHECK(Input::ControllerDescription() == "controller: Pad A (port 0)");
    REQUIRE(env.runtime.profiles.size() == 1);
    const auto& call = env.runtime.profiles[0];
    REQUIRE(call.controller_given);
    CHECK(call.controller.guid == "pad-guid");
    CHECK(call.controller.port == 0);
    CHECK(call.controller.name == "Pad A");
}

TEST_CASE("the port counts the devices with the same GUID before the controller") {
    Env env;
    pads = {{false, "same", "Not a pad"}, {true, "same", "Twin 1"}, {true, "same", "Twin 2"}};
    Input::BuildProfile(Defaults());
    REQUIRE(env.runtime.profiles.size() == 1);
    CHECK(env.runtime.profiles[0].controller.port == 1);
    CHECK(Input::ControllerDescription() == "controller: Twin 1 (port 1)");
}

TEST_CASE("a controller without a name is called controller") {
    Env env;
    pads = {{true, "g", nullptr}};
    Input::BuildProfile(Defaults());
    REQUIRE(env.runtime.profiles.size() == 1);
    CHECK(env.runtime.profiles[0].controller.name == "controller");
    CHECK(Input::ControllerDescription() == "controller: controller (port 0)");
}

TEST_CASE("controller = off gives the profile no controller even when one is plugged in") {
    Env env;
    pads = {{true, "g", "Pad A"}};
    Config config = Defaults();
    config.controller_auto = false;
    Input::BuildProfile(config);
    CHECK(Input::ControllerDescription() == "controller off");
    REQUIRE(env.runtime.profiles.size() == 1);
    CHECK_FALSE(env.runtime.profiles[0].controller_given);
}

TEST_CASE("the mute goes to the runtime as it comes") {
    Env env;
    Input::SetMuted(true);
    Input::SetMuted(false);
    Input::SetMuted(true);
    CHECK(env.runtime.mutes == std::vector<bool>{true, false, true});
}

TEST_CASE("the mouse's buttons and the wheel are keys with names of their own") {
    Env env;
    CHECK(Input::KeyCode("Mouse Right") == Input::MOUSE_KEY + SDL_BUTTON_RIGHT);
    CHECK(Input::KeyCode("mouse left") == Input::MOUSE_KEY + SDL_BUTTON_LEFT);
    CHECK(Input::KeyCode("Mouse 4") == Input::MOUSE_KEY + SDL_BUTTON_X1);
    CHECK(Input::KeyCode("Wheel Up") == Input::WHEEL_UP);
    CHECK_FALSE(env.Logged("no key called"));
    CHECK(Input::MouseCode("Wheel Down") == Input::WHEEL_DOWN);
    CHECK(Input::MouseCode("W") == 0);
    CHECK(Input::KeyName(Input::MOUSE_KEY + SDL_BUTTON_MIDDLE) == "Mouse Middle");
    CHECK(Input::KeyName(SDL_SCANCODE_W) == "W");
    CHECK(Input::KeyName(0) == "");
}

TEST_CASE("a 3DS button takes a list of keys separated by |") {
    Env env;
    CHECK(Input::KeyNames("F | Mouse Right") == std::vector<std::string>{"F", "Mouse Right"});
    CHECK(Input::KeyNames(" W |  | F12 ") == std::vector<std::string>{"W", "F12"});
    CHECK(Input::KeyNames("").empty());
    CHECK(Input::KeyCodes("F | Mouse Right") ==
          std::vector<int>{SDL_SCANCODE_F, Input::MOUSE_KEY + SDL_BUTTON_RIGHT});
    CHECK(Input::KeyCodes("Hyper | W") == std::vector<int>{SDL_SCANCODE_W});  // an unknown one is left out
    CHECK(Input::FirstKey("Mouse Left | V") == "Mouse Left");
    CHECK(Input::FirstKey("") == "");
}

TEST_CASE("input_mode names") {
    CHECK(Input::ParseMode("controller") == Input::Mode::Controller);
    CHECK(Input::ParseMode("keyboard_mouse") == Input::Mode::KeyboardMouse);
    CHECK(Input::ParseMode("mouse") == Input::Mode::KeyboardMouse);
    CHECK(Input::ParseMode("auto") == Input::Mode::Auto);
    CHECK(Input::ParseMode("anything else") == Input::Mode::Auto);
    CHECK(std::string(Input::ModeName(Input::Mode::KeyboardMouse)) == "keyboard_mouse");
    CHECK(std::string(Input::ModeName(Input::Mode::Controller)) == "controller");
    CHECK(std::string(Input::ModeName(Input::Mode::Auto)) == "auto");
}

TEST_CASE("the mouse turns the game's camera directly while the hook runs") {
    Env env;
    Config config = Defaults();
    config.mouse_sensitivity = 50;  // 0.12 degrees a count
    config.mouse_vertical = 100;
    env.runtime.camera_turn_active = true;
    Input::MouseMotion(config, 100, 50);
    REQUIRE(env.runtime.camera_turns.size() == 1);
    CHECK(env.runtime.camera_turns[0].first == doctest::Approx(12.0f));
    CHECK(env.runtime.camera_turns[0].second == doctest::Approx(-6.0f));  // the mouse down turns the camera down
    config.mouse_invert_x = true;
    config.mouse_invert_y = true;
    config.mouse_vertical = 50;
    Input::MouseMotion(config, 100, 50);
    REQUIRE(env.runtime.camera_turns.size() == 2);
    CHECK(env.runtime.camera_turns[1].first == doctest::Approx(-12.0f));
    CHECK(env.runtime.camera_turns[1].second == doctest::Approx(3.0f));
    // no push on the stick while the camera takes the mouse itself
    Input::Tick(config);
    REQUIRE_FALSE(env.runtime.mouse_sticks.empty());
    CHECK(env.runtime.mouse_sticks.back() == std::pair<float, float>{0.0f, 0.0f});
}

TEST_CASE("without the hook the mouse pushes the C-stick, and stopping centres it and drops the turn") {
    Env env;
    const Config config = Defaults();
    env.runtime.camera_turn_active = false;
    Input::MouseMotion(config, 400, 0);
    CHECK(env.runtime.camera_turns.empty());
    Input::Tick(config);
    REQUIRE_FALSE(env.runtime.mouse_sticks.empty());
    CHECK(env.runtime.mouse_sticks.back().first > 0.0f);
    Input::MouseReset();
    CHECK(env.runtime.camera_turn_clears == 1);
    CHECK(env.runtime.mouse_sticks.back() == std::pair<float, float>{0.0f, 0.0f});
}

TEST_CASE("the wheel scrolls the item bar with L and Y or A, and is a short press otherwise") {
    Env env;
    Config config = Defaults();
    Input::Wheel(config, Input::WHEEL_UP);  // item_prev: L held, then Y
    for (int frame = 0; frame < 30; ++frame) {
        Input::Tick(config);
    }
    const int l = Input::MACRO_KEY + 8, y = Input::MACRO_KEY + 3;  // App::Button::L, Y
    CHECK(std::find(env.runtime.keys_down.begin(), env.runtime.keys_down.end(), l) != env.runtime.keys_down.end());
    CHECK(std::find(env.runtime.keys_down.begin(), env.runtime.keys_down.end(), y) != env.runtime.keys_down.end());
    CHECK(std::find(env.runtime.keys_up.begin(), env.runtime.keys_up.end(), l) != env.runtime.keys_up.end());
    env.runtime.keys_down.clear();
    env.runtime.keys_up.clear();
    config.item_prev = "Wheel Left";
    config.item_next = "Wheel Right";
    Input::Wheel(config, Input::WHEEL_UP);  // no longer the item bar's: pressed for a moment
    CHECK(env.runtime.keys_down == std::vector<int>{Input::WHEEL_UP});
    CHECK(env.runtime.keys_up.empty());
    for (int frame = 0; frame < 30; ++frame) {
        Input::Tick(config);
    }
    CHECK(std::find(env.runtime.keys_up.begin(), env.runtime.keys_up.end(), Input::WHEEL_UP) != env.runtime.keys_up.end());
}
