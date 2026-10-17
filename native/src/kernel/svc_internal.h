// Derived from Azahar src/core/hle/kernel/errors.h and svc.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp says so; errors.h says
// GPL-2.0-or-later): the values of the result codes of the SVC layer (ResultMisalignedAddress,
// ResultInvalidEnumValue, ResultOutOfHeapMemory ... and the ErrCodes::OutOf* answers of the
// resource limit), built from the same description, module, summary and level.
// Copyright 2017 Citra Emulator Project (errors.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp)
//
// Shared by the two files of the SVC table (N1-07); not a public header.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>

#include "mh4n/svc.h"

struct RCtx;

namespace mh4n {

// ---- result codes the SVC layer needs besides those of kernel.h, memory.h and threads.h ------------------
// Values computed from the enumerations of Azahar's src/core/hle/result.h as its errors.h and svc.cpp
// build them. Where errors.h writes a different number in a comment, the comment is wrong and the
// computed value is what Azahar returns; the one that matters is OutOfHeapMemory (the comment says
// 0xC860180A, the arguments make 0xC8601BF3).
inline constexpr Result kResultMisalignedAddress = MakeResult(1009, 6, 7, 28);   // 0xE0E01BF1
inline constexpr Result kResultMisalignedSize = MakeResult(1010, 6, 7, 28);      // 0xE0E01BF2
inline constexpr Result kResultInvalidEnumValue = MakeResult(1005, 1, 7, 27);    // 0xD8E007ED
inline constexpr Result kResultInvalidPointer = MakeResult(1014, 1, 7, 27);      // 0xD8E007F6
inline constexpr Result kResultNotFound = MakeResult(1018, 1, 4, 27);            // 0xD88007FA
inline constexpr Result kResultPortNameTooLong = MakeResult(30, 6, 7, 28);       // 0xE0E0181E
inline constexpr Result kResultNotImplemented = MakeResult(1012, 6, 7, 28);      // 0xE0E01BF4
inline constexpr Result kResultOutOfHeapMemory = MakeResult(1011, 6, 3, 25);     // 0xC8601BF3
// ErrCodes::OutOf* with ErrorModule::OS, ErrorSummary::OutOfResource, ErrorLevel::Status.
inline constexpr Result kResultOutOfSharedMems = MakeResult(11, 6, 3, 25);       // 0xC860180B
inline constexpr Result kResultOutOfMutexes = MakeResult(13, 6, 3, 25);          // 0xC860180D
inline constexpr Result kResultOutOfSemaphores = MakeResult(14, 6, 3, 25);       // 0xC860180E
inline constexpr Result kResultOutOfEvents = MakeResult(15, 6, 3, 25);           // 0xC860180F
inline constexpr Result kResultOutOfTimers = MakeResult(16, 6, 3, 25);           // 0xC8601810
inline constexpr Result kResultOutOfAddressArbiters = MakeResult(51, 6, 3, 25);  // 0xC8601833

// The registers of the call being run and the thread that makes it.
struct SvcFrame {
    RCtx& ctx;
    Thread& thread;
    u32 number;
};

struct SvcTable::Impl {
    Impl(Kernel& kernel_, GuestScheduler& scheduler_, ThreadManager& threads_)
        : kernel(kernel_), sched(scheduler_), threads(threads_), memory(kernel_.Memory()) {}

    Kernel& kernel;
    GuestScheduler& sched;
    ThreadManager& threads;
    GuestMemory& memory;

    SvcHooks hooks;
    ConnectToPortHandler connect_to_port;
    SendSyncRequestHandler send_sync_request;
    std::map<u32, SvcCall> pending;  // by thread id
    std::optional<SvcExit> exit;
    std::set<u32> unknown_logged;

    // ---- svc.cpp ----
    void Call(RCtx& ctx, u32 imm, u32 next_pc);
    void Dispatch(SvcFrame& f);
    void LogUnknown(const SvcFrame& f);
    // Ends the run: the first of ExitProcess and Break decides the status. Does not return.
    [[noreturn]] void EndRun(const SvcFrame& f);

    void CreateThread(SvcFrame& f);
    void SleepThread(SvcFrame& f);
    void GetThreadPriority(SvcFrame& f);
    void SetThreadPriority(SvcFrame& f);
    void CreateMutex(SvcFrame& f);
    void ReleaseMutex(SvcFrame& f);
    void CreateSemaphore(SvcFrame& f);
    void ReleaseSemaphore(SvcFrame& f);
    void CreateEvent(SvcFrame& f);
    void SignalEvent(SvcFrame& f);
    void ClearEvent(SvcFrame& f);
    void CreateTimer(SvcFrame& f);
    void SetTimer(SvcFrame& f);
    void CancelTimer(SvcFrame& f);
    void ClearTimer(SvcFrame& f);
    void CreateAddressArbiter(SvcFrame& f);
    void ArbitrateAddress(SvcFrame& f);
    void CloseHandle(SvcFrame& f);
    void WaitSynchronization1(SvcFrame& f);
    void WaitSynchronizationN(SvcFrame& f);
    void DuplicateHandle(SvcFrame& f);
    void GetSystemTick(SvcFrame& f);
    void GetHandleInfo(SvcFrame& f);
    void GetSystemInfo(SvcFrame& f);
    void GetProcessInfo(SvcFrame& f);
    void GetThreadInfo(SvcFrame& f);
    void ConnectToPort(SvcFrame& f);
    void SendSyncRequest(SvcFrame& f);
    void GetProcessId(SvcFrame& f);
    void GetThreadId(SvcFrame& f);
    void GetResourceLimit(SvcFrame& f);
    void GetResourceLimitValues(SvcFrame& f, bool current);
    void Break(SvcFrame& f);
    void OutputDebugString(SvcFrame& f);
    void ControlProcess(SvcFrame& f);

    // ---- svc_memory.cpp ----
    void ControlMemory(SvcFrame& f);
    void QueryMemory(SvcFrame& f);
    void CreateMemoryBlock(SvcFrame& f);
    void MapMemoryBlock(SvcFrame& f);
    void UnmapMemoryBlock(SvcFrame& f);

    // Azahar's Process::HeapAllocate/HeapFree/LinearAllocate/LinearFree and the checks of
    // SVC::ControlMemory, on GuestMemory and FcramRegions.
    Result ControlMemoryOp(u32 operation, u32 addr0, u32 addr1, u32 size, u32 permissions,
                           u32* out_addr);
    Result HeapAllocate(u32* out_addr, u32 target, u32 size, MemPerm perm);
    Result HeapFree(u32 target, u32 size);
    Result LinearAllocate(u32* out_addr, u32 target, u32 size, MemPerm perm);
    Result LinearFree(u32 target, u32 size);
    u32 LinearHeapArea() const;

    // Where the FCRAM behind each piece of the heap came from, by guest address, so that FREE gives
    // the same ranges back (GuestMemory does not keep the backing, only a token for merging).
    struct HeapRun {
        u32 size;
        u32 fcram_offset;
    };
    std::map<u32, HeapRun> heap_runs;
    bool map_unmap_logged = false;
};

// ---- register helpers --------------------------------------------------------------------------------

u64 SvcGet64(const RCtx& ctx, int low, int high);
void SvcPut64(RCtx& ctx, int low, int high, u64 value);

}  // namespace mh4n
