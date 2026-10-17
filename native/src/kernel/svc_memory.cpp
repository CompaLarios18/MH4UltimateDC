// Derived from Azahar src/core/hle/kernel/svc.cpp, process.cpp, vm_manager.cpp and
// src/core/memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp says
// so; process.cpp, vm_manager.cpp and memory.h say GPL-2.0-or-later): SVC::ControlMemory,
// QueryProcessMemory, CreateMemoryBlock, MapMemoryBlock and UnmapMemoryBlock; Process::HeapAllocate,
// HeapFree, LinearAllocate, LinearFree, Map and Unmap, VMManager::GetBackingBlocksForRange, and the
// address ranges they check.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp, memory.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp)
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (vm_manager.cpp)
//
// The memory calls of the SVC table (N1-07): ControlMemory's heap and linear heap on GuestMemory
// and FcramRegions, QueryMemory and the shared memory blocks. See mh4n/svc.h and svc.cpp.
//
// Departures from Azahar:
//  - MAP and UNMAP of ControlMemory alias guest pages, which flat memory cannot do: the arguments
//    are checked as Azahar checks them, then the call is logged and answered with
//    kResultInvalidAddressState, as SharedMemory::Map answers a second mapping.
//  - Azahar asserts when a linear allocation of size 0 finds nothing to map; here the call fails
//    with kResultInvalidAddress and gives the range back.
//  - FREE of memory that the loader mapped (the program's own segments and stack) unmaps it, but
//    its FCRAM is not given back: only the pieces ControlMemory allocated are tracked.
#include <algorithm>

#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "svc_internal.h"

// The registers of a guest thread (RCtx). Last: recomp.h defines short macros.
#include "../cpu/recomp_c.h"

namespace mh4n {

namespace {

// ControlMemoryOperation (svc.cpp).
inline constexpr u32 kMemopFree = 1;
inline constexpr u32 kMemopCommit = 3;
inline constexpr u32 kMemopMap = 4;
inline constexpr u32 kMemopUnmap = 5;
inline constexpr u32 kMemopProtect = 6;
inline constexpr u32 kMemopOperationMask = 0xFF;
inline constexpr u32 kMemopRegionMask = 0xF00;
inline constexpr u32 kMemopLinear = 0x10000;

// Azahar's Memory::SHARED_MEMORY_VADDR_END.
inline constexpr u32 kSharedMemoryVaddrEnd = kSharedMemoryVaddr + 0x04000000;

void Return(RCtx& c, Result result) {
    c.r[0] = result;
}

}  // namespace

u32 SvcTable::Impl::LinearHeapArea() const {
    // Process::GetLinearHeapAreaAddress: the kernel version decides.
    return LinearHeapAreaForKernelVersion(kernel.CurrentProcess()->kernel_version);
}

// ---- ControlMemory ---------------------------------------------------------------------------------

void SvcTable::Impl::ControlMemory(SvcFrame& f) {
    // r0 operation, r1 addr0, r2 addr1, r3 size, r4 permissions; out r1 the address.
    RCtx& c = f.ctx;
    u32 out_addr = 0;
    const Result result = ControlMemoryOp(c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], &out_addr);
    c.r[0] = result;
    c.r[1] = IsError(result) ? 0 : out_addr;
}

Result SvcTable::Impl::ControlMemoryOp(u32 operation, u32 addr0, u32 addr1, u32 size,
                                       u32 permissions, u32* out_addr) {
    if ((addr0 & kPageMask) != 0 || (addr1 & kPageMask) != 0) {
        return kResultMisalignedAddress;
    }
    if ((size & kPageMask) != 0) {
        return kResultMisalignedSize;
    }
    const u32 region = operation & kMemopRegionMask;
    operation &= ~kMemopRegionMask;
    if (region != 0) {
        MH4N_LOG(Warning, "Kernel.SVC", "ControlMemory with specified region not supported, region={:X}",
                 region);
    }
    if ((permissions & static_cast<u32>(MemPerm::ReadWrite)) != permissions) {
        return kResultInvalidCombination;
    }
    const auto perm = static_cast<MemPerm>(permissions);
    const Process& process = *kernel.CurrentProcess();

    switch (operation & kMemopOperationMask) {
    case kMemopFree: {
        // TODO(Subv) in Azahar: what if a SharedMemory points to the block?
        const u32 linear_base = LinearHeapArea() + kernel.Regions().Base(process.memory_region);
        const u32 linear_limit = linear_base + kernel.Regions().Size(process.memory_region);
        if (addr0 >= kHeapVaddr && addr0 < kHeapVaddrEnd) {
            const Result result = HeapFree(addr0, size);
            if (IsError(result)) {
                return result;
            }
        } else if (addr0 >= linear_base && addr0 < linear_limit) {
            const Result result = LinearFree(addr0, size);
            if (IsError(result)) {
                return result;
            }
        } else {
            return kResultInvalidAddress;
        }
        *out_addr = addr0;
        return kResultSuccess;
    }
    case kMemopCommit:
        if ((operation & kMemopLinear) != 0) {
            return LinearAllocate(out_addr, addr0, size, perm);
        }
        return HeapAllocate(out_addr, addr0, size, perm);
    case kMemopMap:
    case kMemopUnmap: {
        // Process::Map / Process::Unmap with privileged = false: `addr0` is the target and `addr1`
        // the source, which must be in the heap, and the two ranges must not overlap.
        const u32 target = addr0;
        const u32 source = addr1;
        if (source < kHeapVaddr || source + size > kHeapVaddrEnd || source + size < source) {
            return kResultInvalidAddress;
        }
        if (source - target < size || target - source < size) {
            return kResultInvalidAddressState;
        }
        if ((operation & kMemopOperationMask) == kMemopMap) {
            const auto block = memory.Query(target);
            if (!block || block->state != MemState::Free ||
                static_cast<u64>(block->base) + block->size < static_cast<u64>(target) + size) {
                return kResultInvalidAddressState;
            }
        }
        if (!map_unmap_logged) {
            map_unmap_logged = true;
            MH4N_LOG(Error, "Kernel.SVC",
                     "ControlMemory MAP/UNMAP (operation {}) would alias guest pages and is not "
                     "supported",
                     operation & kMemopOperationMask);
        }
        return kResultInvalidAddressState;
    }
    case kMemopProtect:
        return memory.Protect(addr0, size, perm);
    default:
        MH4N_LOG(Error, "Kernel.SVC", "unknown operation=0x{:08X}", operation);
        return kResultInvalidCombination;
    }
}

Result SvcTable::Impl::HeapAllocate(u32* out_addr, u32 target, u32 size, MemPerm perm) {
    MH4N_LOG(Debug, "Kernel.SVC", "Allocate heap target={:08X}, size={:08X}", target, size);
    if (target < kHeapVaddr || target + size > kHeapVaddrEnd || target + size < target) {
        MH4N_LOG(Error, "Kernel.SVC", "Invalid heap address");
        return kResultInvalidAddress;
    }
    {
        const auto block = memory.Query(target);
        if (!block || block->state != MemState::Free ||
            static_cast<u64>(block->base) + block->size < static_cast<u64>(target) + size) {
            MH4N_LOG(Error, "Kernel.SVC", "Trying to allocate already allocated memory");
            return kResultInvalidAddressState;
        }
    }
    Process& process = *kernel.CurrentProcess();
    const std::vector<FcramRange> pieces = kernel.Regions().HeapAllocate(process.memory_region, size);
    if (pieces.empty()) {
        MH4N_LOG(Error, "Kernel.SVC", "Not enough space");
        return kResultOutOfHeapMemory;
    }
    // Maps the heap block by block, in the order of the pieces.
    u32 vaddr = target;
    for (const FcramRange& piece : pieces) {
        MH4N_LOG(Debug, "Kernel.SVC", "Allocated FCRAM region lower={:08X}, upper={:08X}",
                 piece.offset, piece.offset + piece.size);
        const Result mapped = memory.Map(vaddr, piece.size, perm, MemState::Private,
                                         static_cast<u64>(kFcramPaddr) + piece.offset);
        MH4N_ASSERT(IsSuccess(mapped), "mapping the heap at 0x{:08X} failed: 0x{:08X}", vaddr, mapped);
        heap_runs[vaddr] = HeapRun{piece.size, piece.offset};
        vaddr += piece.size;
    }
    process.memory_used += size;
    process.resource_limit->Reserve(ResourceLimitType::Commit, static_cast<s32>(size));
    *out_addr = target;
    return kResultSuccess;
}

Result SvcTable::Impl::HeapFree(u32 target, u32 size) {
    MH4N_LOG(Debug, "Kernel.SVC", "Free heap target={:08X}, size={:08X}", target, size);
    if (target < kHeapVaddr || target + size > kHeapVaddrEnd || target + size < target) {
        MH4N_LOG(Error, "Kernel.SVC", "Invalid heap address");
        return kResultInvalidAddress;
    }
    if (size == 0) {
        return kResultSuccess;
    }
    // VMManager::GetBackingBlocksForRange: every block of the range must be there.
    if (!memory.IsMapped(target, size)) {
        MH4N_LOG(Error, "Kernel.SVC", "Trying to use already freed memory");
        return kResultInvalidAddressState;
    }
    Process& process = *kernel.CurrentProcess();
    const u32 end = target + size;
    u32 covered = 0;
    auto it = heap_runs.upper_bound(target);
    if (it != heap_runs.begin()) {
        --it;
    }
    while (it != heap_runs.end() && it->first < end) {
        const u32 run_start = it->first;
        const HeapRun run = it->second;
        const u32 run_end = run_start + run.size;
        if (run_end <= target) {
            ++it;
            continue;
        }
        const u32 lo = std::max(run_start, target);
        const u32 hi = std::min(run_end, end);
        if (!kernel.Regions().Free(process.memory_region, run.fcram_offset + (lo - run_start), hi - lo)) {
            MH4N_LOG(Error, "Kernel.SVC", "heap free: FCRAM at offset 0x{:08X} was not allocated",
                     run.fcram_offset + (lo - run_start));
        }
        covered += hi - lo;
        it = heap_runs.erase(it);
        if (run_start < lo) {  // the part before the range stays
            heap_runs[run_start] = HeapRun{lo - run_start, run.fcram_offset};
        }
        if (hi < run_end) {  // and the part after it
            it = heap_runs.insert_or_assign(hi, HeapRun{run_end - hi, run.fcram_offset + (hi - run_start)}).first;
            ++it;
        }
    }
    if (covered != size) {
        MH4N_LOG(Warning, "Kernel",
                 "heap free of 0x{:08X}+0x{:X}: {} bytes were not allocated by ControlMemory, their "
                 "FCRAM is not given back",
                 target, size, size - covered);
    }
    const Result unmapped = memory.Unmap(target, size);
    MH4N_ASSERT(IsSuccess(unmapped), "unmapping the heap at 0x{:08X} failed: 0x{:08X}", target, unmapped);
    process.memory_used -= size;
    process.resource_limit->Release(ResourceLimitType::Commit, static_cast<s32>(size));
    return kResultSuccess;
}

Result SvcTable::Impl::LinearAllocate(u32* out_addr, u32 target, u32 size, MemPerm perm) {
    MH4N_LOG(Debug, "Kernel.SVC", "Allocate linear heap target={:08X}, size={:08X}", target, size);
    Process& process = *kernel.CurrentProcess();
    FcramRegions& regions = kernel.Regions();
    const FcramRegion region = process.memory_region;
    const u32 area = LinearHeapArea();
    u32 offset;
    if (target == 0) {
        const std::optional<u32> found = regions.LinearAllocate(region, size);
        if (!found) {
            MH4N_LOG(Error, "Kernel.SVC", "Not enough space");
            return kResultOutOfHeapMemory;
        }
        offset = *found;
        target = offset + area;
    } else {
        const u32 base = area + regions.Base(region);
        const u32 limit = base + regions.Size(region);
        if (target < base || target + size > limit || target + size < target) {
            MH4N_LOG(Error, "Kernel.SVC", "Invalid linear heap address");
            return kResultInvalidAddress;
        }
        // Azahar ignores the unclear rules about where a given target may be.
        offset = target - area;
        if (!regions.LinearAllocate(region, offset, size)) {
            MH4N_LOG(Error, "Kernel.SVC", "Trying to allocate already allocated memory");
            return kResultInvalidAddressState;
        }
    }
    const Result mapped = memory.Map(target, size, perm, MemState::Continuous,
                                     static_cast<u64>(kFcramPaddr) + offset);
    if (IsError(mapped)) {
        regions.Free(region, offset, size);  // Azahar asserts that mapping succeeds
        return mapped;
    }
    process.memory_used += size;
    process.resource_limit->Reserve(ResourceLimitType::Commit, static_cast<s32>(size));
    MH4N_LOG(Debug, "Kernel.SVC", "Allocated at target={:08X}", target);
    *out_addr = target;
    return kResultSuccess;
}

Result SvcTable::Impl::LinearFree(u32 target, u32 size) {
    MH4N_LOG(Debug, "Kernel.SVC", "Free linear heap target={:08X}, size={:08X}", target, size);
    Process& process = *kernel.CurrentProcess();
    FcramRegions& regions = kernel.Regions();
    const FcramRegion region = process.memory_region;
    const u32 area = LinearHeapArea();
    const u32 base = area + regions.Base(region);
    const u32 limit = base + regions.Size(region);
    if (target < base || target + size > limit || target + size < target) {
        MH4N_LOG(Error, "Kernel.SVC", "Invalid linear heap address");
        return kResultInvalidAddress;
    }
    if (size == 0) {
        return kResultSuccess;
    }
    const Result unmapped = memory.Unmap(target, size);
    if (IsError(unmapped)) {
        return unmapped;
    }
    if (!regions.Free(region, target - area, size)) {
        MH4N_LOG(Error, "Kernel.SVC", "linear free: FCRAM at offset 0x{:08X} was not allocated", target - area);
    }
    process.memory_used -= size;
    process.resource_limit->Release(ResourceLimitType::Commit, static_cast<s32>(size));
    return kResultSuccess;
}

// ---- QueryMemory -----------------------------------------------------------------------------------

void SvcTable::Impl::QueryMemory(SvcFrame& f) {
    // r2 the address; out r1 base, r2 size, r3 permission, r4 state, r5 the page flags (0).
    RCtx& c = f.ctx;
    const auto block = memory.Query(c.r[2]);
    if (!block) {
        // Azahar's FindVMA gives the end of its map for an address past the managed range.
        for (int i = 1; i <= 5; ++i) {
            c.r[i] = 0;
        }
        Return(c, kResultInvalidAddress);
        return;
    }
    // "Query(Process)Memory merges vma with neighbours when they share the same state and
    // permissions, regardless of their physical mapping." (GuestMemory's blocks, like Azahar's
    // VMAs, are also kept apart when their backing is not contiguous.)
    u32 base = block->base;
    u64 end = static_cast<u64>(block->base) + block->size;
    while (base != 0) {
        const auto before = memory.Query(base - 1);
        if (!before || before->perm != block->perm || before->state != block->state) {
            break;
        }
        base = before->base;
    }
    while (end < kManagedEnd) {
        const auto after = memory.Query(static_cast<u32>(end));
        if (!after || after->perm != block->perm || after->state != block->state) {
            break;
        }
        end = static_cast<u64>(after->base) + after->size;
    }
    c.r[0] = kResultSuccess;
    c.r[1] = base;
    c.r[2] = static_cast<u32>(end - base);
    c.r[3] = static_cast<u32>(block->perm);
    c.r[4] = static_cast<u32>(block->state);
    c.r[5] = 0;
}

// ---- shared memory blocks ----------------------------------------------------------------------------

void SvcTable::Impl::CreateMemoryBlock(SvcFrame& f) {
    // r1 address, r2 size, r3 the creator's permissions, r0 the others'; out r1 the handle.
    RCtx& c = f.ctx;
    const u32 other_permission = c.r[0];
    const u32 addr = c.r[1];
    const u32 size = c.r[2];
    const u32 my_permission = c.r[3];
    c.r[1] = 0;
    if (size % kPageSize != 0) {
        Return(c, kResultMisalignedSize);
        return;
    }
    // Shared memory blocks cannot be created with execute permissions.
    const auto verify = [](u32 permission) {
        switch (static_cast<MemoryPermission>(permission)) {
        case MemoryPermission::None:
        case MemoryPermission::Read:
        case MemoryPermission::Write:
        case MemoryPermission::ReadWrite:
        case MemoryPermission::DontCare:
            return true;
        default:
            return false;
        }
    };
    if (!verify(my_permission) || !verify(other_permission)) {
        Return(c, kResultInvalidCombination);
        return;
    }
    // Azahar: "Processes with memory type APPLICATION are not allowed to create memory blocks with
    // addr = 0, any attempts to do so should return error 0xD92007EA" (a TODO it does not do).
    if ((addr < kProcessImageVaddr || addr + size > kSharedMemoryVaddrEnd) && addr != 0) {
        Return(c, kResultInvalidAddress);
        return;
    }
    const std::shared_ptr<Process> process = kernel.CurrentProcess();
    const std::shared_ptr<ResourceLimit> limit = process->resource_limit;
    if (!limit->Reserve(ResourceLimitType::SharedMemory, 1)) {
        Return(c, kResultOutOfSharedMems);
        return;
    }
    // A block at address 0 comes from the BASE region, or from the process's own when it has the
    // shared device memory flag.
    FcramRegion region = FcramRegion::Base;
    if (addr == 0 && process->shared_device_mem) {
        region = process->memory_region;
    }
    // The block gives the slot reserved above back when it is destroyed, also one that could not
    // be made (SharedMemory's destructor, as Azahar's).
    std::shared_ptr<SharedMemory> block;
    Result result = kernel.NewSharedMemory(&block, process, size, static_cast<MemoryPermission>(my_permission),
                                           static_cast<MemoryPermission>(other_permission), addr, region);
    Handle handle = 0;
    if (!IsError(result)) {
        result = process->handle_table.Create(&handle, std::move(block));
    }
    c.r[0] = result;
    c.r[1] = IsError(result) ? 0 : handle;
}

void SvcTable::Impl::MapMemoryBlock(SvcFrame& f) {
    // r0 the block, r1 the address, r2 the permissions of the caller, r3 of the others.
    RCtx& c = f.ctx;
    Process& process = *kernel.CurrentProcess();
    const auto block = process.handle_table.Get<SharedMemory>(c.r[0]);
    if (!block) {
        Return(c, kResultInvalidHandle);
        return;
    }
    const auto permissions = static_cast<MemoryPermission>(c.r[2]);
    switch (permissions) {
    case MemoryPermission::Read:
    case MemoryPermission::Write:
    case MemoryPermission::ReadWrite:
    case MemoryPermission::Execute:
    case MemoryPermission::ReadExecute:
    case MemoryPermission::WriteExecute:
    case MemoryPermission::ReadWriteExecute:
    case MemoryPermission::DontCare:
        Return(c, block->Map(process, c.r[1], permissions, static_cast<MemoryPermission>(c.r[3])));
        return;
    default:
        MH4N_LOG(Error, "Kernel.SVC", "unknown permissions=0x{:08X}", c.r[2]);
        Return(c, kResultInvalidCombination);
        return;
    }
}

void SvcTable::Impl::UnmapMemoryBlock(SvcFrame& f) {
    RCtx& c = f.ctx;
    Process& process = *kernel.CurrentProcess();
    const auto block = process.handle_table.Get<SharedMemory>(c.r[0]);
    if (!block) {
        Return(c, kResultInvalidHandle);
        return;
    }
    Return(c, block->Unmap(process, c.r[1]));
}

}  // namespace mh4n
