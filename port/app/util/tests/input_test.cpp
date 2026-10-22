// The app's input (input.cpp) against a fake runtime and a fake host (util/tests/fake_host.h):
// which controller the input profile is made for and how the log line names it, a controller's
// port among devices of its kind, key names (with the mouse's and the lists of alternatives), the
// mute that goes to the runtime, and the keyboard-and-mouse mode's calls to it (the mouse's stick,
// the camera turn, the wheel's and the item bar's key presses). The host's services are defined by
// the test programs, so no SDL is involved at all. Synthetic data only.
// Built with -DMH4U_APP_TESTS=ON (port/app/CMakeLists.txt) or by hand against doctest and {fmt}.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "app_window.h"
#include "config.h"
#include "input.h"
#include "util/tests/fake_host.h"
#include "util/tests/fake_runtime.h"

using namespace App;
using fake::Env;

namespace {

/// The controllers the fake host lists (the way the host numbers them is ControllerPort's).
void Plug(std::vector<ControllerInfo> pads) {
    fake::Host().Reset();
    fake::Host().controllers = std::move(pads);
}

} // namespace

TEST_CASE("key names are the host's, and an unknown one is 0 with a warning") {
    Env env;
    fake::Host().Reset();
    CHECK(Input::KeyCode("W") == fake::SCANCODE_W);
    CHECK(Input::KeyCode("F12") == fake::SCANCODE_F12);
    CHECK(Input::KeyCode("") == 0);
    CHECK_FALSE(env.Logged("no key called"));
    CHECK(Input::KeyCode("Hyper") == 0);
    CHECK(env.Logged("no key called \"Hyper\""));
}

TEST_CASE("without a controller the profile is made for the keys alone") {
    Env env;
    Plug({});
    const Config config = Defaults();
    Input::BuildProfile(config);
    CHECK(Input::ControllerDescription() == "no controller found");
    REQUIRE(env.runtime.profiles.size() == 1);
    CHECK_FALSE(env.runtime.profiles[0].controller_given);
}

TEST_CASE("the first controller the host lists is the profile's") {
    Env env;
    Plug({{"pad-guid", 0, "Pad A"}, {"other-guid", 0, "Pad B"}});
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
    // the GUID of every joystick the host knows, controllers or not: {"same", "same", "same"} is
    // a device that is not a controller and two twins
    const std::vector<std::string> guids{"same", "other", "same", "same"};
    CHECK(Input::ControllerPort(guids, 0) == 0);
    CHECK(Input::ControllerPort(guids, 1) == 0);
    CHECK(Input::ControllerPort(guids, 2) == 1);
    CHECK(Input::ControllerPort(guids, 3) == 2);
    CHECK(Input::ControllerPort(guids, 9) == 0);  // past the end: nothing to count
    Plug({{"same", 1, "Twin 1"}, {"same", 2, "Twin 2"}});
    Input::BuildProfile(Defaults());
    REQUIRE(env.runtime.profiles.size() == 1);
    CHECK(env.runtime.profiles[0].controller.port == 1);
    CHECK(Input::ControllerDescription() == "controller: Twin 1 (port 1)");
}

TEST_CASE("a controller without a name is called controller") {
    Env env;
    Plug({{"g", 0, ""}});
    Input::BuildProfile(Defaults());
    REQUIRE(env.runtime.profiles.size() == 1);
    CHECK(env.runtime.profiles[0].controller.name == "controller");
    CHECK(Input::ControllerDescription() == "controller: controller (port 0)");
}

TEST_CASE("controller = off gives the profile no controller even when one is plugged in") {
    Env env;
    Plug({{"g", 0, "Pad A"}});
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
    CHECK(Input::KeyCode("Mouse Right") == Input::MOUSE_KEY + MouseButton::Right);
    CHECK(Input::KeyCode("mouse left") == Input::MOUSE_KEY + MouseButton::Left);
    CHECK(Input::KeyCode("Mouse 4") == Input::MOUSE_KEY + MouseButton::X1);
    CHECK(Input::KeyCode("Wheel Up") == Input::WHEEL_UP);
    CHECK_FALSE(env.Logged("no key called"));
    CHECK(Input::MouseCode("Wheel Down") == Input::WHEEL_DOWN);
    CHECK(Input::MouseCode("W") == 0);
    CHECK(Input::KeyName(Input::MOUSE_KEY + MouseButton::Middle) == "Mouse Middle");
    CHECK(Input::KeyName(fake::SCANCODE_W) == "W");
    CHECK(Input::KeyName(0) == "");
}

TEST_CASE("a 3DS button takes a list of keys separated by |") {
    Env env;
    CHECK(Input::KeyNames("F | Mouse Right") == std::vector<std::string>{"F", "Mouse Right"});
    CHECK(Input::KeyNames(" W |  | F12 ") == std::vector<std::string>{"W", "F12"});
    CHECK(Input::KeyNames("").empty());
    CHECK(Input::KeyCodes("F | Mouse Right") ==
          std::vector<int>{fake::SCANCODE_F, Input::MOUSE_KEY + MouseButton::Right});
    CHECK(Input::KeyCodes("Hyper | W") == std::vector<int>{fake::SCANCODE_W});  // an unknown one is left out
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
