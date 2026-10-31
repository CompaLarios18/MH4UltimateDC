// Child processes for the launcher: the port's Python tools (build_game.py, mods.py) and the
// app's own code check, with their output read as it comes and a way to stop them together
// with whatever they started (a build runs compilers). On Linux a program outside the AppImage
// the app runs from gets the environment without what the AppImage added (launcher/environment.h),
// and every child starts with the default handling of the signals that matter to it. The POSIX
// side needs nothing of Azahar's, so that a test builds it on its own (tests/test_app_process.py).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace Launcher {

class Process {
public:
    Process();
    ~Process();  ///< stops the process if it still runs, and waits for it
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    /// Starts `args` (args[0] is looked for on the PATH when it has no folder), with no input
    /// and its standard output and error in one pipe. False with `error` if it didn't start.
    /// `low_priority`: below the desktop's own programs, and so is all it starts (a build that
    /// takes every core then leaves the desktop and the launcher responsive).
    bool Start(const std::vector<std::string>& args, std::string& error, bool low_priority = false);

    /// Until it has ended and all its output is read.
    bool Running() const;

    /// Stops it and the processes it started (best effort); Running() turns false soon after.
    void Stop();
    bool WasStopped() const;

    /// After the end: its exit status (-1 when it was stopped or died of a signal).
    int ExitCode() const;

    /// After the end: the signal it died of, 0 if none (and when it was stopped; always on Windows).
    int Signal() const;

    /// The output so far.
    std::string Output() const;

    /// The output so far as lines, the last one perhaps unfinished; a carriage return starts
    /// its line over (the way a terminal shows a download's progress).
    std::vector<std::string> Lines() const;

    /// Waits for the end and returns the exit status.
    int Wait();

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

/// Runs `args` to the end (on a worker thread: it blocks) and returns its exit status, -1 if
/// it didn't start (`output` then says why).
int RunToEnd(const std::vector<std::string>& args, std::string& output);

/// Starts `args` on its own, to go on after this program ends (the app again after an update).
bool StartDetached(const std::vector<std::string>& args, std::string& error);

/// This program's own executable, to run it again in a process of its own.
std::string SelfPath();

/// When this app runs from an AppImage (Linux): the variables that the programs it starts outside
/// the AppImage don't get as the AppImage left them, for the log; "" otherwise.
std::string ChildEnvironmentNote();

} // namespace Launcher
