// Derived from Azahar src/video_core/rasterizer_cache/rasterizer_cache.h (ComputeHash),
// src/video_core/rasterizer_cache/utils.cpp (DecodeTexture), src/video_core/custom_textures/
// custom_tex_manager.cpp (DumpTexture, ParseFilename, ReadConfig) and src/common/hash.h
// (ComputeHash64) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of
// those files says "Licensed under GPLv2 or any later version").
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_cache.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (custom_tex_manager.cpp)
// Copyright 2023 Citra Emulator Project (utils.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (hash.h)
#include "gpu/texture/hash.h"

#include <fmt/format.h>

#include "gpu/texture/cityhash.h"

namespace mh4n::gpu::texture {

std::optional<CustomHashes> ComputeCustomHashes(const u8* data, std::size_t size, u32 width,
                                                u32 height, Format format) {
    if (!IsValidSize(width, height)) {
        return std::nullopt;
    }
    const std::size_t encoded = EncodedBytes(format, width, height);
    if (size < encoded) {
        return std::nullopt;
    }
    std::vector<u8> decoded;
    if (!DecodeForHash(data, size, width, height, format, decoded)) {
        return std::nullopt;
    }
    CustomHashes hashes;
    hashes.new_hash = CityHash64(data, encoded);
    hashes.old_hash = CityHash64(decoded.data(), decoded.size());
    return hashes;
}

std::string CustomTextureStem(u32 width, u32 height, u64 hash, Format format) {
    return fmt::format("tex1_{}x{}_{:016X}_{}", width, height, hash, static_cast<u32>(format));
}

} // namespace mh4n::gpu::texture
