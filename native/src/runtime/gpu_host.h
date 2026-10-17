// The runtime's side of the GPU contract (mh4n/gpu.h): what a GPU gets from guest memory, the
// kernel and the game clock. Private to mh4n_runtime; Runtime::GpuHost() hands it out as a
// gpu::Host.
#pragma once

#include <functional>
#include <optional>

#include "mh4n/gpu.h"
#include "mh4n/kernel.h"
#include "mh4n/memory.h"

namespace mh4n::detail {

class RuntimeGpuHost final : public gpu::Host {
public:
    RuntimeGpuHost(GuestMemory& memory, Kernel& kernel) : memory_(memory), kernel_(kernel) {}

    // GuestMemory::PhysicalPointer, plus the one rule gpu.h adds: size 0 accepts the address one
    // past the end of an area (VRAM, DSP RAM, FCRAM), mapped or not, because it is an address and
    // not a byte (Azahar's IsValidPhysicalAddress; the GX engine cuts fills, transfers and DMA at
    // the longest valid prefix). FCRAM is the linear heap, so it has no physical view until the
    // loader has set the heap's base (GuestMemory::SetLinearHeapArea).
    u8* PhysicalPointer(u32 paddr, u32 size) override;
    std::optional<u32> VirtualToPhysical(u32 vaddr) override { return memory_.VirtualToPhysical(vaddr); }
    // Any thread: the sink is called on the scheduler thread after `delay_ns` of game time.
    void SignalInterrupt(gpu::Interrupt id, u64 delay_ns) override;
    // Any thread: `work` runs on the scheduler thread at its next safe point.
    void Post(std::function<void()> work) override { kernel_.Post(std::move(work)); }

    // Scheduler thread only (or before the run starts).
    void SetInterruptSink(std::function<void(gpu::Interrupt)> sink) { sink_ = std::move(sink); }

private:
    GuestMemory& memory_;
    Kernel& kernel_;
    std::function<void(gpu::Interrupt)> sink_;
};

}  // namespace mh4n::detail
