// mh4n_vkprobe: which Vulkan features MH4U's rendering needs a device offers (N2-24).
// The logic is in native/src/vkprobe; docs/research/vkprobe.md is the manual.
#include <iostream>
#include <string>
#include <vector>

#include "../../src/common/utf8.h"
#include "vkprobe/tool.h"

// windows.h last: it defines macros that would break the declarations above
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace {
// The command line as UTF-8 text (D45): main's argv is in the ANSI code page on Windows, so a path with
// an accent would reach the file calls as another name. Empty when Windows cannot split it.
std::vector<std::string> Utf8CommandLine() {
    std::vector<std::string> args;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return args;
    }
    for (int i = 1; i < count; ++i) {
        args.push_back(mh4n::utf8::FromWide(wide[i]));
    }
    LocalFree(wide);
    return args;
}
}  // namespace
#endif

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
#if defined(_WIN32)
    if (const std::vector<std::string> utf8_args = Utf8CommandLine(); !utf8_args.empty() || argc == 1) {
        args = utf8_args;
    }
#endif
    return mh4n::vkprobe::RunVkProbe(args, std::cout, std::cerr);
}
