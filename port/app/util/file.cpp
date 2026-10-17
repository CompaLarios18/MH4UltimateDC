// Derived from Azahar src/common/file_util.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): Exists and IsDirectory (trailing '/' stripped before the system call, a
// drive letter's ':' given a '/' on Windows), CreateFullPath (one folder per '/' of the path, '\'
// only on Windows and only when no '/' is left; at most 100 levels), WriteStringToFile and
// ReadFileToString (text or binary mode, the byte count returned), ForeachDirectoryEntry (".." and
// "." skipped).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Copyright Dolphin Emulator Project
#include "util/file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "util/log.h"
#include "util/strings.h"

namespace App::Util {

namespace {

namespace fs = std::filesystem;

fs::path ToPath(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string ToUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

/// `path` without the '/' that end it (a lone "/" stays).
std::string StripTailSlashes(std::string path) {
    if (path.size() <= 1) {
        return path;
    }
    std::size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') {
        --end;
    }
    path.resize(end);
    return path;
}

fs::path SystemPath(const std::string& utf8) {
    std::string copy = StripTailSlashes(utf8);
#ifdef _WIN32
    // a drive's root needs its separator ("C:" is the drive's current folder)
    if (!copy.empty() && copy.back() == ':') {
        copy += '/';
    }
#endif
    return ToPath(copy);
}

/// fopen on a UTF-8 path (a wide one on Windows).
std::FILE* OpenFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(Utf8ToWide(path).c_str(), Utf8ToWide(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool MakeDirectory(const std::string& path) {
#ifdef _WIN32
    std::error_code ec;
    if (fs::create_directory(ToPath(path), ec) || (!ec && fs::is_directory(ToPath(path), ec))) {
        return true;
    }
    APP_LOG_ERROR("CreateDirectory failed on {}: {}", path, ec.message());
    return false;
#else
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
        return true;
    }
    APP_LOG_ERROR("mkdir failed on {}: {}", path, std::strerror(errno));
    return false;
#endif
}

} // namespace

bool Exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(SystemPath(path), ec) && !ec;
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(SystemPath(path), ec) && !ec;
}

bool CreateFullPath(const std::string& path) {
    if (Exists(path)) {
        return true;
    }
    int levels_left = 100;
    std::size_t position = 0;
    while (true) {
        const std::size_t previous = position;
        position = path.find('/', previous);
#ifdef _WIN32
        if (position == std::string::npos) {
            position = path.find('\\', previous);
        }
#endif
        if (position == std::string::npos) {
            return true;
        }
        // with the separator, so that the first folder of "/a" is "/" and not ""
        const std::string sub = path.substr(0, position + 1);
        if (!IsDirectory(sub) && !MakeDirectory(sub)) {
            APP_LOG_ERROR("CreateFullPath: directory creation failed");
            return false;
        }
        if (--levels_left <= 0) {
            APP_LOG_ERROR("CreateFullPath: directory structure is too deep");
            return false;
        }
        ++position;
    }
}

std::size_t WriteStringToFile(bool text_file, const std::string& path, std::string_view text) {
    std::FILE* file = OpenFile(path, text_file ? "w" : "wb");
    if (!file) {
        return 0;
    }
    const std::size_t written = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), file);
    const bool closed = std::fclose(file) == 0;
    return closed && written == text.size() ? written : 0;
}

std::size_t ReadFileToString(bool text_file, const std::string& path, std::string& text) {
    std::FILE* file = OpenFile(path, text_file ? "r" : "rb");
    if (!file) {
        return 0;
    }
    std::string content;
    char buffer[4096];
    while (const std::size_t count = std::fread(buffer, 1, sizeof(buffer), file)) {
        content.append(buffer, count);
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        return 0;
    }
    text = std::move(content);
    return text.size();
}

bool ListDirectory(const std::string& path, std::vector<std::string>& names) {
    std::error_code ec;
    std::vector<std::string> found;
    for (fs::directory_iterator it(ToPath(path), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = ToUtf8(it->path().filename());
        if (name != "." && name != "..") {
            found.push_back(name);
        }
    }
    if (ec) {
        return false;
    }
    names = std::move(found);
    return true;
}

} // namespace App::Util
