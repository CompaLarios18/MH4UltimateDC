// Derived from Azahar src/core/loader/elf.cpp and src/core/hle/kernel/process.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): ElfReader::LoadInto (which
// PT_LOAD segment becomes text, ro or data), AppLoader_ELF::Load (the defaults of ELF programs)
// and Process::Set3dsxKernelCaps.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (elf.cpp)
// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project (elf.cpp, second notice:
// ElfReader comes from Dolphin; GPL-2.0-or-later)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp)
#include <filesystem>
#include <fstream>

#include <fmt/format.h>

#include "mh4n/loader.h"
#include "mh4n/log.h"

namespace mh4n {
namespace {

constexpr u32 kPtLoad = 1;
constexpr u32 kPfX = 1, kPfW = 2, kPfR = 4;
constexpr u16 kEtExec = 2;
constexpr u16 kEmArm = 40;
constexpr std::size_t kEhdrSize = 52;
constexpr std::size_t kPhdrSize = 32;

u16 Le16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 Le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

}  // namespace

std::unique_ptr<Loader> Loader::OpenElf(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        SetError(error, fmt::format("cannot open {}", path));
        return nullptr;
    }
    const std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        SetError(error, fmt::format("cannot read {}", path));
        return nullptr;
    }
    return FromElf(std::move(bytes), std::filesystem::path(path).filename().string(), error);
}

std::unique_ptr<Loader> Loader::FromElf(std::vector<u8> bytes, std::string name, std::string* error) {
    if (bytes.size() < kEhdrSize || bytes[0] != 0x7F || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F') {
        SetError(error, "not an ELF file");
        return nullptr;
    }
    if (bytes[4] != 1 || bytes[5] != 1 || Le16(&bytes[18]) != kEmArm) {
        SetError(error, "not a 32-bit little-endian ARM ELF file");
        return nullptr;
    }
    const u16 type = Le16(&bytes[16]);
    const u32 entry = Le32(&bytes[24]);
    const u32 phoff = Le32(&bytes[28]);
    const u16 phentsize = Le16(&bytes[42]);
    const u16 phnum = Le16(&bytes[44]);
    if (phnum != 0 && (phentsize < kPhdrSize || static_cast<u64>(phoff) + static_cast<u64>(phnum) * phentsize > bytes.size())) {
        SetError(error, "the ELF program header table is outside the file");
        return nullptr;
    }
    // A relocatable file is loaded at the process image address, as Azahar's ElfReader does.
    const u32 base = type != kEtExec ? kProcessImageVaddr : 0;

    std::unique_ptr<Loader> loader(new Loader);
    ProcessImage& image = loader->image_;
    image.name = std::move(name);
    image.kind = ProgramKind::Elf;
    image.program_id = 0;
    image.entry = base + entry;
    image.stack_size = kDefaultStackSize;
    image.main_thread_priority = kElfMainThreadPriority;
    image.ideal_processor = 0;
    image.memory_mode = kElfMemoryMode;
    image.kernel_version = kElfKernelVersion;
    image.linear_heap_area = LinearHeapAreaForKernelVersion(kElfKernelVersion);
    image.memory_region = FcramRegion::Application;
    image.resource_limit_category = 0;
    image.apply_app_max_cpu = true;
    image.core1_schedule_mode = 1;
    image.core1_max_cpu = 89;
    // Process::Set3dsxKernelCaps
    image.svc_mask.set();
    image.address_mappings = {
        {0x1FF50000, 0x8000, true, false},     // part of DSP RAM
        {0x1FF70000, 0x8000, true, false},     // part of DSP RAM
        {0x1F000000, 0x600000, false, false},  // entire VRAM
    };

    bool have[3] = {false, false, false};  // text, ro, data
    struct Slot {
        SegmentMapping mapping;
        Piece piece;
    };
    std::optional<Slot> slots[3];
    for (u16 i = 0; i < phnum; ++i) {
        const u8* p = &bytes[phoff + static_cast<std::size_t>(i) * phentsize];
        if (Le32(p) != kPtLoad) {
            continue;
        }
        const u32 offset = Le32(p + 4), vaddr = Le32(p + 8), filesz = Le32(p + 16),
                  memsz = Le32(p + 20), flags = Le32(p + 24) & (kPfR | kPfW | kPfX);
        int index;
        MemPerm perm;
        MemState state;
        const char* seg_name;
        if (flags == (kPfR | kPfX)) {
            index = 0, perm = MemPerm::ReadExecute, state = MemState::Code, seg_name = "text";
        } else if (flags == kPfR) {
            index = 1, perm = MemPerm::Read, state = MemState::Code, seg_name = "ro";
        } else if (flags == (kPfR | kPfW)) {
            index = 2, perm = MemPerm::ReadWrite, state = MemState::Private, seg_name = "data";
        } else {
            MH4N_LOG(Error, "Loader", "unexpected ELF PT_LOAD segment {} with flags {:X}", i, flags);
            continue;
        }
        if (have[index]) {
            MH4N_LOG(Error, "Loader", "ELF has more than one segment of the same type, skipping segment {}", i);
            continue;
        }
        if (filesz > memsz || static_cast<u64>(offset) + filesz > bytes.size()) {
            SetError(error, fmt::format("ELF segment {} lies outside the file", i));
            return nullptr;
        }
        const u32 aligned = (memsz + kPageMask) & ~kPageMask;
        const u64 address = static_cast<u64>(base) + vaddr;
        if (aligned == 0) {
            continue;  // nothing to map
        }
        if (address + aligned > 0x100000000ull) {
            SetError(error, fmt::format("ELF segment {} wraps around the address space", i));
            return nullptr;
        }
        have[index] = true;
        slots[index] = Slot{{seg_name, static_cast<u32>(address), aligned, perm, state},
                            {static_cast<u32>(address), aligned, offset, filesz}};
        if (index == 0) {
            image.text_base = static_cast<u32>(address);
            image.text_size = filesz;
        }
    }
    if (!slots[0]) {
        SetError(error, "the ELF file has no executable (R+X) PT_LOAD segment");
        return nullptr;
    }
    for (const auto& slot : slots) {
        if (slot) {
            image.segments.push_back(slot->mapping);
            loader->pieces_.push_back(slot->piece);
        }
    }
    image.segments.push_back({"stack", kStackTop - kDefaultStackSize, kDefaultStackSize,
                              MemPerm::ReadWrite, MemState::Locked});
    loader->pieces_.push_back({kStackTop - kDefaultStackSize, kDefaultStackSize, 0, 0});
    loader->code_ = std::move(bytes);
    return loader;
}

}  // namespace mh4n
