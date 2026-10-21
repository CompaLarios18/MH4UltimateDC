// Derived from Azahar src/core/file_sys/savedata_archive.cpp, src/core/file_sys/savedata_archive.h,
// src/core/file_sys/archive_source_sd_savedata.cpp, src/core/file_sys/archive_savedata.cpp,
// src/core/file_sys/archive_other_savedata.cpp and src/core/file_sys/disk_archive.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the archive that maps a directory on the host to the files of a save (SaveDataArchive: every
// operation checks the path with PathParser first and answers Azahar's result for a missing or existing
// path; a file is made empty or sparse; free space is a fixed 32 MiB), the directory it lists
// (DiskDirectory: the entries in the host's order, with the 8.3 names, the hidden flag of a leading dot and
// the archive bit of every file) and the places the archives live under the user folder.
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (savedata_archive.cpp,
//   savedata_archive.h, archive_other_savedata.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (archive_source_sd_savedata.cpp,
//   archive_savedata.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (disk_archive.cpp)
//
// Save data and extdata archives (N1-13), internal to mh4n_services_fs. The folder layout is Azahar's, so
// one --user folder works with both programs:
//   SaveData, OtherSaveData*:  sdmc/Nintendo 3DS/<SYSTEM_ID>/<SDCARD_ID>/title/<high>/<low>/data/00000001/
//                              and next to it  .../data/00000001.metadata  (the format info, 16 bytes)
//   ExtSaveData, Boss:         sdmc/Nintendo 3DS/<SYSTEM_ID>/<SDCARD_ID>/extdata/<HIGH>/<LOW>/user|boss/
//   SharedExtSaveData:         nand/data/<SYSTEM_ID>/extdata/<HIGH>/<LOW>/user|boss/
// where SYSTEM_ID and SDCARD_ID are 32 zeros, the title ids are lower case hex and the extdata ids upper case.
#pragma once

#include <array>
#include <memory>
#include <string>

#include "fs_state.h"
#include "fs_types.h"
#include "host_files.h"

namespace mh4n::fs {

inline constexpr const char* kSystemId = "00000000000000000000000000000000";
inline constexpr const char* kSdCardId = "00000000000000000000000000000000";

// The bytes Azahar stores in a .metadata / metadata file: total size, directories, files, then the
// duplicate-data byte and three bytes of padding (zero here).
std::array<u8, 16> EncodeFormatInfo(const ArchiveFormatInfo& info);

// "<user>/sdmc/" and "<user>/nand/"; empty without a user folder.
std::string SdmcDirectory(const FsState& state);
std::string NandDirectory(const FsState& state);

// Opening a save data or extdata archive removes the files that a Windows delete left behind when the process
// died with them open ("<name>.deleted.<pid>.<n>", see host::RemoveFile) from `folder`, the archive's own
// directory tree and nothing else.
void RemoveStaleFiles(const std::string& folder);

// The folder that holds a title's save: <sdmc>/Nintendo 3DS/<ids>/title/<high>/<low>/data/00000001/.
std::string SaveDataPath(const std::string& sdmc_directory, u64 program_id);
std::string SaveDataMetadataPath(const std::string& sdmc_directory, u64 program_id);

// The part of a save data or extdata archive that is a plain directory tree on the host.
class SaveDataArchive : public ArchiveBackend {
public:
    explicit SaveDataArchive(std::string mount_point, bool allow_zero_size_create = true)
        : mount_point_(std::move(mount_point)), allow_zero_size_create_(allow_zero_size_create) {}

    std::string GetName() const override { return "SaveDataArchive: " + mount_point_; }
    const std::string& MountPoint() const { return mount_point_; }

    Result OpenFile(const Path& path, u32 mode, u32 attributes, std::shared_ptr<FileBackend>* file) override;
    Result FileDelete(const Path& path) override;
    Result DirectoryDelete(const Path& path) override;
    Result DirectoryDeleteRecursively(const Path& path) override;
    Result FileCreate(const Path& path, u64 size, u32 attributes) override;
    Result DirectoryCreate(const Path& path, u32 attributes) override;
    Result DirectoryOpen(const Path& path, std::unique_ptr<DirectoryBackend>* directory) override;
    // "Stubbed to return 32MiB" (Azahar).
    u64 GetFreeBytes() const override { return 1024 * 1024 * 32; }
    // ControlArchive (the commit action among them): the archive is a tree of files that are written
    // through, so there is nothing to apply; the names of the files and directories are made durable.
    Result Control(u32 action, const u8* input, std::size_t input_size, u8* output, std::size_t output_size) override;

protected:
    std::string mount_point_;
    bool allow_zero_size_create_;
};

// The binary archive path of an extdata: the media type, then the low and high words of the id.
Path ConstructExtDataBinaryPath(u32 media_type, u32 high, u32 low);

// The listing of a directory, taken when it is opened.
std::unique_ptr<DirectoryBackend> OpenDiskDirectory(const std::string& host_path);

}  // namespace mh4n::fs
