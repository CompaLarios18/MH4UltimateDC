#include "config.h"

#include <algorithm>
#include <utility>
#include <vector>
#include <fmt/format.h>
#include <INIReader.h>

#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/hle/service/service.h"
#include "input.h"
#include "speed.h"

namespace App {

namespace {
namespace NativeAnalog = Settings::NativeAnalog;
namespace NativeButton = Settings::NativeButton;
} // namespace

/// The keyboard and mouse as they come, after Monster Hunter Rise on PC: W A S D move, the mouse
/// turns the camera (the C-stick), the left button draws and attacks (X), the right one is the
/// other attack (A, which F also is: gather, carve, talk), Space evades (B), E uses an item or
/// sheathes (Y), Shift dashes and the side button guards (R), Ctrl or the wheel's click resets
/// the camera (L), Q targets a monster (ZL), C kicks or skips (ZR), the wheel scrolls the item
/// bar; O K L ; are the C-stick too and the arrows the D-pad.
Config Defaults() {
    Config c;
    c.keys[NativeButton::A] = "F | Mouse Right";
    c.keys[NativeButton::B] = "Space";
    c.keys[NativeButton::X] = "Mouse Left | V";
    c.keys[NativeButton::Y] = "E";
    c.keys[NativeButton::Up] = "Up";
    c.keys[NativeButton::Down] = "Down";
    c.keys[NativeButton::Left] = "Left";
    c.keys[NativeButton::Right] = "Right";
    c.keys[NativeButton::L] = "Left Ctrl | Mouse Middle";
    c.keys[NativeButton::R] = "Left Shift | Mouse 4";
    c.keys[NativeButton::Start] = "Return";
    c.keys[NativeButton::Select] = "Backspace";
    c.keys[NativeButton::ZL] = "Q";
    c.keys[NativeButton::ZR] = "C";
    c.stick_keys[NativeAnalog::CirclePad] = "W,S,A,D";
    c.stick_keys[NativeAnalog::CStick] = "O,L,K,;";
    return c;
}

/// The keyboard as it came before: the left hand on the circle pad and the hunter's actions (R,
/// the dash, on Shift), the arrows on the C-stick (the camera).
Config OldDefaults() {
    Config c;
    c.keys[NativeButton::A] = "E";
    c.keys[NativeButton::B] = "Space";
    c.keys[NativeButton::X] = "R";
    c.keys[NativeButton::Y] = "F";
    c.keys[NativeButton::Up] = "1";
    c.keys[NativeButton::Down] = "2";
    c.keys[NativeButton::Left] = "3";
    c.keys[NativeButton::Right] = "4";
    c.keys[NativeButton::L] = "Q";
    c.keys[NativeButton::R] = "Left Shift";
    c.keys[NativeButton::Start] = "Return";
    c.keys[NativeButton::Select] = "Backspace";
    c.keys[NativeButton::ZL] = "Tab";
    c.keys[NativeButton::ZR] = "C";
    c.stick_keys[NativeAnalog::CirclePad] = "W,S,A,D,Left Ctrl";
    c.stick_keys[NativeAnalog::CStick] = "Up,Down,Left,Right";
    return c;
}

namespace {

bool IsOn(const std::string& v) {
    return v == "1" || v == "on" || v == "yes" || v == "true";
}

std::string OnOff(bool v) {
    return v ? "on" : "off";
}

/// A value without its key: "button_a" -> "a" for the template's key list.
std::string ButtonKeyName(int button) {
    return NativeButton::mapping[button];
}

/// Whether a key of `spec` is already bound to a 3DS button, a stick or another of the app's keys.
bool KeyTaken(const Config& c, const std::string& spec) {
    std::vector<int> used;
    const auto add = [&used](std::string bindings) {
        // (a stick's keys are separated by commas)
        std::replace(bindings.begin(), bindings.end(), ',', '|');
        for (const int code : Input::KeyCodes(bindings)) {
            used.push_back(code);
        }
    };
    for (const auto& keys : c.keys) {
        add(keys);
    }
    for (const auto& keys : c.stick_keys) {
        add(keys);
    }
    for (const auto* keys : {&c.item_prev, &c.item_next, &c.stylus_key, &c.touch_screen_key, &c.key_guides_key,
                             &c.fullscreen_key, &c.fps_counter_key}) {
        add(*keys);
    }
    for (const auto& tap : c.touch_taps) {
        add(tap.keys);
    }
    const auto codes = Input::KeyCodes(spec);
    return std::any_of(codes.begin(), codes.end(),
                       [&used](int code) { return std::find(used.begin(), used.end(), code) != used.end(); });
}

} // namespace

std::string ConfigPath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "mh4u-app.ini";
}

Config Load() {
    Config c = Defaults();
    const std::string path = ConfigPath();
    if (!FileUtil::Exists(path)) {
        FileUtil::CreateFullPath(path);
        FileUtil::WriteStringToFile(true, path, Template(c));
        LOG_INFO(Frontend, "wrote {} with the defaults", path);
        return c;
    }
    INIReader ini(path);
    if (ini.ParseError() < 0) {
        LOG_ERROR(Frontend, "couldn't read {}, using the defaults", path);
        return c;
    }
    if (ini.ParseError() > 0) {
        LOG_WARNING(Frontend, "{}: line {} isn't understood", path, ini.ParseError());
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
    c.check_updates = flag("Game", "check_updates", c.check_updates);
    c.new_3ds = str("Game", "new_3ds", c.new_3ds);
    c.cpu_clock = num("Game", "cpu_clock", c.cpu_clock, 5, 400);
    c.fast_forward_speed = num("Game", "fast_forward_speed", c.fast_forward_speed, 0, 1000);

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

    for (int i = 0; i < NativeButton::NumButtons; ++i) {
        c.keys[i] = str("Controls", NativeButton::mapping[i], c.keys[i]);
        c.button_params[i] = str("Controls", ("param_" + ButtonKeyName(i)).c_str(), "");
    }
    for (int i = 0; i < NativeAnalog::NumAnalogs; ++i) {
        c.stick_keys[i] = str("Controls", NativeAnalog::mapping[i], c.stick_keys[i]);
        c.stick_params[i] =
            str("Controls", (std::string("param_") + NativeAnalog::mapping[i]).c_str(), "");
    }
    c.controller_auto = str("Controls", "controller", "auto") == "auto";
    c.touch_screen_key = str("Controls", "touch_screen_key", c.touch_screen_key);
    if (ini.GetInteger("Controls", "controls_version", 1) < 2) {
        // a file from before the keyboard-and-mouse layout: with the old keys untouched it moves
        // to the new ones; keys of the player's own stay
        const Config old = OldDefaults();
        if (c.keys == old.keys && c.stick_keys == old.stick_keys) {
            const Config now = Defaults();
            c.keys = now.keys;
            c.stick_keys = now.stick_keys;
            LOG_INFO(Frontend, "the keys move to the keyboard-and-mouse layout");
        }
        if (c.touch_screen_key == "F12") {
            c.touch_screen_key = Config{}.touch_screen_key;
        }
    }
    c.input_mode = str("Controls", "input_mode", c.input_mode);
    c.mouse_sensitivity = num("Controls", "mouse_sensitivity", c.mouse_sensitivity, 1, 100);
    c.mouse_vertical = num("Controls", "mouse_vertical", c.mouse_vertical, 10, 200);
    c.mouse_invert_y = flag("Controls", "mouse_invert_y", c.mouse_invert_y);
    c.mouse_invert_x = flag("Controls", "mouse_invert_x", c.mouse_invert_x);
    c.mouse_deadzone = num("Controls", "mouse_deadzone", c.mouse_deadzone, 0, 60);
    c.item_prev = str("Controls", "item_prev", c.item_prev);
    c.item_next = str("Controls", "item_next", c.item_next);
    c.stylus_key = str("Controls", "stylus_key", c.stylus_key);
    c.key_guides = str("Controls", "key_guides", c.key_guides);
    c.key_guides_key = str("Controls", "key_guides_key", c.key_guides_key);
    c.fullscreen_key = str("Controls", "fullscreen_key", c.fullscreen_key);
    c.fps_counter_key = str("Controls", "fps_counter_key", c.fps_counter_key);
    c.pause_key = str("Controls", "pause_key", c.pause_key);
    c.fast_forward_key = str("Controls", "fast_forward_key", c.fast_forward_key);
    c.fast_forward_hold = flag("Controls", "fast_forward_hold", c.fast_forward_hold);
    // the touch taps (taps.h), touch_tap_1 to touch_tap_8
    for (int i = 1; i <= Taps::MAX; ++i) {
        const std::string key = fmt::format("touch_tap_{}", i);
        const std::string value = str("Controls", key.c_str(), "");
        if (value.empty()) {
            continue;
        }
        Taps::Tap tap;
        if (!Taps::Parse(value, tap)) {
            LOG_WARNING(Frontend, "{} = {} has no point (keys @ x y): left out", key, value);
            continue;
        }
        c.touch_taps.push_back(std::move(tap));
    }
    // keys new to this file don't take one the player already uses (the old layout had ZL on Tab)
    for (const auto& [key, value] : {std::pair{"pause_key", &c.pause_key},
                                     std::pair{"fast_forward_key", &c.fast_forward_key}}) {
        if (!ini.HasValue("Controls", key) && KeyTaken(c, *value)) {
            LOG_INFO(Frontend, "{} = {} is taken by another binding: no key for it", key, *value);
            value->clear();
        }
    }
    // one key, one input: a tap's keys leave the other bindings
    for (const std::string& line : SettleTouchTaps(c)) {
        LOG_INFO(Frontend, "{}", line);
    }

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
    c.lan_discovery = flag("Multiplayer", "lan_discovery", c.lan_discovery);
    c.last_room = str("Multiplayer", "last_room", c.last_room);

    c.catalog_url = str("Mods", "catalog_url", c.catalog_url);
    LOG_INFO(Frontend, "settings from {}", path);
    return c;
}

void Save(const Config& config) {
    const std::string path = ConfigPath();
    FileUtil::CreateFullPath(path);
    if (!FileUtil::WriteStringToFile(true, path, Template(config))) {
        LOG_ERROR(Frontend, "couldn't write {}", path);
    }
}

Taps::Others TapOthers(Config& c) {
    Taps::Others others;
    for (int i = 0; i < NativeButton::NumButtons; ++i) {
        others.bindings.emplace_back(NativeButton::mapping[i], &c.keys[i]);
    }
    for (const auto& [name, keys] : {std::pair{"item_prev", &c.item_prev}, std::pair{"item_next", &c.item_next},
                                     std::pair{"stylus_key", &c.stylus_key}}) {
        others.bindings.emplace_back(name, keys);
    }
    for (int i = 0; i < NativeAnalog::NumAnalogs; ++i) {
        others.sticks.emplace_back(NativeAnalog::mapping[i], &c.stick_keys[i]);
    }
    // the menu's Esc and the window's own keys, which the game never gets (window.cpp, OnKey)
    others.own = {"Escape",         c.fullscreen_key, c.fps_counter_key,  c.touch_screen_key,
                  c.key_guides_key, c.pause_key,      c.fast_forward_key};
    return others;
}

std::vector<std::string> SettleTouchTaps(Config& config) {
    Taps::Others others = TapOthers(config);
    return Taps::Settle(config.touch_taps, others);
}

std::string Template(const Config& c) {
    std::string keys;
    for (int i = 0; i < NativeButton::NumButtons; ++i) {
        keys += fmt::format("{} = {}\n", NativeButton::mapping[i], c.keys[i]);
    }
    for (int i = 0; i < NativeAnalog::NumAnalogs; ++i) {
        keys += fmt::format("{} = {}\n", NativeAnalog::mapping[i], c.stick_keys[i]);
    }
    std::string params;
    for (int i = 0; i < NativeButton::NumButtons; ++i) {
        if (!c.button_params[i].empty()) {
            params += fmt::format("param_{} = {}\n", NativeButton::mapping[i], c.button_params[i]);
        }
    }
    for (int i = 0; i < NativeAnalog::NumAnalogs; ++i) {
        if (!c.stick_params[i].empty()) {
            params += fmt::format("param_{} = {}\n", NativeAnalog::mapping[i], c.stick_params[i]);
        }
    }
    std::string taps;
    for (int i = 0; i < static_cast<int>(c.touch_taps.size()) && i < Taps::MAX; ++i) {
        taps += fmt::format("touch_tap_{} = {}\n", i + 1, Taps::Format(c.touch_taps[i]));
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
# the launcher looks for a newer release of the port when it opens (it asks GitHub; Settings >
# Updates installs it). off: only when you press Check now
check_updates = {}
# Azahar's New 3DS mode: on, off or auto (off while custom textures are on: the HD texture pack's
# monsters show only then; on otherwise)
new_3ds = {}
# the CPU's clock speed in % of the 3DS's (5-400): above 100 a busy scene keeps a high frame
# rate (fps in mh4u.ini) at the cost of more work for your PC; the same as Azahar's CPU Clock Speed
cpu_clock = {}
# fast_forward_key runs the game this fast while you play alone, in % of the 3DS's speed
# (110-1000; 0 = as fast as your PC can)
fast_forward_speed = {}

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
# controls_version: which layout the keys below follow (2: keyboard and mouse)
controls_version = 2
# input_mode: auto (the keyboard and mouse's way while you use them, the controller's while you
# use it), controller (the mouse is always the stylus) or keyboard_mouse. The keyboard and mouse's
# way: the mouse turns the camera and the key guides show; hold stylus_key to use the mouse on the
# touch screen
input_mode = {}
# how fast the mouse turns the camera (1-100; 50 = 0.12 degrees a count, like a PC game's mouse
# look, with a code library built with the camera hook), up and down in % of left and right
# (10-200), up and down or left and right the other way round, and, where the mouse pushes the
# C-stick instead (an older code library), the least push it gives (0-60, % of the way: the game
# ignores smaller ones)
mouse_sensitivity = {}
mouse_vertical = {}
mouse_invert_y = {}
mouse_invert_x = {}
mouse_deadzone = {}
# the item bar: these scroll it left and right (L held with Y or A, the game's own way)
item_prev = {}
item_next = {}
stylus_key = {}
# the key guides (the keys for what the hunter does, small over the game while you play with the
# keyboard and mouse): off, top-left, top-right, bottom-left or bottom-right; key_guides_key
# shows and hides them
key_guides = {}
key_guides_key = {}
# controller = auto maps the first controller found like MH3U on a Wii U: the 3DS's A, B, X, Y
# on the right face buttons by position, L/R on the bumpers, ZL/ZR on the triggers (target
# camera and kick, see mh4u.ini), the circle pad and C-stick (camera) on the sticks, START and
# SELECT on Start and Back; the right stick's click switches the touch screen. off: keys only.
controller = {}
# keyboard keys by their SDL names (https://wiki.libsdl.org/SDL2/SDL_Scancode) or Mouse Left,
# Mouse Right, Mouse Middle, Mouse 4, Mouse 5, Wheel Up, Wheel Down, several for a 3DS button
# separated by | (F | Mouse Right); a stick takes up,down,left,right and optionally a key that
# makes it move half way. The keys as they come follow Monster Hunter Rise on PC
{}# full Azahar input parameters replace a binding altogether, e.g.
# param_button_a = engine:sdl,guid:...,port:0,api:controller,button:1
{}touch_screen_key = {}
fullscreen_key = {}
fps_counter_key = {}
# playing alone (not in a room): pause_key stops the game where it is and starts it again, and
# fast_forward_key runs it at fast_forward_speed, pressed once to start and once to stop (or only
# while held: fast_forward_hold = on). Empty: no key
pause_key = {}
fast_forward_key = {}
fast_forward_hold = {}
# touch taps: while one of a tap's keys is held, the touch screen is touched at the tap's point
# (x 0-319 from its left edge, y 0-239 from its top edge, in its own pixels), say an item
# shortcut or a panel; a name after the point is what the key guides show. Up to 8, touch_tap_1
# to touch_tap_8, e.g. touch_tap_1 = Z @ 52 220 Mega potion (keys as for a 3DS button, F | Z for
# two; the ; key as |;, since a ; after a space starts a comment). A key on a tap leaves the
# other inputs it was on (the app's own keys above stay theirs); the mouse touching the screen
# comes first
{}
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
# a hosted room answers players on your network who look for rooms (Join a room > On this
# network; UDP port 24873). off: they join it by address or invite
lan_discovery = {}
# the last room joined (its invite code) or hosted ("host"), for the menu's Rejoin last room
last_room = {}

[Mods]
# the launcher's mod catalog (Mods, Catalog) comes with the app's tools; a newer one is fetched
# from here when the launcher starts (empty: never; the copy that came with the app is used)
catalog_url = {}
)",
                       c.game, OnOff(c.launcher), OnOff(c.check_updates), c.new_3ds, c.cpu_clock,
                       c.fast_forward_speed, c.renderer, OnOff(c.fullscreen), c.resolution,
                       OnOff(c.vsync), c.screens, OnOff(c.linear_filter), OnOff(c.custom_textures),
                       c.shader, c.fps_counter, c.volume, c.audio_output, c.input_mode, c.mouse_sensitivity,
                       c.mouse_vertical, OnOff(c.mouse_invert_y), OnOff(c.mouse_invert_x), c.mouse_deadzone, c.item_prev, c.item_next,
                       c.stylus_key, c.key_guides, c.key_guides_key, c.controller_auto ? "auto" : "off", keys,
                       params, c.touch_screen_key, c.fullscreen_key, c.fps_counter_key, c.pause_key,
                       c.fast_forward_key, OnOff(c.fast_forward_hold), taps, c.nickname, c.join, c.password,
                       c.host, c.room_port, c.room_name, c.room_members, c.lobby, c.token,
                       OnOff(c.room_public), OnOff(c.lan_discovery), c.last_room, c.catalog_url);
}

void Apply(const Config& c) {
    auto& v = Settings::values;
    if (c.renderer == "opengl") {
        v.graphics_api = Settings::GraphicsAPI::OpenGL;
    } else if (c.renderer == "software") {
        v.graphics_api = Settings::GraphicsAPI::Software;
    } else {
        v.graphics_api = Settings::GraphicsAPI::Vulkan;
    }
    v.resolution_factor = static_cast<u32>(c.resolution);
    v.use_vsync = c.vsync;
    v.filter_mode = c.linear_filter;
    v.custom_textures = c.custom_textures;
    v.is_new_3ds = c.new_3ds == "auto" ? !c.custom_textures : IsOn(c.new_3ds);
    v.cpu_clock_percentage = static_cast<u32>(std::clamp(c.cpu_clock, 5, 400));
    v.async_custom_loading = true;
    if (!c.shader.empty()) {
        v.pp_shader_name = c.shader;
    }
    v.swap_screen = false;
    v.upright_screen = false;
    if (c.screens == "single") {
        v.layout_option = Settings::LayoutOption::SingleScreen;
    } else if (c.screens == "stacked") {
        v.layout_option = Settings::LayoutOption::Default;
    } else if (c.screens == "side") {
        v.layout_option = Settings::LayoutOption::SideScreen;
    } else if (c.screens == "windows") {
        v.layout_option = Settings::LayoutOption::SeparateWindows;
    } else {
        // the custom layout is the port's overlay when mh4u.ini's touch_overlay is on
        v.layout_option = Settings::LayoutOption::CustomLayout;
    }
    v.volume = static_cast<float>(c.volume) / 100.0f;
    if (c.audio_output == "cubeb") {
        v.output_type = AudioCore::SinkType::Cubeb;
    } else if (c.audio_output == "sdl2") {
        v.output_type = AudioCore::SinkType::SDL2;
    } else if (c.audio_output == "openal") {
        v.output_type = AudioCore::SinkType::OpenAL;
    } else if (c.audio_output == "null") {
        v.output_type = AudioCore::SinkType::Null;
    } else {
        v.output_type = AudioCore::SinkType::Auto;
    }
    v.frame_limit = 100;
    Speed::Get().Configure(c.fast_forward_speed, c.fast_forward_hold);
    // every system module high-level emulated, as Azahar's other frontends set it (Service::Init
    // looks each one up and throws on one that isn't there)
    for (const auto& module : Service::service_module_map) {
        v.lle_modules.emplace(module.name, false);
    }
    Input::BuildProfile(c);
}

} // namespace App
