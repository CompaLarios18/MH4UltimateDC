// Derived in part from Azahar src/common/file_util.h, src/common/assert.h, src/common/common_paths.h,
// src/common/string_util.h, src/common/archives.h, src/common/logging/log.h, src/core/file_sys/romfs_reader.h
// and src/core/loader/loader.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each says "GPLv2
// or any later version"), as minimal stand-ins: the names and signatures that the LayeredFS and patch sources
// use (and, for the boost headers they include, the few names those declare).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (file_util.h, string_util.h, loader.h)
// Copyright Dolphin Emulator Project (file_util.h, second notice)
// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project (assert.h)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (common_paths.h)
// Copyright 2013 Dolphin Emulator Project (common_paths.h, second notice)
// Copyright 2020-2025 Citra Emulator Project / Azahar Emulator Project (archives.h)
// Copyright 2014 Citra Emulator Project (log.h)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (romfs_reader.h)
//
// The stand-ins of the LayeredFS oracle (tools/native/layeredfs_oracle/layeredfs_oracle.py). Our own code,
// compiled in place of the Azahar headers that layered_fs.cpp and patch.cpp include:
//   common/file_util.h        -> FileUtil over the host (std::filesystem); a folder is listed in the order of
//                                its names, so that the oracle gives the same answer on every host
//   common/string_util.h      -> UTF-8 <-> UTF-16 (valid text only)
//   common/assert.h, archives.h, common_paths.h, logging/log.h -> ASSERT_MSG, UNREACHABLE, DIR_SEP, logging
//                                as nothing, the serialization macro as nothing
//   core/file_sys/romfs_reader.h -> RomFSReader, and a DirectRomFSReader that reads a host file (a read past
//                                the end gives what is left)
//   core/loader/loader.h      -> Loader::ResultStatus
//   boost/crc.hpp, boost/serialization/*.hpp -> the CRC-32 and the names the headers declare
// The harness writes a forwarding header with the Azahar name for each, so the copied sources compile
// unchanged. common_types.h, swap.h and alignment.h are Azahar's own, copied as they are.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

#define DIR_SEP "/"
#define DIR_SEP_CHR '/'

#define LOG_TRACE(cls, ...) ((void)0)
#define LOG_DEBUG(cls, ...) ((void)0)
#define LOG_INFO(cls, ...) ((void)0)
#define LOG_WARNING(cls, ...) ((void)0)
#define LOG_ERROR(cls, ...) ((void)0)
#define LOG_CRITICAL(cls, ...) ((void)0)

#define ASSERT_MSG(cond, ...)                                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "ASSERT failed: %s\n", #cond);                                    \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)
#define ASSERT(cond) ASSERT_MSG(cond, "")
#define UNREACHABLE()                                                                              \
    do {                                                                                           \
        std::fprintf(stderr, "UNREACHABLE\n");                                                     \
        std::abort();                                                                              \
    } while (0)

#define SERIALIZE_EXPORT_IMPL(x)
#define BOOST_CLASS_EXPORT_KEY(x)

namespace boost::serialization {
class access {};
template <class Base, class Derived>
Base& base_object(Derived& derived) {
    return derived;
}
} // namespace boost::serialization

namespace boost {
// boost::crc_32_type: the standard reflected CRC-32.
class crc_32_type {
public:
    void process_bytes(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const u8*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            crc_ ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit) {
                crc_ = (crc_ >> 1) ^ (0xEDB88320u & (0u - (crc_ & 1u)));
            }
        }
    }
    u32 checksum() const {
        return ~crc_;
    }

private:
    u32 crc_ = 0xFFFFFFFFu;
};
} // namespace boost

namespace Loader {
enum class ResultStatus {
    Success,
    ErrorNotLoaded,
    ErrorNotUsed,
    ErrorAlreadyLoaded,
    ErrorMemoryAllocationFailed,
    ErrorEncrypted,
    ErrorInvalidFormat,
    ErrorGbaTitle,
    ErrorArtic,
    ErrorBadMappings,
    ErrorPatches,
    ErrorPatchesInvalidTitle,
};
} // namespace Loader

namespace Common {

inline std::u16string UTF8ToUTF16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp;
        int n;
        if (c < 0x80) {
            cp = c, n = 1;
        } else if (c < 0xE0) {
            cp = c & 0x1F, n = 2;
        } else if (c < 0xF0) {
            cp = c & 0x0F, n = 3;
        } else {
            cp = c & 0x07, n = 4;
        }
        for (int k = 1; k < n && i + k < s.size(); ++k) {
            cp = cp << 6 | (static_cast<u8>(s[i + k]) & 0x3F);
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

inline std::string UTF16ToUTF8(std::u16string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        u32 cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | cp >> 6));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | cp >> 12));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | cp >> 18));
            out.push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

} // namespace Common

namespace FileUtil {

namespace stdfs = std::filesystem;

inline bool Exists(const std::string& path) {
    std::error_code ec;
    return stdfs::exists(path, ec);
}

inline bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return stdfs::is_directory(path, ec);
}

inline u64 GetSize(const std::string& path) {
    std::error_code ec;
    const auto size = stdfs::file_size(path, ec);
    return ec ? 0 : static_cast<u64>(size);
}

inline bool CreateFullPath(const std::string& path) {
    std::error_code ec;
    stdfs::create_directories(stdfs::path(path).parent_path(), ec);
    return !ec;
}

class IOFile {
public:
    IOFile(const std::string& path, const char* mode) : file_(std::fopen(path.c_str(), mode)) {}
    ~IOFile() {
        if (file_) {
            std::fclose(file_);
        }
    }
    IOFile(const IOFile&) = delete;
    explicit operator bool() const {
        return file_ != nullptr;
    }
    u64 GetSize() const {
        const long here = std::ftell(file_);
        std::fseek(file_, 0, SEEK_END);
        const long end = std::ftell(file_);
        std::fseek(file_, here, SEEK_SET);
        return static_cast<u64>(end);
    }
    std::size_t ReadBytes(void* data, std::size_t length) {
        return std::fread(data, 1, length, file_);
    }
    std::size_t WriteBytes(const void* data, std::size_t length) {
        return std::fwrite(data, 1, length, file_);
    }
    bool Seek(s64 offset, int origin) {
        return std::fseek(file_, static_cast<long>(offset), origin) == 0;
    }

private:
    std::FILE* file_;
};

/// callback(entries found below, the folder as it was given, the entry's name); false stops the walk.
using DirectoryEntryCallable =
    std::function<bool(u64*, const std::string&, const std::string&)>;

inline bool ForeachDirectoryEntry(u64* num_entries_out, const std::string& directory,
                                  DirectoryEntryCallable callback) {
    std::error_code ec;
    std::vector<std::string> names;
    for (stdfs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
    }
    if (ec) {
        return false;
    }
    std::sort(names.begin(), names.end());
    u64 found = 0;
    for (const std::string& name : names) {
        u64 ret = 0;
        if (!callback(&ret, directory, name)) {
            return false;
        }
        found += ret;
    }
    if (num_entries_out) {
        *num_entries_out = found;
    }
    return true;
}

struct FSTEntry {
    bool isDirectory;
    u64 size;
    std::string physicalName;
    std::string virtualName;
    std::vector<FSTEntry> children;
};

inline u64 ScanDirectoryTree(const std::string& directory, FSTEntry& parent_entry,
                             unsigned int recursion = 0, void* stop_flag = nullptr) {
    const auto callback = [recursion, &parent_entry](u64* num_entries_out,
                                                     const std::string& directory,
                                                     const std::string& virtual_name) -> bool {
        FSTEntry entry;
        entry.virtualName = virtual_name;
        entry.physicalName = directory + DIR_SEP + virtual_name;

        if (IsDirectory(entry.physicalName)) {
            entry.isDirectory = true;
            if (recursion > 0) {
                entry.size = ScanDirectoryTree(entry.physicalName, entry, recursion - 1);
                *num_entries_out += entry.size;
            } else {
                entry.size = 0;
            }
        } else {
            entry.isDirectory = false;
            entry.size = GetSize(entry.physicalName);
        }
        (*num_entries_out)++;
        parent_entry.children.push_back(std::move(entry));
        return true;
    };
    u64 num_entries = 0;
    return ForeachDirectoryEntry(&num_entries, directory, callback) ? num_entries : 0;
}

inline void GetAllFilesFromNestedEntries(FSTEntry& directory, std::vector<FSTEntry>& output) {
    for (auto& entry : directory.children) {
        if (entry.isDirectory) {
            GetAllFilesFromNestedEntries(entry, output);
        } else {
            output.push_back(entry);
        }
    }
}

} // namespace FileUtil

namespace FileSys {

class RomFSReader {
public:
    virtual ~RomFSReader() = default;
    virtual std::size_t GetSize() const = 0;
    virtual std::size_t ReadFile(std::size_t offset, std::size_t length, u8* buffer) = 0;
    virtual bool AllowsCachedReads() const = 0;
    virtual bool CacheReady(std::size_t file_offset, std::size_t length) = 0;
};

/// A RomFS reader that reads a host file; a read is cut to what is left of the file.
class DirectRomFSReader : public RomFSReader {
public:
    explicit DirectRomFSReader(const std::string& path) : file_(path, "rb") {}
    bool IsOpen() const {
        return static_cast<bool>(file_);
    }
    std::size_t GetSize() const override {
        return static_cast<std::size_t>(file_.GetSize());
    }
    std::size_t ReadFile(std::size_t offset, std::size_t length, u8* buffer) override {
        const std::size_t size = GetSize();
        if (offset >= size) {
            return 0;
        }
        length = std::min(length, size - offset);
        if (length == 0 || !file_.Seek(static_cast<s64>(offset), SEEK_SET)) {
            return 0;
        }
        return file_.ReadBytes(buffer, length);
    }
    bool AllowsCachedReads() const override {
        return true;
    }
    bool CacheReady(std::size_t, std::size_t) override {
        return true;
    }

private:
    FileUtil::IOFile file_;
};

} // namespace FileSys
