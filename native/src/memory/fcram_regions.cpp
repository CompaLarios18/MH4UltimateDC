// Derived from Azahar src/core/hle/kernel/memory.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the region size table and the algorithms of MemoryRegionInfo (Reset,
// HeapAllocate, both LinearAllocate, RLinearAllocate, Free), on a std::map of free intervals
// instead of boost::icl::interval_set.
#include <algorithm>

#include "mh4n/assert.h"
#include "mh4n/memory.h"

namespace mh4n {
namespace {

// Size of the APPLICATION, SYSTEM and BASE regions for each memory mode (Azahar's
// memory_region_sizes). Mode 1 "appears to be unused" there and is all zeros.
constexpr u32 kRegionSizes[8][3] = {
    // Old 3DS layouts
    {0x04000000, 0x02C00000, 0x01400000}, // 0
    {0, 0, 0},                            // 1
    {0x06000000, 0x00C00000, 0x01400000}, // 2
    {0x05000000, 0x01C00000, 0x01400000}, // 3
    {0x04800000, 0x02400000, 0x01400000}, // 4
    {0x02000000, 0x04C00000, 0x01400000}, // 5
    // New 3DS layouts
    {0x07C00000, 0x06400000, 0x02000000}, // 6
    {0x0B200000, 0x02E00000, 0x02000000}, // 7
};

} // namespace

bool FcramRegions::ValidMemoryMode(u32 memory_mode) {
    return memory_mode < 8 && memory_mode != 1;
}

FcramRegions::FcramRegions(u32 memory_mode) : mode_(memory_mode) {
    MH4N_ASSERT(ValidMemoryMode(memory_mode), "invalid memory mode {}", memory_mode);
    // The regions are laid out in sequence from the start of FCRAM.
    u32 base = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        Region& r = regions_[i];
        r.base = base;
        r.size = kRegionSizes[memory_mode][i];
        r.used = 0;
        r.free.clear();
        r.free.emplace(r.base, r.base + r.size); // the entire region is free
        base += r.size;
    }
    MH4N_ASSERT(base == TotalSize(), "the regions of mode {} do not fill FCRAM", memory_mode);
}

u32 FcramRegions::TotalSize() const {
    return mode_ >= 6 ? kFcramSizeNew3ds : kFcramSize;
}

std::vector<FcramRange> FcramRegions::FreeBlocks(FcramRegion region) const {
    std::vector<FcramRange> out;
    for (const auto& [lo, hi] : At(region).free) {
        out.push_back({lo, hi - lo});
    }
    return out;
}

void FcramRegions::AddFree(Region& r, u32 lo, u32 hi) {
    if (lo >= hi) {
        return;
    }
    // Join with the interval that ends at lo and the one that starts at hi.
    auto next = r.free.lower_bound(lo);
    if (next != r.free.begin()) {
        auto prev = std::prev(next);
        if (prev->second == lo) {
            lo = prev->first;
            r.free.erase(prev);
        }
    }
    if (next != r.free.end() && next->first == hi) {
        hi = next->second;
        r.free.erase(next);
    }
    r.free[lo] = hi;
}

void FcramRegions::RemoveFree(Region& r, u32 lo, u32 hi) {
    if (lo >= hi) {
        return;
    }
    // The range must lie inside one free interval (callers check).
    auto it = std::prev(r.free.upper_bound(lo));
    const u32 block_lo = it->first;
    const u32 block_hi = it->second;
    r.free.erase(it);
    if (block_lo < lo) {
        r.free[block_lo] = lo;
    }
    if (hi < block_hi) {
        r.free[hi] = block_hi;
    }
}

std::vector<FcramRange> FcramRegions::HeapAllocate(FcramRegion region, u32 size) {
    Region& r = At(region);
    std::vector<FcramRange> pieces; // collected from the top, reversed below
    u32 rest = size;

    // Try allocating from the higher address.
    for (auto it = r.free.rbegin(); it != r.free.rend(); ++it) {
        const u32 length = it->second - it->first;
        if (length >= rest) {
            // The request is fulfilled with this block.
            if (rest != 0) {
                pieces.push_back({it->second - rest, rest});
            }
            rest = 0;
            break;
        }
        pieces.push_back({it->first, length});
        rest -= length;
    }

    if (rest != 0) {
        return {}; // not enough free space
    }
    std::reverse(pieces.begin(), pieces.end()); // an interval set iterates in ascending order
    for (const FcramRange& piece : pieces) {
        RemoveFree(r, piece.offset, piece.offset + piece.size);
    }
    r.used += size;
    return pieces;
}

bool FcramRegions::LinearAllocate(FcramRegion region, u32 offset, u32 size) {
    Region& r = At(region);
    const u64 end = static_cast<u64>(offset) + size;
    if (end > 0xFFFFFFFFull) {
        return false;
    }
    if (size != 0) { // an empty interval is contained in any set
        auto it = r.free.upper_bound(offset);
        if (it == r.free.begin()) {
            return false;
        }
        --it;
        if (it->first > offset || it->second < end) {
            return false; // the requested range is already allocated
        }
        RemoveFree(r, offset, static_cast<u32>(end));
    }
    r.used += size;
    return true;
}

std::optional<u32> FcramRegions::LinearAllocate(FcramRegion region, u32 size) {
    Region& r = At(region);
    // Find the first sufficient continuous block from the lower address.
    for (const auto& entry : r.free) {
        const u32 lo = entry.first; // copies: RemoveFree erases the entry
        if (entry.second - lo >= size) {
            RemoveFree(r, lo, lo + size);
            r.used += size;
            return lo;
        }
    }
    return std::nullopt; // no sufficient block found
}

std::optional<u32> FcramRegions::RLinearAllocate(FcramRegion region, u32 size) {
    Region& r = At(region);
    // Find the first sufficient continuous block from the upper address.
    for (auto it = r.free.rbegin(); it != r.free.rend(); ++it) {
        if (it->second - it->first >= size) {
            const u32 lo = it->second - size;
            RemoveFree(r, lo, it->second);
            r.used += size;
            return lo;
        }
    }
    return std::nullopt;
}

bool FcramRegions::Free(FcramRegion region, u32 offset, u32 size) {
    Region& r = At(region);
    const u64 end = static_cast<u64>(offset) + size;
    if (offset < r.base || end > static_cast<u64>(r.base) + r.size || size > r.used) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    // It must be an allocated range: no part of it may be free.
    auto it = r.free.lower_bound(offset);
    if (it != r.free.end() && it->first < end) {
        return false;
    }
    if (it != r.free.begin() && std::prev(it)->second > offset) {
        return false;
    }
    AddFree(r, offset, static_cast<u32>(end));
    r.used -= size;
    return true;
}

} // namespace mh4n
