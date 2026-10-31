// The environment of the programs the launcher starts, as an AppImage leaves it. On Linux the
// app runs from mh4u.AppImage, whose runtime and AppRun add variables of their own (APPDIR,
// APPIMAGE, ARGV0, OWD) and put folders of its mount in others for the app's sake (the checkrt
// hook's LD_LIBRARY_PATH and LD_PRELOAD); every child process inherits them. The system's own
// programs (Python and the tools, the compiler through them) get the environment without them
// instead: the one the AppImage was started with, but for a variable the AppImage set over the
// player's, which goes (what it was can't be told). Nothing of Azahar's here, so that a test
// builds it on its own (tests/test_app_environment.py).
#pragma once

#include <string>
#include <vector>

namespace Launcher::Environment {

/// Whether `path` is `dir` or a path inside it (both absolute).
bool Inside(const std::string& path, const std::string& dir);

/// `env` ("NAME=value" entries, as `environ` has them) without what an AppImage mounted at one of
/// `mounts` added: the runtime's own variables, and in every other variable the parts inside a
/// mount (a value is a list of parts separated by ':', LD_PRELOAD's also by spaces; a path alone
/// is a list of one). A variable left with no parts goes; the others stay as they were, in their
/// order. Nothing changes when `mounts` is empty.
std::vector<std::string> WithoutAppImage(const std::vector<std::string>& env,
                                         const std::vector<std::string>& mounts);

/// The names of the variables in `before` that aren't in `after` as they were (for the log).
std::vector<std::string> Changed(const std::vector<std::string>& before,
                                 const std::vector<std::string>& after);

} // namespace Launcher::Environment
