// Derived from Azahar src/core/file_sys/layered_fs.cpp, src/core/file_sys/layered_fs.h and
// src/core/file_sys/romfs_reader.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of
// them says "GPLv2 or any later version"), with the hunk of port/azahar/azahar.patch for layered_fs.cpp.
// Copyright 2020-2026 Citra Emulator Project / Azahar Emulator Project (layered_fs.cpp, layered_fs.h)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (romfs_reader.h)
#include "layeredfs.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../common/utf8.h"
#include "mh4n/log.h"
#include "patch.h"

namespace mh4n::layeredfs {

namespace {

namespace stdfs = std::filesystem;

constexpr const char* kLog = "Service.FS";

constexpr u32 kNone = 0xFFFFFFFF;
constexpr u32 kHeaderSize = 0x28;          // RomFSHeader
constexpr u32 kDirectoryEntrySize = 0x18;  // DirectoryMetadata, then the name
constexpr u32 kFileEntrySize = 0x20;       // FileMetadata, then the name
constexpr int kMaxDirectoryDepth = 512;    // far deeper than any RomFS; stops a loop in a damaged table
constexpr unsigned kExtRecursion = 256;    // ScanDirectoryTree(patch_ext_path, result, 256)

u32 Le32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 Le64(const u8* p) { return u64(Le32(p)) | u64(Le32(p + 4)) << 32; }
void PutLe32(std::vector<u8>& out, std::size_t at, u32 v) {
    for (int i = 0; i < 4; ++i) {
        out[at + i] = static_cast<u8>(v >> (8 * i));
    }
}
void PutLe64(std::vector<u8>& out, std::size_t at, u64 v) {
    PutLe32(out, at, static_cast<u32>(v));
    PutLe32(out, at + 4, static_cast<u32>(v >> 32));
}
u64 AlignUp16(u64 v) { return (v + 15) & ~u64{15}; }
u32 AlignUp4(u64 v) { return static_cast<u32>((v + 3) & ~u64{3}); }

// ---- the base image ------------------------------------------------------------------------------------

class SliceReader final : public BaseReader {
public:
    SliceReader(std::shared_ptr<fs::PositionedFile> file, u64 offset, u64 size)
        : file_(std::move(file)), offset_(offset), size_(size) {}
    std::size_t ReadAt(u64 offset, void* buffer, std::size_t length) override {
        if (offset >= size_ || length == 0) {
            return 0;
        }
        const std::size_t count = static_cast<std::size_t>(std::min<u64>(length, size_ - offset));
        bool failed = false;
        const std::size_t got = file_->ReadAt(offset_ + offset, buffer, count, &failed);
        if (failed) {
            MH4N_LOG(Error, kLog, "LayeredFS: a read of 0x{:X} bytes at 0x{:X} failed on the host", count,
                     offset_ + offset);
        }
        return got;
    }
    u64 Size() const override { return size_; }

private:
    std::shared_ptr<fs::PositionedFile> file_;
    u64 offset_;
    u64 size_;
};

// ---- the host side of the mods folder ----------------------------------------------------------------

struct HostEntry {
    std::string name;  // UTF-8
    stdfs::path path;
    bool is_directory;
};

// The entries of a host folder in the order of their names (bytes, ascending). False if it cannot be listed.
bool ListHostDirectory(const stdfs::path& dir, std::vector<HostEntry>* out) {
    std::error_code ec;
    stdfs::directory_iterator it(dir, ec);
    if (ec) {
        return false;
    }
    for (const stdfs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            return false;
        }
        std::error_code type_ec;
        const bool is_dir = stdfs::is_directory(it->path(), type_ec);
        out->push_back({utf8::PathToUtf8(it->path().filename()), it->path(), is_dir});
    }
    std::sort(out->begin(), out->end(), [](const HostEntry& a, const HostEntry& b) { return a.name < b.name; });
    return true;
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return !path.empty() && stdfs::is_directory(utf8::PathFromUtf8(path), ec);
}

// ---- the image -----------------------------------------------------------------------------------------

struct FileNode;
struct DirNode {
    std::string name;
    std::u16string name16;
    std::string path;  // with trailing '/'
    std::vector<std::unique_ptr<FileNode>> files;
    std::vector<std::unique_ptr<DirNode>> directories;
    DirNode* parent = nullptr;
    u32 metadata_offset = 0;  // in the rebuilt directory table
};

enum class Relocation { None = 0, Replaced = 1, Patched = 2, Removed = 3 };

struct FileNode {
    std::string name;
    std::u16string name16;
    std::string path;
    Relocation type = Relocation::None;
    u64 original_offset = 0;        // None: absolute in the base image
    std::string replace_file_path;  // Replaced (UTF-8)
    std::vector<u8> patched_file;   // Patched
    u64 size = 0;                   // the relocated file's size
    DirNode* parent = nullptr;
    u32 metadata_offset = 0;        // in the rebuilt file table
    u64 data_offset = 0;            // assigned, relative to the data
    std::shared_ptr<fs::PositionedFile> replace_handle;  // opened when first read (guarded by the image)
    bool replace_open_failed = false;
};

// The hash of a name (3dbrew's), on its UTF-16 units.
u32 CalcHash(const std::u16string& name, u32 parent_offset) {
    u32 hash = parent_offset ^ 123456789;
    for (const char16_t c : name) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= static_cast<u16>(c);
    }
    return hash;
}

std::size_t GetHashTableSize(std::size_t entry_count) {
    if (entry_count < 3) {
        return 3;
    }
    if (entry_count < 19) {
        return entry_count | 1;
    }
    std::size_t count = entry_count;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 ||
           count % 13 == 0 || count % 17 == 0) {
        count++;
    }
    return count;
}

class LayeredImage final : public fs::FileBackend {
public:
    static std::shared_ptr<LayeredImage> Create(std::shared_ptr<BaseReader> base, const std::string& romfs_dir,
                                                const std::string& romfs_ext_dir, std::string* error) {
        std::shared_ptr<LayeredImage> image(new LayeredImage(std::move(base)));
        std::string message;
        if (!image->Load(romfs_dir, romfs_ext_dir, &message)) {
            if (error != nullptr) {
                *error = message;
            }
            MH4N_LOG(Error, kLog, "LayeredFS: {}", message);
            return nullptr;
        }
        return image;
    }

    Result Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) override {
        *read = 0;
        const u64 total = GetSize();
        if (offset >= total || length == 0) {
            return kResultSuccess;
        }
        const std::size_t count = static_cast<std::size_t>(std::min<u64>(length, total - offset));
        ReadImage(static_cast<std::size_t>(offset), count, buffer);
        *read = count;
        return kResultSuccess;
    }
    u64 GetSize() override { return metadata_.size() + current_data_offset_; }

private:
    explicit LayeredImage(std::shared_ptr<BaseReader> base) : base_(std::move(base)) {
        root_.parent = &root_;
    }

    // ---- loading the base image's metadata ----

    bool ReadExactly(u64 offset, u64 length, std::vector<u8>* out, std::string* message, const char* what) {
        if (offset > base_->Size() || length > base_->Size() - offset) {
            *message = std::string("the ") + what + " lies outside the base image";
            return false;
        }
        out->resize(static_cast<std::size_t>(length));
        if (length != 0 && base_->ReadAt(offset, out->data(), out->size()) != out->size()) {
            *message = std::string("the base image is shorter than its ") + what;
            return false;
        }
        return true;
    }

    static std::u16string NameAt(const std::vector<u8>& table, u32 at, u32 name_length) {
        std::u16string name(name_length / 2, u'\0');
        for (std::size_t i = 0; i < name.size(); ++i) {
            name[i] = static_cast<char16_t>(table[at + 2 * i] | table[at + 2 * i + 1] << 8);
        }
        return name;
    }

    // Azahar's LoadDirectory on the tables in memory: this directory, its files, then its child directories
    // (a child's next sibling is read by LoadChild before the child is loaded).
    bool LoadDirectory(DirNode& current, u32 offset, int depth, std::string* message) {
        if (depth > kMaxDirectoryDepth) {
            *message = "the directory table is nested too deeply";
            return false;
        }
        if (offset > directory_table_.size() || directory_table_.size() - offset < kDirectoryEntrySize ||
            ++directories_seen_ > directory_table_.size() / kDirectoryEntrySize) {
            *message = "the directory table is damaged (an entry outside the table, or a loop)";
            return false;
        }
        const u8* entry = &directory_table_[offset];
        const u32 first_child = Le32(entry + 8);
        const u32 first_file = Le32(entry + 12);
        const u32 name_length = Le32(entry + 20);
        if (name_length % 2 != 0 || name_length > directory_table_.size() - offset - kDirectoryEntrySize) {
            *message = "the directory table is damaged (a name outside the table)";
            return false;
        }
        current.name16 = NameAt(directory_table_, offset + kDirectoryEntrySize, name_length);
        current.name = utf8::FromUtf16(current.name16);
        current.path = current.parent->path + current.name + "/";
        directory_path_map_.emplace(current.path, &current);

        u32 file_offset = first_file;
        while (file_offset != kNone) {
            if (!LoadFile(current, file_offset, &file_offset, message)) {
                return false;
            }
        }

        u32 child_offset = first_child;
        while (child_offset != kNone) {
            auto child = std::make_unique<DirNode>();
            DirNode& directory = *child;
            directory.parent = &current;
            current.directories.emplace_back(std::move(child));
            u32 next_child = kNone;
            if (!LoadChild(directory, child_offset, depth + 1, &next_child, message)) {
                return false;
            }
            child_offset = next_child;
        }
        return true;
    }

    // Loads the directory at `offset` and reports the offset of its next sibling.
    bool LoadChild(DirNode& directory, u32 offset, int depth, u32* next_sibling, std::string* message) {
        if (offset > directory_table_.size() || directory_table_.size() - offset < kDirectoryEntrySize) {
            *message = "the directory table is damaged (an entry outside the table)";
            return false;
        }
        *next_sibling = Le32(&directory_table_[offset + 4]);
        return LoadDirectory(directory, offset, depth, message);
    }

    bool LoadFile(DirNode& parent, u32 offset, u32* next_sibling, std::string* message) {
        if (offset > file_table_.size() || file_table_.size() - offset < kFileEntrySize ||
            ++files_seen_ > file_table_.size() / kFileEntrySize) {
            *message = "the file table is damaged (an entry outside the table, or a loop)";
            return false;
        }
        const u8* entry = &file_table_[offset];
        const u32 name_length = Le32(entry + 28);
        if (name_length % 2 != 0 || name_length > file_table_.size() - offset - kFileEntrySize) {
            *message = "the file table is damaged (a name outside the table)";
            return false;
        }
        auto file = std::make_unique<FileNode>();
        file->name16 = NameAt(file_table_, offset + kFileEntrySize, name_length);
        file->name = utf8::FromUtf16(file->name16);
        file->path = parent.path + file->name;
        file->original_offset = u64{file_data_offset_} + Le64(entry + 8);
        file->size = Le64(entry + 16);
        file->parent = &parent;
        *next_sibling = Le32(entry + 4);

        file_path_map_.emplace(file->path, file.get());
        parent.files.emplace_back(std::move(file));
        return true;
    }

    // ---- the layers ----

    static std::u16string ToUtf16(const std::string& name) { return utf8::ToUtf16(name); }

    // Files of romfs/ replace or create files.
    void LoadRelocations(const std::string& patch_path) {
        if (!IsDirectory(patch_path)) {
            return;
        }
        ScanReplacements(utf8::PathFromUtf8(patch_path), root_, "/");
    }

    void ScanReplacements(const stdfs::path& host_dir, DirNode& parent, const std::string& parent_path) {
        std::vector<HostEntry> entries;
        if (!ListHostDirectory(host_dir, &entries)) {
            return;
        }
        for (const HostEntry& entry : entries) {
            if (entry.is_directory) {
                const std::string path = parent_path + entry.name + "/";
                if (!directory_path_map_.count(path)) {  // Add this directory
                    auto child = std::make_unique<DirNode>();
                    child->name = entry.name;
                    child->name16 = ToUtf16(entry.name);
                    child->path = path;
                    child->parent = &parent;
                    directory_path_map_.emplace(path, child.get());
                    parent.directories.emplace_back(std::move(child));
                    ++created_directories_;
                    MH4N_LOG(Debug, kLog, "LayeredFS created directory {}", path);
                }
                ScanReplacements(entry.path, *directory_path_map_.at(path), path);
                continue;
            }

            const std::string path = parent_path + entry.name;
            if (!file_path_map_.count(path)) {  // Newly created file
                auto file = std::make_unique<FileNode>();
                file->name = entry.name;
                file->name16 = ToUtf16(entry.name);
                file->path = path;
                file->parent = &parent;
                file_path_map_.emplace(path, file.get());
                parent.files.emplace_back(std::move(file));
                ++created_files_;
                MH4N_LOG(Debug, kLog, "LayeredFS created file {}", path);
            }

            FileNode& file = *file_path_map_.at(path);
            std::error_code ec;
            const auto size = stdfs::file_size(entry.path, ec);
            file.type = Relocation::Replaced;
            file.replace_file_path = utf8::PathToUtf8(entry.path);
            file.size = ec ? 0 : static_cast<u64>(size);
            ++replaced_files_;
            MH4N_LOG(Debug, kLog, "LayeredFS replacement file in use for {}", path);
        }
    }

    struct ExtFile {
        std::string path;  // "/data/x.arc.ips": below romfs_ext/, with the leading '/'
        stdfs::path host;
    };

    // ScanDirectoryTree(directory, entry, recursion) and GetAllFilesFromNestedEntries: the files below a
    // folder, in the order of the entries, folders entered where they stand. A folder at the recursion limit
    // is not entered.
    static void ScanExt(const stdfs::path& host_dir, const std::string& rel, unsigned recursion,
                        std::vector<ExtFile>* out) {
        std::vector<HostEntry> entries;
        if (!ListHostDirectory(host_dir, &entries)) {
            return;
        }
        for (const HostEntry& entry : entries) {
            if (entry.is_directory) {
                if (recursion > 0) {
                    ScanExt(entry.path, rel + "/" + entry.name, recursion - 1, out);
                }
            } else {
                out->push_back({rel + "/" + entry.name, entry.path});
            }
        }
    }

    // Files of romfs_ext/: `.stub` removes a file, `.ips` and `.bps` patch it.
    void LoadExtRelocations(const std::string& patch_ext_path) {
        if (!IsDirectory(patch_ext_path)) {
            return;
        }
        std::vector<ExtFile> files;
        ScanExt(utf8::PathFromUtf8(patch_ext_path), "", kExtRecursion, &files);

        for (const ExtFile& entry : files) {
            const std::string& path = entry.path;
            const auto ends_with = [&](const char* suffix, std::size_t n) {
                return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
            };
            if (ends_with(".stub", 5)) {
                // Remove the corresponding file if exists
                const std::string file_path = path.substr(0, path.size() - 5);
                const auto found = file_path_map_.find(file_path);
                if (found != file_path_map_.end()) {
                    FileNode& file = *found->second;
                    file.type = Relocation::Removed;
                    file.size = 0;
                    file_path_map_.erase(found);
                    ++removed_files_;
                    MH4N_LOG(Debug, kLog, "LayeredFS removed file {}", file_path);
                } else {
                    MH4N_LOG(Warning, kLog, "LayeredFS file for stub {} not found", path);
                }
            } else if (path.size() >= 4) {
                const bool is_ips = ends_with(".ips", 4);
                if (!is_ips && !ends_with(".bps", 4)) {
                    MH4N_LOG(Warning, kLog, "LayeredFS unknown ext file {}", path);
                }

                // Azahar goes on with any other extension and applies the file as a BPS patch.
                const std::string file_path = path.substr(0, path.size() - 4);
                const auto found = file_path_map_.find(file_path);
                if (found == file_path_map_.end()) {
                    MH4N_LOG(Warning, kLog, "LayeredFS original file for patch {} not found", path);
                    continue;
                }

                std::ifstream patch_file(entry.host, std::ios::binary);
                std::vector<u8> patch_bytes;
                if (patch_file) {
                    patch_bytes.assign(std::istreambuf_iterator<char>(patch_file), std::istreambuf_iterator<char>());
                }
                if (!patch_file || patch_file.bad()) {
                    MH4N_LOG(Error, kLog, "LayeredFS Could not open file {}", path);
                    continue;
                }

                FileNode& file = *found->second;
                // The original size, read from the base image at the file's original offset (for a file the
                // mods replaced or created, whose size is the replacement's, that is what Azahar reads too).
                std::vector<u8> buffer(static_cast<std::size_t>(file.size));
                if (!buffer.empty()) {
                    base_->ReadAt(file.original_offset, buffer.data(), buffer.size());  // short: zeros
                }

                const patch::Status ret = is_ips ? patch::ApplyIps(patch_bytes, buffer) : patch::ApplyBps(patch_bytes, buffer);
                if (ret == patch::Status::Success) {
                    ++patched_files_;
                    MH4N_LOG(Debug, kLog, "LayeredFS patched file {}", file_path);
                    file.type = Relocation::Patched;
                    file.size = buffer.size();
                    file.patched_file = std::move(buffer);
                } else {
                    MH4N_LOG(Error, kLog, "LayeredFS failed to patch file {}", file_path);
                }
            } else {
                MH4N_LOG(Warning, kLog, "LayeredFS unknown ext file {}", path);
            }
        }
    }

    // ---- rebuilding the metadata ----

    // Assigns the offsets of the new tables: the directories and files of `current`'s children, level by level
    // as Azahar's PrepareBuild does.
    void PrepareBuildDirectory(DirNode& current) {
        current.metadata_offset = static_cast<u32>(current_directory_offset_);
        directory_list_.push_back(&current);
        current_directory_offset_ += kDirectoryEntrySize + AlignUp4(current.name16.size() * 2);
    }

    void PrepareBuildFile(FileNode& current) {
        if (current.type == Relocation::Removed) {  // Deleted files are not counted
            return;
        }
        current.metadata_offset = static_cast<u32>(current_file_offset_);
        file_list_.push_back(&current);
        current_file_offset_ += kFileEntrySize + AlignUp4(current.name16.size() * 2);
    }

    void PrepareBuild(DirNode& current) {
        for (const auto& child : current.files) {
            PrepareBuildFile(*child);
        }
        for (const auto& child : current.directories) {
            PrepareBuildDirectory(*child);
        }
        for (const auto& child : current.directories) {
            PrepareBuild(*child);
        }
    }

    // The name, then zeros up to the 4-byte alignment (the table is 0xFF everywhere else).
    static void WriteName(std::vector<u8>& out, std::size_t at, const std::u16string& name) {
        std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(at), AlignUp4(name.size() * 2), u8{0});
        for (std::size_t i = 0; i < name.size(); ++i) {
            out[at + 2 * i] = static_cast<u8>(static_cast<u16>(name[i]));
            out[at + 2 * i + 1] = static_cast<u8>(static_cast<u16>(name[i]) >> 8);
        }
    }

    void BuildDirectories() {
        directory_metadata_table_.assign(static_cast<std::size_t>(current_directory_offset_), 0xFF);

        // The next sibling of a directory is the one after it in its parent's list.
        std::unordered_map<const DirNode*, const DirNode*> next_directory;
        for (const DirNode* directory : directory_list_) {
            for (std::size_t i = 0; i + 1 < directory->directories.size(); ++i) {
                next_directory[directory->directories[i].get()] = directory->directories[i + 1].get();
            }
        }

        std::size_t written = 0;
        for (DirNode* directory : directory_list_) {
            u32 parent_offset = directory->parent->metadata_offset;
            u32 next_sibling = kNone;
            if (directory->parent != directory) {
                const auto next = next_directory.find(directory);
                if (next != next_directory.end()) {
                    next_sibling = next->second->metadata_offset;
                }
            }
            const u32 first_child =
                directory->directories.empty() ? kNone : directory->directories.front()->metadata_offset;
            u32 first_file = kNone;
            for (const auto& file : directory->files) {
                if (file->type != Relocation::Removed) {  // Azahar looks the first file up even if removed
                    first_file = file->metadata_offset;
                    break;
                }
            }

            const std::size_t bucket = CalcHash(directory->name16, parent_offset) % directory_hash_table_.size();
            const u32 hash_next = directory_hash_table_[bucket];
            directory_hash_table_[bucket] = directory->metadata_offset;

            PutLe32(directory_metadata_table_, written + 0, parent_offset);
            PutLe32(directory_metadata_table_, written + 4, next_sibling);
            PutLe32(directory_metadata_table_, written + 8, first_child);
            PutLe32(directory_metadata_table_, written + 12, first_file);
            PutLe32(directory_metadata_table_, written + 16, hash_next);
            PutLe32(directory_metadata_table_, written + 20, static_cast<u32>(directory->name16.size() * 2));
            written += kDirectoryEntrySize;
            WriteName(directory_metadata_table_, written, directory->name16);
            written += AlignUp4(directory->name16.size() * 2);
        }
    }

    void BuildFiles() {
        file_metadata_table_.assign(static_cast<std::size_t>(current_file_offset_), 0xFF);

        // The next sibling of a file is the next one in its folder that was not removed.
        std::unordered_map<const FileNode*, const FileNode*> next_file;
        std::vector<const DirNode*> pending{&root_};
        while (!pending.empty()) {
            const DirNode* directory = pending.back();
            pending.pop_back();
            const FileNode* previous = nullptr;
            for (const auto& file : directory->files) {
                if (file->type == Relocation::Removed) {
                    continue;
                }
                if (previous != nullptr) {
                    next_file[previous] = file.get();
                }
                previous = file.get();
            }
            for (const auto& child : directory->directories) {
                pending.push_back(child.get());
            }
        }

        std::size_t written = 0;
        for (FileNode* file : file_list_) {
            const u32 parent_offset = file->parent->metadata_offset;
            const auto next = next_file.find(file);
            const u32 next_sibling = next != next_file.end() ? next->second->metadata_offset : kNone;

            file->data_offset = current_data_offset_;
            current_data_offset_ += AlignUp16(file->size);
            if (file->size != 0) {
                data_files_.emplace_back(file->data_offset, file);
            }

            const std::size_t bucket = CalcHash(file->name16, parent_offset) % file_hash_table_.size();
            const u32 hash_next = file_hash_table_[bucket];
            file_hash_table_[bucket] = file->metadata_offset;

            PutLe32(file_metadata_table_, written + 0, parent_offset);
            PutLe32(file_metadata_table_, written + 4, next_sibling);
            PutLe64(file_metadata_table_, written + 8, file->data_offset);
            PutLe64(file_metadata_table_, written + 16, file->size);
            PutLe32(file_metadata_table_, written + 24, hash_next);
            PutLe32(file_metadata_table_, written + 28, static_cast<u32>(file->name16.size() * 2));
            written += kFileEntrySize;
            WriteName(file_metadata_table_, written, file->name16);
            written += AlignUp4(file->name16.size() * 2);
        }
    }

    void RebuildMetadata() {
        PrepareBuildDirectory(root_);
        PrepareBuild(root_);

        directory_hash_table_.assign(GetHashTableSize(directory_list_.size()), kNone);
        file_hash_table_.assign(GetHashTableSize(file_list_.size()), kNone);

        BuildDirectories();
        BuildFiles();

        // The header, then the hash tables and the metadata tables, as 3dbrew lays them out.
        const u32 dir_hash_offset = kHeaderSize;
        const u32 dir_hash_length = static_cast<u32>(directory_hash_table_.size() * 4);
        const u32 dir_meta_offset = dir_hash_offset + dir_hash_length;
        const u32 dir_meta_length = static_cast<u32>(directory_metadata_table_.size());
        const u32 file_hash_offset = dir_meta_offset + dir_meta_length;
        const u32 file_hash_length = static_cast<u32>(file_hash_table_.size() * 4);
        const u32 file_meta_offset = file_hash_offset + file_hash_length;
        const u32 file_meta_length = static_cast<u32>(file_metadata_table_.size());
        const u32 data_offset = static_cast<u32>(AlignUp16(u64{file_meta_offset} + file_meta_length));

        metadata_.assign(data_offset, 0);
        PutLe32(metadata_, 0x00, kHeaderSize);
        PutLe32(metadata_, 0x04, dir_hash_offset);
        PutLe32(metadata_, 0x08, dir_hash_length);
        PutLe32(metadata_, 0x0C, dir_meta_offset);
        PutLe32(metadata_, 0x10, dir_meta_length);
        PutLe32(metadata_, 0x14, file_hash_offset);
        PutLe32(metadata_, 0x18, file_hash_length);
        PutLe32(metadata_, 0x1C, file_meta_offset);
        PutLe32(metadata_, 0x20, file_meta_length);
        PutLe32(metadata_, 0x24, data_offset);
        for (std::size_t i = 0; i < directory_hash_table_.size(); ++i) {
            PutLe32(metadata_, dir_hash_offset + 4 * i, directory_hash_table_[i]);
        }
        std::copy(directory_metadata_table_.begin(), directory_metadata_table_.end(),
                  metadata_.begin() + dir_meta_offset);
        for (std::size_t i = 0; i < file_hash_table_.size(); ++i) {
            PutLe32(metadata_, file_hash_offset + 4 * i, file_hash_table_[i]);
        }
        std::copy(file_metadata_table_.begin(), file_metadata_table_.end(), metadata_.begin() + file_meta_offset);
    }

    bool Load(const std::string& romfs_dir, const std::string& romfs_ext_dir, std::string* message) {
        std::vector<u8> header;
        if (!ReadExactly(0, kHeaderSize, &header, message, "RomFS header")) {
            return false;
        }
        if (Le32(header.data()) != kHeaderSize) {
            *message = "the header size of the base image is not 0x28";
            return false;
        }
        const u32 directory_offset = Le32(&header[0x0C]);
        const u32 directory_length = Le32(&header[0x10]);
        const u32 file_offset = Le32(&header[0x1C]);
        const u32 file_length = Le32(&header[0x20]);
        file_data_offset_ = Le32(&header[0x24]);
        // Only the two metadata tables are read: the names and the links. The hash tables are rebuilt.
        if (!ReadExactly(directory_offset, directory_length, &directory_table_, message, "directory table") ||
            !ReadExactly(file_offset, file_length, &file_table_, message, "file table")) {
            return false;
        }

        // TODO in Azahar: "is root always the first directory in table?" Yes, it is the one at offset 0.
        u32 ignored = kNone;
        if (!LoadChild(root_, 0, 0, &ignored, message)) {
            return false;
        }

        LoadRelocations(romfs_dir);
        LoadExtRelocations(romfs_ext_dir);

        RebuildMetadata();
        directory_table_ = {};
        file_table_ = {};
        MH4N_LOG(Info, kLog,
                 "LayeredFS: {} files and {} folders in the image; mods: {} files replaced, {} created, {} patched, "
                 "{} removed, {} folders created",
                 file_list_.size(), directory_list_.size(), replaced_files_, created_files_, patched_files_,
                 removed_files_, created_directories_);
        return true;
    }

    // ---- reading ----

    std::shared_ptr<fs::PositionedFile> ReplacementFile(FileNode& file) {
        std::lock_guard<std::mutex> lock(open_mutex_);
        if (file.replace_handle == nullptr && !file.replace_open_failed) {
            std::string error;
            file.replace_handle = fs::PositionedFile::Open(file.replace_file_path, &error);
            if (file.replace_handle == nullptr) {
                file.replace_open_failed = true;
                MH4N_LOG(Error, kLog, "Could not open replacement file for {}", file.path);
            }
        }
        return file.replace_handle;
    }

    // Azahar's LayeredFS::ReadFile; [offset, offset + length) lies inside the image.
    void ReadImage(std::size_t offset, std::size_t length, u8* buffer) {
        std::size_t read_size = 0;
        if (offset < metadata_.size()) {
            // First read the metadata
            const std::size_t to_read = std::min(metadata_.size() - offset, length);
            std::memcpy(buffer, metadata_.data() + offset, to_read);
            read_size += to_read;
            offset = 0;
        } else {
            offset -= metadata_.size();
        }
        if (read_size >= length) {
            return;
        }

        // Read files: from the one that holds `offset` on, each in the way its relocation says.
        auto current = std::upper_bound(data_files_.begin(), data_files_.end(), u64{offset},
                                        [](u64 value, const std::pair<u64, FileNode*>& entry) {
                                            return value < entry.first;
                                        });
        if (current == data_files_.begin()) {
            std::memset(buffer + read_size, 0, length - read_size);  // cannot happen inside the image
            return;
        }
        --current;
        while (read_size < length) {
            FileNode& file = *current->second;
            const std::size_t relative_offset = static_cast<std::size_t>(offset - current->first);
            std::size_t to_read = 0;
            if (file.size > relative_offset) {
                to_read = static_cast<std::size_t>(std::min<u64>(file.size - relative_offset, length - read_size));
            }
            const std::size_t alignment =
                static_cast<std::size_t>(std::min<u64>(AlignUp16(file.size) - relative_offset, length - read_size)) -
                to_read;

            u8* destination = buffer + read_size;
            std::size_t got = 0;
            switch (file.type) {
            case Relocation::None:
                got = base_->ReadAt(file.original_offset + relative_offset, destination, to_read);
                break;
            case Relocation::Replaced: {
                const auto handle = to_read != 0 ? ReplacementFile(file) : nullptr;
                if (handle != nullptr) {
                    got = handle->ReadAt(relative_offset, destination, to_read);
                }
                break;
            }
            case Relocation::Patched:
                std::memcpy(destination, file.patched_file.data() + relative_offset, to_read);
                got = to_read;
                break;
            case Relocation::Removed:
                break;  // never in data_files_
            }
            // What the source could not give (a short base, a replacement that shrank) reads as zeros.
            std::memset(destination + got, 0, to_read - got + alignment);

            read_size += to_read + alignment;
            offset += to_read + alignment;
            ++current;
            if (current == data_files_.end() && read_size < length) {
                std::memset(buffer + read_size, 0, length - read_size);  // cannot happen inside the image
                return;
            }
        }
    }

    std::shared_ptr<BaseReader> base_;
    std::vector<u8> directory_table_;  // the base's tables, kept until the build is done
    std::vector<u8> file_table_;
    u32 file_data_offset_ = 0;
    std::size_t directories_seen_ = 0;
    std::size_t files_seen_ = 0;

    DirNode root_;
    std::unordered_map<std::string, FileNode*> file_path_map_;
    std::unordered_map<std::string, DirNode*> directory_path_map_;

    std::vector<u8> metadata_;  // the header, the hash tables and the metadata tables
    std::vector<std::pair<u64, FileNode*>> data_files_;  // assigned data offset -> file, ascending
    std::vector<u32> directory_hash_table_;
    std::vector<u32> file_hash_table_;
    std::vector<DirNode*> directory_list_;  // the sequence of directories written to the table
    std::vector<FileNode*> file_list_;
    u64 current_directory_offset_ = 0;
    u64 current_file_offset_ = 0;
    std::vector<u8> directory_metadata_table_;
    std::vector<u8> file_metadata_table_;
    u64 current_data_offset_ = 0;

    std::size_t created_files_ = 0, created_directories_ = 0, replaced_files_ = 0, patched_files_ = 0,
                removed_files_ = 0;
    std::mutex open_mutex_;
};

}  // namespace

std::shared_ptr<BaseReader> MakeBaseReader(std::shared_ptr<fs::PositionedFile> file, u64 offset, u64 size) {
    return std::make_shared<SliceReader>(std::move(file), offset, size);
}

std::shared_ptr<fs::FileBackend> Build(std::shared_ptr<BaseReader> base, const std::string& romfs_dir,
                                       const std::string& romfs_ext_dir, std::string* error) {
    return LayeredImage::Create(std::move(base), romfs_dir, romfs_ext_dir, error);
}

std::shared_ptr<fs::FileBackend> Build(std::shared_ptr<fs::PositionedFile> base, u64 offset, u64 size,
                                       const std::string& romfs_dir, const std::string& romfs_ext_dir,
                                       std::string* error) {
    if (base == nullptr || offset > base->Size()) {
        if (error != nullptr) {
            *error = "the base image is missing or its offset is past the end of the file";
        }
        return nullptr;
    }
    size = std::min<u64>(size, base->Size() - offset);
    return Build(MakeBaseReader(std::move(base), offset, size), romfs_dir, romfs_ext_dir, error);
}

}  // namespace mh4n::layeredfs
