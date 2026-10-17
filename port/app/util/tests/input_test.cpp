// The app's input (input.cpp) against a fake runtime and a fake SDL: which controller the input
// profile is made for and how the log line names it, key names, and the mute that goes to the
// runtime. The SDL calls the file makes are defined here, so no SDL library is linked; only its
// headers are needed. Synthetic data only.
// Built with -DMH4U_APP_TESTS=ON (port/app/CMakeLists.txt) or by hand against doctest, {fmt}
// and the SDL2 headers.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

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
    return SDL_SCANCODE_UNKNOWN;
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
