// Crash files and tester reports of mh4u-native (N3-13, D69, docs/native/alpha.md).
//
// A crash file is written by a signal handler (POSIX) or an unhandled-exception filter (Windows)
// into <user>/log/crash-<pid>.txt, which Install opened in advance. A report is a local text file
// the tester reads and attaches to an issue himself: nothing here sends anything anywhere.
// What a report holds and never holds is the table in docs/native/alpha.md; tools/native/
// report_check.py is the gate that refuses a report that breaks it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mh4n::app::report {

/// The first line of every report.
inline constexpr const char* kMarker = "mh4u-native alpha report v1";

/// What the caller (the shell, N3-30) knows about the PC. Empty strings and zeros are written as
/// "unknown".
struct SystemInfo {
    std::string os;               ///< "Windows", "Linux", "macOS"
    std::string os_version;       ///< "10.0.22631", "6.8.0"
    std::string cpu_model;
    unsigned logical_cores = 0;
    std::uint64_t ram_bytes = 0;
    std::string gpu_name;
    std::string driver_version;
    std::string vulkan_version;
    std::vector<std::string> display_modes;  ///< "3440x1440@120", one per mode
};

struct MakeInput {
    std::string user_dir;
    std::string app_commit;       ///< MH4N_GIT_REVISION of the caller
    SystemInfo system;
    /// Names to hide: names[0] is the OS user name (shown as <os-user>), names[1] the console's
    /// user name (<console-user>), any further name is shown as <name>; an empty entry is skipped.
    /// Make fills an empty names[0] and names[1] itself from CollectNames.
    std::vector<std::string> names;
    std::string last_status;      ///< the last run's status line, or empty
};

/// Installs the crash handlers and opens <user_dir>/log/crash-<pid>.txt (which an orderly exit
/// removes again while it is empty). Called before Runtime::Boot, so that the memory module's
/// fault handler, installed at boot, chains to ours. POSIX: SIGSEGV, SIGBUS, SIGILL, SIGFPE and
/// SIGABRT, each handler calling the previous one after writing; Windows:
/// SetUnhandledExceptionFilter. Calling it again moves the crash file to the new folder.
/// Nothing in the handler allocates, formats with fmt or logs. An empty `user_dir` installs nothing.
void Install(const std::string& user_dir);

/// Publishes the game's frame number and scene value for the crash file (two atomics).
void SetGameState(std::uint64_t frame, std::uint64_t scene);

/// The names to hide for this user: {the OS user name from the environment, the console's user
/// name}, each empty when unknown. The console's name is read from the config savegame only when
/// <user_dir> has one; the file is never created.
std::vector<std::string> CollectNames(const std::string& user_dir);

/// Writes the report to `out_path` (its folder is made). False when it could not be written.
bool Make(const MakeInput& input, const std::string& out_path);

/// `text` with every absolute path, the given names, console ids, MAC addresses, IPv4 and IPv6
/// addresses and mh4u:// invites replaced. `user_dir` and `home_dir` (empty: the environment's
/// HOME or USERPROFILE) are shown as <user> and <home> where they stand alone; a longer path
/// under them is <path>.
std::string Redact(const std::string& text, const std::vector<std::string>& names,
                   const std::string& user_dir = {}, const std::string& home_dir = {});

/// True when <user_dir>/log/ has a crash-*.txt (not empty) newer than the newest report-*.txt.
bool Pending(const std::string& user_dir);

}  // namespace mh4n::app::report
