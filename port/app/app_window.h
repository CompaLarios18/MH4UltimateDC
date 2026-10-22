// The window as the app's shared sources (ui/, launcher/) see it: what the in-game menu asks of
// the window it is drawn over, and no more. Each executable implements it with its own window
// (the Azahar app's App::Window in window.h; mh4u-native's later), so that the shared sources
// include neither Azahar's frontend headers nor an SDL header.
#pragma once

// (opaque here: each executable's own SDL header defines it)
struct SDL_Window;

namespace App {

/// A size in whole units: window points or pixels.
struct WindowSize {
    int w = 0;
    int h = 0;
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

    /// The SDL window behind it, for Dear ImGui's platform backend.
    virtual SDL_Window* SdlWindow() const = 0;
};

} // namespace App
