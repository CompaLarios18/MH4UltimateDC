// mh4n_hdpack: converts an HD texture pack into one BC7 archive (N2-17).
// The logic is in native/src/hdpack; docs/native/hdpack.md is the manual.
#include <iostream>
#include <string>
#include <vector>

#include "hdpack/tool.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return mh4n::hdpack::RunHdPack(args, std::cout, std::cerr);
}
