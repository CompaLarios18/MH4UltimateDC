// mh4n_capture_player: replays an MGPU capture into a Gpu and checks it against the capture.
// The logic is in native/src/gpu_capture; docs/native/capture-player.md is the manual.
#include <iostream>
#include <string>
#include <vector>

#include "gpu_capture/player.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return mh4n::gpu_capture::RunCapturePlayer(args, std::cout, std::cerr);
}
