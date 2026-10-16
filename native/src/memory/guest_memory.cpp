// Derived from Azahar src/core/hle/kernel/vm_manager.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the splitting and merging of the block list (CarveVMA, CarveVMARange,
// SplitVMA, MergeAdjacent, ChangeMemoryState, UnmapRange, ReprotectRange) and the address
// translation of GPU::VirtualToPhysicalAddress (src/video_core/gpu.cpp). The host side (one flat
// reservation, no page table) is ours.
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (vm_manager.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp)
#include <algorithm>
#include <cstring>

#include "host.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"

#ifndef MH4N_DEBUG_GUARD
#define MH4N_DEBUG_GUARD 0
#endif

namespace mh4n {
namespace {

constexpr ZeroRegion kEmbeddedZeroRegions[] = {
#include "mh4n_zero_regions.inc"
};

constexpr u64 AlignDown(u64 value, u64 alignment) {
    return value & ~(alignment - 1);
}
constexpr u64 AlignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr u32 kLinearHeapSize = 0x08000000;
constexpr u32 kNewLinearHeapSize = 0x10000000;

} // namespace

const std::vector<ZeroRegion>& DefaultZeroRegions() {
    static const std::vector<ZeroRegion> regions(std::begin(kEmbeddedZeroRegions),
                                                 std::end(kEmbeddedZeroRegions));
    return regions;
}

ZeroPolicy DefaultZeroPolicy() {
    return MH4N_DEBUG_GUARD ? ZeroPolicy::Fault : ZeroPolicy::ZeroFill;
}

// ---- creation -----------------------------------------------------------------------------------

std::unique_ptr<GuestMemory> GuestMemory::Create(const GuestMemoryOptions& options) {
    std::unique_ptr<GuestMemory> memory(new GuestMemory());
    if (!memory->Init(options)) {
        return nullptr;
    }
    return memory;
}

bool GuestMemory::Init(const GuestMemoryOptions& options) {
    host_page_ = host::PageSize();
    if (options.host_page_size != 0) {
        const std::size_t size = options.host_page_size;
        if ((size & (size - 1)) != 0 || size < host_page_ || size > (1u << 20)) {
            MH4N_LOG(Error, "Memory", "host page size {} is not usable", size);
            return false;
        }
        host_page_ = size;
    }
    zero_policy_ = options.zero_policy;
    zero_regions_ = options.zero_regions;
    for (const ZeroRegion& region : zero_regions_) {
        if (region.start > region.last || (region.start & kPageMask) != 0 ||
            ((region.last + 1ull) & kPageMask) != 0) {
            MH4N_LOG(Error, "Memory", "zero region 0x{:08x}-0x{:08x} is not page aligned",
                     region.start, region.last);
            return false;
        }
    }

    reservation_size_ = static_cast<std::size_t>(kAddressSpaceSize + kGuardSize);
    void* reserved = host::Reserve(reservation_size_);
    if (reserved == nullptr) {
        MH4N_LOG(Error, "Memory", "cannot reserve {} MiB of address space for the guest",
                 reservation_size_ >> 20);
        return false;
    }
    base_ = static_cast<u8*>(reserved);
    if (!fault::RegisterReservation(reinterpret_cast<std::uintptr_t>(base_), reservation_size_)) {
        MH4N_LOG(Error, "Memory", "too many guest memories");
        return false; // the destructor releases the reservation
    }
    host::InstallFaultHandler();
    host::InstallThreadFaultStack();

    // The kernel's view: everything free.
    blocks_[0] = Block{kManagedEnd, MemPerm::None, MemState::Free, false, kNoBacking};

    if (zero_policy_ == ZeroPolicy::ZeroFill) {
        HostCommitZeroRegions(0, kAddressSpaceSize);
    }

    // The fixed regions, as Azahar's special mappings of the exheader (state IO, read-write).
    Result r = Map(kVramVaddr, kVramSize, MemPerm::ReadWrite, MemState::IO, kVramPaddr);
    if (IsError(r)) {
        return false;
    }
    r = Map(kDspRamVaddr, kDspRamSize, MemPerm::ReadWrite, MemState::IO, kDspRamPaddr);
    return IsSuccess(r);
}

GuestMemory::~GuestMemory() {
    if (base_ != nullptr) {
        fault::UnregisterReservation(reinterpret_cast<std::uintptr_t>(base_));
        host::Release(base_, reservation_size_);
    }
}

void GuestMemory::PrepareThreadForFaults() {
    host::InstallThreadFaultStack();
}

// ---- the block list -----------------------------------------------------------------------------

GuestMemory::BlockMap::iterator GuestMemory::FindBlock(u32 vaddr) {
    return std::prev(blocks_.upper_bound(vaddr));
}

GuestMemory::BlockMap::const_iterator GuestMemory::FindBlock(u32 vaddr) const {
    return std::prev(blocks_.upper_bound(vaddr));
}

bool GuestMemory::CanMerge(const Block& a, const Block& b) {
    if (a.perm != b.perm || a.state != b.state || a.mapped != b.mapped) {
        return false;
    }
    if (a.backing == kNoBacking || b.backing == kNoBacking) {
        return a.backing == b.backing; // both without a backing, or the blocks differ
    }
    return a.backing + a.size == b.backing;
}

GuestMemory::BlockMap::iterator GuestMemory::Split(BlockMap::iterator it, u32 offset) {
    Block& old_block = it->second;
    Block new_block = old_block;
    old_block.size = offset;
    new_block.size -= offset;
    if (new_block.backing != kNoBacking) {
        new_block.backing += offset;
    }
    return blocks_.emplace_hint(std::next(it), it->first + offset, new_block);
}

GuestMemory::BlockMap::iterator GuestMemory::MergeAdjacent(BlockMap::iterator it) {
    auto next = std::next(it);
    if (next != blocks_.end() && CanMerge(it->second, next->second)) {
        it->second.size += next->second.size;
        blocks_.erase(next);
    }
    if (it != blocks_.begin()) {
        auto prev = std::prev(it);
        if (CanMerge(prev->second, it->second)) {
            prev->second.size += it->second.size;
            blocks_.erase(it);
            it = prev;
        }
    }
    return it;
}

// Splits the edges of [vaddr, vaddr + size) so that a block starts at vaddr and one ends at the
// end, after checking that no part of the range is free. *first is the block at vaddr.
Result GuestMemory::CarveRange(u32 vaddr, u32 size, BlockMap::iterator* first) {
    const u64 end = static_cast<u64>(vaddr) + size;
    auto begin = FindBlock(vaddr);
    const auto i_end = blocks_.lower_bound(static_cast<u32>(std::min<u64>(end, kManagedEnd)));
    for (auto it = begin; it != i_end; ++it) {
        if (!it->second.mapped) {
            return kResultInvalidAddressState;
        }
    }
    if (vaddr != begin->first) {
        begin = Split(begin, vaddr - begin->first);
    }
    if (end < kManagedEnd) {
        auto end_block = FindBlock(static_cast<u32>(end));
        if (end != end_block->first) {
            Split(end_block, static_cast<u32>(end) - end_block->first);
        }
    }
    *first = begin;
    return 0;
}

namespace {
// The checks every range call starts with (Azahar asserts them).
bool ValidRange(u32 vaddr, u32 size) {
    return size != 0 && (vaddr & kPageMask) == 0 && (size & kPageMask) == 0 &&
           vaddr < kManagedEnd && static_cast<u64>(vaddr) + size <= kManagedEnd;
}
} // namespace

Result GuestMemory::Map(u32 vaddr, u32 size, MemPerm perm, MemState state, u64 backing) {
    if (size == 0 || (vaddr & kPageMask) != 0 || (size & kPageMask) != 0 || vaddr >= kManagedEnd) {
        return kResultInvalidAddress;
    }
    std::lock_guard lock(mutex_);
    auto it = FindBlock(vaddr);
    if (it->second.mapped) {
        return kResultInvalidAddressState; // already mapped
    }
    const u64 start_in_block = vaddr - it->first;
    const u64 end_in_block = start_in_block + size;
    if (end_in_block > it->second.size) {
        return kResultInvalidAddressState; // does not fit in the free block
    }
    if (end_in_block != it->second.size) {
        Split(it, static_cast<u32>(end_in_block));
    }
    if (start_in_block != 0) {
        it = Split(it, static_cast<u32>(start_in_block));
    }
    it->second = Block{size, perm, state, true, backing};
    HostMap(vaddr, vaddr + size);
    MergeAdjacent(it);
    return 0;
}

Result GuestMemory::Unmap(u32 vaddr, u32 size) {
    if (!ValidRange(vaddr, size)) {
        return kResultInvalidAddress;
    }
    std::lock_guard lock(mutex_);
    BlockMap::iterator it;
    if (Result r = CarveRange(vaddr, size, &it); IsError(r)) {
        return r;
    }
    const u64 end = static_cast<u64>(vaddr) + size;
    // Compare addresses, not iterators: merging erases blocks.
    while (it != blocks_.end() && it->first < end) {
        it->second = Block{it->second.size, MemPerm::None, MemState::Free, false, kNoBacking};
        it = std::next(MergeAdjacent(it));
    }
    HostUnmap(vaddr, static_cast<u32>(end));
    return 0;
}

Result GuestMemory::Protect(u32 vaddr, u32 size, MemPerm perm) {
    if (!ValidRange(vaddr, size)) {
        return kResultInvalidAddress;
    }
    std::lock_guard lock(mutex_);
    BlockMap::iterator it;
    if (Result r = CarveRange(vaddr, size, &it); IsError(r)) {
        return r;
    }
    const u64 end = static_cast<u64>(vaddr) + size;
    while (it != blocks_.end() && it->first < end) {
        it->second.perm = perm;
        it = std::next(MergeAdjacent(it));
    }
    return 0;
}

Result GuestMemory::ChangeState(u32 vaddr, u32 size, MemState expected_state, MemPerm expected_perm,
                                MemState new_state, MemPerm new_perm) {
    if (!ValidRange(vaddr, size)) {
        return kResultInvalidAddress;
    }
    std::lock_guard lock(mutex_);
    const u64 end = static_cast<u64>(vaddr) + size;
    const auto i_end = blocks_.lower_bound(static_cast<u32>(std::min<u64>(end, kManagedEnd)));
    for (auto i = FindBlock(vaddr); i != i_end; ++i) {
        if (i->second.state != expected_state ||
            (i->second.perm & expected_perm) != expected_perm) {
            return kResultInvalidAddressState;
        }
    }
    BlockMap::iterator it;
    if (Result r = CarveRange(vaddr, size, &it); IsError(r)) {
        return r;
    }
    while (it != blocks_.end() && it->first < end) {
        it->second.perm = new_perm;
        it->second.state = new_state;
        it = std::next(MergeAdjacent(it));
    }
    return 0;
}

std::optional<MemoryInfo> GuestMemory::Query(u32 vaddr) const {
    if (vaddr >= kManagedEnd) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    const auto it = FindBlock(vaddr);
    return MemoryInfo{it->first, it->second.size, it->second.perm, it->second.state};
}

bool GuestMemory::IsMapped(u32 vaddr, u32 size) const {
    std::lock_guard lock(mutex_);
    return IsMappedLocked(vaddr, size);
}

bool GuestMemory::IsMappedLocked(u32 vaddr, u32 size) const {
    if (size == 0) {
        return true;
    }
    const u64 end = static_cast<u64>(vaddr) + size;
    if (end > kManagedEnd) {
        return false;
    }
    for (auto it = FindBlock(vaddr); it != blocks_.end() && it->first < end; ++it) {
        if (!it->second.mapped) {
            return false;
        }
    }
    return true;
}

// ---- the host side ------------------------------------------------------------------------------

void GuestMemory::HostMap(u32 lo, u32 hi) {
    const u64 host_lo = AlignDown(lo, host_page_);
    const u64 host_hi = AlignUp(hi, host_page_);
    const bool ok = host::Commit(base_ + host_lo, host_hi - host_lo);
    MH4N_ASSERT(ok, "cannot commit guest memory 0x{:08x}-0x{:08x}", lo, hi);

    // Fresh host pages read as zero, but the parts of [lo, hi) that were accessible already may
    // hold what a stray access wrote: the edge host pages shared with other guest pages, and zero
    // regions (whose writes are kept). A new mapping must be zero-filled.
    auto clear = [&](u64 a, u64 b) {
        if (a < b) {
            std::memset(base_ + a, 0, b - a);
        }
    };
    // The first and the last host page of the range, unless the range covers them whole (a range
    // inside one host page is both; clearing a part twice is harmless).
    if (lo > host_lo || hi < host_lo + host_page_) {
        clear(lo, std::min<u64>(hi, host_lo + host_page_));
    }
    if (hi < host_hi || lo > host_hi - host_page_) {
        clear(std::max<u64>(lo, host_hi - host_page_), hi);
    }
    // A zero region keeps its whole host pages accessible, also the parts outside the region (when
    // the region is not host-page aligned), and a stray write there survives until now.
    if (zero_policy_ == ZeroPolicy::ZeroFill) {
        for (const ZeroRegion& region : zero_regions_) {
            clear(std::max<u64>(AlignDown(region.start, host_page_), lo),
                  std::min<u64>(AlignUp(region.last + 1ull, host_page_), hi));
        }
    }
}

// True if some guest page of the host page [page, page + page_size) must stay accessible: it is
// mapped, or in a zero region that is filled with zeros.
bool GuestMemory::HostPageWanted(u64 page, u64 page_size) const {
    if (zero_policy_ == ZeroPolicy::ZeroFill) {
        for (const ZeroRegion& region : zero_regions_) {
            if (region.start < page + page_size && region.last + 1ull > page) {
                return true;
            }
        }
    }
    if (page >= kManagedEnd) {
        return false;
    }
    for (auto it = FindBlock(static_cast<u32>(page));
         it != blocks_.end() && it->first < page + page_size; ++it) {
        if (it->second.mapped) {
            return true;
        }
    }
    return false;
}

void GuestMemory::HostCommitZeroRegions(u64 lo, u64 hi) {
    if (zero_policy_ != ZeroPolicy::ZeroFill) {
        return;
    }
    for (const ZeroRegion& region : zero_regions_) {
        const u64 a = std::max<u64>(region.start, lo);
        const u64 b = std::min<u64>(region.last + 1ull, hi);
        if (a < b) {
            const u64 host_lo = AlignDown(a, host_page_);
            const u64 host_hi = AlignUp(b, host_page_);
            const bool ok = host::Commit(base_ + host_lo, host_hi - host_lo);
            MH4N_ASSERT(ok, "cannot commit zero region 0x{:08x}-0x{:08x}", region.start,
                        region.last);
        }
    }
}

// The guest range [lo, hi) is free now. Whole host pages go back to the OS (and come back as zero
// pages if a zero region covers them). A host page that is shared with a mapped guest page (host
// pages larger than 4 KiB) stays; the freed part of it is zeroed so that mapping it again gives
// zeros.
void GuestMemory::HostUnmap(u32 lo, u32 hi) {
    const u64 hp = host_page_;
    const u64 inner_lo = AlignUp(lo, hp);
    const u64 inner_hi = AlignDown(hi, hp);
    if (inner_lo < inner_hi) {
        const bool ok = host::Decommit(base_ + inner_lo, inner_hi - inner_lo);
        MH4N_ASSERT(ok, "cannot decommit guest memory 0x{:08x}-0x{:08x}", lo, hi);
        HostCommitZeroRegions(inner_lo, inner_hi);
    }
    auto edge = [&](u64 page, u64 freed_lo, u64 freed_hi) {
        if (HostPageWanted(page, hp)) {
            std::memset(base_ + freed_lo, 0, freed_hi - freed_lo);
        } else {
            const bool ok = host::Decommit(base_ + page, hp);
            MH4N_ASSERT(ok, "cannot decommit guest memory page 0x{:08x}", page);
        }
    };
    const u64 left_page = AlignDown(lo, hp);
    if (lo % hp != 0) {
        edge(left_page, lo, std::min<u64>(hi, left_page + hp));
    }
    const u64 right_page = AlignDown(hi, hp);
    if (hi % hp != 0 && !(lo % hp != 0 && right_page == left_page)) {
        edge(right_page, std::max<u64>(lo, right_page), hi);
    }
}

// ---- the physical view --------------------------------------------------------------------------

void GuestMemory::SetLinearHeapArea(u32 vaddr) {
    std::lock_guard lock(mutex_);
    linear_heap_area_ = vaddr;
}

u32 GuestMemory::LinearHeapArea() const {
    std::lock_guard lock(mutex_);
    return linear_heap_area_;
}

u8* GuestMemory::PhysicalPointer(u32 paddr, u32 size) const {
    const u64 length = size != 0 ? size : 1;
    const u64 end = static_cast<u64>(paddr) + length;
    std::lock_guard lock(mutex_);
    u64 vaddr;
    if (paddr >= kVramPaddr && end <= static_cast<u64>(kVramPaddr) + kVramSize) {
        vaddr = static_cast<u64>(kVramVaddr) + (paddr - kVramPaddr);
    } else if (paddr >= kDspRamPaddr && end <= static_cast<u64>(kDspRamPaddr) + kDspRamSize) {
        vaddr = static_cast<u64>(kDspRamVaddr) + (paddr - kDspRamPaddr);
    } else if (paddr >= kFcramPaddr && end <= static_cast<u64>(kFcramPaddr) + kFcramSize) {
        if (linear_heap_area_ == 0) {
            return nullptr; // the loader has not set the linear heap yet
        }
        vaddr = static_cast<u64>(linear_heap_area_) + (paddr - kFcramPaddr);
    } else {
        return nullptr;
    }
    if (vaddr + length > kManagedEnd ||
        !IsMappedLocked(static_cast<u32>(vaddr), static_cast<u32>(length))) {
        return nullptr;
    }
    return base_ + vaddr;
}

std::optional<u32> GuestMemory::VirtualToPhysical(u32 vaddr) const {
    // The ends are accepted: a range end is an address too (a memory fill's end, for one).
    if (vaddr >= kVramVaddr && vaddr <= kVramVaddr + kVramSize) {
        return vaddr - kVramVaddr + kVramPaddr;
    }
    if (vaddr >= kLinearHeapVaddr && vaddr <= kLinearHeapVaddr + kLinearHeapSize) {
        return vaddr - kLinearHeapVaddr + kFcramPaddr;
    }
    if (vaddr >= kNewLinearHeapVaddr &&
        vaddr <= static_cast<u64>(kNewLinearHeapVaddr) + kNewLinearHeapSize) {
        return vaddr - kNewLinearHeapVaddr + kFcramPaddr;
    }
    if (vaddr >= kDspRamVaddr && vaddr <= kDspRamVaddr + kDspRamSize) {
        return vaddr - kDspRamVaddr + kDspRamPaddr;
    }
    return std::nullopt;
}

// ---- helpers ------------------------------------------------------------------------------------

bool GuestMemory::ReadBlock(void* dst, u32 vaddr, u32 size) const {
    std::lock_guard lock(mutex_);
    if (!IsMappedLocked(vaddr, size)) {
        return false;
    }
    if (size != 0) {
        std::memcpy(dst, base_ + vaddr, size);
    }
    return true;
}

bool GuestMemory::WriteBlock(u32 vaddr, const void* src, u32 size) {
    std::lock_guard lock(mutex_);
    if (!IsMappedLocked(vaddr, size)) {
        return false;
    }
    if (size != 0) {
        std::memcpy(base_ + vaddr, src, size);
    }
    return true;
}

std::optional<std::string> GuestMemory::ReadString(u32 vaddr, u32 max_length) const {
    std::lock_guard lock(mutex_);
    std::string out;
    for (u64 i = 0; i < max_length; ++i) {
        const u64 address = static_cast<u64>(vaddr) + i;
        if (address >= kManagedEnd) {
            return std::nullopt;
        }
        if (i == 0 || (address & kPageMask) == 0) {
            const u32 page = static_cast<u32>(address & ~static_cast<u64>(kPageMask));
            if (!IsMappedLocked(page, kPageSize)) {
                return std::nullopt;
            }
        }
        const char c = static_cast<char>(base_[address]);
        if (c == '\0') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace mh4n
