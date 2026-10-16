// Derived from Azahar src/video_core/renderer_software/sw_blitter.cpp, src/video_core/gpu.cpp,
// src/video_core/pica/regs_external.h, src/video_core/utils.h and src/common/color.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of these files says
// "GPLv2 or any later version").
// Copyright 2023 Citra Emulator Project (sw_blitter.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, regs_external.h)
// Copyright 2014 Citra Emulator Project (utils.h, color.h)
#include "gpu/gx/blitter.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "mh4n/log.h"

namespace mh4n::gpu::gx {

namespace {

constexpr const char* kLog = "GxEngine";

// Azahar's Memory::FCRAM_PADDR and FCRAM_PADDR_END (the Old 3DS size, also on the New 3DS).
constexpr u32 kFcramPaddr = 0x20000000;
constexpr u32 kFcramPaddrEnd = kFcramPaddr + 0x08000000;

constexpr u32 AlignDown(u32 value, u32 size) {
    return value - value % size;
}
constexpr u64 AlignUp8(u64 value) {
    return (value + 7) & ~u64{7};
}

// ---- colours (Common::Vec4<u8> in Azahar: r, g, b, a) -----------------------------------------

struct Color {
    int r, g, b, a;
};

constexpr u8 Convert1To8(u8 value) {
    return static_cast<u8>(value * 255);
}
constexpr u8 Convert4To8(u8 value) {
    return static_cast<u8>((value << 4) | value);
}
constexpr u8 Convert5To8(u8 value) {
    return static_cast<u8>((value << 3) | (value >> 2));
}
constexpr u8 Convert6To8(u8 value) {
    return static_cast<u8>((value << 2) | (value >> 4));
}
constexpr u8 Convert8To1(u8 value) {
    return value >> 7;
}
constexpr u8 Convert8To4(u8 value) {
    return value >> 4;
}
constexpr u8 Convert8To5(u8 value) {
    return value >> 3;
}
constexpr u8 Convert8To6(u8 value) {
    return value >> 2;
}

// The decoders read at most 4 bytes; `bytes` always holds 4 (zero past the end of the memory).
Color DecodePixel(u32 format, const u8* bytes) {
    const u32 pixel16 = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8);
    switch (format) {
    case static_cast<u32>(PixelFormat::RGBA8):
        return {bytes[3], bytes[2], bytes[1], bytes[0]};
    case static_cast<u32>(PixelFormat::RGB8):
        return {bytes[2], bytes[1], bytes[0], 255};
    case static_cast<u32>(PixelFormat::RGB565):
        return {Convert5To8((pixel16 >> 11) & 0x1F), Convert6To8((pixel16 >> 5) & 0x3F),
                Convert5To8(pixel16 & 0x1F), 255};
    case static_cast<u32>(PixelFormat::RGB5A1):
        return {Convert5To8((pixel16 >> 11) & 0x1F), Convert5To8((pixel16 >> 6) & 0x1F),
                Convert5To8((pixel16 >> 1) & 0x1F), Convert1To8(pixel16 & 0x1)};
    case static_cast<u32>(PixelFormat::RGBA4):
        return {Convert4To8((pixel16 >> 12) & 0xF), Convert4To8((pixel16 >> 8) & 0xF),
                Convert4To8((pixel16 >> 4) & 0xF), Convert4To8(pixel16 & 0xF)};
    default:
        return {0, 0, 0, 0};
    }
}

// Writes the encoded pixel into `bytes` (4 bytes of room) and returns how many are used.
u32 EncodePixel(u32 format, const Color& color, u8* bytes) {
    const u8 r = static_cast<u8>(color.r);
    const u8 g = static_cast<u8>(color.g);
    const u8 b = static_cast<u8>(color.b);
    const u8 a = static_cast<u8>(color.a);
    u32 data = 0;
    switch (format) {
    case static_cast<u32>(PixelFormat::RGBA8):
        bytes[3] = r;
        bytes[2] = g;
        bytes[1] = b;
        bytes[0] = a;
        return 4;
    case static_cast<u32>(PixelFormat::RGB8):
        bytes[2] = r;
        bytes[1] = g;
        bytes[0] = b;
        return 3;
    case static_cast<u32>(PixelFormat::RGB565):
        data = (Convert8To5(r) << 11) | (Convert8To6(g) << 5) | Convert8To5(b);
        break;
    case static_cast<u32>(PixelFormat::RGB5A1):
        data = (Convert8To5(r) << 11) | (Convert8To5(g) << 6) | (Convert8To5(b) << 1) |
               Convert8To1(a);
        break;
    case static_cast<u32>(PixelFormat::RGBA4):
        data = (Convert8To4(r) << 12) | (Convert8To4(g) << 8) | (Convert8To4(b) << 4) |
               Convert8To4(a);
        break;
    default:
        return 0;
    }
    bytes[0] = static_cast<u8>(data & 0xFF);
    bytes[1] = static_cast<u8>((data >> 8) & 0xFF);
    return 2;
}

// 8x8 Z-order coordinate from 2D coordinates (VideoCore::MortonInterleave).
constexpr u32 MortonInterleave(u32 x, u32 y) {
    constexpr u32 xlut[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    constexpr u32 ylut[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    return xlut[x % 8] + ylut[y % 8];
}

// ---- bounded access to a window of memory ------------------------------------------------------

// Four bytes at `offset`, zero where the window ends.
std::array<u8, 4> ReadFour(std::span<const u8> window, u64 offset) {
    std::array<u8, 4> bytes{};
    if (offset < window.size()) {
        const size_t count = std::min<u64>(4, window.size() - offset);
        std::memcpy(bytes.data(), window.data() + offset, count);
    }
    return bytes;
}

void WriteBytes(std::span<u8> window, u64 offset, const u8* bytes, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (offset + i < window.size()) {
            window[offset + i] = bytes[i];
        }
    }
}

} // namespace

bool FillRegs::IsVram() const {
    const u32 address = StartAddress();
    return !(address >= kFcramPaddr && address < kFcramPaddrEnd);
}

bool TransferRegs::IsInputVram() const {
    const u32 address = InputAddress();
    return !(address >= kFcramPaddr && address < kFcramPaddrEnd);
}

bool TransferRegs::IsOutputVram() const {
    const u32 address = OutputAddress();
    return !(address >= kFcramPaddr && address < kFcramPaddrEnd);
}

u32 BytesPerPixel(u32 format) {
    switch (format) {
    case static_cast<u32>(PixelFormat::RGBA8):
        return 4;
    case static_cast<u32>(PixelFormat::RGB8):
        return 3;
    case static_cast<u32>(PixelFormat::RGB565):
    case static_cast<u32>(PixelFormat::RGB5A1):
    case static_cast<u32>(PixelFormat::RGBA4):
        return 2;
    default:
        return 0;
    }
}

u32 MortonOffset(u32 x, u32 y, u32 bytes_per_pixel) {
    // Images are split into 8x8 tiles. Each tile is composed of four 4x4 subtiles each of which
    // is composed of four 2x2 subtiles each of which is composed of four texels, embedded in a
    // diagonal pattern: texels of a 2x2 subtile are 0 1 / 2 3 (bottom row first). The full tile:
    //
    //  42 43 46 47 58 59 62 63      (y = 7, the last row)
    //  40 41 44 45 56 57 60 61
    //  34 35 38 39 50 51 54 55
    //  32 33 36 37 48 49 52 53
    //  10 11 14 15 26 27 30 31
    //  08 09 12 13 24 25 28 29
    //  02 03 06 07 18 19 22 23
    //  00 01 04 05 16 17 20 21      (y = 0)
    const u32 block_height = 8;
    const u32 coarse_x = x & ~7u;
    const u32 i = MortonInterleave(x, y);
    const u32 offset = coarse_x * block_height;
    return (i + offset) * bytes_per_pixel;
}

std::span<u8> Window(Host& host, u32 paddr, u64 size) {
    if (host.PhysicalPointer(paddr, 0) == nullptr) {
        return {};
    }
    u64 want = std::min<u64>(size, 0xFFFFFFFFull - paddr);
    u8* whole = host.PhysicalPointer(paddr, static_cast<u32>(want));
    if (whole != nullptr) {
        return {whole, static_cast<size_t>(want)};
    }
    // The area ends before the range does: find the longest prefix that is in memory.
    u64 low = 0; // known good
    u64 high = want; // known bad
    while (high - low > 1) {
        const u64 mid = low + (high - low) / 2;
        if (host.PhysicalPointer(paddr, static_cast<u32>(mid)) != nullptr) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return {host.PhysicalPointer(paddr, static_cast<u32>(low)), static_cast<size_t>(low)};
}

bool IsValidPhysicalAddress(Host& host, u32 paddr) {
    return host.PhysicalPointer(paddr, 0) != nullptr;
}

void TextureCopy(Host& host, const TransferRegs& config) {
    const u32 src_addr = config.InputAddress();
    const u32 dst_addr = config.OutputAddress();

    if (!IsValidPhysicalAddress(host, src_addr)) {
        MH4N_LOG(Critical, kLog, "invalid input address {:#010X}", src_addr);
        return;
    }
    if (!IsValidPhysicalAddress(host, dst_addr)) {
        MH4N_LOG(Critical, kLog, "invalid output address {:#010X}", dst_addr);
        return;
    }

    u32 remaining_size = AlignDown(config.texture_copy_size, 16);
    if (remaining_size == 0) {
        MH4N_LOG(Critical, kLog, "zero size. Real hardware freezes on this.");
        return;
    }

    const u32 input_gap = (config.texture_copy_input_size >> 16) * 16;
    const u32 output_gap = (config.texture_copy_output_size >> 16) * 16;

    // Zero gap means contiguous input/output even if width = 0. To avoid an infinite loop below,
    // the width is the total size if the gap is 0.
    const u32 input_width =
        input_gap == 0 ? remaining_size : (config.texture_copy_input_size & 0xFFFF) * 16;
    const u32 output_width =
        output_gap == 0 ? remaining_size : (config.texture_copy_output_size & 0xFFFF) * 16;

    if (input_width == 0) {
        MH4N_LOG(Critical, kLog, "zero input width. Real hardware freezes on this.");
        return;
    }
    if (output_width == 0) {
        MH4N_LOG(Critical, kLog, "zero output width. Real hardware freezes on this.");
        return;
    }

    // The bytes the loop below can touch: the data plus one gap per row that ends.
    const u64 input_extent =
        u64{remaining_size} + (u64{remaining_size} + input_width - 1) / input_width * input_gap;
    const u64 output_extent =
        u64{remaining_size} + (u64{remaining_size} + output_width - 1) / output_width * output_gap;
    std::span<u8> src = Window(host, src_addr, input_extent);
    std::span<u8> dst = Window(host, dst_addr, output_extent);

    u64 src_offset = 0;
    u64 dst_offset = 0;
    u32 remaining_input = input_width;
    u32 remaining_output = output_width;
    while (remaining_size > 0) {
        const u32 copy_size = std::min({remaining_input, remaining_output, remaining_size});

        if (src_offset < src.size() && dst_offset < dst.size()) {
            const u64 available = std::min<u64>(src.size() - src_offset, dst.size() - dst_offset);
            std::memmove(dst.data() + dst_offset, src.data() + src_offset,
                         static_cast<size_t>(std::min<u64>(copy_size, available)));
        }
        src_offset += copy_size;
        dst_offset += copy_size;

        remaining_input -= copy_size;
        remaining_output -= copy_size;
        remaining_size -= copy_size;

        if (remaining_input == 0) {
            remaining_input = input_width;
            src_offset += input_gap;
        }
        if (remaining_output == 0) {
            remaining_output = output_width;
            dst_offset += output_gap;
        }
    }
}

void DisplayTransfer(Host& host, const TransferRegs& config) {
    const u32 src_addr = config.InputAddress();
    u32 dst_addr = config.OutputAddress();

    if (!IsValidPhysicalAddress(host, src_addr)) {
        MH4N_LOG(Critical, kLog, "invalid input address {:#010X}", src_addr);
        return;
    }
    if (!IsValidPhysicalAddress(host, dst_addr)) {
        MH4N_LOG(Critical, kLog, "invalid output address {:#010X}", dst_addr);
        return;
    }
    if (config.InputWidth() == 0) {
        MH4N_LOG(Critical, kLog, "zero input width");
        return;
    }
    if (config.InputHeight() == 0) {
        MH4N_LOG(Critical, kLog, "zero input height");
        return;
    }
    if (config.OutputWidth() == 0) {
        MH4N_LOG(Critical, kLog, "zero output width");
        return;
    }
    if (config.OutputHeight() == 0) {
        MH4N_LOG(Critical, kLog, "zero output height");
        return;
    }

    const u32 src_bytes_per_pixel = BytesPerPixel(config.InputFormat());
    const u32 dst_bytes_per_pixel = BytesPerPixel(config.OutputFormat());
    if (src_bytes_per_pixel == 0 || dst_bytes_per_pixel == 0) {
        // Azahar logs "Unknown framebuffer format" and then asserts in BytesPerPixel.
        MH4N_LOG(Error, kLog, "unknown framebuffer format (input {}, output {})",
                 config.InputFormat(), config.OutputFormat());
        return;
    }

    // Using flip_vertically alongside crop_input_lines produces skewed output on hardware.
    // Azahar emulates this because some games rely on it to render correctly (u32 arithmetic).
    if (config.FlipVertically() && config.CropInputLines()) {
        dst_addr += (config.InputWidth() - config.OutputWidth()) * (config.OutputHeight() - 1) *
                    dst_bytes_per_pixel;
    }

    if (config.Scaling() > 2) {
        MH4N_LOG(Critical, kLog, "Unimplemented display transfer scaling mode {}",
                 config.Scaling());
        return;
    }
    if (config.InputLinear() && config.Scaling() != 0) {
        MH4N_LOG(Critical, kLog, "Scaling is only implemented on tiled input");
        return;
    }

    const u32 horizontal_scale = config.Scaling() != 0 ? 1 : 0;
    const u32 vertical_scale = config.Scaling() == 2 ? 1 : 0;

    const u32 output_width = config.OutputWidth() >> horizontal_scale;
    const u32 output_height = config.OutputHeight() >> vertical_scale;

    // The bytes the loops can reach: whole 8x8 tiles when a side is tiled, the image otherwise.
    // A trailing four-byte read past a pixel (the scaling filters) stays inside the same area.
    const u64 input_extent =
        AlignUp8(config.InputWidth()) * AlignUp8(config.InputHeight()) * src_bytes_per_pixel + 4;
    const u64 output_extent = AlignUp8(output_width) * AlignUp8(output_height) *
                                  dst_bytes_per_pixel + 4;
    std::span<u8> src = Window(host, src_addr, input_extent);
    std::span<u8> dst = Window(host, dst_addr, output_extent);

    for (u32 y = 0; y < output_height; ++y) {
        for (u32 x = 0; x < output_width; ++x) {
            // The [x, y] position of the input image from the output position and the scale.
            const u32 input_x = x << horizontal_scale;
            const u32 input_y = y << vertical_scale;

            u32 output_y;
            if (config.FlipVertically()) {
                // Flip the y value of the output data, after calculating the position of the
                // input image to account for the scaling options.
                output_y = output_height - y - 1;
            } else {
                output_y = y;
            }

            u32 src_offset;
            u32 dst_offset;

            if (config.InputLinear()) {
                if (!config.DontSwizzle()) {
                    // The input is linear and the output is tiled.
                    const u32 coarse_y = output_y & ~7u;
                    const u32 stride = output_width * dst_bytes_per_pixel;

                    src_offset = (input_x + input_y * config.InputWidth()) * src_bytes_per_pixel;
                    dst_offset = MortonOffset(x, output_y, dst_bytes_per_pixel) + coarse_y * stride;
                } else {
                    // Both input and output are linear.
                    src_offset = (input_x + input_y * config.InputWidth()) * src_bytes_per_pixel;
                    dst_offset = (x + output_y * output_width) * dst_bytes_per_pixel;
                }
            } else {
                if (!config.DontSwizzle()) {
                    // The input is tiled and the output is linear.
                    const u32 coarse_y = input_y & ~7u;
                    const u32 stride = config.InputWidth() * src_bytes_per_pixel;

                    src_offset = MortonOffset(input_x, input_y, src_bytes_per_pixel) +
                                 coarse_y * stride;
                    dst_offset = (x + output_y * output_width) * dst_bytes_per_pixel;
                } else {
                    // Both input and output are tiled.
                    const u32 out_coarse_y = output_y & ~7u;
                    const u32 out_stride = output_width * dst_bytes_per_pixel;

                    const u32 in_coarse_y = input_y & ~7u;
                    const u32 in_stride = config.InputWidth() * src_bytes_per_pixel;

                    src_offset = MortonOffset(input_x, input_y, src_bytes_per_pixel) +
                                 in_coarse_y * in_stride;
                    dst_offset = MortonOffset(x, output_y, dst_bytes_per_pixel) +
                                 out_coarse_y * out_stride;
                }
            }

            auto pixel_at = [&](u64 extra) {
                const auto bytes = ReadFour(src, u64{src_offset} + extra);
                return DecodePixel(config.InputFormat(), bytes.data());
            };

            Color src_color = pixel_at(0);
            if (config.Scaling() == 1) {
                const Color pixel = pixel_at(src_bytes_per_pixel);
                src_color = {(src_color.r + pixel.r) / 2, (src_color.g + pixel.g) / 2,
                             (src_color.b + pixel.b) / 2, (src_color.a + pixel.a) / 2};
            } else if (config.Scaling() == 2) {
                const Color pixel1 = pixel_at(1u * src_bytes_per_pixel);
                const Color pixel2 = pixel_at(2u * src_bytes_per_pixel);
                const Color pixel3 = pixel_at(3u * src_bytes_per_pixel);
                src_color = {((src_color.r + pixel1.r) + (pixel2.r + pixel3.r)) / 4,
                             ((src_color.g + pixel1.g) + (pixel2.g + pixel3.g)) / 4,
                             ((src_color.b + pixel1.b) + (pixel2.b + pixel3.b)) / 4,
                             ((src_color.a + pixel1.a) + (pixel2.a + pixel3.a)) / 4};
            }

            u8 encoded[4] = {};
            const u32 count = EncodePixel(config.OutputFormat(), src_color, encoded);
            WriteBytes(dst, dst_offset, encoded, count);
        }
    }
}

void MemoryFill(Host& host, const FillRegs& config) {
    const u32 start_addr = config.StartAddress();
    const u32 end_addr = config.EndAddress();

    if (!IsValidPhysicalAddress(host, start_addr)) {
        MH4N_LOG(Critical, kLog, "invalid start address {:#010X}", start_addr);
        return;
    }
    if (!IsValidPhysicalAddress(host, end_addr)) {
        MH4N_LOG(Critical, kLog, "invalid end address {:#010X}", end_addr);
        return;
    }
    if (end_addr <= start_addr) {
        MH4N_LOG(Critical, kLog, "invalid memory range from {:#010X} to {:#010X}", start_addr,
                 end_addr);
        return;
    }

    // The 24-bit and 16-bit loops write a whole unit at the last position even when it ends past
    // `end` (Azahar does too), so the window has room for it. A range that leaves the area is cut.
    const u64 length = end_addr - start_addr;
    std::span<u8> area = Window(host, start_addr, length + 3);
    auto put = [&](u64 offset, const u8* bytes, u32 count) {
        WriteBytes(area, offset, bytes, count);
    };

    const u32 fill_24bit = (config.control >> 8) & 1;
    const u32 fill_32bit = (config.control >> 9) & 1;
    if (fill_24bit) {
        // Fill with 24-bit values: R, G, B in the bytes 0, 1, 2 of the value.
        const u8 pixel[3] = {static_cast<u8>(config.value), static_cast<u8>(config.value >> 8),
                             static_cast<u8>(config.value >> 16)};
        for (u64 offset = 0; offset < length; offset += 3) {
            put(offset, pixel, 3);
        }
    } else if (fill_32bit) {
        // Fill with 32-bit values.
        const u8 pixel[4] = {static_cast<u8>(config.value), static_cast<u8>(config.value >> 8),
                             static_cast<u8>(config.value >> 16),
                             static_cast<u8>(config.value >> 24)};
        const u64 count = length / sizeof(u32);
        for (u64 i = 0; i < count; ++i) {
            put(i * 4, pixel, 4);
        }
    } else {
        // Fill with 16-bit values.
        const u8 pixel[2] = {static_cast<u8>(config.value), static_cast<u8>(config.value >> 8)};
        for (u64 offset = 0; offset < length; offset += 2) {
            put(offset, pixel, 2);
        }
    }
}

} // namespace mh4n::gpu::gx
