// Derived from Azahar src/core/file_sys/disk_archive.cpp, src/core/file_sys/disk_archive.h,
// src/core/file_sys/archive_extsavedata.cpp and src/core/file_sys/errors.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): a file on the host opened for a save data or extdata archive (DiskFile: a read needs the read
// flag and a write the write flag, else "invalid open flags"; SetSize resizes and flushes; the delays
// are SaveDataDelayGenerator's) and the extdata file whose size cannot change (FixSizeDiskFile: SetSize
// refused, a write beyond the end is an error, a write at the end writes nothing, a write that crosses the
// end is cut there).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (disk_archive.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (disk_archive.h,
//   archive_extsavedata.cpp)
// Copyright 2016 Citra Emulator Project (errors.h)
//
// The files of the save data archives (N1-13). Write goes to the file at once, on the scheduler thread, and
// answers in the request's tick (see fs_file.cpp); with the flush flag, Flush and Close it is made durable
// (fsync) before the reply, so a Close that returned is a save that is on the disk. Departures from Azahar:
//  - A file that was closed answers a read with 0 bytes and a write with "invalid open flags". Azahar's
//    closed FILE* gives a count of (size_t)-1, which File::Read then copies out of an uninitialised buffer.
//  - A write that the host refuses part-way (a full disk) answers the bytes that were written, as
//    Azahar's does, and logs an error.
#pragma once

#include <memory>
#include <string>

#include "fs_types.h"
#include "host_files.h"

namespace mh4n::fs {

class DiskFile : public FileBackend {
public:
    DiskFile(std::shared_ptr<WritableFile> file, u32 mode) : file_(std::move(file)), mode_(mode) {}
    ~DiskFile() override = default;

    // Mode bits: 0 read, 1 write, 2 create.
    bool CanRead() const { return (mode_ & 1) != 0; }
    bool CanWrite() const { return (mode_ & 2) != 0; }

    Result Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) override;
    Result Write(u64 offset, std::size_t length, bool flush, bool update_timestamp, const u8* buffer,
                 std::size_t* written) override;
    u64 GetSize() override { return file_->Size(); }
    bool SetSize(u64 size) override;
    bool Close() override;
    void Flush() override;
    bool ReadsOnSchedulerThread() const override { return true; }
    u64 GetReadDelayNs(std::size_t length) override { return SaveReadDelayNs(length); }

protected:
    std::shared_ptr<WritableFile> file_;
    u32 mode_;
};

// An extdata file: it never grows or shrinks.
class FixSizeDiskFile final : public DiskFile {
public:
    FixSizeDiskFile(std::shared_ptr<WritableFile> file, u32 mode) : DiskFile(std::move(file), mode), size_(file_->Size()) {}

    bool SetSize(u64 size) override {
        (void)size;
        return false;
    }
    Result Write(u64 offset, std::size_t length, bool flush, bool update_timestamp, const u8* buffer,
                 std::size_t* written) override;

private:
    u64 size_;
};

}  // namespace mh4n::fs
