#include "positioned_file.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

#include <fmt/format.h>

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

#if defined(_WIN32)

struct PositionedFile::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

std::shared_ptr<PositionedFile> PositionedFile::Open(const std::string& path, std::string* error) {
    const std::filesystem::path native(std::u8string(path.begin(), path.end()));
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = fmt::format("cannot open {} (error {})", path, GetLastError());
        }
        return nullptr;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        if (error != nullptr) {
            *error = fmt::format("cannot get the size of {} (error {})", path, GetLastError());
        }
        CloseHandle(handle);
        return nullptr;
    }
    std::shared_ptr<PositionedFile> file(new PositionedFile());
    file->impl_ = std::make_unique<Impl>();
    file->impl_->handle = handle;
    file->size_ = static_cast<u64>(size.QuadPart);
    return file;
}

std::size_t PositionedFile::ReadAt(u64 offset, void* buffer, std::size_t length, bool* failed) const {
    if (failed != nullptr) {
        *failed = false;
    }
    std::size_t done = 0;
    auto* out = static_cast<u8*>(buffer);
    while (done < length) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length - done, 1u << 30));
        OVERLAPPED overlapped{};
        const u64 at = offset + done;
        overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        if (!ReadFile(impl_->handle, out + done, chunk, &got, &overlapped)) {
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

#else

struct PositionedFile::Impl {
    int fd = -1;
    ~Impl() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

std::shared_ptr<PositionedFile> PositionedFile::Open(const std::string& path, std::string* error) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags);
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
    std::shared_ptr<PositionedFile> file(new PositionedFile());
    file->impl_ = std::make_unique<Impl>();
    file->impl_->fd = fd;
    file->size_ = static_cast<u64>(info.st_size);
    return file;
}

std::size_t PositionedFile::ReadAt(u64 offset, void* buffer, std::size_t length, bool* failed) const {
    if (failed != nullptr) {
        *failed = false;
    }
    std::size_t done = 0;
    auto* out = static_cast<u8*>(buffer);
    while (done < length) {
        const ssize_t got =
            ::pread(impl_->fd, out + done, length - done, static_cast<off_t>(offset + done));
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

#endif

PositionedFile::~PositionedFile() = default;

}  // namespace mh4n::fs
