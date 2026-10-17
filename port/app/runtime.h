// IRuntime: what the app asks of the emulation runtime underneath it. The launcher, the in-game
// menu and main() call these methods instead of the runtime's own headers; the Azahar build
// implements them in runtime_azahar/. The window and renderer integration (EmuWindow, the
// overlay renderers) is not behind it yet.
//
// Threads: the notes say which thread the app calls each method from. In this build the main
// thread and the emulation thread are the same one (the thread that runs main(): it runs
// RunStep in a loop and the menu builds its frames inside it); the windows' present threads and
// the launcher's worker threads only call the ones marked "any thread".
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "util/buttons.h"

namespace Core {
class System;
}

namespace App {

struct Config;
class Window;

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

/// A game file's header: unreadable (not a 3DS game file), readable, or readable but encrypted.
/// `program_id` is set for the last two.
struct GameHeader {
    enum class State { Unreadable, Ok, Encrypted };
    State state = State::Unreadable;
    std::uint64_t program_id = 0;
};

/// A title installed on the SD card. `content_path` is its first content file's path (empty when
/// the title has none); it may not exist on disk.
struct InstalledTitle {
    bool found = false;
    std::uint16_t version = 0;
    std::string content_path;
};

/// What reading or installing a .cia came to.
enum class CiaResult { Ok, Encrypted, Unreadable, Aborted, Invalid };

struct CiaInfo {
    std::uint64_t title_id = 0;
    std::uint16_t version = 0;
};

/// What to run. Every path is a file of the player's.
struct LoadRequest {
    std::string game;          ///< the decrypted game (.3ds, .cci, .cxi)
    std::string update;        ///< a .cia to install first; empty: the update already installed is used
    std::string code_library;  ///< the code library to run; empty: the usual lookup (environment, user folder)
};

/// How loading the game ended.
enum class LoadResult { Ok, NotAGame, Encrypted, MissingSystemFiles, Failed };

/// What one step of the run ended with.
enum class RunState { Running, Paused, ShutdownRequested, Failed };

enum class RuntimeStatus { Idle, Loaded, Running, Paused, Stopped };

/// How the game is running, since the last time it was asked.
struct PerfStats {
    double game_fps = 0.0;         ///< the game's frames per second
    double system_fps = 0.0;       ///< the screens' refreshes per second
    double emulation_speed = 0.0;  ///< emulated time over real time
};

class IRuntime {
public:
    virtual ~IRuntime() = default;

    // ── Lifecycle ──
    // The order is PrepareCode, a window for Load, Load, Start, then RunStep until it says
    // ShutdownRequested or Failed, then Stop. The window and the renderer are still Azahar's:
    // Load takes the app's windows (window.h).

    /// Chooses the CPU backend for the game, before it loads: installs `request.update` when it
    /// has one, makes `request.code_library` the one to run when it has one, and returns a line
    /// for the log saying what was chosen. Main thread.
    virtual std::string PrepareCode(const LoadRequest& request) = 0;

    /// After PrepareCode: a note for the player when the game runs without the port's code
    /// library, or with one that lacks some of the port's hooks, or when the update given to
    /// PrepareCode didn't install. Empty when all is well. Any thread.
    virtual std::string CodeNotice() = 0;

    /// Loads the game, to be drawn in `window` (and its touch screen in `secondary`, if any).
    /// Main thread. On a result other than Ok, StatusDetails() says more.
    virtual LoadResult Load(const LoadRequest& request, Window& window, Window* secondary) = 0;

    /// What the runtime says about the last load or run that went wrong. Any thread.
    virtual std::string StatusDetails() const = 0;

    /// After Load, once the windows' present threads run: the settings of the game's program and
    /// the shader cache. Main thread.
    virtual void Start() = 0;

    /// Runs the game for a slice and returns. While paused it runs nothing and returns Paused
    /// (the windows pump their events inside it, so a paused game's windows don't answer). The
    /// emulation thread.
    virtual RunState RunStep() = 0;

    /// Pauses the game or lets it go on; RunStep notices at its next call. Any thread.
    virtual void SetPaused(bool paused) = 0;

    /// Shuts the emulated system down. The emulation thread, after the last RunStep.
    virtual void Stop() = 0;

    virtual RuntimeStatus Status() const = 0;

    /// The game's speed since the last call (it starts a new measure). The emulation thread.
    virtual PerfStats TakePerfStats() = 0;

    /// Waits for the work the runtime left running in the background. Main thread, at the end.
    virtual void WaitForBackgroundTasks() = 0;

    // ── Input and network layers ──
    // (N0-24 moves them behind the input and multiplayer interfaces)

    /// Starts the input layer (the keyboard and controller engines) after the app's SDL, and
    /// stops it. Main thread.
    virtual void InitInput() = 0;
    virtual void ShutdownInput() = 0;

    /// The input devices read the settings' input profile again (a controller came or went).
    /// Emulation thread.
    virtual void ReloadInputDevices() = 0;

    /// Starts the network layer (rooms) and stops it. Main thread.
    virtual void InitNetwork() = 0;
    virtual void ShutdownNetwork() = 0;

    /// Gives the console a new random ID, kept in the user folder (a room lets in one player
    /// per console ID). Main thread, before the game loads.
    virtual void NewConsoleId() = 0;

    /// Puts what ApplyConfig set in force: the default applets, then the system's settings.
    /// Main thread, before the game loads.
    virtual void CommitOptions() = 0;

    /// Transitional: the Azahar system that main() hands to the windows, the menu layer (ui.h)
    /// and the rooms (multiplayer.h), which still take it. N0-24 and N2b take it away. Null in a
    /// runtime that isn't Azahar's.
    virtual Core::System* HostSystem() = 0;

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

    // ── Titles: the game file, and titles installed on the virtual SD card ──
    // (the launcher's setup; any thread)

    /// What the header of a game file says (.3ds, .cci, .cxi).
    virtual GameHeader ReadGameHeader(const std::string& path) const = 0;

    /// The title installed on the SD card with this id, if there is one.
    virtual InstalledTitle FindInstalledTitle(std::uint64_t title_id) const = 0;

    /// Whether a .cia can be installed, and the title it holds when the file says it.
    virtual CiaResult CheckCia(const std::string& path, std::optional<CiaInfo>& info) const = 0;

    /// Installs a .cia on the SD card, blocking; `progress` is told the bytes written and the
    /// bytes in all (it may be empty, and runs on the calling thread).
    virtual CiaResult InstallCia(const std::string& path,
                                 const std::function<void(std::size_t, std::size_t)>& progress) = 0;
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
