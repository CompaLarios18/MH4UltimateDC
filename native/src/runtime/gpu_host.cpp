#include "gpu_host.h"

namespace mh4n::detail {

u8* RuntimeGpuHost::PhysicalPointer(u32 paddr, u32 size) {
    if (u8* pointer = memory_.PhysicalPointer(paddr, size)) {
        return pointer;
    }
    if (size != 0) {
        return nullptr;
    }
    // The end of an area: the host address one past its last byte, never dereferenced.
    u8* const base = memory_.Base();
    if (paddr == kVramPaddr + kVramSize) {
        return base + kVramVaddr + kVramSize;
    }
    if (paddr == kDspRamPaddr + kDspRamSize) {
        return base + kDspRamVaddr + kDspRamSize;
    }
    if (paddr == kFcramPaddr + kFcramSize && memory_.LinearHeapArea() != 0) {
        return base + memory_.LinearHeapArea() + kFcramSize;
    }
    return nullptr;
}

u8* RuntimeGpuHost::GuestPointer(u32 vaddr, u32 size) {
    // Flat memory: guest address A is Base() + A. Only the managed range [0, kManagedEnd) is ever
    // mapped; an empty range at or below its end is an address, as in PhysicalPointer.
    if (static_cast<u64>(vaddr) + size > kManagedEnd || !memory_.IsMapped(vaddr, size)) {
        return nullptr;
    }
    return memory_.Base() + vaddr;
}

void RuntimeGpuHost::SignalInterrupt(gpu::Interrupt id, u64 delay_ns) {
    // At once, on the scheduler thread: Kernel::Post runs the call at the next safe point, in the
    // order the interrupts were signalled, and from the thread that signals it, directly. The delay
    // is not waited for (D25, D41); it is only handed to the sink's reader, which records it.
    kernel_.Post([this, id, delay_ns] {
        if (!sink_) {
            return;
        }
        const u64 outer = delivering_delay_ns_;  // a sink that signals again
        delivering_delay_ns_ = delay_ns;
        sink_(id);
        delivering_delay_ns_ = outer;
    });
}

}  // namespace mh4n::detail
