// The app's own settings ("mh4u-app.ini" in Azahar's user folder): which game file to open,
// the renderer and the window, the screens, sound, controls and multiplayer. The port's game
// options (frame rate, widescreen, the touch overlay, ...) stay in "mh4u.ini" (port/src/hooks.h).
#pragma once

#include <array>
#include <string>

#include "common/settings.h"

namespace App {

struct Config {
    // [Game]
    std::string game;  ///< the decrypted game (.3ds, .cci, .cxi); empty: ask
    bool launcher = true;  ///< the launcher first (off: only while the setup is incomplete)

    // [Display]
    std::string renderer = "vulkan";  ///< vulkan, opengl or software
    bool fullscreen = true;
    int resolution = 0;               ///< internal resolution, times the 3DS's; 0: from the window
    bool vsync = true;
    /// The screens: overlay (the game fills the window, the touch screen over it in a corner as
    /// mh4u.ini's touch_overlay says), single (the game alone), stacked (as on a 3DS), side (side
    /// by side), windows (the touch screen in a window of its own)
    std::string screens = "overlay";
    bool linear_filter = true;        ///< smooth the picture when it is scaled
    bool custom_textures = false;     ///< texture packs in load/textures/<program id>/
    std::string shader;               ///< a post-processing shader, empty: none

    // [Audio]
    int volume = 100;                 ///< 0-100
    std::string audio_output = "auto";

    // [Controls]
    /// Per 3DS button (Settings::NativeButton) and stick (Settings::NativeAnalog): keyboard keys
    /// by SDL name ("W", "Left Shift"), and for a stick its four keys and the key that halves it
    /// ("W,S,A,D,Left Shift"). A controller is mapped on its own (controller = auto) like the
    /// Wii U's MH3U: the 3DS's A on the controller's right face button, ZL/ZR on the triggers,
    /// the circle pad and C-stick on the sticks.
    std::array<std::string, Settings::NativeButton::NumButtons> keys;
    std::array<std::string, Settings::NativeAnalog::NumAnalogs> stick_keys;
    bool controller_auto = true;
    /// Azahar input parameters that replace a button's or stick's binding altogether (as the
    /// Qt frontend writes them), empty: keys + the controller
    std::array<std::string, Settings::NativeButton::NumButtons> button_params;
    std::array<std::string, Settings::NativeAnalog::NumAnalogs> stick_params;
    std::string touch_screen_key = "F12";  ///< in quests: the map / the whole touch screen
    std::string fullscreen_key = "F11";

    // [Multiplayer]
    std::string nickname;             ///< empty: the console's user name
    std::string join;                 ///< HOST[:PORT]: join that room before the game starts
    std::string password;
    int host = 0;                     ///< a port: host a room there (and join it)
    std::string room_name = "MH4U";
    int room_members = 16;
    std::string lobby;                ///< a lobby server (server/lobby): the hosted room is listed
    std::string token;                ///< the token for that lobby
    bool room_public = true;          ///< list the hosted room in the lobby (with lobby and token)
};

/// The settings file's path.
std::string ConfigPath();

/// Reads the settings file; one that doesn't exist yet is written with the defaults, explained.
Config Load();

/// Writes the settings back (the game's path after it was chosen, say), keeping the comments.
void Save(const Config& config);

/// Azahar's settings for these choices (renderer, screens, sound, input profile).
void Apply(const Config& config);

/// What a new settings file says.
std::string Template(const Config& config);

} // namespace App
