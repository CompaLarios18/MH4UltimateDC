// mh4n_ipc_replay: replays an IPC trace against the native services (N1-11).
// The logic is in native/src/ipc_replay; docs/native/ipc-replay.md is the manual.
#include <iostream>
#include <string>
#include <vector>

#include "ipc_replay/tool.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return mh4n::ipc_replay::RunIpcReplay(args, std::cout, std::cerr);
}
