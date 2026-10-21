// Derived from Azahar src/core/hle/service/gsp/gsp_gpu.cpp, src/core/hle/service/gsp/gsp_gpu.h,
// src/core/hle/service/gsp/gsp_command.h and src/core/hle/service/gsp/gsp.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the gsp::Gpu commands (ids, parameters, replies and result codes), their checks and the
// order of the checks, the table of command names, the thread index each session gets and the rules
// for the GPU right.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (gsp_gpu.cpp, gsp_gpu.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
// Copyright 2017 Citra Emulator Project (gsp.cpp)
//
// The gsp::Gpu service (N1-17): register writes, the GX command queue, the interrupt relay queue,
// the framebuffer information and the GPU right, in front of gpu::Gpu. Each session has a GSP thread
// index (the lowest unused of four); the state they share is GspState (gsp.h).
//
// Departures from Azahar:
//  - A request Azahar cannot survive gets an answer: RegisterInterruptRelayQueue without an event
//    handle answers kResultInvalidHandle (Azahar asserts); a WriteHWRegs data buffer shorter than
//    its size answers kResultRegsInvalidSize and writes nothing (Azahar reads past the buffer);
//    AcquireRight while another thread has the right answers kResultBusy also when it is allowed to
//    block (Azahar asserts: "Sleeping for GPU right is not yet supported"); TriggerCmdReqQueue
//    without the right answers success and does nothing (Azahar reads the command buffer of thread
//    0xFFFFFFFF).
//  - A SaveVramSysArea or RestoreVramSysArea that waits for the VBlank may be one of several: all of
//    them are answered at the next PDC1 (Azahar keeps one event, so a second request leaves the
//    first thread asleep for ever).
//  - The delay_game_render_thread_us setting (0 for the reference) is not implemented:
//    TriggerCmdReqQueue always answers at once.
//  - GetPerfLog: the times of the interrupts other than the VBlanks are 0 (Azahar records the delay
//    the GPU passes; D25 has the sink ignore it).
#include <algorithm>
#include <cstring>

#include "gsp.h"
#include "mh4n/loader.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"

namespace mh4n::gsp {

namespace {

constexpr const char* kLog = "gsp";

// The address check WriteHWRegs, WriteHWRegsWithMask and ReadHWRegs share.
bool BadRegisterAddress(u32 address) {
    return (address & 3) != 0 || address >= kRegsLimit;
}

class GspGpu final : public Service {
public:
    explicit GspGpu(ServiceContext& context) : Service(context, "gsp::Gpu") {
        state_ = GspState::Get(context);
        thread_id_ = state_->ClaimThread();

        Register(0x0001, "WriteHWRegs", &GspGpu::WriteHWRegs);
        Register(0x0002, "WriteHWRegsWithMask", &GspGpu::WriteHWRegsWithMask);
        Register(0x0003, "WriteHWRegRepeat");
        Register(0x0004, "ReadHWRegs", &GspGpu::ReadHWRegs);
        Register(0x0005, "SetBufferSwap", &GspGpu::SetBufferSwap);
        Register(0x0006, "SetCommandList");
        Register(0x0007, "RequestDma");
        Register(0x0008, "FlushDataCache", &GspGpu::FlushDataCache);
        Register(0x0009, "InvalidateDataCache", &GspGpu::InvalidateDataCache);
        Register(0x000A, "RegisterInterruptEvents");
        Register(0x000B, "SetLcdForceBlack", &GspGpu::SetLcdForceBlack);
        Register(0x000C, "TriggerCmdReqQueue", &GspGpu::TriggerCmdReqQueue);
        Register(0x000D, "SetDisplayTransfer");
        Register(0x000E, "SetTextureCopy");
        Register(0x000F, "SetMemoryFill");
        Register(0x0010, "SetAxiConfigQoSMode", &GspGpu::SetAxiConfigQoSMode);
        Register(0x0011, "SetPerfLogMode", &GspGpu::SetPerfLogMode);
        Register(0x0012, "GetPerfLog", &GspGpu::GetPerfLog);
        Register(0x0013, "RegisterInterruptRelayQueue", &GspGpu::RegisterInterruptRelayQueue);
        Register(0x0014, "UnregisterInterruptRelayQueue", &GspGpu::UnregisterInterruptRelayQueue);
        Register(0x0015, "TryAcquireRight", &GspGpu::TryAcquireRight);
        Register(0x0016, "AcquireRight", &GspGpu::AcquireRight);
        Register(0x0017, "ReleaseRight", &GspGpu::ReleaseRight);
        Register(0x0018, "ImportDisplayCaptureInfo", &GspGpu::ImportDisplayCaptureInfo);
        Register(0x0019, "SaveVramSysArea", &GspGpu::SaveVramSysArea);
        Register(0x001A, "RestoreVramSysArea", &GspGpu::RestoreVramSysArea);
        Register(0x001B, "ResetGpuCore");
        Register(0x001C, "SetLedForceOff", &GspGpu::SetLedForceOff);
        Register(0x001D, "SetTestCommand");
        Register(0x001E, "SetInternalPriorities", &GspGpu::SetInternalPriorities);
        Register(0x001F, "StoreDataCache", &GspGpu::StoreDataCache);
    }

    ~GspGpu() override { state_->ReleaseThread(thread_id_); }

private:
    GspState::ThreadSlot* Slot() {
        return thread_id_ < kMaxThreads ? &state_->Slot(thread_id_) : nullptr;
    }

    // Inputs:  0: 0x00010082, 1: the register offset, 2: the size in bytes, 3: the static buffer
    //          descriptor, 4: the buffer
    // Outputs: 0: 0x00010040, 1: result
    void WriteHWRegs(Request& request) {
        const u32 base_address = request.Pop<u32>();
        const u32 size_in_bytes = request.Pop<u32>();
        const StaticBuffer* data = request.PopStaticBuffer();
        request.MakeReply(1, 0).Push(Write(base_address, size_in_bytes, data, nullptr));
    }

    // Inputs:  0: 0x00020084, 1: the register offset, 2: the size in bytes, 3-4: the data buffer,
    //          5-6: the mask buffer
    // Outputs: 0: 0x00020040, 1: result
    void WriteHWRegsWithMask(Request& request) {
        const u32 base_address = request.Pop<u32>();
        const u32 size_in_bytes = request.Pop<u32>();
        const StaticBuffer* data = request.PopStaticBuffer();
        const StaticBuffer* masks = request.PopStaticBuffer();
        request.MakeReply(1, 0).Push(Write(base_address, size_in_bytes, data, masks, true));
    }

    // WriteHWRegs and WriteHWRegsWithMask of gsp_gpu.cpp: the same three checks in the same order.
    // With `masked`, each register takes the bits of the mask only.
    Result Write(u32 base_address, u32 size_in_bytes, const StaticBuffer* data,
                 const StaticBuffer* masks, bool masked = false) {
        if (BadRegisterAddress(base_address)) {
            MH4N_LOG(Error, kLog,
                     "Write address was out of range or misaligned! (address=0x{:08x}, size=0x{:08x})",
                     base_address, size_in_bytes);
            return kResultRegsOutOfRangeOrMisaligned;
        }
        if (size_in_bytes > kMaxRegBytes) {
            MH4N_LOG(Error, kLog, "Out of range size 0x{:08x}", size_in_bytes);
            return kResultRegsInvalidSize;
        }
        if ((size_in_bytes & 3) != 0) {
            MH4N_LOG(Error, kLog, "Misaligned size 0x{:08x}", size_in_bytes);
            return kResultRegsMisaligned;
        }
        if (data == nullptr || data->data.size() < size_in_bytes ||
            (masked && (masks == nullptr || masks->data.size() < size_in_bytes))) {
            MH4N_LOG(Error, kLog, "The buffers of a register write are shorter than 0x{:08x} bytes",
                     size_in_bytes);
            return kResultRegsInvalidSize;
        }
        gpu::Gpu* gpu = state_->GetGpu();
        for (u32 offset = 0; offset < size_in_bytes; offset += 4) {
            const u32 address = kRegsBegin + base_address + offset;
            u32 value;
            std::memcpy(&value, &data->data[offset], sizeof(value));
            if (masked) {
                u32 mask;
                std::memcpy(&mask, &masks->data[offset], sizeof(mask));
                // Update the current value of the register only for set mask bits.
                const u32 current = gpu != nullptr ? gpu->ReadReg(address) : 0;
                value = (current & ~mask) | (value & mask);
            }
            if (gpu != nullptr) {
                gpu->WriteReg(address, value);
            }
        }
        return kResultSuccess;
    }

    // Inputs:  0: 0x00040080, 1: the register offset, 2: the size in bytes
    // Outputs: 0: 0x00040042, 1: result, 2: the static buffer descriptor, 3: the address
    void ReadHWRegs(Request& request) {
        const u32 reg_addr = request.Pop<u32>();
        const u32 input_size = request.Pop<u32>();

        constexpr u32 kMaxReadSize = 0x80;
        const u32 size = std::min(input_size, kMaxReadSize);

        if (BadRegisterAddress(reg_addr)) {
            request.MakeReply(1, 0).Push(kResultRegsOutOfRangeOrMisaligned);
            MH4N_LOG(Error, kLog, "Invalid address 0x{:08x}", reg_addr);
            return;
        }
        // Size should be word-aligned.
        if ((size % 4) != 0) {
            request.MakeReply(1, 0).Push(kResultRegsMisaligned);
            MH4N_LOG(Error, kLog, "Invalid size 0x{:08x}", size);
            return;
        }

        std::vector<u8> buffer(size);
        gpu::Gpu* gpu = state_->GetGpu();
        for (u32 word = 0; word < size / sizeof(u32); ++word) {
            const u32 data = gpu != nullptr ? gpu->ReadReg(kRegsBegin + reg_addr + word * 4) : 0;
            std::memcpy(buffer.data() + word * sizeof(u32), &data, sizeof(u32));
        }

        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(std::move(buffer), 0);
    }

    // Inputs:  0: 0x00050200, 1: the screen (0 top, 1 bottom), 2-8: a FrameBufferInfo
    // Outputs: 0: 0x00050040, 1: result
    void SetBufferSwap(Request& request) {
        const u32 screen_id = request.Pop<u32>();
        const auto fb_info = request.PopRaw<gpu::FrameBufferInfo>();
        if (gpu::Gpu* gpu = state_->GetGpu()) {
            gpu->SetBufferSwap(screen_id, fb_info);
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // The three cache commands share their shape: 0: header, 1: the address, 2: the size, 3: the
    // handle descriptor (0), 4: the process handle. Azahar stubs them; the GPU is told what the
    // CPU wrote (Flush, Store) or is about to read (Invalidate).
    void FlushDataCache(Request& request) {
        const u32 address = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        MH4N_LOG(Trace, kLog, "FlushDataCache address=0x{:08X}, size=0x{:08X}", address, size);
        if (gpu::Gpu* gpu = state_->GetGpu()) {
            gpu->FlushRegion(address, size);
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void InvalidateDataCache(Request& request) {
        const u32 address = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        MH4N_LOG(Trace, kLog, "InvalidateDataCache address=0x{:08X}, size=0x{:08X}", address, size);
        if (gpu::Gpu* gpu = state_->GetGpu()) {
            gpu->InvalidateRegion(address, size);
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void StoreDataCache(Request& request) {
        const u32 address = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        MH4N_LOG(Trace, kLog, "StoreDataCache address=0x{:08X}, size=0x{:08X}", address, size);
        if (gpu::Gpu* gpu = state_->GetGpu()) {
            gpu->FlushRegion(address, size);
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Inputs:  0: 0x00100040, 1: the mode (unused)
    // Outputs: 0: 0x00100040, 1: result
    void SetAxiConfigQoSMode(Request& request) {
        const u32 mode = request.Pop<u32>();
        MH4N_LOG(Debug, kLog, "(STUBBED) SetAxiConfigQoSMode mode=0x{:08X}", mode);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void SetPerfLogMode(Request& request) {
        state_->SetPerfLogEnabled(request.Pop<u32>() != 0);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Outputs: 0: 0x001203C0, 1: result, 2-15: delta and sum for each of the seven interrupts
    void GetPerfLog(Request& request) {
        Reply& reply = request.MakeReply(15, 0);
        reply.Push(kResultSuccess);
        for (const u32 word : state_->PerfLog()) {
            reply.Push(word);
        }
    }

    // Inputs:  0: 0x00130042, 1: the flags (bit 0: AllowSaveVramSysArea), 2: the handle descriptor
    //          (0), 3: the interrupt event
    // Outputs: 0: 0x00130082, 1: result (0x2A07 the first time), 2: the thread index, 3: the
    //          handle descriptor, 4: the shared memory
    void RegisterInterruptRelayQueue(Request& request) {
        const u32 relay_queue_flags = request.Pop<u32>();
        std::shared_ptr<Event> interrupt_event = request.PopObject<Event>();
        GspState::ThreadSlot* slot = Slot();
        if (interrupt_event == nullptr || slot == nullptr || !state_->SharedMem()) {
            MH4N_LOG(Error, kLog, "RegisterInterruptRelayQueue: no event, no thread index or no memory");
            request.MakeReply(1, 0).Push(interrupt_event == nullptr ? kResultInvalidHandle
                                                                    : kResultOutOfMemory);
            return;
        }
        interrupt_event->SetName("GSP_GPU::interrupt_event");
        slot->interrupt_event = std::move(interrupt_event);
        slot->registered = true;
        slot->flags = relay_queue_flags;

        Reply& reply = request.MakeReply(2, 2);
        if (state_->TakeFirstInitialization()) {
            // This specific code is required for a successful initialization, rather than 0.
            reply.Push(kResultFirstInitialization);
        } else {
            reply.Push(kResultSuccess);
        }
        reply.Push(thread_id_);
        reply.PushCopyObjects(state_->SharedMem());
        MH4N_LOG(Debug, kLog, "RegisterInterruptRelayQueue relay_queue_flags=0x{:08X} thread={}",
                 relay_queue_flags, thread_id_);
    }

    // Outputs: 0: 0x00140040, 1: result
    void UnregisterInterruptRelayQueue(Request& request) {
        if (GspState::ThreadSlot* slot = Slot()) {
            slot->interrupt_event = nullptr;
            slot->registered = false;
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // AcquireGpuRight of gsp_gpu.cpp (without the per-title settings of the renderer).
    Result AcquireGpuRight(const Request& request, bool blocking) {
        if (state_->ThreadWithRights() == thread_id_) {
            return kResultAlreadyDone;
        }
        if (state_->ThreadWithRights() != kNoThread) {
            if (blocking) {
                // Azahar: "Sleeping for GPU right is not yet supported."
                MH4N_LOG(Error, kLog, "AcquireRight while thread {} has the right: not waiting",
                         state_->ThreadWithRights());
            }
            return kResultBusy;
        }
        state_->SetThreadWithRights(thread_id_, request.ClientThreadId());
        return kResultSuccess;
    }

    // Inputs:  0: 0x00150002, 1: the handle descriptor (0), 2: the process handle
    // Outputs: 0: 0x00150040, 1: result
    void TryAcquireRight(Request& request) {
        const Result result = AcquireGpuRight(request, false);
        request.MakeReply(1, 0).Push(result);
    }

    // Inputs:  0: 0x00160042, 1: the flags, 2: the handle descriptor (0), 3: the process handle
    // Outputs: 0: 0x00160040, 1: result
    void AcquireRight(Request& request) {
        const u32 flag = request.Pop<u32>();
        MH4N_LOG(Debug, kLog, "AcquireRight flag={:08X} thread_id={}", flag, thread_id_);
        const Result result = AcquireGpuRight(request, true);
        request.MakeReply(1, 0).Push(result);
    }

    // Outputs: 0: 0x00170040, 1: result. Success even if the wrong thread calls it.
    void ReleaseRight(Request& request) {
        state_->ReleaseRight(thread_id_);
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "ReleaseRight called");
    }

    // Inputs:  0: 0x000B0040, 1: the black fill flag (0 = don't fill)
    // Outputs: 0: 0x000B0040, 1: result
    void SetLcdForceBlack(Request& request) {
        const bool enable_black = request.Pop<bool>();
        // Pica::ColorFill with only is_enabled (bit 24) assigned.
        if (gpu::Gpu* gpu = state_->GetGpu()) {
            gpu->SetColorFill(enable_black ? (1u << 24) : 0u);
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Inputs:  0: 0x000C0000
    // Outputs: 0: 0x000C0040, 1: result. The commands in the command buffer of the thread with the
    // GPU right are run, in order, until the buffer is empty or stopped.
    void TriggerCmdReqQueue(Request& request) {
        state_->ProcessCommandQueue();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Outputs: 0: 0x00180240, 1: result, 2-5: the top screen's left address, right address, format
    // and stride, 6-9: the bottom screen's
    void ImportDisplayCaptureInfo(Request& request) {
        const auto top = state_->GetFrameBufferInfo(state_->ThreadWithRights(), 0);
        const auto bottom = state_->GetFrameBufferInfo(state_->ThreadWithRights(), 1);
        if (top.first) {
            state_->ForceBufferSwap(0);
        }
        if (bottom.first) {
            state_->ForceBufferSwap(1);
        }
        Reply& reply = request.MakeReply(9, 0);
        reply.Push(kResultSuccess);
        for (const gpu::FrameBufferInfo* info : {top.second, bottom.second}) {
            reply.Push(info->address_left);
            reply.Push(info->address_right);
            reply.Push(info->format);
            reply.Push(info->stride);
        }
    }

    // Outputs: 0: 0x00190040, 1: result, after the next VBlank of the bottom screen
    Handled SaveVramSysArea(Request& request) {
        GspState::ThreadSlot* slot = Slot();
        if (slot == nullptr || (slot->flags & kAllowSaveVramSysArea) == 0) {
            request.MakeReply(1, 0).Push(kResultNoSaveVramSysAreaPerm);
            return Handled::Done;
        }
        state_->SaveVramSysArea();
        // Wait for VBlank.
        slot->vblank_waiters.push_back({request.Share(), request.CommandId()});
        return Handled::Deferred;
    }

    // Outputs: 0: 0x001A0040, 1: result, after the next VBlank of the bottom screen
    Handled RestoreVramSysArea(Request& request) {
        GspState::ThreadSlot* slot = Slot();
        if (slot == nullptr || (slot->flags & kAllowSaveVramSysArea) == 0) {
            request.MakeReply(1, 0).Push(kResultNoSaveVramSysAreaPerm);
            return Handled::Done;
        }
        state_->RestoreVramSysArea();
        slot->vblank_waiters.push_back({request.Share(), request.CommandId()});
        return Handled::Deferred;
    }

    // Inputs:  0: 0x001C0040, 1: the state (0 = on, otherwise off)
    // Outputs: 0: 0x001C0040, 1: result
    void SetLedForceOff(Request& request) {
        const u8 state = request.Pop<u8>();
        // GetSharedPageHandler().Set3DLed(state)
        state_->Memory().Write<u8>(kSharedPageVaddr + shared_page::kLedState3d, state);
        MH4N_LOG(Debug, kLog, "(STUBBED) SetLedForceOff");
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Inputs:  0: 0x001E0080, 1: the session thread's priority, 2: the priority with the right
    // Outputs: 0: 0x001E0040, 1: result
    void SetInternalPriorities(Request& request) {
        const u32 priority = request.Pop<u32>();
        const u32 priority_with_rights = request.Pop<u32>();
        MH4N_LOG(Debug, kLog, "(STUBBED) SetInternalPriorities priority={:#04X}, priority_with_rights={:#04X}",
                 priority, priority_with_rights);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    std::shared_ptr<GspState> state_;
    u32 thread_id_ = kNoThread;
};

// gsp::Gpu has four sessions (ServiceFramework("gsp::Gpu", 4)).
MH4N_REGISTER(ServiceFactory, "gsp::Gpu",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<GspGpu>(context); },
                             kMaxThreads));

}  // namespace

}  // namespace mh4n::gsp
