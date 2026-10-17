// Derived from Azahar src/video_core/custom_textures/custom_tex_manager.cpp (FindCustomTextures,
// ParseFilename, ReadConfig, GetTextures), custom_tex_manager.h (the option defaults),
// material.cpp (Material::AddMapTexture) and custom_format.h (CustomFileFormat, MapType) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (custom_tex_manager.cpp)
// Copyright 2023 Citra Emulator Project (material.cpp, custom_format.h, custom_tex_manager.h)
//
// How a texture pack in the layout Azahar loads (load/textures/<title id>/) is read (N2-17):
// the file names, pack.json's "options" and "textures", and which files make up which material.
// The hash a file answers to comes from its name (the format of gpu/texture/hash.h's
// CustomTextureStem) and from pack.json's "textures" map; tools/native/hd_hash_check.py reads a
// pack by the same rules.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::hdpack {

// The pack's options. Without a pack.json Azahar uses its "legacy defaults": the old hash and
// skipped mipmaps (so DDS files are ignored), while flip_png_files keeps its member default.
struct PackOptions {
    bool has_pack_json = false;
    bool skip_mipmap = true;
    bool flip_png_files = true;
    bool use_new_hash = false;
};

enum class FileFormat : u8 { None = 0, Png = 1, Dds = 2, Ktx = 3 }; // Azahar's CustomFileFormat
enum class MapType : u8 { Color = 0, Normal = 1 };                   // Azahar's MapType (norm)
inline constexpr std::size_t kMapTypeCount = 2;

// "tex1_<w>x<h>_<hash as hex>_<format>", with any trailing text (such as "_mip0") ignored:
// Azahar's sscanf("tex1_%ux%u_%llX_%u").
struct TextureStem {
    u32 width = 0;
    u32 height = 0;
    u64 hash = 0;
    u32 format = 0;
};
std::optional<TextureStem> ParseTextureStem(std::string_view name);

// pack.json: the options and the "textures" map (file name without directories -> hashes, in
// the order of the file). nullopt, with a message, when it is unusable: not JSON, no "options"
// object, or one of the three options missing or not a boolean.
struct PackJson {
    PackOptions options;
    std::map<std::string, std::vector<u64>> hashes_by_file;
};
std::optional<PackJson> ParsePackJson(const std::string& text, std::string* error);

struct PackFile {
    std::string path; // relative to the pack folder, with '/'
    FileFormat format = FileFormat::None;
    MapType type = MapType::Color;
    std::vector<u64> hashes; // the hashes this file answers to (never empty)
    u64 size = 0;
    s64 mtime = 0; // nanoseconds, only to notice that a file changed between runs
};

// One hash of the pack: the file of each map. -1 where there is none.
struct PackMaterial {
    int file[kMapTypeCount] = {-1, -1};
};

struct ScanStats {
    u32 ignored_names = 0;       // more than three dot-separated parts, or no known extension
    u32 ignored_dds = 0;         // a DDS file in a pack that skips mipmaps
    u32 files_without_hash = 0;  // a texture file whose name and pack.json give no hash
    u32 duplicate_maps = 0;      // two files for the same map of one hash (the first is kept)
};

struct Pack {
    PackOptions options;
    std::vector<PackFile> files;               // sorted by path; only files with a hash
    std::map<u64, PackMaterial> materials;     // by hash, in hash order
    ScanStats stats;
};

// Scans `dir` (up to 64 levels deep, like Azahar). nullopt, with a message, when `dir` is not a
// directory or its pack.json is unusable.
std::optional<Pack> ScanPack(const std::string& dir, std::string* error);

} // namespace mh4n::hdpack
