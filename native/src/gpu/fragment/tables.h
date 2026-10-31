// Derived from Azahar src/video_core/renderer_vulkan/vk_rasterizer.cpp (the texel-buffer views at 88-110;
// SyncAndUploadLUTsLF and SyncAndUploadLUTs at 906-1030: which table goes into which ring, the sizes
// mapped, the element offsets kept in the uniforms, "invalidate" after a wrap) and
// src/video_core/pica/pica_core.h (the table entry layouts: ProcTex::ValueEntry, ColorEntry,
// ColorDifferenceEntry, Lighting::LutEntry, Fog::LutEntry) and src/video_core/shader/generator/glsl_fs_shader_gen.cpp
// (DefineBindingsVK: the set and binding numbers 3, 4 and 5 of the three texel buffers) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp and glsl_fs_shader_gen.cpp say
// "Licensed under GPLv3 or any later version"; pica_core.h says "GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (vk_rasterizer.cpp, glsl_fs_shader_gen.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h)
//
// The lookup-table rings (N2-33; docs/native/gpu-fragment-design.md 5.10). Core has no table of its own: the
// features fill the rings through `FeatureModule::UploadTables` when `PicaState` says a table changed.
//
// Two texel-buffer rings, three views, as Azahar:
//   ring `Lf`       lut_lf   (binding 3)  R32G32_SFLOAT       the lighting tables (24 x 256 entries) and the fog table (128)
//   ring `General`  lut_rg   (binding 4)  R32G32_SFLOAT       procedural noise, colour map and alpha map (128 entries each)
//                   lut_rgba (binding 5)  R32G32B32A32_SFLOAT the procedural colour table and its difference table (256 each)
// The offset of each table inside its buffer, in elements of the view's format, lives in the uniforms
// (`lighting_lut_offset`, `fog_lut_offset`, `proctex_*_offset`), so an upload never rebuilds a binding: the
// views cover the whole ring buffer (the vendored backend cannot bind a texel buffer at an offset).
//
// One upload per ring per draw: `UploadTables` maps the ring's worst case once (Azahar's `max_size`), the
// features take slices of it, and the session is committed with the bytes used. A wrap of the ring (the
// new allocation restarts at the beginning) makes the tables of that ring all dirty again, as Azahar's
// "invalidate" does.
//
// Final names (the design called them proposals): `TableRings`, `TableRing`, `TableDirty`, `TableUpload`,
// `TableSlice`, `UploadTables`.
#pragma once

#include <memory>
#include <span>

#include "gpu/backend/rhi.h"
#include "gpu/fragment/uniforms.h"

namespace mh4n::gpu::fragment {

enum class TableRing : u8 { Lf, General };

inline constexpr rhi::Format kLutLfFormat = rhi::Format::Rg32Float;
inline constexpr rhi::Format kLutRgFormat = rhi::Format::Rg32Float;
inline constexpr rhi::Format kLutRgbaFormat = rhi::Format::Rgba32Float;

// The worst case of one upload to each ring: Azahar's `max_size` of each sync.
inline constexpr u64 kLfRingMaxUpload = 8ull * 256 * kNumLightingSamplers + 8ull * 128;  // 50,176
inline constexpr u64 kGeneralRingMaxUpload = 8ull * 128 * 3 + 16ull * 256 + 16ull * 256;  // 11,264
inline constexpr u64 kTableAlignment = 16;  // sizeof(Vec4f): the view of the second format needs it

struct TableRingsDesc {
    u64 lf_bytes = 4ull << 20;
    u64 general_bytes = 1ull << 20;
};

// A piece of a mapped ring: `data` is writable; `byte_offset` is in the ring's buffer.
struct TableSlice {
    std::span<u8> data;
    u64 byte_offset = 0;
    // The offset in elements of a view whose texel is `element_bytes` wide (the value kept in the uniforms).
    int ElementOffset(u32 element_bytes) const { return static_cast<int>(byte_offset / element_bytes); }
};

class TableRings {
public:
    static rhi::Result<std::unique_ptr<TableRings>> Create(rhi::Backend& backend, const TableRingsDesc& desc = {});
    ~TableRings();
    TableRings(const TableRings&) = delete;
    TableRings& operator=(const TableRings&) = delete;

    // The buffer behind a ring (for the binding set: lut_lf is Lf, lut_rg and lut_rgba are General).
    const rhi::Buffer& Buffer(TableRing ring) const;

    // Opens an upload session: maps `max_bytes` (a multiple of 16) in the ring. Returns true when the ring wrapped,
    // so every table in it must be uploaded again. One session per ring at a time; needs the calling thread's open
    // recorder (Ring::Commit does).
    rhi::Result<bool> Begin(TableRing ring, u64 max_bytes);
    // The next `bytes` of the session (a multiple of 16), and nothing when the session's room is used up.
    rhi::Result<TableSlice> Take(TableRing ring, u64 bytes);
    // Commits the bytes taken (the space is free again once the recorder's submission has passed).
    rhi::Result<void> End(TableRing ring);

    // Wraps counted since creation, for tests.
    u64 Wraps(TableRing ring) const;

private:
    TableRings();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Which tables to upload: the bits of `PicaState`'s dirty flags, widened to "all" after a wrap of the ring.
struct TableDirty {
    u32 lighting = 0;  // bit n: lighting table n
    bool fog = false;
    u8 proctex = 0;    // bit n: the procedural table with ProcTexLutTable value n (noise 0, colour map 2, alpha map 3,
                       // colour 4, colour difference 5)
    bool operator==(const TableDirty&) const = default;
};

// What a feature's `UploadTables` gets.
struct TableUpload {
    const pica::PicaState& state;
    TableRings& rings;
    FragmentBlock& block;
    UniformDirty& dirty;
    TableDirty need;
};

struct UploadSummary {
    bool uploaded = false;
    bool lf_wrapped = false;
    bool general_wrapped = false;
};

// One upload per ring that has something dirty, as described above, calling each registered feature's
// `UploadTables` between `Begin` and `End`. Does not clear the dirty flags of `state` (the command processor
// does after Draw). On the thread that owns the state, with its recorder open.
rhi::Result<UploadSummary> UploadTables(const pica::PicaState& state, TableRings& rings, FragmentBlock& block,
                                        UniformDirty& dirty);
class FeatureRegistry;
rhi::Result<UploadSummary> UploadTables(const pica::PicaState& state, TableRings& rings, FragmentBlock& block,
                                        UniformDirty& dirty, const FeatureRegistry& features);

} // namespace mh4n::gpu::fragment
