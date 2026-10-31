// What the player is told when the game doesn't start, or stops with an error while it runs
// (main.cpp). Azahar gives most of those statuses no details, and the game's own fatal error (what
// shows "An error has occurred" on a 3DS: Azahar's err:f logs its numbers and stops with
// ErrorUnknown) none at all, so these say what the app knows, what to try and where the log is.
// Their lines are short, a sentence or less each: SDL 2's message box breaks lines only where the
// text does, and is as wide as the longest (on X11, and on Windows without version 6 of the common
// controls). No Azahar here (main.cpp passes its statuses as numbers), so the texts are tested on
// their own (tests/test_app_stop_message.py).
#pragma once

#include <string>

namespace App::StopMessage {

/// RunLoop's status for the game's own fatal error: Core::System::ResultStatus::ErrorUnknown
/// (main.cpp checks the number against Azahar's).
constexpr unsigned FATAL_ERROR = 17;

/// A fatal error within this many seconds of the game starting is one "as it started" (#46's came
/// 2.3 s in, as the game opened its first file).
constexpr double START_SECONDS = 30;

/// The game stopped while it ran: RunLoop's `status` and Azahar's `details` of it (often none),
/// `seconds` after the main loop started. `log` is mh4u_log.txt's path, as the player would type it.
std::string Stopped(unsigned status, const std::string& details, double seconds, const std::string& log);

/// The game didn't start: System::Load's `status` and the details (Load sets none itself).
std::string DidntStart(unsigned status, const std::string& details, const std::string& log);

/// The game didn't start because a code mod (code.ips, code.bps) couldn't be applied to its code:
/// System::Load's ErrorLoader_ErrorPatches and ErrorLoader_ErrorPatchesInvalidTitle.
std::string CodeModDoesntFit(const std::string& log);

} // namespace App::StopMessage
