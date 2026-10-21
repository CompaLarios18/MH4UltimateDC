// Derived from Azahar src/core/file_sys/archive_extsavedata.cpp, src/core/hle/service/fs/archive.cpp,
// src/core/hle/service/fs/archive.h and src/core/file_sys/errors.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: archive.h says "GPLv3 or any later version";
// the others say "GPLv2 or any later version"): the extdata archives (ExtSaveData 6, SharedExtSaveData 7,
// BossExtSaveData 0x12345678): the paths (<HIGH>/<LOW>/user/ and boss/, a metadata file, an icon file;
// the shared kind lives in the NAND folder and always has the high word 0x48000), what each answers for an
// archive that is not there, the file whose size is fixed, the open that always reads and writes and
// refuses the create flag, and CreateExtSaveData / DeleteExtSaveData.
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (archive_extsavedata.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (archive.cpp, archive.h)
// Copyright 2016 Citra Emulator Project (errors.h)
//
// Extdata (N1-13). Departures from Azahar:
//  - An archive path shorter than the 12 bytes of an extdata id is "invalid path"; Azahar reads past its end.
//  - Without --user an extdata is not there and creating one is refused (host failure).
// Like Azahar, the icon of CreateExtSaveData is written under the id as the game gave it, while everything
// else of a shared extdata is under the id with the high word 0x48000; so the icon of a shared extdata whose
// high word is not 0x48000 is lost, as it is in Azahar.
#include <cstring>

#include <fmt/format.h>

#include "disk_file.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "save_archives.h"

namespace mh4n::fs {

namespace {

constexpr const char* kLog = "Service.FS";

enum class ExtType { Normal, Shared, Boss };
constexpr u32 kSharedExtDataHigh = 0x48000;

struct ExtId {
    u32 media_type = 0;
    u32 save_low = 0;
    u32 save_high = 0;
};

bool ParseExtId(const Path& path, ExtId* out) {
    const std::vector<u8> bytes = PathAsBinary(path);
    if (bytes.size() < 12) {
        return false;
    }
    std::memcpy(&out->media_type, bytes.data(), 4);
    std::memcpy(&out->save_low, bytes.data() + 4, 4);
    std::memcpy(&out->save_high, bytes.data() + 8, 4);
    return true;
}

std::string ExtSaveDataFolder(const std::string& mount_point, const ExtId& id) {
    return fmt::format("{}{:08X}/{:08X}/", mount_point, id.save_high, id.save_low);
}

std::string ExtDataContainerPath(const std::string& directory, bool shared) {
    if (directory.empty()) {
        return {};
    }
    if (shared) {
        return fmt::format("{}data/{}/extdata/", directory, kSystemId);
    }
    return fmt::format("{}Nintendo 3DS/{}/{}/extdata/", directory, kSystemId, kSdCardId);
}

// The archive: always read and write, no create flag, a fixed size for each file.
class ExtSaveDataArchive final : public SaveDataArchive {
public:
    explicit ExtSaveDataArchive(std::string mount_point) : SaveDataArchive(std::move(mount_point), false) {}

    std::string GetName() const override { return "ExtSaveDataArchive: " + mount_point_; }
    u64 GetOpenDelayNs() override { return kExtSaveOpenDelayNs; }

    Result OpenFile(const Path& path, u32 mode, u32 attributes, std::shared_ptr<FileBackend>* file) override {
        (void)attributes;
        const PathParser parser(path);
        if (!parser.IsValid()) {
            MH4N_LOG(Error, kLog, "OpenFile: invalid path");
            return kResultInvalidPath;
        }
        if (mode == 0) {
            MH4N_LOG(Error, kLog, "OpenFile: empty open mode");
            return kResultUnsupportedOpenFlags;
        }
        if ((mode & 4) != 0) {
            MH4N_LOG(Error, kLog, "OpenFile: the create flag is not supported");
            return kResultUnsupportedOpenFlags;
        }
        const std::string full_path = parser.BuildHostPath(mount_point_);
        switch (parser.GetHostStatus(mount_point_)) {
        case PathParser::InvalidMountPoint:
            MH4N_LOG(Critical, kLog, "(unreachable) invalid mount point {}", mount_point_);
            return kResultFileNotFound;
        case PathParser::PathNotFound:
            MH4N_LOG(Debug, kLog, "OpenFile: path not found {}", full_path);
            return kResultPathNotFound;
        case PathParser::FileInPath:
        case PathParser::DirectoryFound:
            MH4N_LOG(Debug, kLog, "OpenFile: unexpected file or directory in {}", full_path);
            return kResultUnexpectedFileOrDirectory;
        case PathParser::NotFound:
            MH4N_LOG(Debug, kLog, "OpenFile: {} not found", full_path);
            return kResultFileNotFound;
        case PathParser::FileFound:
            break;
        }
        std::string error;
        std::shared_ptr<WritableFile> opened = WritableFile::Open(full_path, true, &error);
        if (opened == nullptr) {
            MH4N_LOG(Critical, kLog, "(unreachable) {}", error);
            return kResultFileNotFound;
        }
        *file = std::make_shared<FixSizeDiskFile>(std::move(opened), 3u);
        return kResultSuccess;
    }
};

class ExtSaveDataFactory final : public ArchiveFactory {
public:
    ExtSaveDataFactory(FsState& state, ExtType type)
        : state_(state), type_(type),
          mount_point_(ExtDataContainerPath(type == ExtType::Shared ? NandDirectory(state) : SdmcDirectory(state),
                                            type == ExtType::Shared)) {}

    std::string GetName() const override {
        return type_ == ExtType::Normal ? "ExtSaveData" : type_ == ExtType::Shared ? "SharedExtSaveData" : "BossExtSaveData";
    }

    Result Open(const Path& path, u64 program_id, std::unique_ptr<ArchiveBackend>* archive) override {
        (void)program_id;
        ExtId id;
        if (!ParseExtId(path, &id)) {
            return kResultInvalidPath;
        }
        if (type_ == ExtType::Shared) {
            id.save_high = kSharedExtDataHigh;  // "The FS module overwrites the high value of the saveid"
        }
        const std::string full = mount_point_.empty() ? std::string()
                                                      : ExtSaveDataFolder(mount_point_, id) +
                                                            (type_ == ExtType::Boss ? "boss/" : "user/");
        if (full.empty() || !host::Exists(full)) {
            // "ExtSaveData seems to return FS_NotFound (120) when the archive doesn't exist."
            return type_ != ExtType::Shared ? kResultNotFoundInvalidState : kResultNotFormatted;
        }
        *archive = std::make_unique<ExtSaveDataArchive>(full);
        return kResultSuccess;
    }

    Result FormatAsExtData(const Path& path, const ArchiveFormatInfo& info, const std::vector<u8>& icon) override {
        ExtId id;
        if (!ParseExtId(path, &id)) {
            return kResultInvalidPath;
        }
        if (mount_point_.empty()) {
            MH4N_LOG(Error, kLog, "CreateExtSaveData: there is no user folder (--user)");
            return kResultHostFailure;
        }
        ExtId corrected = id;
        if (type_ == ExtType::Shared) {
            corrected.save_high = kSharedExtDataHigh;
        }
        // These folders are always made with an extdata.
        const std::string base = ExtSaveDataFolder(mount_point_, corrected);
        host::CreateFullPath(base + "user/");
        host::CreateFullPath(base + "boss/");

        const std::array<u8, 16> bytes = EncodeFormatInfo(info);
        if (!host::WriteFileDurably(base + "metadata", bytes.data(), bytes.size())) {
            return kResultUnknown;  // Azahar: "TODO(Subv): Find the correct error code"
        }
        // The icon goes under the id as given (see the top of this file); a folder that is not there makes
        // the write fail, quietly.
        host::WriteFileDurably(ExtSaveDataFolder(mount_point_, id) + "icon", icon.data(), icon.size());
        host::SyncDirectoryTree(base);
        return kResultSuccess;
    }

    Result DeleteExtData(u32 media_type, u32 high, u32 low) override {
        std::string media_directory;
        if (media_type == kMediaNand) {
            media_directory = NandDirectory(state_);
        } else if (media_type == kMediaSdmc) {
            media_directory = SdmcDirectory(state_);
        } else {
            MH4N_LOG(Error, kLog, "DeleteExtSaveData: unsupported media type {}", media_type);
            return kResultUnknown;  // Azahar: "TODO(Subv): Find the right error code"
        }
        if (media_directory.empty()) {
            return kResultSuccess;  // no user folder: there is nothing to delete
        }
        const std::string base = ExtDataContainerPath(media_directory, media_type == kMediaNand);
        ExtId id;
        id.media_type = media_type, id.save_low = low, id.save_high = high;
        const std::string folder = ExtSaveDataFolder(base, id);
        if (host::Exists(folder) && !host::DeleteDirectoryRecursively(folder)) {
            return kResultUnknown;
        }
        return kResultSuccess;
    }

private:
    FsState& state_;
    ExtType type_;
    std::string mount_point_;
};

MH4N_REGISTER(ArchiveProvider, "06.extsavedata", ArchiveProvider{archive_id::kExtSaveData, [](FsState& state) {
                                                                    return std::make_unique<ExtSaveDataFactory>(state, ExtType::Normal);
                                                                }});
MH4N_REGISTER(ArchiveProvider, "07.sharedextsavedata", ArchiveProvider{archive_id::kSharedExtSaveData, [](FsState& state) {
                                                                          return std::make_unique<ExtSaveDataFactory>(state, ExtType::Shared);
                                                                      }});
MH4N_REGISTER(ArchiveProvider, "12345678.bossextsavedata", ArchiveProvider{archive_id::kBossExtSaveData, [](FsState& state) {
                                                                              return std::make_unique<ExtSaveDataFactory>(state, ExtType::Boss);
                                                                          }});

}  // namespace

Path ConstructExtDataBinaryPath(u32 media_type, u32 high, u32 low) {
    Path path;
    path.raw_type = static_cast<u32>(LowPathType::Binary);
    path.data.resize(12);
    std::memcpy(path.data.data(), &media_type, 4);
    std::memcpy(path.data.data() + 4, &low, 4);
    std::memcpy(path.data.data() + 8, &high, 4);
    return path;
}

}  // namespace mh4n::fs
