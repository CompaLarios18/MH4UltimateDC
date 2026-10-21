// Derived from Azahar src/core/file_sys/savedata_archive.cpp, src/core/file_sys/path_parser.cpp and
// src/core/file_sys/disk_archive.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// each of them says "GPLv2 or any later version"); see cecd_store.h.
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (savedata_archive.cpp)
// Copyright 2016 Citra Emulator Project (path_parser.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (disk_archive.cpp)
#include "cecd_store.h"

#include <algorithm>
#include <cstring>

namespace mh4n::cecd {

std::size_t OpenFile::Read(u64 offset, std::size_t length, u8* out) const {
    if (closed_ || !read_) {
        return 0;
    }
    const std::vector<u8>& bytes = data_->bytes;
    if (offset >= bytes.size()) {
        return 0;
    }
    const std::size_t count = std::min<std::size_t>(length, bytes.size() - static_cast<std::size_t>(offset));
    std::memcpy(out, bytes.data() + offset, count);
    return count;
}

std::size_t OpenFile::Write(u64 offset, std::size_t length, const u8* in) {
    if (closed_ || !write_) {
        return 0;
    }
    std::vector<u8>& bytes = data_->bytes;
    const u64 end = offset + length;
    if (end > bytes.size()) {
        bytes.resize(static_cast<std::size_t>(end), 0);
    }
    if (length != 0) {
        std::memcpy(bytes.data() + offset, in, length);
    }
    return length;
}

u64 OpenFile::GetSize() const {
    return closed_ ? 0 : data_->bytes.size();
}

bool OpenFile::SetSize(u64 size) {
    if (closed_) {
        return false;
    }
    data_->bytes.resize(static_cast<std::size_t>(size), 0);
    return true;
}

CecStore::CecStore() {
    root_.is_dir = true;
}

bool CecStore::Parse(const std::string& path, std::vector<std::string>* sequence, bool* is_root) {
    if (path.empty() || path[0] != '/') {
        return false;
    }
    // Characters Azahar's PathParser refuses (invalid on the host systems).
    if (path.find_first_of("<>\\|:\"*?") != std::string::npos) {
        return false;
    }
    sequence->clear();
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty() && part != ".") {
            sequence->push_back(part);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    int level = 0;
    for (const std::string& node : *sequence) {
        if (node == "..") {
            if (--level < 0) {
                return false;
            }
        } else {
            ++level;
        }
    }
    *is_root = level == 0;
    // Resolve ".." (the host does it when it walks the path).
    std::vector<std::string> resolved;
    for (const std::string& node : *sequence) {
        if (node == "..") {
            resolved.pop_back();
        } else {
            resolved.push_back(node);
        }
    }
    *sequence = std::move(resolved);
    return true;
}

CecStore::Located CecStore::Locate(const std::vector<std::string>& sequence) {
    Located found;
    if (sequence.empty()) {
        found.status = Status::DirectoryFound;
        found.node = &root_;
        return found;
    }
    Node* current = &root_;
    for (std::size_t i = 0; i + 1 < sequence.size(); ++i) {
        const auto it = current->children.find(sequence[i]);
        if (it == current->children.end()) {
            found.status = Status::PathNotFound;
            return found;
        }
        if (!it->second->is_dir) {
            found.status = Status::FileInPath;
            return found;
        }
        current = it->second.get();
    }
    found.parent = current;
    found.name = sequence.back();
    const auto it = current->children.find(found.name);
    if (it == current->children.end()) {
        found.status = Status::NotFound;
        return found;
    }
    found.node = it->second.get();
    found.status = found.node->is_dir ? Status::DirectoryFound : Status::FileFound;
    return found;
}

Result CecStore::OpenFile(const std::string& path, OpenMode mode,
                          std::shared_ptr<mh4n::cecd::OpenFile>* file) {
    std::vector<std::string> sequence;
    bool is_root = false;
    if (!Parse(path, &sequence, &is_root)) {
        return kFsInvalidPath;
    }
    if (!mode.read && !mode.write && !mode.create) {
        return kFsUnsupportedOpenFlags;
    }
    if (mode.create && !mode.write) {
        return kFsUnsupportedOpenFlags;
    }
    Located found = Locate(sequence);
    switch (found.status) {
    case Status::PathNotFound:
        return kFsPathNotFound;
    case Status::FileInPath:
    case Status::DirectoryFound:
        return kFsUnexpectedFileOrDirectory;
    case Status::NotFound: {
        if (!mode.create) {
            return kFsFileNotFound;
        }
        auto node = std::make_unique<Node>();
        node->file = std::make_shared<FileData>();
        found.node = node.get();
        found.parent->children[found.name] = std::move(node);
        break;
    }
    case Status::FileFound:
        break;
    }
    *file = std::make_shared<mh4n::cecd::OpenFile>(found.node->file, mode.read, mode.write);
    return kFsOk;
}

Result CecStore::DeleteFile(const std::string& path) {
    std::vector<std::string> sequence;
    bool is_root = false;
    if (!Parse(path, &sequence, &is_root)) {
        return kFsInvalidPath;
    }
    const Located found = Locate(sequence);
    switch (found.status) {
    case Status::PathNotFound:
        return kFsPathNotFound;
    case Status::FileInPath:
    case Status::DirectoryFound:
    case Status::NotFound:
        return kFsFileNotFound;
    case Status::FileFound:
        break;
    }
    found.parent->children.erase(found.name);
    return kFsOk;
}

Result CecStore::DeleteDirectoryRecursively(const std::string& path) {
    std::vector<std::string> sequence;
    bool is_root = false;
    if (!Parse(path, &sequence, &is_root)) {
        return kFsInvalidPath;
    }
    if (is_root) {
        return kFsDirectoryNotEmpty;
    }
    const Located found = Locate(sequence);
    switch (found.status) {
    case Status::PathNotFound:
    case Status::NotFound:
        return kFsPathNotFound;
    case Status::FileInPath:
    case Status::FileFound:
        return kFsUnexpectedFileOrDirectory;
    case Status::DirectoryFound:
        break;
    }
    found.parent->children.erase(found.name);
    return kFsOk;
}

Result CecStore::CreateDirectory(const std::string& path) {
    std::vector<std::string> sequence;
    bool is_root = false;
    if (!Parse(path, &sequence, &is_root)) {
        return kFsInvalidPath;
    }
    const Located found = Locate(sequence);
    switch (found.status) {
    case Status::PathNotFound:
        return kFsPathNotFound;
    case Status::FileInPath:
        return kFsUnexpectedFileOrDirectory;
    case Status::DirectoryFound:
    case Status::FileFound:
        return kFsDirectoryAlreadyExists;
    case Status::NotFound:
        break;
    }
    auto node = std::make_unique<Node>();
    node->is_dir = true;
    found.parent->children[found.name] = std::move(node);
    return kFsOk;
}

Result CecStore::OpenDirectory(const std::string& path, std::vector<std::string>* names) {
    std::vector<std::string> sequence;
    bool is_root = false;
    if (!Parse(path, &sequence, &is_root)) {
        return kFsInvalidPath;
    }
    const Located found = Locate(sequence);
    switch (found.status) {
    case Status::PathNotFound:
    case Status::NotFound:
        return kFsPathNotFound;
    case Status::FileInPath:
    case Status::FileFound:
        return kFsUnexpectedFileOrDirectory;
    case Status::DirectoryFound:
        break;
    }
    names->clear();
    for (const auto& [name, child] : found.node->children) {
        names->push_back(name);
    }
    return kFsOk;
}

}  // namespace mh4n::cecd
