// The app's shared sources as mh4u-native links them (native/app/shared, native/app/host): the
// settings file read from a synthetic mh4u-app.ini, App::Host on SDL3 (no display: SDL's dummy
// video driver), and the stubs of the network rooms. Synthetic data only.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "app_window.h"
#include "config.h"
#include "input.h"
#include "launcher/lobby.h"
#include "multiplayer.h"
#include "util/tests/fake_runtime.h"

namespace {

// SDL with no display: the dummy video driver, and the joystick subsystem for the controllers.
struct DummySdl {
    DummySdl() {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        ok = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    }
    ~DummySdl() {
        SDL_Quit();
    }
    bool ok = false;
};

}  // namespace

TEST_CASE("the settings file is read from a synthetic mh4u-app.ini") {
    fake::Env env;
    {
        std::ofstream ini(env.dir.str("mh4u-app.ini"));
        ini << "[Game]\ngame = /synthetic/game.cxi\nlauncher = false\n"
               "[Display]\nrenderer = software\nfullscreen = false\nscreens = side\n"
               "[Audio]\nvolume = 40\n"
               "[Multiplayer]\nnickname = Synthetic Hunter\n";
    }
    const App::Config c = App::Load();
    CHECK(App::ConfigPath() == env.dir.str("mh4u-app.ini"));
    CHECK(c.game == "/synthetic/game.cxi");
    CHECK_FALSE(c.launcher);
    CHECK(c.renderer == "software");
    CHECK_FALSE(c.fullscreen);
    CHECK(c.screens == "side");
    CHECK(c.volume == 40);
    CHECK(c.nickname == "Synthetic Hunter");
    // a key the file does not name keeps its default
    CHECK(c.fullscreen_key == App::Defaults().fullscreen_key);
}

TEST_CASE("App::Host names keys through SDL3, in SDL2's numbering") {
    DummySdl sdl;
    REQUIRE(sdl.ok);
    CHECK(App::Host::ScancodeFromName("W") == 26);
    CHECK(App::Host::ScancodeFromName("Escape") == App::Scancode::Escape);
    CHECK(App::Host::ScancodeFromName("Return") == App::Scancode::Return);
    CHECK(App::Host::ScancodeFromName("Left Shift") == 225);
    CHECK(App::Host::ScancodeFromName("F12") == 69);
    CHECK(App::Host::ScancodeFromName("no such key") == 0);
    CHECK(App::Host::ScancodeName(26) == "W");
    CHECK(App::Host::ScancodeName(App::Scancode::Delete) == "Delete");
    CHECK(App::Host::ScancodeName(0).empty());
    CHECK(App::Host::ScancodeName(-1).empty());
    CHECK(App::Host::ScancodeName(App::Scancode::Count).empty());
    CHECK(App::Host::ScancodeName(100000).empty());
    // names round-trip
    for (const char* name : {"A", "Space", "Left Alt", "F3", "Up"}) {
        CHECK(App::Host::ScancodeName(App::Host::ScancodeFromName(name)) == name);
    }
}

TEST_CASE("App::Host: time, clipboard, cursor and mouse position") {
    DummySdl sdl;
    REQUIRE(sdl.ok);
    const std::uint32_t t0 = App::Host::Ticks();
    SDL_Delay(5);
    const std::uint32_t t1 = App::Host::Ticks();
    CHECK(static_cast<std::uint32_t>(t1 - t0) >= 4);
    CHECK(static_cast<std::uint32_t>(t1 - t0) < 5000);

    App::Host::SetClipboard("synthetic clipboard text");
    CHECK(App::Host::GetClipboard() == "synthetic clipboard text");
    App::Host::SetClipboard("");
    CHECK(App::Host::GetClipboard().empty());

    App::Host::ShowCursor(false);
    CHECK_FALSE(SDL_CursorVisible());
    App::Host::ShowCursor(true);
    CHECK(SDL_CursorVisible());

    const App::Point at = App::Host::MousePosition();   // no window has the focus: some point
    CHECK(at.x >= 0);
    CHECK(at.y >= 0);
}

TEST_CASE("App::Host lists the game controllers, and a virtual one has a port") {
    DummySdl sdl;
    REQUIRE(sdl.ok);
    CHECK(App::Host::ListControllers().empty());

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Synthetic pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    REQUIRE(id != 0);
    SDL_UpdateJoysticks();
    const auto pads = App::Host::ListControllers();
    REQUIRE(pads.size() == 1);
    CHECK(pads[0].name == "Synthetic pad");
    CHECK(pads[0].port == 0);
    CHECK(pads[0].guid.size() == 32);
    SDL_DetachVirtualJoystick(id);
    CHECK(App::Host::ListControllers().empty());
}

TEST_CASE("controller ports count devices of the same GUID") {
    // Input::ControllerPort counts devices of the same GUID before the index
    CHECK(App::Input::ControllerPort({"a", "b", "a", "a"}, 3) == 2);
    CHECK(App::Input::ControllerPort({"a", "b", "a", "a"}, 1) == 0);
}

TEST_CASE("mh4u-native has no rooms") {
    fake::Env env;
    CHECK_FALSE(App::Multiplayer::Available());
    App::Config config = App::Defaults();
    config.nickname = "Synthetic Hunter";
    App::Multiplayer::Init(config);
    CHECK_FALSE(App::Multiplayer::Available());
    CHECK(App::Multiplayer::Nickname() == "Synthetic Hunter");
    std::string error = "left over";
    CHECK(App::Multiplayer::StartAtBoot(error));
    CHECK(error.empty());
    CHECK_FALSE(App::Multiplayer::Join({}));
    CHECK_FALSE(App::Multiplayer::Host({}));
    CHECK_FALSE(App::Multiplayer::Leave(true));
    CHECK_FALSE(App::Multiplayer::Rejoin());
    CHECK(App::Multiplayer::LastRoom().empty());
    App::Multiplayer::Tick();
    App::Multiplayer::JoinLater({}, 1.0);
    const auto status = App::Multiplayer::GetStatus();
    CHECK(status.phase == App::Multiplayer::Phase::Off);
    CHECK(status.members.empty());
    CHECK(App::Multiplayer::ValidNickname("Hunter-01"));
    CHECK_FALSE(App::Multiplayer::ValidNickname("abc"));
    CHECK_FALSE(App::Multiplayer::ValidNickname("bad/name"));
    App::Multiplayer::Shutdown();
    CHECK(App::Multiplayer::Nickname().empty());

    const auto list = Launcher::Lobby::Fetch("lobby.example.org");
    CHECK(list.rooms.empty());
    CHECK_FALSE(list.ok);
    CHECK_FALSE(list.error.empty());
    CHECK(Launcher::Lobby::Parse("{\"rooms\": []}").rooms.empty());
    CHECK(Launcher::Lobby::NormalizeUrl(" lobby.example.org:24872/ ") == "http://lobby.example.org:24872");
    CHECK(Launcher::Lobby::NormalizeUrl("https://x.example") == "https://x.example");
    CHECK(Launcher::Lobby::NormalizeUrl("  ").empty());
}
