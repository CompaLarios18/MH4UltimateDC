#include "tool.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <ostream>

#include "code_patch.h"

namespace mh4n::code_patch {

int RunTool(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    constexpr const char* kUsage = "usage: mh4n_code_patch --mods DIR --bss-pages N IN OUT\n";
    std::string mods, pages_text;
    std::vector<std::string> files;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mods" && i + 1 < args.size()) {
            mods = args[++i];
        } else if (args[i] == "--bss-pages" && i + 1 < args.size()) {
            pages_text = args[++i];
        } else if (args[i].rfind("--", 0) == 0) {
            err << kUsage;
            return 1;
        } else {
            files.push_back(args[i]);
        }
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long pages = std::strtoull(pages_text.c_str(), &end, 0);
    if (mods.empty() || pages_text.empty() || pages_text[0] == '-' || errno != 0 || *end != '\0' ||
        pages > 0x10000 || files.size() != 2) {
        err << kUsage;
        return 1;
    }

    std::vector<u8> code;
    {
        std::ifstream in(files[0], std::ios::binary);
        if (!in) {
            err << "mh4n_code_patch: cannot read the input\n";
            return 3;
        }
        code.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const Outcome outcome = ModFolder(mods).ApplyToCode(code, static_cast<u32>(pages));
    if (!outcome.ok) {
        err << outcome.error << " (" << outcome.detail << ")\n";
        return 2;
    }
    for (const std::string& name : outcome.applied) {
        out << "applied " << name << "\n";
    }
    std::ofstream file(files[1], std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(code.size()));
    file.flush();
    if (!file) {
        err << "mh4n_code_patch: cannot write the output\n";
        return 3;
    }
    return 0;
}

}  // namespace mh4n::code_patch
