#include "launcher/game_file.h"

#include <cstring>

namespace Launcher::GameFile {

namespace {

constexpr std::uint64_t MEDIA_UNIT = 0x200;  ///< Azahar's kBlockSize, for NCSD and NCCH alike
constexpr std::size_t HEADER_SIZE = 0x200;   ///< an NCSD's or an NCCH's header
constexpr std::uint8_t NO_ROMFS = 0x02;      ///< in the NCCH's flags[7]
constexpr std::uint8_t NO_CRYPTO = 0x04;
/// where Azahar takes the RomFS's file system (IVFC level 3, what the game reads) to begin, and the
/// length its header starts with (Azahar's RomFSHeader, which its LayeredFS asserts)
constexpr std::uint64_t LEVEL3 = 0x1000;
constexpr std::uint32_t LEVEL3_HEADER_SIZE = 0x28;

std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool Is(const std::uint8_t* p, const char* magic) {
    return std::memcmp(p, magic, 4) == 0;
}

} // namespace

Verdict Check(const ReadAt& read_at, std::uint64_t size) {
    std::uint8_t header[HEADER_SIZE];
    if (size < HEADER_SIZE || !read_at(0, header, HEADER_SIZE)) {
        return Verdict::Unknown;
    }
    // where the NCCH is: a .cxi is one; a .3ds/.cci (NCSD) has the game in partition 0 (the
    // others hold the manual, download play and system update data)
    std::uint64_t base = 0;
    std::uint64_t ncch_size = size;
    if (Is(header + 0x100, "NCSD")) {
        base = U32(header + 0x120) * MEDIA_UNIT;
        ncch_size = U32(header + 0x124) * MEDIA_UNIT;
        if (base == 0 || ncch_size < HEADER_SIZE) {
            return Verdict::Unknown;
        }
        if (base + HEADER_SIZE > size) {
            return Verdict::Truncated;
        }
        if (!read_at(base, header, HEADER_SIZE)) {
            return Verdict::Unknown;
        }
    }
    if (!Is(header + 0x100, "NCCH")) {
        return Verdict::Unknown;
    }
    const std::uint8_t flags = header[0x18F];
    if ((flags & NO_CRYPTO) == 0) {
        return Verdict::Encrypted;
    }
    const std::uint64_t romfs_offset = U32(header + 0x1B0) * MEDIA_UNIT;
    const std::uint64_t romfs_size = U32(header + 0x1B4) * MEDIA_UNIT;
    // nothing to say about an NCCH without a RomFS, one in other media units (flags[6]: Azahar
    // reads every NCCH in 0x200-byte units), or a prototype's (sizes in bytes: Azahar takes an
    // NCCH whose content size is its size in bytes for one)
    if ((flags & NO_ROMFS) != 0 || romfs_offset == 0 || romfs_size == 0 || header[0x18E] != 0 ||
        U32(header + 0x104) == ncch_size) {
        return Verdict::Unknown;
    }
    const std::uint64_t romfs = base + romfs_offset;
    if (romfs + romfs_size > size) {
        return Verdict::Truncated;
    }
    std::uint8_t magic[4];
    if (!read_at(romfs, magic, sizeof(magic))) {
        return Verdict::Unknown;
    }
    if (Is(magic, "IVFC")) {
        return Verdict::Ok;
    }
    // the game never reads the IVFC header (Azahar hands it the RomFS from its file system on):
    // past a damaged first sector, a file system whose header is where it should be still plays.
    // A RomFS decrypted with the wrong keys is noise there too.
    if (romfs_size < LEVEL3 + sizeof(magic)) {
        return Verdict::BadRomFs;
    }
    if (!read_at(romfs + LEVEL3, magic, sizeof(magic))) {
        return Verdict::Unknown;
    }
    return U32(magic) == LEVEL3_HEADER_SIZE ? Verdict::Ok : Verdict::BadRomFs;
}

} // namespace Launcher::GameFile
