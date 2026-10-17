// Derived from Azahar src/video_core/custom_textures/material.cpp (CustomTexture::LoadPNG,
// LoadDDS, ToCustomPixelFormat), custom_format.h (CustomPixelFormat), src/common/texture.cpp
// (FlipRGBA8Texture) and src/core/frontend/image_interface.cpp (DecodePNG, DecodeDDS) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version").
// Copyright 2023 Citra Emulator Project (material.cpp, custom_format.h, image_interface.cpp)
// Copyright 2019 Citra Emulator Project (texture.cpp)
//
// Reading the images of a pack (N2-17): PNG through LodePNG (native/third_party/png) and DDS
// by hand. Azahar decodes DDS with dds-ktx and uses only the first level of the file; here a
// block-compressed DDS keeps the levels it has, since there is no way to rebuild the others
// without decoding it.
#pragma once

#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::hdpack {

// The numbers of Azahar's CustomPixelFormat; they are also the format field of the archive.
enum class PixelFormat : u8 { RGBA8 = 0, BC1 = 1, BC3 = 2, BC5 = 3, BC7 = 4 };

const char* PixelFormatName(PixelFormat format);
// Bytes per 4x4 block of a compressed format (8 for BC1, 16 for the others); 0 for RGBA8.
u32 BlockBytes(PixelFormat format);
// The bytes of one level of a compressed format, or width * height * 4 for RGBA8.
u64 LevelBytes(PixelFormat format, u32 width, u32 height);
// The number of levels of a full chain down to 1x1.
u32 FullMipCount(u32 width, u32 height);

// The largest side accepted.
inline constexpr u32 kMaxDimension = 16384;

struct Image {
    PixelFormat format = PixelFormat::RGBA8;
    u32 width = 0;
    u32 height = 0;
    // RGBA8: levels[0] is width * height * 4 bytes, R first, top row first. A compressed
    // format: every level present in the file, each LevelBytes() long.
    std::vector<std::vector<u8>> levels;
};

// Decodes a PNG (any colour type and depth) to RGBA8. `flip` turns the rows upside down, which
// is what Azahar does for a pack whose pack.json says flip_png_files.
bool DecodePng(const u8* data, std::size_t size, bool flip, Image& out, std::string& error);

// Decodes a DDS file: RGBA8 or BGRA8 (legacy masks or DXGI formats 28, 29, 87, 88, 91, 93) and
// BC1, BC3, BC5 and BC7 (four-CC DXT1, DXT5, ATI2 or BC5U; DXGI 71, 72, 77, 78, 83, 98, 99).
// Cube maps, volumes and arrays are refused.
bool DecodeDds(const u8* data, std::size_t size, Image& out, std::string& error);

// Turns the rows of an RGBA8 image upside down in place (Azahar's FlipRGBA8Texture).
void FlipRows(u8* rgba, u32 width, u32 height);

} // namespace mh4n::hdpack
