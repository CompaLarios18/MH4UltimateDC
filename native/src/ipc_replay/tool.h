// The command line of mh4n_ipc_replay (N1-11); docs/native/ipc-replay.md is the manual.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mh4n::ipc_replay {

// Runs the tool on `args` (without the program name). Reports go to `out`, errors to `err`.
// Exit status: 0 when nothing differs, 1 when something does, 2 for a usage error, an unreadable or
// invalid input, a --service nobody registered, or a replay in which nothing was replayed.
int RunIpcReplay(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace mh4n::ipc_replay
