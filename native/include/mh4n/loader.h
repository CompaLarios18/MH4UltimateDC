// Derived from Azahar src/core/file_sys/ncch_container.h, src/core/loader/ncch.cpp,
// src/core/loader/elf.cpp, src/core/hle/kernel/process.cpp, src/core/hle/kernel/config_mem.h,
// src/core/hle/kernel/shared_page.h and src/core/hle/kernel/memory.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: ncch_container.h says so, the
// other sources say GPL-2.0-or-later): the exheader capability layouts,
// ParseKernelCaps, the segment and stack mapping of Process::Run, the ELF defaults, and the
// field layouts and initial values of the configuration memory and the shared page.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.h, ncch.cpp, elf.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp)
// Copyright 2014 Citra Emulator Project (config_mem.h)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (shared_page.h)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (memory.cpp)
//
// Loader and system pages (N1-03): the game's process as Azahar sets it up in Old 3DS mode, from
// a game folder (tools/native/prepare_game.py) or from an ELF test program; and the two pages the
// kernel maps into every process, the configuration memory and the shared page.
//
// Typical use (the runner, N1-09):
//   auto loader = Loader::OpenGameFolder(dir, &error);           // parses, builds the ProcessImage
//   FcramRegions regions(loader->Image().memory_mode);
//   loader->Map(memory, regions, &error);                        // segments, stack, linear heap
//   SystemPages pages(init_time);  pages.Map(memory, regions);   // config memory + shared page
// Creating the main thread and applying the CPU setting from the ProcessImage is the kernel's
// (N1-27).
#pragma once

#include <bitset>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mh4n/memory.h"

namespace mh4n {

// ---- the extended header (3dbrew NCCH/Extended Header; Azahar's ExHeader_Header) --------------

struct ExHeaderSegment {
    u32 address;
    u32 num_pages;  // the "maximum pages" field: the segment's size in 4 KiB pages
    u32 size;       // in bytes
};

// A range the process asks the kernel to map (kernel capability descriptors, Azahar's
// AddressMapping). The loader records them; the kernel maps the ones it supports.
struct AddressMapping {
    u32 address;
    u32 size;
    bool read_only;
    bool unk_flag;
    friend bool operator==(const AddressMapping&, const AddressMapping&) = default;
};

// ARM11 kernel capabilities, parsed as Azahar's Process::ParseKernelCaps does.
struct KernelCaps {
    std::optional<u32> kernel_version;       // the kernel version descriptor
    std::optional<u32> handle_table_size;
    std::optional<u16> flags;                // the "misc flags" descriptor
    std::bitset<128> svc_mask;               // allowed SVC numbers
    std::vector<AddressMapping> address_mappings;
    // Default allocation region of the process (flags bits 8-11): 1 APPLICATION, 2 SYSTEM,
    // 3 BASE. APPLICATION when the descriptor is absent, as Azahar's CreateProcess sets it.
    u32 MemoryRegion() const { return flags ? ((*flags >> 8) & 0xF) : 1; }
};

inline constexpr std::size_t kExHeaderMinSize = 0x3E0;  // up to the end of the kernel capabilities

struct ExHeader {
    std::string title;
    u8 flags = 0;                  // bit 0: .code is compressed
    u16 remaster_version = 0;
    ExHeaderSegment text{}, ro{}, data{};
    u32 stack_size = 0;
    u32 bss_size = 0;
    u64 program_id = 0;
    u32 core_version = 0;
    bool enable_l2_cache = false;
    bool enable_804mhz_cpu = false;
    u32 n3ds_mode = 0;             // New 3DS system mode (0: legacy)
    u32 ideal_processor = 0;
    u32 affinity_mask = 0;
    u32 system_mode = 0;           // the Old 3DS memory mode
    u32 priority = 0;              // main thread
    u32 max_cpu = 0;               // core 1 CPU time limit
    u32 schedule_mode = 0;         // core 1: 0 single, 1 multi
    u32 resource_limit_category = 0;  // 0 application, 1 sys applet, 2 lib applet, 3 other
    KernelCaps kernel_caps;
    bool CodeCompressed() const { return (flags & 1) != 0; }
};

// Parses at least the first kExHeaderMinSize bytes of an exheader. False with a message in
// *error if there are fewer.
bool ParseExHeader(const u8* data, std::size_t size, ExHeader* out, std::string* error);

// ---- the process image -------------------------------------------------------------------------

struct SegmentMapping {
    std::string name;  // "text", "ro", "data", "stack"
    u32 vaddr;
    u32 size;          // mapped bytes (page multiple; data includes the bss)
    MemPerm perm;
    MemState state;
};

enum class ProgramKind { GameFolder, Elf };

inline constexpr u32 kDefaultStackSize = 0x4000;           // Azahar's DEFAULT_STACK_SIZE
inline constexpr u32 kStackTop = kHeapVaddrEnd;            // the main thread's stack top
inline constexpr u32 kMainThreadFpscr = 0x03C00010;        // default NaN, flush to zero, IXC
// Kernel versions from 0x22C use the new linear heap layout (system 8.0.0).
inline constexpr u32 kNewLinearHeapKernelVersion = 0x22C;
// The ELF defaults (Azahar's AppLoader_ELF::Load and Process::Set3dsxKernelCaps).
inline constexpr u32 kElfKernelVersion = 0x23A;
inline constexpr u32 kElfMemoryMode = 2;                   // Dev1: 96 MiB for the application
inline constexpr s32 kElfMainThreadPriority = 48;

// What the kernel needs to start the process (N1-27): everything Azahar's loaders pass on.
struct ProcessImage {
    std::string name;
    ProgramKind kind = ProgramKind::GameFolder;
    u64 program_id = 0;
    u32 entry = 0;                         // the text start
    u32 stack_top = kStackTop;
    u32 stack_size = kDefaultStackSize;
    s32 main_thread_priority = 0;
    u32 ideal_processor = 0;
    u32 main_thread_fpscr = kMainThreadFpscr;
    u32 memory_mode = 0;                   // Old 3DS (0, 2, 3, 4, 5); FcramRegions' mode
    u32 kernel_version = 0;
    u32 linear_heap_area = kLinearHeapVaddr;  // GuestMemory::SetLinearHeapArea's argument
    FcramRegion memory_region = FcramRegion::Application;  // where the segments are allocated
    u32 resource_limit_category = 0;
    // ApplyAppMaxCPUSetting(process, core1_schedule_mode, core1_max_cpu), called when
    // apply_app_max_cpu (the application category).
    bool apply_app_max_cpu = true;
    u32 core1_schedule_mode = 0;
    u32 core1_max_cpu = 0;
    std::bitset<128> svc_mask;
    std::vector<AddressMapping> address_mappings;
    std::optional<u32> handle_table_size;
    // text, ro and data in the order they are mapped, then the stack.
    std::vector<SegmentMapping> segments;
    // The text segment's bytes (not its page-rounded size): the part the code library's
    // text_crc32 covers.
    u32 text_base = 0;
    u32 text_size = 0;
};

// Kernel version below 0x22C: 0x14000000, else 0x30000000.
constexpr u32 LinearHeapAreaForKernelVersion(u32 kernel_version) {
    return kernel_version < kNewLinearHeapKernelVersion ? kLinearHeapVaddr : kNewLinearHeapVaddr;
}

// Multi-line text for --print-process: the image fields and the segment map.
std::string DescribeProcessImage(const ProcessImage& image);

class Loader {
public:
    // dir holds exheader.bin and code.bin (decompressed), as prepare_game.py writes them. The
    // memory mode is the exheader's system mode (New 3DS mode off).
    static std::unique_ptr<Loader> OpenGameFolder(const std::string& dir, std::string* error);
    // The same from the two files' contents. `code` is the decompressed .code.
    static std::unique_ptr<Loader> FromExHeader(const ExHeader& header, std::vector<u8> code,
                                                std::string name, std::string* error);
    // A 32-bit little-endian ARM ELF test program with Azahar's defaults for ELF files.
    static std::unique_ptr<Loader> OpenElf(const std::string& path, std::string* error);
    static std::unique_ptr<Loader> FromElf(std::vector<u8> bytes, std::string name,
                                           std::string* error);

    const ProcessImage& Image() const { return image_; }

    // Maps the segments and the stack in Azahar's order (Process::Run), allocating each from
    // FCRAM with FcramRegions::HeapAllocate in the process's memory region so that later
    // allocations land where they do in Azahar, copies the code in (the bss stays zero), and sets
    // the linear heap area. `regions` must have the image's memory mode. Call once.
    bool Map(GuestMemory& memory, FcramRegions& regions, std::string* error);

    // The text segment's bytes in guest memory, for the code host's check against the library's
    // text_crc32 (N1-04). Empty before Map.
    std::span<const u8> TextBytes(const GuestMemory& memory) const;
    u32 TextCrc32(const GuestMemory& memory) const;

private:
    struct Piece {
        u32 vaddr;
        u32 size;
        std::size_t offset;     // into code_
        std::size_t data_size;  // bytes to copy; the rest of the segment stays zero
    };
    Loader() = default;
    ProcessImage image_;
    std::vector<u8> code_;
    std::vector<Piece> pieces_;  // one per segment of image_.segments, the stack last (no data)
    bool mapped_ = false;
};

// ---- configuration memory (0x1FF80000) ---------------------------------------------------------

inline constexpr u32 kConfigMemorySize = 0x1000;

// Offsets of the fields Azahar's ConfigMem::Handler sets (firmware 11.17.0) and MemoryInit fills.
namespace config_mem {
inline constexpr u32 kKernelVersionRev = 0x01;  // u8
inline constexpr u32 kKernelVersionMin = 0x02;  // u8
inline constexpr u32 kKernelVersionMaj = 0x03;  // u8
inline constexpr u32 kUpdateFlag = 0x04;        // u32
inline constexpr u32 kNsTid = 0x08;             // u64
inline constexpr u32 kSysCoreVer = 0x10;        // u32
inline constexpr u32 kUnitInfo = 0x14;          // u8, bit 0: retail
inline constexpr u32 kBootFirm = 0x15;          // u8
inline constexpr u32 kPrevFirm = 0x16;          // u8
inline constexpr u32 kCtrSdkVer = 0x18;         // u32
inline constexpr u32 kAppMemType = 0x30;        // u32, the memory mode
inline constexpr u32 kAppMemAlloc = 0x40;       // u32, APPLICATION region size
inline constexpr u32 kSysMemAlloc = 0x44;       // u32
inline constexpr u32 kBaseMemAlloc = 0x48;      // u32
inline constexpr u32 kFirmVersionRev = 0x61;    // u8
inline constexpr u32 kFirmVersionMin = 0x62;    // u8
inline constexpr u32 kFirmVersionMaj = 0x63;    // u8
inline constexpr u32 kFirmSysCoreVer = 0x64;    // u32
inline constexpr u32 kFirmCtrSdkVer = 0x68;     // u32
}  // namespace config_mem

// Fills a 4 KiB page like Azahar's ConfigMem::Handler followed by KernelSystem::MemoryInit.
void FillConfigMemory(u8* page, const FcramRegions& regions);

// ---- the shared page (0x1FF81000) --------------------------------------------------------------

inline constexpr u32 kSharedPageSize = 0x1000;
inline constexpr u64 kBaseClockRateArm11 = 268111856;  // ticks per second
// The kernel updates the date every hour of game time (UpdateTimeCallback reschedules itself).
inline constexpr u64 kSharedPageUpdateIntervalTicks = kBaseClockRateArm11 * 3600;
// 2026-01-01 12:00:00 UTC: the fixed start date of the reference runs (the reference runner), since the game
// seeds its random numbers from the clock.
inline constexpr u64 kDefaultInitTime = 1767268800;
inline constexpr u64 kUnixTimeOf2000 = 946684800;
inline constexpr u64 kMillisecondsFrom1900To2000 = 3155673600000ull;

namespace shared_page {
inline constexpr u32 kDateTimeCounter = 0x00;  // u32, +1 per update
inline constexpr u32 kRunningHw = 0x04;        // u8, 1: product
inline constexpr u32 kMcuHwInfo = 0x05;        // u8
inline constexpr u32 kDateTime0 = 0x20;        // DateTime, used when the counter is odd
inline constexpr u32 kDateTime1 = 0x40;        // DateTime, used when the counter is even
inline constexpr u32 kWifiMacAddress = 0x60;   // u8[6]
inline constexpr u32 kWifiLinkLevel = 0x66;    // u8, 3: best
inline constexpr u32 kWifiState = 0x67;        // u8, 2: internet
inline constexpr u32 kSliderState3d = 0x80;    // float
inline constexpr u32 kLedState3d = 0x84;       // u8
inline constexpr u32 kBatteryState = 0x85;     // u8: adapter bit 0, charging bit 1, level bits 2-4
inline constexpr u32 kUnknownValue = 0x86;     // u8, 1
inline constexpr u32 kMenuTitleId = 0xA0;      // u64
inline constexpr u32 kActiveMenuTitleId = 0xA8;  // u64
// Inside a DateTime:
inline constexpr u32 kDtDateTime = 0x00;       // u64, milliseconds since 1900
inline constexpr u32 kDtUpdateTick = 0x08;     // u64
inline constexpr u32 kDtTickToSecond = 0x10;   // u64, ticks per second
inline constexpr u32 kDtTickOffset = 0x18;     // u64
}  // namespace shared_page

// The two pages, mapped read-only (state Shared) into the process. The shared page's date starts
// at `init_time` (seconds since the Unix epoch, UTC) and advances with game time: the timer queue
// (N1-06) calls UpdateSharedPage(now_ticks) at tick 0, which Map does, and then every
// kSharedPageUpdateIntervalTicks. Azahar measures from 2000-01-01 in the host's time zone; this
// uses UTC, which is what it computes on the UTC hosts where golden sessions are recorded.
class SystemPages {
public:
    explicit SystemPages(u64 init_time = kDefaultInitTime) : init_time_(init_time) {}

    // Maps both pages and fills them (the date as of tick 0). Errors as GuestMemory::Map.
    Result Map(GuestMemory& memory, const FcramRegions& regions);

    // Azahar's UpdateTimeCallback with `ticks` of game time since the start: writes the date into
    // the date-time block the counter's parity picks, then bumps the counter.
    void UpdateSharedPage(u64 ticks);

    // Azahar's GetSystemTimeSince2000/1900 for `ticks` of game time, in milliseconds.
    u64 SystemTimeSince2000(u64 ticks) const;
    u64 SystemTimeSince1900(u64 ticks) const;

    u64 InitTime() const { return init_time_; }
    // The 3D slider (0.0 to 1.0).
    void Set3DSlider(float value);

private:
    GuestMemory* memory_ = nullptr;
    u64 init_time_;
};

// The page contents without a GuestMemory, for tests and tools: fills `page` (kSharedPageSize
// bytes) as Azahar's Handler constructor does, before the first update.
void FillSharedPage(u8* page);

}  // namespace mh4n
