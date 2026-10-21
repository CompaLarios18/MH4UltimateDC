// Derived from Azahar src/core/hle/service/fs/fs_user.cpp, src/core/hle/service/fs/fs_user.h,
// src/core/hle/service/fs/archive.cpp, src/core/hle/kernel/hle_ipc.h and src/common/settings.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the commands of fs:USER this module answers (Initialize, InitializeWithSdkVersion,
// SetPriority, GetPriority, IsSdmcDetected, OpenArchive, CloseArchive, OpenFile, OpenFileDirectly), their
// command ids, parameters, replies and result codes, the limit of 30 sessions, what the client waits for
// after an open (the archive's open delay) and the defaults of use_virtual_sd and async_fs_operations.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (fs_user.cpp, fs_user.h,
//   archive.cpp, settings.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle_ipc.h)
//
// fs:USER (N1-12): archives, RomFS and the system archives. The commands for save data and extdata are
// N1-13's; until it lands they are answered as unimplemented. Departures from Azahar:
//  - OpenArchive, CloseArchive and the other commands that touch no file answer at once, on the scheduler
//    thread. Azahar sends them to a worker thread and the client wakes when the host gets to it; nothing
//    in them waits for the disk, and answering in the request's tick keeps the game clock independent of
//    the host.
//  - OpenFileDirectly with an archive that cannot be opened answers at once with that result and a null
//    handle. Azahar's asynchronous path writes the error and then the file's result into one reply.
//  - A client that did not call Initialize has program id 0, so SelfNCCH has no RomFS for it (Azahar).
#include <memory>
#include <vector>

#include "fs_state.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n::fs {

namespace {

constexpr const char* kLog = "Service.FS";

// fs:USER allows 30 sessions (ServiceFramework("fs:USER", 30)).
constexpr u32 kMaxSessions = 30;

class FsUser final : public Service {
public:
    explicit FsUser(ServiceContext& context)
        : Service(context, "fs:USER"), state_(GetFsState(context)) {
        Register(0x0801, "Initialize", &FsUser::Initialize);
        Register(0x0802, "OpenFile", &FsUser::OpenFile);
        Register(0x0803, "OpenFileDirectly", &FsUser::OpenFileDirectly);
        // N1-13 (save data and extdata) fills these in.
        Register(0x0804, "DeleteFile");
        Register(0x0806, "DeleteDirectory");
        Register(0x0808, "CreateFile");
        Register(0x0809, "CreateDirectory");
        Register(0x080B, "OpenDirectory");
        Register(0x080C, "OpenArchive", &FsUser::OpenArchive);
        Register(0x080D, "ControlArchive");
        Register(0x080E, "CloseArchive", &FsUser::CloseArchive);
        Register(0x0812, "GetFreeBytes");
        Register(0x0817, "IsSdmcDetected", &FsUser::IsSdmcDetected);
        Register(0x084C, "FormatSaveData");
        Register(0x0851, "CreateExtSaveData");
        Register(0x0852, "DeleteExtSaveData");
        Register(0x0861, "InitializeWithSdkVersion", &FsUser::InitializeWithSdkVersion);
        Register(0x0862, "SetPriority", &FsUser::SetPriority);
        Register(0x0863, "GetPriority", &FsUser::GetPriority);
    }

private:
    // The archive path or file path of a request: its type, and the bytes of the static buffer.
    struct PathArgument {
        Path path;
        bool ok = true;
    };
    PathArgument PopPath(Request& request, u32 type, u32 declared_size) {
        PathArgument out;
        out.path.raw_type = type;
        const StaticBuffer* buffer = request.PopStaticBuffer();
        if (buffer == nullptr) {
            out.ok = false;
            return out;
        }
        out.path.data = buffer->data;
        if (buffer->data.size() != declared_size) {
            MH4N_LOG(Warning, kLog, "a path of 0x{:X} bytes was declared with size 0x{:X}", buffer->data.size(),
                     declared_size);
        }
        return out;
    }

    // Inputs:  0: 0x08010002, 1: the calling PID descriptor, 2: the PID
    // Outputs: 0: 0x08010040, 1: result
    void Initialize(Request& request) {
        request.PopPid();
        initialized_ = true;
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Inputs:  0: 0x08610042, 1: the SDK version, 2: the calling PID descriptor, 3: the PID
    // Outputs: 0: 0x08610040, 1: result
    void InitializeWithSdkVersion(Request& request) {
        const u32 version = request.Pop<u32>();
        request.PopPid();
        initialized_ = true;
        MH4N_LOG(Warning, kLog, "(STUBBED) called, version: 0x{:08X}", version);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Inputs:  0: 0x08620040, 1: the priority
    void SetPriority(Request& request) {
        state_->priority = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Outputs: 0: 0x08630080, 1: result, 2: the priority
    void GetPriority(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(state_->priority);
    }

    // Outputs: 0: 0x08170080, 1: result, 2: whether there is an SD card (Azahar's use_virtual_sd, true)
    void IsSdmcDetected(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(true);
    }

    // Inputs:  0: 0x080C00C2, 1: the archive id, 2: the path type, 3: the path size, 4: the static buffer
    //          descriptor, 5: its address
    // Outputs: 0: 0x080C00C0, 1: result, 2-3: the archive handle (0 on an error)
    void OpenArchive(Request& request) {
        const u32 archive_id = request.Pop<u32>();
        const u32 path_type = request.Pop<u32>();
        const u32 path_size = request.Pop<u32>();
        const PathArgument path = PopPath(request, path_type, path_size);
        ArchiveHandle handle = 0;
        Result result = kResultInvalidBufferDescriptor;
        if (path.ok) {
            result = state_->archives.Open(archive_id, path.path, ProgramId(), &handle);
        }
        MH4N_LOG(Debug, kLog, "OpenArchive archive_id=0x{:08X} -> 0x{:08X}", archive_id, result);
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(result);
        reply.Push<u64>(IsError(result) ? 0 : handle);
    }

    // Inputs:  0: 0x080E0080, 1-2: the archive handle
    // Outputs: 0: 0x080E0040, 1: result
    void CloseArchive(Request& request) {
        const ArchiveHandle handle = request.Pop<u64>();
        request.MakeReply(1, 0).Push(state_->archives.Close(handle));
    }

    // Inputs:  0: 0x080201C2, 1: the transaction, 2-3: the archive handle, 4: the path type, 5: the path
    //          size, 6: the open flags, 7: the attributes, 8: the static buffer descriptor, 9: its address
    // Outputs: 0: 0x08020042, 1: result, 2: the move handle descriptor, 3: the file session (0 on an error)
    Handled OpenFile(Request& request) {
        request.Pop<u32>();  // the transaction
        const ArchiveHandle archive_handle = request.Pop<u64>();
        const u32 path_type = request.Pop<u32>();
        const u32 path_size = request.Pop<u32>();
        const u32 mode = request.Pop<u32>();
        const u32 attributes = request.Pop<u32>();
        const PathArgument path = PopPath(request, path_type, path_size);
        if (!path.ok) {
            return ReplyOpen(request, kResultInvalidBufferDescriptor, nullptr, 0);
        }
        ArchiveBackend* archive = state_->archives.Get(archive_handle);
        if (archive == nullptr) {
            return ReplyOpen(request, kResultInvalidArchiveHandle, nullptr, 0);
        }
        const u64 delay = archive->GetOpenDelayNs();
        std::shared_ptr<FileBackend> file;
        const Result result = archive->OpenFile(path.path, mode, attributes, &file);
        return ReplyOpen(request, result, std::move(file), delay);
    }

    // Inputs:  0: 0x08030204, 1: the transaction, 2: the archive id, 3: the archive path type, 4: its size,
    //          5: the file path type, 6: its size, 7: the open flags, 8: the attributes, 9-10: the archive
    //          path (static buffer), 11-12: the file path (static buffer)
    // Outputs: 0: 0x08030042, 1: result, 2: the move handle descriptor, 3: the file session (0 on an error)
    Handled OpenFileDirectly(Request& request) {
        request.Pop<u32>();  // the transaction
        const u32 archive_id = request.Pop<u32>();
        const u32 archive_path_type = request.Pop<u32>();
        const u32 archive_path_size = request.Pop<u32>();
        const u32 file_path_type = request.Pop<u32>();
        const u32 file_path_size = request.Pop<u32>();
        const u32 mode = request.Pop<u32>();
        const u32 attributes = request.Pop<u32>();
        const PathArgument archive_path = PopPath(request, archive_path_type, archive_path_size);
        const PathArgument file_path = PopPath(request, file_path_type, file_path_size);
        if (!archive_path.ok || !file_path.ok) {
            return ReplyOpen(request, kResultInvalidBufferDescriptor, nullptr, 0);
        }
        MH4N_LOG(Debug, kLog, "OpenFileDirectly archive_id=0x{:08X}", archive_id);

        ArchiveHandle handle = 0;
        const Result opened =
            state_->archives.Open(archive_id, archive_path.path, ProgramId(), &handle);
        if (IsError(opened)) {
            MH4N_LOG(Error, kLog, "failed to get a handle for archive archive_id=0x{:08X}", archive_id);
            return ReplyOpen(request, opened, nullptr, 0);
        }
        ArchiveBackend* archive = state_->archives.Get(handle);
        const u64 delay = archive->GetOpenDelayNs();
        std::shared_ptr<FileBackend> file;
        const Result result = archive->OpenFile(file_path.path, mode, attributes, &file);
        state_->archives.Close(handle);
        return ReplyOpen(request, result, std::move(file), delay);
    }

    // The reply of the two opens, now or once the open delay has passed. The file session is made now.
    Handled ReplyOpen(Request& request, Result result, std::shared_ptr<FileBackend> file, u64 delay_ns) {
        std::shared_ptr<ClientSession> session;
        if (!IsError(result) && file != nullptr) {
            session = CreateFileSession(Context(), state_, std::move(file), 0);
        }
        const u16 command_id = request.CommandId();
        if (!NeedsDelay(*state_, delay_ns)) {
            FillOpenReply(request.MakeReply(1, 2), result, std::move(session));
            return Handled::Done;
        }
        std::shared_ptr<Request> pending = request.Share();
        StartIo(*state_, Context(), delay_ns, {}, [pending, command_id, result, session] {
            Reply reply(command_id, 1, 2);
            FillOpenReply(reply, result, session);
            pending->Deliver(std::move(reply));
        });
        return Handled::Deferred;
    }

    static void FillOpenReply(Reply& reply, Result result, std::shared_ptr<ClientSession> session) {
        reply.Push(result);
        reply.PushMoveObjects(std::move(session));  // a null session is the handle 0
    }

    u64 ProgramId() const { return initialized_ ? state_->ClientProgramId() : 0; }

    std::shared_ptr<FsState> state_;
    bool initialized_ = false;
};

MH4N_REGISTER(ServiceFactory, "fs:USER",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<FsUser>(context); },
                             kMaxSessions));

}  // namespace

}  // namespace mh4n::fs
