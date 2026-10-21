// Derived from Azahar src/core/hle/service/fs/archive.h, src/core/file_sys/errors.h,
// src/core/file_sys/archive_backend.h, src/core/file_sys/file_backend.h,
// src/core/file_sys/directory_backend.h, src/core/file_sys/ivfc_archive.h, src/core/file_sys/ivfc_archive.cpp,
// src/core/file_sys/delay_generator.h and src/core/file_sys/delay_generator.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: archive.h says "GPLv3 or any later
// version"; the others say "GPLv2 or any later version"): the archive ids (ArchiveIdCode), the file system
// result codes (ErrCodes and the Result* constants), the shape of an archive, of a file and of a directory
// (ArchiveBackend, FileBackend, DirectoryBackend and its Entry), the format info of an archive
// (ArchiveFormatInfo), the low path types (LowPathType) and the delays a RomFS open and read take
// (RomFSDelayGenerator, DefaultDelayGenerator: slope, offset, minimum and the open delay).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (archive.h, archive_backend.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (file_backend.h, ivfc_archive.h,
//   ivfc_archive.cpp, directory_backend.h)
// Copyright 2016 Citra Emulator Project (errors.h)
// Copyright 2018 Citra Emulator Project (delay_generator.h, delay_generator.cpp)
//
// What the pieces of the file system (N1-12) share: result codes, the archive and file interfaces, and
// the delay model. Internal to mh4n_services_fs; N1-13 (save data) adds archives behind the same
// interfaces from its own files. An archive that does not override the file operations below answers
// them like Azahar's IVFCArchive (RomFS and the like: nothing can be created or deleted).
//
// Threads (principle 6): archives, the archive manager and the service objects belong to the scheduler
// thread. A FileBackend is shared with the I/O threads: Read and Write may run there, and more than
// one at a time, so a backend keeps no cursor (positioned reads) and guards what it changes.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "mh4n/kernel.h"
#include "mh4n/types.h"

namespace mh4n::fs {

// ---- result codes (Azahar src/core/file_sys/errors.h; ErrorModule::FS is 17) -------------------------

inline constexpr u32 kModuleFs = 17;
// ErrorSummary and ErrorLevel values of src/core/hle/result.h.
inline constexpr u32 kSummaryNotFound = 4;
inline constexpr u32 kSummaryNotSupported = 6;
inline constexpr u32 kSummaryInvalidArgument = 7;
inline constexpr u32 kSummaryWrongArgument = 8;
inline constexpr u32 kSummaryInternal = 11;
inline constexpr u32 kLevelStatus = 25;
inline constexpr u32 kLevelPermanent = 27;
inline constexpr u32 kLevelUsage = 28;

inline constexpr Result kResultRomfsNotFound = MakeResult(100, kModuleFs, kSummaryNotFound, kLevelStatus);
// "Returned when a function is passed an invalid archive handle" (0xC8804465).
inline constexpr Result kResultInvalidArchiveHandle =
    MakeResult(101, kModuleFs, kSummaryNotFound, kLevelStatus);
inline constexpr Result kResultNotFound = MakeResult(120, kModuleFs, kSummaryNotFound, kLevelStatus);
inline constexpr Result kResultExefsSectionNotFound =
    MakeResult(567, kModuleFs, kSummaryNotFound, kLevelStatus);
inline constexpr Result kResultCommandNotAllowed =
    MakeResult(630, kModuleFs, kSummaryWrongArgument, kLevelPermanent);
inline constexpr Result kResultInvalidPath =
    MakeResult(702, kModuleFs, kSummaryInvalidArgument, kLevelUsage);
inline constexpr Result kResultWriteBeyondEnd =
    MakeResult(705, kModuleFs, kSummaryInvalidArgument, kLevelUsage);
inline constexpr Result kResultUnsupportedOpenFlags =
    MakeResult(760, kModuleFs, kSummaryNotSupported, kLevelUsage);
// N1-13 (save data and extdata); the codes of Azahar's errors.h, with the summary and level it gives them.
inline constexpr u32 kSummaryNothingHappened = 1;
inline constexpr u32 kSummaryOutOfResource = 3;
inline constexpr u32 kSummaryInvalidState = 5;
inline constexpr u32 kSummaryCanceled = 9;
inline constexpr u32 kLevelInfo = 1;
inline constexpr Result kResultFileNotFound = MakeResult(112, kModuleFs, kSummaryNotFound, kLevelStatus);
inline constexpr Result kResultPathNotFound = MakeResult(113, kModuleFs, kSummaryNotFound, kLevelStatus);
inline constexpr Result kResultGamecardNotInserted = MakeResult(141, kModuleFs, kSummaryNotFound, kLevelStatus);
inline constexpr Result kResultFileAlreadyExists =
    MakeResult(180, kModuleFs, kSummaryNothingHappened, kLevelStatus);
inline constexpr Result kResultDirectoryAlreadyExists =
    MakeResult(185, kModuleFs, kSummaryNothingHappened, kLevelStatus);
inline constexpr Result kResultInvalidOpenFlags = MakeResult(230, kModuleFs, kSummaryCanceled, kLevelStatus);
inline constexpr Result kResultDirectoryNotEmpty = MakeResult(240, kModuleFs, kSummaryCanceled, kLevelStatus);
// "ExtSaveData seems to return FS_NotFound (120) when the archive doesn't exist": with the summary
// InvalidState (Azahar's ResultNotFoundInvalidState).
inline constexpr Result kResultNotFoundInvalidState =
    MakeResult(120, kModuleFs, kSummaryInvalidState, kLevelStatus);
inline constexpr Result kResultNotFormatted = MakeResult(340, kModuleFs, kSummaryInvalidState, kLevelStatus);
inline constexpr Result kResultUnexpectedFileOrDirectory =
    MakeResult(770, kModuleFs, kSummaryNotSupported, kLevelUsage);
// What Azahar answers for "too large" (CreateFile that cannot be allocated) and for a failed CreateDirectory.
inline constexpr Result kResultTooLarge = MakeResult(1001, kModuleFs, kSummaryOutOfResource, kLevelInfo);
inline constexpr Result kResultCreateDirectoryFailed = MakeResult(1007, kModuleFs, kSummaryCanceled, kLevelStatus);
// UnimplementedFunction(ErrorModule::FS): description NotImplemented (1012), NotSupported, Permanent.
inline constexpr Result kResultUnimplementedFs = MakeResult(1012, kModuleFs, kSummaryNotSupported, kLevelPermanent);
// Azahar's ResultUnknown, for the failures it did not find a code for.
inline constexpr Result kResultUnknown = 0xFFFFFFFF;
// IVFCArchive::DeleteFile: ErrorDescription::NoData (1007), FS, Canceled, Status.
inline constexpr Result kResultNoDataCanceled = MakeResult(1007, kModuleFs, kSummaryCanceled, kLevelStatus);
// IVFCArchive::CreateFile: ErrorDescription::NotAuthorized (1002), FS, NotSupported, Permanent.
inline constexpr Result kResultNotAuthorized = MakeResult(1002, kModuleFs, kSummaryNotSupported, kLevelPermanent);
static_assert(kResultInvalidArchiveHandle == 0xC8804465);

// Not an Azahar code: the host could not do what was asked (an allocation failed on the I/O thread).
// Azahar has no such case; a read that fails on the host gives a short count there, as it does here.
inline constexpr Result kResultHostFailure =
    MakeResult(1000, kModuleFs, kSummaryInternal, kLevelPermanent);

// ---- archive ids (Azahar ArchiveIdCode) ------------------------------------------------------------------

namespace archive_id {
inline constexpr u32 kSelfNCCH = 0x00000003;
inline constexpr u32 kSaveData = 0x00000004;
inline constexpr u32 kExtSaveData = 0x00000006;
inline constexpr u32 kSharedExtSaveData = 0x00000007;
inline constexpr u32 kSystemSaveData = 0x00000008;
inline constexpr u32 kSDMC = 0x00000009;
inline constexpr u32 kSDMCWriteOnly = 0x0000000A;
inline constexpr u32 kBossExtSaveData = 0x12345678;
inline constexpr u32 kNANDRW = 0x1234567D;
inline constexpr u32 kNANDRO = 0x1234567E;
inline constexpr u32 kNANDROW = 0x1234567F;
inline constexpr u32 kNCCH = 0x2345678A;
inline constexpr u32 kOtherSaveDataGeneral = 0x567890B2;
inline constexpr u32 kOtherSaveDataPermitted = 0x567890B4;
}  // namespace archive_id

// Azahar's MediaType (archive.h).
inline constexpr u32 kMediaNand = 0;
inline constexpr u32 kMediaSdmc = 1;
inline constexpr u32 kMediaGameCard = 2;

// An archive handle is 64 bits and numbered from 1 (ArchiveManager::next_handle; the rule is in fs_state.h).
using ArchiveHandle = u64;

// ---- paths ---------------------------------------------------------------------------------------------------

// The low path types of the 3DS (Azahar LowPathType).
enum class LowPathType : u32 { Invalid = 0, Empty = 1, Binary = 2, Char = 3, Wchar = 4 };

// A path as the request carried it. Only binary paths mean something to the archives of this module.
struct Path {
    u32 raw_type = 0;
    std::vector<u8> data;

    LowPathType Type() const {
        return raw_type <= 4 ? static_cast<LowPathType>(raw_type) : LowPathType::Invalid;
    }
    bool IsBinary() const { return raw_type == static_cast<u32>(LowPathType::Binary); }
};

// ---- delays (Azahar RomFSDelayGenerator, ExeFSDelayGenerator, DefaultDelayGenerator) --------------------------

// The delay "measured on O3DS and O2DS" for a read of `length` bytes: 94 ns a byte plus 582778 ns, at
// least 663124 ns. All four of Azahar's generators have these numbers.
constexpr u64 RomFsReadDelayNs(std::size_t length) {
    constexpr u64 slope = 94;
    constexpr u64 offset = 582778;
    constexpr u64 minimum = 663124;
    const u64 delay = static_cast<u64>(length) * slope + offset;
    return delay > minimum ? delay : minimum;
}
// The delay of an open, 9438006 ns.
inline constexpr u64 kRomFsOpenDelayNs = 9438006;

// ---- directories -----------------------------------------------------------------------------------------------------

// The delay of a read of a save data or extdata file: 183 ns a byte plus 524879 ns, at least 631826 ns
// (Azahar SaveDataDelayGenerator and ExtSaveDataDelayGenerator, "measured on O3DS and O2DS").
constexpr u64 SaveReadDelayNs(std::size_t length) {
    constexpr u64 slope = 183;
    constexpr u64 offset = 524879;
    constexpr u64 minimum = 631826;
    const u64 delay = static_cast<u64>(length) * slope + offset;
    return delay > minimum ? delay : minimum;
}
// The open delay of an ExtSaveDataArchive (ExtSaveDataDelayGenerator, "measured on N3DS"), 3085068 ns. A
// SaveDataArchive sets no delay generator in Azahar, so its open takes the default one (RomFS's).
inline constexpr u64 kExtSaveOpenDelayNs = 3085068;

// The entry a directory read returns (Azahar FileSys::Entry, http://3dbrew.org/wiki/FSDir:Read#Entry_format).
inline constexpr std::size_t kFilenameLength = 0x20C / 2;
struct DirEntry {
    char16_t filename[kFilenameLength];  // UTF-16, null-terminated
    char short_name[9];                  // 8.3 name ('longfilename' -> 'LONGFI~1'), null-terminated
    char unknown1;
    char extension[4];                   // set to spaces for directories, null-terminated
    char unknown2;
    char unknown3;
    char is_directory;
    char is_hidden;
    char is_archive;
    char is_read_only;
    u64 file_size;
};
static_assert(sizeof(DirEntry) == 0x228, "a directory entry is 0x228 bytes");
static_assert(offsetof(DirEntry, short_name) == 0x20C);
static_assert(offsetof(DirEntry, extension) == 0x216);
static_assert(offsetof(DirEntry, is_archive) == 0x21E);
static_assert(offsetof(DirEntry, file_size) == 0x220);

// An open directory. Scheduler thread only.
class DirectoryBackend {
public:
    virtual ~DirectoryBackend() = default;
    // Fills up to `count` entries and returns how many.
    virtual u32 Read(u32 count, DirEntry* entries) = 0;
    virtual bool Close() { return true; }
};

// The format info of an archive (Azahar ArchiveFormatInfo): written to the metadata file as these 16 bytes
// (the three padding bytes are zero here; Azahar leaves them as they were in memory).
struct ArchiveFormatInfo {
    u32 total_size = 0;         // the pre-defined size of the archive
    u32 number_directories = 0;
    u32 number_files = 0;
    u8 duplicate_data = 0;
};

// ---- backends ----------------------------------------------------------------------------------------------------

// An open file. Read and Write may run on an I/O thread, several at once.
class FileBackend {
public:
    virtual ~FileBackend() = default;

    // Reads up to `length` bytes at `offset` into `buffer`; *read is the count (fewer at the end of the
    // file). The result is the error of the file system, if there is one.
    virtual Result Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) = 0;
    // The files of this module are read-only: the answer is Azahar's, success with 0 bytes written.
    virtual Result Write(u64 offset, std::size_t length, bool flush, bool update_timestamp,
                         const u8* buffer, std::size_t* written) {
        (void)offset, (void)length, (void)flush, (void)update_timestamp, (void)buffer;
        *written = 0;
        return kResultSuccess;
    }
    virtual u64 GetSize() = 0;
    virtual bool SetSize(u64 size) {
        (void)size;
        return false;
    }
    virtual bool Close() { return true; }
    virtual void Flush() {}

    // True for a backend that can change under the game (a save or extdata file). Its Read is done on the
    // scheduler thread, in the request's tick, and only the reply waits for the delay: what a read returns
    // then cannot depend on how a host thread races a Write (Azahar reads a non-cached backend in the
    // request too, and only sleeps the client). The others (RomFS, memory) never change and read on an I/O thread.
    virtual bool ReadsOnSchedulerThread() const { return false; }

    // How long the game waits for a read of `length` bytes (Azahar FileBackend::GetReadDelayNs).
    virtual u64 GetReadDelayNs(std::size_t length) { return RomFsReadDelayNs(length); }
};

// An opened archive.
class ArchiveBackend {
public:
    virtual ~ArchiveBackend() = default;
    virtual std::string GetName() const = 0;
    // `mode` is the open flags word (bit 0 read, 1 write, 2 create); `attributes` the attribute word.
    virtual Result OpenFile(const Path& path, u32 mode, u32 attributes,
                            std::shared_ptr<FileBackend>* file) = 0;
    // How long the game waits for an open (Azahar ArchiveBackend::GetOpenDelayNs).
    virtual u64 GetOpenDelayNs() { return kRomFsOpenDelayNs; }

    // ---- N1-13: what an archive does besides opening a file --------------------------------------------
    // The defaults are the answers of Azahar's IVFCArchive (RomFS and the archives built like it), so an
    // archive that cannot be written says what Azahar says. NCCHArchive differs only in OpenDirectory and
    // SelfNCCHArchive answers its own codes; both override below. The save data archives override all of it.
    virtual Result FileDelete(const Path& path) {
        (void)path;
        return kResultNoDataCanceled;  // "TODO(Subv): Verify error code"
    }
    virtual Result DirectoryDelete(const Path& path) {
        (void)path;
        return kResultUnknown;
    }
    virtual Result DirectoryDeleteRecursively(const Path& path) {
        (void)path;
        return kResultUnknown;
    }
    virtual Result FileCreate(const Path& path, u64 size, u32 attributes) {
        (void)path, (void)size, (void)attributes;
        return kResultNotAuthorized;
    }
    virtual Result DirectoryCreate(const Path& path, u32 attributes) {
        (void)path, (void)attributes;
        return kResultUnknown;
    }
    // IVFCArchive::OpenDirectory: an empty directory (IVFCDirectory::Read gives 0 entries).
    virtual Result DirectoryOpen(const Path& path, std::unique_ptr<DirectoryBackend>* directory);
    virtual u64 GetFreeBytes() const { return 0; }
    // ControlArchive: Azahar's base class logs and answers success.
    virtual Result Control(u32 action, const u8* input, std::size_t input_size, u8* output, std::size_t output_size) {
        (void)action, (void)input, (void)input_size, (void)output, (void)output_size;
        return kResultSuccess;
    }
};

// What OpenArchive asks for an archive id.
class ArchiveFactory {
public:
    virtual ~ArchiveFactory() = default;
    virtual std::string GetName() const = 0;
    // `program_id` is the client's, 0 for a client that did not initialise the session.
    virtual Result Open(const Path& path, u64 program_id, std::unique_ptr<ArchiveBackend>* archive) = 0;

    // ---- N1-13 ---------------------------------------------------------------------------------------------
    // FormatSaveData: deletes the archive's contents and makes it again (only SaveData does). Azahar's
    // unregistered-archive answer for the others.
    virtual Result Format(const Path& path, const ArchiveFormatInfo& info, u64 program_id, u32 directory_buckets,
                          u32 file_buckets) {
        (void)path, (void)info, (void)program_id, (void)directory_buckets, (void)file_buckets;
        return kResultUnimplementedFs;
    }
    // CreateExtSaveData / DeleteExtSaveData, on the extdata factories.
    virtual Result FormatAsExtData(const Path& path, const ArchiveFormatInfo& info, const std::vector<u8>& icon) {
        (void)path, (void)info, (void)icon;
        return kResultUnimplementedFs;
    }
    virtual Result DeleteExtData(u32 media_type, u32 high, u32 low) {
        (void)media_type, (void)high, (void)low;
        return kResultUnimplementedFs;
    }
};

// IVFCDirectory: no entries, and Close says false (Azahar's, ivfc_archive.h).
class EmptyDirectory final : public DirectoryBackend {
public:
    u32 Read(u32 count, DirEntry* entries) override {
        (void)count, (void)entries;
        return 0;
    }
    bool Close() override { return false; }
};

inline Result ArchiveBackend::DirectoryOpen(const Path& path, std::unique_ptr<DirectoryBackend>* directory) {
    (void)path;
    *directory = std::make_unique<EmptyDirectory>();
    return kResultSuccess;
}

}  // namespace mh4n::fs
