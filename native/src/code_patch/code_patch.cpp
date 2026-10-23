// Derived from Azahar src/core/file_sys/ncch_container.cpp and src/core/loader/ncch.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: ncch_container.cpp says "GPLv3 or any later
// version"; ncch.cpp says "GPLv2 or any later version"): the mods folder's id (GetModId), the places and
// order of the code replacement (LoadOverrideExeFSSection), of the code patches (ApplyCodePatch) and of
// the exheader override, and the bss pages appended before the patch (ncch.cpp).
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ncch.cpp)
#include "code_patch.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "layeredfs/patch.h"

namespace mh4n::code_patch {

namespace {

bool ReadWhole(const std::string& path, std::vector<u8>* out, bool* opened) {
    std::ifstream file(path, std::ios::binary);
    *opened = static_cast<bool>(file);
    if (!file) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

bool Exists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return static_cast<bool>(file);
}

std::string Join(const std::string& dir, const std::string& rel) {
    if (dir.empty() || dir.back() == '/' || dir.back() == '\\') {
        return dir + rel;
    }
    return dir + "/" + rel;
}

}  // namespace

u64 ModId(u64 program_id) {
    constexpr u64 kUpdateMask = 0x0000000e'00000000ull;
    if ((program_id & 0x000000ff'00000000ull) == kUpdateMask) {
        return program_id & ~kUpdateMask;
    }
    return program_id;
}

std::string ModsPath(const std::string& load_dir, u64 program_id) {
    char id[17];
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(ModId(program_id)));
    return Join(load_dir, std::string("mods/") + id);
}

u32 BssPages(u32 bss_size) {
    return static_cast<u32>((static_cast<u64>(bss_size) + (kPageSize - 1)) / kPageSize);
}

ModFolder::ModFolder(std::string path) : path_(std::move(path)) {}

Outcome ModFolder::ApplyToCode(std::vector<u8>& code, u32 bss_pages) const {
    Outcome outcome;

    // 1. A replacement of the code, before the bss pages.
    for (const char* rel : {"exefs/code.bin", "code.bin"}) {
        std::vector<u8> data;
        bool opened = false;
        if (ReadWhole(Join(path_, rel), &data, &opened)) {
            code = std::move(data);
            outcome.applied.push_back(rel);
            break;
        }
    }

    // 2. The bss pages.
    code.resize(code.size() + static_cast<std::size_t>(bss_pages) * kPageSize, 0);

    // 3. The first patch that exists.
    struct Location {
        const char* rel;
        bool ips;
    };
    constexpr std::array<Location, 4> kPatches{{
        {"exefs/code.ips", true},
        {"exefs/code.bps", false},
        {"code.ips", true},
        {"code.bps", false},
    }};
    for (const Location& location : kPatches) {
        const std::string path = Join(path_, location.rel);
        if (!Exists(path)) {
            continue;
        }
        const std::string error = std::string("mods: ") + location.rel + ": patch error";
        std::vector<u8> patch;
        bool opened = false;
        if (!ReadWhole(path, &patch, &opened)) {
            return Outcome{false, error, "unreadable", outcome.applied};
        }
        using layeredfs::patch::Status;
        const Status status =
            location.ips ? layeredfs::patch::ApplyIps(patch, code) : layeredfs::patch::ApplyBps(patch, code);
        if (status != Status::Success) {
            return Outcome{false, error,
                           status == Status::ErrorPatchesInvalidTitle ? "source CRC-32 differs" : "malformed",
                           outcome.applied};
        }
        outcome.applied.push_back(location.rel);
        break;
    }
    return outcome;
}

std::optional<std::vector<u8>> ModFolder::ExheaderOverride() const {
    std::vector<u8> data;
    bool opened = false;
    if (!ReadWhole(Join(path_, "exheader.bin"), &data, &opened) || data.size() < kExheaderSize) {
        return std::nullopt;
    }
    data.resize(kExheaderSize);
    return data;
}

}  // namespace mh4n::code_patch
