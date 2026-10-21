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
//  - A directory listing is sorted (D54, DirectoryNameLess), never in the host's own order that Azahar's
//    ForeachDirectoryEntry gives (readdir, FindNextFile): the entries and their fields are Azahar's, only the
//    order is fixed, so every host and every copy of a user folder shows the game one order.
//  - A symbolic link in a tree that is deleted is removed, never followed.
//  - CreateFullPath (via DirectoryPrefixes) is not Azahar's loop on Windows. Azahar (file_util.cpp:346-385)
//    takes the next '/' from the previous position and looks for '\' from there only when no '/' is
//    left, so on "C:\a\b/c/d/" it never makes "C:\" and "C:\a\". Here every separator of both kinds is a
//    directory boundary, in order, and a UNC or extended-length root (\\server\share, \\?\C:\) is not
//    created, as mkdir cannot make it. On Linux/macOS (separator set "/") the prefixes and the mkdir order
//    are Azahar's. The guard is the same 100 levels, counted the same way (the 100th prefix is made, then
//    the call fails).
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fs_types.h"

namespace mh4n::fs {

namespace host {

// A host path from UTF-8 text, and the UTF-8 text of a host path (D45). Every host file call of this module
// goes through these two: on Windows the path is the wide-character one (UTF-16, converted here), never the
// ANSI code page's; elsewhere the bytes are the path's own. Invalid UTF-8 becomes U+FFFD on Windows.
std::filesystem::path ToPath(const std::string& utf8_text);
std::string ToUtf8(const std::filesystem::path& path);

// The characters that separate the parts of a host path: "/" everywhere, and also "\\" on Windows, where a
// path often mixes both (a user folder from the command line, then "/sdmc/...").
#if defined(_WIN32)
inline constexpr std::string_view kPlatformSeparators = "/\\";
#else
inline constexpr std::string_view kPlatformSeparators = "/";
#endif
inline bool IsSeparator(char c, std::string_view separators = kPlatformSeparators) {
    return separators.find(c) != std::string_view::npos;
}
// The next separator of `separators` at or after `from`, npos if there is none.
std::size_t FindSeparator(const std::string& path, std::size_t from, std::string_view separators = kPlatformSeparators);
// The directories CreateFullPath makes, in order: each prefix of `path` that ends with a separator (the part
// after the last separator is a file name and is not one). With the Windows set, the root of a UNC path
// ("\\server\share\") is not listed: it is not a directory that can be made. At most kMaxDirectoryLevels
// are listed; *too_deep (when not null) says that the limit was reached.
inline constexpr int kMaxDirectoryLevels = 100;
std::vector<std::string> DirectoryPrefixes(const std::string& path, std::string_view separators = kPlatformSeparators,
                                           bool* too_deep = nullptr);

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
// On Windows a file that may still be open is renamed to "<name>.deleted.<pid>.<n>" first and marked for
// deletion, so the name is free at once and the system removes the file when its last handle closes; if
// the process dies before that, the renamed file stays (see RemoveStaleDeletedFiles).
bool RemoveFile(const std::string& path);
// Whether a file name is "<name>.deleted.<digits>.<digits>" (a non-empty <name>), the name a deleted file
// of RemoveFile has on Windows while it is open.
bool IsStaleDeletedName(std::string_view name);
// Removes the files of that name under `folder` and its subfolders: what a process that died between the
// rename and the last close left behind. Only regular files (never a directory or a link), and nothing
// outside `folder`. The number of files removed; one that cannot be removed stays (and is not an error).
std::size_t RemoveStaleDeletedFiles(const std::string& folder);
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
// D54: the order of a directory listing. Names compare as UTF-16 code units after the ASCII letters a-z are
// made A-Z (and nothing else: that is what NTFS does for names of ASCII characters, so '_' (0x5F) sorts after
// the letters, 'a' and 'B' compare as 'A' and 'B', digits come before letters); two names that differ only by
// the case of ASCII letters (a case-sensitive file system) are then ordered bytewise on the code units. The
// result is a strict weak order, in fact total on distinct names.
// Why: Azahar lists in the host's order (NTFS: by name; Linux and macOS: the file system's own, hash order on
// ext4; FAT-family volumes: creation order), so one user folder could show the game its files in another
// order on another host. Departure from Azahar on those hosts, where the order was never defined.
bool DirectoryNameLess(std::u16string_view a, std::u16string_view b);
// The children of a directory sorted by DirectoryNameLess on the UTF-16 form of the name (directories and files
// together, as one stream), without "." and ".."; nullopt if it cannot be listed.
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
// Close() makes the data durable (when it was changed since the last sync) and stops all further use; the handle itself is released with the object,
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
    // Makes what was written durable. False if the host refused. A file that was not written or resized
    // since its last successful sync is already durable: Sync() answers true and does not ask the host (D42).
    bool Sync() const;
    // Sync() (when writable), then no more reads or writes. False if the sync failed.
    bool Close();

    // Changed since the last successful sync (always false for a file opened read-only).
    bool Dirty() const;
    // How many times this file asked the host to sync (fdatasync, fsync, FlushFileBuffers), and how many all
    // files did in this process; for the tests of D42.
    u64 HostSyncCount() const;
    static u64 TotalHostSyncs();

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
