// mh4n_code_patch: applies a mods folder to a code file (N3-09). The logic is in native/src/code_patch;
// the command line and exit statuses are documented in its header (tool.h).
#include <iostream>
#include <string>
#include <vector>

#include "code_patch/tool.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return mh4n::code_patch::RunTool(args, std::cout, std::cerr);
}
