#include "gpu_host.h"

#include "mh4n/clock.h"

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

void RuntimeGpuHost::SignalInterrupt(gpu::Interrupt id, u64 delay_ns) {
    kernel_.Post([this, id, delay_ns] {
        if (delay_ns == 0) {
            if (sink_) {
                sink_(id);
            }
            return;
        }
        kernel_.ScheduleAt(kernel_.NowTicks() + NsToTicks(delay_ns), [this, id] {
            if (sink_) {
                sink_(id);
            }
        });
    });
}

}  // namespace mh4n::detail
