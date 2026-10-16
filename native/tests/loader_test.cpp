// mh4n_loader (N1-03): the exheader parser, the process image and segment map of a game folder
// and of an ELF program, the linear heap rule, and the configuration memory and shared page.
//
// Synthetic inputs only: the exheader and the ELF are built here. The expected values are the
// ones Azahar's loaders and kernel produce for the same inputs (worked out from
// src/core/loader/ncch.cpp, elf.cpp, hle/kernel/process.cpp, memory.cpp, config_mem.cpp and
// shared_page.cpp).
#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mh4n/crc32.h"
#include "mh4n/loader.h"

using namespace mh4n;

namespace {

void Put16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v);
    b[at + 1] = static_cast<u8>(v >> 8);
}
void Put32(std::vector<u8>& b, std::size_t at, u32 v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<u8>(v >> (8 * i));
}
void Put64(std::vector<u8>& b, std::size_t at, u64 v) {
    Put32(b, at, static_cast<u32>(v));
    Put32(b, at + 4, static_cast<u32>(v >> 32));
}

struct ExhParams {
    u32 text_addr = 0x00100000, text_pages = 3, text_size = 0x2F00;
    u32 ro_addr = 0x00200000, ro_pages = 1, ro_size = 0x900;
    u32 data_addr = 0x00300000, data_pages = 2, data_size = 0x1800;
    u32 bss_size = 0x1234;
    u32 stack = 0x4000;
    u32 system_mode = 2, ideal = 1, priority = 0x30, max_cpu = 0x59, schedule = 1, category = 0;
    u32 kernel_version = 0x23A;
    std::vector<u32> extra_caps;
};

std::vector<u8> MakeExheader(const ExhParams& p = {}) {
    std::vector<u8> b(0x800, 0);
    std::memcpy(b.data(), "SYNTH", 5);
    b[0x0D] = 1;
    Put32(b, 0x10, p.text_addr), Put32(b, 0x14, p.text_pages), Put32(b, 0x18, p.text_size);
    Put32(b, 0x1C, p.stack);
    Put32(b, 0x20, p.ro_addr), Put32(b, 0x24, p.ro_pages), Put32(b, 0x28, p.ro_size);
    Put32(b, 0x30, p.data_addr), Put32(b, 0x34, p.data_pages), Put32(b, 0x38, p.data_size);
    Put32(b, 0x3C, p.bss_size);
    Put64(b, 0x200, 0x0004000000ABCD00);
    Put32(b, 0x208, 2);
    b[0x20C] = 3;
    b[0x20D] = 1;
    b[0x20E] = static_cast<u8>(p.ideal | (2 << 2) | (p.system_mode << 4));
    b[0x20F] = static_cast<u8>(p.priority);
    Put16(b, 0x210, p.max_cpu | (p.schedule << 7));
    b[0x36F] = static_cast<u8>(p.category);
    std::vector<u32> caps = {0xFC000000u | p.kernel_version, 0xFE000000u | 0x200};
    caps.insert(caps.end(), p.extra_caps.begin(), p.extra_caps.end());
    caps.resize(28, 0xFFFFFFFFu);
    for (std::size_t i = 0; i < 28; ++i) Put32(b, 0x370 + 4 * i, caps[i]);
    return b;
}

// code.bin: the three segments padded to their pages, with recognisable bytes.
std::vector<u8> MakeCode(const ExhParams& p = {}) {
    std::vector<u8> code((static_cast<std::size_t>(p.text_pages) + p.ro_pages + p.data_pages) * 0x1000);
    for (std::size_t i = 0; i < code.size(); ++i) code[i] = static_cast<u8>(0x11 + (i * 7) % 0xE0);
    return code;
}

std::unique_ptr<GuestMemory> NewMemory() {
    auto memory = GuestMemory::Create();
    REQUIRE(memory != nullptr);
    return memory;
}

std::vector<u8> Bytes(const GuestMemory& memory, u32 vaddr, u32 size) {
    std::vector<u8> out(size);
    REQUIRE(memory.ReadBlock(out.data(), vaddr, size));
    return out;
}

// ---- a small ELF -------------------------------------------------------------------------------

struct ElfSeg {
    u32 vaddr, filesz, memsz, flags;
};

std::vector<u8> MakeElf(const std::vector<ElfSeg>& segs, u32 entry, u16 type = 2, u16 machine = 40) {
    const std::size_t ph_off = 52;
    std::size_t data_off = ph_off + 32 * segs.size();
    std::vector<u8> b(data_off, 0);
    b[0] = 0x7F, b[1] = 'E', b[2] = 'L', b[3] = 'F', b[4] = 1, b[5] = 1, b[6] = 1;
    Put16(b, 16, type), Put16(b, 18, machine), Put32(b, 20, 1), Put32(b, 24, entry);
    Put32(b, 28, ph_off), Put16(b, 40, 52), Put16(b, 42, 32), Put16(b, 44, static_cast<u32>(segs.size()));
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const std::size_t p = ph_off + 32 * i;
        Put32(b, p, 1);  // PT_LOAD
        Put32(b, p + 4, static_cast<u32>(b.size()));
        Put32(b, p + 8, segs[i].vaddr), Put32(b, p + 12, segs[i].vaddr);
        Put32(b, p + 16, segs[i].filesz), Put32(b, p + 20, segs[i].memsz);
        Put32(b, p + 24, segs[i].flags), Put32(b, p + 28, 0x1000);
        for (u32 j = 0; j < segs[i].filesz; ++j) b.push_back(static_cast<u8>(0xA0 + i * 8 + j % 7));
    }
    return b;
}

constexpr u32 kRX = 5, kR = 4, kRW = 6;

}  // namespace

TEST_CASE("ParseExHeader reads the segments and the capabilities") {
    ExhParams p;
    p.extra_caps = {0xF0000000u | 0x00FFFF, 0xF1000000u | 0x5, 0xFF000000u | (2 << 8) | 4,
                    0xFF800000u | 0x1FF50, 0xFF800000u | 0x1FF58, 0xFFE00000u | 0x1EC40,
                    0xE0000000u | 0x1F};
    const auto bytes = MakeExheader(p);
    ExHeader h;
    std::string error;
    REQUIRE(ParseExHeader(bytes.data(), bytes.size(), &h, &error));
    CHECK(h.title == "SYNTH");
    CHECK(h.CodeCompressed());
    CHECK(h.text.address == 0x00100000);
    CHECK(h.text.num_pages == 3);
    CHECK(h.text.size == 0x2F00);
    CHECK(h.ro.address == 0x00200000);
    CHECK(h.data.size == 0x1800);
    CHECK(h.bss_size == 0x1234);
    CHECK(h.stack_size == 0x4000);
    CHECK(h.program_id == 0x0004000000ABCD00);
    CHECK(h.core_version == 2);
    CHECK(h.enable_l2_cache);
    CHECK(h.enable_804mhz_cpu);
    CHECK(h.n3ds_mode == 1);
    CHECK(h.ideal_processor == 1);
    CHECK(h.affinity_mask == 2);
    CHECK(h.system_mode == 2);
    CHECK(h.priority == 0x30);
    CHECK(h.max_cpu == 0x59);
    CHECK(h.schedule_mode == 1);
    CHECK(h.resource_limit_category == 0);

    const KernelCaps& kc = h.kernel_caps;
    CHECK(*kc.kernel_version == 0x23A);
    CHECK(*kc.handle_table_size == 0x200);
    CHECK(*kc.flags == ((2 << 8) | 4));
    CHECK(kc.MemoryRegion() == 2);
    CHECK(kc.svc_mask.count() == 16 + 2);
    CHECK(kc.svc_mask.test(0));
    CHECK(kc.svc_mask.test(15));
    CHECK_FALSE(kc.svc_mask.test(16));
    CHECK(kc.svc_mask.test(24));
    CHECK(kc.svc_mask.test(26));
    REQUIRE(kc.address_mappings.size() == 2);
    CHECK(kc.address_mappings[0] == AddressMapping{0x1FF50000, 0x8000, false, false});
    CHECK(kc.address_mappings[1] == AddressMapping{0x1EC40000, 0x1000, false, false});
}

TEST_CASE("ParseExHeader: the read-only and unknown flags of a mapped range, an incomplete one") {
    ExhParams p;
    p.extra_caps = {0xFF800000u | (1 << 20) | 0x1F000, 0xFF800000u | (1 << 20) | 0x1F600,
                    0xFF800000u | 0x5000,  // wants a second range descriptor, gets a page one
                    0xFFE00000u | 0x2345};
    const auto bytes = MakeExheader(p);
    ExHeader h;
    REQUIRE(ParseExHeader(bytes.data(), bytes.size(), &h, nullptr));
    const auto& m = h.kernel_caps.address_mappings;
    REQUIRE(m.size() == 2);
    CHECK(m[0] == AddressMapping{0x1F000000, 0x600000, true, true});
    CHECK(m[1] == AddressMapping{0x02345000, 0x1000, false, false});
    // No misc flags descriptor: the APPLICATION region, as Azahar's CreateProcess sets it.
    CHECK(h.kernel_caps.MemoryRegion() == 1);
}

TEST_CASE("ParseExHeader refuses a short exheader") {
    std::vector<u8> bytes(kExHeaderMinSize - 1);
    ExHeader h;
    std::string error;
    CHECK_FALSE(ParseExHeader(bytes.data(), bytes.size(), &h, &error));
    CHECK(error.find("too small") != std::string::npos);
}

TEST_CASE("the linear heap area follows the kernel version") {
    CHECK(LinearHeapAreaForKernelVersion(0x22B) == 0x14000000);
    CHECK(LinearHeapAreaForKernelVersion(0x22C) == 0x30000000);
    CHECK(LinearHeapAreaForKernelVersion(0x0) == 0x14000000);
    for (const u32 version : {0x22Bu, 0x22Cu}) {
        ExhParams p;
        p.kernel_version = version;
        const auto bytes = MakeExheader(p);
        ExHeader h;
        REQUIRE(ParseExHeader(bytes.data(), bytes.size(), &h, nullptr));
        std::string error;
        auto loader = Loader::FromExHeader(h, MakeCode(p), "x", &error);
        REQUIRE_MESSAGE(loader, error);
        CHECK(loader->Image().kernel_version == version);
        CHECK(loader->Image().linear_heap_area == (version == 0x22B ? 0x14000000u : 0x30000000u));
        auto memory = NewMemory();
        FcramRegions regions(loader->Image().memory_mode);
        REQUIRE_MESSAGE(loader->Map(*memory, regions, &error), error);
        CHECK(memory->LinearHeapArea() == loader->Image().linear_heap_area);
    }
}

TEST_CASE("a game folder: the process image and the segment map") {
    const ExhParams p;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "mh4n_loader_test_folder";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto exheader = MakeExheader(p);
    const auto code = MakeCode(p);
    std::ofstream(dir / "exheader.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(exheader.data()), static_cast<std::streamsize>(exheader.size()));
    std::ofstream(dir / "code.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(code.size()));

    std::string error;
    auto loader = Loader::OpenGameFolder(dir.string(), &error);
    fs::remove_all(dir);
    REQUIRE_MESSAGE(loader, error);
    const ProcessImage& image = loader->Image();
    CHECK(image.entry == 0x00100000);
    CHECK(image.stack_top == 0x10000000);
    CHECK(image.stack_size == 0x4000);
    CHECK(image.main_thread_priority == 0x30);
    CHECK(image.ideal_processor == 1);
    CHECK(image.memory_mode == 2);
    CHECK(image.kernel_version == 0x23A);
    CHECK(image.program_id == 0x0004000000ABCD00);
    CHECK(image.main_thread_fpscr == 0x03C00010);
    CHECK(image.resource_limit_category == 0);
    CHECK(image.apply_app_max_cpu);
    CHECK(image.core1_schedule_mode == 1);
    CHECK(image.core1_max_cpu == 0x59);
    CHECK(image.memory_region == FcramRegion::Application);
    CHECK(*image.handle_table_size == 0x200);

    REQUIRE(image.segments.size() == 4);
    CHECK(image.segments[0].name == "text");
    CHECK(image.segments[0].vaddr == 0x00100000);
    CHECK(image.segments[0].size == 0x3000);
    CHECK(image.segments[0].perm == MemPerm::ReadExecute);
    CHECK(image.segments[0].state == MemState::Code);
    CHECK(image.segments[1].size == 0x1000);
    CHECK(image.segments[1].perm == MemPerm::Read);
    // data plus the bss rounded to pages: 0x2000 + 0x2000
    CHECK(image.segments[2].size == 0x4000);
    CHECK(image.segments[2].perm == MemPerm::ReadWrite);
    CHECK(image.segments[2].state == MemState::Private);
    CHECK(image.segments[3].name == "stack");
    CHECK(image.segments[3].vaddr == 0x0FFFC000);
    CHECK(image.segments[3].state == MemState::Locked);

    auto memory = NewMemory();
    FcramRegions regions(image.memory_mode);
    REQUIRE_MESSAGE(loader->Map(*memory, regions, &error), error);

    // Permissions, states and extents as svcQueryMemory would see them.
    CHECK(*memory->Query(0x00100000) == MemoryInfo{0x00100000, 0x3000, MemPerm::ReadExecute, MemState::Code});
    CHECK(*memory->Query(0x00200000) == MemoryInfo{0x00200000, 0x1000, MemPerm::Read, MemState::Code});
    CHECK(*memory->Query(0x00300000) == MemoryInfo{0x00300000, 0x4000, MemPerm::ReadWrite, MemState::Private});
    CHECK(*memory->Query(0x0FFFC000) == MemoryInfo{0x0FFFC000, 0x4000, MemPerm::ReadWrite, MemState::Locked});
    CHECK_FALSE(memory->IsMapped(0x00103000, 1));
    CHECK_FALSE(memory->IsMapped(0x0FFFB000, 1));

    // Contents: each segment's pages, the bss and the stack zero.
    CHECK(Bytes(*memory, 0x00100000, 0x3000) == std::vector<u8>(code.begin(), code.begin() + 0x3000));
    CHECK(Bytes(*memory, 0x00200000, 0x1000) == std::vector<u8>(code.begin() + 0x3000, code.begin() + 0x4000));
    CHECK(Bytes(*memory, 0x00300000, 0x2000) == std::vector<u8>(code.begin() + 0x4000, code.end()));
    const auto bss = Bytes(*memory, 0x00302000, 0x2000);
    CHECK(bss == std::vector<u8>(0x2000, 0));
    CHECK(Bytes(*memory, 0x0FFFC000, 0x4000) == std::vector<u8>(0x4000, 0));

    // FCRAM: text, ro, data and the stack were taken from the top of APPLICATION, in that order
    // (Azahar's Process::Run), so the first linear allocation lands at the bottom.
    // sizes: 0x3000 + 0x1000 + 0x4000 + 0x4000 = 0xC000
    CHECK(regions.Used(FcramRegion::Application) == 0xC000);
    const auto free_blocks = regions.FreeBlocks(FcramRegion::Application);
    REQUIRE(free_blocks.size() == 1);
    CHECK(free_blocks[0] == FcramRange{0, 0x06000000 - 0xC000});
    CHECK(regions.LinearAllocate(FcramRegion::Application, 0x1000) == std::optional<u32>(0));

    // The text bytes the code host compares with the library's text_crc32: the exheader's text
    // size, not the page-rounded one.
    const auto text = loader->TextBytes(*memory);
    REQUIRE(text.size() == 0x2F00);
    CHECK(std::memcmp(text.data(), code.data(), text.size()) == 0);
    CHECK(loader->TextCrc32(*memory) == Crc32(code.data(), 0x2F00));
    CHECK(image.text_base == 0x00100000);
    CHECK(image.text_size == 0x2F00);
}

TEST_CASE("a game folder: the memory mode follows the system mode") {
    for (const u32 mode : {0u, 2u, 3u, 4u, 5u}) {
        ExhParams p;
        p.system_mode = mode;
        ExHeader h;
        const auto bytes = MakeExheader(p);
        REQUIRE(ParseExHeader(bytes.data(), bytes.size(), &h, nullptr));
        std::string error;
        auto loader = Loader::FromExHeader(h, MakeCode(p), "x", &error);
        REQUIRE_MESSAGE(loader, error);
        CHECK(loader->Image().memory_mode == mode);
    }
    for (const u32 mode : {1u, 6u, 7u, 8u}) {
        ExhParams p;
        p.system_mode = mode;
        ExHeader h;
        const auto bytes = MakeExheader(p);
        REQUIRE(ParseExHeader(bytes.data(), bytes.size(), &h, nullptr));
        std::string error;
        CHECK_FALSE(Loader::FromExHeader(h, MakeCode(p), "x", &error));
        CHECK(error.find("Old 3DS") != std::string::npos);
    }
}

TEST_CASE("a game folder: what the loader refuses") {
    const ExhParams p;
    ExHeader h;
    const auto bytes = MakeExheader(p);
    REQUIRE(ParseExHeader(bytes.data(), bytes.size(), &h, nullptr));
    std::string error;

    auto code = MakeCode(p);
    code.pop_back();
    CHECK_FALSE(Loader::FromExHeader(h, code, "x", &error));
    CHECK(error.find("code.bin") != std::string::npos);

    ExHeader bad = h;
    bad.stack_size = 0x4100;
    CHECK_FALSE(Loader::FromExHeader(bad, MakeCode(p), "x", &error));
    bad = h;
    bad.text.size = bad.text.num_pages * 0x1000 + 1;
    CHECK_FALSE(Loader::FromExHeader(bad, MakeCode(p), "x", &error));

    // The FCRAM regions must be those of the program's memory mode.
    auto loader = Loader::FromExHeader(h, MakeCode(p), "x", &error);
    REQUIRE(loader);
    auto memory = NewMemory();
    FcramRegions wrong(0);
    CHECK_FALSE(loader->Map(*memory, wrong, &error));
    CHECK(error.find("memory mode") != std::string::npos);

    // A segment on top of something already mapped.
    REQUIRE(IsSuccess(memory->Map(0x00200000, 0x1000, MemPerm::ReadWrite, MemState::Private)));
    FcramRegions regions(2);
    CHECK_FALSE(loader->Map(*memory, regions, &error));
    CHECK(error.find("overlaps") != std::string::npos);
}

TEST_CASE("a game folder: SYSTEM and BASE regions, and the CPU setting of other categories") {
    ExhParams p;
    p.category = 2;
    p.extra_caps = {0xFF000000u | (3 << 8)};
    ExHeader h;
    const auto bytes = MakeExheader(p);
    REQUIRE(ParseExHeader(bytes.data(), bytes.size(), &h, nullptr));
    std::string error;
    auto loader = Loader::FromExHeader(h, MakeCode(p), "x", &error);
    REQUIRE_MESSAGE(loader, error);
    CHECK(loader->Image().memory_region == FcramRegion::Base);
    CHECK_FALSE(loader->Image().apply_app_max_cpu);
    auto memory = NewMemory();
    FcramRegions regions(2);
    REQUIRE_MESSAGE(loader->Map(*memory, regions, &error), error);
    CHECK(regions.Used(FcramRegion::Base) == 0xC000);
    CHECK(regions.Used(FcramRegion::Application) == 0);

    p.extra_caps = {0xFF000000u};  // misc flags present with region 0: not usable
    const auto bytes0 = MakeExheader(p);
    REQUIRE(ParseExHeader(bytes0.data(), bytes0.size(), &h, nullptr));
    CHECK_FALSE(Loader::FromExHeader(h, MakeCode(p), "x", &error));
}

TEST_CASE("an ELF program: Azahar's defaults") {
    const auto elf = MakeElf({{0x00100000, 0x40, 0x40, kRX},
                              {0x00101000, 0x20, 0x20, kR},
                              {0x00102000, 0x30, 0x1800, kRW}},
                             0x00100010);
    std::string error;
    auto loader = Loader::FromElf(elf, "test.elf", &error);
    REQUIRE_MESSAGE(loader, error);
    const ProcessImage& image = loader->Image();
    CHECK(image.kind == ProgramKind::Elf);
    CHECK(image.entry == 0x00100010);
    CHECK(image.kernel_version == 0x23A);
    CHECK(image.linear_heap_area == 0x30000000);
    CHECK(image.memory_mode == 2);
    CHECK(image.main_thread_priority == 48);
    CHECK(image.stack_size == 0x4000);
    CHECK(image.stack_top == 0x10000000);
    CHECK(image.apply_app_max_cpu);
    CHECK(image.core1_schedule_mode == 1);
    CHECK(image.core1_max_cpu == 89);
    CHECK(image.resource_limit_category == 0);
    CHECK(image.svc_mask.all());
    REQUIRE(image.address_mappings.size() == 3);
    CHECK(image.address_mappings[0] == AddressMapping{0x1FF50000, 0x8000, true, false});
    CHECK(image.address_mappings[1] == AddressMapping{0x1FF70000, 0x8000, true, false});
    CHECK(image.address_mappings[2] == AddressMapping{0x1F000000, 0x600000, false, false});
    CHECK(image.text_base == 0x00100000);
    CHECK(image.text_size == 0x40);

    REQUIRE(image.segments.size() == 4);
    CHECK(image.segments[0].size == 0x1000);
    CHECK(image.segments[0].perm == MemPerm::ReadExecute);
    CHECK(image.segments[1].perm == MemPerm::Read);
    CHECK(image.segments[2].size == 0x2000);  // memsz 0x1800 rounded up
    CHECK(image.segments[2].perm == MemPerm::ReadWrite);
    CHECK(image.segments[3].vaddr == 0x0FFFC000);

    auto memory = NewMemory();
    FcramRegions regions(image.memory_mode);
    REQUIRE_MESSAGE(loader->Map(*memory, regions, &error), error);
    CHECK(memory->LinearHeapArea() == 0x30000000);
    CHECK(Bytes(*memory, 0x00100000, 4) == std::vector<u8>{0xA0, 0xA1, 0xA2, 0xA3});
    CHECK(Bytes(*memory, 0x00102000, 2) == std::vector<u8>{0xB0, 0xB1});
    CHECK(Bytes(*memory, 0x00102030, 0x100) == std::vector<u8>(0x100, 0));  // beyond filesz: zero
    CHECK(regions.Used(FcramRegion::Application) == 0x1000 + 0x1000 + 0x2000 + 0x4000);
    CHECK(loader->TextBytes(*memory).size() == 0x40);
}

TEST_CASE("an ELF program: relocatable files load at the process image address") {
    const auto elf = MakeElf({{0x0, 0x10, 0x10, kRX}}, 0x8, /*type=*/3);
    std::string error;
    auto loader = Loader::FromElf(elf, "dyn.elf", &error);
    REQUIRE_MESSAGE(loader, error);
    CHECK(loader->Image().entry == 0x00100008);
    CHECK(loader->Image().segments[0].vaddr == 0x00100000);
}

TEST_CASE("an ELF program: odd segments are skipped, bad files refused") {
    std::string error;
    // A second R+X segment and an RWX one are skipped (Azahar logs them).
    auto loader = Loader::FromElf(MakeElf({{0x00100000, 0x10, 0x10, kRX},
                                           {0x00110000, 0x10, 0x10, kRX},
                                           {0x00120000, 0x10, 0x10, 7}},
                                          0x00100000),
                                  "x", &error);
    REQUIRE_MESSAGE(loader, error);
    CHECK(loader->Image().segments.size() == 2);  // text and the stack

    CHECK_FALSE(Loader::FromElf(MakeElf({{0x00100000, 0x10, 0x10, kR}}, 0x00100000), "x", &error));
    CHECK(error.find("executable") != std::string::npos);
    CHECK_FALSE(Loader::FromElf(MakeElf({{0x00100000, 0x10, 0x10, kRX}}, 0, 2, /*machine=*/3), "x", &error));
    CHECK_FALSE(Loader::FromElf(std::vector<u8>(100, 0), "x", &error));
    CHECK(error == "not an ELF file");
    auto truncated = MakeElf({{0x00100000, 0x40, 0x40, kRX}}, 0);
    truncated.resize(truncated.size() - 8);
    CHECK_FALSE(Loader::FromElf(truncated, "x", &error));
    CHECK_FALSE(Loader::OpenElf("/nonexistent/path.elf", &error));
}

TEST_CASE("the configuration memory") {
    for (const u32 mode : {0u, 2u}) {
        auto memory = NewMemory();
        FcramRegions regions(mode);
        SystemPages pages;
        REQUIRE(IsSuccess(pages.Map(*memory, regions)));
        CHECK(*memory->Query(0x1FF80000) == MemoryInfo{0x1FF80000, 0x1000, MemPerm::Read, MemState::Shared});

        auto u8_at = [&](u32 off) { return *memory->Read<u8>(0x1FF80000 + off); };
        auto u32_at = [&](u32 off) { return *memory->Read<u32>(0x1FF80000 + off); };
        CHECK(u8_at(0x02) == 0x3A);  // kernel version 2.58
        CHECK(u8_at(0x03) == 0x02);
        CHECK(*memory->Read<u64>(0x1FF80000 + 0x08) == 0x0004013000008002ull);
        CHECK(u32_at(0x10) == 2);
        CHECK(u8_at(0x14) == 1);  // retail
        CHECK(u8_at(0x16) == 1);
        CHECK(u32_at(0x18) == 0xF450);
        CHECK(u32_at(0x30) == mode);  // app memory type
        CHECK(u32_at(0x40) == (mode == 0 ? 0x04000000u : 0x06000000u));
        CHECK(u32_at(0x44) == (mode == 0 ? 0x02C00000u : 0x00C00000u));
        CHECK(u32_at(0x48) == 0x01400000);
        CHECK(u8_at(0x62) == 0x3A);
        CHECK(u8_at(0x63) == 0x02);
        CHECK(u32_at(0x64) == 2);
        CHECK(u32_at(0x68) == 0xF450);
        // Everything else is zero.
        CHECK(u32_at(0x00) == 0x023A0000);  // unk, revision, minor, major
        CHECK(u32_at(0x60) == 0x023A0000);  // the same for the firmware version
        for (const u32 off : {0x04u, 0x1Cu, 0x34u, 0x4Cu, 0x6Cu, 0xFFCu}) {
            CHECK(u32_at(off) == 0);
        }
    }
}

TEST_CASE("the shared page: the fields, and the date from init_time") {
    auto memory = NewMemory();
    FcramRegions regions(2);
    SystemPages pages;  // 2026-01-01 12:00:00 UTC
    CHECK(pages.InitTime() == 1767268800);
    REQUIRE(IsSuccess(pages.Map(*memory, regions)));
    CHECK(*memory->Query(0x1FF81000) == MemoryInfo{0x1FF81000, 0x1000, MemPerm::Read, MemState::Shared});

    const u32 base = 0x1FF81000;
    auto u8_at = [&](u32 off) { return *memory->Read<u8>(base + off); };
    auto u64_at = [&](u32 off) { return *memory->Read<u64>(base + off); };
    CHECK(u8_at(0x04) == 1);   // running hardware: product
    CHECK(u8_at(0x66) == 3);   // Wi-Fi link level: best
    CHECK(u8_at(0x67) == 2);   // Wi-Fi state: internet
    CHECK(*memory->Read<float>(base + 0x80) == 0.0f);  // 3D slider
    CHECK(u8_at(0x84) == 0);
    CHECK(u8_at(0x85) == 0x17);  // adapter, charging, level 5
    CHECK(u8_at(0x86) == 1);

    // The first update runs at tick 0 and uses the even counter's block (date_time_1, at 0x40).
    // 2026-01-01 12:00 UTC is 820584000 s after 2000-01-01; the console time counts from 1900.
    const u64 since_2000_ms = 820584000000ull;
    CHECK(pages.SystemTimeSince2000(0) == since_2000_ms);
    CHECK(pages.SystemTimeSince1900(0) == since_2000_ms + 3155673600000ull);
    CHECK(*memory->Read<u32>(base + 0x00) == 1);
    CHECK(u64_at(0x40) == since_2000_ms + 3155673600000ull);
    CHECK(u64_at(0x48) == 0);                  // update tick
    CHECK(u64_at(0x50) == 268111856);          // ticks per second
    CHECK(u64_at(0x58) == 0);
    CHECK(u64_at(0x20) == 0);                  // the other block is untouched

    // One hour later the odd counter picks date_time_0.
    pages.UpdateSharedPage(kSharedPageUpdateIntervalTicks);
    CHECK(*memory->Read<u32>(base + 0x00) == 2);
    CHECK(u64_at(0x20) == since_2000_ms + 3155673600000ull + 3600000);
    CHECK(u64_at(0x28) == kSharedPageUpdateIntervalTicks);
    CHECK(u64_at(0x30) == 268111856);
    pages.UpdateSharedPage(2 * kSharedPageUpdateIntervalTicks + 134055928);  // 2 h 0.5 s
    CHECK(*memory->Read<u32>(base + 0x00) == 3);
    CHECK(u64_at(0x40) == since_2000_ms + 3155673600000ull + 7200000 + 500);
}

TEST_CASE("the shared page: the clock before 2000 and another init_time") {
    auto memory = NewMemory();
    FcramRegions regions(2);
    SystemPages early(946684800 - 5);  // 5 s before 2000-01-01 UTC
    REQUIRE(IsSuccess(early.Map(*memory, regions)));
    CHECK(early.SystemTimeSince2000(0) == 0);
    CHECK(early.SystemTimeSince2000(268111856 * 4) == 0);
    CHECK(early.SystemTimeSince2000(268111856 * 6) == 1000);
    CHECK(*memory->Read<u64>(0x1FF81040) == 3155673600000ull);

    early.Set3DSlider(0.5f);
    CHECK(*memory->Read<float>(0x1FF81080) == 0.5f);
}

TEST_CASE("the game clock does not overflow for long runs") {
    SystemPages pages(kDefaultInitTime);
    // 30 days of game time
    const u64 ticks = 268111856ull * 86400 * 30;
    CHECK(pages.SystemTimeSince2000(ticks) == 820584000000ull + 86400ull * 30 * 1000);
}

TEST_CASE("DescribeProcessImage lists the fields and the segment map") {
    std::string error;
    auto loader = Loader::FromElf(MakeElf({{0x00100000, 0x40, 0x40, kRX}}, 0x00100000), "t.elf", &error);
    REQUIRE_MESSAGE(loader, error);
    const std::string text = DescribeProcessImage(loader->Image());
    CHECK(text.find("main thread priority : 48") != std::string::npos);
    CHECK(text.find("kernel version       : 0x23A") != std::string::npos);
    CHECK(text.find("linear heap area     : 0x30000000") != std::string::npos);
    CHECK(text.find("core 1 schedule mode : 1 (multi), max cpu 89") != std::string::npos);
    CHECK(text.find("text   0x00100000-0x00101000 size 0x00001000 r-x Code") != std::string::npos);
    CHECK(text.find("stack  0x0FFFC000-0x10000000 size 0x00004000 rw- Locked") != std::string::npos);
}
