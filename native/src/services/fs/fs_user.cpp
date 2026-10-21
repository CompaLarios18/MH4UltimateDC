// Derived from Azahar src/core/hle/service/fs/fs_user.cpp, src/core/hle/service/fs/fs_user.h,
// src/core/hle/service/fs/archive.cpp, src/core/hle/service/fs/archive.h, src/core/hle/kernel/hle_ipc.h and
// src/common/settings.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: archive.h says "GPLv3
// or any later version"; the others say "GPLv2 or any later version"; archive.h is the source of the layout
// of ExtSaveDataInfo and the media types): the commands of fs:USER this module answers (Initialize,
// InitializeWithSdkVersion, SetPriority, GetPriority, IsSdmcDetected, OpenArchive, CloseArchive, OpenFile,
// OpenFileDirectly), their
// command ids, parameters, replies and result codes, the limit of 30 sessions, what the client waits for
// after an open (the archive's open delay) and the defaults of use_virtual_sd and async_fs_operations.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (fs_user.cpp, fs_user.h,
//   archive.cpp, archive.h, settings.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle_ipc.h)
//
// fs:USER (N1-12): archives, RomFS and the system archives; (N1-13) the commands of save data and extdata:
// DeleteFile, DeleteDirectory, DeleteDirectoryRecursively, CreateFile, CreateDirectory, OpenDirectory,
// ControlArchive, GetFreeBytes, FormatSaveData, CreateExtSaveData and DeleteExtSaveData, with their
// parameters and replies as Azahar parses and builds them. An archive that is not a save data kind (RomFS,
// the system archives) leaves them to the unimplemented-command answer, as before N1-13. Like OpenArchive,
// they answer in the request's tick (Azahar runs them on a worker with no delay). Departures from Azahar:
//  - OpenArchive, CloseArchive and the other commands that touch no file answer at once, on the scheduler
//    thread. Azahar sends them to a worker thread and the client wakes when the host gets to it; nothing
//    in them waits for the disk, and answering in the request's tick keeps the game clock independent of
//    the host.
//  - OpenFileDirectly with an archive that cannot be opened answers at once with that result and a null
//    handle. Azahar's asynchronous path writes the error and then the file's result into one reply.
//  - A client that did not call Initialize has program id 0, so SelfNCCH has no RomFS for it (Azahar).
#include <algorithm>
#include <memory>
#include <vector>

#include "fs_state.h"
#include "save_archives.h"
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
        Register(0x0804, "DeleteFile", &FsUser::CmdDeleteFile);
        Register(0x0806, "DeleteDirectory", &FsUser::CmdDeleteDirectory);
        Register(0x0807, "DeleteDirectoryRecursively", &FsUser::CmdDeleteDirectoryRecursively);
        Register(0x0808, "CreateFile", &FsUser::CmdCreateFile);
        Register(0x0809, "CreateDirectory", &FsUser::CmdCreateDirectory);
        Register(0x080B, "OpenDirectory", &FsUser::CmdOpenDirectory);
        Register(0x080C, "OpenArchive", &FsUser::OpenArchive);
        Register(0x080D, "ControlArchive", &FsUser::CmdControlArchive);
        Register(0x080E, "CloseArchive", &FsUser::CloseArchive);
        Register(0x0812, "GetFreeBytes", &FsUser::CmdGetFreeBytes);
        Register(0x0817, "IsSdmcDetected", &FsUser::IsSdmcDetected);
        Register(0x084C, "FormatSaveData", &FsUser::CmdFormatSaveData);
        Register(0x0851, "CreateExtSaveData", &FsUser::CmdCreateExtSaveData);
        Register(0x0852, "DeleteExtSaveData", &FsUser::CmdDeleteExtSaveData);
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


    // ---- N1-13: save data and extdata ---------------------------------------------------------------------

    // The archive a file operation names, or null with the reply already made: an unknown handle is
    // "invalid archive handle" (`reply_words` is the size of the reply the command makes). Every archive
    // answers the file operations itself (the defaults of ArchiveBackend are Azahar's IVFC ones).
    ArchiveBackend* FileOpArchive(Request& request, ArchiveHandle handle, u32 reply_words, u32 translate_words = 0) {
        ArchiveBackend* archive = state_->archives.Get(handle);
        if (archive == nullptr) {
            Reply& reply = request.MakeReply(reply_words, translate_words);
            reply.Push(kResultInvalidArchiveHandle);
            for (u32 i = 1; i < reply_words; ++i) {
                reply.Push<u32>(0);
            }
            if (translate_words != 0) {
                reply.PushMoveObjects(std::shared_ptr<ClientSession>());
            }
            return nullptr;
        }
        return archive;
    }

    void ReplyResult(Request& request, Result result) { request.MakeReply(1, 0).Push(result); }

    // Inputs:  0: 0x08040142, 1: the transaction, 2-3: the archive handle, 4: the path type, 5: its size,
    //          6: the static buffer descriptor, 7: its address
    // Outputs: 0: 0x08040040, 1: result
    void CmdDeleteFile(Request& request) {
        request.Pop<u32>();  // the transaction
        const ArchiveHandle handle = request.Pop<u64>();
        const u32 type = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const PathArgument path = PopPath(request, type, size);
        if (!path.ok) {
            return ReplyResult(request, kResultInvalidBufferDescriptor);
        }
        if (ArchiveBackend* archive = FileOpArchive(request, handle, 1)) {
            ReplyResult(request, archive->FileDelete(path.path));
        }
    }

    // Inputs:  0: 0x08060142, as DeleteFile. Outputs: 0: 0x08060040, 1: result
    void CmdDeleteDirectory(Request& request) {
        request.Pop<u32>();
        const ArchiveHandle handle = request.Pop<u64>();
        const u32 type = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const PathArgument path = PopPath(request, type, size);
        if (!path.ok) {
            return ReplyResult(request, kResultInvalidBufferDescriptor);
        }
        if (ArchiveBackend* archive = FileOpArchive(request, handle, 1)) {
            ReplyResult(request, archive->DirectoryDelete(path.path));
        }
    }

    // Inputs:  0: 0x08070142, as DeleteFile. Outputs: 0: 0x08070040, 1: result
    void CmdDeleteDirectoryRecursively(Request& request) {
        request.Pop<u32>();
        const ArchiveHandle handle = request.Pop<u64>();
        const u32 type = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const PathArgument path = PopPath(request, type, size);
        if (!path.ok) {
            return ReplyResult(request, kResultInvalidBufferDescriptor);
        }
        if (ArchiveBackend* archive = FileOpArchive(request, handle, 1)) {
            ReplyResult(request, archive->DirectoryDeleteRecursively(path.path));
        }
    }

    // Inputs:  0: 0x08080202, 1: the transaction, 2-3: the archive handle, 4: the path type, 5: its size,
    //          6: the attributes, 7-8: the file size, 9: the static buffer descriptor, 10: its address
    // Outputs: 0: 0x08080040, 1: result
    void CmdCreateFile(Request& request) {
        request.Pop<u32>();
        const ArchiveHandle handle = request.Pop<u64>();
        const u32 type = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const u32 attributes = request.Pop<u32>();
        const u64 file_size = request.Pop<u64>();
        const PathArgument path = PopPath(request, type, size);
        if (!path.ok) {
            return ReplyResult(request, kResultInvalidBufferDescriptor);
        }
        MH4N_LOG(Debug, kLog, "CreateFile attributes={} size=0x{:x}", attributes, file_size);
        if (ArchiveBackend* archive = FileOpArchive(request, handle, 1)) {
            ReplyResult(request, archive->FileCreate(path.path, file_size, attributes));
        }
    }

    // Inputs:  0: 0x08090182, 1: the transaction, 2-3: the archive handle, 4: the path type, 5: its size,
    //          6: the attributes, 7: the static buffer descriptor, 8: its address
    // Outputs: 0: 0x08090040, 1: result
    void CmdCreateDirectory(Request& request) {
        request.Pop<u32>();
        const ArchiveHandle handle = request.Pop<u64>();
        const u32 type = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const u32 attributes = request.Pop<u32>();
        const PathArgument path = PopPath(request, type, size);
        if (!path.ok) {
            return ReplyResult(request, kResultInvalidBufferDescriptor);
        }
        if (ArchiveBackend* archive = FileOpArchive(request, handle, 1)) {
            ReplyResult(request, archive->DirectoryCreate(path.path, attributes));
        }
    }

    // Inputs:  0: 0x080B0102, 1-2: the archive handle, 3: the path type, 4: its size, 5: the static buffer
    //          descriptor, 6: its address
    // Outputs: 0: 0x080B0042, 1: result, 2: the move handle descriptor, 3: the directory session (0 on an error)
    void CmdOpenDirectory(Request& request) {
        const ArchiveHandle handle = request.Pop<u64>();
        const u32 type = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const PathArgument path = PopPath(request, type, size);
        if (!path.ok) {
            return FillOpenReply(request.MakeReply(1, 2), kResultInvalidBufferDescriptor, nullptr);
        }
        ArchiveBackend* archive = FileOpArchive(request, handle, 1, 2);
        if (archive == nullptr) {
            return;
        }
        std::unique_ptr<DirectoryBackend> directory;
        const Result result = archive->DirectoryOpen(path.path, &directory);
        std::shared_ptr<ClientSession> session;
        if (!IsError(result) && directory != nullptr) {
            session = CreateDirectorySession(Context(), std::move(directory));
        }
        FillOpenReply(request.MakeReply(1, 2), result, std::move(session));
    }

    // Inputs:  0: 0x080D0144, 1-2: the archive handle, 3: the action, 4: the input size, 5: the output size,
    //          6: the input buffer descriptor, 7: its address, 8: the output buffer descriptor, 9: its address
    // Outputs: 0: 0x080D0040, 1: result
    void CmdControlArchive(Request& request) {
        const ArchiveHandle handle = request.Pop<u64>();
        const u32 action = request.Pop<u32>();
        const u32 input_size = request.Pop<u32>();
        const u32 output_size = request.Pop<u32>();
        const MappedBuffer* input = request.PopMappedBuffer();
        const MappedBuffer* output = request.PopMappedBuffer();
        if (input == nullptr || output == nullptr) {
            return ReplyResult(request, kResultInvalidBufferDescriptor);
        }
        ArchiveBackend* archive = FileOpArchive(request, handle, 1);
        if (archive == nullptr) {
            return;
        }
        // Azahar makes both buffers the size the client declared; a size beyond the mapped buffer is cut to it
        // (the bytes beyond cannot be read or written).
        std::vector<u8> in_data(std::min(input_size, input->Size()));
        if (!in_data.empty()) {
            input->Read(in_data.data(), 0, static_cast<u32>(in_data.size()));
        }
        std::vector<u8> out_data(std::min(output_size, output->Size()));
        const Result result =
            archive->Control(action, in_data.data(), in_data.size(), out_data.data(), out_data.size());
        if (!IsError(result) && output_size != 0 && !out_data.empty()) {
            output->Write(out_data.data(), 0, static_cast<u32>(out_data.size()));
        }
        ReplyResult(request, result);
    }

    // Inputs:  0: 0x08120080, 1-2: the archive handle
    // Outputs: 0: 0x08120140, 1: result, 2-3: the free bytes (0 on an error)
    void CmdGetFreeBytes(Request& request) {
        const ArchiveHandle handle = request.Pop<u64>();
        ArchiveBackend* archive = FileOpArchive(request, handle, 3);
        if (archive == nullptr) {
            return;
        }
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(kResultSuccess);
        reply.Push<u64>(archive->GetFreeBytes());
    }

    // Inputs:  0: 0x084C0242, 1: the archive id, 2: the path type, 3: its size, 4: the block size,
    //          5: the number of directories, 6: the number of files, 7: the directory buckets,
    //          8: the file buckets, 9: whether the data is duplicated, 10: the static buffer descriptor, 11: its address
    // Outputs: 0: 0x084C0040, 1: result
    void CmdFormatSaveData(Request& request) {
        const u32 archive_id = request.Pop<u32>();
        const u32 type = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const u32 block_size = request.Pop<u32>();
        const u32 directories = request.Pop<u32>();
        const u32 files = request.Pop<u32>();
        const u32 directory_buckets = request.Pop<u32>();
        const u32 file_buckets = request.Pop<u32>();
        const bool duplicate_data = request.Pop<bool>();
        const PathArgument path = PopPath(request, type, size);
        if (!path.ok) {
            return ReplyResult(request, kResultInvalidBufferDescriptor);
        }
        if (archive_id != archive_id::kSaveData) {
            MH4N_LOG(Error, kLog, "tried to format an archive different than SaveData, 0x{:08X}", archive_id);
            return ReplyResult(request, kResultInvalidPath);
        }
        if (path.path.Type() != LowPathType::Empty) {
            // "TODO(Subv): Implement formatting the SaveData of other games"
            MH4N_LOG(Error, kLog, "archive LowPath type other than empty is currently unsupported");
            return ReplyResult(request, kResultUnimplementedFs);
        }
        ArchiveFormatInfo info;
        info.duplicate_data = duplicate_data ? 1 : 0;
        info.number_directories = directories;
        info.number_files = files;
        info.total_size = block_size * 512;
        ArchiveFactory* factory = state_->archives.Factory(archive_id::kSaveData);
        ReplyResult(request, factory == nullptr
                                 ? kResultUnimplementedFs
                                 : factory->Format(path.path, info, ProgramId(), directory_buckets, file_buckets));
    }

    // Inputs:  0: 0x08510242, 1-4: the ExtSaveDataInfo (media type, unknown, the low and high ids),
    //          5: the number of directories, 6: the number of files, 7-8: the size limit, 9: the icon size,
    //          10: the mapped buffer descriptor (read), 11: its address
    // Outputs: 0: 0x08510042, 1: result, 2: the descriptor, 3: its id
    void CmdCreateExtSaveData(Request& request) {
        const u32 first = request.Pop<u32>();
        const u32 low = request.Pop<u32>();
        const u32 high = request.Pop<u32>();
        request.Pop<u32>();  // reserved
        const u32 directories = request.Pop<u32>();
        const u32 files = request.Pop<u32>();
        const u64 size_limit = request.Pop<u64>();
        const u32 icon_size = request.Pop<u32>();
        const MappedBuffer* mapped = request.PopMappedBuffer();
        if (mapped == nullptr) {
            return ReplyResult(request, kResultInvalidBufferDescriptor);
        }
        const u32 media_type = first & 0xFF;
        // The icon is read as big as the client said, or as big as its buffer if that is smaller.
        std::vector<u8> icon(std::min(icon_size, mapped->Size()));
        if (!icon.empty()) {
            mapped->Read(icon.data(), 0, static_cast<u32>(icon.size()));
        }
        ArchiveFormatInfo info;
        info.number_directories = directories;
        info.number_files = files;
        info.duplicate_data = 0;
        info.total_size = static_cast<u32>(size_limit);
        MH4N_LOG(Debug, kLog, "CreateExtSaveData high={:08X} low={:08X} media={} files={} directories={} icon_size={}",
                 high, low, media_type, files, directories, icon_size);

        ArchiveFactory* factory =
            state_->archives.Factory(media_type == kMediaNand ? archive_id::kSharedExtSaveData : archive_id::kExtSaveData);
        const Result result = factory == nullptr
                                  ? kResultUnimplementedFs
                                  : factory->FormatAsExtData(ConstructExtDataBinaryPath(media_type, high, low), info, icon);
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(result);
        reply.PushMappedBuffer(*mapped);
    }

    // Inputs:  0: 0x08520100, 1-4: the ExtSaveDataInfo. Outputs: 0: 0x08520040, 1: result
    void CmdDeleteExtSaveData(Request& request) {
        const u32 first = request.Pop<u32>();
        const u32 low = request.Pop<u32>();
        const u32 high = request.Pop<u32>();
        request.Pop<u32>();
        const u32 media_type = first & 0xFF;
        MH4N_LOG(Debug, kLog, "DeleteExtSaveData high={:08X} low={:08X} media={}", high, low, media_type);
        ArchiveFactory* factory =
            state_->archives.Factory(media_type == kMediaNand ? archive_id::kSharedExtSaveData : archive_id::kExtSaveData);
        ReplyResult(request, factory == nullptr ? kResultUnimplementedFs : factory->DeleteExtData(media_type, high, low));
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
