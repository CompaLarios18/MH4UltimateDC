// mh4u_native_runner: the native port's command-line program (docs/native/runner.md). The runtime
// does the work (mh4n/runtime.h); this file parses the command line, answers --help, --version and
// --print-process, prints the program's debug strings and returns Boot's exit status.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/common/utf8.h"
#include "mh4n/loader.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"
#include "mh4n_version.h"

// windows.h last: it defines macros (Yield() is an empty one) that would break the declarations of
// the project headers above it (GuestThread::Yield in mh4n/cpu.h).
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

MH4N_OPTION("runner", "--help", "", "print this help and exit");
MH4N_OPTION("runner", "--version", "", "print the git revision and exit");
MH4N_OPTION("runner", "--log", "FILTER", "log filter, e.g. \"*:Info Kernel:Debug\" (default \"*:Info\")");
MH4N_OPTION("runner", "--print-process", "",
            "load PROGRAM (a game folder or an ELF), print its process image and segment map, and exit");

namespace {

constexpr const char* kUsage =
    "usage: mh4u_native_runner PROGRAM --code LIB [options]\n"
    "PROGRAM is a game folder (tools/native/prepare_game.py) or a 3DS ELF test program.\n"
    "exit status: 0 the program exited or a --ms / --frames limit was reached, 2 loading failed,\n"
    "             3 a runtime error (svcBreak, a fatal error, a bad memory access), 4 the recompiled\n"
    "             code stopped\n"
    "options:\n";

// --print-process: the loader's view of a program (N1-03). Exit status 2 when loading fails.
int PrintProcess(const std::string& path) {
    std::string error;
    auto loader = std::filesystem::is_directory(mh4n::utf8::PathFromUtf8(path))
                      ? mh4n::Loader::OpenGameFolder(path, &error)
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

#if defined(_WIN32)
// The command line as UTF-8 text (D45). The argv that main gets on Windows is in the ANSI code page, which
// has no place for most characters outside it: a folder with an accent or any other non-ASCII character
// would reach the file calls as a different name. The process's own command line is UTF-16; convert that.
// Empty when Windows cannot split it (the caller then keeps main's argv).
std::vector<std::string> Utf8CommandLine() {
    std::vector<std::string> args;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return args;
    }
    for (int i = 0; i < count; ++i) {
        args.push_back(mh4n::utf8::FromWide(wide[i]));
    }
    LocalFree(wide);
    return args;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    const char* const* arguments = argv;
#if defined(_WIN32)
    const std::vector<std::string> utf8_args = Utf8CommandLine();
    std::vector<const char*> utf8_argv;
    for (const std::string& arg : utf8_args) {
        utf8_argv.push_back(arg.c_str());
    }
    if (!utf8_argv.empty()) {
        argc = static_cast<int>(utf8_argv.size());
        arguments = utf8_argv.data();
    }
#endif
    const mh4n::Options options = mh4n::ParseOptions("runner", argc, arguments);
    if (!options.ok()) {
        return 2;
    }
    if (options.Has("--help")) {
        const std::string text = kUsage + mh4n::Usage("runner");
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
    if (options.positional().size() != 1) {
        const std::string text = kUsage + mh4n::Usage("runner");
        std::fputs(text.c_str(), stderr);
        return 2;
    }
    if (options.Has("--print-process")) {
        return PrintProcess(options.positional()[0]);
    }

    mh4n::Runtime runtime;
    // The program's debug strings (svcOutputDebugString) go to stdout, one per line, in the form
    // of the reference runner's log so that the tests' regular expression reads both runners.
    mh4n::SvcHooks print;
    print.debug_string = [](std::string_view text) {
        std::printf("OutputDebugString:0: %.*s\n", static_cast<int>(text.size()), text.data());
        std::fflush(stdout);
    };
    runtime.AddSvcHooks(std::move(print));
    return runtime.Boot(options);
}
