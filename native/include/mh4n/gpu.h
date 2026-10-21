// Derived from Azahar src/video_core/gpu.h and src/core/hle/service/gsp/gsp_gpu.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the shape of the interface and the
// FrameBufferInfo layout (members, order and names, as GSP shared memory holds them).
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (gsp_gpu.h)
//
// The GPU contract (D20, N1-01). The runtime and every renderer meet here; it mirrors the entry
// points of Azahar's VideoCore::GPU, which N0-05's captures record. Implemented by the null GPU
// (N1-17), the capture player's GPUs (N2-02) and the renderer (N2).
#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/types.h"

namespace mh4n::gpu {

struct GxCommand {
    u32 words[8]; // as in GSP shared memory; the id is in the low byte of word 0
};

struct FrameBufferInfo {
    u32 active_fb, address_left, address_right, stride, format, shown_fb, unknown;
};

enum class Interrupt : u32 { PSC0 = 0, PSC1 = 1, PDC0 = 2, PDC1 = 3, PPF = 4, P3D = 5, DMA = 6 };

// What a GPU needs from the runtime.
class Host {
public:
    virtual ~Host() = default;
    // FCRAM, VRAM or DSP RAM, else nullptr. Non-null only when the whole [paddr, paddr + size) lies
    // inside one of those areas, mapped; a range that leaves its area, or is partly unmapped, is
    // nullptr (Azahar's GetPhysicalPointer ignores the size; the GX engine relies on this stricter
    // rule to cut a DMA, fill or transfer at the end of an area). Size 0 probes the address: non-null
    // for an address inside an area and for the address one past its end.
    virtual u8* PhysicalPointer(u32 paddr, u32 size) = 0;
    virtual std::optional<u32> VirtualToPhysical(u32 vaddr) = 0;
    // A pointer to guest memory for [vaddr, vaddr + size) when the whole range is mapped, else
    // nullptr (size 0 is an empty range, which is mapped). `vaddr` is a guest virtual address of
    // the application's memory map, not a physical one: Azahar's DMA goes through that map
    // (MemorySystem::CopyBlock), so it can copy between ranges that have no physical view, such as
    // the application's heap. Scheduler thread only, as every access to guest memory (principle 6).
    // The default is nullptr, a host with no guest memory of its own (a capture's) serves none.
    virtual u8* GuestPointer(u32 vaddr, u32 size) {
        (void)vaddr;
        (void)size;
        return nullptr;
    }
    // Thread-safe; goes through Kernel::Post. The interrupt reaches the runtime's sink at once, on
    // the scheduler thread, whatever `delay_ns` is (D25, D41): the delay is the time the work would
    // take on a console, which Azahar's GSP only records (GetPerfLog), with the reference's
    // settings. A host that wants it while the sink runs asks InterruptDelayNs.
    virtual void SignalInterrupt(Interrupt id, u64 delay_ns) = 0;
    // While the runtime's interrupt sink is being called: the `delay_ns` the GPU passed for that
    // interrupt. 0 at any other time and for a host that does not keep it. Scheduler thread only.
    virtual u64 InterruptDelayNs() const { return 0; }
    // Thread-safe; runs on the scheduler thread at a safe point (a capture's host: at once).
    virtual void Post(std::function<void()> work) = 0;
};

class Gpu {
public:
    virtual ~Gpu() = default;
    // Virtual addresses, as the game wrote them.
    virtual void Execute(const GxCommand& command) = 0;
    // GSP WriteHWRegs: 0x1EB00000 + the register offset, i.e. the virtual addresses LCD registers at
    // 0x1ED02000 and GPU registers at 0x1EF00000 and 0x1EF01000 (docs/native/mgpu-format.md).
    virtual void WriteReg(u32 addr, u32 value) = 0;
    virtual u32 ReadReg(u32 addr) = 0;
    virtual void SetBufferSwap(u32 screen, const FrameBufferInfo& info) = 0;
    // The raw ColorFill register word, both screens.
    virtual void SetColorFill(u32 fill) = 0;
    // The CPU wrote this range (FlushDataCache).
    virtual void FlushRegion(u32 vaddr, u32 size) = 0;
    // The CPU is about to read this range.
    virtual void InvalidateRegion(u32 vaddr, u32 size) = 0;
    // A refresh of `screen` finished.
    virtual void VBlank(u32 screen) = 0;
};

// Registered by name: MH4N_REGISTER(gpu::GpuFactory, "null", ...).
using GpuFactory = std::function<std::unique_ptr<Gpu>(Host&, const Options&)>;

} // namespace mh4n::gpu
