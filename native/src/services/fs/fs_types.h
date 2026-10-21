// Derived from Azahar src/core/hle/service/fs/archive.h, src/core/file_sys/errors.h,
// src/core/file_sys/archive_backend.h, src/core/file_sys/file_backend.h,
// src/core/file_sys/ivfc_archive.h, src/core/file_sys/delay_generator.h and
// src/core/file_sys/delay_generator.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: archive.h says "GPLv3 or any later version"; the others say "GPLv2 or any later
// version"): the archive ids (ArchiveIdCode), the file system result codes (ErrCodes and the Result*
// constants), the shape of an archive and of a file (ArchiveBackend, FileBackend), the low path types
// (LowPathType) and the delays a RomFS open and read take (RomFSDelayGenerator, DefaultDelayGenerator:
// slope, offset, minimum and the open delay).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (archive.h, archive_backend.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (file_backend.h, ivfc_archive.h)
// Copyright 2016 Citra Emulator Project (errors.h)
// Copyright 2018 Citra Emulator Project (delay_generator.h, delay_generator.cpp)
//
// What the pieces of the file system (N1-12) share: result codes, the archive and file interfaces, and
// the delay model. Internal to mh4n_services_fs; N1-13 (save data) adds archives behind the same
// interfaces from its own files.
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

// An archive handle is 64 bits and numbered from 1 (ArchiveManager::next_handle).
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
};

// What OpenArchive asks for an archive id.
class ArchiveFactory {
public:
    virtual ~ArchiveFactory() = default;
    virtual std::string GetName() const = 0;
    // `program_id` is the client's, 0 for a client that did not initialise the session.
    virtual Result Open(const Path& path, u64 program_id, std::unique_ptr<ArchiveBackend>* archive) = 0;
};

}  // namespace mh4n::fs
