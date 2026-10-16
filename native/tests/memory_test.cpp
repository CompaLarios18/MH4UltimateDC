// Derived from Azahar src/core/hle/kernel/vm_manager.cpp, src/core/hle/kernel/memory.cpp,
// src/core/hle/kernel/process.cpp, src/core/hle/kernel/errors.h, src/core/memory.h and
// src/video_core/gpu.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of those files says "GPLv2 or any later version"): the expected
// values of the VMA, allocator and physical-view cases, worked out from VMManager,
// MemoryRegionInfo, Process::Map/HeapAllocate and GPU::VirtualToPhysicalAddress, the result code
// values, the address constants, and the table of FCRAM region sizes per memory mode
// (memory_region_sizes), written out again.
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (vm_manager.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (memory.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp)
// Copyright 2017 Citra Emulator Project (errors.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp)
//
// mh4n_memory (N1-02): guest memory mapping, zero regions, the physical view, the FCRAM
// allocator and bad accesses.
//
// The expected values of the VMA and allocator cases were worked out by hand from the algorithms
// of Azahar's VMManager (src/core/hle/kernel/vm_manager.cpp) and MemoryRegionInfo
// (src/core/hle/kernel/memory.cpp), with the comments saying how. Bad accesses end the process,
// so they run in a child process: this binary with `--fault-test <scenario>`.
#define MH4N_TEST_OWN_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "mh4n/memory.h"

using namespace mh4n;

namespace {

std::string g_self_path;

std::unique_ptr<GuestMemory> MakeMemory(const GuestMemoryOptions& options = {}) {
    auto memory = GuestMemory::Create(options);
    REQUIRE(memory != nullptr);
    return memory;
}

constexpr MemPerm RW = MemPerm::ReadWrite;
constexpr MemPerm R = MemPerm::Read;

// Is the byte at `p` readable? The kernel reports EFAULT for a bad source address instead of
// faulting. (Windows: VirtualQuery.)
bool HostReadable(const void* p) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(p, &info, sizeof info) == 0) {
        return false;
    }
    return info.State == MEM_COMMIT && (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) != 0;
#else
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    const bool ok = write(fds[1], p, 1) == 1;
    close(fds[0]);
    close(fds[1]);
    return ok;
#endif
}

bool ReadableAt(const GuestMemory& memory, u64 guest_addr) {
    return HostReadable(memory.Base() + guest_addr);
}

struct ChildResult {
    int exit_code = -1;  // -1 if it did not exit normally
    int signal = 0;
    std::string output;  // stdout and stderr
};

ChildResult RunSelf(const std::string& arguments) {
    ChildResult result;
#if defined(_WIN32)
    const std::string command = "\"" + g_self_path + "\" " + arguments + " 2>&1";
    FILE* pipe = _popen(command.c_str(), "r");
#else
    const std::string command = "\"" + g_self_path + "\" " + arguments + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
#endif
    REQUIRE(pipe != nullptr);
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
        result.output.append(buffer, n);
    }
#if defined(_WIN32)
    result.exit_code = _pclose(pipe);
#else
    const int status = pclose(pipe);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        if (result.exit_code > 128) { // the shell reports a signal as 128 + n
            result.signal = result.exit_code - 128;
        }
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    }
#endif
    return result;
}

#if defined(__linux__)
long RssKiB() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) {
        return -1;
    }
    char line[256];
    long value = -1;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::sscanf(line, "VmRSS: %ld kB", &value) == 1) {
            break;
        }
    }
    std::fclose(f);
    return value;
}
#endif

} // namespace

// ---- the reservation ------------------------------------------------------------------------------

TEST_CASE("Create reserves the address space without committing it") {
#if defined(__linux__)
    const long before = RssKiB();
#endif
    auto memory = MakeMemory();
    CHECK(memory->Base() != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(memory->Base()) % 4096 == 0);
    // everything is free but the two fixed regions
    CHECK(memory->Query(0) == MemoryInfo{0, 0x1F000000, MemPerm::None, MemState::Free});
    CHECK(memory->Query(0x40000000 - 1).has_value());
    CHECK_FALSE(memory->Query(0x40000000).has_value());
    CHECK_FALSE(memory->Query(0xFFFFFFFF).has_value());
#if defined(__linux__)
    // 6.5 MiB of VRAM and DSP RAM are mapped, none of it touched; the 4 GiB are not resident
    CHECK(RssKiB() - before < 32 * 1024);
#endif
    // the page after the 4 GiB is the guard: not accessible
    CHECK_FALSE(HostReadable(memory->Base() + kAddressSpaceSize));
    CHECK_FALSE(HostReadable(memory->Base() + kAddressSpaceSize + kGuardSize - 1));
}

TEST_CASE("two guest memories coexist and are independent") {
    auto a = MakeMemory();
    auto b = MakeMemory();
    CHECK(a->Base() != b->Base());
    REQUIRE(a->Map(0x08000000, 0x1000, RW, MemState::Private) == 0);
    CHECK(a->IsMapped(0x08000000, 1));
    CHECK_FALSE(b->IsMapped(0x08000000, 1));
}

TEST_CASE("VRAM and DSP RAM are mapped at creation") {
    auto memory = MakeMemory();
    // Azahar's special mapping: state IO, read-write; free space on both sides
    CHECK(memory->Query(0x1F000000) == MemoryInfo{0x1F000000, 0x600000, RW, MemState::IO});
    CHECK(memory->Query(0x1F5FFFFF) == MemoryInfo{0x1F000000, 0x600000, RW, MemState::IO});
    CHECK(memory->Query(0x1F600000) == MemoryInfo{0x1F600000, 0x900000, MemPerm::None, MemState::Free});
    CHECK(memory->Query(0x1FF00000) == MemoryInfo{0x1FF00000, 0x80000, RW, MemState::IO});
    CHECK(memory->Query(0x1FF80000) ==
          MemoryInfo{0x1FF80000, 0x40000000 - 0x1FF80000, MemPerm::None, MemState::Free});
    CHECK(memory->IsMapped(0x1F000000, 0x600000));
    CHECK_FALSE(memory->IsMapped(0x1F000000, 0x600001));
    u8* vram = memory->Base() + 0x1F000000;
    vram[0] = 1;
    vram[0x5FFFFF] = 2;
    CHECK(vram[0] == 1);
    CHECK(HostReadable(memory->Base() + 0x1FF7FFFF));
    CHECK_FALSE(HostReadable(memory->Base() + 0x1FF80000));
}

// ---- Map, Unmap, Protect, Query ---------------------------------------------------------------------

TEST_CASE("blocks merge when adjacent and alike, and split again on Protect and Unmap") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    REQUIRE(memory->Map(A, 0x2000, RW, MemState::Private) == 0);
    CHECK(memory->Query(A) == MemoryInfo{A, 0x2000, RW, MemState::Private});
    // the free neighbours: [0, A) below, [A + 0x3000, VRAM) above once the next page is mapped
    CHECK(memory->Query(A - 1) == MemoryInfo{0, A, MemPerm::None, MemState::Free});

    // an adjacent block with the same permission and state merges (MergeAdjacent)
    REQUIRE(memory->Map(A + 0x2000, 0x1000, RW, MemState::Private) == 0);
    CHECK(memory->Query(A + 0x2000) == MemoryInfo{A, 0x3000, RW, MemState::Private});
    CHECK(memory->Query(A + 0x3000) ==
          MemoryInfo{A + 0x3000, 0x1F000000 - (A + 0x3000), MemPerm::None, MemState::Free});

    // Protect in the middle splits the block in three (CarveVMARange)
    REQUIRE(memory->Protect(A + 0x1000, 0x1000, R) == 0);
    CHECK(memory->Query(A) == MemoryInfo{A, 0x1000, RW, MemState::Private});
    CHECK(memory->Query(A + 0x1FFF) == MemoryInfo{A + 0x1000, 0x1000, R, MemState::Private});
    CHECK(memory->Query(A + 0x2000) == MemoryInfo{A + 0x2000, 0x1000, RW, MemState::Private});
    // and Protect back merges them
    REQUIRE(memory->Protect(A + 0x1000, 0x1000, RW) == 0);
    CHECK(memory->Query(A + 0x1000) == MemoryInfo{A, 0x3000, RW, MemState::Private});

    // Unmap in the middle leaves two blocks and a hole
    REQUIRE(memory->Unmap(A + 0x1000, 0x1000) == 0);
    CHECK(memory->Query(A) == MemoryInfo{A, 0x1000, RW, MemState::Private});
    CHECK(memory->Query(A + 0x1000) ==
          MemoryInfo{A + 0x1000, 0x1000, MemPerm::None, MemState::Free});
    CHECK(memory->Query(A + 0x2000) == MemoryInfo{A + 0x2000, 0x1000, RW, MemState::Private});
    // freeing the first merges its space with the free block below it (Unmap -> MergeAdjacent)
    REQUIRE(memory->Unmap(A, 0x1000) == 0);
    CHECK(memory->Query(0) ==
          MemoryInfo{0, A + 0x2000, MemPerm::None, MemState::Free});
    // freeing the last one gives the original single free block back
    REQUIRE(memory->Unmap(A + 0x2000, 0x1000) == 0);
    CHECK(memory->Query(0) == MemoryInfo{0, 0x1F000000, MemPerm::None, MemState::Free});
}

TEST_CASE("blocks that differ in permission or state do not merge; one Unmap can span blocks") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    REQUIRE(memory->Map(A, 0x1000, RW, MemState::Private) == 0);
    REQUIRE(memory->Map(A + 0x1000, 0x1000, R, MemState::Private) == 0);
    REQUIRE(memory->Map(A + 0x2000, 0x1000, R, MemState::Shared) == 0);
    CHECK(memory->Query(A)->size == 0x1000);
    CHECK(memory->Query(A + 0x1000)->size == 0x1000);
    CHECK(memory->Query(A + 0x2000) == MemoryInfo{A + 0x2000, 0x1000, R, MemState::Shared});
    // one Unmap over all three
    REQUIRE(memory->Unmap(A, 0x3000) == 0);
    CHECK(memory->Query(A) == MemoryInfo{0, 0x1F000000, MemPerm::None, MemState::Free});
}

TEST_CASE("an Unmap in the middle of a block splits it into two") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    REQUIRE(memory->Map(A, 0x4000, RW, MemState::Private) == 0);
    REQUIRE(memory->Unmap(A + 0x1000, 0x2000) == 0);
    CHECK(memory->Query(A) == MemoryInfo{A, 0x1000, RW, MemState::Private});
    CHECK(memory->Query(A + 0x1000) ==
          MemoryInfo{A + 0x1000, 0x2000, MemPerm::None, MemState::Free});
    CHECK(memory->Query(A + 0x3000) == MemoryInfo{A + 0x3000, 0x1000, RW, MemState::Private});
}

TEST_CASE("blocks with a backing merge only when the backings are contiguous") {
    auto memory = MakeMemory();
    const u32 A = 0x09000000;
    // ControlMemory's heap: two allocations take FCRAM downwards but are mapped upwards, so
    // their host memory is not contiguous and Azahar's VMAs stay apart
    REQUIRE(memory->Map(A, 0x1000, RW, MemState::Private, 0x20100000) == 0);
    REQUIRE(memory->Map(A + 0x1000, 0x1000, RW, MemState::Private, 0x200FF000) == 0);
    CHECK(memory->Query(A) == MemoryInfo{A, 0x1000, RW, MemState::Private});
    CHECK(memory->Query(A + 0x1000) == MemoryInfo{A + 0x1000, 0x1000, RW, MemState::Private});
    // a third one that continues the second one's backing merges with it, not with the first
    REQUIRE(memory->Map(A + 0x2000, 0x1000, RW, MemState::Private, 0x20100000) == 0);
    CHECK(memory->Query(A + 0x2000) == MemoryInfo{A + 0x1000, 0x2000, RW, MemState::Private});
    CHECK(memory->Query(A)->size == 0x1000);
    // a block without a backing does not merge with one that has one
    REQUIRE(memory->Map(A + 0x3000, 0x1000, RW, MemState::Private) == 0);
    CHECK(memory->Query(A + 0x3000) == MemoryInfo{A + 0x3000, 0x1000, RW, MemState::Private});
    // splitting a block with a backing and merging the parts again works (the halves continue
    // each other)
    const u32 B = 0x0A000000;
    REQUIRE(memory->Map(B, 0x3000, RW, MemState::Continuous, 0x20200000) == 0);
    REQUIRE(memory->Protect(B + 0x1000, 0x1000, R) == 0);
    CHECK(memory->Query(B + 0x1000)->size == 0x1000);
    REQUIRE(memory->Protect(B + 0x1000, 0x1000, RW) == 0);
    CHECK(memory->Query(B) == MemoryInfo{B, 0x3000, RW, MemState::Continuous});
    // linear allocations of consecutive FCRAM merge (first fit from the bottom gives
    // consecutive offsets at consecutive addresses)
    const u32 L = 0x14000000;
    REQUIRE(memory->Map(L, 0x4000, RW, MemState::Continuous, 0x20000000) == 0);
    REQUIRE(memory->Map(L + 0x4000, 0x2000, RW, MemState::Continuous, 0x20004000) == 0);
    CHECK(memory->Query(L) == MemoryInfo{L, 0x6000, RW, MemState::Continuous});
}

TEST_CASE("Map, Unmap and Protect report Azahar's errors and change nothing") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    CHECK(kResultInvalidAddress == 0xE0E01BF5);
    CHECK(kResultInvalidAddressState == 0xE0A01BF5);
    // misaligned, empty, outside the managed range: invalid address
    CHECK(memory->Map(A + 0x800, 0x1000, RW, MemState::Private) == kResultInvalidAddress);
    CHECK(memory->Map(A, 0x800, RW, MemState::Private) == kResultInvalidAddress);
    CHECK(memory->Map(A, 0, RW, MemState::Private) == kResultInvalidAddress);
    CHECK(memory->Map(0x40000000, 0x1000, RW, MemState::Private) == kResultInvalidAddress);
    CHECK(memory->Map(0xFFFFF000, 0x1000, RW, MemState::Private) == kResultInvalidAddress);
    // starts in the managed range but does not fit: invalid address state
    CHECK(memory->Map(0x3FFFF000, 0x2000, RW, MemState::Private) == kResultInvalidAddressState);
    CHECK(memory->Query(0x3FFFF000).has_value());
    CHECK(memory->Query(0x3FFFF000)->state == MemState::Free);

    REQUIRE(memory->Map(A, 0x2000, RW, MemState::Private) == 0);
    // mapped already, in whole or in part
    CHECK(memory->Map(A, 0x1000, RW, MemState::Private) == kResultInvalidAddressState);
    CHECK(memory->Map(A + 0x1000, 0x2000, RW, MemState::Private) == kResultInvalidAddressState);
    CHECK(memory->Map(A - 0x1000, 0x2000, RW, MemState::Private) == kResultInvalidAddressState);
    // a range with a hole cannot be unmapped or protected
    CHECK(memory->Unmap(A, 0x3000) == kResultInvalidAddressState);
    CHECK(memory->Unmap(A - 0x1000, 0x2000) == kResultInvalidAddressState);
    CHECK(memory->Protect(A, 0x3000, R) == kResultInvalidAddressState);
    CHECK(memory->Unmap(A + 0x4000, 0x1000) == kResultInvalidAddressState);
    CHECK(memory->Unmap(A + 0x800, 0x1000) == kResultInvalidAddress);
    CHECK(memory->Unmap(0x40000000, 0x1000) == kResultInvalidAddress);
    // nothing changed
    CHECK(memory->Query(A) == MemoryInfo{A, 0x2000, RW, MemState::Private});
    CHECK(memory->Query(A + 0x2000)->state == MemState::Free);
    CHECK(memory->IsMapped(A, 0x2000));
}

TEST_CASE("ChangeState checks the expected state and permission first") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    REQUIRE(memory->Map(A, 0x3000, RW, MemState::Private) == 0);
    // The source of a Map becomes Aliased, as in Process::Map (Azahar leaves a non-privileged
    // source read-write; read-only here exercises ChangeState's permission rules below)
    REQUIRE(memory->ChangeState(A + 0x1000, 0x1000, MemState::Private, RW, MemState::Aliased, R) ==
            0);
    CHECK(memory->Query(A + 0x1000) == MemoryInfo{A + 0x1000, 0x1000, R, MemState::Aliased});
    CHECK(memory->Query(A)->size == 0x1000);
    // wrong state
    CHECK(memory->ChangeState(A, 0x2000, MemState::Private, RW, MemState::Shared, RW) ==
          kResultInvalidAddressState);
    // the block needs at least the expected permissions: Write on a read-only block fails
    CHECK(memory->ChangeState(A + 0x1000, 0x1000, MemState::Aliased, RW, MemState::Private, RW) ==
          kResultInvalidAddressState);
    CHECK(memory->ChangeState(A + 0x1000, 0x1000, MemState::Aliased, R, MemState::Private, RW) == 0);
    CHECK(memory->Query(A) == MemoryInfo{A, 0x3000, RW, MemState::Private}); // merged back
    // a free range is not in the expected state
    CHECK(memory->ChangeState(A + 0x3000, 0x1000, MemState::Private, RW, MemState::Shared, RW) ==
          kResultInvalidAddressState);
}

TEST_CASE("Unmap gives the pages back and a new mapping is zero-filled") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    REQUIRE(memory->Map(A, 0x3000, RW, MemState::Private) == 0);
    std::memset(memory->Base() + A, 0xAB, 0x3000);
    REQUIRE(memory->Unmap(A + 0x1000, 0x1000) == 0);
    CHECK(ReadableAt(*memory, A));
    CHECK_FALSE(ReadableAt(*memory, A + 0x1000));
    CHECK_FALSE(ReadableAt(*memory, A + 0x1FFF));
    CHECK(ReadableAt(*memory, A + 0x2000));
    CHECK(memory->Base()[A + 0x2000] == 0xAB); // the neighbours keep their contents
    REQUIRE(memory->Map(A + 0x1000, 0x1000, RW, MemState::Private) == 0);
    for (u32 i = 0; i < 0x1000; ++i) {
        REQUIRE(memory->Base()[A + 0x1000 + i] == 0);
    }
    CHECK(memory->Base()[A] == 0xAB);
}

#if defined(__linux__)
TEST_CASE("Unmap returns memory to the OS") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    const u32 size = 64 * 1024 * 1024;
    const long before = RssKiB();
    REQUIRE(memory->Map(A, size, RW, MemState::Private) == 0);
    std::memset(memory->Base() + A, 1, size);
    const long touched = RssKiB();
    CHECK(touched - before >= 60 * 1024);
    REQUIRE(memory->Unmap(A, size) == 0);
    CHECK(touched - RssKiB() >= 60 * 1024);
}
#endif

TEST_CASE("host pages larger than guest pages (Apple silicon's 16 KiB) share and zero their pages") {
    GuestMemoryOptions options;
    options.zero_policy = ZeroPolicy::ZeroFill;
    options.zero_regions = {{0x05000000, 0x05000FFF}}; // one guest page in a 16 KiB host page
    options.host_page_size = 0x4000;
    auto memory = MakeMemory(options);
    const u32 A = 0x08000000; // host-page aligned
    u8* base = memory->Base();

    // two guest pages at the start of a host page: the whole host page is accessible
    REQUIRE(memory->Map(A, 0x2000, RW, MemState::Private) == 0);
    CHECK(ReadableAt(*memory, A + 0x3FFF));
    CHECK_FALSE(ReadableAt(*memory, A + 0x4000));
    // guest bookkeeping still works in 4 KiB steps
    CHECK(memory->Query(A + 0x2000) ==
          MemoryInfo{A + 0x2000, 0x1F000000 - (A + 0x2000), MemPerm::None, MemState::Free});
    std::memset(base + A, 0xAB, 0x2000);
    REQUIRE(memory->Map(A + 0x2000, 0x1000, R, MemState::Shared) == 0);
    std::memset(base + A + 0x2000, 0xCD, 0x1000);

    // freeing one guest page keeps the host page (its neighbours are mapped) and zeroes the page
    REQUIRE(memory->Unmap(A + 0x1000, 0x1000) == 0);
    CHECK(ReadableAt(*memory, A + 0x1000));
    CHECK(base[A] == 0xAB);
    CHECK(base[A + 0x2000] == 0xCD);
    for (u32 i = 0; i < 0x1000; ++i) {
        REQUIRE(base[A + 0x1000 + i] == 0);
    }
    // a stray write into the free guest page of a shared host page does not survive a new mapping
    base[A + 0x1000] = 0x55;
    REQUIRE(memory->Unmap(A + 0x2000, 0x1000) == 0);
    REQUIRE(memory->Map(A + 0x1000, 0x2000, RW, MemState::Private) == 0);
    CHECK(base[A + 0x1000] == 0);
    CHECK(base[A + 0x2000] == 0);
    CHECK(base[A] == 0xAB);
    REQUIRE(memory->Unmap(A + 0x1000, 0x2000) == 0);
    REQUIRE(memory->Map(A + 0x1000, 0x1000, RW, MemState::Private) == 0);

    // the last mapped guest page of the host page goes: the host page goes back to the OS
    REQUIRE(memory->Unmap(A, 0x2000) == 0);
    CHECK_FALSE(ReadableAt(*memory, A));
    CHECK_FALSE(ReadableAt(*memory, A + 0x3FFF));

    // a range that straddles two host pages
    REQUIRE(memory->Map(A + 0x3000, 0x2000, RW, MemState::Private) == 0);
    CHECK(ReadableAt(*memory, A));
    CHECK(ReadableAt(*memory, A + 0x7FFF));
    CHECK_FALSE(ReadableAt(*memory, A + 0x8000));
    std::memset(base + A + 0x3000, 0xEE, 0x2000);
    REQUIRE(memory->Unmap(A + 0x3000, 0x1000) == 0); // the first host page is free now
    CHECK_FALSE(ReadableAt(*memory, A));
    CHECK(ReadableAt(*memory, A + 0x4000)); // the second keeps its mapped page
    CHECK(base[A + 0x4000] == 0xEE);
    REQUIRE(memory->Unmap(A + 0x4000, 0x1000) == 0);
    CHECK_FALSE(ReadableAt(*memory, A + 0x4000));

    // a range inside one host page with the rest of it free: nothing else is mapped there
    REQUIRE(memory->Map(A + 0x9000, 0x1000, RW, MemState::Private) == 0);
    base[A + 0x9000] = 1;
    REQUIRE(memory->Unmap(A + 0x9000, 0x1000) == 0);
    CHECK_FALSE(ReadableAt(*memory, A + 0x8000));
    CHECK_FALSE(ReadableAt(*memory, A + 0x9000));

    // a zero region keeps its host page accessible, and zeroed, whatever happens around it
    CHECK(ReadableAt(*memory, 0x05000000));
    CHECK(ReadableAt(*memory, 0x05003FFF)); // same host page
    CHECK_FALSE(ReadableAt(*memory, 0x05004000));
    // a write into the zero region is kept, but a mapping over it starts zero-filled
    base[0x05000100] = 9;
    REQUIRE(memory->Map(0x05000000, 0x1000, RW, MemState::Private) == 0);
    CHECK(base[0x05000100] == 0);
    REQUIRE(memory->Unmap(0x05000000, 0x1000) == 0);
    REQUIRE(memory->Map(0x05001000, 0x1000, RW, MemState::Private) == 0);
    base[0x05001000] = 3;
    REQUIRE(memory->Unmap(0x05001000, 0x1000) == 0);
    CHECK(ReadableAt(*memory, 0x05001000));
    CHECK(base[0x05001000] == 0);
}

TEST_CASE("host pages larger than guest pages: a new mapping inside one host page is zero-filled") {
    GuestMemoryOptions options;
    options.zero_policy = ZeroPolicy::ZeroFill;
    options.zero_regions = {{0x05000000, 0x05000FFF}};
    options.host_page_size = 0x4000;
    auto memory = MakeMemory(options);
    u8* base = memory->Base();
    const u32 A = 0x08000000; // host-page aligned

    // the second half of a host page is mapped, a stray write hits the free first half (the host
    // page is accessible because of its neighbour), and a mapping of the first half, which starts
    // on the host page boundary and ends inside the same host page, must read zero
    REQUIRE(memory->Map(A + 0x2000, 0x2000, RW, MemState::Private) == 0);
    std::memset(base + A + 0x2000, 0x33, 0x2000);
    base[A + 0x100] = 0x77;
    base[A + 0x1FFF] = 0x78;
    REQUIRE(memory->Map(A, 0x2000, RW, MemState::Private) == 0);
    for (u32 i = 0; i < 0x2000; ++i) {
        REQUIRE(base[A + i] == 0);
    }
    for (u32 i = 0; i < 0x2000; ++i) {
        REQUIRE(base[A + 0x2000 + i] == 0x33); // the neighbour keeps its contents
    }

    // the mirror image: the first half is mapped, the stray write hits the second half
    const u32 B = A + 0x10000;
    REQUIRE(memory->Map(B, 0x2000, RW, MemState::Private) == 0);
    std::memset(base + B, 0x44, 0x2000);
    base[B + 0x2000] = 0x77;
    base[B + 0x3FFF] = 0x78;
    REQUIRE(memory->Map(B + 0x2000, 0x2000, RW, MemState::Private) == 0);
    for (u32 i = 0; i < 0x2000; ++i) {
        REQUIRE(base[B + 0x2000 + i] == 0);
        REQUIRE(base[B + i] == 0x44);
    }

    // one guest page in the middle of a host page, with its neighbours mapped
    const u32 C = A + 0x20000;
    REQUIRE(memory->Map(C, 0x1000, RW, MemState::Private) == 0);
    REQUIRE(memory->Map(C + 0x2000, 0x2000, RW, MemState::Private) == 0);
    base[C + 0x1000] = 0x77;
    REQUIRE(memory->Map(C + 0x1000, 0x1000, RW, MemState::Private) == 0);
    CHECK(base[C + 0x1000] == 0);
}

TEST_CASE("host pages larger than guest pages: a zero region off the host page grid is cleared by Map") {
    GuestMemoryOptions options;
    options.zero_policy = ZeroPolicy::ZeroFill;
    options.zero_regions = {{0x08005000, 0x08007FFF}}; // starts in the middle of a host page
    options.host_page_size = 0x4000;
    auto memory = MakeMemory(options);
    u8* base = memory->Base();

    // the region ends at 0x08007FFF, so only the host page 0x08004000-0x08007FFF is accessible,
    // and a stray write into its first guest page (outside the region, in the same host page) is
    // kept
    CHECK(ReadableAt(*memory, 0x08004000));
    CHECK(ReadableAt(*memory, 0x08007FFF));
    CHECK_FALSE(ReadableAt(*memory, 0x08008000));
    CHECK_FALSE(ReadableAt(*memory, 0x08003FFF));
    base[0x08004000] = 0xA5;
    base[0x08006000] = 0xA6; // inside the region
    base[0x08007FFF] = 0xA7;

    // a mapping that covers the whole host page, and more, starts zero-filled
    REQUIRE(memory->Map(0x08004000, 0x5000, RW, MemState::Private) == 0);
    for (u32 i = 0; i < 0x5000; ++i) {
        REQUIRE(base[0x08004000 + i] == 0);
    }

    // the same with a stray write in the part of the host page outside the region, after the
    // range was freed again: the host page stays accessible because of the region
    base[0x08004000] = 0xA5;
    REQUIRE(memory->Unmap(0x08004000, 0x5000) == 0);
    CHECK(ReadableAt(*memory, 0x08004000));
    CHECK(ReadableAt(*memory, 0x08007FFF));
    CHECK_FALSE(ReadableAt(*memory, 0x08008000));
    base[0x08004000] = 0xA5;
    REQUIRE(memory->Map(0x08004000, 0x1000, RW, MemState::Private) == 0);
    CHECK(base[0x08004000] == 0);
}

TEST_CASE("an unusable host page size is refused") {
    GuestMemoryOptions options;
    options.host_page_size = 3000;
    CHECK(GuestMemory::Create(options) == nullptr);
    options.host_page_size = 0x100; // smaller than the host's
    CHECK(GuestMemory::Create(options) == nullptr);
}

// ---- zero regions -------------------------------------------------------------------------------

TEST_CASE("the embedded zero regions come from config/native/zero_regions.txt") {
    const auto& regions = DefaultZeroRegions();
    REQUIRE_FALSE(regions.empty());
    // the first entry of the file: 0x00000000 0x000fffff
    CHECK(regions[0] == ZeroRegion{0x00000000, 0x000fffff});
#if MH4N_DEBUG_GUARD
    CHECK(DefaultZeroPolicy() == ZeroPolicy::Fault);
#else
    CHECK(DefaultZeroPolicy() == ZeroPolicy::ZeroFill);
#endif
}

TEST_CASE("zero regions under the fill policy: reads give zero, writes are kept, nothing is mapped") {
    GuestMemoryOptions options;
    options.zero_policy = ZeroPolicy::ZeroFill;
    auto memory = MakeMemory(options);
    CHECK(memory->GetZeroPolicy() == ZeroPolicy::ZeroFill);
    CHECK(ReadableAt(*memory, 0));
    CHECK(ReadableAt(*memory, 0xFFFFF));
    CHECK_FALSE(ReadableAt(*memory, 0x100000));
    CHECK(memory->Base()[0] == 0);
    CHECK(memory->Base()[0xFFFFF] == 0);
    memory->Base()[0x10] = 7;
    CHECK(memory->Base()[0x10] == 7);
    // for the kernel the range is free, like in Azahar
    CHECK(memory->Query(0x10)->state == MemState::Free);
    CHECK_FALSE(memory->IsMapped(0x10, 4));
    CHECK_FALSE(memory->Read<u32>(0x10).has_value());
}

TEST_CASE("zero regions under the fault policy stay inaccessible") {
    GuestMemoryOptions options;
    options.zero_policy = ZeroPolicy::Fault;
    auto memory = MakeMemory(options);
    CHECK(memory->GetZeroPolicy() == ZeroPolicy::Fault);
    CHECK_FALSE(ReadableAt(*memory, 0));
    CHECK_FALSE(ReadableAt(*memory, 0x10));
    CHECK_FALSE(ReadableAt(*memory, 0xFFFFF));
}

TEST_CASE("a custom zero region survives a mapping being removed over it") {
    GuestMemoryOptions options;
    options.zero_policy = ZeroPolicy::ZeroFill;
    options.zero_regions = {{0x05000000, 0x05001FFF}};
    auto memory = MakeMemory(options);
    CHECK(ReadableAt(*memory, 0x05000000));
    CHECK(ReadableAt(*memory, 0x05001FFF));
    CHECK_FALSE(ReadableAt(*memory, 0x05002000));
    CHECK_FALSE(ReadableAt(*memory, 0)); // the default regions are replaced, not added to
    memory->Base()[0x05000100] = 4; // kept, being in a zero region
    CHECK(memory->Base()[0x05000100] == 4);
    REQUIRE(memory->Map(0x05000000, 0x3000, RW, MemState::Private) == 0);
    CHECK(memory->Base()[0x05000100] == 0); // the mapping starts zero-filled
    memory->Base()[0x05000100] = 9;
    memory->Base()[0x05002100] = 9;
    REQUIRE(memory->Unmap(0x05000000, 0x3000) == 0);
    CHECK(ReadableAt(*memory, 0x05000100)); // back to a zero region
    CHECK(memory->Base()[0x05000100] == 0);
    CHECK_FALSE(ReadableAt(*memory, 0x05002100));
}

TEST_CASE("a zero region that is not page aligned is refused") {
    GuestMemoryOptions options;
    options.zero_regions = {{0x1000, 0x1800}};
    CHECK(GuestMemory::Create(options) == nullptr);
    options.zero_regions = {{0x2000, 0x1000}};
    CHECK(GuestMemory::Create(options) == nullptr);
}

// ---- the physical view ----------------------------------------------------------------------------

TEST_CASE("PhysicalPointer: VRAM, DSP RAM and FCRAM through the linear heap") {
    auto memory = MakeMemory();
    u8* base = memory->Base();

    // VRAM 0x18000000-0x18600000 is at guest 0x1F000000
    CHECK(memory->PhysicalPointer(0x18000000, 0x600000) == base + 0x1F000000);
    CHECK(memory->PhysicalPointer(0x18000100, 16) == base + 0x1F000100);
    CHECK(memory->PhysicalPointer(0x185FFFFF, 1) == base + 0x1F5FFFFF);
    CHECK(memory->PhysicalPointer(0x185FFFFF, 2) == nullptr);
    CHECK(memory->PhysicalPointer(0x18600000, 1) == nullptr);
    CHECK(memory->PhysicalPointer(0x17FFFFFF, 1) == nullptr);
    CHECK(memory->PhysicalPointer(0x18000000, 0x600001) == nullptr);
    CHECK(memory->PhysicalPointer(0x18000000, 0) == base + 0x1F000000); // size 0: the byte at paddr
    CHECK(memory->PhysicalPointer(0x18600000, 0) == nullptr);           // so an area's end is null

    // DSP RAM 0x1FF00000-0x1FF80000 at the same guest address
    CHECK(memory->PhysicalPointer(0x1FF00000, 0x80000) == base + 0x1FF00000);
    CHECK(memory->PhysicalPointer(0x1FF7FFFF, 1) == base + 0x1FF7FFFF);
    CHECK(memory->PhysicalPointer(0x1FF7FFFF, 2) == nullptr);
    CHECK(memory->PhysicalPointer(0x1FF80000, 1) == nullptr);
    CHECK(memory->PhysicalPointer(0x1FF80000, 0) == nullptr); // size 0 is the byte at paddr too

    // between the areas
    CHECK(memory->PhysicalPointer(0x19000000, 4) == nullptr);
    CHECK(memory->PhysicalPointer(0x1FF80000, 4) == nullptr);

    // FCRAM has no view until the linear heap is set
    REQUIRE(memory->Map(0x14100000, 0x2000, RW, MemState::Continuous, 0x20100000) == 0);
    CHECK(memory->LinearHeapArea() == 0);
    CHECK(memory->PhysicalPointer(0x20100000, 0x2000) == nullptr);
    memory->SetLinearHeapArea(kLinearHeapVaddr);
    CHECK(memory->LinearHeapArea() == 0x14000000);
    // vaddr = paddr - 0x20000000 + 0x14000000
    CHECK(memory->PhysicalPointer(0x20100000, 0x2000) == base + 0x14100000);
    CHECK(memory->PhysicalPointer(0x20101000, 16) == base + 0x14101000);
    // not completely mapped, or not mapped at all
    CHECK(memory->PhysicalPointer(0x20100000, 0x2001) == nullptr);
    CHECK(memory->PhysicalPointer(0x200FFFFF, 2) == nullptr);
    CHECK(memory->PhysicalPointer(0x20200000, 4) == nullptr);
    // the end of FCRAM, and past it
    CHECK(memory->PhysicalPointer(0x27FFFFFF, 1) == nullptr); // inside, but not mapped
    CHECK(memory->PhysicalPointer(0x28000000, 1) == nullptr);
    CHECK(memory->PhysicalPointer(0x1FFFFFFF, 1) == nullptr);

    // the New 3DS kernel's linear heap
    memory->SetLinearHeapArea(kNewLinearHeapVaddr);
    CHECK(memory->PhysicalPointer(0x20100000, 0x2000) == nullptr); // nothing mapped there
    REQUIRE(memory->Map(0x30000000, 0x1000, RW, MemState::Continuous, 0x20000000) == 0);
    CHECK(memory->PhysicalPointer(0x20000000, 0x1000) == base + 0x30000000);

    // after an Unmap the view is gone
    REQUIRE(memory->Unmap(0x30000000, 0x1000) == 0);
    CHECK(memory->PhysicalPointer(0x20000000, 0x1000) == nullptr);
}

TEST_CASE("VirtualToPhysical for VRAM, the linear heaps and DSP RAM") {
    auto memory = MakeMemory();
    CHECK(memory->VirtualToPhysical(0x1F000000) == 0x18000000u);
    CHECK(memory->VirtualToPhysical(0x1F012345) == 0x18012345u);
    CHECK(memory->VirtualToPhysical(0x1F5FFFFF) == 0x185FFFFFu);
    CHECK(memory->VirtualToPhysical(0x1F600000) == 0x18600000u); // the end of the area is accepted
    CHECK_FALSE(memory->VirtualToPhysical(0x1F600001).has_value());
    CHECK(memory->VirtualToPhysical(0x14000000) == 0x20000000u);
    CHECK(memory->VirtualToPhysical(0x14123456) == 0x20123456u);
    CHECK(memory->VirtualToPhysical(0x1C000000) == 0x28000000u);
    CHECK_FALSE(memory->VirtualToPhysical(0x1C000001).has_value());
    CHECK(memory->VirtualToPhysical(0x30000000) == 0x20000000u);
    CHECK(memory->VirtualToPhysical(0x37FFFFFF) == 0x27FFFFFFu);
    CHECK(memory->VirtualToPhysical(0x40000000) == 0x30000000u);
    CHECK(memory->VirtualToPhysical(0x1FF00000) == 0x1FF00000u);
    CHECK(memory->VirtualToPhysical(0x1FF7FFFF) == 0x1FF7FFFFu);
    CHECK(memory->VirtualToPhysical(0x1FF80000) == 0x1FF80000u); // the end of DSP RAM
    CHECK_FALSE(memory->VirtualToPhysical(0x1FF81000).has_value());
    CHECK_FALSE(memory->VirtualToPhysical(0x08000000).has_value());
    CHECK_FALSE(memory->VirtualToPhysical(0).has_value());
    // round trip with PhysicalPointer
    memory->SetLinearHeapArea(kLinearHeapVaddr);
    REQUIRE(memory->Map(0x14000000, 0x1000, RW, MemState::Continuous, 0x20000000) == 0);
    const auto paddr = memory->VirtualToPhysical(0x14000800);
    REQUIRE(paddr.has_value());
    CHECK(memory->PhysicalPointer(*paddr, 16) == memory->Base() + 0x14000800);
}

// ---- helpers --------------------------------------------------------------------------------------

TEST_CASE("Read, Write and the block helpers check the range instead of faulting") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    REQUIRE(memory->Map(A, 0x1000, RW, MemState::Private) == 0);
    REQUIRE(memory->Map(A + 0x1000, 0x1000, R, MemState::Shared) == 0); // a second block

    CHECK(memory->Write<u32>(A, 0xDEADBEEF));
    CHECK(memory->Read<u32>(A) == 0xDEADBEEFu);
    CHECK(memory->Read<u8>(A) == 0xEFu); // little endian, as the guest
    CHECK(memory->Read<u16>(A + 1) == 0xADBEu); // unaligned is fine
    CHECK(memory->Write<u64>(A + 0xFFC, 0x1122334455667788ull)); // across the blocks
    CHECK(memory->Read<u64>(A + 0xFFC) == 0x1122334455667788ull);
    CHECK(memory->Read<u32>(A + 0x1000) == 0x11223344u);

    // out of range: an error, and a write touches nothing
    CHECK_FALSE(memory->Read<u32>(A + 0x1FFE).has_value());
    CHECK_FALSE(memory->Read<u32>(A - 2).has_value());
    CHECK_FALSE(memory->Read<u32>(0xFFFFFFFE).has_value());
    CHECK_FALSE(memory->Read<u32>(0).has_value());
    const u8 bytes[4] = {1, 2, 3, 4};
    CHECK_FALSE(memory->WriteBlock(A + 0x1FFE, bytes, 4));
    CHECK(memory->Base()[A + 0x1FFE] == 0);
    CHECK(memory->Base()[A + 0x1FFF] == 0);
    u8 out[4] = {9, 9, 9, 9};
    CHECK_FALSE(memory->ReadBlock(out, A + 0x1FFE, 4));
    CHECK(out[0] == 9);
    // an empty range is always fine
    CHECK(memory->ReadBlock(out, 0x12345678, 0));
    CHECK(memory->IsMapped(0x12345678, 0));
    CHECK(memory->IsMapped(A, 0x2000));
    CHECK_FALSE(memory->IsMapped(A, 0x2001));
    CHECK_FALSE(memory->IsMapped(0xFFFFF000, 0x2000)); // wraps
    CHECK_FALSE(memory->IsMapped(0x3FFFF000, 0x2000)); // beyond the managed range
}

TEST_CASE("ReadString stops at NUL or the limit and fails on unmapped memory") {
    auto memory = MakeMemory();
    const u32 A = 0x08000000;
    REQUIRE(memory->Map(A, 0x2000, RW, MemState::Private) == 0);
    const char text[] = "hello";
    REQUIRE(memory->WriteBlock(A + 0x100, text, 6));
    CHECK(memory->ReadString(A + 0x100, 100) == "hello");
    CHECK(memory->ReadString(A + 0x100, 3) == "hel");
    CHECK(memory->ReadString(A + 0x100, 5) == "hello"); // no NUL needed at the limit
    CHECK(memory->ReadString(A + 0x105, 10) == "");
    CHECK(memory->ReadString(A + 0x100, 0) == "");
    // runs into the end of the mapping before a NUL
    memory->WriteBlock(A + 0x1FFD, "abc", 3);
    CHECK_FALSE(memory->ReadString(A + 0x1FFD, 10).has_value());
    CHECK(memory->ReadString(A + 0x1FFD, 3) == "abc");
    CHECK_FALSE(memory->ReadString(0x20000, 4).has_value());
    CHECK_FALSE(memory->ReadString(0xFFFFFFFF, 4).has_value());
}

// ---- the FCRAM allocator --------------------------------------------------------------------------

namespace {
using Ranges = std::vector<FcramRange>;
constexpr FcramRegion kApp = FcramRegion::Application;
constexpr FcramRegion kSys = FcramRegion::System;
constexpr FcramRegion kBase = FcramRegion::Base;
} // namespace

TEST_CASE("FCRAM regions have Azahar's sizes for each memory mode") {
    struct Row {
        u32 mode;
        u32 sizes[3];
    };
    // Azahar's memory_region_sizes, written out; each region starts where the previous one ends
    const Row rows[] = {
        {0, {0x04000000, 0x02C00000, 0x01400000}}, // 64, 44, 20 MiB
        {2, {0x06000000, 0x00C00000, 0x01400000}}, // 96, 12, 20 MiB
        {3, {0x05000000, 0x01C00000, 0x01400000}},
        {4, {0x04800000, 0x02400000, 0x01400000}},
        {5, {0x02000000, 0x04C00000, 0x01400000}},
        {6, {0x07C00000, 0x06400000, 0x02000000}},
        {7, {0x0B200000, 0x02E00000, 0x02000000}},
    };
    for (const Row& row : rows) {
        CAPTURE(row.mode);
        REQUIRE(FcramRegions::ValidMemoryMode(row.mode));
        FcramRegions regions(row.mode);
        CHECK(regions.MemoryMode() == row.mode);
        CHECK(regions.Base(kApp) == 0);
        CHECK(regions.Size(kApp) == row.sizes[0]);
        CHECK(regions.Base(kSys) == row.sizes[0]);
        CHECK(regions.Size(kSys) == row.sizes[1]);
        CHECK(regions.Base(kBase) == row.sizes[0] + row.sizes[1]);
        CHECK(regions.Size(kBase) == row.sizes[2]);
        CHECK(row.sizes[0] + row.sizes[1] + row.sizes[2] == regions.TotalSize());
        CHECK(regions.TotalSize() == (row.mode >= 6 ? 0x10000000u : 0x08000000u));
        CHECK(regions.FreeBlocks(kApp) == Ranges{{0, row.sizes[0]}});
        CHECK(regions.Used(kApp) == 0);
    }
    CHECK_FALSE(FcramRegions::ValidMemoryMode(1)); // unused in Azahar
    CHECK_FALSE(FcramRegions::ValidMemoryMode(8));
}

TEST_CASE("FCRAM allocator, memory mode 0 (Prod), APPLICATION region 0x00000000-0x04000000") {
    FcramRegions regions(0);
    CHECK(regions.Base(kSys) == 0x04000000);
    CHECK(regions.Base(kBase) == 0x06C00000);

    // 1. HeapAllocate takes from the top of the free space
    CHECK(regions.HeapAllocate(kApp, 0x100000) == Ranges{{0x03F00000, 0x100000}});
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0, 0x03F00000}});
    CHECK(regions.Used(kApp) == 0x100000);
    // 2. and again, just below the first
    CHECK(regions.HeapAllocate(kApp, 0x200000) == Ranges{{0x03D00000, 0x200000}});
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0, 0x03D00000}});
    // 3. LinearAllocate(size) is the first fit from the bottom
    CHECK(regions.LinearAllocate(kApp, 0x80000) == 0u);
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x80000, 0x03C80000}});
    // 4.
    CHECK(regions.LinearAllocate(kApp, 0x1000) == 0x80000u);
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x81000, 0x03C7F000}});
    // 5. a given range, in the middle of the free block, splits it
    CHECK(regions.LinearAllocate(kApp, 0x100000, 0x1000));
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x81000, 0x7F000}, {0x101000, 0x03BFF000}});
    // 6. a range that is allocated, or spans an allocated page, is refused and changes nothing
    CHECK_FALSE(regions.LinearAllocate(kApp, 0x100000, 0x1000));
    CHECK_FALSE(regions.LinearAllocate(kApp, 0xFF000, 0x2000));
    CHECK_FALSE(regions.LinearAllocate(kApp, 0x03C00000, 0x200000)); // runs into the heap
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x81000, 0x7F000}, {0x101000, 0x03BFF000}});
    // 7. first fit skips the first block (0x7F000 < 0x80000)
    CHECK(regions.LinearAllocate(kApp, 0x80000) == 0x101000u);
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x81000, 0x7F000}, {0x181000, 0x03B7F000}});
    // 8. an exact fit uses the first block up
    CHECK(regions.LinearAllocate(kApp, 0x7F000) == 0x81000u);
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x181000, 0x03B7F000}});
    CHECK(regions.Used(kApp) == 0x481000);
    // 9. Free puts a range back
    CHECK(regions.Free(kApp, 0x80000, 0x1000));
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x80000, 0x1000}, {0x181000, 0x03B7F000}});
    // 10. and joins the neighbouring free ranges
    CHECK(regions.Free(kApp, 0x81000, 0x7F000));
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x80000, 0x80000}, {0x181000, 0x03B7F000}});
    CHECK(regions.Used(kApp) == 0x401000);
    // 11. freeing what is free, or outside the region, is refused
    CHECK_FALSE(regions.Free(kApp, 0x81000, 0x7F000));
    CHECK_FALSE(regions.Free(kApp, 0x7F000, 0x2000));
    CHECK_FALSE(regions.Free(kApp, 0x04000000, 0x1000));
    CHECK(regions.Used(kApp) == 0x401000);
    //    the first allocation (3.) freed in one go joins the free block that starts at 0x80000
    CHECK(regions.Free(kApp, 0, 0x80000));
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0, 0x100000}, {0x181000, 0x03B7F000}});
    CHECK(regions.Used(kApp) == 0x381000);
    // 12. HeapAllocate spans blocks: the whole top block, then the rest from the top of the next
    CHECK(regions.HeapAllocate(kApp, 0x03B80000) == Ranges{{0xFF000, 0x1000}, {0x181000, 0x03B7F000}});
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0, 0xFF000}});
    CHECK(regions.Used(kApp) == 0x03F01000);
    // 13. not enough space: empty, nothing changes
    CHECK(regions.HeapAllocate(kApp, 0x100000).empty());
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0, 0xFF000}});
    CHECK(regions.Used(kApp) == 0x03F01000);
    CHECK(regions.HeapAllocate(kApp, 0xFF000) == Ranges{{0, 0xFF000}});
    CHECK(regions.FreeBlocks(kApp).empty());
    CHECK(regions.Used(kApp) == 0x04000000);
    CHECK_FALSE(regions.LinearAllocate(kApp, 1).has_value());
    CHECK_FALSE(regions.RLinearAllocate(kApp, 1).has_value());
    CHECK_FALSE(regions.LinearAllocate(kApp, 0, 1));
    CHECK(regions.HeapAllocate(kApp, 1).empty());

    // the other regions were not touched
    CHECK(regions.FreeBlocks(kSys) == Ranges{{0x04000000, 0x02C00000}});
    CHECK(regions.FreeBlocks(kBase) == Ranges{{0x06C00000, 0x01400000}});
}

TEST_CASE("FCRAM allocator, memory mode 2 (Dev1), all three regions") {
    FcramRegions regions(2);
    CHECK(regions.Base(kSys) == 0x06000000);
    CHECK(regions.Size(kSys) == 0x00C00000);
    CHECK(regions.Base(kBase) == 0x06C00000);

    // APPLICATION 0x00000000-0x06000000
    CHECK(regions.HeapAllocate(kApp, 0x05F00000) == Ranges{{0x00100000, 0x05F00000}});
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0, 0x00100000}});
    CHECK(regions.LinearAllocate(kApp, 0x100000) == 0u);
    CHECK(regions.FreeBlocks(kApp).empty());
    CHECK_FALSE(regions.LinearAllocate(kApp, 0x1000).has_value());
    // free the middle of the heap allocation
    CHECK(regions.Free(kApp, 0x02000000, 0x01000000));
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x02000000, 0x01000000}});
    // 0x1800000 does not fit in the 0x1000000 that is free: refused as a whole
    CHECK(regions.HeapAllocate(kApp, 0x01800000).empty());
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x02000000, 0x01000000}});
    CHECK(regions.LinearAllocate(kApp, 0x02800000, 0x100000));
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x02000000, 0x800000}, {0x02900000, 0x700000}});
    // a heap request of 0xC00000: the top block (0x700000) whole, 0x500000 from the top of the next
    CHECK(regions.HeapAllocate(kApp, 0xC00000) == Ranges{{0x02300000, 0x500000}, {0x02900000, 0x700000}});
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x02000000, 0x300000}});
    CHECK(regions.Used(kApp) == 0x05D00000);
    // RLinearAllocate: from the top of the last block that is large enough
    CHECK(regions.RLinearAllocate(kApp, 0x100000) == 0x02200000u);
    CHECK(regions.FreeBlocks(kApp) == Ranges{{0x02000000, 0x200000}});
    CHECK_FALSE(regions.RLinearAllocate(kApp, 0x300000).has_value());
    CHECK(regions.LinearAllocate(kApp, 0x200000) == 0x02000000u);
    CHECK(regions.FreeBlocks(kApp).empty());

    // SYSTEM 0x06000000-0x06C00000: offsets are absolute
    CHECK(regions.LinearAllocate(kSys, 0x1000) == 0x06000000u);
    CHECK(regions.HeapAllocate(kSys, 0x1000) == Ranges{{0x06BFF000, 0x1000}});
    CHECK(regions.FreeBlocks(kSys) == Ranges{{0x06001000, 0x00BFE000}});
    CHECK(regions.Free(kSys, 0x06000000, 0x1000));
    CHECK(regions.FreeBlocks(kSys) == Ranges{{0x06000000, 0x00BFF000}});
    CHECK(regions.Used(kSys) == 0x1000);
    // a range in the next region is not SYSTEM's to give
    CHECK_FALSE(regions.LinearAllocate(kSys, 0x06BFF000, 0x2000));
    CHECK_FALSE(regions.LinearAllocate(kSys, 0x06C00000, 0x1000));
    CHECK_FALSE(regions.Free(kSys, 0x06C00000, 0x1000));

    // BASE 0x06C00000-0x08000000
    CHECK(regions.LinearAllocate(kBase, 0x1000) == 0x06C00000u);
    CHECK(regions.LinearAllocate(kBase, 0x07FFF000, 0x1000));
    CHECK(regions.HeapAllocate(kBase, 0x013FE000) == Ranges{{0x06C01000, 0x013FE000}});
    CHECK(regions.FreeBlocks(kBase).empty());
}

TEST_CASE("FCRAM allocator, memory mode 3, RLinearAllocate and the New 3DS mode 6") {
    FcramRegions mode3(3);
    CHECK(mode3.HeapAllocate(kApp, 0x01000000) == Ranges{{0x04000000, 0x01000000}});
    CHECK(mode3.LinearAllocate(kApp, 0x01000000) == 0u);
    CHECK(mode3.FreeBlocks(kApp) == Ranges{{0x01000000, 0x03000000}});
    CHECK(mode3.RLinearAllocate(kApp, 0x01000000) == 0x03000000u);
    CHECK(mode3.FreeBlocks(kApp) == Ranges{{0x01000000, 0x02000000}});

    FcramRegions mode6(6);
    CHECK(mode6.HeapAllocate(kApp, 0x07C00000) == Ranges{{0, 0x07C00000}});
    CHECK(mode6.LinearAllocate(kSys, 0x1000) == 0x07C00000u);
    CHECK(mode6.LinearAllocate(kBase, 0x1000) == 0x0E000000u);
}

// ---- bad accesses ---------------------------------------------------------------------------------

TEST_CASE("the fault report line") {
    char line[200];
    auto format = [&](u32 addr, FaultAccess access, bool ctx, u32 id, u32 lr) {
        const std::size_t n = FormatFaultReport(line, sizeof line, addr, access, ctx, id, lr);
        return std::string(line, n);
    };
    CHECK(format(0x08001234, FaultAccess::Read, true, 7, 0x00100ABC) ==
          "mh4n: bad guest memory access: read at 0x08001234 (guest thread 7, lr 0x00100abc)\n");
    CHECK(format(0, FaultAccess::Write, true, 4294967295u, 0xFFFFFFFF) ==
          "mh4n: bad guest memory access: write at 0x00000000 (guest thread 4294967295, lr "
          "0xffffffff)\n");
    CHECK(format(0xDEADBEEF, FaultAccess::Unknown, false, 0, 0) ==
          "mh4n: bad guest memory access: access at 0xdeadbeef (no guest thread)\n");
    CHECK(format(1, FaultAccess::Read, true, 0, 0) ==
          "mh4n: bad guest memory access: read at 0x00000001 (guest thread 0, lr 0x00000000)\n");
    // a short buffer is filled and not overrun
    char small[16 + 4];
    std::memset(small, '#', sizeof small);
    const std::size_t n = FormatFaultReport(small, 16, 0x1234, FaultAccess::Read, false, 0, 0);
    CHECK(n == 16);
    CHECK(std::string(small, 16) == "mh4n: bad guest ");
    CHECK(small[16] == '#');
}

namespace {

// Is the child's report for `guest_addr` there? `access` is "read" or "write" where the platform
// tells (Linux on x86-64), otherwise any word.
void CheckReport(const ChildResult& child, const char* guest_addr, const char* access,
                 const std::string& tail) {
    CAPTURE(child.output);
    CHECK(child.exit_code == 3);
    CHECK(child.output.find("mh4n: bad guest memory access: ") == 0);
    CHECK(child.output.find(std::string(" at ") + guest_addr + " ") != std::string::npos);
#if (defined(__linux__) || defined(__APPLE__)) && defined(__x86_64__) || defined(_WIN32)
    CHECK(child.output.find(std::string(access) + " at") != std::string::npos);
#else
    (void)access;
#endif
    CHECK(child.output.find(tail) != std::string::npos);
    CHECK(child.output.back() == '\n');
}

} // namespace

TEST_CASE("an unmapped read ends the process with status 3 and the guest address") {
    const ChildResult child = RunSelf("--fault-test read-unmapped");
    CheckReport(child, "0x08000010", "read", "(no guest thread)");
}

TEST_CASE("an unmapped write is reported as a write") {
    const ChildResult child = RunSelf("--fault-test write-unmapped");
    CheckReport(child, "0x08001234", "write", "(no guest thread)");
}

TEST_CASE("a page that was unmapped faults, its neighbours do not") {
    const ChildResult child = RunSelf("--fault-test after-unmap");
    CHECK(child.output.find("neighbours ok\n") == 0);
    CHECK(child.exit_code == 3);
    CHECK(child.output.find(" at 0x08001010 ") != std::string::npos);
}

TEST_CASE("the report names the guest thread and its LR") {
    const ChildResult child = RunSelf("--fault-test context");
    CheckReport(child, "0x08001234", "read", "(guest thread 7, lr 0x00100abc)");
}

TEST_CASE("zero regions fault under the guard policy and read zero under the fill policy") {
    const ChildResult guard = RunSelf("--fault-test zero-guard");
    CheckReport(guard, "0x00000010", "read", "(no guest thread)");
    const ChildResult fill = RunSelf("--fault-test zero-fill");
    CHECK(fill.exit_code == 0);
    CHECK(fill.output == "zero ok\n");
}

TEST_CASE("an access in the guard after the 4 GiB is a bad access too") {
    const ChildResult child = RunSelf("--fault-test guard");
    CheckReport(child, "0x00000010", "read", "(no guest thread)"); // wrapped to 32 bits
}

TEST_CASE("another host thread reports bad accesses as well") {
    const ChildResult child = RunSelf("--fault-test thread");
    CheckReport(child, "0x09000000", "write", "(guest thread 3, lr 0x00000042)");
}

#if !defined(_WIN32)
TEST_CASE("a fault outside the reservation goes to the handler that was there before") {
    const ChildResult child = RunSelf("--fault-test foreign-handler");
    CHECK(child.exit_code == 77);
    CHECK(child.output == "previous handler\n");
    // with no handler before ours the process dies of the signal, as it would have without us
    const ChildResult dies = RunSelf("--fault-test foreign-default");
    CHECK(dies.signal == SIGSEGV);
    CHECK(dies.output.find("bad guest memory access") == std::string::npos);
}
#endif

// ---- the child side of the fault tests -------------------------------------------------------------

namespace {

volatile u8 g_sink;

int RunFaultScenario(const std::string& name) {
#if !defined(_WIN32)
    rlimit no_core = {0, 0};
    setrlimit(RLIMIT_CORE, &no_core); // a crash on purpose is not worth a core file
#endif
    if (name == "foreign-handler") {
#if !defined(_WIN32)
        struct sigaction action;
        std::memset(&action, 0, sizeof action);
        action.sa_handler = [](int) {
            const char text[] = "previous handler\n";
            (void)!write(1, text, sizeof text - 1);
            _exit(77);
        };
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, nullptr);
        sigaction(SIGBUS, &action, nullptr);
        auto memory = GuestMemory::Create();
        if (memory == nullptr) {
            return 1;
        }
        g_sink = *static_cast<volatile u8*>(nullptr); // a fault of the process, not of the guest
        return 0;
#endif
    }
    if (name == "foreign-default") {
        auto memory = GuestMemory::Create();
        if (memory == nullptr) {
            return 1;
        }
        g_sink = *static_cast<volatile u8*>(nullptr);
        return 0;
    }

    GuestMemoryOptions options;
    // the default policy follows MH4N_DEBUG_GUARD; the scenarios say which one they mean
    options.zero_policy = name == "zero-guard" ? ZeroPolicy::Fault : ZeroPolicy::ZeroFill;
    auto memory = GuestMemory::Create(options);
    if (memory == nullptr) {
        return 1;
    }
    volatile u8* base = memory->Base();
    if (name == "read-unmapped") {
        g_sink = base[0x08000010];
    } else if (name == "write-unmapped") {
        base[0x08001234] = 1;
    } else if (name == "after-unmap") {
        if (memory->Map(0x08000000, 0x3000, MemPerm::ReadWrite, MemState::Private) != 0 ||
            memory->Unmap(0x08001000, 0x1000) != 0) {
            return 1;
        }
        base[0x08000FFF] = 1;
        g_sink = base[0x08002000];
        std::fputs("neighbours ok\n", stderr);
        std::fflush(stderr);
        g_sink = base[0x08001010];
    } else if (name == "context") {
        u32 regs[16] = {};
        regs[14] = 0x00100ABC;
        SetFaultContext(regs, 7);
        g_sink = base[0x08001234];
    } else if (name == "zero-guard") {
        g_sink = base[0x10];
    } else if (name == "zero-fill") {
        g_sink = base[0x10];
        base[0x20] = 5;
        if (g_sink != 0 || base[0x20] != 5) {
            return 1;
        }
        std::fputs("zero ok\n", stderr);
        return 0;
    } else if (name == "guard") {
        g_sink = base[kAddressSpaceSize + 0x10];
    } else if (name == "thread") {
        std::thread worker([&] {
            GuestMemory::PrepareThreadForFaults();
            u32 regs[16] = {};
            regs[14] = 0x42;
            SetFaultContext(regs, 3);
            base[0x09000000] = 1;
        });
        worker.join();
    } else {
        std::fprintf(stderr, "unknown scenario %s\n", name.c_str());
        return 1;
    }
    return 0; // reaching here means the access did not fault
}

} // namespace

int main(int argc, char** argv) {
    g_self_path = argv[0];
#if defined(__linux__)
    {
        char path[4096];
        const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
        if (n > 0) {
            path[n] = '\0';
            g_self_path = path;
        }
    }
#endif
    if (argc >= 3 && std::strcmp(argv[1], "--fault-test") == 0) {
        return RunFaultScenario(argv[2]);
    }
    return doctest::Context(argc, argv).run();
}
