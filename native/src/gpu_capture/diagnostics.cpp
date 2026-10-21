// Derived from Azahar src/core/hle/service/gsp/gsp_gpu.cpp, src/core/hle/service/gsp/gsp_interrupt.h,
// src/core/hle/service/gsp/gsp_command.h, src/video_core/pica/regs_external.h and
// src/video_core/renderer_software/sw_blitter.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of these files says "GPLv2 or any later version").
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (gsp_gpu.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gsp_interrupt.h, regs_external.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
// Copyright 2023 Citra Emulator Project (sw_blitter.cpp)
#include "gpu_capture/diagnostics.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace mh4n::gpu_capture {

namespace {

// ---- the relay queue's fields (InterruptRelayQueue) ------------------------------------------------
constexpr u32 kQueueIndex = 0;
constexpr u32 kQueueNumber = 1;
constexpr u32 kQueueError = 2;
constexpr u32 kQueueSlot0 = 12;  // after index, number, error, config and the two u32 counters
constexpr u8 kQueueFullError = 1;

constexpr u32 kPageBytes = 4096;

u64 AlignUp8(u64 value) {
    return (value + 7) & ~u64{7};
}

u32 BytesPerPixel(u32 format) {
    switch (format) {
    case 0:
        return 4;
    case 1:
        return 3;
    case 2:
    case 3:
    case 4:
        return 2;
    default:
        return 0;
    }
}

// "0x20100000", or "0x20100000 (0x14100000 in the command)" when the command held another number.
std::string Address(u32 physical, std::optional<u32> in_command) {
    if (in_command && *in_command != physical) {
        return fmt::format("0x{:08x} (0x{:08x} in the command)", physical, *in_command);
    }
    return fmt::format("0x{:08x}", physical);
}

const char* ScalingName(u32 scaling) {
    switch (scaling) {
    case 0:
        return "none";
    case 1:
        return "halve the width (box filter)";
    case 2:
        return "halve the width and the height (box filter)";
    default:
        return "invalid (3: Azahar does not implement it)";
    }
}

const char* YesNo(bool value) {
    return value ? "yes" : "no";
}

// The display transfer, from the words the GPU's registers hold (the addresses already physical).
CommandInfo DescribeDisplayTransfer(u32 input_address, std::optional<u32> input_in_command,
                                    u32 output_address, std::optional<u32> output_in_command,
                                    u32 input_size, u32 output_size, u32 flags) {
    CommandInfo info;
    info.type = CommandInfo::Type::DisplayTransfer;

    const u32 input_width = input_size & 0xFFFF, input_height = input_size >> 16;
    const u32 output_width = output_size & 0xFFFF, output_height = output_size >> 16;
    const bool flip = (flags & 1) != 0;
    const bool input_linear = (flags & 2) != 0;
    const bool crop = (flags & 4) != 0;
    const bool texture_copy_bit = (flags & 8) != 0;
    const bool dont_swizzle = (flags & 0x20) != 0;
    const u32 input_format = (flags >> 8) & 7;
    const u32 output_format = (flags >> 12) & 7;
    const bool block32 = (flags & (1u << 16)) != 0;
    const u32 scaling = (flags >> 24) & 3;
    // Azahar: input linear and swizzling -> output tiled; input linear and not swizzling -> linear;
    // input tiled and swizzling -> linear; input tiled and not swizzling -> tiled.
    const bool output_tiled = input_linear != dont_swizzle;

    const u32 horizontal = scaling != 0 ? 1 : 0;
    const u32 vertical = scaling == 2 ? 1 : 0;
    const u32 scaled_width = output_width >> horizontal;
    const u32 scaled_height = output_height >> vertical;
    const u32 source_bpp = BytesPerPixel(input_format);
    const u32 destination_bpp = BytesPerPixel(output_format);

    std::string text;
    text += fmt::format("kind: display transfer (flags bit 3 {})",
                        texture_copy_bit ? "set: Azahar runs a texture copy with the registers as "
                                           "they were" : "clear");
    text += fmt::format("\ninput: {} {} {}x{}, {}", Address(input_address, input_in_command),
                        PixelFormatName(input_format), input_width, input_height,
                        input_linear ? "linear" : "tiled 8x8");
    text += fmt::format("\noutput: {} {} {}x{} (as written {}x{}), {}",
                        Address(output_address, output_in_command), PixelFormatName(output_format),
                        scaled_width, scaled_height, output_width, output_height,
                        output_tiled ? "tiled 8x8" : "linear");
    if (destination_bpp != 0) {
        if (output_tiled) {
            text += fmt::format(", tile-row stride {} bytes, no gap",
                                scaled_width * destination_bpp * 8);
        } else {
            text += fmt::format(", row stride {} bytes, no gap", scaled_width * destination_bpp);
        }
    }
    text += fmt::format("\nscaling: {}; vertical flip: {}; crop input lines: {}; dont_swizzle: {}; "
                        "block32 bit: {}; flags 0x{:08x}",
                        ScalingName(scaling), YesNo(flip), YesNo(crop), YesNo(dont_swizzle),
                        YesNo(block32), flags);
    info.description = std::move(text);

    // The range the loops can write (the same bound the engine uses): whole 8x8 tiles for a tiled
    // output, the image for a linear one. The refused requests write nothing.
    const bool refused = input_width == 0 || input_height == 0 || output_width == 0 ||
                         output_height == 0 || source_bpp == 0 || destination_bpp == 0 ||
                         scaling > 2 || (input_linear && scaling != 0) || texture_copy_bit;
    if (!refused && scaled_width != 0 && scaled_height != 0) {
        u32 begin = output_address;
        if (flip && crop) {
            // u32 arithmetic, as Azahar's.
            begin += (input_width - output_width) * (output_height - 1) * destination_bpp;
        }
        const u64 length = output_tiled ? AlignUp8(scaled_width) * AlignUp8(scaled_height) *
                                              destination_bpp
                                        : u64{scaled_width} * scaled_height * destination_bpp;
        info.output.push_back({begin, length});
    }
    return info;
}

CommandInfo DescribeTextureCopy(u32 input_address, std::optional<u32> input_in_command,
                                u32 output_address, std::optional<u32> output_in_command, u32 size,
                                u32 input_words, u32 output_words, u32 flags) {
    CommandInfo info;
    info.type = CommandInfo::Type::TextureCopy;

    const u32 aligned = size & ~15u;
    const u32 input_gap = (input_words >> 16) * 16;
    const u32 output_gap = (output_words >> 16) * 16;
    // A zero gap means contiguous: the width is then the whole size (Azahar's rule).
    const u32 input_width = input_gap == 0 ? aligned : (input_words & 0xFFFF) * 16;
    const u32 output_width = output_gap == 0 ? aligned : (output_words & 0xFFFF) * 16;

    std::string text = "kind: texture copy (bytes copied unchanged, line by line, with the gaps)";
    text += fmt::format("\ninput: {}, line width {} bytes, gap {} bytes",
                        Address(input_address, input_in_command), input_width, input_gap);
    text += fmt::format("\noutput: {}, line width {} bytes, gap {} bytes",
                        Address(output_address, output_in_command), output_width, output_gap);
    text += fmt::format("\nsize: {} bytes (the low 4 bits are ignored: {} bytes copied); flags 0x{:08x}",
                        size, aligned, flags);
    info.description = std::move(text);

    if (aligned != 0 && input_width != 0 && output_width != 0) {
        const u64 rows = (u64{aligned} + output_width - 1) / output_width;
        info.output.push_back({output_address, u64{aligned} + (rows - 1) * output_gap});
    }
    return info;
}

CommandInfo DescribeFill(u32 unit, u32 start_physical, std::optional<u32> start_in_command,
                         u32 end_physical, u32 control) {
    CommandInfo info;
    info.type = CommandInfo::Type::MemoryFill;
    const char* width = (control & 0x100) ? "24-bit" : ((control & 0x200) ? "32-bit" : "16-bit");
    std::string text = fmt::format("kind: memory fill, unit {}, {} units", unit, width);
    text += fmt::format("\nrange: start {} end 0x{:08x}", Address(start_physical, start_in_command),
                        end_physical);
    if (end_physical > start_physical) {
        text += fmt::format(" ({} bytes)", end_physical - start_physical);
        info.output.push_back({start_physical, u64{end_physical} - start_physical});
    } else {
        text += " (empty or backwards: nothing is written)";
    }
    text += fmt::format("\ncontrol word 0x{:04x}", control);
    info.description = std::move(text);
    return info;
}

} // namespace

// ---- the relay queue ----------------------------------------------------------------------------

void PushRelayInterrupt(u8* queue, u32 id) {
    if (queue[kQueueNumber] >= kRelayQueueSlots) {
        queue[kQueueError] = kQueueFullError;
        return;
    }
    u8 next = queue[kQueueIndex];
    next = static_cast<u8>(next + queue[kQueueNumber]);
    next = static_cast<u8>(next % kRelayQueueSlots);
    queue[kQueueNumber] = static_cast<u8>(queue[kQueueNumber] + 1);
    queue[kQueueSlot0 + next] = static_cast<u8>(id);
}

bool ExplainedByInterruptQueue(std::span<const u8> before, std::span<const u8> after,
                               std::span<const u32> ids) {
    if (ids.empty() || before.size() != kPageBytes || after.size() != kPageBytes ||
        std::memcmp(before.data(), after.data(), kPageBytes) == 0) {
        return false;
    }
    std::vector<u8> page(kPageBytes);
    for (u32 queue = 0; queue < kRelayQueueCount; ++queue) {
        std::memcpy(page.data(), before.data(), kPageBytes);
        for (const u32 id : ids) {
            PushRelayInterrupt(page.data() + queue * kRelayQueueBytes, id);
        }
        if (std::memcmp(page.data(), after.data(), kPageBytes) == 0) {
            return true;
        }
    }
    return false;
}

PageDifference ComparePages(std::span<const u8> a, std::span<const u8> b) {
    PageDifference difference;
    const size_t size = std::min(a.size(), b.size());
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) {
            if (difference.count == 0) {
                difference.first = static_cast<u32>(i);
            }
            difference.last = static_cast<u32>(i);
            ++difference.count;
        }
    }
    return difference;
}

// ---- decoded commands ---------------------------------------------------------------------------

std::string DescribeRange(u32 begin, u64 length) {
    const u64 end = static_cast<u64>(begin) + length;
    const u64 first_page = begin / kPageBytes;
    const u64 last_page = length == 0 ? first_page : (end - 1) / kPageBytes;
    return fmt::format("0x{:08x} to 0x{:08x} ({} bytes, {} page{})", begin, end, length,
                       length == 0 ? 0 : last_page - first_page + 1,
                       length != 0 && last_page == first_page ? "" : "s");
}

std::string PixelFormatName(u32 format) {
    switch (format) {
    case 0:
        return "RGBA8";
    case 1:
        return "RGB8";
    case 2:
        return "RGB565";
    case 3:
        return "RGB5A1";
    case 4:
        return "RGBA4";
    default:
        return fmt::format("invalid({})", format);
    }
}

CommandInfo DescribeGxCommand(std::span<const u32, 8> words, const ToPhysical& to_physical) {
    auto physical = [&](u32 address) {
        if (address == 0) {
            return u32{0};
        }
        return to_physical ? to_physical(address).value_or(address) : address;
    };
    const u32 id = words[0] & 0xFF;
    CommandInfo info;
    switch (id) {
    case 0: { // RequestDma: source, destination, size
        info.type = CommandInfo::Type::Dma;
        const u32 source = physical(words[1]);
        const u32 destination = physical(words[2]);
        info.description = fmt::format("kind: DMA\nsource: {}\ndestination: {}\nsize: {} bytes",
                                       Address(source, words[1]), Address(destination, words[2]),
                                       words[3]);
        if (words[3] != 0) {
            info.output.push_back({destination, words[3]});
        }
        break;
    }
    case 2: { // MemoryFill: two units
        info.type = CommandInfo::Type::MemoryFill;
        std::string text;
        const u32 controls[2] = {words[7] & 0xFFFF, words[7] >> 16};
        for (u32 unit = 0; unit < 2; ++unit) {
            const u32 start = words[1 + 3 * unit], end = words[3 + 3 * unit];
            if (start == 0) {
                continue;
            }
            const CommandInfo one =
                DescribeFill(unit, physical(start), start, physical(end), controls[unit]);
            text += (text.empty() ? "" : "\n") + one.description;
            info.output.insert(info.output.end(), one.output.begin(), one.output.end());
        }
        info.description = text.empty() ? "kind: memory fill with no buffer" : text;
        break;
    }
    case 3: { // DisplayTransfer: in address, out address, in size, out size, flags
        info = DescribeDisplayTransfer(physical(words[1]), words[1], physical(words[2]), words[2],
                                       words[3], words[4], words[5]);
        break;
    }
    case 4: { // TextureCopy: in address, out address, size, in width/gap, out width/gap, flags
        info = DescribeTextureCopy(physical(words[1]), words[1], physical(words[2]), words[2],
                                   words[3], words[4], words[5], words[6]);
        break;
    }
    default:
        break;
    }
    return info;
}

CommandInfo DescribeTransferRegisters(std::span<const u32, 11> regs) {
    // Register order: input address, output address, output size, input size, flags, unused,
    // trigger, unused, texture copy size, input width/gap, output width/gap. Addresses are >> 3.
    const u32 input = regs[0] * 8, output = regs[1] * 8;
    if (regs[4] & 8) {
        return DescribeTextureCopy(input, std::nullopt, output, std::nullopt, regs[8], regs[9],
                                   regs[10], regs[4]);
    }
    return DescribeDisplayTransfer(input, std::nullopt, output, std::nullopt, regs[3], regs[2],
                                   regs[4]);
}

CommandInfo DescribeFillRegisters(u32 unit, std::span<const u32, 4> regs) {
    return DescribeFill(unit, regs[0] * 8, std::nullopt, regs[1] * 8, regs[3]);
}

} // namespace mh4n::gpu_capture
