// mh4u-native: the native port's app. In this work package only --help and --version; N2b-10
// replaces main with the call to the shell (window, game, UI).
#include <cstdio>
#include <string>

#include "mh4n/options.h"
#include "mh4n_version.h"

MH4N_OPTION("runner", "--help", "", "print this help and exit");
MH4N_OPTION("runner", "--version", "", "print the git revision and exit");

namespace {

constexpr const char* kUsage =
    "usage: mh4u-native [options]\n"
    "options:\n";

}  // namespace

int main(int argc, char** argv) {
    const mh4n::Options options = mh4n::ParseOptions("runner", argc, argv);
    if (!options.ok()) {
        return 2;
    }
    if (options.Has("--help")) {
        const std::string text = kUsage + mh4n::Usage("runner");
        std::fputs(text.c_str(), stdout);
        return 0;
    }
    if (options.Has("--version")) {
        std::printf("mh4u-native %s\n", MH4N_GIT_REVISION);
        return 0;
    }
    const std::string text = kUsage + mh4n::Usage("runner");
    std::fputs(text.c_str(), stderr);
    return 2;
}
