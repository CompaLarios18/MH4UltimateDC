// Derived from Azahar src/core/file_sys/ivfc_archive.cpp, src/core/file_sys/ivfc_archive.h,
// src/core/file_sys/romfs_reader.cpp and src/core/file_sys/romfs_reader.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the files that serve RomFS bytes (IVFCFile over a DirectRomFSReader: a read is cut to what is
// left of the image, the size is the image's, writes and SetSize do nothing) and the file held in memory
// (IVFCFileInMemory: a read is cut to what is left, the size is the data's).
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (ivfc_archive.cpp, ivfc_archive.h)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (romfs_reader.cpp, romfs_reader.h)
//
// The two kinds of file the archives of this module open (N1-12). The SDK parses the RomFS itself, so a
// file serves bytes at offsets and nothing more.
#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "fs_types.h"
#include "mh4n/log.h"
#include "positioned_file.h"

namespace mh4n::fs {

// A slice [offset, offset + size) of a file on the host, read by position.
class SliceFile final : public FileBackend {
public:
    SliceFile(std::shared_ptr<PositionedFile> file, u64 offset, u64 size)
        : file_(std::move(file)), offset_(offset), size_(size) {}

    Result Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) override {
        *read = 0;
        if (offset >= size_ || length == 0) {
            return kResultSuccess;  // nothing left (Azahar's reader would underflow here)
        }
        const std::size_t count = static_cast<std::size_t>(std::min<u64>(length, size_ - offset));
        bool failed = false;
        *read = file_->ReadAt(offset_ + offset, buffer, count, &failed);
        if (failed) {
            MH4N_LOG(Error, "Service.FS", "a read of 0x{:X} bytes at 0x{:X} failed on the host", count,
                     offset_ + offset);
        }
        return kResultSuccess;
    }
    u64 GetSize() override { return size_; }

private:
    std::shared_ptr<PositionedFile> file_;
    u64 offset_;
    u64 size_;
};

// Bytes in memory (the open-source replacements of the system archives).
class MemoryFile final : public FileBackend {
public:
    MemoryFile(const unsigned char* data, u64 size) : data_(data), size_(size) {}

    Result Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) override {
        *read = 0;
        if (offset >= size_ || length == 0) {
            return kResultSuccess;
        }
        const std::size_t count = static_cast<std::size_t>(std::min<u64>(length, size_ - offset));
        std::memcpy(buffer, data_ + offset, count);
        *read = count;
        return kResultSuccess;
    }
    u64 GetSize() override { return size_; }

private:
    const unsigned char* data_;
    u64 size_;
};

}  // namespace mh4n::fs
