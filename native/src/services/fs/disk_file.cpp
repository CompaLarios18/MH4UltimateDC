// Derived from Azahar src/core/file_sys/disk_archive.cpp, src/core/file_sys/archive_extsavedata.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each says "GPLv2 or any later version");
// see the header of disk_file.
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (disk_archive.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (archive_extsavedata.cpp)
#include "disk_file.h"

#include "mh4n/log.h"

namespace mh4n::fs {

namespace {
constexpr const char* kLog = "Service.FS";
}

Result DiskFile::Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) {
    *read = 0;
    if (!CanRead()) {
        return kResultInvalidOpenFlags;
    }
    bool failed = false;
    *read = file_->ReadAt(offset, buffer, length, &failed);
    if (failed) {
        MH4N_LOG(Error, kLog, "a read of 0x{:X} bytes at 0x{:X} failed on the host", length, offset);
    }
    return kResultSuccess;
}

Result DiskFile::Write(u64 offset, std::size_t length, bool flush, bool update_timestamp, const u8* buffer,
                       std::size_t* written) {
    (void)update_timestamp;
    *written = 0;
    if (!CanWrite() || !file_->IsOpen()) {
        return kResultInvalidOpenFlags;
    }
    bool failed = false;
    *written = file_->WriteAt(offset, buffer, length, &failed);
    if (failed) {
        MH4N_LOG(Error, kLog, "a write of 0x{:X} bytes at 0x{:X} stopped after 0x{:X} on the host", length, offset,
                 *written);
    }
    if (flush && !file_->Sync()) {
        MH4N_LOG(Error, kLog, "the host could not make a flushed write durable");
    }
    return kResultSuccess;
}

bool DiskFile::SetSize(u64 size) {
    if (!file_->Resize(size)) {
        MH4N_LOG(Error, kLog, "the host could not resize a file to 0x{:X} bytes", size);
    }
    if (!file_->Sync()) {
        MH4N_LOG(Error, kLog, "the host could not make a resized file durable");
    }
    return true;  // Azahar's DiskFile::SetSize answers true whatever the host said
}

bool DiskFile::Close() {
    if (!file_->Close()) {
        MH4N_LOG(Error, kLog, "the host could not make a file durable when it was closed");
        return false;
    }
    return true;
}

void DiskFile::Flush() {
    if (!file_->Sync()) {
        MH4N_LOG(Error, kLog, "the host could not flush a file");
    }
}

Result FixSizeDiskFile::Write(u64 offset, std::size_t length, bool flush, bool update_timestamp, const u8* buffer,
                              std::size_t* written) {
    *written = 0;
    if (offset > size_) {
        return kResultWriteBeyondEnd;
    }
    if (offset == size_) {
        return kResultSuccess;
    }
    if (offset + length > size_) {
        length = static_cast<std::size_t>(size_ - offset);
    }
    return DiskFile::Write(offset, length, flush, update_timestamp, buffer, written);
}

}  // namespace mh4n::fs
