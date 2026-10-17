// Derived from Azahar src/video_core/pica/regs_texturing.h, regs_internal.h and
// src/video_core/renderer_vulkan/vk_rasterizer.cpp (SyncTextureUnits: which units and types load
// a texture) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// vk_rasterizer.cpp says "Licensed under GPLv3 or any later version"; regs_texturing.h and
// regs_internal.h say "GPLv2 or any later version").
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (vk_rasterizer.cpp)
//
// The texture registers of the PICA (N2-14): where the three texture units are configured, and
// the textures a draw uses.
#pragma once

#include <array>
#include <vector>

#include "gpu/texture/formats.h"

namespace mh4n::gpu::texture {

// Register ids (ASSERT_REG_POSITION in regs_internal.h). A TextureConfig is five words: the
// border colour, the size (height in bits 0-10, width in bits 16-26), the parameters (the type in
// bits 28-30, for unit 0), the LOD word and the address (bits 0-27, in units of 8 bytes).
inline constexpr u32 kMainConfig = 0x80;       // bit n: unit n enabled (n = 0-2)
inline constexpr u32 kTexture0 = 0x81;
inline constexpr u32 kCubeAddress = 0x86;      // 5 words, bits 0-21: faces -X, +Y, -Y, +Z, -Z
inline constexpr u32 kTexture0Format = 0x8E;   // bits 0-3
inline constexpr u32 kTexture1 = 0x91;
inline constexpr u32 kTexture1Format = 0x96;
inline constexpr u32 kTexture2 = 0x99;
inline constexpr u32 kTexture2Format = 0x9E;
inline constexpr u32 kTextureRegisters = 0x9F; // registers up to here are read

// TextureConfig::TextureType
enum class Type : u32 {
    Texture2D = 0,
    TextureCube = 1,
    Shadow2D = 2,
    Projection2D = 3,
    ShadowCube = 4,
    Disabled = 5,
};

// A texture a draw samples: one face for a 2D texture, six for a cube.
struct TextureRef {
    u32 unit = 0;
    u32 face = 0; // 0-5 for a cube (+X, -X, +Y, -Y, +Z, -Z), else 0
    bool cube = false;
    u32 paddr = 0;
    u32 width = 0;
    u32 height = 0;
    Format format = Format::RGBA8;

    friend bool operator==(const TextureRef&, const TextureRef&) = default;
};

// The colour textures an enabled unit of `regs` (the 0x300 internal registers) loads, as Azahar's
// rasterizers bind them: unit 0 by its type (2D and projection: one texture, cube: six faces),
// units 1 and 2 as 2D textures. Shadow textures are not colour textures and are left out, as are
// formats above 13 and sizes that are not a whole number of tiles. `skipped`, when given, counts
// the units left out.
std::vector<TextureRef> TexturesOfDraw(const u32* regs, u32* skipped = nullptr);

} // namespace mh4n::gpu::texture
