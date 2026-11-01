// The game file's own headers, read before the game starts (launcher/checks.cpp): the NCCH of a
// .cxi, or of a .3ds/.cci's partition 0 (NCSD, a cartridge image), whether its flags say it is
// decrypted, and whether its RomFS (the game's own files) starts with the IVFC header that a
// decrypted one starts with or, past a damaged first sector, has the header of the file system the
// game reads where Azahar takes it to begin. A file whose flags say "no crypto" but whose RomFS has
// neither was decrypted wrongly or is damaged, and the game would most likely stop with a fatal
// error of its own as it opens its files. Only the headers and a few bytes are read (the files are
// gigabytes), never the RomFS's hashes: a good dump with a few bad hash blocks still plays.
// Standard library only (tests/test_app_game_file.py).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace Launcher::GameFile {

/// Reads `size` bytes at `offset` of the file into `out`; false if they can't all be read.
using ReadAt = std::function<bool(std::uint64_t offset, void* out, std::size_t size)>;

enum class Verdict {
    Unknown,    ///< a layout this check doesn't know, or a read failed: it has nothing to say
    Ok,         ///< decrypted, and the RomFS starts as a decrypted one does
    Encrypted,  ///< the NCCH's flags say its contents are encrypted
    BadRomFs,   ///< the flags say decrypted, but the RomFS has neither its IVFC header nor, 0x1000
                ///< bytes in, its file system's
    Truncated,  ///< the file ends before its RomFS does: a part of it is missing
};

/// The game's NCCH in a file of `size` bytes, found where Azahar finds it (the offsets and sizes
/// in 0x200-byte media units).
Verdict Check(const ReadAt& read_at, std::uint64_t size);

} // namespace Launcher::GameFile
