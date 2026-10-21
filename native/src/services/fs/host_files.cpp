// Derived from Azahar src/common/file_util.cpp, src/common/file_util.h and src/core/file_sys/path_parser.cpp
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"); see the header of host_files for what is taken from them.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (file_util.cpp, file_util.h)
// Copyright 2016 Citra Emulator Project (path_parser.cpp)
#include "host_files.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <set>
#include <system_error>

#include <fmt/format.h>

#include "mh4n/log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mh4n::fs {

namespace {
constexpr const char* kLog = "Service.FS";
}

// ---- names ---------------------------------------------------------------------------------------------------------

std::u16string Utf8ToUtf16(const std::string& text) {
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const u8 lead = static_cast<u8>(text[i]);
        u32 code = 0xFFFD;
        std::size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2, code = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3, code = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4, code = lead & 0x07u;
        }
        if (length > 1) {
            bool good = i + length <= text.size();
            for (std::size_t k = 1; good && k < length; ++k) {
                const u8 next = static_cast<u8>(text[i + k]);
                good = (next & 0xC0) == 0x80;
                code = (code << 6) | (next & 0x3Fu);
            }
            if (!good || code > 0x10FFFF || (code >= 0xD800 && code < 0xE000)) {
                code = 0xFFFD;
                length = 1;
            }
        }
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}

std::string Utf16ToUtf8(const std::u16string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        u32 code = text[i];
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] < 0xE000) {
            code = 0x10000 + ((code - 0xD800) << 10) + (text[i + 1] - 0xDC00u);
            ++i;
        } else if (code >= 0xD800 && code < 0xE000) {
            code = 0xFFFD;
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

void SplitFilename83(const std::string& filename, char short_name[9], char extension[4]) {
    const std::string forbidden_characters = ".\"/\\[]:;=, ";
    std::memcpy(short_name, "        ", 9);  // eight spaces and the null
    std::memcpy(extension, "   ", 4);         // three spaces and the null

    std::string::size_type point = filename.rfind('.');
    if (point == filename.size() - 1) {
        point = filename.rfind('.', point);
    }
    const auto upper = [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); };

    int j = 0;
    for (const char letter : filename.substr(0, point)) {
        if (forbidden_characters.find(letter, 0) != std::string::npos) {
            continue;
        }
        if (j == 8) {
            short_name[6] = '~';
            short_name[7] = '1';
            break;
        }
        short_name[j++] = upper(letter);
    }
    if (point != std::string::npos) {
        j = 0;
        for (const char letter : filename.substr(point + 1, 3)) {
            extension[j++] = upper(letter);
        }
    }
}

// ---- host paths ------------------------------------------------------------------------------------------------------

namespace host {

std::filesystem::path ToPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

namespace {
std::string ToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}
}  // namespace

bool Exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(ToPath(path), ec);
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(ToPath(path), ec);
}

u64 FileSize(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path native = ToPath(path);
    if (!std::filesystem::exists(native, ec) || std::filesystem::is_directory(native, ec)) {
        return 0;
    }
    const auto size = std::filesystem::file_size(native, ec);
    return ec ? 0 : static_cast<u64>(size);
}

bool CreateDir(const std::string& path) {
    std::error_code ec;
    if (std::filesystem::create_directory(ToPath(path), ec)) {
        return true;
    }
    // create_directory says false without an error when the directory is already there
    return !ec && IsDirectory(path);
}

bool CreateFullPath(const std::string& path) {
    if (Exists(path)) {
        return true;
    }
    std::size_t position = 0;
    int guard = 100;
    for (;;) {
        position = path.find('/', position);
#if defined(_WIN32)
        if (position == std::string::npos) {
            position = path.find('\\', 0);
        }
#endif
        if (position == std::string::npos) {
            return true;
        }
        const std::string prefix = path.substr(0, position + 1);
        if (!IsDirectory(prefix) && !CreateDir(prefix)) {
            MH4N_LOG(Error, kLog, "cannot create the directory {}", prefix);
            return false;
        }
        if (--guard <= 0) {
            MH4N_LOG(Error, kLog, "the directory structure of {} is too deep", path);
            return false;
        }
        ++position;
    }
}

bool CreateEmptyFile(const std::string& path) {
#if defined(_WIN32)
    HANDLE handle = CreateFileW(ToPath(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(handle);
    return true;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        MH4N_LOG(Error, kLog, "cannot create {}: {}", path, std::strerror(errno));
        return false;
    }
    ::close(fd);
    return true;
#endif
}

bool RemoveFile(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path native = ToPath(path);
    const auto status = std::filesystem::symlink_status(native, ec);
    if (ec || status.type() == std::filesystem::file_type::not_found) {
        return true;  // "we care about the file not being there, not the actual delete"
    }
    if (status.type() == std::filesystem::file_type::directory) {
        MH4N_LOG(Error, kLog, "cannot delete {}: it is a directory", path);
        return false;
    }
#if defined(_WIN32)
    // A file with an open handle that is deleted can not be made again until the handle is closed: rename
    // it to a unique name first, then mark it for deletion (what Azahar does).
    static std::atomic<u64> counter{0};
    const std::wstring wide = native.wstring();
    const std::wstring deleted =
        wide + L".deleted." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(counter++);
    if (MoveFileExW(wide.c_str(), deleted.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(deleted.c_str());
        return true;
    }
    return false;
#else
    if (::unlink(path.c_str()) == -1) {
        MH4N_LOG(Error, kLog, "cannot delete {}: {}", path, std::strerror(errno));
        return false;
    }
    return true;
#endif
}

bool RemoveEmptyDirectory(const std::string& path) {
    if (!IsDirectory(path)) {
        return false;
    }
    std::error_code ec;
    // remove() on a directory is rmdir: it fails when the directory is not empty
    return std::filesystem::remove(ToPath(path), ec) && !ec;
}

std::optional<std::vector<Child>> ListDirectory(const std::string& path) {
    std::error_code ec;
    std::filesystem::directory_iterator it(ToPath(path), ec);
    if (ec) {
        return std::nullopt;
    }
    std::vector<Child> children;
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        Child child;
        child.name = ToUtf8(it->path().filename());
        const std::string full = ToUtf8(it->path());
        child.is_directory = IsDirectory(full);
        child.size = child.is_directory ? 0 : FileSize(full);
        children.push_back(std::move(child));
    }
    return children;
}

namespace {
bool DeleteTree(const std::string& directory, unsigned depth) {
    const auto children = ListDirectory(directory);
    if (!children) {
        return false;
    }
    for (const Child& child : *children) {
        const std::string full = directory + "/" + child.name;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(ToPath(full), ec);
        if (!ec && status.type() == std::filesystem::file_type::directory) {
            if (depth == 0 || !DeleteTree(full, depth - 1)) {
                return false;
            }
        } else if (!RemoveFile(full)) {
            return false;
        }
    }
    RemoveEmptyDirectory(directory);
    return true;
}
}  // namespace

bool DeleteDirectoryRecursively(const std::string& path) {
    return DeleteTree(path, 256);
}

bool WriteFileDurably(const std::string& path, const void* data, std::size_t size) {
#if defined(_WIN32)
    HANDLE handle = CreateFileW(ToPath(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool good = true;
    const auto* in = static_cast<const u8*>(data);
    std::size_t done = 0;
    while (good && done < size) {
        DWORD put = 0;
        good = WriteFile(handle, in + done, static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 30)), &put, nullptr) &&
               put != 0;
        done += put;
    }
    good = good && FlushFileBuffers(handle);
    CloseHandle(handle);
    return good;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        MH4N_LOG(Error, kLog, "cannot create {}: {}", path, std::strerror(errno));
        return false;
    }
    bool good = true;
    const auto* in = static_cast<const u8*>(data);
    std::size_t done = 0;
    while (good && done < size) {
        const ssize_t put = ::write(fd, in + done, size - done);
        if (put < 0 && errno == EINTR) {
            continue;
        }
        good = put > 0;
        if (good) {
            done += static_cast<std::size_t>(put);
        }
    }
    good = good && ::fsync(fd) == 0;
    if (!good) {
        MH4N_LOG(Error, kLog, "cannot write {}: {}", path, std::strerror(errno));
    }
    ::close(fd);
    return good;
#endif
}

void SyncDirectory(const std::string& path) {
#if !defined(_WIN32)
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)path;
#endif
}

void SyncDirectoryTree(const std::string& path) {
    const auto children = ListDirectory(path);
    if (!children) {
        return;
    }
    for (const Child& child : *children) {
        // A link is not followed (a cycle inside a save folder would never end).
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(ToPath(path + "/" + child.name), ec);
        if (!ec && status.type() == std::filesystem::file_type::directory) {
            SyncDirectoryTree(path + "/" + child.name);
        }
    }
    SyncDirectory(path);
}

}  // namespace host

// ---- WritableFile ------------------------------------------------------------------------------------------------

#if defined(_WIN32)

struct WritableFile::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::atomic<bool> closed{false};
    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

std::shared_ptr<WritableFile> WritableFile::Open(const std::string& path, bool writable, std::string* error) {
    HANDLE handle = CreateFileW(host::ToPath(path).c_str(), writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = fmt::format("cannot open {} (error {})", path, GetLastError());
        }
        return nullptr;
    }
    std::shared_ptr<WritableFile> file(new WritableFile());
    file->impl_ = std::make_unique<Impl>();
    file->impl_->handle = handle;
    file->writable_ = writable;
    return file;
}

bool WritableFile::IsOpen() const {
    return !impl_->closed.load();
}

u64 WritableFile::Size() const {
    LARGE_INTEGER size{};
    return GetFileSizeEx(impl_->handle, &size) ? static_cast<u64>(size.QuadPart) : 0;
}

std::size_t WritableFile::ReadAt(u64 offset, void* buffer, std::size_t length, bool* failed) const {
    if (failed != nullptr) {
        *failed = false;
    }
    std::size_t done = 0;
    auto* out = static_cast<u8*>(buffer);
    while (!impl_->closed.load() && done < length) {
        OVERLAPPED overlapped{};
        const u64 at = offset + done;
        overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        if (!ReadFile(impl_->handle, out + done, static_cast<DWORD>(std::min<std::size_t>(length - done, 1u << 30)),
                      &got, &overlapped)) {
            if (GetLastError() != ERROR_HANDLE_EOF && failed != nullptr) {
                *failed = true;
            }
            break;
        }
        if (got == 0) {
            break;
        }
        done += got;
    }
    return done;
}

std::size_t WritableFile::WriteAt(u64 offset, const void* buffer, std::size_t length, bool* failed) const {
    if (failed != nullptr) {
        *failed = false;
    }
    std::size_t done = 0;
    const auto* in = static_cast<const u8*>(buffer);
    while (!impl_->closed.load() && done < length) {
        OVERLAPPED overlapped{};
        const u64 at = offset + done;
        overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD put = 0;
        if (!WriteFile(impl_->handle, in + done, static_cast<DWORD>(std::min<std::size_t>(length - done, 1u << 30)),
                       &put, &overlapped) ||
            put == 0) {
            if (failed != nullptr) {
                *failed = true;
            }
            break;
        }
        done += put;
    }
    return done;
}

bool WritableFile::Resize(u64 size) const {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return writable_ && !impl_->closed.load() &&
           SetFileInformationByHandle(impl_->handle, FileEndOfFileInfo, &info, sizeof(info));
}

bool WritableFile::Sync() const {
    return !writable_ || FlushFileBuffers(impl_->handle);
}

#else

struct WritableFile::Impl {
    int fd = -1;
    std::atomic<bool> closed{false};
    ~Impl() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

std::shared_ptr<WritableFile> WritableFile::Open(const std::string& path, bool writable, std::string* error) {
    const int fd = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) {
        if (error != nullptr) {
            *error = fmt::format("cannot open {}: {}", path, std::strerror(errno));
        }
        return nullptr;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        if (error != nullptr) {
            *error = fmt::format("{} is not a regular file", path);
        }
        ::close(fd);
        return nullptr;
    }
    std::shared_ptr<WritableFile> file(new WritableFile());
    file->impl_ = std::make_unique<Impl>();
    file->impl_->fd = fd;
    file->writable_ = writable;
    return file;
}

bool WritableFile::IsOpen() const {
    return !impl_->closed.load();
}

u64 WritableFile::Size() const {
    struct stat info{};
    return ::fstat(impl_->fd, &info) == 0 ? static_cast<u64>(info.st_size) : 0;
}

std::size_t WritableFile::ReadAt(u64 offset, void* buffer, std::size_t length, bool* failed) const {
    if (failed != nullptr) {
        *failed = false;
    }
    std::size_t done = 0;
    auto* out = static_cast<u8*>(buffer);
    while (!impl_->closed.load() && done < length) {
        const ssize_t got = ::pread(impl_->fd, out + done, length - done, static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (failed != nullptr) {
                *failed = true;
            }
            break;
        }
        if (got == 0) {
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    return done;
}

std::size_t WritableFile::WriteAt(u64 offset, const void* buffer, std::size_t length, bool* failed) const {
    if (failed != nullptr) {
        *failed = false;
    }
    std::size_t done = 0;
    const auto* in = static_cast<const u8*>(buffer);
    while (!impl_->closed.load() && done < length) {
        const ssize_t put = ::pwrite(impl_->fd, in + done, length - done, static_cast<off_t>(offset + done));
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            if (failed != nullptr) {
                *failed = true;
            }
            break;
        }
        done += static_cast<std::size_t>(put);
    }
    return done;
}

bool WritableFile::Resize(u64 size) const {
    return writable_ && !impl_->closed.load() && ::ftruncate(impl_->fd, static_cast<off_t>(size)) == 0;
}

bool WritableFile::Sync() const {
    if (!writable_) {
        return true;
    }
    for (;;) {
#if defined(__linux__)
        // The data and the size, which is all a reader of the file needs; not the times.
        if (::fdatasync(impl_->fd) == 0) {
            return true;
        }
#else
        if (::fsync(impl_->fd) == 0) {
            return true;
        }
#endif
        if (errno != EINTR) {
            return false;
        }
    }
}

#endif

// A file that was never closed (the game ended with it open) still gets its data to the disk.
WritableFile::~WritableFile() {
    if (impl_ != nullptr && writable_ && !impl_->closed.load()) {
        Sync();
    }
}

bool WritableFile::Close() {
    if (impl_->closed.exchange(true)) {
        return true;
    }
    return Sync();
}

// ---- paths -------------------------------------------------------------------------------------------------------

std::optional<std::string> PathAsString(const Path& path) {
    switch (path.Type()) {
    case LowPathType::Char:
        // "Data is always null-terminated": the last byte goes, whatever it is.
        return std::string(path.data.begin(), path.data.empty() ? path.data.end() : path.data.end() - 1);
    case LowPathType::Wchar: {
        std::u16string units(path.data.size() / 2, u'\0');
        std::memcpy(units.data(), path.data.data(), units.size() * 2);
        if (!units.empty()) {
            units.pop_back();
        }
        return Utf16ToUtf8(units);
    }
    default:
        return std::nullopt;
    }
}

std::vector<u8> PathAsBinary(const Path& path) {
    switch (path.Type()) {
    case LowPathType::Binary:
        return path.data;
    case LowPathType::Char: {
        const std::optional<std::string> text = PathAsString(path);
        return std::vector<u8>(text->begin(), text->end());
    }
    case LowPathType::Wchar: {
        std::vector<u8> bytes = path.data;
        if (bytes.size() >= 2) {
            bytes.resize(bytes.size() - 2);
        } else {
            bytes.clear();
        }
        bytes.resize(bytes.size() & ~std::size_t{1});
        return bytes;
    }
    default:
        return {};
    }
}

PathParser::PathParser(const Path& path) {
    if (path.Type() != LowPathType::Char && path.Type() != LowPathType::Wchar) {
        return;
    }
    const std::string text = *PathAsString(path);
    if (text.empty() || text[0] != '/') {
        return;
    }
    // Characters that are valid on the 3DS and not on every host; games do not use them.
    static const std::set<char> invalid_chars{'<', '>', '\\', '|', ':', '"', '*', '?'};
    if (std::any_of(text.begin(), text.end(), [](char c) { return invalid_chars.count(c) != 0; })) {
        return;
    }
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = text.find('/', start);
        std::string part = text.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    int level = 0;
    for (const std::string& node : parts) {
        if (node == "..") {
            if (--level < 0) {
                return;  // out of the archive
            }
        } else {
            ++level;
        }
    }
    parts_ = std::move(parts);
    is_valid_ = true;
    is_root_ = level == 0;
}

PathParser::HostStatus PathParser::GetHostStatus(const std::string& mount_point) const {
    std::string path = mount_point;
    if (!host::IsDirectory(path)) {
        return InvalidMountPoint;
    }
    if (parts_.empty()) {
        return DirectoryFound;
    }
    for (auto it = parts_.begin(); it != parts_.end() - 1; ++it) {
        if (path.back() != '/') {
            path += '/';
        }
        path += *it;
        if (!host::Exists(path)) {
            return PathNotFound;
        }
        if (host::IsDirectory(path)) {
            continue;
        }
        return FileInPath;
    }
    path += "/" + parts_.back();
    if (!host::Exists(path)) {
        return NotFound;
    }
    if (host::IsDirectory(path)) {
        return DirectoryFound;
    }
    return FileFound;
}

std::string PathParser::BuildHostPath(const std::string& mount_point) const {
    std::string path = mount_point;
    for (const std::string& node : parts_) {
        if (path.empty() || path.back() != '/') {
            path += '/';
        }
        path += node;
    }
    return path;
}

}  // namespace mh4n::fs
