// Derived from Azahar src/core/file_sys/layered_fs.cpp, src/core/file_sys/layered_fs.h and
// src/core/file_sys/romfs_reader.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of
// them says "GPLv2 or any later version"), with the hunk of port/azahar/azahar.patch for layered_fs.cpp
// (the files in the sub-folders of romfs_ext/ too).
// Copyright 2020-2026 Citra Emulator Project / Azahar Emulator Project (layered_fs.cpp, layered_fs.h)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (romfs_reader.h)
//
// LayeredFS (N3-10, the file system half of D72): the game's RomFS with the files of a mods folder laid over
// it, as Azahar's `LayeredFS` does.
//
//   <mods>/romfs/       files that replace the game's, or add new ones (new folders too)
//   <mods>/romfs_ext/   `<file>.stub` removes <file>; `<file>.ips` and `<file>.bps` patch it. The patches may
//                       sit in folders that mirror the RomFS (romfs_ext/data/x.arc.ips patches data/x.arc), as
//                       in the port's Azahar.
//
// Build() reads the base image's metadata (the header and the directory and file tables) and nothing of the
// files' data, then lays out a new RomFS: the header and tables from memory, each file's data from the base
// image, from the replacement file (opened when it is first read) or from a patched copy in memory (made
// at build time: that one reads the original file from the base). A RomFS is gigabytes: the work at build
// time is a function of the number of files and folders only.
//
// What the SDK sees: a level-3 RomFS image, i.e. the bytes of the base from the start of its header. The
// caller passes the base of that slice (N1-12 serves the RomFS section from 0x1000 on).
//
// Departures from Azahar, all in behaviour Azahar's code does not define:
//  - A mods file or folder is read in the order of its names (bytes, ascending); Azahar reads a folder in
//    the order the host lists it, which the hash tables do not care about but which orders new files.
//  - A `.stub` for the first file of a folder is handled; Azahar looks the removed file up in a map that
//    does not hold it and ends the process with an exception.
//  - A base image that does not hold what its header says (short reads, offsets past a table) is an error
//    (a null result and a message) where Azahar reads garbage; bytes of a file past the end of the base read as
//    zeros.
//  - Names are converted with mh4n::utf8 (a lone surrogate becomes U+FFFD; Azahar's boost conversion drops it).
//  - Messages: counts at Info; the names of mods files at Debug (and the warnings Azahar has).
//
// Threads: Read may run on any thread, several at once. Build runs on one thread.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "../services/fs/fs_types.h"
#include "../services/fs/positioned_file.h"
#include "mh4n/types.h"

namespace mh4n::layeredfs {

// What Build reads the base image through: the bytes at `offset` of the image (0 is the image's header),
// fewer at its end. Any thread. Tests use one that counts and one that has no file behind it.
class BaseReader {
public:
    virtual ~BaseReader() = default;
    virtual std::size_t ReadAt(u64 offset, void* buffer, std::size_t length) = 0;
    // The size of the image in bytes.
    virtual u64 Size() const = 0;
};

// The slice [offset, offset + size) of a host file.
std::shared_ptr<BaseReader> MakeBaseReader(std::shared_ptr<fs::PositionedFile> file, u64 offset, u64 size);

// The layered image as a file. `romfs_dir` and `romfs_ext_dir` are the mods folder's `romfs` and `romfs_ext`;
// a folder that does not exist (or an empty name) is a layer with nothing in it. On an error the result is
// null and *error says what is wrong with the base image.
std::shared_ptr<fs::FileBackend> Build(std::shared_ptr<BaseReader> base, const std::string& romfs_dir,
                                       const std::string& romfs_ext_dir, std::string* error = nullptr);
std::shared_ptr<fs::FileBackend> Build(std::shared_ptr<fs::PositionedFile> base, u64 offset, u64 size,
                                       const std::string& romfs_dir, const std::string& romfs_ext_dir,
                                       std::string* error = nullptr);

}  // namespace mh4n::layeredfs
