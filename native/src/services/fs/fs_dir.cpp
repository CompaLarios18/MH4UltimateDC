// Derived from Azahar src/core/hle/service/fs/directory.cpp and src/core/file_sys/directory_backend.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later version"): the commands of a directory session (Read 0x0801: the count and
// the mapped buffer in, the result, the number of entries and the buffer out; Close 0x0802) and that both
// answer success.
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (directory.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (directory_backend.h)
//
// A directory the game opened with OpenDirectory (N1-13). Azahar reads on a worker thread with no delay; the
// listing here was taken when the directory was opened, so a read is answered in the request's tick on the
// scheduler thread. Departure from Azahar: at most as many entries as the client's buffer holds are read,
// whatever count the client asked for (Azahar allocates `count` entries first).
#include <memory>
#include <vector>

#include "fs_state.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/service.h"

namespace mh4n::fs {

namespace {

constexpr const char* kLog = "Service.FS";

class FsDirectory final : public Service {
public:
    FsDirectory(ServiceContext& context, std::unique_ptr<DirectoryBackend> backend)
        : Service(context, "fs:dir"), backend_(std::move(backend)) {
        Register(0x0801, "Read", &FsDirectory::Read);
        Register(0x0802, "Close", &FsDirectory::Close);
    }

    const char* SessionKind() const override { return "directory"; }

private:
    // Inputs:  0: 0x08010042, 1: the number of entries, 2: the mapped buffer descriptor (write), 3: its address
    // Outputs: 0: 0x08010082, 1: result, 2: the number of entries read, 3: the descriptor, 4: its id
    void Read(Request& request) {
        const u32 count = request.Pop<u32>();
        const MappedBuffer* mapped = request.PopMappedBuffer();
        if (mapped == nullptr) {
            Reply& reply = request.MakeReply(2, 0);
            reply.Push(kResultInvalidBufferDescriptor);
            reply.Push<u32>(0);
            return;
        }
        const u32 fits = mapped->Size() / static_cast<u32>(sizeof(DirEntry));
        if (count > fits) {
            MH4N_LOG(Warning, kLog, "Read of {} entries into a buffer of 0x{:X} bytes", count, mapped->Size());
        }
        std::vector<DirEntry> entries(std::min(count, fits));
        const u32 read = backend_->Read(static_cast<u32>(entries.size()), entries.data());
        if (read != 0) {
            mapped->Write(entries.data(), 0, read * static_cast<u32>(sizeof(DirEntry)));
        }
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push(read);
        reply.PushMappedBuffer(*mapped);
    }

    void Close(Request& request) {
        backend_->Close();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    std::unique_ptr<DirectoryBackend> backend_;
};

}  // namespace

std::shared_ptr<ClientSession> CreateDirectorySession(ServiceContext& context, std::unique_ptr<DirectoryBackend> backend) {
    return context.CreateSession(std::make_unique<FsDirectory>(context, std::move(backend)));
}

}  // namespace mh4n::fs
