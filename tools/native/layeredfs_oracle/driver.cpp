// The driver of the LayeredFS oracle (N3-10): Azahar's own LayeredFS and Patch code, compiled from its
// sources (layeredfs_oracle.py), run on files.
//
//     layeredfs_driver layered BASE MODS_DIR OUT
//         BASE is a level-3 RomFS image. MODS_DIR/romfs and MODS_DIR/romfs_ext are laid over it as Azahar does
//         (the paths with a trailing '/', as the file system service passes them) and the whole layered image
//         is read through LayeredFS::ReadFile and written to OUT. Exit 3 if Azahar's code throws.
//     layeredfs_driver patch KIND BUFFER PATCH OUT
//         KIND is ips or bps. The patch is applied to the contents of BUFFER with Patch::ApplyIpsPatch or
//         Patch::ApplyBpsPatch and the buffer written to OUT; exit 2 when Azahar's code says it is an error.
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/file_sys/layered_fs.h"
#include "core/file_sys/patch.h"
#include "core/file_sys/romfs_reader.h"

static std::vector<u8> ReadWhole(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool WriteWhole(const std::string& path, const u8* data, std::size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string(argv[1]) == "layered") {
            auto base = std::make_shared<FileSys::DirectRomFSReader>(argv[2]);
            if (!base->IsOpen()) {
                std::fprintf(stderr, "cannot open %s\n", argv[2]);
                return 1;
            }
            const std::string mods = argv[3];
            FileSys::LayeredFS layered(base, mods + "/romfs/", mods + "/romfs_ext/");
            const std::size_t size = layered.GetSize();
            std::vector<u8> image(size);
            std::size_t done = 0;
            while (done < size) {
                const std::size_t chunk = std::min<std::size_t>(size - done, 1 << 20);
                layered.ReadFile(done, chunk, image.data() + done);
                done += chunk;
            }
            return WriteWhole(argv[4], image.data(), image.size()) ? 0 : 1;
        }
        if (argc == 6 && std::string(argv[1]) == "patch") {
            bool ok1 = false, ok2 = false;
            std::vector<u8> buffer = ReadWhole(argv[3], &ok1);
            const std::vector<u8> patch = ReadWhole(argv[4], &ok2);
            if (!ok1 || !ok2) {
                std::fprintf(stderr, "cannot read the inputs\n");
                return 1;
            }
            const std::string kind = argv[2];
            if (kind != "ips" && kind != "bps") {
                std::fprintf(stderr, "KIND is ips or bps\n");
                return 1;
            }
            const auto status = kind == "ips" ? FileSys::Patch::ApplyIpsPatch(patch, buffer)
                                              : FileSys::Patch::ApplyBpsPatch(patch, buffer);
            if (status != Loader::ResultStatus::Success) {
                std::fprintf(stderr, "Azahar's code reports an error (%d)\n", static_cast<int>(status));
                return 2;
            }
            return WriteWhole(argv[5], buffer.data(), buffer.size()) ? 0 : 1;
        }
        std::fprintf(stderr, "usage: layeredfs_driver layered BASE MODS_DIR OUT | patch ips|bps BUFFER PATCH OUT\n");
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Azahar's code threw: %s\n", error.what());
        return 3;
    }
}
