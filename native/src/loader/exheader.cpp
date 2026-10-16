// Derived from Azahar src/core/file_sys/ncch_container.h and src/core/hle/kernel/process.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: ncch_container.h says so, process.cpp
// says GPL-2.0-or-later): the layout of the ARM11 local
// capabilities and Process::ParseKernelCaps.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp)
#include <cstring>

#include <fmt/format.h>

#include "mh4n/loader.h"

namespace mh4n {
namespace {

u16 Le16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 Le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
u64 Le64(const u8* p) {
    return Le32(p) | (static_cast<u64>(Le32(p + 4)) << 32);
}

ExHeaderSegment ReadSegment(const u8* p) {
    return ExHeaderSegment{Le32(p), Le32(p + 4), Le32(p + 8)};
}

constexpr std::size_t kLocalCapsOffset = 0x200;
constexpr std::size_t kKernelCapsOffset = kLocalCapsOffset + 0x170;
constexpr std::size_t kKernelCapsCount = 28;

KernelCaps ParseKernelCaps(const u32* caps, std::size_t len) {
    KernelCaps out;
    for (std::size_t i = 0; i < len; ++i) {
        const u32 descriptor = caps[i];
        const u32 type = descriptor >> 20;

        if (descriptor == 0xFFFFFFFF) {
            // Unused descriptor entry
        } else if ((type & 0xF00) == 0xE00) {
            // Allowed interrupts list: ignored
        } else if ((type & 0xF80) == 0xF00) {
            // Allowed syscalls mask
            std::size_t index = ((descriptor >> 24) & 7) * 24;
            u32 bits = descriptor & 0xFFFFFF;
            while (bits && index < out.svc_mask.size()) {
                out.svc_mask.set(index, bits & 1);
                ++index;
                bits >>= 1;
            }
        } else if ((type & 0xFF0) == 0xFE0) {
            out.handle_table_size = descriptor & 0x3FF;
        } else if ((type & 0xFF8) == 0xFF0) {
            out.flags = static_cast<u16>(descriptor & 0xFFFF);
        } else if ((type & 0xFFE) == 0xFF8) {
            // Mapped memory range: two descriptors
            if (i + 1 >= len || ((caps[i + 1] >> 20) & 0xFFE) != 0xFF8) {
                continue;  // incomplete: ignored
            }
            const u32 end_desc = caps[i + 1];
            ++i;
            AddressMapping mapping{};
            mapping.address = descriptor << 12;
            const u32 end_address = end_desc << 12;
            mapping.size = mapping.address < end_address ? end_address - mapping.address : 0;
            mapping.read_only = (descriptor & (1u << 20)) != 0;
            mapping.unk_flag = (end_desc & (1u << 20)) != 0;
            out.address_mappings.push_back(mapping);
        } else if ((type & 0xFFF) == 0xFFE) {
            // Mapped memory page
            out.address_mappings.push_back(AddressMapping{descriptor << 12, kPageSize, false, false});
        } else if ((type & 0xFE0) == 0xFC0) {
            out.kernel_version = descriptor & 0xFFFF;
        }
        // Anything else: Azahar logs it as unhandled and goes on.
    }
    return out;
}

}  // namespace

bool ParseExHeader(const u8* data, std::size_t size, ExHeader* out, std::string* error) {
    if (size < kExHeaderMinSize) {
        if (error) {
            *error = fmt::format("exheader too small (0x{:X} bytes, need at least 0x{:X})", size,
                                 kExHeaderMinSize);
        }
        return false;
    }
    ExHeader h;
    const char* title = reinterpret_cast<const char*>(data);
    h.title.assign(title, strnlen(title, 8));
    h.flags = data[0x0D];
    h.remaster_version = Le16(data + 0x0E);
    h.text = ReadSegment(data + 0x10);
    h.stack_size = Le32(data + 0x1C);
    h.ro = ReadSegment(data + 0x20);
    h.data = ReadSegment(data + 0x30);
    h.bss_size = Le32(data + 0x3C);

    const u8* lc = data + kLocalCapsOffset;
    h.program_id = Le64(lc);
    h.core_version = Le32(lc + 8);
    h.enable_l2_cache = (lc[0x0C] & 1) != 0;
    h.enable_804mhz_cpu = (lc[0x0C] & 2) != 0;
    h.n3ds_mode = lc[0x0D];
    h.ideal_processor = lc[0x0E] & 3;
    h.affinity_mask = (lc[0x0E] >> 2) & 3;
    h.system_mode = lc[0x0E] >> 4;
    h.priority = lc[0x0F];
    const u16 core1 = Le16(lc + 0x10);
    h.max_cpu = core1 & 0x7F;
    h.schedule_mode = (core1 >> 7) & 1;
    h.resource_limit_category = lc[0x16F];

    u32 caps[kKernelCapsCount];
    for (std::size_t i = 0; i < kKernelCapsCount; ++i) {
        caps[i] = Le32(data + kKernelCapsOffset + 4 * i);
    }
    h.kernel_caps = ParseKernelCaps(caps, kKernelCapsCount);
    *out = std::move(h);
    return true;
}

}  // namespace mh4n
