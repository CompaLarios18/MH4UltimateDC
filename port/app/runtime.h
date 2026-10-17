// IRuntime: what the app asks of the emulation runtime underneath it. The launcher, the in-game
// menu and main() call these methods instead of the runtime's own headers; the Azahar build
// implements them in runtime_azahar/. The window and renderer integration (EmuWindow, the
// overlay renderers) is not behind it yet.
//
// Threads: the methods say which thread may call them. "Emulation thread" is the thread that
// runs RunStep (main()'s loop), "main thread" is the one that started the app. The launcher's
// worker threads only call the ones marked "any thread".
#pragma once

#include <memory>
#include <string>

#include "buttons.h"

namespace App {

struct Config;

/// The graphics API the game is drawn with.
enum class Renderer { Vulkan, OpenGL, Software };

/// Where the two 3DS screens go. Overlay is the game filling the window with the touch screen
/// over it in a corner (mh4u.ini's touch_overlay says where). Other is a layout of the runtime's that the app never sets.
enum class ScreenLayout { Overlay, Stacked, Single, Side, SeparateWindows, Other };

/// The renderer a settings-file name stands for (vulkan, opengl, software); any other is Vulkan.
inline Renderer RendererFor(const std::string& name) {
    if (name == "opengl") {
        return Renderer::OpenGL;
    }
    if (name == "software") {
        return Renderer::Software;
    }
    return Renderer::Vulkan;
}

/// The layout a settings-file name stands for (single, stacked, side, windows); any other is the
/// overlay.
inline ScreenLayout ScreenLayoutFor(const std::string& name) {
    if (name == "single") {
        return ScreenLayout::Single;
    }
    if (name == "stacked") {
        return ScreenLayout::Stacked;
    }
    if (name == "side") {
        return ScreenLayout::Side;
    }
    if (name == "windows") {
        return ScreenLayout::SeparateWindows;
    }
    return ScreenLayout::Overlay;
}

/// The runtime's build: `name` is its version ("Azahar 2123.3"), `branch` and `description` say
/// which commit of it this is.
struct BuildInfo {
    std::string name;
    std::string branch;
    std::string description;
};

class IRuntime {
public:
    virtual ~IRuntime() = default;

    // ── Paths ──

    /// The user folder, with the separator at its end: where the settings, the log, the options
    /// and the code library are. Any thread.
    virtual std::string UserDir() const = 0;

    /// Another user folder (--user), before anything reads or writes in the old one. Main thread.
    virtual void SetUserDir(const std::string& dir) = 0;

    /// The virtual SD card, with the separator at its end: the saves and the installed update.
    virtual std::string SdmcDir() const = 0;

    /// The game's options file (mh4u.ini) and what a new one says. Any thread.
    virtual std::string GameOptionsPath() const = 0;
    virtual std::string GameOptionsTemplate() const = 0;

    // ── Build and log ──

    virtual BuildInfo Build() const = 0;

    /// Starts the log: `file_name` in the user folder, and the console. `filter` is the log
    /// filter ("*:Info Render.OpenGL:Debug"); empty: the runtime's own setting. Main thread,
    /// before other threads log.
    virtual void StartLogging(const std::string& file_name, const std::string& filter) = 0;

    /// Writes what is queued and stops the log.
    virtual void StopLogging() = 0;

    /// The runtime's settings, into the log.
    virtual void LogOptions() = 0;

    // ── Options ──

    /// The runtime's settings for the app's: renderer, screens, sound, input profile, New 3DS
    /// mode, CPU clock, and the frame-time log. Main thread, before the game loads.
    virtual void ApplyConfig(const Config& config) = 0;

    /// The renderer in use, and the one the next window is made for.
    virtual Renderer ActiveRenderer() const = 0;
    virtual void SetActiveRenderer(Renderer renderer) = 0;

    /// The screen layout the runtime has. SetLayout only changes the setting (before the game
    /// loads); ApplyLayout also puts the screens in place at once. Emulation thread.
    virtual ScreenLayout CurrentLayout() const = 0;
    virtual void SetLayout(ScreenLayout layout) = 0;
    virtual void ApplyLayout(ScreenLayout layout) = 0;

    /// Display and sound, while the game runs. Emulation thread, or the main thread while the
    /// game isn't running.
    virtual void SetResolution(int factor) = 0;       ///< times the 3DS's; 0: from the window
    virtual void SetLinearFilter(bool on) = 0;
    virtual void SetVsync(bool on) = 0;
    virtual void SetVolume(int percent) = 0;          ///< 0-100
    virtual void SetCpuClock(int percent) = 0;        ///< % of the 3DS's

    /// Changes one of the game's options (mh4u.ini keys) while the game runs; false for a key
    /// the port's hooks read only at start. Emulation thread.
    virtual bool SetGameOption(const std::string& key, const std::string& value) = 0;

    /// The HUD layout editor of the menu is up or gone. Emulation thread.
    virtual void SetLayoutEditing(bool on) = 0;

    // ── The 3DS's buttons ──

    /// The key of a button or a stick in the settings file ("button_a", "circle_pad").
    virtual const char* ButtonSettingName(int button) const = 0;
    virtual const char* StickSettingName(int stick) const = 0;

    /// The input the runtime reads a button or a stick from, as its own parameter string, empty
    /// when it has none.
    virtual std::string ButtonBinding(int button) const = 0;
    virtual std::string StickBinding(int stick) const = 0;

    /// What a controller does for such a parameter string ("A (bottom)", "Left stick"), or empty
    /// when no controller does.
    virtual std::string DescribeControllerInput(const std::string& binding) const = 0;
};

/// The runtime the app runs on. Set once by main() before anything else uses it.
inline IRuntime*& RuntimeSlot() {
    static IRuntime* runtime = nullptr;
    return runtime;
}
inline void SetRuntime(IRuntime* runtime) {
    RuntimeSlot() = runtime;
}
inline IRuntime& Runtime() {
    return *RuntimeSlot();
}

} // namespace App
