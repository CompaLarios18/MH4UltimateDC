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
    // FCRAM, VRAM or DSP RAM, else nullptr.
    virtual u8* PhysicalPointer(u32 paddr, u32 size) = 0;
    virtual std::optional<u32> VirtualToPhysical(u32 vaddr) = 0;
    // Thread-safe; goes through Kernel::Post.
    virtual void SignalInterrupt(Interrupt id, u64 delay_ns) = 0;
    // Thread-safe; runs on the scheduler thread at a safe point (a capture's host: at once).
    virtual void Post(std::function<void()> work) = 0;
};

class Gpu {
public:
    virtual ~Gpu() = default;
    // Virtual addresses, as the game wrote them.
    virtual void Execute(const GxCommand& command) = 0;
    // GSP WriteHWRegs: 0x1EB00000 + the register offset.
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
