// Derived from Azahar src/core/hle/service/gsp/gsp_gpu.cpp, src/core/hle/service/gsp/gsp_gpu.h,
// src/core/hle/service/gsp/gsp_command.h, src/core/hle/service/gsp/gsp_interrupt.h,
// src/video_core/gpu.cpp, src/video_core/gpu.h and src/core/core_timing.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the interrupt relay queue and its rules (SignalInterrupt, SignalInterruptForThread,
// ProcessPendingInterruptImpl), the framebuffer update at VBlank (GetFrameBufferInfo), the GX command
// queue loop of TriggerCmdReqQueue, the VBlank callback and its rescheduling (GPU::VBlankCallback),
// the performance log, and SaveVramSysArea / RestoreVramSysArea with the VRAM backup handler.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (gsp_gpu.cpp, gsp_gpu.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gsp_interrupt.h, gpu.cpp, gpu.h)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
#include <algorithm>
#include <cstring>

#include "gsp.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::gsp {

namespace {

constexpr const char* kLog = "gsp";

// The bank masks of the VRAM backup handler (VRAMBankBackupMask).
constexpr u32 kMask0 = 1u << 0;
constexpr u32 kMask1 = 1u << 1;
constexpr u32 kMask2 = 1u << 2;
constexpr std::array<u32, kVramBackupBanks> kMaskPerBank = {
    kMask0,            // 0x1F000000 - 0x1F07FFFF
    kMask0,            // 0x1F080000 - 0x1F0FFFFF
    kMask0,            // 0x1F100000 - 0x1F17FFFF
    kMask0 | kMask1,   // 0x1F180000 - 0x1F1FFFFF
    kMask0 | kMask1,   // 0x1F200000 - 0x1F27FFFF
    kMask0 | kMask1,   // 0x1F280000 - 0x1F2FFFFF
    kMask0,            // 0x1F300000 - 0x1F37FFFF
    kMask0 | kMask2,   // 0x1F380000 - 0x1F3FFFFF
    kMask0 | kMask2,   // 0x1F400000 - 0x1F47FFFF
    kMask0 | kMask1,   // 0x1F480000 - 0x1F4FFFFF
    kMask0 | kMask1,   // 0x1F500000 - 0x1F57FFFF
    kMask0 | kMask1,   // 0x1F580000 - 0x1F5FFFFF
};

}  // namespace

u64 VBlankPeriodForRate(double hz, bool* clamped) {
    if (clamped != nullptr) {
        *clamped = false;
    }
    if (!(hz > 0.0)) {
        return kFrameTicks;  // 0 means the default; so does anything that is not a positive number
    }
    const double ticks = static_cast<double>(kTicksPerSecond) / hz;
    if (ticks >= static_cast<double>(kMaxVBlankPeriodTicks)) {
        if (clamped != nullptr) {
            *clamped = true;
        }
        return kMaxVBlankPeriodTicks;
    }
    const u64 period = static_cast<u64>(ticks);
    if (period < 1) {
        if (clamped != nullptr) {
            *clamped = true;
        }
        return 1;
    }
    return period;
}

GspState::GspState() = default;

GspState::~GspState() {
    if (kernel_ != nullptr && vblank_event_ != 0) {
        kernel_->Cancel(vblank_event_);
    }
}

std::shared_ptr<GspState> GspState::Get(ServiceContext& context) {
    std::shared_ptr<GspState> state = context.State<GspState>("gsp::Gpu");
    state->Init(context);
    return state;
}

void GspState::Init(ServiceContext& context) {
    if (kernel_ != nullptr) {
        return;
    }
    kernel_ = &context.GetKernel();
    memory_ = &context.Memory();
    interfaces_ = &context.Interfaces();
    // CreateSharedMemory(nullptr, 0x1000, ReadWrite, ReadWrite, 0, BASE, "GSP:SharedMemory"): a block
    // the kernel allocates itself, so the application maps it with DontCare.
    const Result result = kernel_->NewSharedMemory(
        &shared_memory_, nullptr, kSharedMemorySize, MemoryPermission::ReadWrite,
        MemoryPermission::ReadWrite, 0, FcramRegion::Base, "GSP:SharedMemory");
    if (IsError(result)) {
        MH4N_LOG(Critical, kLog, "cannot allocate the GSP shared memory (0x{:08X})", result);
        shared_memory_ = nullptr;
    }
}

// ---- threads ------------------------------------------------------------------------------------------------

u32 GspState::ClaimThread() {
    for (u32 thread = 0; thread < kMaxThreads; ++thread) {
        if (!slots_[thread].used) {
            slots_[thread] = ThreadSlot{};
            slots_[thread].used = true;
            return thread;
        }
    }
    MH4N_LOG(Error, kLog, "all GSP threads are in use");
    return kNoThread;
}

void GspState::ReleaseThread(u32 thread) {
    if (thread >= kMaxThreads) {
        return;
    }
    // ClientDisconnected, then ~SessionData.
    ThreadSlot& slot = slots_[thread];
    slot.interrupt_event = nullptr;
    slot.registered = false;
    if (thread_with_rights_ == thread) {
        ReleaseRight(thread);
    }
    if ((slot.flags & kAllowSaveVramSysArea) != 0) {
        vram_state_ = 0;  // vram_backup_handler.ResetState()
    }
    slot = ThreadSlot{};
}

void GspState::ReleaseRight(u32 thread) {
    if (thread_with_rights_ == thread) {
        thread_with_rights_ = kNoThread;
    }
}

bool GspState::TakeFirstInitialization() {
    const bool first = first_initialization_;
    first_initialization_ = false;
    return first;
}

// ---- shared memory --------------------------------------------------------------------------------------------

u8* GspState::SharedPointer(u32 offset, u32 size) {
    if (!shared_memory_ || static_cast<u64>(offset) + size > kSharedMemorySize) {
        return nullptr;
    }
    return shared_memory_->GetPointer(offset);
}

InterruptRelayQueue* GspState::Queue(u32 thread) {
    return reinterpret_cast<InterruptRelayQueue*>(
        SharedPointer(static_cast<u32>(sizeof(InterruptRelayQueue)) * thread, sizeof(InterruptRelayQueue)));
}

std::pair<bool, gpu::FrameBufferInfo*> GspState::GetFrameBufferInfo(u32 thread, u32 screen,
                                                                    bool force_update) {
    bool is_dirty = false;

    // Matches real HW behaviour of a cached copy of the framebuffer info being maintained that is
    // only updated with the dirty flag every time the FB info is accessed. If the thread ID is
    // invalid, the last cached data is used.
    if (thread != kNoThread && screen < 2) {
        // For each thread there are two FrameBufferUpdate fields.
        const u32 offset =
            kFrameBufferUpdateOffset + (2 * thread + screen) * static_cast<u32>(sizeof(FrameBufferUpdate));
        auto* update = reinterpret_cast<FrameBufferUpdate*>(SharedPointer(offset, sizeof(FrameBufferUpdate)));
        if (update != nullptr && ((update->is_dirty & 1) != 0 || force_update)) {
            cached_framebuffer_infos_[screen] = update->framebuffer_info[update->index & 1];
            update->is_dirty = static_cast<u8>(update->is_dirty & ~1u);
            is_dirty = true;
        }
    }
    return {is_dirty, &cached_framebuffer_infos_[screen < 2 ? screen : 0]};
}

// ---- interrupts -----------------------------------------------------------------------------------------------

void GspState::SignalInterrupt(gpu::Interrupt id) {
    if (!shared_memory_) {
        MH4N_LOG(Warning, kLog, "cannot synchronize until GSP shared memory has been created!");
        return;
    }
    // The PDC0 and PDC1 interrupts are fired even if the GPU right hasn't been acquired. Normal
    // interrupts are only signaled for the active thread (ie, the thread that has the GPU right),
    // but the PDC0/1 interrupts are signaled for every registered thread.
    if (id == gpu::Interrupt::PDC0 || id == gpu::Interrupt::PDC1) {
        for (u32 thread = 0; thread < kMaxThreads; ++thread) {
            SignalInterruptForThread(id, thread);
        }
        return;
    }
    // For normal interrupts, don't do anything if no process has acquired the GPU right.
    if (thread_with_rights_ == kNoThread) {
        return;
    }
    SignalInterruptForThread(id, thread_with_rights_);
}

void GspState::SignalInterruptForThread(gpu::Interrupt id, u32 thread) {
    // With `simulate_3ds_gpu_timings` off and no texture-copy delay for the title, Azahar's wait
    // delay is 0 for every interrupt but the VBlanks, which are signalled from an event that already
    // has the proper timing: the interrupt is processed here and now (D25).
    if (perf_enabled_) {
        if (id == gpu::Interrupt::PDC0 || id == gpu::Interrupt::PDC1) {
            constexpr u64 nanoseconds_per_frame = static_cast<u64>(
                (static_cast<double>(kFrameTicks) / static_cast<double>(kTicksPerSecond)) * 1e9);
            PerfUpdate(id, nanoseconds_per_frame);
        } else {
            // Azahar records the delay the GPU passed; the sink does not get it.
            PerfUpdate(id, 0);
        }
    }
    ProcessInterrupt(id, thread);
}

void GspState::ProcessInterrupt(gpu::Interrupt id, u32 thread) {
    // FindRegisteredThreadData
    ThreadSlot& slot = slots_[thread];
    if (!slot.used || !slot.registered) {
        return;
    }
    if (slot.interrupt_event == nullptr) {
        MH4N_LOG(Warning, kLog, "cannot synchronize until GSP event has been created!");
        return;
    }
    InterruptRelayQueue* queue = Queue(thread);
    if (queue == nullptr) {
        return;
    }

    const bool is_pdc = id == gpu::Interrupt::PDC0 || id == gpu::Interrupt::PDC1;
    auto queue_interrupt = [&]() {
        if (queue->number_interrupts >= InterruptRelayQueue::kMaxSlots) {
            queue->error_code = InterruptRelayQueue::kQueueFullError;
        } else {
            u8 next = queue->index;
            next += queue->number_interrupts;
            next %= InterruptRelayQueue::kMaxSlots;

            queue->number_interrupts += 1;

            queue->slot[next] = static_cast<u8>(id);

            slot.interrupt_event->Signal();
        }
    };

    if (is_pdc) {
        if (!queue->IgnorePdc()) {
            if (queue->number_interrupts >= InterruptRelayQueue::kStopQueuingPdcThreshold) {
                if (id == gpu::Interrupt::PDC0) {
                    queue->missed_pdc0++;
                } else {
                    queue->missed_pdc1++;
                }
            } else {
                queue_interrupt();
            }
        }

        // Update framebuffer information if requested.
        const u32 screen = id == gpu::Interrupt::PDC0 ? 0 : 1;
        const auto info = GetFrameBufferInfo(thread, screen);
        if (info.first || force_buffer_swap_[screen]) {
            force_buffer_swap_[screen] = false;
            if (gpu::Gpu* gpu = GetGpu()) {
                gpu->SetBufferSwap(screen, *info.second);
            }
        }

        // If it's the bottom screen interrupt and a request waits for the VBlank, answer it.
        if (id == gpu::Interrupt::PDC1 && !slot.vblank_waiters.empty()) {
            std::vector<Waiter> waiters;
            waiters.swap(slot.vblank_waiters);
            for (Waiter& waiter : waiters) {
                Reply reply(waiter.command_id, 1, 0);
                reply.Push(kResultSuccess);
                Complete(waiter.request, std::move(reply));
            }
        }
    } else {
        queue_interrupt();
    }
}

// ---- the GX command queue -----------------------------------------------------------------------------------

void GspState::ProcessCommandQueue() {
    if (thread_with_rights_ == kNoThread) {
        // Azahar answers success and then reads the command buffer of thread 0xFFFFFFFF, which is
        // outside its shared memory.
        return;
    }
    u8* const base = SharedPointer(kCommandBufferOffset + kCommandBufferSize * thread_with_rights_,
                                   kCommandBufferSize);
    if (base == nullptr) {
        return;
    }
    // The header word: index, number of commands, status, should_stop.
    auto load_header = [&]() {
        u32 header;
        std::memcpy(&header, base, sizeof(header));
        return header;
    };
    auto store_header = [&](u32 header) { std::memcpy(base, &header, sizeof(header)); };

    while (((load_header() >> 8) & 0xFF) != 0) {  // number_commands
        u32 header = load_header();
        const u32 status = (header >> 16) & 0xFF;
        if ((header >> 24) != 0) {  // should_stop
            header = (header & ~(0xFFu << 16)) | (kCommandBufferStatusStopped << 16);
            store_header(header);
            break;
        }
        if (status == kCommandBufferStatusStopped) {
            break;
        }

        const u32 index = header & 0xFF;
        if (index >= kCommandBufferCommands) {
            // Azahar reads the command from past the buffer.
            MH4N_LOG(Error, kLog, "GX command buffer of thread {} has index {}", thread_with_rights_, index);
            break;
        }
        gpu::GxCommand command;
        std::memcpy(command.words, base + 0x20 + 0x20 * index, sizeof(command.words));

        // Decrease the number of commands remaining and increase the current index.
        const u32 remaining = ((header >> 8) & 0xFF) - 1;
        header = (header & ~0xFFFFu) | (remaining << 8) | ((index + 1) % kCommandBufferCommands);
        store_header(header);

        // Decode and execute command.
        if (gpu::Gpu* gpu = GetGpu()) {
            gpu->Execute(command);
        }

        if (((command.words[0] >> 16) & 0xFF) != 0) {  // command.stop
            header = load_header();
            header = (header & ~(0xFFu << 16)) | (kCommandBufferStatusStopped << 16);
            store_header(header);
        }
    }
}

// ---- VBlank ----------------------------------------------------------------------------------------------------

void GspState::Start() {
    if (started_ || kernel_ == nullptr) {
        return;
    }
    started_ = true;
    // The first VBlank is FRAME_TICKS away whatever the refresh rate: Azahar's GPU constructor
    // schedules it so, and the patch's SetRefreshRate only changes the later reschedules.
    next_vblank_tick_ = kernel_->NowTicks() + kFrameTicks;
    ScheduleVBlank();
}

void GspState::ScheduleVBlank() {
    vblank_event_ = kernel_->ScheduleAt(next_vblank_tick_, [this] { OnVBlank(); });
}

void GspState::OnVBlank() {
    vblank_event_ = 0;
    ++vblank_count_;
    // Reschedule from the deadline of this event, not from now (GPU::VBlankCallback's
    // FRAME_TICKS - cycles_late), so a late callback shortens the next wait and the cadence does not
    // drift. The period in force now is the one for the next VBlank.
    next_vblank_tick_ += period_ticks_;
    ScheduleVBlank();

    // Signal to GSP that a GPU interrupt has occurred, then present.
    SignalInterrupt(gpu::Interrupt::PDC0);
    SignalInterrupt(gpu::Interrupt::PDC1);
    if (gpu::Gpu* gpu = GetGpu()) {
        gpu->VBlank(0);
        gpu->VBlank(1);
    }
}

void GspState::SetRefreshRate(double hz) {
    bool clamped = false;
    const u64 period = VBlankPeriodForRate(hz, &clamped);
    if (clamped) {
        MH4N_LOG(Warning, kLog, "refresh rate {} Hz gives a VBlank period outside the usable range; using {} ticks",
                 hz, period);
    }
    if (period == period_ticks_) {
        return;
    }
    period_ticks_ = period;
    MH4N_LOG(Info, kLog, "vblank period {} ticks", period);
}

// ---- the performance log ------------------------------------------------------------------------------------

void GspState::SetPerfLogEnabled(bool enabled) {
    perf_enabled_ = enabled;
    if (enabled) {
        perf_.fill(0);
    }
}

void GspState::PerfUpdate(gpu::Interrupt id, u64 nanoseconds) {
    // These counters may overflow, which is normal.
    const u32 index = static_cast<u32>(id);
    perf_[2 * index] = static_cast<u32>(nanoseconds);
    perf_[2 * index + 1] += static_cast<u32>(nanoseconds);
}

// ---- SaveVramSysArea and RestoreVramSysArea -----------------------------------------------------------------

void GspState::CopyFrameBuffer(u32 dst, u32 src, u32 dst_stride, u32 src_stride, u32 lines) {
    const u32 copy_bytes_per_line = std::min(src_stride, dst_stride);
    // The last row needs only the bytes that are copied.
    const u64 src_span = lines == 0 ? 0 : static_cast<u64>(lines - 1) * src_stride + copy_bytes_per_line;
    const u64 dst_span = lines == 0 ? 0 : static_cast<u64>(lines - 1) * dst_stride + copy_bytes_per_line;
    if (src_span > 0xFFFFFFFFu || dst_span > 0xFFFFFFFFu ||
        !memory_->IsMapped(src, static_cast<u32>(src_span)) ||
        !memory_->IsMapped(dst, static_cast<u32>(dst_span))) {
        MH4N_LOG(Warning, kLog, "Could not resolve pointers for framebuffer capture, skipping screen.");
        return;
    }
    gpu::Gpu* gpu = GetGpu();
    if (gpu != nullptr) {
        gpu->InvalidateRegion(src, src_stride * lines);  // RasterizerFlushVirtualRegion(Flush)
    }
    u8* dst_ptr = memory_->Base() + dst;
    const u8* src_ptr = memory_->Base() + src;
    for (u32 y = 0; y < lines; ++y) {
        std::memcpy(dst_ptr, src_ptr, copy_bytes_per_line);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
    if (gpu != nullptr) {
        gpu->FlushRegion(dst, dst_stride * lines);  // RasterizerFlushVirtualRegion(Invalidate)
    }
}

void GspState::ClearFramebuffer(u32 dst, u32 dst_stride, u32 lines) {
    const u64 span = static_cast<u64>(dst_stride) * lines;
    if (span > 0xFFFFFFFFu || !memory_->IsMapped(dst, static_cast<u32>(span))) {
        MH4N_LOG(Warning, kLog, "Could not resolve pointers for framebuffer clear, skipping screen.");
        return;
    }
    u8* dst_ptr = memory_->Base() + dst;
    for (u32 y = 0; y < lines; ++y) {
        std::memset(dst_ptr, 0, dst_stride);
        dst_ptr += dst_stride;
    }
    if (gpu::Gpu* gpu = GetGpu()) {
        gpu->FlushRegion(dst, dst_stride * lines);
    }
}

void GspState::SaveVramWithState(u32 state) {
    u32 mask = 0;
    if (state == 0 && vram_state_ == 0) {
        mask = kMask1;
        vram_state_ = 1;
    } else if (state == 1 && vram_state_ == 1) {
        mask = kMask2;
        vram_state_ = 2;
    }
    for (u32 bank = 0; bank < kVramBackupBanks; ++bank) {
        if ((kMaskPerBank[bank] & mask) == 0) {
            continue;
        }
        const u32 vaddr = kVramVaddr + bank * kVramBackupBankSize;
        if (!memory_->IsMapped(vaddr, kVramBackupBankSize)) {
            MH4N_LOG(Error, kLog, "VRAM bank {} is not mapped", bank);
            continue;
        }
        if (vram_backup_.empty()) {
            vram_backup_.resize(kVramBackupBanks);
        }
        if (gpu::Gpu* gpu = GetGpu()) {
            gpu->InvalidateRegion(vaddr, kVramBackupBankSize);
        }
        vram_backup_[bank].resize(kVramBackupBankSize);
        std::memcpy(vram_backup_[bank].data(), memory_->Base() + vaddr, kVramBackupBankSize);
    }
}

void GspState::RestoreVramWithState(u32 state) {
    u32 mask = 0;
    if (state == 0 && vram_state_ == 1) {
        mask = kMask1;
        vram_state_ = 0;
    } else if (state == 1 && vram_state_ == 2) {
        mask = kMask2;
        vram_state_ = 1;
    }
    for (u32 bank = 0; bank < kVramBackupBanks; ++bank) {
        if ((kMaskPerBank[bank] & mask) == 0) {
            continue;
        }
        const u32 vaddr = kVramVaddr + bank * kVramBackupBankSize;
        if (!memory_->IsMapped(vaddr, kVramBackupBankSize) || vram_backup_.empty() ||
            vram_backup_[bank].size() != kVramBackupBankSize) {
            MH4N_LOG(Error, kLog, "VRAM bank {} cannot be restored", bank);
            continue;
        }
        std::memcpy(memory_->Base() + vaddr, vram_backup_[bank].data(), kVramBackupBankSize);
        if (gpu::Gpu* gpu = GetGpu()) {
            gpu->FlushRegion(vaddr, kVramBackupBankSize);
        }
    }
}

void GspState::SaveVramSysArea() {
    // Taken from GSP decomp (Azahar's comment): the bytes per pixel of each framebuffer format; the
    // last three formats are invalid.
    static constexpr u8 bpp_per_format[] = {4, 3, 2, 2, 2, 0, 0, 0};

    SaveVramWithState(0);

    gpu::FrameBufferInfo& top = *GetFrameBufferInfo(thread_with_rights_, 0).second;
    {
        const u32 bytes_per_pixel = bpp_per_format[top.format & 7];
        if (top.address_left != 0 && bytes_per_pixel != 0 && bytes_per_pixel != 4) {
            CopyFrameBuffer(kFramebufferSaveAreaTopLeft, top.address_left,
                            kFramebufferWidth * bytes_per_pixel, top.stride, kTopFramebufferHeight);
        } else {
            MH4N_LOG(Debug, kLog, "Invalid framebuffer bound to top left screen, clearing...");
            ClearFramebuffer(kFramebufferSaveAreaTopLeft, kFramebufferWidth * bytes_per_pixel,
                             kTopFramebufferHeight);
        }
        if (top.address_right != 0 && bytes_per_pixel != 0 && bytes_per_pixel != 4) {
            CopyFrameBuffer(kFramebufferSaveAreaTopRight, top.address_right,
                            kFramebufferWidth * bytes_per_pixel, top.stride, kTopFramebufferHeight);
        } else {
            MH4N_LOG(Debug, kLog, "Invalid framebuffer bound to top right screen, clearing...");
            ClearFramebuffer(kFramebufferSaveAreaTopRight, kFramebufferWidth * bytes_per_pixel,
                             kTopFramebufferHeight);
        }
        top.address_left = kFramebufferSaveAreaTopLeft;
        top.address_right = kFramebufferSaveAreaTopRight;
        top.stride = kFramebufferWidth * bytes_per_pixel;
        force_buffer_swap_[0] = true;
    }

    gpu::FrameBufferInfo& bottom = *GetFrameBufferInfo(thread_with_rights_, 1).second;
    {
        const u32 bytes_per_pixel = bpp_per_format[bottom.format & 7];
        if (bottom.address_left != 0 && bytes_per_pixel != 0 && bytes_per_pixel != 4) {
            CopyFrameBuffer(kFramebufferSaveAreaBottom, bottom.address_left,
                            kFramebufferWidth * bytes_per_pixel, bottom.stride, kBottomFramebufferHeight);
        } else {
            MH4N_LOG(Debug, kLog, "Invalid framebuffer bound to bottom screen, clearing...");
            ClearFramebuffer(kFramebufferSaveAreaBottom, kFramebufferWidth * bytes_per_pixel,
                             kBottomFramebufferHeight);
        }
        bottom.address_left = kFramebufferSaveAreaBottom;
        bottom.stride = kFramebufferWidth * bytes_per_pixel;
        force_buffer_swap_[1] = true;
    }

    SaveVramWithState(1);
}

void GspState::RestoreVramSysArea() {
    RestoreVramWithState(1);

    GetFrameBufferInfo(thread_with_rights_, 0, true);
    force_buffer_swap_[0] = true;
    GetFrameBufferInfo(thread_with_rights_, 1, true);
    force_buffer_swap_[1] = true;

    RestoreVramWithState(0);
}

// ---- the boot step -------------------------------------------------------------------------------------------

namespace {

// The `services` step of this module: wires the runtime's two sinks to the state and starts the
// VBlank timer. It must run after the service manager is made ("services.ipc"), and the steps of a
// phase run in name order: hence the name.
void StartGsp(Runtime& rt) {
    ServiceManager* manager = FindServiceManager(rt);
    MH4N_ASSERT(manager != nullptr, "services.ipc.gsp runs before the service manager exists");
    std::shared_ptr<GspState> state = GspState::Get(manager->Context());
    std::weak_ptr<GspState> weak = state;
    rt.SetGpuInterruptSink([weak](gpu::Interrupt id) {
        if (auto locked = weak.lock()) {
            locked->SignalInterrupt(id);
        }
    });
    // A rate that is already known reaches the sink at once; it is the period of the VBlanks after
    // the first (see Start).
    rt.SetRefreshRateSink([weak](double hz) {
        if (auto locked = weak.lock()) {
            locked->SetRefreshRate(hz);
        }
    });
    state->Start();
}

MH4N_REGISTER(BootStep, "services.ipc.gsp", BootStep{Phase::Services, StartGsp});

}  // namespace

}  // namespace mh4n::gsp
