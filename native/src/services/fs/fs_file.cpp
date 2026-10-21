// Derived from Azahar src/core/hle/service/fs/file.cpp, src/core/hle/service/fs/file.h,
// src/core/hle/kernel/hle_ipc.h and src/core/file_sys/file_backend.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: file.cpp says "GPLv3 or any later
// version"; the others say "GPLv2 or any later version"): the commands of a file session (Read, Write,
// GetSize, SetSize, Close, Flush, SetPriority, GetPriority, OpenLinkFile), their command ids, replies and
// result codes, what a session keeps (priority and size; a session made by OpenLinkFile shares the
// backend), that a read is never copied beyond what was asked, that the client waits for the modelled read
// delay (RunAsync, SleepClientThread) and that SetSize on a file that cannot change still records the size.
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (file.cpp, file.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle_ipc.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (file_backend.h)
//
// A file session (N1-12): Read and Write run on an I/O thread and answer by a deferred reply, at the time
// StartIo says. Departures from Azahar:
//  - The data is read into a buffer on the I/O thread and copied into the client's buffer on the scheduler
//    thread when the reply is written (Azahar does the same); the I/O thread never touches guest memory.
//  - The count of a read is cut to the client's buffer and to what is left of the file before anything is
//    allocated (Azahar allocates the length the client asked for).
//  - The delay is counted from the request on the game clock. Azahar subtracts the host time the read took
//    from the modelled delay, so the reply moves with the host.
//  - OpenSubFile (0x0801) is not implemented: the game does not use it.
#include <algorithm>
#include <memory>
#include <vector>

#include "fs_state.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/service.h"

namespace mh4n::fs {

namespace {

constexpr const char* kLog = "Service.FS";

class FsFile final : public Service {
public:
    FsFile(ServiceContext& context, std::shared_ptr<FsState> state, std::shared_ptr<FileBackend> backend,
           u32 priority)
        : Service(context, "fs:file"), state_(std::move(state)), backend_(std::move(backend)),
          priority_(priority), size_(backend_->GetSize()) {
        Register(0x0802, "Read", &FsFile::Read);
        Register(0x0803, "Write", &FsFile::Write);
        Register(0x0804, "GetSize", &FsFile::GetSize);
        Register(0x0805, "SetSize", &FsFile::SetSize);
        Register(0x0808, "Close", &FsFile::Close);
        Register(0x0809, "Flush", &FsFile::Flush);
        Register(0x080A, "SetPriority", &FsFile::SetPriority);
        Register(0x080B, "GetPriority", &FsFile::GetPriority);
        Register(0x080C, "OpenLinkFile", &FsFile::OpenLinkFile);
        Register(0x0801, "OpenSubFile");
    }

    const char* SessionKind() const override { return "file"; }

private:
    // Inputs:  0: 0x080200C2, 1-2: offset, 3: length, 4: the mapped buffer descriptor (write), 5: its address
    // Outputs: 0: 0x08020142, 1: result, 2: bytes read, 3: the mapped buffer descriptor, 4: its id
    Handled Read(Request& request) {
        const u64 offset = request.Pop<u64>();
        const u32 length = request.Pop<u32>();
        const MappedBuffer* mapped = request.PopMappedBuffer();
        if (mapped == nullptr) {
            Reply& reply = request.MakeReply(2, 0);
            reply.Push(kResultInvalidBufferDescriptor);
            reply.Push<u32>(0);
            return Handled::Done;
        }
        MH4N_LOG(Trace, kLog, "Read: offset=0x{:x} length=0x{:08X}", offset, length);

        struct ReadData {
            Result result = kResultHostFailure;  // what the reply says if the I/O thread never got to it
            std::size_t read = 0;
            std::unique_ptr<u8[]> bytes;
        };
        const u64 file_size = backend_->GetSize();
        const u64 left = file_size > offset ? file_size - offset : 0;
        const std::size_t count = static_cast<std::size_t>(std::min<u64>({length, mapped->Size(), left}));
        if (length > mapped->Size()) {
            MH4N_LOG(Warning, kLog, "Read of 0x{:X} bytes into a buffer of 0x{:X}", length, mapped->Size());
        }
        if (offset + length > file_size) {
            MH4N_LOG(Debug, kLog, "Reading from out of bounds offset=0x{:x} length=0x{:08X} file_size=0x{:x}",
                     offset, length, file_size);
        }
        auto data = std::make_shared<ReadData>();
        std::shared_ptr<FileBackend> backend = backend_;
        const u16 command_id = request.CommandId();
        std::shared_ptr<Request> pending = request.Share();
        const MappedBuffer buffer = *mapped;
        StartIo(
            *state_, Context(), backend_->GetReadDelayNs(length),
            [data, backend, offset, count] {
                data->bytes = std::make_unique_for_overwrite<u8[]>(count);
                data->result = backend->Read(offset, count, data->bytes.get(), &data->read);
            },
            [data, pending, buffer, command_id, count] {
                Reply reply(command_id, 2, 2);
                if (IsError(data->result)) {
                    reply.Push(data->result);
                    reply.Push<u32>(0);
                } else {
                    // Never copy more than was requested, even if the backend reports more.
                    const std::size_t size = std::min(data->read, count);
                    buffer.Write(data->bytes.get(), 0, static_cast<u32>(size));
                    reply.Push(kResultSuccess);
                    reply.Push<u32>(static_cast<u32>(size));
                }
                reply.PushMappedBuffer(buffer);
                pending->Deliver(std::move(reply));
            });
        return Handled::Deferred;
    }

    // Inputs:  0: 0x08030102, 1-2: offset, 3: length, 4: flags, 5: the mapped buffer descriptor (read), 6: its address
    // Outputs: 0: 0x08030142, 1: result, 2: bytes written, 3: the mapped buffer descriptor, 4: its id
    Handled Write(Request& request) {
        const u64 offset = request.Pop<u64>();
        const u32 length = request.Pop<u32>();
        const u32 flags = request.Pop<u32>();
        const MappedBuffer* mapped = request.PopMappedBuffer();
        if (mapped == nullptr) {
            Reply& reply = request.MakeReply(2, 0);
            reply.Push(kResultInvalidBufferDescriptor);
            reply.Push<u32>(0);
            return Handled::Done;
        }
        MH4N_LOG(Trace, kLog, "Write: offset=0x{:x} length={} flags=0x{:x}", offset, length, flags);
        const bool flush = (flags & 0xFF) != 0;
        const bool update_timestamp = (flags & 0xFF00) != 0;

        struct WriteData {
            Result result = kResultHostFailure;
            std::size_t written = 0;
            std::vector<u8> bytes;
        };
        auto data = std::make_shared<WriteData>();
        data->bytes.resize(std::min<u64>(length, mapped->Size()));
        if (!data->bytes.empty()) {
            mapped->Read(data->bytes.data(), 0, static_cast<u32>(data->bytes.size()));
        }
        std::shared_ptr<FileBackend> backend = backend_;
        const u16 command_id = request.CommandId();
        std::shared_ptr<Request> pending = request.Share();
        const MappedBuffer buffer = *mapped;
        StartIo(
            *state_, Context(), 0,
            [data, backend, offset, flush, update_timestamp] {
                data->result = backend->Write(offset, data->bytes.size(), flush, update_timestamp,
                                              data->bytes.data(), &data->written);
            },
            [this, backend, data, pending, buffer, command_id] {
                // The session is alive while the request is (a client that stopped waiting releases it,
                // and then the size has no reader).
                if (pending->Session() != nullptr) {
                    size_ = backend->GetSize();
                }
                Reply reply(command_id, 2, 2);
                reply.Push(data->result);
                reply.Push<u32>(IsError(data->result) ? 0 : static_cast<u32>(data->written));
                reply.PushMappedBuffer(buffer);
                pending->Deliver(std::move(reply));
            });
        return Handled::Deferred;
    }

    // Outputs: 0: 0x08040140 (3 normal words), 1: result, 2-3: size
    void GetSize(Request& request) {
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(kResultSuccess);
        reply.Push<u64>(size_);
    }

    // Inputs:  0: 0x08050080, 1-2: size. Azahar records the size whether or not the backend took it.
    void SetSize(Request& request) {
        const u64 size = request.Pop<u64>();
        size_ = size;
        backend_->SetSize(size);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void Close(Request& request) {
        backend_->Close();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void Flush(Request& request) {
        backend_->Flush();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void SetPriority(Request& request) {
        priority_ = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void GetPriority(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(priority_);
    }

    // Outputs: 0: 0x080C0041 (1 normal, 2 translate), 1: result, 2: the descriptor, 3: the new session
    void OpenLinkFile(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) File command OpenLinkFile");
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushMoveObjects(CreateFileSession(Context(), state_, backend_, priority_));
    }

    std::shared_ptr<FsState> state_;
    std::shared_ptr<FileBackend> backend_;
    u32 priority_;
    u64 size_;
};

}  // namespace

std::shared_ptr<ClientSession> CreateFileSession(ServiceContext& context, std::shared_ptr<FsState> state,
                                                 std::shared_ptr<FileBackend> backend, u32 priority) {
    return context.CreateSession(
        std::make_unique<FsFile>(context, std::move(state), std::move(backend), priority));
}

}  // namespace mh4n::fs
