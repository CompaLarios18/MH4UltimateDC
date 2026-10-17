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

namespace App {

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
