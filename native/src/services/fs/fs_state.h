// Derived from Azahar src/core/hle/service/fs/archive.h, src/core/hle/service/fs/archive.cpp and
// src/core/hle/service/fs/fs_user.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// archive.h says "GPLv3 or any later version"; the others say "GPLv2 or any later version"): the archive
// registry and the archive handles (ArchiveManager: RegisterArchiveType, OpenArchive, CloseArchive,
// GetArchive; handles are numbered from 1 and never reused while open), and the state fs:USER shares
// between its sessions (the priority, -1 until set).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (archive.h, archive.cpp, fs_user.h)
//
// The state of the file system services (N1-12): what every fs:USER session and every file session
// share. It lives in the ServiceContext (`ServiceContext::State<FsState>("fs:USER")`) and is made on
// first use by `GetFsState`.
//
// Archives register themselves, like services do (mh4n/registry.h):
//
//     MH4N_REGISTER(ArchiveProvider, "03.selfncch", ArchiveProvider{archive_id::kSelfNCCH, [](FsState& s) {
//         return std::make_unique<MyFactory>(s); }});
//
// N1-13 adds save data and extdata that way, from files of its own (archive_savedata.cpp,
// archive_extsavedata.cpp).
//
// Threads (principle 6): everything here belongs to the scheduler thread except `Pool()`'s workers,
// which run the `work` of `StartIo`.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "fs_types.h"
#include "mh4n/service.h"
#include "positioned_file.h"

namespace mh4n::fs {

// ---- when replies arrive (D26) -----------------------------------------------------------------------------

// --io-latency: `azahar` answers no earlier than Azahar's modelled delay, on the game clock; `none` as
// soon as the data is read.
enum class IoLatency { Azahar, None };
// "azahar" or "none"; false for anything else.
bool ParseIoLatency(const std::string& text, IoLatency* out);

class IoPool;

// ---- archives -------------------------------------------------------------------------------------------------

// Handles are numbered as Azahar's ArchiveManager numbers them (archive.cpp, OpenArchive): from 1, one number
// for each open that succeeds, none for one that fails (an id nobody registered, an archive that is not
// there), none given back when an archive is closed, and a number in use is skipped. fs:USER's
// OpenFileDirectly opens an archive and closes it again, so it takes a number too, whether or not its file
// opens. The numbers matter: a replayed request carries the handle the recording had (docs/native/ipc-replay.md).
class ArchiveManager {
public:
    // False if the id is taken.
    bool Register(u32 id_code, std::unique_ptr<ArchiveFactory> factory);
    // kResultNotFound for an id nobody registered; otherwise what the factory says.
    Result Open(u32 id_code, const Path& path, u64 program_id, ArchiveHandle* handle);
    Result Close(ArchiveHandle handle);  // kResultInvalidArchiveHandle if it is not open
    ArchiveBackend* Get(ArchiveHandle handle);
    // The factory registered for an id, or null (N1-13: Format and the extdata commands go to it).
    ArchiveFactory* Factory(u32 id_code);

private:
    std::map<u32, std::unique_ptr<ArchiveFactory>> factories_;
    std::map<ArchiveHandle, std::unique_ptr<ArchiveBackend>> open_;
    ArchiveHandle next_handle_ = 1;
};

// ---- the game folder --------------------------------------------------------------------------------------------

// The RomFS of an image starts 0x1000 bytes into the NCCH's RomFS section (the IVFC header, which the SDK
// does not read through the file system services): Azahar's NCCHContainer::ReadRomFS reads
// [offset + 0x1000, offset + size).
inline constexpr u64 kRomFsHeaderSize = 0x1000;

// What the game folder (N1-03) gives the file system. A part that is missing or unreadable is null.
struct GameFiles {
    std::shared_ptr<PositionedFile> romfs;         // romfs.bin: the base game's RomFS section
    std::shared_ptr<PositionedFile> update_romfs;  // update_romfs.bin: the update's
    std::shared_ptr<PositionedFile> code;          // code.bin: the update's decompressed .code
    std::shared_ptr<PositionedFile> icon;          // icon.bin: the base game's ExeFS 'icon' file (N1-12e)
    u64 program_id = 0;                            // the base title's id (exheader.bin), 0 if unknown
};

// ---- the shared state -----------------------------------------------------------------------------------------------

// What a factory is made from.
struct FsState;
struct ArchiveProvider {
    u32 id_code;
    std::function<std::unique_ptr<ArchiveFactory>(FsState&)> make;
};

struct FsState {
    FsState();
    ~FsState();
    FsState(const FsState&) = delete;
    FsState& operator=(const FsState&) = delete;

    // Reads the options and the game folder, makes the archives. Called once by GetFsState.
    void Init(ServiceContext& context);

    bool initialized = false;
    IoLatency latency = IoLatency::Azahar;
    u32 priority = 0xFFFFFFFFu;  // SetPriority / GetPriority (Azahar's FS_USER::priority is -1 at first)
    // The operations whose `work` has not finished on its I/O thread (it counts down after the completion
    // has been posted). A clock that skips idle time, as ManualTimeSource does, must not skip it while this
    // is not 0: the host is still reading. Any thread. (Declared before `pool`: the pool's threads are joined
    // first.)
    std::atomic<int> io_in_flight{0};
    GameFiles game;
    // --user, with a slash at the end; empty without it. The save data and extdata archives live under
    // it, in Azahar's layout (N1-13).
    std::string user_folder;
    ArchiveManager archives;
    std::unique_ptr<IoPool> pool;

    // The id the client sessions that called Initialize have (the SelfNCCH data is registered under it).
    u64 ClientProgramId() const { return game.program_id != 0 ? game.program_id : kUnknownProgramId; }
    static constexpr u64 kUnknownProgramId = 1;
};

// The state of this runtime's file system, made on first use. The services keep a copy of the pointer, so
// the state outlives the sessions that use it.
std::shared_ptr<FsState> GetFsState(ServiceContext& context);

// ---- file sessions ------------------------------------------------------------------------------------------------

// A session of a file the game opened: the client side to push into the reply of OpenFile. `priority` is
// what GetPriority starts with. Scheduler thread.
std::shared_ptr<ClientSession> CreateFileSession(ServiceContext& context, std::shared_ptr<FsState> state,
                                                 std::shared_ptr<FileBackend> backend, u32 priority);

// A session of a directory the game opened (N1-13): the client side to push into the reply of OpenDirectory.
std::shared_ptr<ClientSession> CreateDirectorySession(ServiceContext& context, std::unique_ptr<DirectoryBackend> backend);

// Makes the I/O pool of `state` (io.cpp, where the pool is a complete type).
struct FsState;
void MakeIoPool(FsState& state, Kernel& kernel);

// ---- running I/O ----------------------------------------------------------------------------------------------------

// Starts an operation on behalf of a request that returns Handled::Deferred. `work` runs on an I/O thread
// (it may be empty); `finish` runs once on the scheduler thread, after `work` is done and, with
// IoLatency::Azahar, not before `delay_ns` of game time have passed since this call. The data `work`
// leaves for `finish` travels in what both lambdas capture. Scheduler thread.
void StartIo(FsState& fs, ServiceContext& context, u64 delay_ns, std::function<void()> work,
             std::function<void()> finish);

// True if a request with this delay must be answered later than the request itself (so the handler
// returns Handled::Deferred and calls StartIo): there is a delay and the latency is `azahar`.
inline bool NeedsDelay(const FsState& fs, u64 delay_ns) {
    return fs.latency == IoLatency::Azahar && delay_ns > 0;
}

}  // namespace mh4n::fs
