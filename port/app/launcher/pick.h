// The system's own file and folder pickers for the launcher (a game file, the update's .cia, a
// mod's folder or .zip): Windows' and macOS's dialogs, zenity or kdialog on Linux. A dialog can be
// missing, fail or not show (Steam's Game Mode has no desktop for it): the launcher's own browser
// (browse.h) takes over then, and what the dialog said goes to the log.
#pragma once

#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace Launcher::Pick {

/// What a picker is asked for.
struct Request {
    std::string title;
    std::string kind;                     ///< the files' kind, for a dialog's filter ("CIA files")
    std::vector<std::string> extensions;  ///< the files that will do ("cia", "3ds", ...)
    /// A folder; the launcher's own browser then also takes a file with one of `extensions`
    bool folder = false;
    std::string start;   ///< the folder it opens in ("": the dialog's own choice)
    std::string select;  ///< a file or folder in `start` to show first (the one chosen last time)
};

struct Result {
    std::string path;       ///< empty if cancelled
    bool available = true;  ///< false: this system has no dialog (zenity or kdialog)
    std::string program;    ///< the dialog (zenity, kdialog, osascript, Windows'), for the log
    std::string error;      ///< not empty: the dialog didn't work (it crashed or never showed), and why
};

/// The launcher's window, which the dialogs belong to (Windows).
void SetOwner(SDL_Window* window);

/// The system has a dialog to offer (on Linux, zenity or kdialog is installed).
bool Available();

/// Steam's Game Mode or Big Picture (gamescope): a desktop's dialog may not show there, and it
/// doesn't take a controller.
bool GameMode();

/// The system's dialog for one request. Windows' is modal: Start returns once it has closed.
/// Elsewhere it is another program, which Poll follows without waiting, so that the launcher
/// draws on (and keeps answering the desktop) while it is up.
class Dialog {
public:
    Dialog();
    ~Dialog();  ///< ends the dialog's program if it still runs
    Dialog(const Dialog&) = delete;
    Dialog& operator=(const Dialog&) = delete;

    void Start(const Request& request);
    /// True once the dialog has closed and Get() has what came of it.
    bool Poll();
    /// Ends the dialog's program: Poll() turns true soon after, without a path.
    void Cancel();
    const Result& Get() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Launcher::Pick
