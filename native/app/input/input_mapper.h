// The input of mh4u-native (N3-07, docs/native/input.md): the keyboard, the mouse and controllers
// at the same time on every 3DS button, a controller mapped by itself as in `mh4u`, the keyboard
// and mouse mode with the mouse as the camera, and the in-game menu's mute, as one
// mh4n::InputState for the live input source (input_live::LiveInput::Set).
//
// It is the Azahar app's input (port/app/window.cpp's decisions, input.h's key names and kbm.h's
// mouse arithmetic, runtime_azahar/azahar_input.cpp's profile) without Azahar's input engine: the
// window's events come in as App::InputEvent, the controllers are known through a small interface
// (a test brings its own), and what the game reads goes out as a state.
//
// Threads: the main thread only (the events, Tick and the setters); on_state and on_camera_turn
// are called from inside those calls.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_window.h"
#include "config.h"
#include "mh4n/host.h"

namespace mh4n::app {

/// A controller as the layer knows it (a gamepad: SDL has a mapping for it).
struct PadInfo {
    std::int32_t id = 0;  ///< the instance id the window's events carry (InputEvent::pad)
    std::string guid;     ///< SDL's GUID string, as button_params and the profile name it
    int port = 0;         ///< how many controllers before it share its GUID
    std::string name;     ///< may be empty
};

/// The controllers plugged in now, in SDL's order. The mapper asks again whenever a controller
/// comes or goes (and when it is configured); the layer makes sure the window's events for the
/// ones it lists arrive (SDL needs a gamepad opened for that).
class IGamepadLayer {
public:
    virtual ~IGamepadLayer() = default;
    virtual std::vector<PadInfo> List() = 0;
};

/// The layer on SDL3's gamepad API: opens every gamepad it lists, closes the ones that went. SDL's
/// joystick and gamepad subsystems must be initialised (the window does it) before List is called.
std::unique_ptr<IGamepadLayer> MakeSdlGamepads();

class InputMapper {
public:
    /// `pads` outlives the mapper; null: no controllers.
    explicit InputMapper(IGamepadLayer* pads);
    ~InputMapper();
    InputMapper(const InputMapper&) = delete;
    InputMapper& operator=(const InputMapper&) = delete;

    /// Reads the settings: the keys, the stylus key and the hotkeys, the parameters that replace a
    /// binding, the mode and the mouse's, and picks the controller (controller_auto: the first
    /// one). Call it again when the settings change (the menu's controls page): what is held
    /// stays held, and the mode the player is in stays.
    void Configure(const App::Config& config);

    /// One event of the window (the ones the in-game menu did not take). True: the event belongs
    /// to input (a key or a controller's, a mouse event while the mouse is the camera) and the
    /// shell does not use it; false: the shell's (the app's hotkeys, the window's events, and the
    /// mouse's while it is the stylus: those are touches). Press and release of the mouse's
    /// buttons, the wheel and its motion count only while the mouse is the camera.
    bool OnEvent(const App::InputEvent& event);

    /// Once per loop, after the events, with a clock that only goes forward (seconds): the mouse
    /// as the camera is decided, the mouse's push on the C-stick and the timed presses (the item
    /// bar's L+Y / L+A, the wheel's keys) go on.
    void Tick(double now_s);

    /// What the game reads now.
    mh4n::InputState State() const;

    /// The in-game menu is up: every button reads released and every stick centred; once it is
    /// gone, a button still held reads released until it is let go, a stick still pushed reads
    /// centred until it has come back. The mouse stops being the camera while it is up.
    void SetMuted(bool muted);

    /// The mouse is the touch screen's stylus (the shell reads its events as touches): it is not
    /// the camera. Refreshed by Tick.
    bool MouseIsStylus() const;
    /// The window should be in relative mouse mode: the mouse is the camera (the keyboard and
    /// mouse mode is on, the window has the focus, the stylus key is not held, the menu is not
    /// up). Refreshed by Tick; the shell lets go of a touch when it turns on.
    bool WantsRelativeMouse() const;

    /// The game's camera turns by the mouse itself (the port's camera hook runs): the mouse's
    /// motion then goes to on_camera_turn instead of pushing the C-stick. Whether the hook runs
    /// is the runtime's to say (N3-14); this class never asks the hooks.
    void SetCameraTurnActive(bool active);

    /// `yaw` degrees to the right and `pitch` up, for each motion of the mouse while it is the
    /// camera and the camera turn is active.
    std::function<void(float yaw, float pitch)> on_camera_turn;
    /// Called when State() changed (after an event, Tick, SetMuted or Configure).
    std::function<void(const mh4n::InputState&)> on_state;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mh4n::app
