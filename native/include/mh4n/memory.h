// Derived from Azahar src/core/memory.h and src/core/hle/kernel/errors.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the address-space constants and the result code values.
// Guest memory (N1-02): the 3DS process's 4 GiB address space as one host reservation, the
// mapping bookkeeping behind svcQueryMemory, the physical view the GPU and DSP use, and Azahar's
// FCRAM allocation order.
//
// Flat memory: guest address A lives at GuestMemory::Base() + A, which is what goes in RCtx::mem
// (tools/recomp/runtime/recomp.h). Mapped pages are readable, writable and zero-filled; the
// guest permissions are kept for Query but not enforced (recompiled code never executes guest
// memory, and Apple silicon's 16 KiB host pages are coarser than the guest's 4 KiB). An access
// to a page that is not mapped faults, and the fault handler ends the process with status 3 after
// one line on stderr.
//
// Threads: the bookkeeping (Map ... VirtualToPhysical, the Read/Write helpers' range checks) is
// internally synchronised, because the GPU thread asks for PhysicalPointer; the bytes are not
// (principle 6 of docs/native/ROADMAP.md: only the scheduler's host thread touches guest memory).
#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "mh4n/types.h"

namespace mh4n {

// ---- the address map (Azahar src/core/memory.h) -----------------------------------------------

inline constexpr u32 kPageSize = 0x1000;       // the guest's page
inline constexpr u32 kPageMask = kPageSize - 1;
inline constexpr u64 kAddressSpaceSize = 1ull << 32;
inline constexpr u64 kGuardSize = 0x10000;     // inaccessible host address space after the 4 GiB
// The kernel manages [0, 0x40000000) (Azahar's VMManager::MAX_ADDRESS, the New 3DS limit).
inline constexpr u32 kManagedEnd = 0x40000000;

inline constexpr u32 kProcessImageVaddr = 0x00100000;
inline constexpr u32 kHeapVaddr = 0x08000000;
inline constexpr u32 kHeapVaddrEnd = 0x10000000;
inline constexpr u32 kSharedMemoryVaddr = 0x10000000;
inline constexpr u32 kLinearHeapVaddr = 0x14000000;     // kernel version below 0x22C
inline constexpr u32 kNewLinearHeapVaddr = 0x30000000;  // kernel version 0x22C and later
inline constexpr u32 kVramVaddr = 0x1F000000;
inline constexpr u32 kDspRamVaddr = 0x1FF00000;
inline constexpr u32 kConfigMemoryVaddr = 0x1FF80000;
inline constexpr u32 kSharedPageVaddr = 0x1FF81000;
inline constexpr u32 kTlsAreaVaddr = 0x1FF82000;

inline constexpr u32 kVramPaddr = 0x18000000;
inline constexpr u32 kVramSize = 0x00600000;
inline constexpr u32 kDspRamPaddr = 0x1FF00000;
inline constexpr u32 kDspRamSize = 0x00080000;
inline constexpr u32 kFcramPaddr = 0x20000000;
inline constexpr u32 kFcramSize = 0x08000000;           // Old 3DS
inline constexpr u32 kFcramSizeNew3ds = 0x10000000;

// ---- permissions and states (the 3DS values, as svcQueryMemory returns them) ------------------

enum class MemPerm : u8 {
    None = 0,
    Read = 1,
    Write = 2,
    Execute = 4,
    ReadWrite = Read | Write,
    ReadExecute = Read | Execute,
    WriteExecute = Write | Execute,
    ReadWriteExecute = Read | Write | Execute,
};
constexpr MemPerm operator|(MemPerm a, MemPerm b) {
    return static_cast<MemPerm>(static_cast<u8>(a) | static_cast<u8>(b));
}
constexpr MemPerm operator&(MemPerm a, MemPerm b) {
    return static_cast<MemPerm>(static_cast<u8>(a) & static_cast<u8>(b));
}

enum class MemState : u8 {
    Free = 0,
    Reserved = 1,
    IO = 2,
    Static = 3,
    Code = 4,
    Private = 5,
    Shared = 6,
    Continuous = 7,
    Aliased = 8,
    Alias = 9,
    AliasCode = 10,
    Locked = 11,
};

// The block that contains an address: Azahar's VMA, so the extents are the same.
struct MemoryInfo {
    u32 base;
    u32 size;
    MemPerm perm;
    MemState state;
    friend bool operator==(const MemoryInfo&, const MemoryInfo&) = default;
};

// Results of the mapping calls (Azahar src/core/hle/kernel/errors.h).
inline constexpr Result kResultInvalidAddress = 0xE0E01BF5;       // misaligned, empty or outside
inline constexpr Result kResultInvalidAddressState = 0xE0A01BF5;  // wrong state: mapped, or free
static_assert(kResultInvalidAddress == MakeResult(0x3F5, 6, 7, 28));
static_assert(kResultInvalidAddressState == MakeResult(0x3F5, 6, 5, 28));

// ---- zero regions ------------------------------------------------------------------------------

// Guest addresses [start, last] that the game reads although nothing is mapped there, and that
// Azahar answers with zeros (config/native/zero_regions.txt).
struct ZeroRegion {
    u32 start;
    u32 last;
    friend bool operator==(const ZeroRegion&, const ZeroRegion&) = default;
};

enum class ZeroPolicy {
    ZeroFill,  // the regions are host-accessible and zero: reads give 0, writes are kept
    Fault,     // the regions stay unmapped, so the access faults (MH4N_DEBUG_GUARD builds)
};

// The regions of config/native/zero_regions.txt, embedded at configure time.
const std::vector<ZeroRegion>& DefaultZeroRegions();
// Fault when MH4N_DEBUG_GUARD is on, ZeroFill otherwise.
ZeroPolicy DefaultZeroPolicy();

struct GuestMemoryOptions {
    ZeroPolicy zero_policy = DefaultZeroPolicy();
    std::vector<ZeroRegion> zero_regions = DefaultZeroRegions();
    // 0 uses the host's page size. A larger power of two (a multiple of the host's) makes the
    // commit and decommit granularity coarser, as on Apple silicon (16 KiB): tests use it to run
    // that path on a 4 KiB host.
    std::size_t host_page_size = 0;
};

// ---- bad accesses ------------------------------------------------------------------------------

// The guest thread that runs on this host thread, for the report of a bad access (and for logs).
// `rctx` is the thread's RCtx (tools/recomp/runtime/recomp.h; only r[14], the LR, is read, which
// is word 14 of the first member). The code host (N1-04) calls this on every switch into a guest
// fiber and ClearFaultContext() when it switches out. thread_local, so one per host thread.
void SetFaultContext(const void* rctx, u32 thread_id);
void ClearFaultContext();

// Formats the line the fault handler writes to stderr, without allocating (async-signal-safe):
//   mh4n: bad guest memory access: <read|write|access> at 0x<addr> (guest thread <id>, lr 0x<lr>)
// or "(no guest thread)" in place of the parenthesis when no context is set; ends with a newline.
// Returns the length written, cut at `capacity`. Exposed for tests.
enum class FaultAccess { Unknown, Read, Write };
std::size_t FormatFaultReport(char* out, std::size_t capacity, u32 guest_addr, FaultAccess access,
                              bool has_context, u32 thread_id, u32 lr);

// ---- guest memory ------------------------------------------------------------------------------

inline constexpr u64 kNoBacking = ~0ull;

class GuestMemory {
public:
    // Reserves 4 GiB plus a 64 KiB guard (inaccessible, nothing committed), installs the fault
    // handler, maps VRAM and DSP RAM and applies the zero regions. nullptr if the host refuses.
    static std::unique_ptr<GuestMemory> Create(const GuestMemoryOptions& options = {});
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Host address of guest address 0 (RCtx::mem).
    u8* Base() const { return base_; }
    ZeroPolicy GetZeroPolicy() const { return zero_policy_; }

    // Makes the calling host thread report bad accesses on a stack of its own (POSIX sigaltstack).
    // Create() does it for its caller; a thread that runs guest code calls it once.
    static void PrepareThreadForFaults();

    // ---- mapping (Azahar's VMManager) ----
    // vaddr and size are multiples of 4 KiB and the range lies in [0, kManagedEnd). The range
    // becomes readable, writable and zero-filled. `backing` is an opaque address of the memory
    // behind the block, in practice the physical address (kFcramPaddr + FCRAM offset) of its first
    // byte: two adjacent blocks with the same permission and state are merged into one only if
    // their backings are contiguous or both absent, as Azahar's VMAs, whose host pointers are
    // contiguous only when the FCRAM is. ControlMemory (N1-07) passes the FCRAM address of each
    // piece, so that Query returns Azahar's extents.
    // Errors: kResultInvalidAddress (misaligned, empty, or vaddr outside the managed range),
    // kResultInvalidAddressState (the range is not inside one free block).
    Result Map(u32 vaddr, u32 size, MemPerm perm, MemState state, u64 backing = kNoBacking);
    // The whole range must be mapped, but may span blocks. Gives the pages back to the OS.
    // Error: kResultInvalidAddressState (part of the range is free), kResultInvalidAddress.
    Result Unmap(u32 vaddr, u32 size);
    // Changes the permission of a mapped range (blocks split and merge). Errors as Unmap.
    Result Protect(u32 vaddr, u32 size, MemPerm perm);
    // Azahar's ChangeMemoryState: every block of the range must have `expected_state` and at least
    // the permissions `expected_perm`, else kResultInvalidAddressState.
    Result ChangeState(u32 vaddr, u32 size, MemState expected_state, MemPerm expected_perm,
                       MemState new_state, MemPerm new_perm);

    // The block containing vaddr (free blocks included), nullopt at or above kManagedEnd.
    std::optional<MemoryInfo> Query(u32 vaddr) const;
    // Every page of [vaddr, vaddr + size) is mapped. An empty range is mapped. Zero regions are
    // not mapped in this sense.
    bool IsMapped(u32 vaddr, u32 size) const;

    // ---- the physical view (the GPU and the DSP) ----
    // The guest address where FCRAM starts: kLinearHeapVaddr or kNewLinearHeapVaddr by the kernel
    // version (the loader sets it, N1-03). Until it is set, FCRAM has no physical view.
    void SetLinearHeapArea(u32 vaddr);
    u32 LinearHeapArea() const;
    // Host pointer for a physical range inside one of: FCRAM [0x20000000, 0x28000000) through
    // the linear heap (vaddr = paddr - 0x20000000 + the linear heap area), VRAM
    // [0x18000000, 0x18600000) at kVramVaddr, DSP RAM [0x1FF00000, 0x1FF80000). nullptr when
    // the range leaves its area or, for FCRAM, is not completely mapped. size 0 checks the byte
    // at paddr.
    u8* PhysicalPointer(u32 paddr, u32 size) const;
    // Azahar's GPU::VirtualToPhysicalAddress for VRAM, the old linear heap window and the new one
    // (0x14000000 and 0x30000000 + FCRAM size), plus the DSP RAM. The end of an area is accepted
    // (a range end is an address too). Not mapped-checked. Azahar returns 0 for address 0 and the
    // address itself for unknown ones; this returns nullopt for both, the caller decides.
    std::optional<u32> VirtualToPhysical(u32 vaddr) const;

    // ---- helpers: the range must be mapped (IsMapped), else they fail and touch nothing
    bool ReadBlock(void* dst, u32 vaddr, u32 size) const;
    bool WriteBlock(u32 vaddr, const void* src, u32 size);
    template <class T>
    std::optional<T> Read(u32 vaddr) const {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        if (!ReadBlock(&value, vaddr, sizeof(T))) {
            return std::nullopt;
        }
        return value;
    }
    template <class T>
    bool Write(u32 vaddr, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        return WriteBlock(vaddr, &value, sizeof(T));
    }
    // Up to max_length bytes, stopping at a NUL (not included), as Azahar's ReadCString. An
    // unmapped byte before the end of the string is an error.
    std::optional<std::string> ReadString(u32 vaddr, u32 max_length) const;

private:
    GuestMemory() = default;
    struct Block {
        u32 size;
        MemPerm perm;
        MemState state;
        bool mapped;
        u64 backing;
    };
    using BlockMap = std::map<u32, Block>;

    bool Init(const GuestMemoryOptions& options);
    bool IsMappedLocked(u32 vaddr, u32 size) const;
    BlockMap::iterator FindBlock(u32 vaddr);
    BlockMap::const_iterator FindBlock(u32 vaddr) const;
    BlockMap::iterator Split(BlockMap::iterator it, u32 offset);
    BlockMap::iterator MergeAdjacent(BlockMap::iterator it);
    static bool CanMerge(const Block& a, const Block& b);
    Result CarveRange(u32 vaddr, u32 size, BlockMap::iterator* first);
    void HostMap(u32 lo, u32 hi);
    void HostUnmap(u32 lo, u32 hi);
    bool HostPageWanted(u64 page, u64 page_size) const;
    void HostCommitZeroRegions(u64 lo, u64 hi);

    u8* base_ = nullptr;
    std::size_t reservation_size_ = 0;
    ZeroPolicy zero_policy_ = ZeroPolicy::ZeroFill;
    std::vector<ZeroRegion> zero_regions_;
    std::size_t host_page_ = 0x1000;
    mutable std::mutex mutex_;
    BlockMap blocks_;
    u32 linear_heap_area_ = 0;
};

// ---- the FCRAM allocator (Azahar's MemoryRegionInfo) -------------------------------------------

enum class FcramRegion : u32 { Application = 0, System = 1, Base = 2 };

// A range of FCRAM, as an offset from the start of FCRAM (physical address - kFcramPaddr).
struct FcramRange {
    u32 offset;
    u32 size;
    friend bool operator==(const FcramRange&, const FcramRange&) = default;
};

// Bookkeeping of the three regions, laid out in the order APPLICATION, SYSTEM, BASE from FCRAM
// offset 0 with the sizes of Azahar's table for the memory mode, and the same results as Azahar's
// MemoryRegionInfo for the same calls. Free space is a set of joined intervals; offsets are
// absolute (a region's offsets start at its Base()).
class FcramRegions {
public:
    // Memory modes 0 and 2-5 are Old 3DS layouts (128 MiB), 6 and 7 New 3DS (256 MiB); 1 is
    // unused in Azahar.
    static bool ValidMemoryMode(u32 memory_mode);
    explicit FcramRegions(u32 memory_mode);  // aborts for an invalid mode

    u32 MemoryMode() const { return mode_; }
    u32 TotalSize() const;
    u32 Base(FcramRegion region) const { return At(region).base; }
    u32 Size(FcramRegion region) const { return At(region).size; }
    u32 Used(FcramRegion region) const { return At(region).used; }
    // The free intervals, ascending.
    std::vector<FcramRange> FreeBlocks(FcramRegion region) const;

    // Free space taken from the top of the region, in as many pieces as it takes, returned in
    // ascending order; empty (and nothing changed) if there is not enough space.
    std::vector<FcramRange> HeapAllocate(FcramRegion region, u32 size);
    // A given range: false (nothing changed) unless it is entirely free.
    bool LinearAllocate(FcramRegion region, u32 offset, u32 size);
    // The first free block from the bottom that is large enough, from its start.
    std::optional<u32> LinearAllocate(FcramRegion region, u32 size);
    // The same from the top: the end of the last block that is large enough.
    std::optional<u32> RLinearAllocate(FcramRegion region, u32 size);
    // Returns an allocated range; false (nothing changed) if any of it is free or outside the
    // region (Azahar asserts).
    bool Free(FcramRegion region, u32 offset, u32 size);

private:
    struct Region {
        u32 base = 0;
        u32 size = 0;
        u32 used = 0;
        std::map<u32, u32> free;  // lower -> upper, joined, ascending
    };
    Region& At(FcramRegion region) { return regions_[static_cast<std::size_t>(region)]; }
    const Region& At(FcramRegion region) const {
        return regions_[static_cast<std::size_t>(region)];
    }
    static void AddFree(Region& r, u32 lo, u32 hi);
    static void RemoveFree(Region& r, u32 lo, u32 hi);

    u32 mode_;
    std::array<Region, 3> regions_;
};

} // namespace mh4n
