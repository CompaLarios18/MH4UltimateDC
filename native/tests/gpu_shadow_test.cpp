// Derived from Azahar src/video_core/renderer_software/sw_framebuffer.cpp (DecodeD24S8Shadow, EncodeD24X8Shadow,
// EncodeX24S8Shadow and Framebuffer::DrawShadowMapPixel: the shadow texel's bytes, the write rule, the buffer row a window row is
// stored in), src/video_core/renderer_software/sw_rasterizer.cpp (the shadow branch of the fragment loop: the depth, clamped, and
// the green intensity; RasterizerSoftware::TextureColor: the Shadow2D coordinate, the texel index, the border test, the t flip
// and the compare), src/video_core/renderer_software/sw_clipper.cpp (ConvertCubeCoord), src/video_core/renderer_software/
// sw_texturing.cpp (GetWrappedTexCoord), src/video_core/rasterizer_accelerated.cpp (the shadow uniforms), src/video_core/
// pica_types.h (Float::FromRaw, Float::operator* and the float24 layout of F24), src/video_core/shader/generator/
// glsl_fs_shader_gen.cpp (what the shadow-write shader declares) and, for the registers the cases write, src/video_core/pica/
// regs_internal.h (ASSERT_REG_POSITION), regs_framebuffer.h (the colour buffer, the output merger, the shadow bias word),
// regs_texturing.h (the texture unit words, the shadow texture word, the TEV stage words), regs_rasterizer.h (the viewport, the
// depth range, the output map and the semantics), regs_pipeline.h (the vertex arrays and the draw triggers), regs_shader.h (the
// vertex program setup) and regs_lighting.h (the lighting disable word), at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: glsl_fs_shader_gen.cpp says "Licensed under GPLv3 or any later version"; every other file named here says
// "Licensed under GPLv2 or any later version").
// The rig and the `--gpu native` helpers are adapted from N2-10's and N2-23's tests in this directory (gpu_fog_test,
// gpu_sampling_test; GPL-3.0-or-later). The vertex-program words of the `--gpu native` case place their fields where
// externals/nihstro/include/nihstro/shader_bytecode.h does (the submodule Azahar pins at
// f4d8659decbfe5d234f04134b5002b82dc515a44; it is NOT GPL: its licence is the BSD 3-clause licence of
// externals/nihstro/license.txt, reproduced below because it asks for that); the file as a whole is GPL-3.0-or-later.
// Copyright 2017 Citra Emulator Project (sw_framebuffer.cpp, sw_texturing.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp, pica_types.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (sw_clipper.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_accelerated.cpp, glsl_fs_shader_gen.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_framebuffer.h, regs_pipeline.h,
//   regs_shader.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
// Copyright 2017 Citra Emulator Project (regs_lighting.h)
// Copyright 2014 Tony Wasserka (shader_bytecode.h)
//
//   Copyright 2014 Tony Wasserka
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without modification, are permitted
//   provided that the following conditions are met:
//       * Redistributions of source code must retain the above copyright notice, this list of
//         conditions and the following disclaimer.
//       * Redistributions in binary form must reproduce the above copyright notice, this list of
//         conditions and the following disclaimer in the documentation and/or other materials
//         provided with the distribution.
//       * Neither the name of the owner nor the names of its contributors may be used to endorse or
//         promote products derived from this software without specific prior written permission.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
//   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
//   AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
//   OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//   POSSIBILITY OF SUCH DAMAGE.
//
// Shadows (N2-12): the shadow write and the shadow lookups, on whatever Vulkan device the machine has (MH4N_VULKAN_DEVICE; a
// case that needs one skips with a message without it and fails under MH4N_REQUIRE_VULKAN=1).
//
// WHERE THE EXPECTED VALUES COME FROM. A CPU port, below (`sw`), of the SOFTWARE renderer: DrawShadowMapPixel on the word of a
// texel (fp32, Pica's infinity times zero, the clamp and the truncation), TextureColor's shadow branch (the texel index truncated
// toward zero, GetWrappedTexCoord as written, the border colour compared as a texel, the t flip) and ConvertCubeCoord. The maps
// are words in the decoder's order (row 0 is the first decoded row); the upload reverses the rows and the write-back puts image
// row y at guest row h - 1 - y, so a fragment at image row y writes decoded row h - 1 - y, and a lookup of texel (s, t) reads
// decoded row h - 1 - t, as DrawShadowMapPixel and TextureColor do.
//
// EXACTNESS. d and z_int are near 2^24 and truncated: every depth and every |w| the cases use is a float in [0.125, 0.25) whose
// fp32 product by 16777215 is a whole number plus 0.5 (DepthFor), so one ulp either way gives the same integer, and the
// primitives have that depth and w constant over them (clip w = 1, depth scale -1, offset 0: `depth` is gl_FragCoord.z). s', the
// texel indices and the cube face choices are held at least 0.01 from a decision boundary (`sw::g_margin`, checked per case).
//
// BOTH WRITE PATHS, BOTH BORDER PATHS, BOTH MODES. Every composer case runs on the device as it is and on a second backend made in
// the same process with `interlock` and `custom_border_color` masked (the compare-and-swap path and the shader's border
// emulation), each in the specialised shader and in the ubershader: all four must give the port's values exactly.
#include <doctest/doctest.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/cpuvertex/cpu_vertex.h"
#include "gpu/cpuvertex/triangle_batch.h"
#include "gpu/fbops/fixed_function.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"
#include "gpu/fragment/key.h"
#include "gpu/fragment/profile.h"
#include "gpu/fragment/sampling/sampling.h"
#include "gpu/fragment/state.h"
#include "gpu/fragment/tables.h"
#include "gpu/fragment/uniforms.h"
#include "gpu/native/bindings.h"
#include "gpu/native/compose.h"
#include "gpu/native/default_provider.h"
#include "gpu/native/native_gpu.h"
#include "gpu/native/pipeline_builder.h"
#include "gpu/pica/state.h"
#include "gpu/shader/vertex_shader.h"
#include "gpu/texture/decode.h"
#include "gpu_capture/capture_host.h"
#include "mh4n/gpu.h"
#include "support/vulkan_device.h"

using namespace mh4n;
using namespace mh4n::gpu::fragment;
namespace re = mh4n::gpu::fragment::regenum;
namespace native = mh4n::gpu::native;
namespace cpuvertex = mh4n::gpu::cpuvertex;
namespace fbops = mh4n::gpu::fbops;
namespace shader = mh4n::gpu::shader;
namespace texture = mh4n::gpu::texture;
namespace rhi = mh4n::gpu::rhi;
namespace pica = mh4n::gpu::pica;
namespace test = mh4n::test;

namespace {

constexpr u32 kSize = 8;  // the colour target of the lookups, and most maps

// ---- the port of the software renderer --------------------------------------------------------------------------------

namespace sw {

// The smallest distance of a decision value from its boundary in the current case (reset per case).
double g_margin = 1e9;
void Margin(double distance) { g_margin = std::min(g_margin, std::fabs(distance)); }
void IndexMargin(double scaled) { Margin(scaled - std::round(scaled)); }  // a value truncated to an integer

// Pica::f16::FromRaw(half).ToFloat32() (pica_types.h).
float HalfToFloat(u32 half) {
    const u32 sign = (half >> 15) & 1u;
    s32 exponent = static_cast<s32>((half >> 10) & 0x1Fu);
    u32 mantissa = half & 0x3FFu;
    u32 bits;
    if (exponent == 0 && mantissa == 0) {
        bits = sign << 31;
    } else if (exponent == 0) {
        exponent = 127 - 15 + 1;
        while ((mantissa & 0x400u) == 0) {
            --exponent;
            mantissa <<= 1;
        }
        mantissa &= 0x3FFu;
        bits = (sign << 31) | (static_cast<u32>(exponent) << 23) | (mantissa << 13);
    } else if (exponent == 0x1F) {
        bits = (sign << 31) | (0xFFu << 23) | (mantissa << 13);
    } else {
        bits = (sign << 31) | (static_cast<u32>(exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

// Float::operator*: the PICA gives 0 instead of NaN when multiplying by infinity.
float PicaMul(float a, float b) {
    float result = a * b;
    if (std::isnan(result) && !std::isnan(a) && !std::isnan(b)) result = 0.0f;
    return result;
}

// DrawShadowMapPixel on a texel's word, (ref_z << 8) | ref_s.
u32 Write(u32 word, u32 d, u32 s, u32 c_raw, u32 l_raw) {
    const u32 ref_z = word >> 8, ref_s = word & 0xFFu;
    if (d >= ref_z) return word;
    if (s == 0) return (d << 8) | ref_s;
    const float c = HalfToFloat(c_raw), l = HalfToFloat(l_raw);
    const float x = static_cast<float>(d) / static_cast<float>(ref_z);
    const float s_new = static_cast<float>(s) / (c + PicaMul(l, x));
    if (std::isfinite(s_new) && s_new > 0.0f && s_new < 255.0f) IndexMargin(s_new);
    const u8 stencil = static_cast<u8>(std::clamp(s_new, 0.0f, 255.0f));
    return stencil < ref_s ? ((ref_z << 8) | stencil) : word;
}

// GetWrappedTexCoord (sw_texturing.cpp), as written.
s32 Wrap(u32 mode, s32 val, u32 size) {
    switch (mode) {
    case 4:  // ClampToEdge2
        if (val < 0) return static_cast<s32>(static_cast<u32>(val) % size);
        [[fallthrough]];
    case 0: return std::min(std::max(val, 0), static_cast<s32>(size) - 1);
    case 1: return val;
    case 5:
    case 6:
    case 7:
    case 2: return static_cast<s32>(static_cast<u32>(val) % size);
    case 3: {
        u32 coord = static_cast<u32>(val) % (2 * size);
        if (coord >= size) coord = 2 * size - 1 - coord;
        return static_cast<s32>(coord);
    }
    default: return 0;
    }
}

bool Border(u32 mode, s32 val, u32 size) {
    return (mode == 1 && (val < 0 || val >= static_cast<s32>(size))) || (mode == 5 && val >= static_cast<s32>(size));
}

// A map: its words in the decoder's order.
struct Map {
    u32 w = 0, h = 0;
    std::vector<u32> words;
    u32 At(u32 col, u32 row) const { return words[static_cast<size_t>(row) * w + col]; }
};

// The unit-0 state of a lookup.
struct Lookup {
    u32 wrap_s = 0, wrap_t = 0;
    u32 border = 0;        // the register word (red in bits 0-7): as a texel it is this very word
    u32 shadow_word = 1;   // 0x8B: bit 0 orthographic, bits 1-23 the bias
    bool enabled = true;
};

// TextureColor's compare: lit when the texel's depth is at least z_int, (i, i, i, i) with i the texel's intensity.
int Compare(u32 word, float z, u32 shadow_word) {
    s32 z_int = static_cast<s32>(std::min(z, 1.0f) * 16777215.0f);
    z_int -= static_cast<s32>(shadow_word & 0x00FFFFFEu);
    const s32 z_ref = static_cast<s32>(word >> 8);
    return z_ref >= z_int ? static_cast<int>(word & 0xFFu) : 0;
}

// The texel the face (or the 2D map) gives at (s, t), or the border word.
u32 Select(const Map& m, const Lookup& lk, s32 s, s32 t) {
    if (Border(lk.wrap_s, s, m.w) || Border(lk.wrap_t, t, m.h)) return lk.border;
    const s32 ws = Wrap(lk.wrap_s, s, m.w);
    const s32 wt = static_cast<s32>(m.h) - 1 - Wrap(lk.wrap_t, t, m.h);
    return m.At(static_cast<u32>(ws), static_cast<u32>(wt));
}

// Shadow2D at (u, v) and w (TextureColor).
int Shadow2D(const Map& m, const Lookup& lk, double u_d, double v_d, double w_d) {
    if (!lk.enabled) return 0;
    float u = static_cast<float>(u_d), v = static_cast<float>(v_d);
    const float w = static_cast<float>(w_d);
    const float z = std::fabs(w);
    if ((lk.shadow_word & 1u) == 0) {
        u /= w;
        v /= w;
        u_d /= w_d;
        v_d /= w_d;
    }
    IndexMargin(u_d * m.w);
    IndexMargin(v_d * m.h);
    const s32 s = static_cast<s32>(u * static_cast<float>(m.w));
    const s32 t = static_cast<s32>(v * static_cast<float>(m.h));
    return Compare(Select(m, lk, s, t), z, lk.shadow_word);
}

// ShadowCube at (u, v, w): ConvertCubeCoord's face (+X, -X, +Y, -Y, +Z, -Z) and coordinates, then Shadow2D's rules.
int ShadowCube(const std::array<Map, 6>& faces, const Lookup& lk, double u_d, double v_d, double w_d) {
    if (!lk.enabled) return 0;
    const float u = static_cast<float>(u_d), v = static_cast<float>(v_d), w = static_cast<float>(w_d);
    const float au = std::fabs(u), av = std::fabs(v), aw = std::fabs(w);
    int face;
    float x, y, z;
    if (au > av && au > aw) {
        Margin(std::min(au - av, au - aw));
        face = u > 0 ? 0 : 1;
        y = u > 0 ? -v : v;
        x = -w;
        z = u;
    } else if (av > aw) {
        Margin(std::max(av - au, aw - au));  // what made the X test fail
        Margin(av - aw);
        face = v > 0 ? 2 : 3;
        x = v > 0 ? u : -u;
        y = w;
        z = v;
    } else {
        Margin(std::max(av - au, aw - au));
        Margin(aw - av);
        face = w > 0 ? 4 : 5;
        y = w > 0 ? -v : v;
        x = u;
        z = w;
    }
    const Map& m = faces[static_cast<size_t>(face)];
    const float fu = x / z * 0.5f + 0.5f, fv = y / z * 0.5f + 0.5f;
    IndexMargin(static_cast<double>(fu) * m.w);
    IndexMargin(static_cast<double>(fv) * m.h);
    const s32 s = static_cast<s32>(fu * static_cast<float>(m.w));
    const s32 t = static_cast<s32>(fv * static_cast<float>(m.h));
    return Compare(Select(m, lk, s, t), std::fabs(z), lk.shadow_word);
}

}  // namespace sw

// The float in [0.125, 0.25) whose fp32 product by 16777215 is k + 0.5: d (or z_int) is k with one ulp either way.
float DepthFor(u32 k) {
    REQUIRE(k >= 2097152u);
    REQUIRE(k < 4194303u);
    float d = static_cast<float>((static_cast<double>(k) + 0.5) / 16777215.0);
    for (int i = 0; i < 64; ++i, d = std::nextafter(d, 0.0f)) {}
    for (int i = 0; i < 128; ++i, d = std::nextafter(d, 1.0f)) {
        const float p = d * 16777215.0f;
        const float below = std::nextafter(d, 0.0f) * 16777215.0f, above = std::nextafter(d, 1.0f) * 16777215.0f;
        if (p == static_cast<float>(k) + 0.5f && static_cast<u32>(below) == k && static_cast<u32>(above) == k) {
            return d;
        }
    }
    FAIL("no depth for " << k);
    return 0.0f;
}

u32 Word(u32 depth, u32 intensity) { return (depth << 8) | intensity; }

// ---- registers ---------------------------------------------------------------------------------------------------------

constexpr u32 kRegDepthRange = 0x4D, kRegDepthNear = 0x4E, kRegDepthmap = 0x6D;
constexpr u32 kRegMain = 0x80, kRegTex0 = 0x81, kRegShadowTexture = 0x8B, kRegTex0Format = 0x8E;
constexpr u32 kRegTev0 = 0xC0;
constexpr u32 kRegOutputMerger = 0x100, kRegLogicOp = 0x102, kRegDepthColorMask = 0x107, kRegAllowColor = 0x113;
constexpr u32 kRegFramebufferShadow = 0x130;
constexpr u32 kShadowMode = 3;  // FragmentOperationMode::Shadow
constexpr u32 kSourcePrimary = 0, kSourceTexture0 = 3;

u32 StageSources(u32 source) { return source | (0xFu << 4) | (0xFu << 8) | (source << 16) | (0xFu << 20) | (0xFu << 24); }
constexpr u32 kPassSources = 0xFu | (0xFu << 4) | (0xFu << 8) | (0xFu << 16) | (0xFu << 20) | (0xFu << 24);

// Float24 (1 sign, 7 exponent, 16 mantissa bits) of a normal float.
u32 F24(float f) {
    if (f == 0.0f) return 0;
    u32 bits;
    std::memcpy(&bits, &f, 4);
    const u32 sign = bits >> 31;
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 63;
    return (sign << 23) | (static_cast<u32>(exponent) << 16) | ((bits >> 7) & 0xFFFF);
}

// Stage 0 replaces with `source`, the others pass through; colour writes on, no blending (logic op Copy), no lighting, no alpha
// test, Z-buffering with a depth scale of -1 and an offset of 0 (`depth` is gl_FragCoord.z).
std::shared_ptr<pica::PicaState> BaseState(u32 source) {
    auto state = std::make_shared<pica::PicaState>();
    state->regs.fill(0);
    state->dirty.SetAll();
    auto& r = state->regs;
    r[kRegTev0 + 0] = StageSources(source);
    for (u32 base : {0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u}) r[base + 0] = kPassSources;
    r[kRegLogicOp] = static_cast<u32>(re::LogicOp::Copy);
    r[kRegDepthColorMask] = 0xF00;
    r[kRegAllowColor] = 0xF;
    r[0x1C6] = 1;  // lighting disabled
    r[kRegDepthRange] = F24(-1.0f);
    r[kRegDepthNear] = 0;
    r[kRegDepthmap] = 1;
    return state;
}

// A shadow-mode draw: the vertex colour's green is the intensity, 0x130 holds the two halves.
std::shared_ptr<pica::PicaState> WriteState(u32 c_raw, u32 l_raw) {
    auto state = BaseState(kSourcePrimary);
    state->regs[kRegOutputMerger] = kShadowMode;
    state->regs[kRegFramebufferShadow] = c_raw | (l_raw << 16);
    return state;
}

// A lookup: unit 0 of `type` (2 Shadow2D, 4 ShadowCube) replaces the colour.
std::shared_ptr<pica::PicaState> LookupState(u32 type, const sw::Lookup& lk) {
    auto state = BaseState(kSourceTexture0);
    auto& r = state->regs;
    r[kRegMain] = lk.enabled ? 1u : 0u;
    r[kRegTex0 + 0] = lk.border;
    r[kRegTex0 + 1] = kSize | (kSize << 16);
    r[kRegTex0 + 2] = (lk.wrap_t << 8) | (lk.wrap_s << 12) | (type << 28);
    r[kRegTex0 + 4] = 0x200000u >> 3;  // an address (the rig binds the map itself)
    r[kRegShadowTexture] = lk.shadow_word;
    return state;
}

// ---- geometry ------------------------------------------------------------------------------------------------------------

using cpuvertex::OutputVertex;

// A ramp over the target: a0 + ax * x + ay * y with (x, y) in [0, 1] from the top left (image row 0).
struct Coord {
    double a0 = 0, ax = 0, ay = 0;
    double At(double x, double y) const { return a0 + ax * x + ay * y; }
};

// The rectangle [x0, x1) x [y0, y1) of a w x h target (pixels, image rows), as two triangles, at PICA z = -depth with clip w 1;
// the colour, and the texture coordinates of the ramps at the vertices.
std::vector<OutputVertex> Rect(u32 x0, u32 y0, u32 x1, u32 y1, u32 w, u32 h, float depth, std::array<float, 4> color,
                               const std::array<Coord, 3>& uvw = {}) {
    auto vertex = [&](u32 px, u32 py) {
        OutputVertex v{};
        v.pos[0] = 2.0f * static_cast<float>(px) / static_cast<float>(w) - 1.0f;
        v.pos[1] = 2.0f * static_cast<float>(py) / static_cast<float>(h) - 1.0f;
        v.pos[2] = -depth;
        v.pos[3] = 1.0f;
        v.quat[3] = 1.0f;
        for (int i = 0; i < 4; ++i) v.color[i] = color[i];
        const double x = static_cast<double>(px) / w, y = static_cast<double>(py) / h;
        v.tc0[0] = static_cast<float>(uvw[0].At(x, y));
        v.tc0[1] = static_cast<float>(uvw[1].At(x, y));
        v.tc0_w = static_cast<float>(uvw[2].At(x, y));
        return v;
    };
    const OutputVertex a = vertex(x0, y0), b = vertex(x1, y0), c = vertex(x1, y1), d = vertex(x0, y1);
    return {a, b, c, a, c, d};
}

std::array<float, 4> Green(u32 s) { return {0.0f, static_cast<float>(s) / 255.0f, 0.0f, 1.0f}; }

native::VertexInputKey InputKey() {
    const rhi::VertexLayout layout = cpuvertex::MakeBatchVertexLayout();
    native::VertexInputKey key;
    key.binding_count = static_cast<u8>(layout.bindings.size());
    for (size_t i = 0; i < layout.bindings.size(); ++i) key.bindings[i].stride = layout.bindings[i].stride;
    key.attribute_count = static_cast<u8>(layout.attributes.size());
    for (size_t i = 0; i < layout.attributes.size(); ++i) {
        key.attributes[i] = {static_cast<u8>(layout.attributes[i].location), static_cast<u8>(layout.attributes[i].binding),
                             layout.attributes[i].format, layout.attributes[i].offset};
    }
    return key;
}

// ---- the rig: draws through the composition with the real generator ------------------------------------------------------

// One step of a recording: a draw (a shadow write into `shadow_target`, or a lookup into the colour target with `unit0`), or,
// with no state, the end of the composer's pass.
struct Step {
    std::shared_ptr<pica::PicaState> state;
    std::vector<OutputVertex> vertices;
    rhi::Image* shadow_target = nullptr;
    native::TextureBinding unit0;
};

Step EndPass() { return Step{}; }

std::vector<u8> Bytes(const std::vector<u32>& words) {
    std::vector<u8> bytes(words.size() * 4);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

struct Rig {
    std::shared_ptr<rhi::Backend> backend;
    DeviceProfile profile;
    std::unique_ptr<native::PipelineBuilder> builder;
    std::unique_ptr<native::DefaultPipelineProvider> provider;
    std::unique_ptr<native::DrawComposer> composer;
    std::unique_ptr<FragmentUniformRing> uniforms;
    std::unique_ptr<TableRings> tables;
    std::unique_ptr<rhi::Ring> vertex_ring, vs_ring, staging;
    std::shared_ptr<rhi::Image> color;
    std::shared_ptr<rhi::Image> null_shadow;  // a 1x1 shadow source holding word 0 (what NativeGpu binds to a unit that is off)
    bool uber;

    Rig(std::shared_ptr<rhi::Backend> b, bool force_uber) : backend(std::move(b)), uber(force_uber) {
        profile = ProfileOf(backend->Caps());
        builder = std::make_unique<native::PipelineBuilder>(backend, FeatureRegistry::Global());
        provider = std::make_unique<native::DefaultPipelineProvider>(
            [this](const native::PipelineKey& key) { return builder->Build(key); }, native::DefaultProviderOptions{force_uber});
        auto made = native::DrawComposer::Create(backend, *provider);
        REQUIRE_MESSAGE(made.ok(), made.error());
        composer = std::move(*made);
        auto ring = FragmentUniformRing::Create(*backend);
        REQUIRE_MESSAGE(ring.ok(), ring.error());
        uniforms = std::move(*ring);
        auto table_rings = TableRings::Create(*backend);
        REQUIRE_MESSAGE(table_rings.ok(), table_rings.error());
        tables = std::move(*table_rings);
        auto vr = backend->CreateRing(1 << 20, rhi::BufferUsage::Vertex, "shadow vertices");
        auto sr = backend->CreateRing(1 << 20, rhi::BufferUsage::Uniform, "shadow vs uniforms");
        auto tr = backend->CreateRing(1 << 20, rhi::BufferUsage::Transfer, "shadow staging");
        REQUIRE(vr.ok());
        REQUIRE(sr.ok());
        REQUIRE(tr.ok());
        vertex_ring = std::move(*vr);
        vs_ring = std::move(*sr);
        staging = std::move(*tr);
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = desc.height = kSize;
        desc.name = "shadow test colour";
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        color = *image;
        null_shadow = Upload(sw::Map{1, 1, {0u}});
    }

    std::string Name() const {
        return std::string(uber ? "ubershader" : "specialised") + (profile.interlock ? ", interlock" : ", compare-and-swap") +
               (profile.custom_border_color ? ", custom border" : ", emulated border");
    }

    // A shadow source holding the map (the decoder's order; uploaded with the rows reversed, as NativeGpu uploads a texture).
    std::shared_ptr<rhi::Image> Upload(const sw::Map& m) {
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = m.w;
        desc.height = m.h;
        desc.shadow_source = true;
        desc.name = "shadow map";
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        rhi::Recorder rec = backend->BeginRecording();
        auto r = sampling::UploadRgba8(rec, *staging, **image, 0, 0, m.w, m.h, Bytes(m.words));
        REQUIRE_MESSAGE(r.ok(), r.error());
        backend->WaitIdle(backend->Submit(std::move(rec)));
        return *image;
    }

    // The plain RGBA8 cube of six faces (what NativeGpu binds to a ShadowCube unit).
    std::shared_ptr<rhi::Image> UploadCube(const std::array<sw::Map, 6>& faces) {
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = faces[0].w;
        desc.height = faces[0].h;
        desc.cube = true;
        desc.layers = 6;
        desc.name = "shadow cube";
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        rhi::Recorder rec = backend->BeginRecording();
        for (u32 layer = 0; layer < 6; ++layer) {
            auto r = sampling::UploadRgba8(rec, *staging, **image, 0, layer, faces[layer].w, faces[layer].h, Bytes(faces[layer].words));
            REQUIRE_MESSAGE(r.ok(), r.error());
        }
        backend->WaitIdle(backend->Submit(std::move(rec)));
        return *image;
    }

    // The map's words as the write-back gives them (ReadbackToGuest in PICA RGBA8, decoded: the decoder's order).
    std::vector<u32> ReadMap(const rhi::Image& image, u32 w, u32 h) {
        std::vector<u8> guest(static_cast<size_t>(w) * h * 4);
        auto r = backend->ReadbackToGuest(image, rhi::Rect{0, 0, w, h}, rhi::PicaFormat::Rgba8, guest);
        REQUIRE_MESSAGE(r.ok(), r.error());
        std::vector<u8> rgba;
        REQUIRE(texture::DecodeToRgba8(guest.data(), guest.size(), w, h, texture::Format::RGBA8, rgba));
        std::vector<u32> words(static_cast<size_t>(w) * h);
        std::memcpy(words.data(), rgba.data(), rgba.size());
        return words;
    }

    // Records the steps in one recording (one FragmentState, so a draw sees what the previous one left in the block), ends the
    // composer's pass, submits and waits. Returns the colour target's pixels (cleared to (1, 2, 3, 4) at the first lookup).
    std::vector<u8> Run(const std::vector<Step>& steps) {
        rhi::Recorder rec = backend->BeginRecording();
        composer->BeginRecording(rec);
        uniforms->BeginRecording();
        FragmentState fragment(profile, FeatureRegistry::Global());
        bool dynamic = false, cleared = false;
        for (const Step& step : steps) {
            if (step.state == nullptr) {
                composer->EndPass(rec);
                continue;
            }
            auto prepared = fragment.Prepare(*step.state, {}, tables.get());
            REQUIRE_MESSAGE(prepared.ok(), prepared.error());
            auto alloc = uniforms->Write(fragment.block(), prepared->block_changed);
            REQUIRE_MESSAGE(alloc.ok(), alloc.error());
            cpuvertex::TriangleBatch batch;
            for (size_t i = 0; i + 2 < step.vertices.size(); i += 3) {
                batch.AddTriangle(step.vertices[i], step.vertices[i + 1], step.vertices[i + 2]);
            }
            auto streamed = cpuvertex::UploadBatch(*vertex_ring, batch);
            REQUIRE_MESSAGE(streamed.ok(), streamed.error());
            auto vs = vs_ring->Map(2048, std::max<u64>(256, backend->Caps().uniform_buffer_offset_alignment));
            REQUIRE_MESSAGE(vs.ok(), vs.error());
            std::memset(vs->data.data(), 0, 2048);
            REQUIRE(vs_ring->Commit(2048).ok());
            if (!dynamic) {
                fbops::ApplyDynamicState(rec, fbops::MakeDynamicState(*step.state), false);
                dynamic = true;
            }

            const FragmentKey key = fragment.key();
            native::DrawDesc d;
            d.vertex.kind = native::VertexStageKind::Trivial;
            d.vertex.input = InputKey();
            d.fragment = &key;
            d.needs = fragment.needs();
            d.state = fbops::MakeFixedFunctionState(*step.state);
            d.vs_pica_uniforms = native::UniformRange{vs_ring->GetBuffer().get(), vs->offset, 1616};
            d.vs_uniforms = native::UniformRange{vs_ring->GetBuffer().get(), vs->offset + 1792, 32};
            d.fragment_uniforms = *alloc;
            d.tables = tables.get();
            const rhi::VertexBufferBinding binding{vertex_ring->GetBuffer().get(), streamed->offset};
            d.vertex_buffers = std::span<const rhi::VertexBufferBinding>(&binding, 1);
            d.count = streamed->vertex_count;
            u32 w = kSize, h = kSize;
            if (step.shadow_target != nullptr) {
                REQUIRE(d.needs.shadow_write_pass);
                d.shadow_target = step.shadow_target;
                w = step.shadow_target->Width();
                h = step.shadow_target->Height();
            } else {
                REQUIRE_FALSE(d.needs.shadow_write_pass);
                rhi::TargetPassDesc pass;
                pass.color = color.get();
                pass.area = rhi::Rect{0, 0, kSize, kSize};
                if (!cleared) {
                    rhi::TargetPassDesc clear = pass;
                    clear.color_load = rhi::LoadOp::Clear;
                    clear.clear_color = {1 / 255.0f, 2 / 255.0f, 3 / 255.0f, 4 / 255.0f};
                    composer->BeginTargetPass(rec, clear);
                    cleared = true;
                }
                d.target = pass;
                d.color_format = rhi::Format::Rgba8Unorm;
                d.units[0] = step.unit0;
            }
            d.viewport = rhi::Viewport{0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
            d.scissor = rhi::Rect{0, 0, w, h};
            auto result = composer->Draw(rec, d);
            REQUIRE_MESSAGE(result.ok(), result.error());
        }
        composer->EndPass(rec);
        backend->WaitIdle(backend->Submit(std::move(rec)));
        if (!cleared) return {};
        auto read = backend->ReadImage(*color, 0, 0, kSize, kSize);
        REQUIRE_MESSAGE(read.ok(), read.error());
        return *read;
    }
};

// The device as it is, and a second backend in the same process with interlock and the custom border colour masked off: the
// compare-and-swap loop and the shader's border emulation (as MH4N_RHI_MASK=interlock,custom_border_color does for a whole run).
struct Rigs {
    std::shared_ptr<rhi::Backend> full, lean;
    std::vector<std::unique_ptr<Rig>> all;
    Rigs() {
        full = test::RequireBackend();
        if (!full) return;
        rhi::BackendOptions masked;
        masked.mask = rhi::CapabilityMask{}.With(rhi::Cap::Interlock).With(rhi::Cap::CustomBorderColor);
        lean = test::RequireBackend(masked);
        if (!lean) return;
        for (const auto& backend : {full, lean}) {
            all.push_back(std::make_unique<Rig>(backend, false));
            all.push_back(std::make_unique<Rig>(backend, true));
        }
    }
    explicit operator bool() const { return !all.empty(); }
};

// ---- the write ---------------------------------------------------------------------------------------------------------------

struct WriteDraw {
    u32 x0, y0, x1, y1;  // image pixels
    u32 d;               // the depth as an integer (DepthFor), or 0
    u32 s;               // the green byte
};

struct WriteCase {
    std::string name;
    u32 c_raw = 0x3C00, l_raw = 0x0000;
    sw::Map map;
    std::vector<std::vector<WriteDraw>> passes;  // one kind of s per texel within a pass
};

// The map of the write cases: 8x8, depth `depth(col, row)`, intensity 40 + 3 (col + 8 row) (from 40 to 229, all different).
template <typename Depth>
sw::Map WriteMap(Depth&& depth) {
    sw::Map m{kSize, kSize, std::vector<u32>(kSize * kSize)};
    for (u32 row = 0; row < kSize; ++row) {
        for (u32 col = 0; col < kSize; ++col) m.words[row * kSize + col] = Word(depth(col, row), 40 + 3 * (col + 8 * row));
    }
    return m;
}

float DepthOf(u32 d) { return d == 0 ? 0.0f : DepthFor(d); }

std::vector<u32> ExpectWrite(const WriteCase& wc) {
    std::vector<u32> words = wc.map.words;
    for (const auto& pass : wc.passes) {
        for (const WriteDraw& draw : pass) {
            for (u32 y = draw.y0; y < draw.y1; ++y) {
                for (u32 x = draw.x0; x < draw.x1; ++x) {
                    u32& word = words[(wc.map.h - 1 - y) * wc.map.w + x];  // image row y is decoded row h - 1 - y
                    word = sw::Write(word, draw.d, draw.s, wc.c_raw, wc.l_raw);
                }
            }
        }
    }
    return words;
}

void CheckWrite(Rigs& rigs, const WriteCase& wc) {
    INFO(wc.name);
    sw::g_margin = 1e9;
    const std::vector<u32> want = ExpectWrite(wc);
    CHECK_MESSAGE(sw::g_margin >= 0.01, wc.name << ": a value of the case is within " << sw::g_margin << " of a decision boundary");
    for (auto& rig : rigs.all) {
        std::shared_ptr<rhi::Image> map = rig->Upload(wc.map);
        std::vector<Step> steps;
        for (const auto& pass : wc.passes) {
            for (const WriteDraw& draw : pass) {
                Step step;
                step.state = WriteState(wc.c_raw, wc.l_raw);
                step.vertices = Rect(draw.x0, draw.y0, draw.x1, draw.y1, wc.map.w, wc.map.h, DepthOf(draw.d), Green(draw.s));
                step.shadow_target = map.get();
                steps.push_back(step);
            }
            steps.push_back(EndPass());
        }
        rig->Run(steps);
        const std::vector<u32> got = rig->ReadMap(*map, wc.map.w, wc.map.h);
        u32 wrong = 0;
        std::string first;
        for (size_t i = 0; i < want.size(); ++i) {
            if (got[i] != want[i] && wrong++ == 0) {
                first = "decoded row " + std::to_string(i / wc.map.w) + " column " + std::to_string(i % wc.map.w) + ": 0x" +
                        fmt::format("{:08x}", got[i]) + ", the port says 0x" + fmt::format("{:08x}", want[i]);
            }
        }
        CHECK_MESSAGE(wrong == 0, wc.name << " [" << rig->Name() << "]: " << wrong << " texels differ, the first at " << first);
    }
}

// ---- the lookups ---------------------------------------------------------------------------------------------------------------

struct LookupScene {
    std::string name;
    u32 type = 2;  // Shadow2D; 4 ShadowCube
    sw::Lookup lk;
    sw::Map map;                 // Shadow2D
    std::array<sw::Map, 6> faces;  // ShadowCube
    std::array<Coord, 3> uvw;    // texcoord0 (u, v) and texcoord0_w
    bool unit_off_binding = false;  // bind the 1x1 word-0 image (NativeGpu's binding of a unit that is off)
};

std::array<int, 4> Pixel(const std::vector<u8>& pixels, u32 x, u32 y) {
    const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
    return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
}

void CheckLookup(Rigs& rigs, const LookupScene& sc) {
    INFO(sc.name);
    sw::g_margin = 1e9;
    std::vector<int> want(kSize * kSize);
    for (u32 py = 0; py < kSize; ++py) {
        for (u32 px = 0; px < kSize; ++px) {
            const double x = (px + 0.5) / kSize, y = (py + 0.5) / kSize;
            const double u = sc.uvw[0].At(x, y), v = sc.uvw[1].At(x, y), w = sc.uvw[2].At(x, y);
            want[py * kSize + px] = sc.type == 2 ? sw::Shadow2D(sc.map, sc.lk, u, v, w) : sw::ShadowCube(sc.faces, sc.lk, u, v, w);
        }
    }
    CHECK_MESSAGE(sw::g_margin >= 0.01, sc.name << ": a pixel of the scene is within " << sw::g_margin << " of a decision boundary");
    if (sc.type == 4) {
        // every cube scene mixes lit and unlit texels: one that is all lit or all unlit would not tell the faces apart
        const size_t lit = static_cast<size_t>(std::count_if(want.begin(), want.end(), [](int i) { return i != 0; }));
        CHECK_MESSAGE((0 < lit && lit < want.size()), sc.name << ": the scene is all lit or all unlit");
    }
    for (auto& rig : rigs.all) {
        std::shared_ptr<rhi::Image> image;
        Step step;
        step.state = LookupState(sc.type, sc.lk);
        step.vertices = Rect(0, 0, kSize, kSize, kSize, kSize, 0.5f, {1, 1, 1, 1}, sc.uvw);
        if (sc.unit_off_binding) {
            step.unit0 = native::TextureBinding{rig->null_shadow.get(), rhi::ImageView::ShadowStorage, nullptr};
        } else if (sc.type == 2) {
            image = rig->Upload(sc.map);
            step.unit0 = native::TextureBinding{image.get(), rhi::ImageView::ShadowStorage, nullptr};
        } else {
            image = rig->UploadCube(sc.faces);
            step.unit0 = native::TextureBinding{image.get(), rhi::ImageView::Sampled, nullptr};
        }
        const std::vector<u8> pixels = rig->Run({step});
        u32 wrong = 0;
        std::string first;
        for (u32 py = 0; py < kSize; ++py) {
            for (u32 px = 0; px < kSize; ++px) {
                const int i = want[py * kSize + px];
                if (Pixel(pixels, px, py) != std::array<int, 4>{i, i, i, i} && wrong++ == 0) {
                    const auto p = Pixel(pixels, px, py);
                    first = "pixel (" + std::to_string(px) + "," + std::to_string(py) + ") is " + std::to_string(p[0]) + "," +
                            std::to_string(p[1]) + "," + std::to_string(p[2]) + "," + std::to_string(p[3]) + ", the port says " +
                            std::to_string(i);
                }
            }
        }
        CHECK_MESSAGE(wrong == 0, sc.name << " [" << rig->Name() << "]: " << wrong << " pixels differ, the first: " << first);
    }
}

// texel centres: pixel (px, py) reads texel (px, py) of an 8x8 map
const Coord kCentreU{0, 1, 0}, kCentreV{0, 0, 1};

// An 8x8 map whose texel at software (s, t) (decoded row 7 - t) has the depth `depth(s, t)` and the intensity 20 + 3 (s + 8 t).
template <typename Depth>
sw::Map LookupMap(u32 w, u32 h, Depth&& depth) {
    sw::Map m{w, h, std::vector<u32>(static_cast<size_t>(w) * h)};
    for (u32 t = 0; t < h; ++t) {
        for (u32 s = 0; s < w; ++s) {
            m.words[(h - 1 - t) * w + s] = Word(depth(s, t), (20 + 3 * (s + w * t)) & 0xFF);
        }
    }
    return m;
}

// ---- SPIR-V --------------------------------------------------------------------------------------------------------------------

constexpr u32 kCapFragmentShaderPixelInterlock = 5378;  // FragmentShaderPixelInterlockEXT
constexpr u32 kModePixelInterlockOrdered = 5366;        // PixelInterlockOrderedEXT

bool HasCapability(const std::vector<u32>& spirv, u32 capability) {
    for (size_t i = 5; i < spirv.size();) {
        const u32 count = spirv[i] >> 16, opcode = spirv[i] & 0xFFFF;
        if (count == 0) break;
        if (opcode == 17 && i + 1 < spirv.size() && spirv[i + 1] == capability) return true;
        i += count;
    }
    return false;
}

bool HasExecutionMode(const std::vector<u32>& spirv, u32 mode) {
    for (size_t i = 5; i < spirv.size();) {
        const u32 count = spirv[i] >> 16, opcode = spirv[i] & 0xFFFF;
        if (count == 0) break;
        if (opcode == 16 && i + 2 < spirv.size() && spirv[i + 2] == mode) return true;
        i += count;
    }
    return false;
}

constexpr u32 kOpAtomicCompareExchange = 230;

bool HasInstruction(const std::vector<u32>& spirv, u32 op) {
    for (size_t i = 5; i < spirv.size();) {
        const u32 count = spirv[i] >> 16, opcode = spirv[i] & 0xFFFF;
        if (count == 0) break;
        if (opcode == op) return true;
        i += count;
    }
    return false;
}

FragmentKey ShadowKey() {
    auto state = WriteState(0x3C00, 0);
    return MakeKey(*state);
}

}  // namespace

// ---- without a device ------------------------------------------------------------------------------------------------------------

TEST_CASE("shadow/the_uniforms_are_rasterizer_accelerateds_and_nothing_is_reported_as_a_stub") {
    auto state = BaseState(kSourcePrimary);
    auto bits = [](float f) {
        u32 b;
        std::memcpy(&b, &f, 4);
        return b;
    };
    struct Case {
        u32 framebuffer_shadow, shadow_texture;
    };
    // a normal pair; a denormal constant and a negative denormal linear; infinities; a NaN; 0x8B with bit 0 set and clear
    for (const Case& c : {Case{0x3800'3C00u, 0x00ABCDEFu}, Case{0x8001'03FFu, 0x00ABCDEEu}, Case{0xFC00'7C00u, 0x00FFFFFFu},
                          Case{0x7E01'0001u, 0x00000001u}, Case{0x0000'8000u, 0xFF000002u}}) {
        state->regs[kRegFramebufferShadow] = c.framebuffer_shadow;
        state->regs[kRegShadowTexture] = c.shadow_texture;
        state->dirty.SetAll();
        FragmentBlock block;
        std::memset(&block, 0, sizeof(block));
        UniformDirty dirty;
        dirty.all = true;
        FillUniforms(*state, block, dirty, {});
        CHECK(dirty.changed);
        // Pica::f16::FromRaw(...).ToFloat32() bit for bit (a denormal kept exactly, infinities and NaNs kept)
        CHECK(bits(block.uniforms.shadow_bias_constant) == bits(sw::HalfToFloat(c.framebuffer_shadow & 0xFFFF)));
        CHECK(bits(block.uniforms.shadow_bias_linear) == bits(sw::HalfToFloat(c.framebuffer_shadow >> 16)));
        // regs.texturing.shadow.bias << 1: bits 1-23 (bit 0 is the orthographic bit, the top byte is not the bias)
        CHECK(block.uniforms.shadow_texture_bias == static_cast<int>(c.shadow_texture & 0x00FFFFFEu));
    }
    // the converter's values themselves (by hand)
    CHECK(sw::HalfToFloat(0x3C00) == 1.0f);
    CHECK(sw::HalfToFloat(0x03FF) == 1023.0f / 16777216.0f);
    CHECK(sw::HalfToFloat(0x8001) == -1.0f / 16777216.0f);
    CHECK(std::isinf(sw::HalfToFloat(0x7C00)));
    CHECK(sw::HalfToFloat(0xFC00) < 0.0f);
    CHECK(std::isnan(sw::HalfToFloat(0x7E01)));
    CHECK(std::signbit(sw::HalfToFloat(0x8000)));

    // the dirty bits: 0x130 or 0x8B dirty writes, nothing dirty writes nothing, the same value marks nothing
    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    UniformDirty first;
    first.all = true;
    FillUniforms(*state, block, first, {});
    state->ClearDirty();
    UniformDirty clean;
    FillUniforms(*state, block, clean, {});
    CHECK_FALSE(clean.changed);
    state->regs[kRegFramebufferShadow] = 0x4000'3C00u;
    state->dirty.Set(kRegFramebufferShadow);
    UniformDirty changed;
    FillUniforms(*state, block, changed, {});
    CHECK(changed.changed);
    CHECK(block.uniforms.shadow_bias_linear == 2.0f);
    state->ClearDirty();
    state->regs[kRegShadowTexture] = 0x00000101u;
    state->dirty.Set(kRegShadowTexture);
    UniformDirty bias;
    FillUniforms(*state, block, bias, {});
    CHECK(bias.changed);
    CHECK(block.uniforms.shadow_texture_bias == 0x100);
    state->ClearDirty();
    state->dirty.Set(kRegShadowTexture);
    UniformDirty same;
    FillUniforms(*state, block, same, {});
    CHECK_FALSE(same.changed);

    // no stub: a shadow-mode key, a Shadow2D or ShadowCube unit 0 a TEV stage reads, and lighting's shadow selector on unit 0
    std::array<const char*, 8> names{};
    auto reported = [&](const FragmentKey& key) {
        const size_t n = StubbedFeatures(key, FeatureRegistry::Global(), names);
        for (size_t i = 0; i < n; ++i) {
            if (std::string(names[i]) == "shadow") return true;
        }
        return false;
    };
    CHECK_FALSE(reported(ShadowKey()));
    for (u32 type : {2u, 4u}) {
        auto lookup = LookupState(type, sw::Lookup{});
        CHECK_FALSE(reported(MakeKey(*lookup)));
        FragmentKey lit = MakeKey(*BaseState(kSourcePrimary));
        lit.texture.set_texture0_type(static_cast<re::TextureType>(type));
        lit.lighting.set_enable(1);
        lit.lighting.set_enable_shadow(1);
        lit.lighting.set_shadow_selector(0);
        CHECK_FALSE(reported(lit));
    }
    const FeatureModule* shadow = FeatureRegistry::Global().Find("shadow");
    REQUIRE(shadow != nullptr);
    CHECK(shadow->Stubbed(ShadowKey()) == nullptr);
    // core asks for the pass; shadow/ adds nothing
    CHECK(NeedsOf(ShadowKey(), DeviceProfile{}).shadow_write_pass);
    CHECK_FALSE(shadow->NeedsOf(ShadowKey(), DeviceProfile{}).shadow_write_pass);
}

TEST_CASE("shadow/the_shaders_compile_in_both_modes_and_passes_and_the_tail_is_in_the_shadow_write_pass_only") {
    const FragmentKey write_key = ShadowKey();
    const FragmentKey lookup_2d = MakeKey(*LookupState(2, sw::Lookup{}));
    const FragmentKey lookup_cube = MakeKey(*LookupState(4, sw::Lookup{1, 5, 0, 0, true}));
    for (const bool interlock : {true, false}) {
        DeviceProfile profile;
        profile.interlock = interlock;
        profile.logic_op = true;
        for (const bool custom_border : {true, false}) {
            profile.custom_border_color = custom_border;
            for (const FragmentKey* key : {&write_key, &lookup_2d, &lookup_cube}) {
                for (ShaderPass pass : {ShaderPass::Normal, ShaderPass::ShadowWrite}) {
                    const Generated g = GenerateSpecialised(*key, profile, pass);
                    CHECK_MESSAGE(g.ok(), g.error);
                }
            }
            const Generated uber = GenerateUber(ShaderPass::Normal, profile);
            CHECK_MESSAGE(uber.ok(), uber.error);
            const Generated uber_write = GenerateUber(ShaderPass::ShadowWrite, profile);
            CHECK_MESSAGE(uber_write.ok(), uber_write.error);
            // the write's ordering is the profile's: the interlock section, or the compare-and-swap loop guarded against helpers
            for (const Generated* g : {&uber_write}) {
                CHECK(HasCapability(g->spirv, kCapFragmentShaderPixelInterlock) == interlock);
                CHECK(HasExecutionMode(g->spirv, kModePixelInterlockOrdered) == interlock);
                CHECK((g->glsl.find("beginInvocationInterlock();") != std::string::npos) == interlock);
                CHECK((g->glsl.find("imageAtomicCompSwap") != std::string::npos) == !interlock);
                CHECK((g->glsl.find("if (!gl_HelperInvocation)") != std::string::npos) == !interlock);
            }
            const Generated spec_write = GenerateSpecialised(write_key, profile, ShaderPass::ShadowWrite);
            CHECK(HasCapability(spec_write.spirv, kCapFragmentShaderPixelInterlock) == interlock);
        }
    }
    DeviceProfile profile;
    profile.custom_border_color = true;
    // the normal pass holds no storage image, no UpdateShadow and no tail; the shadow-write pass declares no colour output
    for (const std::string& glsl : {GenerateUberGlsl(ShaderPass::Normal, profile), GenerateSpecialisedGlsl(lookup_2d, profile, ShaderPass::Normal)}) {
        CHECK(glsl.find("shadow_buffer") == std::string::npos);
        CHECK(glsl.find("UpdateShadow") == std::string::npos);
        CHECK(glsl.find("mh4n_shadow_s") == std::string::npos);
        CHECK(glsl.find("layout (location = 0) out vec4 color;") != std::string::npos);
        CHECK(glsl.find("vec4 shadowTexture(vec2 uv, float w) {") != std::string::npos);
        CHECK(glsl.find("vec4 shadowTextureCube(vec2 uv, float w) {") != std::string::npos);
    }
    for (const std::string& glsl : {GenerateUberGlsl(ShaderPass::ShadowWrite, profile), GenerateSpecialisedGlsl(write_key, profile, ShaderPass::ShadowWrite)}) {
        CHECK(glsl.find("layout(set = 2, binding = 0, r32ui) uniform coherent uimage2D shadow_buffer;") != std::string::npos);
        CHECK(glsl.find("out vec4 color") == std::string::npos);
        CHECK(glsl.find("uint UpdateShadow(uint pixel, uint d, uint s)") != std::string::npos);
        // s is taken at the AlphaTest point, before fog/'s FogGas point
        const size_t s_at = glsl.find("uint mh4n_shadow_s = uint(round(combiner_output.g * 255.0));");
        REQUIRE(s_at != std::string::npos);
        CHECK(glsl.find("uint mh4n_shadow_d = uint(clamp(depth, 0.0, 1.0) * 16777215.0);") > s_at);
    }
    // the specialised shader of an orthographic lookup divides nothing; a perspective one divides; the ubershader reads the bit
    sw::Lookup perspective;
    perspective.shadow_word = 0;
    CHECK(GenerateSpecialisedGlsl(lookup_2d, profile, ShaderPass::Normal).find("uv /= w;") == std::string::npos);
    CHECK(GenerateSpecialisedGlsl(MakeKey(*LookupState(2, perspective)), profile, ShaderPass::Normal).find("uv /= w;") != std::string::npos);
    // shadow/'s helpers in a specialised Texture2D shader return no border colour uncompared (gpu_sampling_test's border case)
    FragmentKey plain = MakeKey(*BaseState(kSourceTexture0));
    CHECK(GenerateSpecialisedGlsl(plain, profile, ShaderPass::Normal).find("return tex_border_color[0];") == std::string::npos);
    // sampling/ no longer tests a border for Shadow2D: with the capability off its emulation is in the Texture2D shader only
    profile.custom_border_color = false;
    sw::Lookup border;
    border.wrap_s = 1;
    FragmentKey shadow_border = MakeKey(*LookupState(2, border));
    CHECK(GenerateSpecialisedGlsl(shadow_border, profile, ShaderPass::Normal).find("return tex_border_color[0];") == std::string::npos);
    FragmentKey texture_border = shadow_border;
    texture_border.texture.set_texture0_type(re::TextureType::Texture2D);
    CHECK(GenerateSpecialisedGlsl(texture_border, profile, ShaderPass::Normal).find("return tex_border_color[0];") != std::string::npos);
}

// ---- the write, byte for byte -------------------------------------------------------------------------------------------------------

TEST_CASE("shadow/the_write_is_DrawShadowMapPixel_byte_for_byte_on_both_write_paths_and_in_both_modes") {
    Rigs rigs;
    if (!rigs) return;
    const u32 d1 = 3000000, d2 = 2500000, d3 = 3500000, d_low = 2200000;
    auto high = [](u32, u32) { return 0xF00000u; };
    std::vector<WriteCase> cases;
    {
        // d >= ref_z: nothing changes, whatever s (the texels' depths are below d or equal to it)
        WriteCase wc;
        wc.name = "d >= ref_z leaves the texel";
        wc.map = WriteMap([&](u32 col, u32) { return col < 4 ? d1 - 1000 : d1; });
        wc.passes = {{{0, 0, 8, 4, d1, 0}}, {{0, 4, 8, 8, d1, 77}}};
        cases.push_back(wc);
    }
    {
        WriteCase wc;
        wc.name = "s = 0 writes only the depth";
        wc.map = WriteMap(high);
        wc.passes = {{{1, 2, 7, 6, d1, 0}}};
        cases.push_back(wc);
    }
    {
        // c = 1.0, l = 1.0: s' = s / (1 + d / ref_z) with ref_z = 2 d: s / 1.5 (s = 100: 66.67; the texels' intensities from 40 up,
        // so some keep theirs)
        WriteCase wc;
        wc.name = "s > 0 writes only min(ref_s, s'), a normal pair";
        wc.c_raw = 0x3C00;
        wc.l_raw = 0x3C00;
        wc.map = WriteMap([&](u32, u32) { return 2 * d1; });
        wc.passes = {{{0, 0, 8, 8, d1, 100}}};
        cases.push_back(wc);
    }
    {
        // a denormal c (0x03FF, 1023 * 2^-24) with l = 1.0 at x = 0.5: s' = 100 / 0.500061 = 199.976 (a flushed c gives 200)
        WriteCase wc;
        wc.name = "a denormal c";
        wc.c_raw = 0x03FF;
        wc.l_raw = 0x3C00;
        wc.map = WriteMap([&](u32, u32) { return 2 * d1; });
        for (u32 i = 0; i < 64; ++i) wc.map.words[i] = (wc.map.words[i] & ~0xFFu) | 250u;  // intensities above s'
        wc.passes = {{{0, 0, 8, 8, d1, 100}}};
        cases.push_back(wc);
    }
    {
        // c = 0 at d = 0: the denominator is +0, s' = 255, the intensity is kept (the depth too: s > 0)
        WriteCase wc;
        wc.name = "c = 0 at d = 0 (a denominator of +0: kept)";
        wc.c_raw = 0x0000;
        wc.l_raw = 0x3C00;
        wc.map = WriteMap(high);
        wc.passes = {{{0, 0, 8, 8, 0, 200}}};
        cases.push_back(wc);
    }
    {
        // c = -1, l = 0.5, x = 0.5: the denominator is -0.75, s' = 0
        WriteCase wc;
        wc.name = "a negative c (a denominator below 0: 0)";
        wc.c_raw = 0xBC00;
        wc.l_raw = 0x3800;
        wc.map = WriteMap([&](u32, u32) { return 2 * d1; });
        wc.passes = {{{2, 2, 6, 6, d1, 150}}};
        cases.push_back(wc);
    }
    {
        WriteCase wc;
        wc.name = "an infinite c (0)";
        wc.c_raw = 0x7C00;
        wc.l_raw = 0x3C00;
        wc.map = WriteMap(high);
        wc.passes = {{{0, 0, 8, 8, d1, 150}}};
        cases.push_back(wc);
    }
    {
        // an infinite l at d = 0: infinity times zero is 0 (Pica), s' = s / c = 151 / 2 = 75.5
        WriteCase wc;
        wc.name = "an infinite l at d = 0";
        wc.c_raw = 0x4000;
        wc.l_raw = 0x7C00;
        wc.map = WriteMap(high);
        wc.passes = {{{0, 0, 8, 8, 0, 151}}};
        cases.push_back(wc);
    }
    {
        WriteCase wc;
        wc.name = "an infinite l at d > 0 (0)";
        wc.c_raw = 0x4000;
        wc.l_raw = 0xFC00;
        wc.map = WriteMap(high);
        wc.passes = {{{0, 0, 8, 8, d1, 151}}};
        cases.push_back(wc);
    }
    {
        // three overlapping draws with s = 0: each texel takes the smallest depth below its own
        WriteCase wc;
        wc.name = "overlapping draws, all s = 0";
        wc.map = WriteMap([&](u32 col, u32 row) { return (col + row) % 3 == 0 ? d2 + 1000 : 0xF00000u; });
        wc.passes = {{{0, 0, 6, 5, d1, 0}, {2, 3, 8, 8, d2, 0}, {1, 1, 5, 7, d3, 0}}};
        cases.push_back(wc);
    }
    {
        // three overlapping draws with s > 0 (c = 2, l = 1, ref_z = 4 194 304 + ...): each texel takes the smallest intensity
        WriteCase wc;
        wc.name = "overlapping draws, all s > 0";
        wc.c_raw = 0x4000;
        wc.l_raw = 0x3C00;
        wc.map = WriteMap([&](u32, u32) { return 2 * d3; });
        wc.passes = {{{0, 0, 6, 5, d1, 201}, {2, 3, 8, 8, d2, 211}, {1, 1, 5, 7, d3, 181}}};
        cases.push_back(wc);
    }
    {
        // a texel that sees both kinds: in separate passes (EndPass between them), so barrier (1) orders them
        WriteCase wc;
        wc.name = "both kinds on a texel, in separate passes";
        wc.c_raw = 0x3C00;
        wc.l_raw = 0x3C00;
        wc.map = WriteMap(high);
        wc.passes = {{{0, 0, 8, 6, d3, 0}}, {{0, 2, 8, 8, d_low, 223}}, {{3, 0, 8, 8, d2, 0}}};
        cases.push_back(wc);
    }
    for (const WriteCase& wc : cases) CheckWrite(rigs, wc);
}

// ---- the lookups ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("shadow/Shadow2D_is_TextureColors_one_texel_compared_with_the_wrap_modes_and_the_border_as_a_texel") {
    Rigs rigs;
    if (!rigs) return;
    // the second backend really takes the shader's border emulation (sampling/ is out of Shadow2D's way on both paths)
    CHECK_FALSE(rigs.lean->Caps().custom_border_color);
    const u32 k = 3000000;
    const float z = DepthFor(k);  // z_int = k with the bias 0
    // texel (s, t): depth k - 1, k or k + 1 by (s + 2 t) mod 3: unlit, lit (equal), lit
    auto around = [&](u32 s, u32 t) { return k - 1 + (s + 2 * t) % 3; };
    const sw::Map map = LookupMap(kSize, kSize, around);
    std::vector<LookupScene> scenes;
    {
        LookupScene sc;
        sc.name = "texel centres: equal (lit), above (lit), below (unlit); orthographic";
        sc.map = map;
        sc.uvw = {kCentreU, kCentreV, Coord{z}};
        scenes.push_back(sc);
        sc.name = "a negative w: z = |w|";
        sc.uvw[2] = Coord{-z};
        scenes.push_back(sc);
    }
    {
        // 0x8B bit 0 clear: (u, v) / w; the coordinates are the centres times w
        LookupScene sc;
        sc.name = "the perspective divide (0x8B bit 0 clear)";
        sc.map = map;
        sc.lk.shadow_word = 0;
        sc.uvw = {Coord{0, z, 0}, Coord{0, 0, z}, Coord{z}};
        scenes.push_back(sc);
        // the same with 0x8B = 4 (bias field 2): z_int = k - 4, everything lit
        sc.name = "the perspective divide with 0x8B = 4 (bias field 2)";
        sc.lk.shadow_word = 4;
        scenes.push_back(sc);
        // bit 0 set is the orthographic bit, not part of the bias: 0x8B = 5 is a bias of 4 (z_int = k - 4)
        sc.name = "0x8B = 5: orthographic, a bias of 4";
        sc.lk.shadow_word = 5;
        sc.uvw = {kCentreU, kCentreV, Coord{z}};
        scenes.push_back(sc);
    }
    {
        // a bias above z * 16777215: z_int < 0, a texel of depth 0 is lit
        LookupScene sc;
        sc.name = "a bias larger than z * 16777215 (depth 0 lit)";
        sc.map = LookupMap(kSize, kSize, [](u32 s, u32 t) { return (s + t) % 2 == 0 ? 0u : 5u; });
        sc.lk.shadow_word = (k + 1000) << 1 | 1u;
        sc.uvw = {kCentreU, kCentreV, Coord{z}};
        scenes.push_back(sc);
    }
    // every wrap mode on s, on t and on both: a ramp from -1.375 to 2.125 (texels -11 to 17 of 8), with a border colour that is not
    // white and is lit (depth 0x804020 above k, intensity 0x10), and once with an unlit one (depth 0x010203)
    const char* mode_names[8] = {"ClampToEdge", "ClampToBorder", "Repeat", "MirroredRepeat", "ClampToEdge2", "ClampToBorder2", "Repeat2", "Repeat3"};
    const Coord ramp{-1.375, 3.5, 0};
    for (u32 m = 0; m < 8; ++m) {
        for (int axis = 0; axis < 3; ++axis) {
            for (u32 border : {0x80402010u, 0x01020390u}) {
                if (border == 0x01020390u && m != 1 && m != 5) continue;
                LookupScene sc;
                sc.name = std::string(mode_names[m]) + (axis == 0 ? " on s" : axis == 1 ? " on t" : " on s and t") +
                          (border == 0x80402010u ? ", border lit" : ", border unlit");
                sc.map = map;
                sc.lk.border = border;
                if (axis != 1) sc.lk.wrap_s = m;
                if (axis != 0) sc.lk.wrap_t = m;
                sc.uvw = {axis == 1 ? kCentreU : ramp, axis == 0 ? kCentreV : Coord{-1.375, 0, 3.5}, Coord{z}};
                scenes.push_back(sc);
            }
        }
    }
    {
        // u in (-1/8, 0): int() truncates toward zero, texel 0 (not the border, not the last texel)
        LookupScene sc;
        sc.name = "u in (-1/width, 0) is texel 0";
        sc.map = map;
        sc.lk.wrap_s = 1;
        sc.lk.wrap_t = 2;
        sc.lk.border = 0x80402010u;
        sc.uvw = {Coord{-1.25 / 8, 1, 0}, Coord{-0.75 / 8, 0, 1}, Coord{z}};
        scenes.push_back(sc);
    }
    {
        // Repeat on the negative side of a texture 24 texels wide: uint(s) % 24 is not a signed modulo (s = -5 gives texel 11)
        LookupScene sc;
        sc.name = "Repeat on the negative side of a texture 24 texels wide";
        sc.map = LookupMap(24, kSize, [&](u32 s, u32 t) { return (s * 7 + t) % 2 == 0 ? k : k - 1; });
        sc.lk.wrap_s = 2;
        sc.uvw = {Coord{-0.5, 0.375, 0}, kCentreV, Coord{z}};
        scenes.push_back(sc);
    }
    {
        // unit 0 off: (0, 0, 0, 0), from its binding (word 0) and from its border colour (transparent black)
        LookupScene sc;
        sc.name = "unit 0 off";
        sc.map = map;
        sc.lk.enabled = false;
        sc.lk.wrap_s = 1;
        sc.lk.border = 0x80402010u;
        sc.unit_off_binding = true;
        sc.uvw = {ramp, kCentreV, Coord{z}};
        scenes.push_back(sc);
    }
    for (const LookupScene& sc : scenes) CheckLookup(rigs, sc);
    // the port's 24-wide repeat really differs from a signed modulo there
    CHECK(sw::Wrap(2, -5, 24) == 11);
}

TEST_CASE("shadow/ShadowCube_is_ConvertCubeCoords_face_and_texel_compared_like_Shadow2D") {
    Rigs rigs;
    if (!rigs) return;
    const u32 k = 3000001;
    // the major coordinate's magnitude is at least 1 in every scene, so min(z, 1) * 16777215 = 16777215; 0x8B holds bias << 1 in
    // bits 1-23 (bit 0, the orthographic bit, is clear and not read by the cube), and z_int = 16777215 - (0x8B & 0xFFFFFE) = k
    const u32 shadow_word = 16777215u - k;
    REQUIRE((shadow_word & 0x00FFFFFEu) == shadow_word);
    std::array<sw::Map, 6> faces;
    for (u32 f = 0; f < 6; ++f) {
        faces[f] = LookupMap(kSize, kSize, [&](u32 s, u32 t) { return k - 1 + (s + t + f) % 3; });
        for (u32& word : faces[f].words) word = (word & ~0xFFu) | ((word + 37 * f) & 0xFFu) | 1u;  // other words per face
    }
    std::vector<LookupScene> scenes;
    const char* names[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
    for (int face = 0; face < 6; ++face) {
        // the dominant component +-2, the other two sweep (-1, 1) along x and along y
        std::array<Coord, 3> comp{};
        bool first = true;
        for (int i = 0; i < 3; ++i) {
            if (i == face / 2) {
                comp[i] = Coord{face % 2 != 0 ? -2.0 : 2.0};
            } else if (first) {
                comp[i] = Coord{-1, 2, 0};
                first = false;
            } else {
                comp[i] = Coord{-1, 0, 2};
            }
        }
        LookupScene sc;
        sc.name = std::string("face ") + names[face];
        sc.type = 4;
        sc.faces = faces;
        sc.lk.shadow_word = shadow_word;
        sc.uvw = comp;
        scenes.push_back(sc);
        if (face == 0 || face == 3) {
            // a border wrap on the cube: a face coordinate is inside, so no border shows here
            sc.name += " with ClampToBorder";
            sc.lk.wrap_s = sc.lk.wrap_t = 1;
            sc.lk.border = 0x01020390u;
            scenes.push_back(sc);
        }
    }
    {
        // each side of a major-axis choice: |u| against |v| (+X or +Y by the column), |u| against |w| (-X or -Z), |v| against |w|
        // (-Y or +Z by the row)
        LookupScene sc;
        sc.type = 4;
        sc.faces = faces;
        sc.lk.shadow_word = shadow_word;
        sc.name = "+X or +Y";
        sc.uvw = {Coord{1.0 - 0.125, 0.25, 0}, Coord{1.0 + 0.125, -0.25, 0}, Coord{-0.55, 0, 1.1}};
        scenes.push_back(sc);
        sc.name = "-X or -Z";
        sc.uvw = {Coord{-1.0 + 0.125, -0.25, 0}, Coord{-0.55, 0, 1.1}, Coord{-1.0 - 0.125, 0.25, 0}};
        scenes.push_back(sc);
        sc.name = "-Y or +Z";
        sc.uvw = {Coord{-0.55, 1.1, 0}, Coord{-1.0 + 0.125, 0, -0.25}, Coord{1.0 + 0.125, 0, -0.25}};
        scenes.push_back(sc);
    }
    for (const LookupScene& sc : scenes) CheckLookup(rigs, sc);
}

// ---- the interlock path is the one tested ----------------------------------------------------------------------------------------

TEST_CASE("shadow/the_device_as_it_is_takes_the_interlock_and_the_masked_backend_the_compare_and_swap_loop") {
    Rigs rigs;
    if (!rigs) return;
    const rhi::Capabilities& lean = rigs.lean->Caps();
    CHECK_FALSE(lean.interlock);
    CHECK(lean.masked.Has(rhi::Cap::Interlock));
    const Generated lean_uber = GenerateUber(ShaderPass::ShadowWrite, ProfileOf(lean));
    const Generated lean_spec = GenerateSpecialised(ShadowKey(), ProfileOf(lean), ShaderPass::ShadowWrite);
    REQUIRE(lean_uber.ok());
    REQUIRE(lean_spec.ok());
    CHECK_FALSE(HasCapability(lean_uber.spirv, kCapFragmentShaderPixelInterlock));
    CHECK_FALSE(HasCapability(lean_spec.spirv, kCapFragmentShaderPixelInterlock));
    CHECK(HasInstruction(lean_uber.spirv, kOpAtomicCompareExchange));
    CHECK(HasInstruction(lean_spec.spirv, kOpAtomicCompareExchange));

    const rhi::Capabilities& full = rigs.full->Caps();
    if (full.masked.Has(rhi::Cap::Interlock)) {
        CHECK_FALSE(full.interlock);  // MH4N_RHI_MASK=interlock: both backends take the loop
        return;
    }
    if (!full.interlock) {
        const bool lavapipe = full.device_name.find("llvmpipe") != std::string::npos;
        if (test::VulkanRequired() && lavapipe) {
            FAIL("lavapipe does not report the interlock capability and MH4N_REQUIRE_VULKAN=1");
        }
        test::SkipCase("the interlock half: the device has no fragment shader pixel interlock (" + full.device_name + ")");
        return;
    }
    const Generated full_uber = GenerateUber(ShaderPass::ShadowWrite, ProfileOf(full));
    const Generated full_spec = GenerateSpecialised(ShadowKey(), ProfileOf(full), ShaderPass::ShadowWrite);
    REQUIRE(full_uber.ok());
    REQUIRE(full_spec.ok());
    CHECK(HasCapability(full_uber.spirv, kCapFragmentShaderPixelInterlock));
    CHECK(HasCapability(full_spec.spirv, kCapFragmentShaderPixelInterlock));
    CHECK(HasExecutionMode(full_spec.spirv, kModePixelInterlockOrdered));
    CHECK(rigs.all[0]->profile.interlock);
}

// ---- the barriers --------------------------------------------------------------------------------------------------------------

namespace {

// What the interceptor saw, in order: each memory barrier of a vkCmdPipelineBarrier (its stages and access masks), each
// vkCmdBeginRenderPass and each vkCmdDraw.
struct Event {
    enum Kind { Memory, BeginRenderPass, Draw } kind;
    VkPipelineStageFlags src = 0, dst = 0;
    VkAccessFlags src_access = 0, dst_access = 0;
};
std::mutex gEventsMutex;
std::vector<Event> gEvents;
PFN_vkCmdPipelineBarrier gRealPipelineBarrier = nullptr;
PFN_vkCmdBeginRenderPass gRealBeginRenderPass = nullptr;
PFN_vkCmdDraw gRealDraw = nullptr;

void VKAPI_PTR TracePipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
                                    uint32_t memory_count, const VkMemoryBarrier* memory, uint32_t buffer_count,
                                    const VkBufferMemoryBarrier* buffer, uint32_t image_count, const VkImageMemoryBarrier* image) {
    {
        std::lock_guard lock(gEventsMutex);
        for (uint32_t i = 0; i < memory_count; ++i) {
            gEvents.push_back(Event{Event::Memory, src, dst, memory[i].srcAccessMask, memory[i].dstAccessMask});
        }
    }
    gRealPipelineBarrier(commands, src, dst, flags, memory_count, memory, buffer_count, buffer, image_count, image);
}
void VKAPI_PTR TraceBeginRenderPass(VkCommandBuffer commands, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    {
        std::lock_guard lock(gEventsMutex);
        gEvents.push_back(Event{Event::BeginRenderPass});
    }
    gRealBeginRenderPass(commands, info, contents);
}
void VKAPI_PTR TraceDraw(VkCommandBuffer commands, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) {
    {
        std::lock_guard lock(gEventsMutex);
        gEvents.push_back(Event{Event::Draw});
    }
    gRealDraw(commands, vertices, instances, first_vertex, first_instance);
}

// Installs the three through volk's function pointers while it lives.
struct TraceCommands {
    TraceCommands() {
        gRealPipelineBarrier = vkCmdPipelineBarrier;
        gRealBeginRenderPass = vkCmdBeginRenderPass;
        gRealDraw = vkCmdDraw;
        vkCmdPipelineBarrier = TracePipelineBarrier;
        vkCmdBeginRenderPass = TraceBeginRenderPass;
        vkCmdDraw = TraceDraw;
        std::lock_guard lock(gEventsMutex);
        gEvents.clear();
    }
    ~TraceCommands() {
        vkCmdPipelineBarrier = gRealPipelineBarrier;
        vkCmdBeginRenderPass = gRealBeginRenderPass;
        vkCmdDraw = gRealDraw;
    }
    // The events as letters: F (a fragment-to-fragment memory barrier), C (fragment to compute), R (a render pass begins), D (a
    // draw); the other memory barriers (the targets module's, the readback's) are left out.
    std::string Letters() {
        std::lock_guard lock(gEventsMutex);
        std::string out;
        for (const Event& e : gEvents) {
            if (e.kind == Event::BeginRenderPass) out += 'R';
            if (e.kind == Event::Draw) out += 'D';
            if (e.kind != Event::Memory || e.src != VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) continue;
            const bool writes_seen = (e.src_access & VK_ACCESS_SHADER_WRITE_BIT) != 0;
            if (e.dst == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT && writes_seen && (e.dst_access & VK_ACCESS_SHADER_READ_BIT) != 0 &&
                (e.dst_access & VK_ACCESS_SHADER_WRITE_BIT) != 0) {
                out += 'F';
            } else if (e.dst == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && writes_seen && (e.dst_access & VK_ACCESS_SHADER_READ_BIT) != 0) {
                out += 'C';
            } else {
                out += '?';
            }
        }
        return out;
    }
};

}  // namespace

TEST_CASE("shadow/two_maps_written_and_looked_up_in_one_recording_give_the_stored_values_and_the_barriers_are_recorded") {
    Rigs rigs;
    if (!rigs) return;
    const u32 d1 = 3000000, d2 = 2500000, k = 2750000, d3 = 2200000;
    const float z = DepthFor(k);
    for (auto& rig : rigs.all) {
        INFO(rig->Name());
        // two maps of the same size, every texel at depth 0xF00000 and intensity 90
        const sw::Map start{kSize, kSize, std::vector<u32>(kSize * kSize, Word(0xF00000u, 90))};
        std::shared_ptr<rhi::Image> a = rig->Upload(start), b = rig->Upload(start);
        // map a: depth d1 over the left half; map b: depth d2 over the right half; then a looked up on the left half of the colour
        // target and b on the right half, with z_int = k: a's d1 is lit, b's d2 is not, the untouched 0xF00000 is; then map a
        // written again, depth d3 (below d1 and 0xF00000) over the top half (image rows 0-3), with no EndPass step after it: Run's
        // final EndPass closes that pass
        std::vector<Step> steps(5);
        steps[0].state = WriteState(0x3C00, 0);
        steps[0].vertices = Rect(0, 0, 4, 8, kSize, kSize, DepthFor(d1), Green(0));
        steps[0].shadow_target = a.get();
        steps[1].state = WriteState(0x3C00, 0);
        steps[1].vertices = Rect(4, 0, 8, 8, kSize, kSize, DepthFor(d2), Green(0));
        steps[1].shadow_target = b.get();
        sw::Lookup lk;
        lk.wrap_s = lk.wrap_t = 0;
        const std::array<Coord, 3> uvw = {kCentreU, kCentreV, Coord{z}};
        steps[2].state = LookupState(2, lk);
        steps[2].vertices = Rect(0, 0, 4, 8, kSize, kSize, 0.5f, {1, 1, 1, 1}, uvw);
        steps[2].unit0 = native::TextureBinding{a.get(), rhi::ImageView::ShadowStorage, nullptr};
        steps[3].state = LookupState(2, lk);
        steps[3].vertices = Rect(4, 0, 8, 8, kSize, kSize, 0.5f, {1, 1, 1, 1}, uvw);
        steps[3].unit0 = native::TextureBinding{b.get(), rhi::ImageView::ShadowStorage, nullptr};
        steps[4].state = WriteState(0x3C00, 0);
        steps[4].vertices = Rect(0, 0, 8, 4, kSize, kSize, DepthFor(d3), Green(0));
        steps[4].shadow_target = a.get();
        std::string letters;
        std::vector<u8> pixels;
        {
            TraceCommands trace;
            pixels = rig->Run(steps);
            letters = trace.Letters();
        }
        // (1) before each map's pass; (2) after each, before the next draw; the second lookup continues the target pass; the
        // "F" before the last "R" is (1) for the write to a after a's lookup (write after read), and the last "FC" is (2) recorded
        // by EndPass (the path FlushRecording takes before each submission and before the write-back)
        CHECK_MESSAGE(letters == "FRDFCFRDFCRDDFRDFC", "the recorded sequence is " << letters);
        // the stored values: the left half (map a, d1 >= k) lit with 90, the right half (map b at d2 < k) unlit (the lookups come
        // before the last write); and the maps
        for (u32 py = 0; py < kSize; ++py) {
            for (u32 px = 0; px < kSize; ++px) {
                const int value = px < 4 ? 90 : 0;
                const std::array<int, 4> want = {value, value, value, value};
                CHECK_MESSAGE(Pixel(pixels, px, py) == want, "pixel (" << px << "," << py << ")");
            }
        }
        const std::vector<u32> got_a = rig->ReadMap(*a, kSize, kSize), got_b = rig->ReadMap(*b, kSize, kSize);
        for (u32 i = 0; i < kSize * kSize; ++i) {
            // image rows 0-3 are decoded rows 7-4: d3 there, below both depths it covers
            const u32 before_a = (i % kSize) < 4 ? Word(d1, 90) : Word(0xF00000u, 90);
            CHECK(got_a[i] == (i / kSize >= 4 ? Word(d3, 90) : before_a));
            CHECK(got_b[i] == ((i % kSize) >= 4 ? Word(d2, 90) : Word(0xF00000u, 90)));
        }
    }
}

// ---- `--gpu native` ------------------------------------------------------------------------------------------------------------------

namespace {

constexpr u32 kFcram = mh4n::gpu_capture::kFcramPaddr;
constexpr u32 kVertexBase = kFcram + 0x100000;
constexpr u32 kMapAddress = kFcram + 0x200000;
constexpr u32 kColorBuffer = mh4n::gpu_capture::kVramPaddr + 0x10000;
constexpr u32 kColorBuffer2 = mh4n::gpu_capture::kVramPaddr + 0x11000;  // 8x8 RGBA8 each: none overlaps another or the map
constexpr u32 kColorBuffer3 = mh4n::gpu_capture::kVramPaddr + 0x12000;
constexpr u32 kListAddress = kFcram + 0x1000;
constexpr u32 kVs = 0x2B0;
constexpr u32 kRegOutTotal = 0x4F, kRegOutMap = 0x50;

namespace pi {
// nihstro's field positions: opcode 26-31, destination 21-25, source 1 12-18, source 2 7-11, operand descriptor 0-6.
constexpr u32 MOV = 0x13, END = 0x22;
constexpr u32 V(u32 n) { return n; }
constexpr u32 O(u32 n) { return n; }
constexpr u32 Ins(u32 op, u32 dest, u32 s1, u32 s2) { return (op << 26) | (dest << 21) | (s1 << 12) | (s2 << 7); }
constexpr u32 kEnd = END << 26;
constexpr u32 IdentityDescriptor() {
    u32 w = 0xF;
    for (u32 c = 0; c < 4; ++c) {
        w |= c << (11 - 2 * c);
        w |= c << (20 - 2 * c);
        w |= c << (29 - 2 * c);
    }
    return w;
}
}  // namespace pi

class CmdList {
public:
    CmdList& Write(u32 id, u32 value) {
        words_.push_back(value);
        words_.push_back(id | (0xFu << 16));
        return *this;
    }
    CmdList& Program(u32 base, const std::vector<u32>& code, const std::vector<u32>& descs) {
        Write(base + 0x1B, 0);
        for (u32 w : code) Write(base + 0x1C, w);
        Write(base + 0x25, 0);
        for (u32 w : descs) Write(base + 0x26, w);
        return *this;
    }
    std::vector<u8> Bytes() const {
        std::vector<u8> bytes(words_.size() * 4);
        std::memcpy(bytes.data(), words_.data(), bytes.size());
        return bytes;
    }

private:
    std::vector<u32> words_;
};

// The registers of every list: an 8x8 colour buffer at `color_address` (the size register's bit 24 set: not flipped), a
// viewport over it, no depth buffer, no blending, Z-buffering with a depth scale of -1, stage 0 replacing with `source`.
void Registers(CmdList& l, u32 color_address, u32 source) {
    l.Write(0x11D, color_address >> 3);
    l.Write(0x117, 0);
    l.Write(0x116, 0);
    l.Write(0x11C, 0);
    l.Write(0x11E, kSize | ((kSize - 1) << 12) | (1u << 24));
    l.Write(0x113, 0xF);
    l.Write(0x115, 0);
    l.Write(0x102, 3);
    l.Write(0x107, 0xF00);
    l.Write(0x104, 0);
    l.Write(0x1C6, 1);
    l.Write(0x41, F24(kSize / 2.0f));
    l.Write(0x43, F24(kSize / 2.0f));
    l.Write(0x68, 0);
    l.Write(0x4D, F24(-1.0f));
    l.Write(0x4E, 0);
    l.Write(0x6D, 1);
    l.Write(0x65, 0);
    l.Write(0x40, 0);
    for (u32 base : {0xC0u, 0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u}) {
        l.Write(base + 0, base == 0xC0 ? StageSources(source) : kPassSources);
        l.Write(base + 1, 0);
        l.Write(base + 2, 0);
    }
}

// The vertex program o0 = v0, o1 = v1: v1 is the colour (`texcoord` false) or texcoord0 (u, v, w) (true); two float4 attributes.
void VertexProgram(CmdList& l, bool texcoord) {
    using namespace pi;
    l.Program(kVs, {Ins(MOV, O(0), V(0), 0), Ins(MOV, O(1), V(1), 0), kEnd}, {IdentityDescriptor()});
    l.Write(kVs + 0x09, 1);
    l.Write(kVs + 0x0A, 0);
    l.Write(kVs + 0x0B, 0x76543210u);
    l.Write(kVs + 0x0C, 0xFEDCBA98u);
    l.Write(kVs + 0x0D, 0x3);
    l.Write(kRegOutTotal, 2);
    using namespace shader::semantic;
    auto map = [](u32 x, u32 y, u32 z, u32 w) { return x | (y << 8) | (z << 16) | (w << 24); };
    l.Write(kRegOutMap + 0, map(kPositionX, kPositionY, kPositionZ, kPositionW));
    l.Write(kRegOutMap + 1, texcoord ? map(kTexcoord0U, kTexcoord0V, kTexcoord0W, kInvalid) : map(kColorR, kColorG, kColorB, kColorA));
    l.Write(0x24A, 1);
    l.Write(0x251, 1);
    l.Write(0x200, (kVertexBase >> 4) << 1);
    l.Write(0x201, 0xFFu);
    l.Write(0x202, 1u << 28);
    l.Write(0x203, 0);
    l.Write(0x204, 0x10u);
    l.Write(0x205, (32u << 16) | (2u << 28));
    l.Write(0x229, 0);
    l.Write(0x25E, 0);
}

void DrawArrays(CmdList& l, u32 first, u32 count) { l.Write(0x228, count).Write(0x22A, first).Write(0x22E, 0); }

// The vertices of a rectangle [x0, x1) x [y0, y1) of the 8x8 window (image rows), each followed by its second attribute.
void RectVertices(std::vector<std::array<float, 4>>& out, u32 x0, u32 y0, u32 x1, u32 y1, float depth,
                  const std::array<float, 4>& second, const std::array<Coord, 3>* uvw = nullptr) {
    auto vertex = [&](u32 px, u32 py) {
        out.push_back({2.0f * px / kSize - 1.0f, 2.0f * py / kSize - 1.0f, -depth, 1.0f});
        if (uvw != nullptr) {
            const double x = static_cast<double>(px) / kSize, y = static_cast<double>(py) / kSize;
            out.push_back({static_cast<float>((*uvw)[0].At(x, y)), static_cast<float>((*uvw)[1].At(x, y)),
                           static_cast<float>((*uvw)[2].At(x, y)), 0.0f});
        } else {
            out.push_back(second);
        }
    };
    vertex(x0, y0);
    vertex(x1, y0);
    vertex(x1, y1);
    vertex(x0, y0);
    vertex(x1, y1);
    vertex(x0, y1);
}

struct Sim {
    mh4n::gpu_capture::CaptureHost host;
    std::unique_ptr<native::NativeGpu> gpu;
    Sim() : gpu(native::MakeNativeGpu(host, {})) {}

    void Write(u32 paddr, const std::vector<u8>& bytes) { REQUIRE(host.WriteBoth(paddr, bytes.data(), static_cast<u32>(bytes.size()))); }
    void Submit(const CmdList& list) {
        const std::vector<u8> bytes = list.Bytes();
        Write(kListAddress, bytes);
        mh4n::gpu::GxCommand command{};
        command.words[0] = 1;  // SubmitCmdList
        command.words[1] = kListAddress;
        command.words[2] = static_cast<u32>(bytes.size());
        gpu->Execute(command);
    }
    void Vertices(const std::vector<std::array<float, 4>>& vectors) {
        std::vector<u8> bytes(vectors.size() * 16);
        std::memcpy(bytes.data(), vectors.data(), bytes.size());
        Write(kVertexBase, bytes);
    }
    u64 Counter(const std::string& name) {
        for (const mh4n::gpu::Counter& c : gpu->PeekCounters()) {
            if (c.name == name) return c.value;
        }
        FAIL("no counter " << name);
        return 0;
    }
    // Writes an 8x8 RGBA8 texture whose decoded texels are `words`, by inverting the decoder: a probe whose byte i is i tells which
    // byte of the tile each channel of each texel is.
    void WriteWords(u32 paddr, const std::vector<u32>& words) {
        std::vector<u8> probe(256);
        for (size_t i = 0; i < 256; ++i) probe[i] = static_cast<u8>(i);
        std::vector<u8> decoded;
        REQUIRE(texture::DecodeToRgba8(probe.data(), probe.size(), 8, 8, texture::Format::RGBA8, decoded));
        std::vector<u8> memory(256);
        for (size_t t = 0; t < 64; ++t) {
            for (int c = 0; c < 4; ++c) memory[decoded[t * 4 + c]] = static_cast<u8>(words[t] >> (8 * c));
        }
        Write(paddr, memory);
    }
    std::vector<u32> ReadWords(u32 paddr) {
        const u8* memory = host.PhysicalPointer(paddr, 256);
        REQUIRE(memory != nullptr);
        std::vector<u8> rgba;
        REQUIRE(texture::DecodeToRgba8(memory, 256, 8, 8, texture::Format::RGBA8, rgba));
        std::vector<u32> words(64);
        std::memcpy(words.data(), rgba.data(), 256);
        return words;
    }
};

}  // namespace

TEST_CASE("native/a_shadow_map_drawn_by_shadow_mode_lists_is_written_back_and_looked_up_from_guest_memory") {
    if (!test::RequireBackend()) return;
    Sim sim;
    // The map's guest bytes hold a pattern: texel i (decoded order) has the depth 9 000 000 + 1000 i, or 1 000 000 for every
    // seventh texel (below every d: a write leaves it), and the intensity 60 + i.
    std::vector<u32> pattern(64);
    for (u32 i = 0; i < 64; ++i) pattern[i] = Word(i % 7 == 0 ? 1000000u : 9000000u + 1000u * i, 60 + i);
    sim.WriteWords(kMapAddress, pattern);

    // list 1: two overlapping rectangles with green 0 at different depths (one kind per list: the loop is exact per kind)
    const u32 d_a = 3000000, d_b = 2500000, d_c = 2200000;
    struct Rectangle {
        u32 x0, y0, x1, y1, d, s;
    };
    const Rectangle a{0, 0, 6, 5, d_a, 0}, b{2, 3, 8, 8, d_b, 0}, c{4, 0, 8, 4, d_c, 151};
    {
        std::vector<std::array<float, 4>> v;
        RectVertices(v, a.x0, a.y0, a.x1, a.y1, DepthFor(a.d), Green(0));
        RectVertices(v, b.x0, b.y0, b.x1, b.y1, DepthFor(b.d), Green(0));
        sim.Vertices(v);
        CmdList l;
        Registers(l, kMapAddress, kSourcePrimary);
        l.Write(0x117, 3u << 16);  // RGB565: 0x117 is ignored in shadow mode, DrawShadowMapPixel writes 4 bytes a texel
        l.Write(kRegOutputMerger, kShadowMode);
        l.Write(kRegFramebufferShadow, 0x4000);  // c = 2, l = 0
        VertexProgram(l, false);
        DrawArrays(l, 0, 6);
        DrawArrays(l, 6, 6);
        sim.Submit(l);
    }
    // list 2: green 151 (s' = 151 / 2 = 75.5) over part of the first's texels and part of the untouched pattern
    {
        std::vector<std::array<float, 4>> v;
        RectVertices(v, c.x0, c.y0, c.x1, c.y1, DepthFor(c.d), Green(c.s));
        sim.Vertices(v);
        CmdList l;
        Registers(l, kMapAddress, kSourcePrimary);
        l.Write(0x117, 3u << 16);  // the same 2-byte format as list 1 (one shadow target), ignored as there
        l.Write(kRegOutputMerger, kShadowMode);
        l.Write(kRegFramebufferShadow, 0x4000);
        VertexProgram(l, false);
        DrawArrays(l, 0, 6);
        sim.Submit(l);
    }
    // The colour buffers of the lists below hold 0x5A bytes first, so a draw that is dropped or leaves a pixel shows.
    sim.Write(kColorBuffer2, std::vector<u8>(256, 0x5A));
    sim.Write(kColorBuffer3, std::vector<u8>(256, 0x5A));
    const u32 k = 2750000;
    // list T: the map's memory read as a Texture2D (unit 0 on, nearest, ClampToEdge) into a third colour buffer, before list 3's
    // Shadow2D lookup of the same memory in the same frame: TexImageKey's shadow flag gives the two their own images (a Texture2D
    // image has no ShadowStorage view, which the Shadow2D lookup needs)
    {
        const std::array<Coord, 3> uvw = {kCentreU, kCentreV, Coord{}};
        std::vector<std::array<float, 4>> v;
        RectVertices(v, 0, 0, kSize, kSize, 0.5f, {}, &uvw);
        sim.Vertices(v);
        CmdList l;
        Registers(l, kColorBuffer3, kSourceTexture0);
        l.Write(kRegOutputMerger, 0);
        l.Write(kRegMain, 1);
        l.Write(0x81, 0);
        l.Write(0x82, kSize | (kSize << 16));
        l.Write(0x83, 0);  // Texture2D, nearest, ClampToEdge
        l.Write(0x84, 0);
        l.Write(0x85, kMapAddress >> 3);
        l.Write(kRegTex0Format, 0);  // RGBA8
        VertexProgram(l, true);
        DrawArrays(l, 0, 6);
        sim.Submit(l);
    }
    // list 3: a normal draw into a colour target elsewhere whose TEV output is texture 0, unit 0 a Shadow2D at the map's address:
    // pixel (px, py) reads texel (px, py), orthographic, z_int = 2 750 000
    {
        const std::array<Coord, 3> uvw = {kCentreU, kCentreV, Coord{DepthFor(k)}};
        std::vector<std::array<float, 4>> v;
        RectVertices(v, 0, 0, kSize, kSize, 0.5f, {}, &uvw);
        sim.Vertices(v);
        CmdList l;
        Registers(l, kColorBuffer, kSourceTexture0);
        l.Write(kRegOutputMerger, 0);
        l.Write(kRegMain, 1);
        l.Write(0x81, 0);
        l.Write(0x82, kSize | (kSize << 16));
        l.Write(0x83, 2u << 28);  // Shadow2D, ClampToEdge
        l.Write(0x84, 0);
        l.Write(0x85, kMapAddress >> 3);
        l.Write(kRegTex0Format, 0);  // RGBA8
        l.Write(kRegShadowTexture, 1);  // orthographic, bias 0
        VertexProgram(l, true);
        DrawArrays(l, 0, 6);
        sim.Submit(l);
    }
    // list 4: NativeGpu's fallback binding of a Shadow2D unit, into a fourth colour buffer. The left half with unit 0 off (0x080 =
    // 0), ClampToBorder on s with a border colour that is neither black nor white and s running from -0.5 (two columns on the
    // border); the right half with unit 0 on and address 0, coordinates inside. Both read word 0 (the 1x1 shadow image) and the
    // off unit's border is transparent black: (0, 0, 0, 0) everywhere, where the composer's white dummy (word 0xFFFFFFFF) gives
    // 255 and null_2d_ (no ShadowStorage view) drops the draw. The address-0 unit gives (0, 0, 0, 1) in the software renderer:
    // the spec's listed difference.
    {
        const Coord z{DepthFor(k)};
        const std::array<Coord, 3> border = {Coord{-0.5, 2, 0}, kCentreV, z}, inside = {kCentreU, kCentreV, z};
        std::vector<std::array<float, 4>> v;
        RectVertices(v, 0, 0, 4, kSize, 0.5f, {}, &border);
        RectVertices(v, 4, 0, kSize, kSize, 0.5f, {}, &inside);
        sim.Vertices(v);
        CmdList l;
        Registers(l, kColorBuffer2, kSourceTexture0);
        l.Write(kRegOutputMerger, 0);
        l.Write(kRegMain, 0);
        l.Write(0x81, 0x80402010u);
        l.Write(0x82, kSize | (kSize << 16));
        l.Write(0x83, (2u << 28) | (1u << 12));  // Shadow2D, ClampToBorder on s
        l.Write(0x84, 0);
        l.Write(0x85, kMapAddress >> 3);
        l.Write(kRegTex0Format, 0);
        l.Write(kRegShadowTexture, 1);
        VertexProgram(l, true);
        DrawArrays(l, 0, 6);
        l.Write(kRegMain, 1);
        l.Write(0x85, 0);
        DrawArrays(l, 6, 6);
        sim.Submit(l);
    }
    sim.gpu->WriteBackColorBuffers();

    // The port: a fragment at window (x, y) writes decoded row 7 - y (DrawShadowMapPixel's y = framebuffer.height - y, the register
    // holding h - 1); list 1 then list 2.
    std::vector<u32> want = pattern;
    for (const Rectangle& r : {a, b, c}) {
        for (u32 y = r.y0; y < r.y1; ++y) {
            for (u32 x = r.x0; x < r.x1; ++x) {
                u32& word = want[(7 - y) * 8 + x];
                word = sw::Write(word, r.d, r.s, 0x4000, 0x0000);
            }
        }
    }
    const std::vector<u32> map = sim.ReadWords(kMapAddress);
    u32 wrong = 0, touched = 0;
    for (u32 i = 0; i < 64; ++i) {
        if (map[i] != want[i]) ++wrong;
        if (want[i] != pattern[i]) ++touched;
        else CHECK_MESSAGE(map[i] == pattern[i], "untouched texel " << i << " changed");
    }
    CHECK_MESSAGE(wrong == 0, wrong << " texels of the map differ from the port");
    CHECK(touched >= 30);  // the case changes a good part of the map
    // by hand: the fragment at window (6, 1) is in rectangle c only, so its texel is the pattern's (depth 9 000 000 + 1000 i above
    // d_c) with the intensity min(60 + i, 75): decoded row 7 - 1 = 6, column 6, i = 54, intensity 114 -> 75. In guest memory it is
    // at Morton(6, 6) = 0b111100 = 60 texels, byte 240 of the tile: the depth's bytes most significant first, then the intensity.
    {
        const u8* memory = sim.host.PhysicalPointer(kMapAddress, 256);
        REQUIRE(memory != nullptr);
        const u32 depth = 9000000u + 54000u;  // 0x8A2730
        CHECK(memory[240] == ((depth >> 16) & 0xFF));
        CHECK(memory[241] == ((depth >> 8) & 0xFF));
        CHECK(memory[242] == (depth & 0xFF));
        CHECK(memory[243] == 75);
        CHECK(want[6 * 8 + 6] == Word(depth, 75));
    }

    // The colour target: pixel (px, py) is lit with the texel's intensity when its depth is at least k; window row py is decoded
    // row 7 - py of the colour buffer, which reads decoded row 7 - py of the map (TextureColor's t flip).
    const u8* color = sim.host.PhysicalPointer(kColorBuffer, 256);
    REQUIRE(color != nullptr);
    std::vector<u8> rgba;
    REQUIRE(texture::DecodeToRgba8(color, 256, 8, 8, texture::Format::RGBA8, rgba));
    u32 lit = 0, colour_wrong = 0;
    for (u32 i = 0; i < 64; ++i) {
        const int intensity = sw::Compare(want[i], DepthFor(k), 1);
        lit += intensity != 0 ? 1 : 0;
        for (int ch = 0; ch < 4; ++ch) {
            if (rgba[i * 4 + ch] != intensity) ++colour_wrong;
        }
    }
    CHECK_MESSAGE(colour_wrong == 0, colour_wrong << " colour channels differ from the port");
    CHECK(lit >= 10);
    CHECK(lit <= 54);

    // List T: the map's decoded RGBA8 bytes as they stood after list 2 (no compare), in the same layout as list 3's target.
    {
        const u8* memory = sim.host.PhysicalPointer(kColorBuffer3, 256);
        REQUIRE(memory != nullptr);
        std::vector<u8> texture_rgba;
        REQUIRE(texture::DecodeToRgba8(memory, 256, 8, 8, texture::Format::RGBA8, texture_rgba));
        u32 texture_wrong = 0;
        for (u32 i = 0; i < 64; ++i) {
            for (u32 ch = 0; ch < 4; ++ch) {
                if (texture_rgba[i * 4 + ch] != ((want[i] >> (8 * ch)) & 0xFF)) ++texture_wrong;
            }
        }
        CHECK_MESSAGE(texture_wrong == 0, texture_wrong << " channels of the Texture2D read differ from the map's bytes");
    }
    // List 4: (0, 0, 0, 0) at all 64 pixels.
    {
        const u8* memory = sim.host.PhysicalPointer(kColorBuffer2, 256);
        REQUIRE(memory != nullptr);
        std::vector<u8> fallback_rgba;
        REQUIRE(texture::DecodeToRgba8(memory, 256, 8, 8, texture::Format::RGBA8, fallback_rgba));
        u32 not_zero = 0;
        for (u8 byte : fallback_rgba) not_zero += byte != 0 ? 1 : 0;
        CHECK_MESSAGE(not_zero == 0, not_zero << " channels of the off and address-0 units are not 0");
    }

    CHECK(sim.Counter("shadow_write_draws") == 3);
    CHECK(sim.Counter("shadow_lookups") == 1);  // list 3's; the Texture2D read, the off unit and address 0 are not lookups
    CHECK(sim.Counter("shadow_unavailable") == 0);
    CHECK(sim.Counter("stubbed:shadow") == 0);
    CHECK(sim.Counter("native_draws_dropped") == 0);
    CHECK(sim.Counter("native_draws") == 7);
}
