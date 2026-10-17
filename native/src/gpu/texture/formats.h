// Derived from Azahar src/video_core/texture/texture_decode.cpp, texture_decode.h, etc1.cpp,
// src/video_core/rasterizer_cache/texture_codec.h, pixel_format.h, utils.cpp, src/video_core/utils.h,
// src/common/color.h and src/video_core/pica/regs_texturing.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version").
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (texture_decode.cpp,
//   texture_decode.h)
// Copyright 2017 Citra Emulator Project (etc1.cpp)
// Copyright 2022-2026 Citra Emulator Project / Azahar Emulator Project (texture_codec.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pixel_format.h)
// Copyright 2023 Citra Emulator Project (utils.cpp)
// Copyright 2014 Citra Emulator Project (utils.h, color.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
//
// The PICA texture formats (N2-14): their numbering, sizes and the 8x8 Morton tile.
#pragma once

#include <cstddef>

#include "mh4n/types.h"

namespace mh4n::gpu::texture {

// TexturingRegs::TextureFormat (regs_texturing.h); the same numbers as VideoCore::PixelFormat's
// colour formats, which Azahar writes into the file names of dumped textures.
enum class Format : u32 {
    RGBA8 = 0,
    RGB8 = 1,
    RGB5A1 = 2,
    RGB565 = 3,
    RGBA4 = 4,
    IA8 = 5,
    RG8 = 6,
    I8 = 7,
    A8 = 8,
    IA4 = 9,
    I4 = 10,
    A4 = 11,
    ETC1 = 12,
    ETC1A4 = 13,
};
inline constexpr u32 kFormatCount = 14;

constexpr bool IsValidFormat(u32 value) {
    return value < kFormatCount;
}

// The bits one texel takes in memory (FORMAT_MAP's bits_per_block of pixel_format.h; ETC1 is 4
// bits per texel, 8 bytes per 4x4 block).
constexpr u32 BitsPerTexel(Format format) {
    switch (format) {
    case Format::RGBA8:
        return 32;
    case Format::RGB8:
        return 24;
    case Format::RGB5A1:
    case Format::RGB565:
    case Format::RGBA4:
    case Format::IA8:
    case Format::RG8:
        return 16;
    case Format::I8:
    case Format::A8:
    case Format::IA4:
    case Format::ETC1A4:
        return 8;
    case Format::I4:
    case Format::A4:
    case Format::ETC1:
        return 4;
    }
    return 0;
}

// CalculateTileSize (texture_decode.cpp): the bytes of one 8x8 tile.
constexpr std::size_t TileBytes(Format format) {
    return static_cast<std::size_t>(BitsPerTexel(format)) * 64 / 8;
}

// The bytes a width x height texture takes in memory (both multiples of 8): whole tiles, laid out
// row by row (the default stride of TextureInfo::SetDefaultStride).
constexpr std::size_t EncodedBytes(Format format, u32 width, u32 height) {
    return TileBytes(format) * (width / 8) * (height / 8);
}

// The bytes per texel of the decoded form the texture cache hashes with convert = false
// (GetFormatBytesPerPixel of pixel_format.h): the five colour formats keep their own size, every
// other format decodes to RGBA8.
constexpr u32 HashBytesPerTexel(Format format) {
    switch (format) {
    case Format::RGBA8:
        return 4;
    case Format::RGB8:
        return 3;
    case Format::RGB5A1:
    case Format::RGB565:
    case Format::RGBA4:
        return 2;
    default:
        return 4;
    }
}

// MortonInterleave (video_core/utils.h): the index of texel (x, y) inside an 8x8 tile.
constexpr u32 MortonInterleave(u32 x, u32 y) {
    constexpr u32 xlut[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    constexpr u32 ylut[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    return xlut[x % 8] + ylut[y % 8];
}

// A texture's size must be a multiple of 8 in both directions (it is a whole number of tiles).
constexpr bool IsValidSize(u32 width, u32 height) {
    return width != 0 && height != 0 && width % 8 == 0 && height % 8 == 0 && width <= 2048 &&
           height <= 2048;
}

} // namespace mh4n::gpu::texture
