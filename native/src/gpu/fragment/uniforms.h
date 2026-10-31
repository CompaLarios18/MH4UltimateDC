// Derived from Azahar src/video_core/shader/generator/shader_uniforms.h (FSUniformData, LightSrc),
// src/video_core/rasterizer_accelerated.cpp (SyncDrawUniforms: which registers set which member and
// when) and src/video_core/rasterizer_cache/framebuffer_base.h (the scissor rectangle) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: shader_uniforms.h,
// rasterizer_accelerated.cpp, framebuffer_base.h and pica/dirty_regs.h each say "Licensed under GPLv2
// or any later version").
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (shader_uniforms.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_accelerated.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (framebuffer_base.h)
// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project (dirty_regs.h)
//
// The fragment uniforms (N2-33; docs/native/gpu-fragment-design.md 5.10).
//
// ONE UNIFORM BLOCK, one ring allocation per fragment-state change (`FragmentBlock`):
//
//     offset    size   member           what
//        0     1328   uniforms         `FragmentUniforms`: Azahar's FSUniformData, std140 as it was
//     1328      280   cfg              the `FragmentKey` itself (70 words), written with every fragment-uniform
//                                      write, in both modes (the ubershader reads it; the specialised shader does not)
//     1608        8   (padding)        so the gas region starts on a 16-byte boundary
//     1616       64   gas.regs[16]     the 16 raw gas register words (0x120-0x12F), owned by `fog/`
//     1680       64   gas.table[16]    the 16-entry gas table, raw words, owned by `fog/` (no data source yet: N2-38)
//     1744                             sizeof(FragmentBlock); the ring allocation is 1792 (a multiple of 256)
//
// A std140 declaration for the generator: `layout(std140, set = 0, binding = 2) uniform FSBlock { ...the
// members of FragmentUniforms in order...; uvec4 cfg[18]; uvec4 gas_regs[4]; uvec4 gas_table[4]; };`
// (cfg is 70 words in 72 words of room; gas_regs and gas_table are 16 words each).
//
// Final names (the design called them proposals): `FragmentUniforms`, `FragmentBlock`, `GasUniforms`,
// `UniformDirty`, `UniformContext`, `FillUniforms`, `kFragmentBlockBytes`, `kFragmentBlockAllocation`.
#pragma once

#include <cstddef>
#include <optional>
#include <type_traits>

#include "gpu/fragment/key.h"
#include "mh4n/types.h"

namespace mh4n::gpu::pica {
struct PicaState;
}

namespace mh4n::gpu::fragment {

struct Vec2f {
    float x, y;
};
struct Vec3f {
    float x, y, z;
};
struct Vec4f {
    float x, y, z, w;
};
struct Vec4i {
    s32 x, y, z, w;
};

// Azahar's LightSrc (Vec3f members are 12 bytes at 16-byte alignment).
struct LightSrc {
    alignas(16) Vec3f specular_0;
    alignas(16) Vec3f specular_1;
    alignas(16) Vec3f diffuse;
    alignas(16) Vec3f ambient;
    alignas(16) Vec3f position;
    alignas(16) Vec3f spot_direction;  // negated
    float dist_atten_bias;
    float dist_atten_scale;
};

inline constexpr u32 kNumLightingSamplers = 24;

// Azahar's FSUniformData (0x530 = 1,328 bytes). The offsets are asserted below against the values worked
// out from Azahar's declaration.
struct FragmentUniforms {
    int framebuffer_scale;
    int alphatest_ref;
    float depth_scale;
    float depth_offset;
    float shadow_bias_constant;
    float shadow_bias_linear;
    int scissor_x1;
    int scissor_y1;
    int scissor_x2;
    int scissor_y2;
    int fog_lut_offset;
    int proctex_noise_lut_offset;
    int proctex_color_map_offset;
    int proctex_alpha_map_offset;
    int proctex_lut_offset;
    int proctex_diff_lut_offset;
    float proctex_bias;
    int shadow_texture_bias;
    alignas(16) Vec4i lighting_lut_offset[kNumLightingSamplers / 4];
    alignas(16) Vec3f fog_color;
    alignas(8) Vec2f proctex_noise_f;
    alignas(8) Vec2f proctex_noise_a;
    alignas(8) Vec2f proctex_noise_p;
    alignas(16) Vec3f lighting_global_ambient;
    LightSrc light_src[8];
    alignas(16) Vec4f const_color[6];  // a colour for each of the six TEV stages
    alignas(16) Vec4f tev_combiner_buffer_color;
    alignas(16) Vec3f tex_lod_bias;
    alignas(16) Vec4f tex_border_color[3];
    alignas(16) Vec4f blend_color;
};

static_assert(sizeof(FragmentUniforms) == 0x530);
static_assert(alignof(FragmentUniforms) == 16);
static_assert(sizeof(LightSrc) == 112);
static_assert(offsetof(FragmentUniforms, framebuffer_scale) == 0);
static_assert(offsetof(FragmentUniforms, alphatest_ref) == 4);
static_assert(offsetof(FragmentUniforms, depth_scale) == 8);
static_assert(offsetof(FragmentUniforms, depth_offset) == 12);
static_assert(offsetof(FragmentUniforms, shadow_bias_constant) == 16);
static_assert(offsetof(FragmentUniforms, shadow_bias_linear) == 20);
static_assert(offsetof(FragmentUniforms, scissor_x1) == 24);
static_assert(offsetof(FragmentUniforms, scissor_y1) == 28);
static_assert(offsetof(FragmentUniforms, scissor_x2) == 32);
static_assert(offsetof(FragmentUniforms, scissor_y2) == 36);
static_assert(offsetof(FragmentUniforms, fog_lut_offset) == 40);
static_assert(offsetof(FragmentUniforms, proctex_noise_lut_offset) == 44);
static_assert(offsetof(FragmentUniforms, proctex_color_map_offset) == 48);
static_assert(offsetof(FragmentUniforms, proctex_alpha_map_offset) == 52);
static_assert(offsetof(FragmentUniforms, proctex_lut_offset) == 56);
static_assert(offsetof(FragmentUniforms, proctex_diff_lut_offset) == 60);
static_assert(offsetof(FragmentUniforms, proctex_bias) == 64);
static_assert(offsetof(FragmentUniforms, shadow_texture_bias) == 68);
static_assert(offsetof(FragmentUniforms, lighting_lut_offset) == 80);
static_assert(offsetof(FragmentUniforms, fog_color) == 176);
static_assert(offsetof(FragmentUniforms, proctex_noise_f) == 192);
static_assert(offsetof(FragmentUniforms, proctex_noise_a) == 200);
static_assert(offsetof(FragmentUniforms, proctex_noise_p) == 208);
static_assert(offsetof(FragmentUniforms, lighting_global_ambient) == 224);
static_assert(offsetof(FragmentUniforms, light_src) == 240);
static_assert(offsetof(FragmentUniforms, const_color) == 1136);
static_assert(offsetof(FragmentUniforms, tev_combiner_buffer_color) == 1232);
static_assert(offsetof(FragmentUniforms, tex_lod_bias) == 1248);
static_assert(offsetof(FragmentUniforms, tex_border_color) == 1264);
static_assert(offsetof(FragmentUniforms, blend_color) == 1312);
static_assert(std::is_trivially_copyable_v<FragmentUniforms>);

// The reserved gas region, owned by `fog/` (N2-10): the raw words only; the feature decodes them.
struct GasUniforms {
    u32 regs[16];   // GPUREG 0x120-0x12F, as the registers hold them
    u32 table[16];  // the 16-entry gas table (no data source in PicaState yet: N2-38)
};
static_assert(sizeof(GasUniforms) == 128);

// What one ring allocation holds.
struct FragmentBlock {
    FragmentUniforms uniforms;
    alignas(16) FragmentKey cfg;
    u32 cfg_padding[2];
    GasUniforms gas;
};
static_assert(offsetof(FragmentBlock, uniforms) == 0);
static_assert(offsetof(FragmentBlock, cfg) == 1328);
static_assert(offsetof(FragmentBlock, gas) == 1616);
static_assert(sizeof(FragmentBlock) == 1744);
static_assert(std::is_trivially_copyable_v<FragmentBlock>);

inline constexpr u32 kFragmentUniformsBytes = sizeof(FragmentUniforms);
inline constexpr u32 kFragmentBlockBytes = sizeof(FragmentBlock);
// The ring allocation of a block: aligned to 256, the largest alignment Vulkan allows for an offset.
inline constexpr u32 kFragmentBlockAlignment = 256;
inline constexpr u32 kFragmentBlockAllocation = (kFragmentBlockBytes + kFragmentBlockAlignment - 1) / kFragmentBlockAlignment * kFragmentBlockAlignment;
static_assert(kFragmentBlockAllocation == 1792);

// Set when any member was written since the flag was cleared (a draw whose flag stays clear reuses the
// previous allocation).
//
// `all` is set by the caller on the first use of a block (a `FragmentState` made after the processor's first draws sees
// no dirty bit): every hook then writes all its members, whatever the dirty bits say.
struct UniformDirty {
    bool changed = false;
    bool all = false;
    void Mark() { changed = true; }
};

// Inputs of the uniforms that do not come from the registers: the surface cache's (N2-15). The defaults are
// what a 1x framebuffer with the PICA's own origin gives.
struct UniformContext {
    int framebuffer_scale = 1;
    // The scissor rectangle in host pixels, as Azahar's FramebufferHelper::Scissor() computes it (left, bottom,
    // right, top of the PICA rectangle, scaled, with 1 added to x2 and y2, mirrored when the framebuffer is
    // flipped). Unset: the PICA's rectangle at scale 1 (x1, y1, x2 + 1, y2 + 1).
    struct Scissor {
        int x1, y1, x2, y2;
        bool operator==(const Scissor&) const = default;
    };
    std::optional<Scissor> scissor;
};

class FeatureRegistry;

// Writes core's members of `uniforms`: `const_color[6]` and `tev_combiner_buffer_color` (when a TEV constant
// register is dirty), `alphatest_ref` and `blend_color` (alpha test or blend constant dirty), `depth_scale`
// and `depth_offset` (depth map registers dirty), and the scissor rectangle and `framebuffer_scale` (compared
// with the stored values at every call, because they depend on `context`). `dirty.changed` is set when
// anything was written. Runs on the thread that owns the command processor (`state` is valid only during
// Draw).
void FillCoreUniforms(const pica::PicaState& state, FragmentUniforms& uniforms, UniformDirty& dirty,
                      const UniformContext& context = {});

// `FillCoreUniforms` on `block.uniforms`, then each registered feature's `FillUniforms` (they get the whole block:
// their members of `uniforms` and, for `fog/`, the gas region). This is the spec's
// `FillUniforms(PicaState, FragmentUniforms&, UniformDirty&)` with the block in place of the structure alone,
// because a feature's hook has to reach the reserved gas region.
void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty,
                  const UniformContext& context = {});
void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty,
                  const UniformContext& context, const FeatureRegistry& features);

// Azahar's PicaToVK::ColorRGBA8 (r, g, b, a bytes of a register word to 0..1), as rasterizer_accelerated.cpp
// has it.
Vec4f ColorRGBA8(u32 word);

// Copies the key into `cfg` (written with every fragment-uniform write). Returns whether it changed.
bool StoreKey(const FragmentKey& key, FragmentBlock& block);

} // namespace mh4n::gpu::fragment
