// Derived from Azahar src/core/hle/kernel/thread.h, kernel.h, errors.h, process.cpp, svc.cpp and
// resource_limit.cpp, src/core/hle/service/pm/pm_app.cpp and src/core/memory.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: thread.h, kernel.h and svc.cpp say so; errors.h, process.cpp,
// resource_limit.cpp, pm_app.cpp and memory.h say GPL-2.0-or-later): the processor-id and priority
// enumerations (ThreadProcessorId, ThreadPrio), the core-1 schedule modes and CPU time values
// (Core1ScheduleMode, Core1CpuTime), the TLS layout constants (TLS_AREA_VADDR, TLS_ENTRY_SIZE),
// the result code values and the FPSCR constants of the thread creation paths.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.h, kernel.h, svc.cpp,
//   memory.h)
// Copyright 2017 Citra Emulator Project (errors.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp,
//   resource_limit.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (pm_app.cpp)
// PPSSPP Project (thread.h, kernel.h, second notice: Licensed under GPLv2 or any later version)
//
// Threads (N1-27): how guest threads are made (the main thread and svcCreateThread), their
// thread-local storage, and the time limit of application threads on core 1. It sits on top of the
// kernel objects (kernel.h, N1-05) and the scheduler (scheduler.h, N1-06): the kernel's `Thread`
// stays a bare object, this module gives it a stack, a TLS slot and registers, hands it to the
// scheduler and makes it ready.
//
// Typical use (the runner, N1-09; the SVC layer, N1-07; APT, N1-14):
//   ThreadManager threads(kernel, scheduler);
//   threads.CreateMainThread(loader->Image());     // after Loader::Map and SystemPages::Map
//   ...                                           // svcCreateThread: threads.CreateThread(spec, &handle)
//   ...                                           // APT SetAppCpuTimeLimit: threads.SetAppCpuTimeLimit(n)
//
// Lifetime: a ThreadManager holds a selection filter in the scheduler and an event in its timer
// queue. Destroy it before the scheduler and the kernel (after GuestScheduler::Shutdown(), which
// drops the threads). The order of scheduler.h then reads: Shutdown(), the manager, the kernel, the
// scheduler.
//
// Departures from Azahar, all deliberate:
//  - Processors 2 and 3 are New 3DS cores. Azahar lets them through svcCreateThread and then indexes
//    a core that does not exist on an Old 3DS; here they are answered with kResultOutOfRange, the
//    answer for any other processor id that is not valid.
//  - Azahar's SetupMainThread can put the main thread of the first application to sleep for 2.6 s
//    while the LLE system modules start. There are none here, so the main thread starts at once.
//  - The TLS slot of a thread that ended is found free by looking at the thread (Dead), not by a bit
//    cleared in Thread::Stop, because kernel.h (a contract) has no place for the bitmap.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "mh4n/cpu.h"
#include "mh4n/kernel.h"
#include "mh4n/loader.h"
#include "mh4n/memory.h"
#include "mh4n/scheduler.h"
#include "mh4n/types.h"

namespace mh4n {

// ---- result codes (Azahar src/core/hle/kernel/errors.h and svc.cpp; the values are its comments') ---

// ResultOutOfRange: a priority above 63, a processor id that is not valid.
inline constexpr Result kResultOutOfRange = 0xE0E01BFD;
// ResultNotAuthorized: a priority better than the resource limit's priority value.
inline constexpr Result kResultNotAuthorized = 0xD9001BEA;
// KernelSystem::CreateThread's answer for an entry address that is not mapped
// (ErrorDescription::InvalidAddress in the kernel module, level Permanent; Azahar marks it
// "TODO: Verify error").
inline constexpr Result kResultInvalidEntryAddress = 0xD8E007F5;
// SVC::CreateThread's answer when the resource limit refuses a thread (ErrCodes::OutOfThreads). The
// limit never refuses in Azahar (ResourceLimit::Reserve always succeeds), so neither does this.
inline constexpr Result kResultOutOfThreads = 0xC860180C;
static_assert(kResultOutOfRange == MakeResult(1021, 6, 7, 28));
static_assert(kResultNotAuthorized == MakeResult(1002, 6, 8, 27));
static_assert(kResultInvalidEntryAddress == MakeResult(1013, 1, 7, 27));
static_assert(kResultOutOfThreads == MakeResult(12, 6, 3, 25));

// ---- constants -----------------------------------------------------------------------------------

// svcCreateThread's processor ids (Azahar's ThreadProcessorId).
inline constexpr s32 kThreadProcessorIdDefault = -2;  // the process's ideal processor
inline constexpr s32 kThreadProcessorIdAll = -1;      // either core: Azahar runs it on core 0
inline constexpr s32 kThreadProcessorId0 = 0;         // the application core
inline constexpr s32 kThreadProcessorId1 = 1;         // the system core
inline constexpr s32 kThreadProcessorId2 = 2;         // New 3DS only
inline constexpr s32 kThreadProcessorId3 = 3;         // New 3DS only
inline constexpr s32 kThreadProcessorIdMax = 4;       // Azahar: "Processor ID must be less than this"

// FPSCR of a thread made by svcCreateThread: default NaN, flush to zero, round toward zero. The main
// thread also has the inexact-cumulative bit (ProcessImage::main_thread_fpscr, kMainThreadFpscr).
inline constexpr u32 kThreadFpscr = 0x03C00000;

// Thread-local storage: slots of 0x200 bytes, eight to a 4 KiB page, pages from kTlsAreaVaddr up.
inline constexpr u32 kTlsEntrySize = 0x200;
inline constexpr u32 kTlsSlotsPerPage = kPageSize / kTlsEntrySize;
// The IPC command buffer is at +0x80 of a thread's slot (Azahar's Thread::GetCommandBufferAddress).
inline constexpr u32 kTlsCommandBufferOffset = 0x80;
constexpr u32 TlsSlotAddress(u32 page, u32 slot) {
    return kTlsAreaVaddr + kPageSize * page + kTlsEntrySize * slot;
}
inline u32 CommandBufferAddress(const Thread& thread) {
    return thread.tls_address + kTlsCommandBufferOffset;
}

// ---- the core-1 CPU time limit (resource_limit.cpp, thread.cpp) ----------------------------------

// Azahar's Core1ScheduleMode. (The exheader's schedule_mode field is the other way round: 0 is
// Single, 1 is Multi.) Both modes use the same limiter in Azahar, "Single" being unimplemented.
enum class Core1ScheduleMode : u32 { Multi, Single };

// Azahar's Core1CpuTime special values; any other value is a percentage.
inline constexpr s32 kCore1CpuTimeDisabled = 0;     // PREEMPTION_DISABLED
inline constexpr s32 kCore1CpuTimeSysmodule = 1000;  // PREEMPTION_SYSMODULE
inline constexpr s32 kCore1CpuTimeExempted = 10000;  // PREEMPTION_EXCEMPTED

// What ResourceLimit::ApplyAppMaxCPUSetting(process, exh_mode, exh_cpu_limit) decides: the
// schedule mode, the CpuTime limit value and its current value. `program_id` matters only for a
// 1.0 title with no CPU time set (exh_mode and exh_cpu_limit both 0), which gets 80 % as the limit
// and 25 % as the share, unless it is on the table of titles whose real kernel has other values.
struct Core1CpuSetting {
    Core1ScheduleMode mode;
    s32 limit_value;
    s32 current_value;
    friend bool operator==(const Core1CpuSetting&, const Core1CpuSetting&) = default;
};
Core1CpuSetting ComputeAppMaxCpuSetting(u64 program_id, u32 exh_mode, u32 exh_cpu_limit);

// The phase of the alternation on core 1 (Azahar's CpuLimiterMulti::SchedState): during Sys, the
// threads of the application category on core 1 are held back; during App they run.
enum class Core1Phase : u32 { App, Sys };

// ---- svcCreateThread -----------------------------------------------------------------------------

struct ThreadSpec {
    u32 entry_point = 0;  // bit 0 set: Thumb
    u32 arg = 0;          // r0
    u32 stack_top = 0;    // sp
    u32 priority = 0;
    s32 processor_id = kThreadProcessorIdDefault;
    // For tests and tools: run this C++ function on the thread's fiber instead of the guest
    // function at entry_point. It must end the thread (GuestScheduler::ExitCurrent).
    GuestThread::Entry host_entry;
};

// ---- the manager ---------------------------------------------------------------------------------

class Core1Limiter;

class ThreadManager {
public:
    ThreadManager(Kernel& kernel, GuestScheduler& scheduler);
    ~ThreadManager();
    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;

    // ---- creating threads ----

    // Process::Run's thread part and SetupMainThread for the current process (made with
    // Kernel::NewProcess(image.name) if there is none): the process takes the image's kernel
    // version, ideal processor and memory region; the CPU setting of the image is applied
    // (ApplyAppMaxCpuSetting, for the application category); one thread is reserved from the
    // resource limit (kResultOutOfThreads, as the real kernel's 0xC860180C, if it refuses); the
    // thread gets id 1 (the first the kernel makes), `main_thread_priority`, the ideal processor,
    // the stack top and the image's FPSCR, and is ready. The loader has mapped the segments and the
    // stack (Loader::Map) already. Errors: kResultOutOfRange (a priority above 63, an ideal
    // processor of 2 to 4: the exheader's field has 2 bits, so only a hand-built image has more
    // than 3), kResultOutOfRangeKernel (an ideal processor of 5 or more, hand-built images only),
    // kResultInvalidEntryAddress (the entry is not mapped), and those of TLS allocation.
    // `host_entry` is as in ThreadSpec.
    Result CreateMainThread(const ProcessImage& image, std::shared_ptr<Thread>* out = nullptr,
                            GuestThread::Entry host_entry = {});

    // SVC::CreateThread for the current process, with its checks in its order, then
    // KernelSystem::CreateThread's:
    //   1. a priority above 63: kResultOutOfRange;
    //   2. a priority numerically below the resource limit's Priority value (a better priority than
    //      the process may have): kResultNotAuthorized, unless NoThreadRestrictions();
    //   3. processor -2 is the process's ideal processor, -1 is core 0, 0 and 1 are valid; 2 and 3
    //      (New 3DS cores), 4 and above and below -2: kResultOutOfRange;
    //   4. one thread is reserved from the resource limit (kResultOutOfThreads if it refuses; the
    //      reservation is not given back if a later step fails, as in Azahar);
    //   5. an entry address that is not mapped: kResultInvalidEntryAddress;
    //   6. the thread gets its id, then its TLS slot (a new page is taken from the BASE region; its
    //      errors are kResultOutOfMemory or those of GuestMemory::Map).
    // The new thread is ready (and the scheduler is asked to reschedule); *out_handle is its handle
    // in the process's table (kResultOutOfHandles if full; the thread exists then, as in Azahar).
    Result CreateThread(const ThreadSpec& spec, Handle* out_handle,
                        std::shared_ptr<Thread>* out_thread = nullptr);

    // PROCESSOP_DISABLE_CREATE_THREAD_RESTRICTIONS of svcControlProcess: lifts check 2.
    void SetNoThreadRestrictions(bool value) { no_thread_restrictions_ = value; }
    bool NoThreadRestrictions() const { return no_thread_restrictions_; }

    // ---- thread-local storage ----

    // Pages allocated so far, and whether slot `slot` of page `page` belongs to a thread that has
    // not ended.
    std::size_t TlsPageCount() const { return tls_pages_.size(); }
    bool TlsSlotInUse(u32 page, u32 slot) const;

    // ---- the core-1 time limit ----

    // ResourceLimit::ApplyAppMaxCPUSetting for the process's resource limit: sets the CpuTime limit
    // value and current value, changes the schedule mode (which ends the limiter and starts it
    // again, so threads it held are released and it is inactive until a current value is set) and
    // updates the limiter. CreateMainThread calls it from the image.
    void ApplyAppMaxCpuSetting(u64 program_id, u32 exh_mode, u32 exh_cpu_limit);
    // PM_APP::UpdateResourceLimit(CpuTime, percent), which is what APT's SetAppCpuTimeLimit does:
    // the value becomes the current CpuTime value if it is not above the limit value (as
    // unsigned numbers, as in Azahar) and the limiter follows; otherwise nothing happens. Always
    // kResultSuccess, as in Azahar. 0 turns the limit off, anything else is the share of core 1
    // that application threads get, in percent.
    Result SetAppCpuTimeLimit(u32 percent);
    // PM_APP::GetResourceLimit(CpuTime): the current value (0 before anything was set).
    u32 AppCpuTimeLimit() const;
    // Observing the limiter (tests, logs): ready after ApplyAppMaxCpuSetting, active while the
    // current value is not 0, and the phase it is in (meaningful while active).
    Core1ScheduleMode ScheduleMode() const;
    bool Core1LimiterReady() const;
    bool Core1LimiterActive() const;
    Core1Phase Core1CurrentPhase() const;
    // Threads the limiter holds back at the moment.
    std::size_t Core1HeldThreads() const;

private:
    struct TlsPage {
        // Who holds each slot; a slot is free when its thread is gone or Dead.
        std::array<std::weak_ptr<Thread>, kTlsSlotsPerPage> owner;
    };

    Result MakeThread(std::string name, u32 entry_point, u32 priority, u32 arg, s32 processor_id,
                      u32 stack_top, u32 fpscr, const std::shared_ptr<Process>& process,
                      GuestThread::Entry host_entry, std::shared_ptr<Thread>* out);
    Result AllocateThreadLocalStorage(const std::shared_ptr<Process>& process,
                                      const std::shared_ptr<Thread>& thread);

    Kernel& kernel_;
    GuestScheduler& scheduler_;
    std::vector<TlsPage> tls_pages_;
    bool no_thread_restrictions_ = false;
    std::unique_ptr<Core1Limiter> limiter_;
};

}  // namespace mh4n
