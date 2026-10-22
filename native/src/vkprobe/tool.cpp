#include "vkprobe/tool.h"

#include <fstream>
#include <iterator>
#include <ostream>
#include <sstream>

#include "../common/utf8.h"
#include "mh4n/options.h"
#include "vkprobe/live.h"
#include "vkprobe/report.h"

namespace mh4n::vkprobe {

MH4N_OPTION("vkprobe", "--help", "", "print this help and exit");
MH4N_OPTION("vkprobe", "--json", "", "print one JSON document per device instead of the table");
MH4N_OPTION("vkprobe", "--from-json", "FILE",
            "read `vulkaninfo --json` output (or this program's --json) instead of probing a device");
MH4N_OPTION("vkprobe", "--loader", "PATH", "load this Vulkan loader (or MoltenVK) library instead of the default ones");

namespace {

constexpr const char* kUsage =
    "usage: mh4n_vkprobe [--json] [--from-json FILE] [--loader PATH]\n"
    "Reports which of the Vulkan features MH4U's rendering needs each device offers, and the\n"
    "fallback for each one that is missing: from the Vulkan devices of this machine, or (--from-json)\n"
    "from the output of `vulkaninfo --json`. Exit status: 0 done, 2 bad arguments or file, 3 no Vulkan\n"
    "loader or no device.\n"
    "options:\n";

}  // namespace

int RunVkProbe(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const Options options = ParseOptions("vkprobe", args);
    if (!options.ok()) {
        err << "mh4n_vkprobe: " << options.error() << "\n" << kUsage << Usage("vkprobe");
        return 2;
    }
    if (options.Has("--help")) {
        out << kUsage << Usage("vkprobe");
        return 0;
    }
    if (!options.positional().empty()) {
        err << "mh4n_vkprobe: unexpected argument '" << options.positional()[0] << "'\n" << kUsage << Usage("vkprobe");
        return 2;
    }
    const bool as_json = options.Has("--json");

    std::vector<DeviceReport> reports;
    if (const auto file = options.Get("--from-json")) {
        if (options.Has("--loader")) {
            err << "mh4n_vkprobe: --loader is for the live probe; it cannot be combined with --from-json\n";
            return 2;
        }
        std::ifstream in(utf8::PathFromUtf8(*file), std::ios::binary);
        if (!in) {
            err << "mh4n_vkprobe: cannot open " << *file << "\n";
            return 2;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        if (!ParseReports(text, &reports, &error)) {
            err << "mh4n_vkprobe: " << *file << ": " << error << "\n";
            return 2;
        }
    } else {
        std::vector<std::string> loaders = DefaultLoaderNames();
        if (const auto loader = options.Get("--loader")) {
            loaders = {*loader};
        }
        std::vector<Facts> devices;
        std::string error;
        if (!ProbeLive(loaders, &devices, &error)) {
            err << "mh4n_vkprobe: " << error << "\n"
                << "mh4n_vkprobe: nothing to report; `--from-json FILE` still reads a `vulkaninfo --json` file\n";
            return 3;
        }
        for (const Facts& facts : devices) {
            reports.push_back(Evaluate(facts, "live"));
        }
    }

    bool first = true;
    for (const DeviceReport& report : reports) {
        if (as_json) {
            out << RenderJson(report);
        } else {
            out << (first ? "" : "\n") << RenderTable(report);
        }
        first = false;
    }
    return 0;
}

}  // namespace mh4n::vkprobe
