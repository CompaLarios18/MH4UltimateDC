// Derived from Azahar src/core/file_sys/savedata_archive.cpp, src/core/file_sys/archive_source_sd_savedata.cpp,
// src/core/file_sys/archive_savedata.cpp, src/core/file_sys/archive_other_savedata.cpp,
// src/core/file_sys/disk_archive.cpp, src/core/hle/service/fs/archive.cpp and
// src/core/hle/service/fs/archive.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: archive.h
// says "GPLv3 or any later version", the source of the media types; the others say "GPLv2 or any later
// version"); see the header of save_archives (the header file) for what is taken from them.
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (savedata_archive.cpp,
//   archive_other_savedata.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (archive_source_sd_savedata.cpp,
//   archive_savedata.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (disk_archive.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (archive.cpp, archive.h)
//
// SaveData (archive 4) and OtherSaveDataPermitted / OtherSaveDataGeneral (0x567890B4 / 0x567890B2), and
// the archive they open (N1-13). Departures from Azahar:
//  - The three padding bytes of the format info are zero (Azahar writes whatever its stack held).
//  - A CreateFile that cannot be allocated removes the half-made file; Azahar leaves it, and the game's
//    next try then finds "file already exists".
//  - CreateDirectory builds the host path as the other operations do (Azahar appends the path text to the
//    mount point as it came), which differs only for paths with "." / ".." / repeated slashes.
//  - A listing is never longer than the client's buffer (see fs_dir.cpp), and the macOS name normalisation
//    of Azahar's DiskDirectory::Read is not done.
//  - Without --user the archives answer as if the disk were empty (NotFormatted) and a format is refused.
#include "save_archives.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <fmt/format.h>

#include "disk_file.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"

namespace mh4n::fs {

namespace {
constexpr const char* kLog = "Service.FS";
}

std::array<u8, 16> EncodeFormatInfo(const ArchiveFormatInfo& info) {
    std::array<u8, 16> bytes{};
    const u32 words[3] = {info.total_size, info.number_directories, info.number_files};
    for (int i = 0; i < 3; ++i) {
        for (int b = 0; b < 4; ++b) {
            bytes[static_cast<std::size_t>(i * 4 + b)] = static_cast<u8>(words[i] >> (8 * b));
        }
    }
    bytes[12] = info.duplicate_data;
    return bytes;
}

std::string SdmcDirectory(const FsState& state) {
    return state.user_folder.empty() ? std::string() : state.user_folder + "sdmc/";
}

std::string NandDirectory(const FsState& state) {
    return state.user_folder.empty() ? std::string() : state.user_folder + "nand/";
}

namespace {
std::string SaveDataContainerPath(const std::string& sdmc_directory) {
    return fmt::format("{}Nintendo 3DS/{}/{}/title/", sdmc_directory, kSystemId, kSdCardId);
}
}  // namespace

std::string SaveDataPath(const std::string& sdmc_directory, u64 program_id) {
    return fmt::format("{}{:08x}/{:08x}/data/00000001/", SaveDataContainerPath(sdmc_directory),
                       static_cast<u32>(program_id >> 32), static_cast<u32>(program_id & 0xFFFFFFFF));
}

std::string SaveDataMetadataPath(const std::string& sdmc_directory, u64 program_id) {
    return fmt::format("{}{:08x}/{:08x}/data/00000001.metadata", SaveDataContainerPath(sdmc_directory),
                       static_cast<u32>(program_id >> 32), static_cast<u32>(program_id & 0xFFFFFFFF));
}

// ---- the directory tree archive ---------------------------------------------------------------------------

Result SaveDataArchive::OpenFile(const Path& path, u32 mode, u32 attributes, std::shared_ptr<FileBackend>* file) {
    (void)attributes;
    const PathParser parser(path);
    if (!parser.IsValid()) {
        MH4N_LOG(Error, kLog, "OpenFile: invalid path");
        return kResultInvalidPath;
    }
    const bool write = (mode & 2) != 0, create = (mode & 4) != 0;
    if (mode == 0) {
        MH4N_LOG(Error, kLog, "OpenFile: empty open mode");
        return kResultUnsupportedOpenFlags;
    }
    if (create && !write) {
        MH4N_LOG(Error, kLog, "OpenFile: the create flag is set but not the write flag");
        return kResultUnsupportedOpenFlags;
    }
    const std::string full_path = parser.BuildHostPath(mount_point_);
    switch (parser.GetHostStatus(mount_point_)) {
    case PathParser::InvalidMountPoint:
        MH4N_LOG(Critical, kLog, "(unreachable) invalid mount point {}", mount_point_);
        return kResultFileNotFound;
    case PathParser::PathNotFound:
        MH4N_LOG(Error, kLog, "OpenFile: path not found {}", full_path);
        return kResultPathNotFound;
    case PathParser::FileInPath:
    case PathParser::DirectoryFound:
        MH4N_LOG(Error, kLog, "OpenFile: unexpected file or directory in {}", full_path);
        return kResultUnexpectedFileOrDirectory;
    case PathParser::NotFound:
        if (!create) {
            MH4N_LOG(Error, kLog, "OpenFile: {} does not exist and the create flag is not set", full_path);
            return kResultFileNotFound;
        }
        host::CreateEmptyFile(full_path);
        break;
    case PathParser::FileFound:
        break;
    }
    std::string error;
    std::shared_ptr<WritableFile> opened = WritableFile::Open(full_path, write, &error);
    if (opened == nullptr) {
        MH4N_LOG(Critical, kLog, "(unreachable) {}", error);
        return kResultFileNotFound;
    }
    *file = std::make_shared<DiskFile>(std::move(opened), mode);
    return kResultSuccess;
}

Result SaveDataArchive::FileDelete(const Path& path) {
    const PathParser parser(path);
    if (!parser.IsValid()) {
        return kResultInvalidPath;
    }
    const std::string full_path = parser.BuildHostPath(mount_point_);
    switch (parser.GetHostStatus(mount_point_)) {
    case PathParser::InvalidMountPoint:
        MH4N_LOG(Critical, kLog, "(unreachable) invalid mount point {}", mount_point_);
        return kResultFileNotFound;
    case PathParser::PathNotFound:
        MH4N_LOG(Error, kLog, "DeleteFile: path not found {}", full_path);
        return kResultPathNotFound;
    case PathParser::FileInPath:
    case PathParser::DirectoryFound:
    case PathParser::NotFound:
        MH4N_LOG(Error, kLog, "DeleteFile: file not found {}", full_path);
        return kResultFileNotFound;
    case PathParser::FileFound:
        break;
    }
    if (host::RemoveFile(full_path)) {
        return kResultSuccess;
    }
    MH4N_LOG(Critical, kLog, "(unreachable) unknown error deleting {}", full_path);
    return kResultFileNotFound;
}

namespace {
template <class Deleter>
Result DeleteDirectoryHelper(const Path& path, const std::string& mount_point, Deleter deleter) {
    const PathParser parser(path);
    if (!parser.IsValid()) {
        return kResultInvalidPath;
    }
    if (parser.IsRootDirectory()) {
        return kResultDirectoryNotEmpty;
    }
    const std::string full_path = parser.BuildHostPath(mount_point);
    switch (parser.GetHostStatus(mount_point)) {
    case PathParser::InvalidMountPoint:
        MH4N_LOG(Critical, kLog, "(unreachable) invalid mount point {}", mount_point);
        return kResultPathNotFound;
    case PathParser::PathNotFound:
    case PathParser::NotFound:
        MH4N_LOG(Error, kLog, "DeleteDirectory: path not found {}", full_path);
        return kResultPathNotFound;
    case PathParser::FileInPath:
    case PathParser::FileFound:
        MH4N_LOG(Error, kLog, "DeleteDirectory: unexpected file or directory {}", full_path);
        return kResultUnexpectedFileOrDirectory;
    case PathParser::DirectoryFound:
        break;
    }
    if (deleter(full_path)) {
        return kResultSuccess;
    }
    MH4N_LOG(Error, kLog, "DeleteDirectory: directory not empty {}", full_path);
    return kResultDirectoryNotEmpty;
}
}  // namespace

Result SaveDataArchive::DirectoryDelete(const Path& path) {
    return DeleteDirectoryHelper(path, mount_point_, [](const std::string& p) { return host::RemoveEmptyDirectory(p); });
}

Result SaveDataArchive::DirectoryDeleteRecursively(const Path& path) {
    return DeleteDirectoryHelper(path, mount_point_,
                                 [](const std::string& p) { return host::DeleteDirectoryRecursively(p); });
}

Result SaveDataArchive::FileCreate(const Path& path, u64 size, u32 attributes) {
    (void)attributes;
    const PathParser parser(path);
    if (!parser.IsValid()) {
        return kResultInvalidPath;
    }
    const std::string full_path = parser.BuildHostPath(mount_point_);
    switch (parser.GetHostStatus(mount_point_)) {
    case PathParser::InvalidMountPoint:
        MH4N_LOG(Critical, kLog, "(unreachable) invalid mount point {}", mount_point_);
        return kResultFileNotFound;
    case PathParser::PathNotFound:
        MH4N_LOG(Error, kLog, "CreateFile: path not found {}", full_path);
        return kResultPathNotFound;
    case PathParser::FileInPath:
        MH4N_LOG(Error, kLog, "CreateFile: unexpected file in path {}", full_path);
        return kResultUnexpectedFileOrDirectory;
    case PathParser::DirectoryFound:
    case PathParser::FileFound:
        MH4N_LOG(Error, kLog, "CreateFile: {} already exists", full_path);
        return kResultFileAlreadyExists;
    case PathParser::NotFound:
        break;
    }
    if (size == 0) {
        if (allow_zero_size_create_) {
            host::CreateEmptyFile(full_path);
            return kResultSuccess;
        }
        MH4N_LOG(Debug, kLog, "CreateFile: a zero-size file is not supported");
        return kResultUnsupportedOpenFlags;
    }
    // A sparse file (or a normal one where the host has no such thing): seek to the size, write one null byte.
    if (host::CreateEmptyFile(full_path)) {
        std::string error;
        if (const std::shared_ptr<WritableFile> file = WritableFile::Open(full_path, true, &error)) {
            const u8 zero = 0;
            bool failed = false;
            if (file->WriteAt(size - 1, &zero, 1, &failed) == 1 && file->Close()) {
                return kResultSuccess;
            }
        }
        host::RemoveFile(full_path);  // not a half-made file
    }
    MH4N_LOG(Error, kLog, "CreateFile: too large a file ({} bytes)", size);
    return kResultTooLarge;
}

Result SaveDataArchive::DirectoryCreate(const Path& path, u32 attributes) {
    (void)attributes;
    const PathParser parser(path);
    if (!parser.IsValid()) {
        return kResultInvalidPath;
    }
    const std::string full_path = parser.BuildHostPath(mount_point_);
    switch (parser.GetHostStatus(mount_point_)) {
    case PathParser::InvalidMountPoint:
        MH4N_LOG(Critical, kLog, "(unreachable) invalid mount point {}", mount_point_);
        return kResultFileNotFound;
    case PathParser::PathNotFound:
        MH4N_LOG(Error, kLog, "CreateDirectory: path not found {}", full_path);
        return kResultPathNotFound;
    case PathParser::FileInPath:
        MH4N_LOG(Error, kLog, "CreateDirectory: unexpected file in path {}", full_path);
        return kResultUnexpectedFileOrDirectory;
    case PathParser::DirectoryFound:
    case PathParser::FileFound:
        MH4N_LOG(Error, kLog, "CreateDirectory: {} already exists", full_path);
        return kResultDirectoryAlreadyExists;
    case PathParser::NotFound:
        break;
    }
    if (host::CreateDir(full_path)) {
        return kResultSuccess;
    }
    MH4N_LOG(Critical, kLog, "(unreachable) unknown error creating {}", full_path);
    return kResultCreateDirectoryFailed;
}

Result SaveDataArchive::DirectoryOpen(const Path& path, std::unique_ptr<DirectoryBackend>* directory) {
    const PathParser parser(path);
    if (!parser.IsValid()) {
        return kResultInvalidPath;
    }
    const std::string full_path = parser.BuildHostPath(mount_point_);
    switch (parser.GetHostStatus(mount_point_)) {
    case PathParser::InvalidMountPoint:
        MH4N_LOG(Critical, kLog, "(unreachable) invalid mount point {}", mount_point_);
        return kResultFileNotFound;
    case PathParser::PathNotFound:
    case PathParser::NotFound:
        MH4N_LOG(Error, kLog, "OpenDirectory: path not found {}", full_path);
        return kResultPathNotFound;
    case PathParser::FileInPath:
    case PathParser::FileFound:
        MH4N_LOG(Error, kLog, "OpenDirectory: unexpected file in path {}", full_path);
        return kResultUnexpectedFileOrDirectory;
    case PathParser::DirectoryFound:
        break;
    }
    *directory = OpenDiskDirectory(full_path);
    return kResultSuccess;
}

Result SaveDataArchive::Control(u32 action, const u8* input, std::size_t input_size, u8* output,
                                std::size_t output_size) {
    (void)input, (void)output;
    MH4N_LOG(Debug, kLog, "ControlArchive {}: action={:08X} input_size={:08X} output_size={:08X}", GetName(), action,
             input_size, output_size);
    host::SyncDirectoryTree(mount_point_);
    return kResultSuccess;
}

// ---- directories ------------------------------------------------------------------------------------------------

namespace {

class DiskDirectory final : public DirectoryBackend {
public:
    explicit DiskDirectory(const std::string& host_path) {
        // Taken once, when the directory is opened (Azahar scans the tree then too).
        if (auto children = host::ListDirectory(host_path)) {
            children_ = std::move(*children);
        }
    }

    u32 Read(u32 count, DirEntry* entries) override {
        u32 read = 0;
        while (read < count && next_ < children_.size()) {
            const host::Child& child = children_[next_];
            DirEntry& entry = entries[read];
            std::memset(&entry, 0, sizeof(entry));
            const std::u16string name = Utf8ToUtf16(child.name);
            const std::size_t copy = std::min(name.size(), kFilenameLength - 1);
            std::copy_n(name.begin(), copy, entry.filename);
            SplitFilename83(child.name, entry.short_name, entry.extension);
            entry.is_directory = child.is_directory;
            entry.is_hidden = !child.name.empty() && child.name[0] == '.';
            entry.is_read_only = 0;
            entry.file_size = child.size;
            // An SD card whose archive bit was never cleared; "some homebrews mistakenly use the archive bit as
            // a file bit".
            entry.is_archive = !child.is_directory;
            ++read;
            ++next_;
        }
        return read;
    }

private:
    std::vector<host::Child> children_;
    std::size_t next_ = 0;
};

}  // namespace

std::unique_ptr<DirectoryBackend> OpenDiskDirectory(const std::string& host_path) {
    return std::make_unique<DiskDirectory>(host_path);
}

// ---- SaveData, OtherSaveData -----------------------------------------------------------------------------------

namespace {

// Opens a title's save: NotFormatted until it has been formatted (Azahar's ArchiveSource_SDSaveData::Open).
Result OpenSdSave(const FsState& state, u64 program_id, std::unique_ptr<ArchiveBackend>* archive) {
    const std::string sdmc = SdmcDirectory(state);
    const std::string concrete = sdmc.empty() ? std::string() : SaveDataPath(sdmc, program_id);
    if (concrete.empty() || !host::Exists(concrete)) {
        // Not formatted yet: the game answers by formatting it.
        return kResultNotFormatted;
    }
    *archive = std::make_unique<SaveDataArchive>(concrete);
    return kResultSuccess;
}

// ArchiveSource_SDSaveData::Format: the save's folder is deleted and made again, and the format info is
// written next to it. A metadata file that cannot be written is not an error (Azahar's answer).
Result FormatSdSave(const FsState& state, u64 program_id, const ArchiveFormatInfo& info) {
    const std::string sdmc = SdmcDirectory(state);
    if (sdmc.empty()) {
        MH4N_LOG(Error, kLog, "FormatSaveData: there is no user folder (--user)");
        return kResultHostFailure;
    }
    const std::string concrete = SaveDataPath(sdmc, program_id);
    host::DeleteDirectoryRecursively(concrete);
    if (!host::CreateFullPath(concrete)) {
        MH4N_LOG(Error, kLog, "FormatSaveData: cannot make {}", concrete);
    }
    const std::array<u8, 16> bytes = EncodeFormatInfo(info);
    if (!host::WriteFileDurably(SaveDataMetadataPath(sdmc, program_id), bytes.data(), bytes.size())) {
        MH4N_LOG(Warning, kLog, "FormatSaveData: cannot write the metadata of {}", concrete);
    }
    host::SyncDirectory(concrete);
    return kResultSuccess;
}

class SaveDataFactory final : public ArchiveFactory {
public:
    explicit SaveDataFactory(FsState& state) : state_(state) {}
    std::string GetName() const override { return "SaveData"; }

    Result Open(const Path& path, u64 program_id, std::unique_ptr<ArchiveBackend>* archive) override {
        (void)path;
        return OpenSdSave(state_, program_id, archive);
    }

    Result Format(const Path& path, const ArchiveFormatInfo& info, u64 program_id, u32 directory_buckets,
                  u32 file_buckets) override {
        (void)path, (void)directory_buckets, (void)file_buckets;
        return FormatSdSave(state_, program_id, info);
    }

private:
    FsState& state_;
};


// The archive path of OtherSaveData: binary, 12 bytes (media type, then two words that name the title).
struct OtherPath {
    u32 media_type = 0;
    u32 word1 = 0, word2 = 0;
};

Result ParseOtherPath(const Path& path, OtherPath* out) {
    if (path.Type() != LowPathType::Binary) {
        MH4N_LOG(Error, kLog, "OtherSaveData: wrong path type {}", path.raw_type);
        return kResultInvalidPath;
    }
    if (path.data.size() != 12) {
        MH4N_LOG(Error, kLog, "OtherSaveData: wrong path length {}", path.data.size());
        return kResultInvalidPath;
    }
    std::memcpy(&out->media_type, path.data.data(), 4);
    std::memcpy(&out->word1, path.data.data() + 4, 4);
    std::memcpy(&out->word2, path.data.data() + 8, 4);
    if (out->media_type != kMediaSdmc && out->media_type != kMediaGameCard) {
        MH4N_LOG(Error, kLog, "OtherSaveData: unsupported media type {}", out->media_type);
        // "this is strange, but the error code was verified with a real 3DS"
        return kResultUnsupportedOpenFlags;
    }
    return kResultSuccess;
}

class OtherSaveDataFactory final : public ArchiveFactory {
public:
    OtherSaveDataFactory(FsState& state, bool permitted) : state_(state), permitted_(permitted) {}
    std::string GetName() const override { return permitted_ ? "OtherSaveDataPermitted" : "OtherSaveDataGeneral"; }

    Result Open(const Path& path, u64 client_program_id, std::unique_ptr<ArchiveBackend>* archive) override {
        (void)client_program_id;
        OtherPath parsed;
        if (const Result result = ParseOtherPath(path, &parsed); IsError(result)) {
            return result;
        }
        if (parsed.media_type == kMediaGameCard) {
            MH4N_LOG(Warning, kLog, "(STUBBED) Unimplemented media type GameCard");
            return kResultGamecardNotInserted;
        }
        // Permitted: the title is 0x0004000000000000 | word1 << 8, the shift done on 32 bits as in Azahar.
        const u64 program_id = permitted_ ? (static_cast<u64>(static_cast<u32>(parsed.word1 << 8)) | 0x0004000000000000ULL)
                                          : (parsed.word1 | (static_cast<u64>(parsed.word2) << 32));
        return OpenSdSave(state_, program_id, archive);
    }

    // Permitted refuses ("Attempted to format a OtherSaveDataPermitted archive"). General formats the SD
    // save of its title, as ArchiveFactory_OtherSaveDataGeneral::Format does. FormatSaveData only lets
    // archive 4 through today, so neither is reached from the game; the code follows Azahar for when it is.
    Result Format(const Path& path, const ArchiveFormatInfo& info, u64 program_id, u32 directory_buckets,
                  u32 file_buckets) override {
        (void)program_id, (void)directory_buckets, (void)file_buckets;
        if (permitted_) {
            return kResultInvalidPath;
        }
        OtherPath parsed;
        if (const Result result = ParseOtherPath(path, &parsed); IsError(result)) {
            return result;
        }
        if (parsed.media_type == kMediaGameCard) {
            MH4N_LOG(Warning, kLog, "(STUBBED) Unimplemented media type GameCard");
            return kResultGamecardNotInserted;
        }
        return FormatSdSave(state_, parsed.word1 | (static_cast<u64>(parsed.word2) << 32), info);
    }

private:
    FsState& state_;
    bool permitted_;
};

MH4N_REGISTER(ArchiveProvider, "04.savedata",
              ArchiveProvider{archive_id::kSaveData, [](FsState& state) { return std::make_unique<SaveDataFactory>(state); }});
MH4N_REGISTER(ArchiveProvider, "567890b2.othersavedatageneral",
              ArchiveProvider{archive_id::kOtherSaveDataGeneral,
                              [](FsState& state) { return std::make_unique<OtherSaveDataFactory>(state, false); }});
MH4N_REGISTER(ArchiveProvider, "567890b4.othersavedatapermitted",
              ArchiveProvider{archive_id::kOtherSaveDataPermitted,
                              [](FsState& state) { return std::make_unique<OtherSaveDataFactory>(state, true); }});

}  // namespace

}  // namespace mh4n::fs
