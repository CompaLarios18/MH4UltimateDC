// mh4u_native_runner: the native port's command-line program. N1-09 adds the options, the
// loader and the boot; this file only parses the command line and answers --help and --version.
#include <cstdio>
#include <string>

#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n_version.h"

MH4N_OPTION("runner", "--help", "", "print this help and exit");
MH4N_OPTION("runner", "--version", "", "print the git revision and exit");
MH4N_OPTION("runner", "--log", "FILTER", "log filter, e.g. \"*:Info Kernel:Debug\" (default \"*:Info\")");

int main(int argc, char** argv) {
    const mh4n::Options options = mh4n::ParseOptions("runner", argc, argv);
    if (!options.ok()) {
        return 2;
    }
    if (options.Has("--help")) {
        const std::string text = "usage: mh4u_native_runner [options]\noptions:\n" + mh4n::Usage("runner");
        std::fputs(text.c_str(), stdout);
        return 0;
    }
    if (options.Has("--version")) {
        std::printf("mh4u_native_runner %s\n", MH4N_GIT_REVISION);
        return 0;
    }
    if (const auto filter = options.Get("--log")) {
        mh4n::LogFilter parsed;
        std::string error;
        if (!parsed.Parse(*filter, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        mh4n::SetLogFilter(parsed);
    }
    MH4N_LOG(Info, "Runner", "nothing to run yet (N1-09)");
    return 0;
}
