// Derived from Azahar src/core/loader/ncch.cpp and src/core/hle/kernel/process.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): AppLoader_NCCH::LoadExec (the
// segment layout, the memory mode, what is handed to the kernel) and Process::Run /
// HeapAllocate (mapping each segment, then the stack, from the process's memory region).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ncch.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp)
#include <algorithm>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>

#include "../common/utf8.h"
#include "mh4n/crc32.h"
#include "mh4n/loader.h"
#include "mh4n/log.h"

namespace mh4n {
namespace {

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

bool ReadFile(const std::filesystem::path& path, std::vector<u8>* out, std::string* error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        SetError(error, fmt::format("cannot open {}", utf8::PathToUtf8(path)));
        return false;
    }
    const std::streamsize size = file.tellg();
    file.seekg(0);
    out->resize(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(out->data()), size)) {
        SetError(error, fmt::format("cannot read {}", utf8::PathToUtf8(path)));
        return false;
    }
    return true;
}

bool IsOldMemoryMode(u32 mode) {
    return mode == 0 || (mode >= 2 && mode <= 5);
}

FcramRegion RegionFromNumber(u32 number) {
    return static_cast<FcramRegion>(number - 1);
}

const char* RegionName(FcramRegion region) {
    switch (region) {
    case FcramRegion::Application:
        return "application";
    case FcramRegion::System:
        return "system";
    case FcramRegion::Base:
        return "base";
    }
    return "?";
}

std::string PermString(MemPerm perm) {
    std::string s = "---";
    if ((perm & MemPerm::Read) != MemPerm::None) s[0] = 'r';
    if ((perm & MemPerm::Write) != MemPerm::None) s[1] = 'w';
    if ((perm & MemPerm::Execute) != MemPerm::None) s[2] = 'x';
    return s;
}

const char* StateName(MemState state) {
    switch (state) {
    case MemState::Code:
        return "Code";
    case MemState::Private:
        return "Private";
    case MemState::Locked:
        return "Locked";
    case MemState::Shared:
        return "Shared";
    default:
        return "?";
    }
}

constexpr u32 AlignUp(u32 value) {
    return (value + kPageMask) & ~kPageMask;
}

}  // namespace

std::unique_ptr<Loader> Loader::OpenGameFolder(const std::string& dir, std::string* error) {
    // `dir` is UTF-8 text (D45): the path is made from it without the ANSI code page of Windows.
    const std::filesystem::path folder = utf8::PathFromUtf8(dir);
    std::vector<u8> exheader_bytes, code;
    if (!ReadFile(folder / "exheader.bin", &exheader_bytes, error) ||
        !ReadFile(folder / "code.bin", &code, error)) {
        return nullptr;
    }
    ExHeader header;
    if (!ParseExHeader(exheader_bytes.data(), exheader_bytes.size(), &header, error)) {
        return nullptr;
    }
    return FromExHeader(header, std::move(code), header.title, error);
}

std::unique_ptr<Loader> Loader::FromExHeader(const ExHeader& header, std::vector<u8> code,
                                             std::string name, std::string* error) {
    if (!IsOldMemoryMode(header.system_mode)) {
        SetError(error, fmt::format("the exheader's system mode {} is not an Old 3DS memory mode",
                                    header.system_mode));
        return nullptr;
    }
    const u32 region_number = header.kernel_caps.MemoryRegion();
    if (region_number < 1 || region_number > 3) {
        SetError(error, fmt::format("the exheader's memory region {} is not 1, 2 or 3", region_number));
        return nullptr;
    }
    const ExHeaderSegment* segs[3] = {&header.text, &header.ro, &header.data};
    for (const ExHeaderSegment* s : segs) {
        if (static_cast<u64>(s->num_pages) * kPageSize < s->size) {
            SetError(error, "an exheader segment is larger than its page count");
            return nullptr;
        }
    }
    const u64 expected = (static_cast<u64>(header.text.num_pages) + header.ro.num_pages +
                          header.data.num_pages) * kPageSize;
    if (code.size() != expected) {
        SetError(error, fmt::format("code.bin is 0x{:X} bytes but the exheader's segments take 0x{:X}; "
                                    "were they made from the same title?",
                                    code.size(), expected));
        return nullptr;
    }
    if (header.stack_size == 0 || (header.stack_size & kPageMask) != 0 ||
        header.stack_size > kStackTop - kHeapVaddr) {
        SetError(error, fmt::format("the exheader's stack size 0x{:X} is not usable", header.stack_size));
        return nullptr;
    }
    const u32 bss_pages_size = AlignUp(header.bss_size);

    std::unique_ptr<Loader> loader(new Loader);
    ProcessImage& image = loader->image_;
    image.name = std::move(name);
    image.kind = ProgramKind::GameFolder;
    image.program_id = header.program_id;
    image.entry = header.text.address;
    image.stack_size = header.stack_size;
    image.main_thread_priority = static_cast<s32>(header.priority);
    image.ideal_processor = header.ideal_processor;
    image.memory_mode = header.system_mode;
    image.kernel_version = header.kernel_caps.kernel_version.value_or(0);
    image.linear_heap_area = LinearHeapAreaForKernelVersion(image.kernel_version);
    image.memory_region = RegionFromNumber(region_number);
    image.resource_limit_category = header.resource_limit_category;
    // LoadExec applies the setting for the application category only.
    image.apply_app_max_cpu = header.resource_limit_category == 0;
    image.core1_schedule_mode = header.schedule_mode;
    image.core1_max_cpu = header.max_cpu;
    image.svc_mask = header.kernel_caps.svc_mask;
    image.address_mappings = header.kernel_caps.address_mappings;
    image.handle_table_size = header.kernel_caps.handle_table_size;
    image.text_base = header.text.address;
    image.text_size = header.text.size;

    const u32 text_size = header.text.num_pages * kPageSize;
    const u32 ro_size = header.ro.num_pages * kPageSize;
    const u32 data_size = header.data.num_pages * kPageSize;
    std::size_t offset = 0;
    struct Seg {
        const char* name;
        u32 vaddr;
        u32 size;
        u32 file_size;
        MemPerm perm;
        MemState state;
    };
    const Seg list[3] = {
        {"text", header.text.address, text_size, text_size, MemPerm::ReadExecute, MemState::Code},
        {"ro", header.ro.address, ro_size, ro_size, MemPerm::Read, MemState::Code},
        {"data", header.data.address, data_size + bss_pages_size, data_size, MemPerm::ReadWrite,
         MemState::Private},
    };
    for (const Seg& s : list) {
        // Azahar maps a segment of size 0 as nothing (the allocation of 0 bytes yields no piece).
        if (s.size != 0) {
            if (static_cast<u64>(s.vaddr) + s.size > 0x100000000ull) {
                SetError(error, fmt::format("the {} segment wraps around the address space", s.name));
                return nullptr;
            }
            image.segments.push_back({s.name, s.vaddr, s.size, s.perm, s.state});
            loader->pieces_.push_back({s.vaddr, s.size, offset, s.file_size});
        }
        offset += s.file_size;
    }
    image.segments.push_back({"stack", kStackTop - header.stack_size, header.stack_size,
                              MemPerm::ReadWrite, MemState::Locked});
    loader->pieces_.push_back({kStackTop - header.stack_size, header.stack_size, 0, 0});
    loader->code_ = std::move(code);
    return loader;
}

bool Loader::Map(GuestMemory& memory, FcramRegions& regions, std::string* error) {
    if (mapped_) {
        SetError(error, "the process is already mapped");
        return false;
    }
    if (regions.MemoryMode() != image_.memory_mode) {
        SetError(error, fmt::format("the FCRAM regions are for memory mode {} but the program wants {}",
                                    regions.MemoryMode(), image_.memory_mode));
        return false;
    }
    for (std::size_t i = 0; i < image_.segments.size(); ++i) {
        const SegmentMapping& seg = image_.segments[i];
        const Piece& piece = pieces_[i];
        // Process::HeapAllocate: the target must be a free block that holds the whole segment.
        const auto info = memory.Query(seg.vaddr);
        if (!info || info->state != MemState::Free ||
            static_cast<u64>(info->base) + info->size < static_cast<u64>(seg.vaddr) + seg.size) {
            SetError(error, fmt::format("the {} segment at 0x{:08X}+0x{:X} overlaps mapped memory",
                                        seg.name, seg.vaddr, seg.size));
            return false;
        }
        const std::vector<FcramRange> pieces = regions.HeapAllocate(image_.memory_region, seg.size);
        if (pieces.empty()) {
            SetError(error, fmt::format("not enough {} memory for the {} segment (0x{:X} bytes)",
                                        RegionName(image_.memory_region), seg.name, seg.size));
            return false;
        }
        u32 target = seg.vaddr;
        for (const FcramRange& range : pieces) {
            const Result result = memory.Map(target, range.size, seg.perm, seg.state,
                                             static_cast<u64>(kFcramPaddr) + range.offset);
            if (IsError(result)) {
                SetError(error, fmt::format("mapping the {} segment failed with 0x{:08X}", seg.name, result));
                return false;
            }
            target += range.size;
        }
        if (piece.data_size != 0) {
            if (!memory.WriteBlock(seg.vaddr, code_.data() + piece.offset,
                                   static_cast<u32>(piece.data_size))) {
                SetError(error, fmt::format("writing the {} segment failed", seg.name));
                return false;
            }
        }
    }
    memory.SetLinearHeapArea(image_.linear_heap_area);
    mapped_ = true;
    code_.clear();
    code_.shrink_to_fit();
    return true;
}

std::span<const u8> Loader::TextBytes(const GuestMemory& memory) const {
    if (!mapped_ || image_.text_size == 0) {
        return {};
    }
    return {memory.Base() + image_.text_base, image_.text_size};
}

u32 Loader::TextCrc32(const GuestMemory& memory) const {
    const std::span<const u8> text = TextBytes(memory);
    return Crc32(text.data(), text.size());
}

std::string DescribeProcessImage(const ProcessImage& image) {
    static constexpr const char* kCategoryNames[] = {"application", "sys applet", "lib applet", "other"};
    std::string out;
    out += fmt::format("name                 : {}\n", image.name);
    out += fmt::format("kind                 : {}\n", image.kind == ProgramKind::Elf ? "elf" : "game folder");
    out += fmt::format("program id           : {:016X}\n", image.program_id);
    out += fmt::format("entry                : 0x{:08X}\n", image.entry);
    out += fmt::format("stack                : top 0x{:08X}, size 0x{:X}\n", image.stack_top, image.stack_size);
    out += fmt::format("main thread priority : {}\n", image.main_thread_priority);
    out += fmt::format("ideal processor      : {}\n", image.ideal_processor);
    out += fmt::format("main thread fpscr    : 0x{:08X}\n", image.main_thread_fpscr);
    out += fmt::format("memory mode          : {}\n", image.memory_mode);
    out += fmt::format("kernel version       : 0x{:X}\n", image.kernel_version);
    out += fmt::format("linear heap area     : 0x{:08X}\n", image.linear_heap_area);
    out += fmt::format("memory region        : {}\n", RegionName(image.memory_region));
    out += fmt::format("resource limit cat.  : {} ({})\n", image.resource_limit_category,
                       image.resource_limit_category < 4 ? kCategoryNames[image.resource_limit_category] : "?");
    out += fmt::format("core 1 schedule mode : {} ({}), max cpu {}, applied {}\n", image.core1_schedule_mode,
                       image.core1_schedule_mode ? "multi" : "single", image.core1_max_cpu,
                       image.apply_app_max_cpu ? "yes" : "no");
    out += fmt::format("handle table size    : {}\n", image.handle_table_size
                                                          ? std::to_string(*image.handle_table_size)
                                                          : std::string("(none)"));
    out += fmt::format("allowed SVCs         : {}\n", image.svc_mask.count());
    out += fmt::format("text bytes           : 0x{:08X}+0x{:X}\n", image.text_base, image.text_size);
    out += "segments:\n";
    for (const SegmentMapping& s : image.segments) {
        out += fmt::format("  {:<6} 0x{:08X}-0x{:08X} size 0x{:08X} {} {}\n", s.name, s.vaddr,
                           s.vaddr + s.size, s.size, PermString(s.perm), StateName(s.state));
    }
    out += fmt::format("address mappings     : {}\n", image.address_mappings.size());
    for (const AddressMapping& m : image.address_mappings) {
        out += fmt::format("  0x{:08X} size 0x{:X}{}{}\n", m.address, m.size, m.read_only ? " read-only" : "",
                           m.unk_flag ? " unk" : "");
    }
    return out;
}

}  // namespace mh4n
