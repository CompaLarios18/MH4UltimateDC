// Derived from Azahar src/video_core/rasterizer_cache/rasterizer_cache.h (ComputeHash),
// src/video_core/rasterizer_cache/utils.cpp (DecodeTexture), src/video_core/custom_textures/
// custom_tex_manager.cpp (DumpTexture, ParseFilename, ReadConfig) and src/common/hash.h
// (ComputeHash64) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of
// those files says "Licensed under GPLv2 or any later version").
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_cache.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (custom_tex_manager.cpp)
// Copyright 2023 Citra Emulator Project (utils.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (hash.h)
//
// The custom-texture hashes (N2-14): the 64-bit numbers a texture pack names its files by
// (tex1_<w>x<h>_<hash>_<format>.png, or the keys of pack.json), computed exactly as Azahar does
// when it looks a texture up (RasterizerCache::ComputeHash):
//  - with `use_new_hash` (a pack whose pack.json says so): CityHash64 over the raw bytes the game
//    uploaded, i.e. the tiled texture as it is in memory;
//  - otherwise, and for a pack without pack.json: CityHash64 over the texture decoded with
//    convert = false (DecodeForHash).
// Level 0 of the texture, all of it, is what the hashes cover.
#pragma once

#include <optional>
#include <string>

#include "gpu/texture/decode.h"

namespace mh4n::gpu::texture {

struct CustomHashes {
    u64 new_hash;
    u64 old_hash;
};

// nullopt when the size is not a whole number of tiles or `size` is smaller than the texture.
std::optional<CustomHashes> ComputeCustomHashes(const u8* data, std::size_t size, u32 width,
                                                u32 height, Format format);

// The names of a texture file in a pack: tex1_<w>x<h>_<hash as 16 upper-case hex digits>_<format>
// (the `_mip<level>` suffix and the extension are the pack's business).
std::string CustomTextureStem(u32 width, u32 height, u64 hash, Format format);

} // namespace mh4n::gpu::texture
