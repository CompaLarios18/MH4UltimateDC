#include "tool.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <ostream>

#include "../common/utf8.h"
#include "layeredfs.h"

namespace mh4n::layeredfs {

namespace {

bool ParseNumber(const std::string& text, u64* value) {
    if (text.empty() || text[0] == '-') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

}  // namespace

int RunTool(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    (void)out;
    if (args.size() != 5) {
        err << "usage: mh4n_layeredfs BASE OFFSET SIZE MODS_DIR OUT\n";
        return 1;
    }
    u64 offset = 0, size = 0;
    if (!ParseNumber(args[1], &offset) || !ParseNumber(args[2], &size)) {
        err << "mh4n_layeredfs: OFFSET and SIZE are numbers (decimal or 0x...)\n";
        return 1;
    }

    std::string error;
    auto base = fs::PositionedFile::Open(args[0], &error);
    if (base == nullptr) {
        err << "mh4n_layeredfs: " << error << "\n";
        return 2;
    }
    if (offset > base->Size()) {
        err << "mh4n_layeredfs: OFFSET is past the end of BASE\n";
        return 2;
    }
    if (size == 0) {
        size = base->Size() - offset;
    }

    const std::string romfs_dir = utf8::PathToUtf8(utf8::PathFromUtf8(args[3]) / "romfs");
    const std::string romfs_ext = utf8::PathToUtf8(utf8::PathFromUtf8(args[3]) / "romfs_ext");
    auto image = Build(base, offset, size, romfs_dir, romfs_ext, &error);
    if (image == nullptr) {
        err << "mh4n_layeredfs: " << error << "\n";
        return 2;
    }

    std::ofstream file(utf8::PathFromUtf8(args[4]), std::ios::binary | std::ios::trunc);
    if (!file) {
        err << "mh4n_layeredfs: cannot write " << args[4] << "\n";
        return 2;
    }
    std::vector<u8> chunk(1 << 20);
    const u64 total = image->GetSize();
    for (u64 at = 0; at < total;) {
        std::size_t got = 0;
        image->Read(at, chunk.size(), chunk.data(), &got);
        if (got == 0) {
            err << "mh4n_layeredfs: the image ended early at " << at << "\n";
            return 2;
        }
        file.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(got));
        at += got;
    }
    file.flush();
    if (!file) {
        err << "mh4n_layeredfs: writing " << args[4] << " failed\n";
        return 2;
    }
    return 0;
}

}  // namespace mh4n::layeredfs
