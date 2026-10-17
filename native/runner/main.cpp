// mh4u_native_runner: the native port's command-line program. N1-09 adds the options, the
// loader and the boot; this file only parses the command line and answers --help and --version.
#include <cstdio>
#include <filesystem>
#include <string>

#include "mh4n/loader.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n_version.h"

MH4N_OPTION("runner", "--help", "", "print this help and exit");
MH4N_OPTION("runner", "--version", "", "print the git revision and exit");
MH4N_OPTION("runner", "--log", "FILTER", "log filter, e.g. \"*:Info Kernel:Debug\" (default \"*:Info\")");

MH4N_OPTION("runner", "--print-process", "PATH",
            "load a game folder or an ELF program, print its process image and segment map, and exit");

namespace {

// --print-process: the loader's view of a program (N1-03). Exit status 2 when loading fails.
int PrintProcess(const std::string& path) {
    std::string error;
    auto loader = std::filesystem::is_directory(path) ? mh4n::Loader::OpenGameFolder(path, &error)
                                                      : mh4n::Loader::OpenElf(path, &error);
    if (!loader) {
        std::fprintf(stderr, "cannot load %s: %s\n", path.c_str(), error.c_str());
        return 2;
    }
    auto memory = mh4n::GuestMemory::Create();
    if (!memory) {
        std::fprintf(stderr, "cannot reserve the guest address space\n");
        return 2;
    }
    mh4n::FcramRegions regions(loader->Image().memory_mode);
    if (!loader->Map(*memory, regions, &error)) {
        std::fprintf(stderr, "cannot map %s: %s\n", path.c_str(), error.c_str());
        return 2;
    }
    std::fputs(mh4n::DescribeProcessImage(loader->Image()).c_str(), stdout);
    return 0;
}

}  // namespace

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
    if (const auto path = options.Get("--print-process")) {
        return PrintProcess(*path);
    }
    MH4N_LOG(Info, "Runner", "nothing to run yet (N1-09)");
    return 0;
}
