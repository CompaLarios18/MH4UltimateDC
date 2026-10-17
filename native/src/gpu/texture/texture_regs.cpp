// Derived from Azahar src/video_core/pica/regs_texturing.h, regs_internal.h and
// src/video_core/renderer_vulkan/vk_rasterizer.cpp (SyncTextureUnits: which units and types load
// a texture) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// vk_rasterizer.cpp says "Licensed under GPLv3 or any later version"; regs_texturing.h and
// regs_internal.h say "GPLv2 or any later version").
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (vk_rasterizer.cpp)
#include "gpu/texture/texture_regs.h"

namespace mh4n::gpu::texture {
namespace {

constexpr u32 Bits(u32 word, u32 first, u32 count) {
    return (word >> first) & ((1u << count) - 1u);
}

} // namespace

std::vector<TextureRef> TexturesOfDraw(const u32* regs, u32* skipped) {
    std::vector<TextureRef> textures;
    u32 left_out = 0;
    const u32 config_base[3] = {kTexture0, kTexture1, kTexture2};
    const u32 format_reg[3] = {kTexture0Format, kTexture1Format, kTexture2Format};

    for (u32 unit = 0; unit < 3; ++unit) {
        if (((regs[kMainConfig] >> unit) & 1) == 0) {
            continue;
        }
        const u32 base = config_base[unit];
        const u32 size_word = regs[base + 1];
        const u32 params = regs[base + 2];
        const u32 address = Bits(regs[base + 4], 0, 28);
        const u32 format_value = Bits(regs[format_reg[unit]], 0, 4);
        // The type field is only valid for unit 0 (regs_texturing.h)
        const Type type = unit == 0 ? static_cast<Type>(Bits(params, 28, 3)) : Type::Texture2D;

        TextureRef ref;
        ref.unit = unit;
        ref.width = Bits(size_word, 16, 11);
        ref.height = Bits(size_word, 0, 11);
        ref.format = static_cast<Format>(format_value);

        const bool colour = type == Type::Texture2D || type == Type::Projection2D ||
                            type == Type::TextureCube;
        if (!colour || !IsValidFormat(format_value) || !IsValidSize(ref.width, ref.height)) {
            ++left_out;
            continue;
        }

        if (type == Type::TextureCube) {
            // GetCubePhysicalAddress: bits 22-27 of the main address are shared by all faces
            for (u32 face = 0; face < 6; ++face) {
                u32 face_address = address;
                if (face != 0) {
                    const u32 extra = Bits(regs[kCubeAddress + face - 1], 0, 22);
                    face_address = (face_address & ~0x3FFFFFu) | extra;
                }
                TextureRef cube_face = ref;
                cube_face.cube = true;
                cube_face.face = face;
                cube_face.paddr = face_address * 8;
                textures.push_back(cube_face);
            }
        } else {
            ref.paddr = address * 8;
            textures.push_back(ref);
        }
    }
    if (skipped != nullptr) {
        *skipped = left_out;
    }
    return textures;
}

} // namespace mh4n::gpu::texture
