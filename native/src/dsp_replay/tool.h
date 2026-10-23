// The command line of mh4n_dsp_replay (N3-04); docs/native/dsp-replay.md is the manual.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mh4n::dsp_replay {

// Runs the tool on `args` (without the program name): `CAPTURE.mdsp [--report FILE]`. The lines of the
// result go to `out`, errors to `err`. Exit status: 0 PASS, 1 FAIL, 2 a usage error or a capture that
// cannot be read.
int RunDspReplay(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace mh4n::dsp_replay
