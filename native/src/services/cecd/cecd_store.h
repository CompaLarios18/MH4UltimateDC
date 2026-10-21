// Derived from Azahar src/core/file_sys/savedata_archive.cpp, src/core/file_sys/path_parser.cpp,
// src/core/file_sys/disk_archive.cpp and src/core/file_sys/errors.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the checks and result codes of the save data archive's file and directory operations, the
// path rules, and the behaviour of a file after it is closed.
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (savedata_archive.cpp)
// Copyright 2016 Citra Emulator Project (path_parser.cpp, errors.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (disk_archive.cpp)
//
// The CEC system save data archive (0x00010026) that cecd:u works on (N1-19), kept in memory with the
// operations and results of Azahar's SaveDataArchive over a host folder. Departures: nothing is
// written to disk (Azahar keeps the archive in the NAND folder between runs), and a directory lists
// its entries in name order (Azahar lists them in the host's order).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::cecd {

// Azahar's FileSys result codes (errors.h) for the module FS.
inline constexpr u32 kModuleFs = 17;
inline constexpr u32 kSummaryNothingHappened = 1;
inline constexpr u32 kSummaryNotFound = 4;
inline constexpr u32 kSummaryNotSupported = 6;
inline constexpr u32 kSummaryInvalidArgument = 7;
inline constexpr u32 kSummaryCanceled = 9;
inline constexpr u32 kLevelStatus = 25;
inline constexpr u32 kLevelUsage = 28;
inline constexpr Result kFsOk = 0;
inline constexpr Result kFsInvalidPath = MakeResult(702, kModuleFs, kSummaryInvalidArgument, kLevelUsage);
inline constexpr Result kFsUnsupportedOpenFlags =
    MakeResult(760, kModuleFs, kSummaryNotSupported, kLevelUsage);
inline constexpr Result kFsFileNotFound = MakeResult(112, kModuleFs, kSummaryNotFound, kLevelStatus);
inline constexpr Result kFsPathNotFound = MakeResult(113, kModuleFs, kSummaryNotFound, kLevelStatus);
inline constexpr Result kFsUnexpectedFileOrDirectory =
    MakeResult(770, kModuleFs, kSummaryNotSupported, kLevelUsage);
inline constexpr Result kFsDirectoryAlreadyExists =
    MakeResult(185, kModuleFs, kSummaryNothingHappened, kLevelStatus);
inline constexpr Result kFsFileAlreadyExists =
    MakeResult(180, kModuleFs, kSummaryNothingHappened, kLevelStatus);
inline constexpr Result kFsDirectoryNotEmpty = MakeResult(240, kModuleFs, kSummaryCanceled, kLevelStatus);

struct FileData {
    std::vector<u8> bytes;
};

// An open file (DiskFile over an IOFile). After Close every operation does nothing: reads and writes
// transfer 0 bytes, the size is 0, the size cannot be changed.
class OpenFile {
public:
    OpenFile(std::shared_ptr<FileData> data, bool read, bool write)
        : data_(std::move(data)), read_(read), write_(write) {}

    // Bytes read into `out` (`length` of them at most), starting at `offset`.
    std::size_t Read(u64 offset, std::size_t length, u8* out) const;
    // Writes `length` bytes at `offset`, growing the file with zeros if needed; the bytes written.
    std::size_t Write(u64 offset, std::size_t length, const u8* in);
    u64 GetSize() const;
    bool SetSize(u64 size);
    void Close() { closed_ = true; }
    bool IsClosed() const { return closed_; }

private:
    std::shared_ptr<FileData> data_;
    bool read_;
    bool write_;
    bool closed_ = false;
};

struct OpenMode {
    bool read = false;
    bool write = false;
    bool create = false;
};

class CecStore {
public:
    CecStore();

    Result OpenFile(const std::string& path, OpenMode mode, std::shared_ptr<OpenFile>* file);
    Result DeleteFile(const std::string& path);
    Result DeleteDirectoryRecursively(const std::string& path);
    Result CreateDirectory(const std::string& path);
    // The names of the entries of a directory, in name order.
    Result OpenDirectory(const std::string& path, std::vector<std::string>* names);

private:
    struct Node {
        bool is_dir = false;
        std::shared_ptr<FileData> file;
        std::map<std::string, std::unique_ptr<Node>> children;
    };
    enum class Status { PathNotFound, FileInPath, NotFound, DirectoryFound, FileFound };
    struct Located {
        Status status = Status::NotFound;
        Node* parent = nullptr;  // the directory holding the last component (null for the root)
        Node* node = nullptr;    // the last component if it exists
        std::string name;
    };
    // False for a path Azahar's PathParser refuses; `sequence` is the path without empty and "."
    // components.
    static bool Parse(const std::string& path, std::vector<std::string>* sequence, bool* is_root);
    Located Locate(const std::vector<std::string>& sequence);

    Node root_;
};

}  // namespace mh4n::cecd
