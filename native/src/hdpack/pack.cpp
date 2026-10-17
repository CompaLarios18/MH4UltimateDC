// Derived from Azahar src/video_core/custom_textures/custom_tex_manager.cpp (FindCustomTextures,
// ParseFilename, ReadConfig, GetTextures), custom_tex_manager.h (the option defaults),
// material.cpp (Material::AddMapTexture), material.h (MapType) and custom_format.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (custom_tex_manager.cpp)
// Copyright 2023 Citra Emulator Project (material.cpp, material.h, custom_format.h, custom_tex_manager.h)
#include "hdpack/pack.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include "hdpack/json.h"
#include "mh4n/log.h"

namespace mh4n::hdpack {

namespace fs = std::filesystem;

namespace {

bool ParseUInt(std::string_view s, std::size_t& pos, u64& value, int base) {
    const std::size_t begin = pos;
    value = 0;
    while (pos < s.size()) {
        const char c = s[pos];
        unsigned digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
        } else {
            break;
        }
        value = value * static_cast<u64>(base) + digit; // wraps like sscanf's %llX would not; fine
        ++pos;
    }
    return pos > begin;
}

FileFormat MakeFileFormat(std::string_view ext) {
    if (ext == "png") {
        return FileFormat::Png;
    }
    if (ext == "dds") {
        return FileFormat::Dds;
    }
    if (ext == "ktx") {
        return FileFormat::Ktx;
    }
    return FileFormat::None;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = s.find(sep, begin);
        if (end == std::string::npos) {
            parts.push_back(s.substr(begin));
            return parts;
        }
        parts.push_back(s.substr(begin, end - begin));
        begin = end + 1;
    }
}

// std::stoull(key, &idx, 16): leading white space, an optional 0x, the hex digits; no digits
// means the key is invalid (Azahar logs "Key is invalid, skipping").
std::optional<u64> ParseHashKey(const std::string& key) {
    std::size_t pos = 0;
    while (pos < key.size() && std::isspace(static_cast<unsigned char>(key[pos]))) {
        ++pos;
    }
    if (key.compare(pos, 2, "0x") == 0 || key.compare(pos, 2, "0X") == 0) {
        pos += 2;
    }
    const std::size_t begin = pos;
    u64 value = 0;
    if (!ParseUInt(key, pos, value, 16) || pos - begin > 16) {
        return std::nullopt;
    }
    return value;
}

std::string FileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ReadFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

} // namespace

std::optional<TextureStem> ParseTextureStem(std::string_view name) {
    if (name.substr(0, 5) != "tex1_") {
        return std::nullopt;
    }
    std::size_t pos = 5;
    u64 width = 0;
    u64 height = 0;
    TextureStem stem;
    u64 format = 0;
    if (!ParseUInt(name, pos, width, 10) || pos >= name.size() || name[pos] != 'x') {
        return std::nullopt;
    }
    ++pos;
    if (!ParseUInt(name, pos, height, 10) || pos >= name.size() || name[pos] != '_') {
        return std::nullopt;
    }
    ++pos;
    if (pos + 1 < name.size() && name[pos] == '0' && (name[pos + 1] == 'x' || name[pos + 1] == 'X')) {
        pos += 2; // %llX accepts a 0x prefix
    }
    if (!ParseUInt(name, pos, stem.hash, 16) || pos >= name.size() || name[pos] != '_') {
        return std::nullopt;
    }
    ++pos;
    if (!ParseUInt(name, pos, format, 10)) {
        return std::nullopt;
    }
    stem.width = static_cast<u32>(width);
    stem.height = static_cast<u32>(height);
    stem.format = static_cast<u32>(format);
    return stem;
}

std::optional<PackJson> ParsePackJson(const std::string& text, std::string* error) {
    auto fail = [&](const std::string& message) -> std::optional<PackJson> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    std::string parse_error;
    const std::optional<Json> root = ParseJson(text, &parse_error);
    if (!root) {
        return fail("pack.json: " + parse_error);
    }
    const Json* options = root->Find("options");
    if (!options || !options->IsObject()) {
        return fail("pack.json: no \"options\" object");
    }
    PackJson result;
    result.options.has_pack_json = true;
    struct Flag {
        const char* name;
        bool* target;
    };
    for (const Flag& flag : {Flag{"skip_mipmap", &result.options.skip_mipmap},
                             Flag{"flip_png_files", &result.options.flip_png_files},
                             Flag{"use_new_hash", &result.options.use_new_hash}}) {
        const Json* value = options->Find(flag.name);
        if (!value || !value->IsBool()) {
            return fail(fmt::format("pack.json: options.{} is missing or not a boolean", flag.name));
        }
        *flag.target = value->boolean;
    }
    const Json* textures = root->Find("textures");
    if (textures) {
        if (!textures->IsObject()) {
            return fail("pack.json: \"textures\" is not an object");
        }
        for (const auto& [key, value] : textures->object) {
            const std::optional<u64> hash = ParseHashKey(key);
            if (!hash) {
                MH4N_LOG(Warning, "HdPack", "pack.json: key {} is invalid, skipping", key);
                continue;
            }
            auto add = [&](const std::string& file) {
                result.hashes_by_file[FileName(file)].push_back(*hash);
            };
            if (value.IsString()) {
                add(value.string);
            } else if (value.IsArray()) {
                for (const Json& entry : value.array) {
                    if (entry.IsString()) {
                        add(entry.string);
                    }
                }
            } else {
                MH4N_LOG(Warning, "HdPack", "pack.json: material with key {} is invalid", key);
            }
        }
    }
    return result;
}

std::optional<Pack> ScanPack(const std::string& dir, std::string* error) {
    auto fail = [&](const std::string& message) -> std::optional<Pack> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return fail(dir + ": not a directory");
    }
    Pack pack;
    std::map<std::string, std::vector<u64>> mapping;
    const fs::path config = fs::path(dir) / "pack.json";
    if (fs::is_regular_file(config, ec)) {
        std::string text;
        if (!ReadFile(config, text)) {
            return fail(config.string() + ": cannot read");
        }
        std::string json_error;
        std::optional<PackJson> json = ParsePackJson(text, &json_error);
        if (!json) {
            return fail(json_error);
        }
        pack.options = json->options;
        mapping = std::move(json->hashes_by_file);
    }

    // Azahar scans 64 levels deep.
    std::vector<fs::path> paths;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return fail(dir + ": " + ec.message());
    }
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            return fail(dir + ": " + ec.message());
        }
        if (it.depth() >= 64) {
            it.disable_recursion_pending();
        }
        if (it->is_regular_file(ec)) {
            paths.push_back(it->path());
        }
    }
    std::vector<std::pair<std::string, fs::path>> sorted;
    sorted.reserve(paths.size());
    for (const fs::path& path : paths) {
        sorted.emplace_back(fs::relative(path, dir, ec).generic_string(), path);
    }
    std::sort(sorted.begin(), sorted.end());

    for (const auto& [relative, path] : sorted) {
        const std::string name = path.filename().string();
        // ParseFilename: more than three parts is invalid; the last is the extension.
        std::vector<std::string> parts = Split(name, '.');
        if (parts.size() > 3) {
            ++pack.stats.ignored_names;
            continue;
        }
        const FileFormat format = MakeFileFormat(parts.back());
        if (format == FileFormat::None) {
            ++pack.stats.ignored_names;
            continue;
        }
        if (format == FileFormat::Dds && pack.options.skip_mipmap) {
            ++pack.stats.ignored_dds; // "Mipmap skip is incompatible with DDS textures"
            continue;
        }
        parts.pop_back();
        MapType type = MapType::Color;
        if (parts.size() > 1) {
            if (parts.back() == "norm") {
                type = MapType::Normal;
            } else {
                MH4N_LOG(Warning, "HdPack", "{}: unknown material extension {}, using colour",
                         relative, parts.back());
            }
            parts.pop_back();
        }
        PackFile file;
        file.path = relative;
        file.format = format;
        file.type = type;
        const auto mapped = mapping.find(name);
        if (mapped != mapping.end()) {
            file.hashes = mapped->second;
        }
        // A pack may keep the default name and still map the file to other hashes.
        if (const std::optional<TextureStem> stem = ParseTextureStem(parts.back())) {
            if (std::find(file.hashes.begin(), file.hashes.end(), stem->hash) == file.hashes.end()) {
                file.hashes.push_back(stem->hash);
            }
        }
        if (file.hashes.empty()) {
            ++pack.stats.files_without_hash;
            continue;
        }
        file.size = fs::file_size(path, ec);
        const auto mtime = fs::last_write_time(path, ec);
        file.mtime = static_cast<s64>(mtime.time_since_epoch().count());
        pack.files.push_back(std::move(file));
    }

    // Material::AddMapTexture: a second file for the same map of a hash is ignored.
    for (std::size_t i = 0; i < pack.files.size(); ++i) {
        const PackFile& file = pack.files[i];
        for (const u64 hash : file.hashes) {
            PackMaterial& material = pack.materials[hash];
            int& slot = material.file[static_cast<std::size_t>(file.type)];
            if (slot >= 0) {
                if (slot != static_cast<int>(i)) {
                    ++pack.stats.duplicate_maps;
                    MH4N_LOG(Warning, "HdPack", "{} and {} are assigned to the same material, ignoring the second",
                             pack.files[static_cast<std::size_t>(slot)].path, file.path);
                }
                continue;
            }
            slot = static_cast<int>(i);
        }
    }
    return pack;
}

} // namespace mh4n::hdpack
