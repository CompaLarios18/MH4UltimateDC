// mh4n_layeredfs: writes a RomFS image with a mods folder laid over it (N3-10).
// The logic is in native/src/layeredfs; docs are in its header (tool.h).
#include <iostream>
#include <string>
#include <vector>

#include "layeredfs/tool.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return mh4n::layeredfs::RunTool(args, std::cout, std::cerr);
}
