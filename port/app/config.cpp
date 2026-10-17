#include "config.h"

#include <algorithm>
#include <fmt/format.h>
#include <INIReader.h>

#include "runtime.h"
#include "util/file.h"
#include "util/log.h"

namespace App {

/// The keyboard as it comes: the left hand on the circle pad and the hunter's actions (R, the
/// dash, on Shift), the arrows on the C-stick (the camera).
Config Defaults() {
    Config c;
    c.keys[Button::A] = "E";
    c.keys[Button::B] = "Space";
    c.keys[Button::X] = "R";
    c.keys[Button::Y] = "F";
    c.keys[Button::Up] = "1";
    c.keys[Button::Down] = "2";
    c.keys[Button::Left] = "3";
    c.keys[Button::Right] = "4";
    c.keys[Button::L] = "Q";
    c.keys[Button::R] = "Left Shift";
    c.keys[Button::Start] = "Return";
    c.keys[Button::Select] = "Backspace";
    c.keys[Button::ZL] = "Tab";
    c.keys[Button::ZR] = "C";
    c.stick_keys[Analog::CirclePad] = "W,S,A,D,Left Ctrl";
    c.stick_keys[Analog::CStick] = "Up,Down,Left,Right";
    return c;
}

namespace {

/// [Debug] perf_log: where the runtime starts the per-frame CSV (N0-15; empty: off). Kept here,
/// not in Config, so that Template writes it back unchanged whoever saves the file.
std::string g_perf_log;

bool IsOn(const std::string& v) {
    return v == "1" || v == "on" || v == "yes" || v == "true";
}

std::string OnOff(bool v) {
    return v ? "on" : "off";
}

/// A value without its key: "button_a" -> "a" for the template's key list.
std::string ButtonKeyName(int button) {
    return Runtime().ButtonSettingName(button);
}

} // namespace

std::string ConfigPath() {
    return Runtime().UserDir() + "mh4u-app.ini";
}

Config Load() {
    Config c = Defaults();
    const std::string path = ConfigPath();
    if (!App::Util::Exists(path)) {
        App::Util::CreateFullPath(path);
        App::Util::WriteStringToFile(true, path, Template(c));
        APP_LOG_INFO("wrote {} with the defaults", path);
        return c;
    }
    INIReader ini(path);
    if (ini.ParseError() < 0) {
        APP_LOG_ERROR("couldn't read {}, using the defaults", path);
        return c;
    }
    if (ini.ParseError() > 0) {
        APP_LOG_WARNING("{}: line {} isn't understood", path, ini.ParseError());
    }
    const auto str = [&](const char* section, const char* key, const std::string& def) {
        return ini.Get(section, key, def);
    };
    const auto flag = [&](const char* section, const char* key, bool def) {
        return IsOn(ini.Get(section, key, def ? "on" : "off"));
    };
    const auto num = [&](const char* section, const char* key, int def, int lo, int hi) {
        return std::clamp(static_cast<int>(ini.GetInteger(section, key, def)), lo, hi);
    };

    c.game = str("Game", "game", c.game);
    c.launcher = flag("Game", "launcher", c.launcher);
    c.new_3ds = str("Game", "new_3ds", c.new_3ds);
    c.cpu_clock = num("Game", "cpu_clock", c.cpu_clock, 5, 400);

    c.renderer = str("Display", "renderer", c.renderer);
    c.fullscreen = flag("Display", "fullscreen", c.fullscreen);
    c.resolution = num("Display", "resolution", c.resolution, 0, 10);
    c.vsync = flag("Display", "vsync", c.vsync);
    c.screens = str("Display", "screens", c.screens);
    c.linear_filter = flag("Display", "linear_filter", c.linear_filter);
    c.custom_textures = flag("Display", "custom_textures", c.custom_textures);
    c.shader = str("Display", "shader", c.shader);
    c.fps_counter = str("Display", "fps_counter", c.fps_counter);

    c.volume = num("Audio", "volume", c.volume, 0, 100);
    c.audio_output = str("Audio", "output", c.audio_output);

    for (int i = 0; i < Button::Count; ++i) {
        c.keys[i] = str("Controls", Runtime().ButtonSettingName(i), c.keys[i]);
        c.button_params[i] = str("Controls", ("param_" + ButtonKeyName(i)).c_str(), "");
    }
    for (int i = 0; i < Analog::Count; ++i) {
        c.stick_keys[i] = str("Controls", Runtime().StickSettingName(i), c.stick_keys[i]);
        c.stick_params[i] =
            str("Controls", (std::string("param_") + Runtime().StickSettingName(i)).c_str(), "");
    }
    c.controller_auto = str("Controls", "controller", "auto") == "auto";
    c.touch_screen_key = str("Controls", "touch_screen_key", c.touch_screen_key);
    c.fullscreen_key = str("Controls", "fullscreen_key", c.fullscreen_key);
    c.fps_counter_key = str("Controls", "fps_counter_key", c.fps_counter_key);

    c.nickname = str("Multiplayer", "nickname", c.nickname);
    c.join = str("Multiplayer", "join", c.join);
    c.password = str("Multiplayer", "password", c.password);
    c.host = num("Multiplayer", "host", c.host, 0, 65535);
    c.room_port = num("Multiplayer", "room_port", c.room_port, 1, 65535);
    c.room_name = str("Multiplayer", "room_name", c.room_name);
    c.room_members = num("Multiplayer", "room_members", c.room_members, 2, 254);
    c.lobby = str("Multiplayer", "lobby", c.lobby);
    c.token = str("Multiplayer", "token", c.token);
    c.room_public = flag("Multiplayer", "room_public", c.room_public);
    c.last_room = str("Multiplayer", "last_room", c.last_room);

    c.catalog_url = str("Mods", "catalog_url", c.catalog_url);
    g_perf_log = str("Debug", "perf_log", "");
    APP_LOG_INFO("settings from {}", path);
    return c;
}

void Save(const Config& config) {
    const std::string path = ConfigPath();
    App::Util::CreateFullPath(path);
    if (!App::Util::WriteStringToFile(true, path, Template(config))) {
        APP_LOG_ERROR("couldn't write {}", path);
    }
}

std::string Template(const Config& c) {
    std::string keys;
    for (int i = 0; i < Button::Count; ++i) {
        keys += fmt::format("{} = {}\n", Runtime().ButtonSettingName(i), c.keys[i]);
    }
    for (int i = 0; i < Analog::Count; ++i) {
        keys += fmt::format("{} = {}\n", Runtime().StickSettingName(i), c.stick_keys[i]);
    }
    std::string params;
    for (int i = 0; i < Button::Count; ++i) {
        if (!c.button_params[i].empty()) {
            params += fmt::format("param_{} = {}\n", Runtime().ButtonSettingName(i), c.button_params[i]);
        }
    }
    for (int i = 0; i < Analog::Count; ++i) {
        if (!c.stick_params[i].empty()) {
            params += fmt::format("param_{} = {}\n", Runtime().StickSettingName(i), c.stick_params[i]);
        }
    }
    return fmt::format(R"(# MH4U PC port: the app's settings. The game's own options (frame rate, widescreen, the touch
# screen's corner, the kick and target camera buttons, the screen filters) are in mh4u.ini next
# to this file. A line starting with # is ignored; on/off settings also take 1/0.

[Game]
# your decrypted Monster Hunter 4 Ultimate (USA): .3ds, .cci or .cxi. Empty: the app asks.
# The v1.3.0 update must be installed (Build code library does that) for the port's code.
game = {}
# the launcher before the game: Play, the setup, mods, the room, settings. off: straight into
# the game (the launcher still shows while the game, the update or the code library is missing)
launcher = {}
# Azahar's New 3DS mode: on, off or auto (off while custom textures are on: the HD texture pack's
# monsters show only then; on otherwise)
new_3ds = {}
# the CPU's clock speed in % of the 3DS's (5-400): above 100 a busy scene keeps a high frame
# rate (fps in mh4u.ini) at the cost of more work for your PC; the same as Azahar's CPU Clock Speed
cpu_clock = {}

[Display]
# vulkan, opengl or software (opengl isn't available on macOS; vulkan runs on Metal there)
renderer = {}
fullscreen = {}
# internal resolution in multiples of the 3DS's (1-10); 0 = as many as the window needs
resolution = {}
vsync = {}
# overlay: the game fills the window and the touch screen sits over it in a corner (mh4u.ini's
#          touch_overlay and touch_screen say where and when; F12 or the right stick's click
#          switch it)
# single: the game alone; stacked: the two screens as on a 3DS; side: side by side;
# windows: the touch screen in a window of its own
screens = {}
# smooth the picture when it is scaled up
linear_filter = {}
# texture packs from load/textures/0004000000126300/ in this folder
custom_textures = {}
# a post-processing shader from the shaders folder (empty: none)
shader = {}
# the frame-rate counter over the game: off, top-left, top-right, bottom-left or bottom-right
# (fps_counter_key, F3, shows and hides it while you play)
fps_counter = {}

[Audio]
# 0-100
volume = {}
# auto, or cubeb, sdl2, openal, null
output = {}

[Controls]
# controller = auto maps the first controller found like MH3U on a Wii U: the 3DS's A, B, X, Y
# on the right face buttons by position, L/R on the bumpers, ZL/ZR on the triggers (target
# camera and kick, see mh4u.ini), the circle pad and C-stick (camera) on the sticks, START and
# SELECT on Start and Back; the right stick's click switches the touch screen. off: keys only.
controller = {}
# keyboard keys by their SDL names (https://wiki.libsdl.org/SDL2/SDL_Scancode): a 3DS button
# each; a stick takes up,down,left,right and optionally a key that makes it move half way
{}# full Azahar input parameters replace a binding altogether, e.g.
# param_button_a = engine:sdl,guid:...,port:0,api:controller,button:1
{}touch_screen_key = {}
fullscreen_key = {}
fps_counter_key = {}

[Multiplayer]
# MH4U's local play over the internet, through a room (Azahar's network rooms; docs/SERVER.md).
# The in-game menu's Multiplayer tab (Esc) joins, hosts and leaves rooms while the game runs and
# keeps these lines up to date. Your name in the room (empty: the console's user name, set in
# Azahar's System settings)
nickname = {}
# join this room when the game starts: HOST, HOST:PORT (default port 24872) or an invite code
# (mh4u://...), and its password if it has one
join = {}
password = {}
# or host a room on this UDP port when the game starts (24872; forward it on your router), and
# join it (0: don't)
host = {}
# the port the menu hosts on
room_port = {}
room_name = {}
room_members = {}
# a lobby server lists the hosted room publicly (http://HOST:PORT); the token is your password
# there (any you like on an open lobby)
lobby = {}
token = {}
# room_public = off keeps a hosted room out of the lobby's list (players join it by address)
room_public = {}
# the last room joined (its invite code) or hosted ("host"), for the menu's Rejoin last room
last_room = {}

[Mods]
# the launcher's mod catalog (Mods, Catalog) comes with the app's tools; a newer one is fetched
# from here when the launcher starts (empty: never; the copy that came with the app is used)
catalog_url = {}

[Debug]
# write one CSV row per frame to this file: the frame time and the time in the game's code, in
# system calls and in the GPU's command processing (docs/native/perf-procedure.md). Empty: off
perf_log = {}
)",
                       c.game, OnOff(c.launcher), c.new_3ds, c.cpu_clock, c.renderer, OnOff(c.fullscreen), c.resolution,
                       OnOff(c.vsync), c.screens, OnOff(c.linear_filter), OnOff(c.custom_textures),
                       c.shader, c.fps_counter, c.volume, c.audio_output, c.controller_auto ? "auto" : "off", keys,
                       params, c.touch_screen_key, c.fullscreen_key, c.fps_counter_key, c.nickname, c.join, c.password,
                       c.host, c.room_port, c.room_name, c.room_members, c.lobby, c.token,
                       OnOff(c.room_public), c.last_room, c.catalog_url, g_perf_log);
}

const std::string& PerfLogPath() {
    return g_perf_log;
}

} // namespace App
