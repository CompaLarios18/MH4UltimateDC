// Derived from Azahar src/common/file_util.cpp, src/common/file_util.h and src/core/file_sys/path_parser.cpp
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): what the save data archives do on the host (Exists and IsDirectory by stat, CreateDir that
// accepts an existing directory, CreateFullPath, Delete that accepts a missing file, DeleteDir, a recursive
// delete, ScanDirectoryTree with no recursion), and the path of an archive's file (PathParser: only char
// and wide char paths, a leading slash, none of < > \ | : " * ?, empty and "." parts dropped, ".." that
// may not leave the archive, GetHostStatus, BuildHostPath).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (file_util.cpp, file_util.h)
// Copyright 2016 Citra Emulator Project (path_parser.cpp)
//
// The host side of the save data and extdata archives (N1-13). Departures from Azahar:
//  - A file is read and written by position (pread/pwrite, ReadFile/WriteFile with an offset), so the I/O
//    threads and the scheduler thread can use one file at the same time and there is no cursor. A file
//    written with Sync() is on the disk when it returns (fdatasync or fsync / FlushFileBuffers); Azahar's "flush" is an
//    fflush, which only hands the bytes to the operating system.
//  - A directory listing is the host's own order (readdir, FindNextFile), as Azahar's ForeachDirectoryEntry
//    gives; nothing sorts it.
//  - A symbolic link in a tree that is deleted is removed, never followed.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fs_types.h"

namespace mh4n::fs {

namespace host {

// A host path from UTF-8 text.
std::filesystem::path ToPath(const std::string& utf8);

bool Exists(const std::string& path);
bool IsDirectory(const std::string& path);
// The size of a file, 0 if it cannot be read (Azahar's FileUtil::GetSize).
u64 FileSize(const std::string& path);

// mkdir; an existing directory is success (Azahar's CreateDir).
bool CreateDir(const std::string& path);
// Makes every directory of `path` up to its last slash (a path that ends in a slash is made whole).
bool CreateFullPath(const std::string& path);
// Makes (or empties) the file.
bool CreateEmptyFile(const std::string& path);
// Removes a file; a path that is not there counts as removed, a directory does not (Azahar's Delete).
bool RemoveFile(const std::string& path);
// rmdir: only an empty directory.
bool RemoveEmptyDirectory(const std::string& path);
// Removes everything under `path` and `path` itself. False if `path` cannot be listed or something in it
// cannot be removed (the part removed so far stays removed, as in Azahar).
bool DeleteDirectoryRecursively(const std::string& path);

struct Child {
    std::string name;  // UTF-8
    bool is_directory = false;
    u64 size = 0;      // of a file; 0 for a directory
};
// The children of a directory in the host's order, without "." and ".."; nullopt if it cannot be listed.
std::optional<std::vector<Child>> ListDirectory(const std::string& path);

// Writes a whole small file (metadata, an icon) and makes it durable before returning. False if it could
// not be created or fully written.
bool WriteFileDurably(const std::string& path, const void* data, std::size_t size);
// Makes the entries of a directory (new and removed names) durable. Best effort; nothing on Windows.
void SyncDirectory(const std::string& path);
// SyncDirectory on `path` and every directory under it.
void SyncDirectoryTree(const std::string& path);

}  // namespace host

// A file on the host that is read and written by position. Any thread may call any member at the same time.
// Close() makes the data durable and stops all further use; the handle itself is released with the object,
// so an I/O thread that is still reading never reads through a handle the host has reused.
class WritableFile {
public:
    // Opens an existing file for reading, or for reading and writing. Null, and a message in *error, if it
    // cannot be opened.
    static std::shared_ptr<WritableFile> Open(const std::string& path, bool writable, std::string* error);
    ~WritableFile();
    WritableFile(const WritableFile&) = delete;
    WritableFile& operator=(const WritableFile&) = delete;

    bool Writable() const { return writable_; }
    bool IsOpen() const;
    // The size now.
    u64 Size() const;
    // Fewer than `length` bytes at the end of the file or when the host fails (*failed says which of the
    // two when it is not null). Nothing after Close().
    std::size_t ReadAt(u64 offset, void* buffer, std::size_t length, bool* failed = nullptr) const;
    // Fewer than `length` only when the host fails (a full disk): *failed is then true. Nothing after Close().
    std::size_t WriteAt(u64 offset, const void* buffer, std::size_t length, bool* failed = nullptr) const;
    // Sets the size (zeros when it grows). False if the host refused.
    bool Resize(u64 size) const;
    // Makes what was written durable. False if the host refused.
    bool Sync() const;
    // Sync() (when writable), then no more reads or writes. False if the sync failed.
    bool Close();

private:
    WritableFile() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool writable_ = false;
};

// ---- the path of a file in an archive (Azahar PathParser) ---------------------------------------------------------------

// The UTF-8 text of a char or wide char low path (Azahar Path::AsString), nullopt for the other types.
// A char path's last byte (its null) is dropped, whatever it is; so is a wide char path's last unit.
std::optional<std::string> PathAsString(const Path& path);

// The bytes of a low path as Azahar's Path::AsBinary gives them: a binary path's, a char path's text, a wide
// char path's units; nothing for the others. Both text types drop the null at the end.
std::vector<u8> PathAsBinary(const Path& path);

class PathParser {
public:
    explicit PathParser(const Path& path);

    bool IsValid() const { return is_valid_; }
    bool IsRootDirectory() const { return is_root_; }

    enum HostStatus {
        InvalidMountPoint,  // the archive's directory is not there
        PathNotFound,       // a directory on the way does not exist
        FileInPath,         // a file is where a directory on the way should be
        FileFound,
        DirectoryFound,
        NotFound,           // the last part does not exist
    };
    HostStatus GetHostStatus(const std::string& mount_point) const;
    // The host path of the file: the mount point, then each part with a slash.
    std::string BuildHostPath(const std::string& mount_point) const;

private:
    std::vector<std::string> parts_;
    bool is_valid_ = false;
    bool is_root_ = false;
};

// ---- names -------------------------------------------------------------------------------------------------------------

std::u16string Utf8ToUtf16(const std::string& text);
std::string Utf16ToUtf8(const std::u16string& text);
// Azahar FileUtil::SplitFilename83: 'longfilename.txt' -> 'LONGFI~1' and 'TXT'.
void SplitFilename83(const std::string& filename, char short_name[9], char extension[4]);

}  // namespace mh4n::fs
