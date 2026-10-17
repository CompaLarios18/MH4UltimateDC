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
// Texture decoding (N2-14), bit for bit as Azahar's.
//
// Two forms, both from the Morton-tiled layout:
//  - DecodeToRgba8: Azahar's software renderer (texture_decode.cpp, LookupTexture). Row 0 of the
//    image is the first row of tiles of the memory, and texel (x, y) of a tile is at Morton index
//    MortonInterleave(x, y); every format gives RGBA8 (r, g, b, a order in memory).
//  - DecodeForHash: the form the rasterizer cache hashes for custom textures when the pack does
//    not use the new hash (RasterizerCache::ComputeHash with convert = false): rows bottom-up
//    (texture_codec.h's MortonCopy writes the linear buffer from the bottom), the five colour
//    formats copied as they are, the others decoded to RGBA8.
#pragma once

#include <vector>

#include "gpu/texture/formats.h"

namespace mh4n::gpu::texture {

struct Rgba {
    u8 r, g, b, a;
    friend bool operator==(const Rgba&, const Rgba&) = default;
};

// LookupTexelInTile: texel (x, y), both below 8, of the tile that starts at `tile`.
// `disable_alpha` is the debug form of texture_decode.cpp (alpha 255, the information moved to an
// unused channel for IA8, IA4 and A4/A8).
Rgba LookupTexelInTile(const u8* tile, u32 x, u32 y, Format format, bool disable_alpha = false);

// LookupTexture: texel (x, y) of a width x height texture whose tiles start at `source`.
Rgba LookupTexture(const u8* source, u32 x, u32 y, u32 width, Format format,
                   bool disable_alpha = false);

// The whole texture as RGBA8, width * height * 4 bytes, row 0 first in memory order. False (and
// `out` untouched) when the size is not a whole number of tiles or `size` is smaller than
// EncodedBytes.
bool DecodeToRgba8(const u8* source, std::size_t size, u32 width, u32 height, Format format,
                   std::vector<u8>& out);

// The bytes the old-style hash covers: width * height * HashBytesPerTexel(format), rows bottom-up.
bool DecodeForHash(const u8* source, std::size_t size, u32 width, u32 height, Format format,
                   std::vector<u8>& out);

} // namespace mh4n::gpu::texture
