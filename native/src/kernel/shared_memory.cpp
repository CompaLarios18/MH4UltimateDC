// Derived from Azahar src/core/hle/kernel/shared_memory.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the permission checks of Map, the
// target address of a block created at address 0, the creation of both kinds of block and the
// clean-up in the destructor. The backing is host memory plus the guest range instead of FCRAM
// pointers.
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project
#include <cstring>

#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {
namespace {

// SharedMemory::ConvertPermissions: the low three bits are the VMA permission.
MemPerm ToMemPerm(MemoryPermission permission) {
    return static_cast<MemPerm>(static_cast<u32>(permission) & 7u);
}

constexpr u32 kSharedMemoryVaddrEnd = 0x14000000;  // Azahar's SHARED_MEMORY_VADDR_END

} // namespace

SharedMemory::SharedMemory(Kernel& kernel) : Object(kernel) {}

SharedMemory::~SharedMemory() {
    for (const Piece& piece : pieces_) {
        if (!kernel_.Regions().Free(region_, piece.offset, piece.size)) {
            MH4N_LOG(Error, "Kernel", "shared memory: cannot give back FCRAM offset 0x{:08X}",
                     piece.offset);
        }
    }
    if (auto process = owner_process_.lock()) {
        if (process->resource_limit) {
            process->resource_limit->Release(ResourceLimitType::SharedMemory, 1);
        }
        if (base_address_ != 0) {
            // The owner's memory goes back to ordinary heap.
            kernel_.Memory().ChangeState(base_address_, size_, MemState::Locked, MemPerm::None,
                                         MemState::Private, MemPerm::ReadWrite);
        } else if (!pieces_.empty()) {
            process->memory_used -= size_;
        }
    }
}

Result SharedMemory::Map(Process& target, u32 address, MemoryPermission permissions,
                         MemoryPermission other_permissions) {
    const auto owner = owner_process_.lock();
    const MemoryPermission own_other_permissions =
        &target == owner.get() ? permissions_ : other_permissions_;

    // Automatically allocated blocks can only be mapped with other_permissions = DontCare.
    if (base_address_ == 0 && other_permissions != MemoryPermission::DontCare) {
        return kResultInvalidCombination;
    }
    // The requested permissions must match what the creator allows.
    if (static_cast<u32>(permissions) & ~static_cast<u32>(own_other_permissions)) {
        MH4N_LOG(Error, "Kernel",
                 "cannot map id={}, address=0x{:08X} name={}, permissions don't match", ObjectId(),
                 address, Name());
        return kResultInvalidCombination;
    }
    // Heap-backed blocks cannot be mapped with other_permissions = DontCare.
    if (base_address_ != 0 && other_permissions == MemoryPermission::DontCare) {
        MH4N_LOG(Error, "Kernel",
                 "cannot map id={}, address=0x{:08X} name={}, permissions don't match", ObjectId(),
                 address, Name());
        return kResultInvalidCombination;
    }
    // The permissions must be compatible with what the creator needs.
    if (other_permissions != MemoryPermission::DontCare &&
        (static_cast<u32>(permissions_) & ~static_cast<u32>(other_permissions))) {
        MH4N_LOG(Error, "Kernel",
                 "cannot map id={}, address=0x{:08X} name={}, permissions don't match", ObjectId(),
                 address, Name());
        return kResultWrongPermission;
    }
    if (address != 0) {
        // 64-bit sum: Azahar's 32-bit one would wrap for addresses near the top.
        if (address < kHeapVaddr ||
            static_cast<u64>(address) + size_ >= kSharedMemoryVaddrEnd) {
            MH4N_LOG(Error, "Kernel", "cannot map id={}, address=0x{:08X} name={}, invalid address",
                     ObjectId(), address, Name());
            return kResultInvalidAddress;
        }
    }

    u32 target_address = address;
    if (base_address_ == 0 && target_address == 0) {
        // Even on new firmware the target is in the old linear heap area, to keep the shared font
        // compatible (see APT:GetSharedFont).
        target_address = kLinearHeapVaddr + linear_heap_phys_offset_;
    }

    GuestMemory& memory = kernel_.Memory();
    const auto block = memory.Query(target_address);
    if (!block) {
        return kResultInvalidAddress;
    }
    if (block->state != MemState::Free ||
        static_cast<u64>(block->base) + block->size < static_cast<u64>(target_address) + size_) {
        MH4N_LOG(Error, "Kernel",
                 "cannot map id={}, address=0x{:08X} name={}, mapping to already allocated memory",
                 ObjectId(), address, Name());
        return kResultInvalidAddressState;
    }

    // Host aliasing is unsupported (docs/native/wp/N1.md, N1-05): a second address, or any address
    // for a block that lives in the owner's own memory.
    if (mapped_ || pieces_.empty()) {
        MH4N_LOG(Error, "Kernel",
                 "unsupported: shared memory id={} name={} would be mapped at a second address "
                 "(0x{:08X})",
                 ObjectId(), Name(), target_address);
        return kResultInvalidAddressState;
    }

    u32 cursor = target_address;
    for (const Piece& piece : pieces_) {
        const Result result = memory.Map(cursor, piece.size, ToMemPerm(permissions),
                                         MemState::Shared, kFcramPaddr + piece.offset);
        MH4N_ASSERT(IsSuccess(result), "shared memory: mapping 0x{:08X} failed with 0x{:08X}",
                    cursor, result);
        cursor += piece.size;
    }
    // The guest range becomes the home of the contents.
    memory.WriteBlock(target_address, host_data_.data(), size_);
    mapped_ = true;
    mapped_at_ = target_address;
    return kResultSuccess;
}

Result SharedMemory::Unmap(Process&, u32 address) {
    if (!mapped_ || address != mapped_at_) {
        MH4N_LOG(Error, "Kernel", "unmap of shared memory id={} name={} at 0x{:08X}: not mapped there",
                 ObjectId(), Name(), address);
        return kResultInvalidAddressState;
    }
    GuestMemory& memory = kernel_.Memory();
    // The contents go back to the host, so that a later map finds them (FCRAM keeps them in Azahar).
    memory.ReadBlock(host_data_.data(), mapped_at_, size_);
    const Result result = memory.Unmap(mapped_at_, size_);
    if (IsSuccess(result)) {
        mapped_ = false;
        mapped_at_ = 0;
    }
    return result;
}

u8* SharedMemory::GetPointer(u32 offset) {
    if (mapped_) {
        return kernel_.Memory().Base() + mapped_at_ + offset;
    }
    if (pieces_.empty()) {
        return kernel_.Memory().Base() + base_address_ + offset;  // the owner's own memory
    }
    return host_data_.data() + offset;
}

} // namespace mh4n
