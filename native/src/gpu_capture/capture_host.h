// Derived from Azahar src/core/memory.h (the VRAM, FCRAM and DSP RAM physical address constants)
// and src/video_core/gpu.cpp (the windows of GPU::VirtualToPhysicalAddress) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later); see native/THIRD_PARTY.md.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp)
//
// The gpu::Host of a capture replay (N2-02): a physical memory image of FCRAM, VRAM and DSP RAM
// built from MEM records, and a second copy that is the capture's own reference (what the memory
// holds when every GPU_PAGES record is applied).
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "gpu_capture/mgpu.h"
#include "mh4n/gpu.h"

namespace mh4n::gpu_capture {

inline constexpr u32 kFcramPaddr = 0x20000000;
inline constexpr u32 kFcramSize = 0x10000000; // room for the New 3DS; untouched pages cost nothing
inline constexpr u32 kVramPaddr = 0x18000000;
inline constexpr u32 kVramSize = 0x00600000;
inline constexpr u32 kDspRamPaddr = 0x1FF00000;
inline constexpr u32 kDspRamSize = 0x00080000;

// What the capture says the current event produced. The player fills it from the records that
// follow the event (PICA_REG, GPU_PAGES, IRQ) before it calls the Gpu, so that the `replay` Gpu
// can write them. Valid only while that event runs.
struct EventReference {
    std::vector<PageData> pages;                  // GPU_PAGES, in file order
    std::vector<gpu::RegisterWrite> registers;    // PICA_REG
    std::vector<IrqData> interrupts;              // IRQ records that are not VBlanks
};

struct SignalledInterrupt {
    gpu::Interrupt id;
    u64 delay_ns;
};

class CaptureHost final : public gpu::Host {
public:
    CaptureHost();
    ~CaptureHost() override;

    // ---- gpu::Host: the Gpu's memory ------------------------------------------------------------
    u8* PhysicalPointer(u32 paddr, u32 size) override;
    // Azahar's GPU::VirtualToPhysicalAddress: the VRAM window, the linear heap (below and from
    // kernel version 0x22C) and addresses that already are physical. nullopt for 0 and for an
    // address that is in none of them.
    std::optional<u32> VirtualToPhysical(u32 vaddr) override;
    void SignalInterrupt(gpu::Interrupt id, u64 delay_ns) override;
    // The capture has no scheduler: the work runs at once, on the calling thread.
    void Post(std::function<void()> work) override;

    // ---- the reference memory --------------------------------------------------------------------
    // The capture's own copy: MEM, CPU_PAGES and GPU_PAGES applied in file order. Never written
    // by a Gpu.
    const u8* ReferencePointer(u32 paddr, u32 size) const;

    // MEM and CPU_PAGES go to both copies (the Gpu's memory is the reference plus what it has
    // written since). False if the range is not in FCRAM, VRAM or DSP RAM.
    bool WriteBoth(u32 paddr, const u8* data, u32 size);
    // GPU_PAGES go into the reference copy as soon as the Gpu has run the command, and into the
    // Gpu's memory only after the command is judged (isolated mode, and commands that are not
    // judged).
    bool ApplyReferencePage(const PageData& page);
    bool ApplyPageToGpu(const PageData& page);

    // ---- interrupts the Gpu signalled ---------------------------------------------------------------
    std::vector<SignalledInterrupt> TakeSignalled();

    // ---- the current event's reference ---------------------------------------------------------------
    // For Gpus that replay a capture: null outside an event.
    const EventReference* CurrentEvent() const {
        return event_;
    }
    void SetCurrentEvent(const EventReference* event) {
        event_ = event;
    }

private:
    struct Area;
    struct Memory;
    std::unique_ptr<Memory> gpu_memory_;
    std::unique_ptr<Memory> reference_memory_;
    std::mutex signal_mutex_;
    std::vector<SignalledInterrupt> signalled_;
    const EventReference* event_ = nullptr;
};

} // namespace mh4n::gpu_capture
