// mh4n_dsp_replay: an MDSP capture replayed through our HLE DSP (N3-04).
// The logic is in native/src/dsp_replay; docs/native/dsp-replay.md is the manual.
#include <iostream>
#include <string>
#include <vector>

#include "dsp_replay/tool.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return mh4n::dsp_replay::RunDspReplay(args, std::cout, std::cerr);
}
