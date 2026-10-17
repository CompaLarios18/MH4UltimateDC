// Derived from Azahar src/video_core/custom_textures/material.cpp (CustomTexture::LoadPNG,
// LoadDDS, ToCustomPixelFormat), custom_format.h, src/common/texture.cpp (FlipRGBA8Texture) and
// src/core/frontend/image_interface.cpp (DecodePNG, DecodeDDS) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version").
// Copyright 2023 Citra Emulator Project (material.cpp, custom_format.h, image_interface.cpp)
// Copyright 2019 Citra Emulator Project (texture.cpp)
#include "hdpack/image.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <lodepng.h>

#include <fmt/format.h>

namespace mh4n::hdpack {

const char* PixelFormatName(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8: return "RGBA8";
    case PixelFormat::BC1: return "BC1";
    case PixelFormat::BC3: return "BC3";
    case PixelFormat::BC5: return "BC5";
    case PixelFormat::BC7: return "BC7";
    }
    return "?";
}

u32 BlockBytes(PixelFormat format) {
    switch (format) {
    case PixelFormat::BC1: return 8;
    case PixelFormat::BC3:
    case PixelFormat::BC5:
    case PixelFormat::BC7: return 16;
    case PixelFormat::RGBA8: break;
    }
    return 0;
}

u64 LevelBytes(PixelFormat format, u32 width, u32 height) {
    const u32 block = BlockBytes(format);
    if (block == 0) {
        return static_cast<u64>(width) * height * 4;
    }
    return static_cast<u64>((width + 3) / 4) * ((height + 3) / 4) * block;
}

u32 FullMipCount(u32 width, u32 height) {
    u32 count = 1;
    for (u32 side = std::max(width, height); side > 1; side >>= 1) {
        ++count;
    }
    return count;
}

void FlipRows(u8* rgba, u32 width, u32 height) {
    const std::size_t line = static_cast<std::size_t>(width) * 4;
    for (u32 row = 0; row < height / 2; ++row) {
        std::swap_ranges(rgba + row * line, rgba + (row + 1) * line,
                         rgba + static_cast<std::size_t>(height - row - 1) * line);
    }
}

bool DecodePng(const u8* data, std::size_t size, bool flip, Image& out, std::string& error) {
    // Read the size first, so that an absurd image is refused before it is allocated.
    unsigned width = 0;
    unsigned height = 0;
    LodePNGState state;
    lodepng_state_init(&state);
    unsigned code = lodepng_inspect(&width, &height, &state, data, size);
    lodepng_state_cleanup(&state);
    if (code != 0) {
        error = fmt::format("not a PNG: {}", lodepng_error_text(code));
        return false;
    }
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        error = fmt::format("PNG of {}x{} is out of range", width, height);
        return false;
    }
    unsigned char* pixels = nullptr;
    code = lodepng_decode_memory(&pixels, &width, &height, data, size, LCT_RGBA, 8);
    if (code != 0) {
        std::free(pixels);
        error = fmt::format("cannot decode the PNG: {}", lodepng_error_text(code));
        return false;
    }
    out.format = PixelFormat::RGBA8;
    out.width = width;
    out.height = height;
    out.levels.assign(1, std::vector<u8>(pixels, pixels + static_cast<std::size_t>(width) * height * 4));
    std::free(pixels);
    if (flip) {
        FlipRows(out.levels[0].data(), width, height);
    }
    return true;
}

namespace {

u32 Le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

constexpr u32 FourCC(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

struct DdsFormat {
    bool known = false;
    PixelFormat format = PixelFormat::RGBA8;
    bool swap_rb = false;  // BGRA in the file
    bool opaque = false;   // the fourth channel is padding
};

DdsFormat FromDxgi(u32 dxgi) {
    DdsFormat f;
    f.known = true;
    switch (dxgi) {
    case 28: case 29: break; // R8G8B8A8_UNORM, _SRGB
    case 87: case 91: f.swap_rb = true; break; // B8G8R8A8_UNORM, _SRGB
    case 88: case 93: f.swap_rb = true; f.opaque = true; break; // B8G8R8X8_UNORM, _SRGB
    case 71: case 72: f.format = PixelFormat::BC1; break;
    case 77: case 78: f.format = PixelFormat::BC3; break;
    case 83: f.format = PixelFormat::BC5; break;
    case 98: case 99: f.format = PixelFormat::BC7; break;
    default: f.known = false; break;
    }
    return f;
}

} // namespace

bool DecodeDds(const u8* data, std::size_t size, Image& out, std::string& error) {
    constexpr std::size_t kHeader = 4 + 124;
    if (size < kHeader || std::memcmp(data, "DDS ", 4) != 0 || Le32(data + 4) != 124) {
        error = "not a DDS file";
        return false;
    }
    const u8* h = data + 4;
    const u32 flags = Le32(h + 4);
    const u32 height = Le32(h + 8);
    const u32 width = Le32(h + 12);
    const u32 depth = Le32(h + 20);
    u32 mip_count = (flags & 0x20000) ? Le32(h + 24) : 1; // DDSD_MIPMAPCOUNT
    // DDS_PIXELFORMAT starts at offset 72 of the header: size, flags, four-CC, bit count, masks.
    const u32 pf_flags = Le32(h + 76);
    const u32 four_cc = Le32(h + 80);
    const u32 bit_count = Le32(h + 84);
    const u32 mask_r = Le32(h + 88);
    const u32 mask_g = Le32(h + 92);
    const u32 mask_b = Le32(h + 96);
    const u32 mask_a = Le32(h + 100);
    const u32 caps2 = Le32(h + 108);
    std::size_t offset = kHeader;

    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        error = fmt::format("DDS of {}x{} is out of range", width, height);
        return false;
    }
    if ((caps2 & 0x200) || (caps2 & 0x200000) || ((flags & 0x800000) && depth > 1)) {
        error = "cube maps and volume textures are not supported";
        return false;
    }
    mip_count = std::clamp<u32>(mip_count, 1, FullMipCount(width, height));

    DdsFormat format;
    if ((pf_flags & 0x4) && four_cc == FourCC('D', 'X', '1', '0')) { // DDPF_FOURCC
        if (size < kHeader + 20) {
            error = "truncated DX10 header";
            return false;
        }
        const u32 dxgi = Le32(data + kHeader);
        const u32 array_size = Le32(data + kHeader + 12);
        if (array_size > 1) {
            error = "texture arrays are not supported";
            return false;
        }
        format = FromDxgi(dxgi);
        if (!format.known) {
            error = fmt::format("DXGI format {} is not supported", dxgi);
            return false;
        }
        offset += 20;
    } else if (pf_flags & 0x4) {
        format.known = true;
        if (four_cc == FourCC('D', 'X', 'T', '1')) {
            format.format = PixelFormat::BC1;
        } else if (four_cc == FourCC('D', 'X', 'T', '5')) {
            format.format = PixelFormat::BC3;
        } else if (four_cc == FourCC('A', 'T', 'I', '2') || four_cc == FourCC('B', 'C', '5', 'U')) {
            format.format = PixelFormat::BC5;
        } else {
            error = "this four-CC format is not supported";
            return false;
        }
    } else if ((pf_flags & 0x40) && bit_count == 32 && mask_g == 0xFF00) { // DDPF_RGB
        format.known = true;
        if (mask_r == 0xFF && mask_b == 0xFF0000) {
            format.swap_rb = false;
        } else if (mask_r == 0xFF0000 && mask_b == 0xFF) {
            format.swap_rb = true;
        } else {
            error = "this 32-bit channel order is not supported";
            return false;
        }
        format.opaque = (mask_a == 0);
    } else {
        error = "this DDS pixel format is not supported";
        return false;
    }

    out.format = format.format;
    out.width = width;
    out.height = height;
    out.levels.clear();
    const u32 levels = format.format == PixelFormat::RGBA8 ? 1 : mip_count;
    u32 w = width;
    u32 h2 = height;
    for (u32 level = 0; level < levels; ++level) {
        const u64 bytes = LevelBytes(format.format, w, h2);
        if (offset + bytes > size) {
            error = "the DDS file is truncated";
            return false;
        }
        out.levels.emplace_back(data + offset, data + offset + bytes);
        offset += static_cast<std::size_t>(bytes);
        w = std::max(1u, w >> 1);
        h2 = std::max(1u, h2 >> 1);
    }
    if (format.format == PixelFormat::RGBA8) {
        std::vector<u8>& pixels = out.levels[0];
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            if (format.swap_rb) {
                std::swap(pixels[i], pixels[i + 2]);
            }
            if (format.opaque) {
                pixels[i + 3] = 255;
            }
        }
    }
    return true;
}

} // namespace mh4n::hdpack
