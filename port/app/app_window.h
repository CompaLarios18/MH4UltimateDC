// The window and the host as the app's shared sources (ui/, input.cpp, config.cpp, ...) see
// them: what the in-game menu asks of the window it is drawn over, the events the window hands
// it, and the few services of the host (time, clipboard, cursor, key names, message boxes,
// controllers, the window icon). Each executable implements them with its own SDL: the Azahar
// app's App::Window (window.h) and host_sdl2.cpp, mh4u-native's later, so that the shared sources
// include neither Azahar's frontend headers nor an SDL header.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// (opaque here: each executable's own SDL header defines it)
struct SDL_Window;

namespace App {

/// A size in whole units: window points or pixels.
struct WindowSize {
    int w = 0;
    int h = 0;
};

/// A position in window points.
struct Point {
    int x = 0;
    int y = 0;
};

// ── Input events ──
// SDL2's numbering, whichever SDL a window runs on: the window translates its own events into
// it. The few constants the shared code names are here, with SDL2's values.

namespace Scancode {
inline constexpr int Return = 40;
inline constexpr int Escape = 41;
inline constexpr int Delete = 76;
/// SDL2's scancodes end at 512; the codes past it are input.h's (the mouse, the wheel, the macro).
inline constexpr int Count = 512;
} // namespace Scancode

/// InputEvent::mods: SDL2's KMOD_* bits (a left and a right key each).
namespace Mod {
inline constexpr unsigned Shift = 0x0003;
inline constexpr unsigned Ctrl = 0x00C0;
inline constexpr unsigned Alt = 0x0300;
inline constexpr unsigned Gui = 0x0C00;
} // namespace Mod

/// InputEvent::button of a mouse button (SDL_BUTTON_*).
namespace MouseButton {
inline constexpr int Left = 1;
inline constexpr int Middle = 2;
inline constexpr int Right = 3;
inline constexpr int X1 = 4;
inline constexpr int X2 = 5;
} // namespace MouseButton

/// InputEvent::button of a controller's button (SDL_CONTROLLER_BUTTON_*).
namespace PadButton {
inline constexpr int Back = 4;
inline constexpr int Guide = 5;
inline constexpr int Start = 6;
} // namespace PadButton

/// One event of the window, as the shared sources read it. A field means what it says only for
/// the types that use it (the comment on each).
struct InputEvent {
    enum class Type {
        KeyDown,        ///< scancode, repeat, mods
        KeyUp,          ///< scancode, repeat, mods
        Text,           ///< text (UTF-8)
        TextEditing,    ///< an input method's composition in progress (nothing else is read of it)
        MouseMotion,    ///< x, y (window points), dx, dy (counts moved), from_touch
        MouseButton,    ///< button (MouseButton::*), down, x, y, from_touch
        Wheel,          ///< wheel_x, wheel_y (notches, up and right positive), from_touch
        PadButton,      ///< pad (instance id), button (PadButton::* or any other of SDL2's), down
        PadAxis,        ///< pad, axis (SDL2's numbering), value (-32768..32767)
        PadAdded,       ///< pad
        PadRemoved,     ///< pad
        FocusLost,      ///< the window lost the keyboard focus
        Resized,        ///< the window's size, its display or its state changed
        CloseRequested, ///< the window's close button
        Quit,           ///< the application was asked to quit
        Touch,          ///< a finger on a touch screen (the stylus of the game): nothing else
        Other,          ///< an event of the window system the shared sources don't look at
    };

    Type type = Type::Other;
    int scancode = 0;
    bool repeat = false;
    unsigned mods = 0;
    std::string text;
    int x = 0, y = 0;
    int dx = 0, dy = 0;
    int button = 0;
    bool down = false;
    /// The mouse event came from a touch screen (SDL's touch mouse id), not from a mouse.
    bool from_touch = false;
    int wheel_x = 0, wheel_y = 0;
    std::int32_t pad = 0;
    int axis = 0;
    int value = 0;
};

class IWindow {
public:
    virtual ~IWindow() = default;

    /// False once the window was closed or asked to close.
    virtual bool IsOpen() const = 0;
    /// Closes the window, which ends the run as when the player closes it.
    virtual void RequestClose() = 0;

    virtual void SetFullscreen(bool on) = 0;
    virtual bool IsFullscreen() const = 0;

    /// The input profile is built again as if a controller had come (the in-game menu changed
    /// the controls), and the window's own keys are read again.
    virtual void RequestInputReload() = 0;

    /// Whether the keyboard and mouse's way is on: the mouse turns the camera, and the key
    /// guides show.
    virtual bool KeyboardMouseActive() const = 0;

    /// The touch screen's stylus is let go (the menu took the mouse).
    virtual void ReleaseTouch() = 0;

    /// The window's size in the units its events use (SDL's window coordinates) and in drawable
    /// pixels; they differ on high-DPI screens.
    virtual WindowSize SizeInPoints() const = 0;
    virtual WindowSize SizeInPixels() const = 0;

    /// The SDL window behind it (for the host's message boxes and the like).
    virtual SDL_Window* SdlWindow() const = 0;

    // ── Dear ImGui's platform backend, which the window owns (the menu's context is current
    // and its lock is held when the menu calls these) ──

    /// Starts the backend for this window, with every controller driving the menu's navigation.
    virtual void ImGuiInit() = 0;
    /// Stops it, before the menu's context is destroyed.
    virtual void ImGuiShutdown() = 0;
    /// The backend's start of a frame: the display size, the time, the mouse and the controllers.
    virtual void ImGuiNewFrame() = 0;
    /// Gives the backend the event the window is translating now, while the window hands it to
    /// InputEvent's reader (Ui::OnEvent); a no-op outside of that.
    virtual void ImGuiForwardCurrentEvent() = 0;
};

// ── The host ──
// Services that depend on the executable's SDL; each executable defines them (the Azahar app's
// are in host_sdl2.cpp and input_sdl2.cpp, the unit tests' in util/tests/fake_host.cpp).

/// A game controller the app found, as the input profile names it: the SDL GUID, its number
/// among controllers with that GUID, and its name.
struct ControllerInfo {
    std::string guid;
    int port = 0;
    std::string name;
};

namespace Host {

/// Milliseconds on a clock that only goes forward (SDL's ticks); wraps around after 49 days.
std::uint32_t Ticks();

/// The system clipboard's text ("" when it holds none) and a new text for it.
std::string GetClipboard();
void SetClipboard(const std::string& text);

/// The mouse cursor over the app's windows.
void ShowCursor(bool show);

/// The mouse's position in the focused window, in its points.
Point MousePosition();

/// The scancode of a key by its SDL name ("W", "Left Shift", "F12"), 0 when there is no such key,
/// and the name of a scancode ("" when it has none or is out of range).
int ScancodeFromName(const std::string& name);
std::string ScancodeName(int scancode);

/// A message box, over `parent` when there is one (it may be called before any window).
void ShowMessageBox(const std::string& title, const std::string& text, bool error, IWindow* parent = nullptr);

/// The game controllers plugged in now, in SDL's order.
std::vector<ControllerInfo> ListControllers();

/// Gives the window the app's icon.
void SetWindowIcon(IWindow& window);

} // namespace Host
} // namespace App
