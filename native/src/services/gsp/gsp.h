// Derived from Azahar src/core/hle/service/gsp/gsp_gpu.cpp, src/core/hle/service/gsp/gsp_gpu.h,
// src/core/hle/service/gsp/gsp_command.h, src/core/hle/service/gsp/gsp_interrupt.h,
// src/video_core/gpu.h and src/core/core_timing.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: each of them says "GPLv2 or any later version"): the layout of the GSP shared
// memory (the interrupt relay queue, the framebuffer updates, the GX command buffer), the result
// codes, the limits, the VBlank length and the VRAM backup banks.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (gsp_gpu.cpp, gsp_gpu.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gsp_interrupt.h, gpu.h)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
//
// The state of the GSP service that all its sessions share (N1-17): the shared memory block, which
// session has the GPU right, the interrupts, the VBlank timer, the framebuffer information and the
// VRAM backup. Private to mh4n_services_gsp; the tests include it.
//
// Where the pieces are:
//  - gsp_gpu.cpp is the `gsp::Gpu` service (one session = one GSP thread index, at most four);
//  - gsp_lcd.cpp is `gsp::Lcd`;
//  - state.cpp is this class and the `services` boot step that wires the runtime's sinks.
//
// Interrupts (D25): the GPU calls Host::SignalInterrupt(id, delay), which reaches `SignalInterrupt`
// here through Runtime::SetGpuInterruptSink. The interrupt is written into the queue of each thread
// it goes to and the thread's event is signalled at once, as Azahar's SignalInterruptForThread does
// with `simulate_3ds_gpu_timings` off and a title that is not on the texture-copy list: PDC0 and
// PDC1 go to every registered thread, the others to the thread that has the GPU right.
//
// VBlank: PDC0 and PDC1 come together every `VBlankPeriodTicks()` game ticks, from a timer-queue
// event that is rescheduled from its own deadline (no drift). The period is 4,481,136 ticks unless
// Runtime::SetRefreshRate says otherwise (VBlankPeriodForRate).
//
// Threads (principle 6): everything here runs on the scheduler thread. The queue in shared memory
// is popped by the game with LDREX/STREX; it is written only from here, only on that thread.
#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/gpu.h"
#include "mh4n/host.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/memory.h"
#include "mh4n/service.h"
#include "mh4n/types.h"

namespace mh4n::gsp {

// ---- limits ---------------------------------------------------------------------------------------

// Sessions of gsp::Gpu, and so the GSP thread indices (Azahar's MaxGSPThreads and its max sessions).
inline constexpr u32 kMaxThreads = 4;
inline constexpr u32 kNoThread = 0xFFFFFFFF;
inline constexpr u32 kSharedMemorySize = 0x1000;
// WriteHWRegs addresses are offsets from here (GSP_GPU's REGS_BEGIN).
inline constexpr u32 kRegsBegin = 0x1EB00000;
inline constexpr u32 kRegsLimit = 0x420000;  // an offset at or above it is out of range
inline constexpr u32 kMaxRegBytes = 0x80;    // "verified to be done by the gsp module"

// ---- results (Azahar's ErrCodes and result.h; the values are the comments' in gsp_gpu.cpp) ---------

inline constexpr Result kResultRegsOutOfRangeOrMisaligned = 0xE0E02A01;
inline constexpr Result kResultRegsMisaligned = 0xE0E02BF2;
inline constexpr Result kResultRegsInvalidSize = 0xE0E02BEC;
inline constexpr Result kResultNoSaveVramSysAreaPerm = MakeResult(518, 10, 1, 27);
// The result of the first RegisterInterruptRelayQueue: "required for a successful initialization,
// rather than 0".
inline constexpr Result kResultFirstInitialization = MakeResult(519, 10, 0, 0);
inline constexpr Result kResultAlreadyDone = MakeResult(1003, 10, 0, 0);
inline constexpr Result kResultBusy = MakeResult(1008, 10, 2, 25);
static_assert(kResultRegsOutOfRangeOrMisaligned == MakeResult(513, 10, 7, 28));
static_assert(kResultRegsMisaligned == MakeResult(1010, 10, 7, 28));
static_assert(kResultRegsInvalidSize == MakeResult(1004, 10, 7, 28));
static_assert(kResultFirstInitialization == 0x2A07);

// ---- the shared memory (3dbrew "GSP Shared Memory", Azahar's gsp_interrupt.h and gsp_command.h) ----

// Thread t's interrupt relay queue is at 0x40 * t.
struct InterruptRelayQueue {
    static constexpr u32 kMaxSlots = 0x34;
    static constexpr u32 kStopQueuingPdcThreshold = 0x20;
    static constexpr u8 kQueueFullError = 0x1;

    u8 index;              // the oldest interrupt in the queue
    u8 number_interrupts;  // how many the game has yet to pop
    u8 error_code;         // 0, or kQueueFullError
    u8 config;             // bit 0: ignore_pdc
    u32 missed_pdc0;
    u32 missed_pdc1;
    u8 slot[kMaxSlots];    // gpu::Interrupt ids

    bool IgnorePdc() const { return (config & 1) != 0; }
};
static_assert(sizeof(InterruptRelayQueue) == 0x40);

// Thread t's framebuffer updates are at 0x200 + (2 * t + screen) * 0x40.
inline constexpr u32 kFrameBufferUpdateOffset = 0x200;
struct FrameBufferUpdate {
    u8 index;     // bit 0: which of the two infos is the current one
    u8 is_dirty;  // bit 0: GSP should update the GPU's framebuffer registers
    u16 pad1;
    gpu::FrameBufferInfo framebuffer_info[2];
    u32 pad2;
};
static_assert(sizeof(FrameBufferUpdate) == 0x40);
static_assert(offsetof(FrameBufferUpdate, framebuffer_info[1]) == 0x20);

// Thread t's GX command buffer is at 0x800 + 0x200 * t: a header word, seven unknown words and 15
// commands of eight words.
inline constexpr u32 kCommandBufferOffset = 0x800;
inline constexpr u32 kCommandBufferSize = 0x200;
inline constexpr u32 kCommandBufferCommands = 0xF;
inline constexpr u32 kCommandBufferStatusStopped = 0x1;
// The header word: index bits 0-7, number of commands 8-15, status 16-23, should_stop 24-31.
// A command's first word: id bits 0-7, stop 16-23.

// The saved framebuffers' places in VRAM, and the banks SaveVramSysArea backs up.
inline constexpr u32 kFramebufferWidth = 240;
inline constexpr u32 kTopFramebufferHeight = 400;
inline constexpr u32 kBottomFramebufferHeight = 320;
inline constexpr u32 kFramebufferSaveAreaTopLeft = kVramVaddr + 0x273000;
inline constexpr u32 kFramebufferSaveAreaTopRight = kVramVaddr + 0x2B9800;
inline constexpr u32 kFramebufferSaveAreaBottom = kVramVaddr + 0x4C7800;
inline constexpr u32 kVramBackupBankSize = 0x80000;
inline constexpr u32 kVramBackupBanks = kVramSize / kVramBackupBankSize;

// RelayEventQueueFlags::AllowSaveVramSysArea.
inline constexpr u32 kAllowSaveVramSysArea = 1u << 0;

// ---- VBlank ------------------------------------------------------------------------------------------

// "Measured on hardware to be 2240568 timer cycles or 4481136 ARM11 cycles" (FRAME_TICKS).
inline constexpr u64 kFrameTicks = 4481136;
// The longest VBlank period accepted: an hour. A refresh rate that would give a longer one, or none
// (more than the tick rate), is clamped; see VBlankPeriodForRate.
inline constexpr u64 kMaxVBlankPeriodTicks = 3600 * kTicksPerSecond;

// The VBlank length in ticks for `hz` as port/azahar/azahar.patch computes it: BASE_CLOCK_RATE_ARM11
// / hz, truncated, and kFrameTicks for 0 (or anything that is not a positive number). The result is
// at least 1 and at most kMaxVBlankPeriodTicks (the patch has neither bound: a period of 0 would
// never let time pass, and a huge one does not fit a u64); `clamped` says whether a bound applied.
u64 VBlankPeriodForRate(double hz, bool* clamped = nullptr);

// ---- the state ---------------------------------------------------------------------------------------

class GspState {
public:
    GspState();
    ~GspState();
    GspState(const GspState&) = delete;
    GspState& operator=(const GspState&) = delete;

    // The state of the runtime `context` belongs to: made on the first call, with its shared memory.
    static std::shared_ptr<GspState> Get(ServiceContext& context);

    // ---- the services' side ----
    // The GSP thread index a new session gets (Azahar: the lowest unused), or kNoThread.
    u32 ClaimThread();
    // The client closed the session: Azahar's ClientDisconnected and ~SessionData.
    void ReleaseThread(u32 thread);

    // What a thread's session registered with RegisterInterruptRelayQueue.
    struct Waiter {
        std::shared_ptr<Request> request;
        u16 command_id;
    };
    struct ThreadSlot {
        bool used = false;
        bool registered = false;
        u32 flags = 0;  // RelayEventQueueFlags
        std::shared_ptr<Event> interrupt_event;
        std::vector<Waiter> vblank_waiters;  // SaveVramSysArea and RestoreVramSysArea, until PDC1
    };
    ThreadSlot& Slot(u32 thread) { return slots_[thread]; }

    const std::shared_ptr<SharedMemory>& SharedMem() const { return shared_memory_; }
    u32 ThreadWithRights() const { return thread_with_rights_; }
    void SetThreadWithRights(u32 thread) { thread_with_rights_ = thread; }
    void ReleaseRight(u32 thread);
    // True once, for the first RegisterInterruptRelayQueue of the run.
    bool TakeFirstInitialization();

    gpu::Gpu* GetGpu() const { return interfaces_ != nullptr ? interfaces_->gpu : nullptr; }
    GuestMemory& Memory() const { return *memory_; }
    Kernel& GetKernel() const { return *kernel_; }

    // ---- shared memory ----
    // A pointer to `size` bytes at `offset` of the block, or null if they are not inside it.
    u8* SharedPointer(u32 offset, u32 size = 1);
    InterruptRelayQueue* Queue(u32 thread);
    // Azahar's GetFrameBufferInfo: the dirty flag of thread's update for `screen` and the cached
    // copy of the current info. With `force_update` the info is taken even if the flag is clear.
    // thread == kNoThread: the cached copy only.
    std::pair<bool, gpu::FrameBufferInfo*> GetFrameBufferInfo(u32 thread, u32 screen,
                                                              bool force_update = false);
    void ForceBufferSwap(u32 screen) { force_buffer_swap_[screen] = true; }

    // ---- the GX command queue ----
    // TriggerCmdReqQueue's work: runs the commands in the command buffer of the thread with the
    // right, through Gpu::Execute.
    void ProcessCommandQueue();

    // ---- interrupts ----
    // The runtime's interrupt sink.
    void SignalInterrupt(gpu::Interrupt id);

    // ---- VBlank ----
    // Schedules the first VBlank, one period from now. Once.
    void Start();
    // The runtime's refresh-rate sink: the period for the next VBlank to be scheduled from.
    void SetRefreshRate(double hz);
    u64 VBlankPeriodTicks() const { return period_ticks_; }
    // The tick of the VBlank that is pending (0 before Start).
    u64 NextVBlankTick() const { return next_vblank_tick_; }
    u64 VBlankCount() const { return vblank_count_; }

    // ---- the performance log (SetPerfLogMode, GetPerfLog) ----
    void SetPerfLogEnabled(bool enabled);
    // The 14 words GetPerfLog answers with: delta and sum for each of the seven interrupts.
    std::array<u32, 14> PerfLog() const { return perf_; }

    // ---- SaveVramSysArea and RestoreVramSysArea ----
    // The work of the two commands, without the permission check and the wait for the VBlank, which
    // are the service's (see Waiter).
    void SaveVramSysArea();
    void RestoreVramSysArea();

private:
    void Init(ServiceContext& context);
    void SignalInterruptForThread(gpu::Interrupt id, u32 thread);
    void ProcessInterrupt(gpu::Interrupt id, u32 thread);
    void OnVBlank();
    void ScheduleVBlank();
    void CopyFrameBuffer(u32 dst, u32 src, u32 dst_stride, u32 src_stride, u32 lines);
    void ClearFramebuffer(u32 dst, u32 dst_stride, u32 lines);
    void SaveVramWithState(u32 state);
    void RestoreVramWithState(u32 state);
    void PerfUpdate(gpu::Interrupt id, u64 nanoseconds);

    Kernel* kernel_ = nullptr;
    GuestMemory* memory_ = nullptr;
    HostInterfaces* interfaces_ = nullptr;
    std::shared_ptr<SharedMemory> shared_memory_;

    std::array<ThreadSlot, kMaxThreads> slots_;
    u32 thread_with_rights_ = kNoThread;
    bool first_initialization_ = true;

    std::array<gpu::FrameBufferInfo, 2> cached_framebuffer_infos_{};
    std::array<bool, 2> force_buffer_swap_{};

    u64 period_ticks_ = kFrameTicks;
    u64 next_vblank_tick_ = 0;
    u64 vblank_event_ = 0;
    u64 vblank_count_ = 0;
    bool started_ = false;

    bool perf_enabled_ = false;
    std::array<u32, 14> perf_{};

    // The VRAM banks, saved lazily (6 MiB), and Azahar's three states of the backup handler.
    std::vector<std::vector<u8>> vram_backup_;
    u32 vram_state_ = 0;
};

}  // namespace mh4n::gsp
