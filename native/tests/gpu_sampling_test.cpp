// Derived from Azahar src/video_core/renderer_software/sw_rasterizer.cpp (RasterizerSoftware::TextureColor: the texel index
// from the coordinate, the border test, the t flip, the projection and the cube conversion's caller), sw_texturing.cpp
// (GetWrappedTexCoord: the eight wrap modes), sw_clipper.cpp (ConvertCubeCoord), sw_framebuffer.cpp (Framebuffer::DrawPixel: the buffer row a window row is stored in),
// src/video_core/pica/regs_texturing.h (the
// register fields, the texture types and the wrap-mode enumerators), src/video_core/rasterizer_accelerated.cpp (the
// tex_lod_bias and tex_border_color uniforms), src/video_core/rasterizer_cache/sampler_params.h and rasterizer_cache.h (what a
// sampler is made of), src/video_core/renderer_vulkan/vk_texture_runtime.cpp (Sampler::Sampler) and pica_to_vk.h (the wrap and
// filter tables), and src/video_core/shader/generator/glsl_fs_shader_gen.cpp (getLod, the texture lookups of
// DefineTexUnitSampler and the procedural slot's routing), and for the registers the cases write: regs_internal.h
// (ASSERT_REG_POSITION: where each block starts), regs_framebuffer.h (the colour address, format and size, the output
// merger words), regs_rasterizer.h (the viewport, the depth range, the cull mode, the output map and the VSOutputAttributes
// semantic numbers), regs_pipeline.h (the vertex arrays, the draw triggers), regs_shader.h (the vertex shader's program and
// uniform setup registers), regs_lighting.h (the lighting disable word) and pica_types.h (the float24 layout of F24), at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: glsl_fs_shader_gen.cpp says "Licensed under GPLv3 or any later
// version"; the others say "GPLv2 or any later version").
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_framebuffer.h, regs_pipeline.h,
//   regs_shader.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
// Copyright 2017 Citra Emulator Project (regs_lighting.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
// The vertex-program words of the `--gpu native` cases place their fields where externals/nihstro/include/nihstro/shader_bytecode.h
// does (the submodule Azahar pins at f4d8659decbfe5d234f04134b5002b82dc515a44; it is NOT GPL: its licence is the BSD 3-clause
// licence of externals/nihstro/license.txt, reproduced below because it asks for that); the file as a whole is GPL-3.0-or-later.
// Copyright 2014 Tony Wasserka (shader_bytecode.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp, rasterizer_cache.h)
// Copyright 2017 Citra Emulator Project (sw_texturing.cpp, sw_framebuffer.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (sw_clipper.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2023 Citra Emulator Project (sampler_params.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_accelerated.cpp, vk_texture_runtime.cpp,
//   pica_to_vk.h, glsl_fs_shader_gen.cpp)
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
// Texture sampling (N2-23): the three texture units and the procedural slot, on whatever Vulkan device the machine has (lavapipe
// in CI), in the specialised mode and in the ubershader, with the custom border colour as the device has it and with that
// capability masked off (the M2's path), and through `--gpu native` from guest memory. A case that needs a device skips with a
// message without one and fails under MH4N_REQUIRE_VULKAN=1.
//
// WHERE THE EXPECTED PIXELS COME FROM. On the CPU, by an independent model (`sw` below) of the SOFTWARE renderer's formulas
// (TextureColor, GetWrappedTexCoord, ConvertCubeCoord) on the textures' texels in the decoder's memory order, never from the
// generator's output. The model departs from the software renderer in these places, each because the software renderer has no
// formula or is not what any other renderer does:
//   1. FILTERS AND LEVELS. The software renderer samples level 0 with the nearest texel only ("TODO: Apply the min and mag
//      filters", sw_rasterizer.cpp). For the filters, the level of detail, the bias and the mip filter the model is the Vulkan
//      specification's formulas (the linear weights of the 2x2 footprint, `ceil(lambda + 0.5) - 1` for a nearest mip level,
//      the mix of two levels for a linear one, lambda clamped to the sampler's levels) with Azahar's getLod
//      (log2 of the largest derivative of the coordinate in texels). The coordinates are chosen so that the nearest-texel
//      index, the chosen level and every weight are exact in binary and away from a decision boundary by at least 0.01
//      (`sw::g_margin` is checked at the end of every scene, so a scene that put a pixel next to a boundary fails instead of
//      being flaky); a blend is compared with a tolerance of one step per interpolation stage it uses (the
//      specification lets an implementation round each stage), fixed before any output was looked at.
//   2. NEGATIVE COORDINATES. The software renderer converts the scaled coordinate to an integer by truncation toward zero
//      (`static_cast<s32>(u * width)`), so a coordinate in (-1, 0) texels is texel 0 there and texel -1 everywhere else (a
//      sampler floors, and so do Azahar's three accelerated renderers). The model floors.
//   3. THE BORDER IS A HARD SWITCH AT THE EDGE IN THE SHADER'S EMULATION and a blend of the edge texel and the border colour
//      within half a texel of it in a sampler that has the colour. Linear filtering next to a border edge is therefore not
//      compared between the two paths: a scene with a border mode has nearest filters, or (the one linear border scene) pixels
//      at least a texel and a half from the edge, where both paths agree.
// The row order is the software renderer's `t = height - 1 - wrapped(t)`: pixel row r of a texture drawn with v = (r + 0.5) / 8
// shows the decoder's row (height - 1 - r). The upload (`sampling::UploadRgba8`) flips the rows and the model flips them
// independently, so a wrong order in either fails here.
//
// THE BORDER COLOUR RUNS BOTH WAYS. Every border scene is drawn on the backend as the device has it and on a backend created
// in the same process with `custom_border_color` masked off (as `MH4N_RHI_MASK=custom_border_color` does for the whole run);
// both must give the model's pixels, and the case checks that the masked run really used the shader's emulation (the device
// profile, the key's border bits in the generated text, and the samplers it asks for).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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

constexpr u32 kSize = 8;  // the target is 8x8

// Azahar's TevStageConfig::Source values the scenes use, and the stage word for "Replace with this source".
constexpr u32 kSourceTexture0 = 3;
u32 StageSources(u32 source) {  // colour and alpha source 0 = `source`, the others Previous
    return source | (0xFu << 4) | (0xFu << 8) | (source << 16) | (0xFu << 20) | (0xFu << 24);
}
constexpr u32 kPassSources = 0xFu | (0xFu << 4) | (0xFu << 8) | (0xFu << 16) | (0xFu << 20) | (0xFu << 24);

constexpr u32 kRegDepthRange = 0x4D, kRegDepthmap = 0x6D, kRegLogicOp = 0x102;
constexpr u32 kRegDepthColorMask = 0x107, kRegAllowColor = 0x113, kRegTev0 = 0xC0;
constexpr u32 kRegMain = 0x80;
constexpr u32 kRegTex[3] = {0x81, 0x91, 0x99};

u32 F24(float f) {
    if (f == 0.0f) return 0;
    u32 bits;
    std::memcpy(&bits, &f, 4);
    const u32 sign = bits >> 31;
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 63;
    return (sign << 23) | (static_cast<u32>(exponent) << 16) | ((bits >> 7) & 0xFFFF);
}

}  // namespace

// ---- the model of the software renderer (and the Vulkan filter formulas) -------------------------------------------------------

namespace sw {

using Color = std::array<int, 4>;
using Colord = std::array<double, 4>;

// The smallest distance from a decision boundary (an integer where a texel index or a level is chosen, 0 and 0.5 where a filter
// or a nearest mip level is) any pixel of the scene was from; checked at the end of each scene.
double g_margin = 1e9;
void Margin(double x) {  // x is compared with the integers
    const double f = std::floor(x);
    g_margin = std::min({g_margin, x - f, f + 1.0 - x});
}
bool g_track = true;  // off while a level is one flat colour: which texel is chosen cannot show
long Fl(double x) {
    if (g_track) Margin(x);
    return static_cast<long>(std::floor(x));
}

struct Level {
    u32 w = 0, h = 0;
    std::vector<Color> texel;  // the decoder's order: texel (x, y) at y * w + x, y = 0 the memory's first row
};
struct Texture {
    std::vector<Level> levels;
};

struct Sampler {
    int mag = 0, min = 0, mip = 0;   // 0 nearest, 1 linear
    int wrap_s = 0, wrap_t = 0;      // the PICA's modes 0 to 7
    Color border{};                  // the register's colour
    int lod_min = 0, lod_max = 0;
    double bias = 0.0;               // in levels
    bool custom_border = true;       // the sampler has the border colour (else the shader tests it)
};

// One axis of a nearest lookup: the texel index, or -1 for the border colour. `u` is the normalised coordinate.
long NearestAxis(int mode, double u, u32 n) {
    if ((mode == 4 || mode == 5) && u < 0) {
        u -= std::floor(u);  // a negative coordinate repeats (the software renderer; the shader's fract)
    }
    const long i = Fl(u * n);
    const long size = n;
    switch (mode) {
    case 0:
    case 4: return std::clamp(i, 0L, size - 1);
    case 1: return (i < 0 || i >= size) ? -1 : i;
    case 5: return i >= size ? -1 : i;
    case 3: {
        long m = ((i % (2 * size)) + 2 * size) % (2 * size);
        return m >= size ? 2 * size - 1 - m : m;
    }
    default: return ((i % size) + size) % size;  // 2, 6, 7
    }
}

// The texel of a level, flipped as the software renderer flips it: image row j shows the decoder's row h - 1 - j.
Color Texel(const Level& l, long i, long j) {
    return l.texel[static_cast<size_t>((l.h - 1 - j) * l.w + i)];
}

Colord ToD(const Color& c) { return {double(c[0]), double(c[1]), double(c[2]), double(c[3])}; }

// A lookup in one level, nearest or linear, with the sampler's wrap modes.
Colord SampleLevel(const Level& l, const Sampler& s, double u, double v, bool linear) {
    const Colord border = ToD(s.border);
    g_track = !std::all_of(l.texel.begin(), l.texel.end(), [&](const Color& c) { return c == l.texel[0]; });
    // The shader's own tests: ClampToBorder where the sampler has no border colour, ClampToBorder2 always (positive side).
    auto hard = [&](int mode, double c) {
        return (mode == 1 && !s.custom_border && (c < 0 || c >= 1)) || (mode == 5 && c >= 1);
    };
    if (hard(s.wrap_s, u) || hard(s.wrap_t, v)) {
        return border;
    }
    if (!linear) {
        const long i = NearestAxis(s.wrap_s, u, l.w);
        const long j = NearestAxis(s.wrap_t, v, l.h);
        if (i < 0 || j < 0) {
            return border;
        }
        return ToD(Texel(l, i, j));
    }
    // linear: the 2x2 footprint around (u * w - 0.5, v * h - 0.5), each texel addressed by the sampler's mode
    auto prep = [](int mode, double c) { return ((mode == 4 || mode == 5) && c < 0) ? c - std::floor(c) : c; };
    const double x = prep(s.wrap_s, u) * l.w - 0.5, y = prep(s.wrap_t, v) * l.h - 0.5;
    const long i0 = static_cast<long>(std::floor(x)), j0 = static_cast<long>(std::floor(y));
    const double a = x - i0, b = y - j0;
    // the sampler's address mode of a PICA mode
    auto address = [&](int mode, long i, long n) -> long {  // -1: the border colour
        switch (mode) {
        case 1:
            if (s.custom_border) return (i < 0 || i >= n) ? -1 : i;
            return std::clamp(i, 0L, n - 1);
        case 2:
        case 6:
        case 7: return ((i % n) + n) % n;
        case 3: {
            long m = ((i % (2 * n)) + 2 * n) % (2 * n);
            return m >= n ? 2 * n - 1 - m : m;
        }
        default: return std::clamp(i, 0L, n - 1);  // 0, 4, 5
        }
    };
    Colord out{};
    for (int dj = 0; dj < 2; ++dj) {
        for (int di = 0; di < 2; ++di) {
            const long i = address(s.wrap_s, i0 + di, l.w), j = address(s.wrap_t, j0 + dj, l.h);
            const Colord t = (i < 0 || j < 0) ? border : ToD(Texel(l, i, j));
            const double weight = (di ? a : 1 - a) * (dj ? b : 1 - b);
            for (int c = 0; c < 4; ++c) out[c] += weight * t[c];
        }
    }
    return out;
}

// A lookup with a level of detail: `lod` is Azahar's getLod (log2 of the largest derivative in texels), the bias is added.
Colord Sample(const Texture& t, const Sampler& s, double u, double v, double lod) {
    const double lo = s.lod_min, hi = std::max(s.lod_max, s.lod_min);
    const double lambda = std::clamp(lod + s.bias, lo, hi);
    const int nlev = static_cast<int>(t.levels.size());
    const bool filters_differ = s.mag != s.min;
    if (filters_differ) {
        // magnification (lambda <= 0) or minification: judged on the level of detail before the clamp to the sampler's levels
        // (minLod is never below 0, so a magnified lookup is clamped to exactly 0, which is a magnification by the rule `<= 0`)
        g_margin = std::min(g_margin, std::fabs(lod + s.bias));
    }
    const bool linear = lambda <= 0 ? s.mag == 1 : s.min == 1;
    if (s.mip == 0 || nlev == 1) {
        if (nlev > 1) Margin(lambda + 0.5);
        const int d = std::clamp(static_cast<int>(std::ceil(lambda + 0.5)) - 1, 0, nlev - 1);
        return SampleLevel(t.levels[d], s, u, v, linear);
    }
    if (lambda >= nlev - 1) {
        return SampleLevel(t.levels[nlev - 1], s, u, v, linear);
    }
    const int l0 = std::max(0, static_cast<int>(std::floor(lambda)));
    const double f = lambda <= 0 ? 0.0 : lambda - l0;
    const Colord a = SampleLevel(t.levels[l0], s, u, v, linear);
    const Colord b = SampleLevel(t.levels[l0 + 1], s, u, v, linear);
    Colord out{};
    for (int c = 0; c < 4; ++c) out[c] = a[c] * (1 - f) + b[c] * f;
    return out;
}

// ConvertCubeCoord (sw_clipper.cpp) in doubles: the face (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z) and the (s, t) in [0, 1].
struct CubeCoord {
    int face;
    double s, t;
};
CubeCoord ConvertCube(double u, double v, double w) {
    const double au = std::fabs(u), av = std::fabs(v), aw = std::fabs(w);
    double x, y, z;
    int face;
    if (au > av && au > aw) {
        if (u > 0) { face = 0; y = -v; } else { face = 1; y = v; }
        x = -w;
        z = u;
    } else if (av > aw) {
        if (v > 0) { face = 2; x = u; } else { face = 3; x = -u; }
        y = w;
        z = v;
    } else {
        if (w > 0) { face = 4; y = -v; } else { face = 5; y = v; }
        x = u;
        z = w;
    }
    return CubeCoord{face, x / z * 0.5 + 0.5, y / z * 0.5 + 0.5};
}

}  // namespace sw

// ---- scenes ------------------------------------------------------------------------------------------------------------------------

namespace {

// The coordinate at the centre of pixel (px, py): u0 + ux * (px + 0.5) / 8 + uy * (py + 0.5) / 8, and the same for v.
struct Coord {
    double u0 = 0, ux = 0, uy = 0, v0 = 0, vx = 0, vy = 0;
};
// The identity mapping of an 8x8 target: texel centres of an 8x8 texture.
constexpr Coord kIdentity{0, 1, 0, 0, 0, 1};

sw::Level PatternLevel(u32 w, u32 h, int seed) {
    sw::Level l;
    l.w = w;
    l.h = h;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            l.texel.push_back({static_cast<int>((x * 31 + y * 7 + seed * 50) % 241 + 8), static_cast<int>((x * 13 + y * 29 + seed * 17) % 241 + 8),
                               static_cast<int>((x * 3 + y * 47 + seed * 90) % 241 + 8), static_cast<int>(255 - (x * 5 + y * 3 + seed) % 200)});
        }
    }
    return l;
}
sw::Level FlatLevel(u32 w, u32 h, sw::Color c) {
    sw::Level l;
    l.w = w;
    l.h = h;
    l.texel.assign(static_cast<size_t>(w) * h, c);
    return l;
}
sw::Texture Single(sw::Level level) {
    sw::Texture t;
    t.levels.push_back(std::move(level));
    return t;
}
// A chain of flat levels 8x8, 4x4, 2x2, 1x1 of four different colours (the level shows in the colour).
sw::Texture FlatChain() {
    sw::Texture t;
    const sw::Color colors[4] = {{200, 40, 40, 255}, {40, 200, 40, 255}, {40, 40, 200, 255}, {200, 200, 40, 255}};
    for (u32 i = 0; i < 4; ++i) t.levels.push_back(FlatLevel(8u >> i, 8u >> i, colors[i]));
    return t;
}
// A chain of patterned levels 8x8, 4x4, 2x2.
sw::Texture PatternChain(int seed) {
    sw::Texture t;
    for (u32 i = 0; i < 3; ++i) t.levels.push_back(PatternLevel(8u >> i, 8u >> i, seed + static_cast<int>(i) * 3));
    return t;
}

struct UnitScene {
    bool enabled = false;
    sw::Texture tex;
    std::array<sw::Texture, 6> faces;  // unit 0 as a cube
    u32 type = 0;                      // TextureConfig::TextureType (unit 0)
    u32 wrap_s = 0, wrap_t = 0;
    u32 mag = 0, min = 0, mip = 0;
    sw::Color border = {0, 0, 0, 0};
    int bias_raw = 0;                  // fixed 1.4.8
    u32 lod_min = 0, lod_max = 0;
};

struct Scene {
    const char* name = "";
    std::array<UnitScene, 3> unit;
    u32 read = 0;                      // the stage-0 source: the texture unit 0 to 3
    std::array<Coord, 3> tc;
    Coord w;                           // texcoord0_w (u0, ux, uy only)
    bool coord1 = false;               // texture 2 on texcoord1
    bool proctex = false;
    bool border = false;               // the scene uses a border mode: also drawn with the capability masked off
    bool exact = false;                // a coordinate exactly on a texel edge on purpose: not held 0.01 away from the boundary
};

// The registers of a scene (the state a game would have written).
std::unique_ptr<pica::PicaState> StateOf(const Scene& sc) {
    auto state = std::make_unique<pica::PicaState>();
    state->regs.fill(0);
    state->dirty.SetAll();
    auto& r = state->regs;
    r[kRegTev0 + 0] = StageSources(kSourceTexture0 + sc.read);
    r[kRegTev0 + 1] = 0;
    r[kRegTev0 + 2] = 0;  // Replace, Replace
    for (u32 base : {0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u}) {
        r[base + 0] = kPassSources;
    }
    r[kRegLogicOp] = static_cast<u32>(re::LogicOp::Copy);
    r[kRegDepthColorMask] = 0xF00;
    r[kRegAllowColor] = 0xF;
    r[0x1C6] = 1;  // lighting disabled
    r[kRegDepthRange] = F24(-1.0f);
    r[kRegDepthmap] = 1;
    u32 main = 0;
    for (u32 u = 0; u < 3; ++u) {
        const UnitScene& us = sc.unit[u];
        if (!us.enabled) continue;
        main |= 1u << u;
        const sw::Level& base = us.type == 1 ? us.faces[0].levels[0] : us.tex.levels[0];
        r[kRegTex[u] + 0] = static_cast<u32>(us.border[0] | (us.border[1] << 8) | (us.border[2] << 16) | (static_cast<u32>(us.border[3]) << 24));
        r[kRegTex[u] + 1] = base.h | (base.w << 16);
        r[kRegTex[u] + 2] = (us.mag << 1) | (us.min << 2) | (us.wrap_t << 8) | (us.wrap_s << 12) | (us.mip << 24) | (u == 0 ? us.type << 28 : 0);
        r[kRegTex[u] + 3] = static_cast<u32>(us.bias_raw & 0x1FFF) | (us.lod_max << 16) | (us.lod_min << 24);
        r[kRegTex[u] + 4] = (0x1000u * (u + 1)) >> 3;  // an address (never read by this test)
    }
    if (sc.proctex) main |= 1u << 10;
    if (sc.coord1) main |= 1u << 13;
    r[kRegMain] = main;
    return state;
}

// Expected colour (as doubles) of pixel (px, py).
sw::Colord Expect(const Scene& sc, u32 px, u32 py, bool custom_border) {
    const double x = (px + 0.5) / kSize, y = (py + 0.5) / kSize;
    if (sc.read == 3) {
        return sc.proctex ? sw::Colord{51, 102, 153, 204} : sw::Colord{0, 0, 0, 0};  // the probe's ProcTex(), or vec4(0.0)
    }
    const UnitScene& us = sc.unit[sc.read];
    const Coord& c = sc.tc[(sc.read == 2 && sc.coord1) ? 1 : sc.read];
    double u = c.u0 + c.ux * x + c.uy * y, v = c.v0 + c.vx * x + c.vy * y;
    const double w = sc.w.u0 + sc.w.ux * x + sc.w.uy * y;
    sw::Sampler s;
    s.mag = us.mag; s.min = us.min; s.mip = us.mip; s.wrap_s = us.wrap_s; s.wrap_t = us.wrap_t;
    s.border = us.border; s.lod_min = us.lod_min; s.lod_max = us.lod_max; s.bias = us.bias_raw / 256.0;
    s.custom_border = custom_border;  // the border path of the backend the scene is drawn on
    if (sc.read == 0) {
        switch (us.type) {
        case 5: return {0, 0, 0, 0};
        case 2:  // the probe: shadowTexture(uv, w) gives vec4(uv, w * 0.25, 1.0)
        case 4: return {std::clamp(u, 0.0, 1.0) * 255, std::clamp(v, 0.0, 1.0) * 255, std::clamp(w * 0.25, 0.0, 1.0) * 255, 255};
        case 3: {
            u /= w;
            v /= w;
            sw::Sampler p = s;
            p.mag = p.min = 0;
            return sw::Sample(us.tex, p, u, v, 0.0);
        }
        case 1: {
            const sw::CubeCoord cc = sw::ConvertCube(u, v, w);
            sw::Sampler p = s;
            p.mag = p.min = 0;
            p.wrap_s = p.wrap_t = 0;
            return sw::Sample(us.faces[cc.face], p, cc.s, cc.t, 0.0);
        }
        default: break;
        }
    }
    // Azahar's getLod: log2 of the largest derivative of the coordinate in texels
    const sw::Level& base = us.tex.levels[0];
    const double dx = std::max(std::fabs(c.ux), std::fabs(c.uy)) / kSize * base.w;
    const double dy = std::max(std::fabs(c.vx), std::fabs(c.vy)) / kSize * base.h;
    const double d = std::max(dx, dy);
    const double lod = d > 0 ? std::log2(d) : -1000.0;
    return sw::Sample(us.tex, s, u, v, lod);
}

// ---- the rig: draws through the composition with the real generator -------------------------------------------------------------

using cpuvertex::OutputVertex;

rhi::VertexLayout BatchLayout() { return cpuvertex::MakeBatchVertexLayout(); }

native::VertexInputKey InputKey() {
    const rhi::VertexLayout layout = BatchLayout();
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

OutputVertex VertexOf(float x, float y, const Scene& sc, double fx, double fy) {  // fx, fy: the barycentric steps of the attribute ramps
    OutputVertex v{};
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = -0.5f; v.pos[3] = 1.0f;
    v.quat[3] = 1.0f;
    for (int i = 0; i < 4; ++i) v.color[i] = 1.0f;
    auto at = [&](const Coord& c, float* out) {
        // a0 + fx * 2 * ux + fy * 2 * uy (see Expect): the vertices are (0,0), (1,0), (0,1) of the triangle that spans 4 pixels' worth
        out[0] = static_cast<float>(c.u0 + fx * 2 * c.ux + fy * 2 * c.uy);
        out[1] = static_cast<float>(c.v0 + fx * 2 * c.vx + fy * 2 * c.vy);
    };
    at(sc.tc[0], v.tc0);
    at(sc.tc[1], v.tc1);
    at(sc.tc[2], v.tc2);
    v.tc0_w = static_cast<float>(sc.w.u0 + fx * 2 * sc.w.ux + fy * 2 * sc.w.uy);
    return v;
}

// Probes for the two features that are stubs in the tree (shadow/, proctex/): they stand in for them in a private registry,
// so this test does not change when N2-11 and N2-12 land, and they show what sampling/ passes them.
class ShadowProbe final : public FeatureModule {
public:
    const char* Name() const override { return "shadow"; }
    void Emit(Point point, EmitContext& ctx) const override {
        if (point == Point::Declarations) {
            ctx.emitter->Raw(
                "vec4 shadowTexture(vec2 uv, float w) { return vec4(uv, w * 0.25, 1.0); }\n"
                "vec4 shadowTextureCube(vec2 uv, float w) { return vec4(uv, w * 0.25, 1.0); }\n");
        }
    }
};
class ProcTexProbe final : public FeatureModule {
public:
    const char* Name() const override { return "proctex"; }
    void Emit(Point point, EmitContext& ctx) const override {
        if (point == Point::Declarations) ctx.emitter->Raw("vec4 ProcTex() { return vec4(0.2, 0.4, 0.6, 0.8); }\n");
    }
};
const ShadowProbe kShadowProbe;
const ProcTexProbe kProcTexProbe;

const FeatureRegistry& ProbeRegistry() {
    static const FeatureRegistry registry = [] {
        FeatureRegistry r;
        for (const char* name : {"sampling", "lighting", "fog"}) {
            const FeatureModule* f = FeatureRegistry::Global().Find(name);
            REQUIRE(f != nullptr);
            REQUIRE(r.Add(*f));
        }
        REQUIRE(r.Add(kShadowProbe));
        REQUIRE(r.Add(kProcTexProbe));
        return r;
    }();
    return registry;
}

struct Rig {
    std::shared_ptr<rhi::Backend> backend;
    DeviceProfile profile;
    const FeatureRegistry* registry;
    std::unique_ptr<native::PipelineBuilder> builder;
    std::unique_ptr<native::DefaultPipelineProvider> provider;
    std::unique_ptr<native::DrawComposer> composer;
    std::unique_ptr<FragmentUniformRing> uniforms;
    std::unique_ptr<TableRings> tables;
    std::unique_ptr<rhi::Ring> vertex_ring, vs_ring, staging;
    std::unique_ptr<sampling::SamplerCache> samplers;
    std::shared_ptr<rhi::Image> color;
    bool uber;

    Rig(std::shared_ptr<rhi::Backend> b, bool force_uber, const FeatureRegistry* features)
        : backend(std::move(b)), registry(features), uber(force_uber) {
        profile = ProfileOf(backend->Caps());
        builder = std::make_unique<native::PipelineBuilder>(backend, *registry);
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
        auto vr = backend->CreateRing(1 << 20, rhi::BufferUsage::Vertex, "sampling vertices");
        auto sr = backend->CreateRing(1 << 20, rhi::BufferUsage::Uniform, "sampling vs uniforms");
        auto tr = backend->CreateRing(4 << 20, rhi::BufferUsage::Transfer, "sampling staging");
        REQUIRE(vr.ok());
        REQUIRE(sr.ok());
        REQUIRE(tr.ok());
        vertex_ring = std::move(*vr);
        vs_ring = std::move(*sr);
        staging = std::move(*tr);
        samplers = std::make_unique<sampling::SamplerCache>(backend);
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = desc.height = kSize;
        desc.name = "sampling colour";
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        color = *image;
    }

    bool CustomBorder() const { return backend->Caps().custom_border_color; }

    // A backend image with the texture's levels (and layers, for a cube), uploaded in the decoder's row order.
    std::shared_ptr<rhi::Image> Upload(const sw::Texture* tex, const std::array<sw::Texture, 6>* faces) {
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        const sw::Texture& first = faces != nullptr ? (*faces)[0] : *tex;
        desc.width = first.levels[0].w;
        desc.height = first.levels[0].h;
        desc.mip_levels = static_cast<u32>(first.levels.size());
        desc.cube = faces != nullptr;
        desc.layers = faces != nullptr ? 6 : 1;
        desc.name = "sampling texture";
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        rhi::Recorder rec = backend->BeginRecording();
        for (u32 layer = 0; layer < desc.layers; ++layer) {
            const sw::Texture& t = faces != nullptr ? (*faces)[layer] : *tex;
            for (u32 mip = 0; mip < t.levels.size(); ++mip) {
                const sw::Level& l = t.levels[mip];
                std::vector<u8> bytes;
                for (const sw::Color& c : l.texel) {
                    for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<u8>(c[k]));
                }
                auto r = sampling::UploadRgba8(rec, *staging, **image, mip, layer, l.w, l.h, bytes);
                REQUIRE_MESSAGE(r.ok(), r.error());
            }
        }
        backend->WaitIdle(backend->Submit(std::move(rec)));
        return *image;
    }

    // Draws the scene: the key, the uniforms and the sampler objects come from the registers (the code under test), the
    // textures from the scene. Returns the colour target's pixels.
    std::vector<u8> Render(const Scene& sc) {
        auto state = StateOf(sc);
        const FragmentKey key = MakeKey(*state);
        FragmentBlock block;
        std::memset(&block, 0, sizeof(block));
        UniformDirty dirty;
        dirty.all = true;
        FillUniforms(*state, block, dirty, {}, *registry);
        block.cfg = key;

        std::array<std::shared_ptr<rhi::Image>, 3> images;
        std::array<std::shared_ptr<rhi::Sampler>, 3> sampler_objects;
        native::DrawDesc d;
        for (u32 u = 0; u < 3; ++u) {
            if (!sc.unit[u].enabled) continue;
            const UnitScene& us = sc.unit[u];
            const bool cube = u == 0 && us.type == 1;
            if (u == 0 && (us.type == 2 || us.type == 4 || us.type == 5)) continue;  // the shadow classes and Disabled bind no colour texture
            images[u] = Upload(cube ? nullptr : &us.tex, cube ? &us.faces : nullptr);
            auto smp = samplers->Get(sampling::SamplerParamsOf(*state, u));
            REQUIRE_MESSAGE(smp.ok(), smp.error());
            sampler_objects[u] = *smp;
            d.units[u] = native::TextureBinding{images[u].get(), rhi::ImageView::Sampled, sampler_objects[u].get()};
        }

        std::vector<u8> pixels;
        rhi::Recorder rec = backend->BeginRecording();
        composer->BeginRecording(rec);
        uniforms->BeginRecording();
        auto alloc = uniforms->Write(block, true);
        REQUIRE_MESSAGE(alloc.ok(), alloc.error());
        cpuvertex::TriangleBatch batch;
        batch.AddTriangle(VertexOf(-1, -1, sc, 0, 0), VertexOf(3, -1, sc, 1, 0), VertexOf(-1, 3, sc, 0, 1));
        auto streamed = cpuvertex::UploadBatch(*vertex_ring, batch);
        REQUIRE_MESSAGE(streamed.ok(), streamed.error());
        auto vs = vs_ring->Map(2048, std::max<u64>(256, backend->Caps().uniform_buffer_offset_alignment));
        REQUIRE_MESSAGE(vs.ok(), vs.error());
        std::memset(vs->data.data(), 0, 2048);
        REQUIRE(vs_ring->Commit(2048).ok());

        rhi::TargetPassDesc pass;
        pass.color = color.get();
        pass.area = rhi::Rect{0, 0, kSize, kSize};
        rhi::TargetPassDesc clear = pass;
        clear.color_load = rhi::LoadOp::Clear;
        clear.clear_color = {0.04f, 0.08f, 0.12f, 0.16f};
        composer->BeginTargetPass(rec, clear);
        const fbops::DynamicState dynamic = fbops::MakeDynamicState(*state);
        fbops::ApplyDynamicState(rec, dynamic, false);

        d.vertex.kind = native::VertexStageKind::Trivial;
        d.vertex.input = InputKey();
        d.fragment = &key;
        d.needs = NeedsOf(key, profile, *registry);
        d.state = fbops::MakeFixedFunctionState(*state);
        d.color_format = rhi::Format::Rgba8Unorm;
        d.target = pass;
        d.vs_pica_uniforms = native::UniformRange{vs_ring->GetBuffer().get(), vs->offset, 1616};
        d.vs_uniforms = native::UniformRange{vs_ring->GetBuffer().get(), vs->offset + 1792, 32};
        d.fragment_uniforms = *alloc;
        d.tables = tables.get();
        const rhi::VertexBufferBinding binding{vertex_ring->GetBuffer().get(), streamed->offset};
        d.vertex_buffers = std::span<const rhi::VertexBufferBinding>(&binding, 1);
        d.count = streamed->vertex_count;
        d.viewport = rhi::Viewport{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
        d.scissor = rhi::Rect{0, 0, kSize, kSize};
        auto result = composer->Draw(rec, d);
        REQUIRE_MESSAGE(result.ok(), result.error());
        backend->WaitIdle(backend->Submit(std::move(rec)));
        auto read = backend->ReadImage(*color, 0, 0, kSize, kSize);
        REQUIRE_MESSAGE(read.ok(), read.error());
        return *read;
    }
};

// The tolerance of a scene, fixed by the model: nothing for nearest sampling; one step per interpolation stage (two axes of a
// bilinear footprint, one mip mix) otherwise.
int ToleranceOf(const Scene& sc) {
    if (sc.read >= 3) return sc.read == 3 ? 1 : 0;
    const UnitScene& us = sc.unit[sc.read];
    if (sc.read == 0 && us.type == 2) return 1;  // the probe's float arithmetic and the byte rounding
    if (sc.read == 0 && (us.type == 4 || us.type == 1 || us.type == 3 || us.type == 5)) return us.type == 4 ? 1 : 0;
    int tol = 0;
    if (us.mag == 1 || us.min == 1) tol += 2;
    if (us.mip == 1 && us.tex.levels.size() > 1) tol += 1;
    return tol;
}

struct Rigs {
    std::shared_ptr<rhi::Backend> full, lean;
    std::unique_ptr<Rig> full_spec, full_uber, lean_spec, lean_uber;
    explicit Rigs(const FeatureRegistry* registry = &FeatureRegistry::Global()) {
        full = test::RequireBackend();
        rhi::BackendOptions masked;
        masked.mask = rhi::CapabilityMask{}.With(rhi::Cap::CustomBorderColor);
        if (full) lean = test::RequireBackend(masked);
        if (!full || !lean) return;
        full_spec = std::make_unique<Rig>(full, false, registry);
        full_uber = std::make_unique<Rig>(full, true, registry);
        lean_spec = std::make_unique<Rig>(lean, false, registry);
        lean_uber = std::make_unique<Rig>(lean, true, registry);
    }
    explicit operator bool() const { return full_spec != nullptr; }
};

// Draws a scene on the rigs that apply, and compares each with the model and the two modes with each other.
void CheckScene(Rigs& rigs, const Scene& sc) {
    std::vector<Rig*> list = {rigs.full_spec.get(), rigs.full_uber.get()};
    if (sc.border) {
        list.push_back(rigs.lean_spec.get());
        list.push_back(rigs.lean_uber.get());
    }
    const int tol = ToleranceOf(sc);
    std::vector<u8> specialised_pixels;
    for (Rig* rig : list) {
        sw::g_margin = 1e9;
        const std::vector<u8> pixels = rig->Render(sc);
        int worst = 0;
        std::string where;
        for (u32 py = 0; py < kSize; ++py) {
            for (u32 px = 0; px < kSize; ++px) {
                const sw::Colord want = Expect(sc, px, py, rig->CustomBorder());
                for (int c = 0; c < 4; ++c) {
                    const int got = pixels[(py * kSize + px) * 4 + c];
                    const int diff = static_cast<int>(std::lround(std::fabs(got - want[c])));
                    if (diff > worst) {
                        worst = diff;
                        where = "pixel (" + std::to_string(px) + "," + std::to_string(py) + ") channel " + std::to_string(c) + " is " +
                                std::to_string(got) + ", the model says " + std::to_string(want[c]);
                    }
                }
            }
        }
        CHECK_MESSAGE(worst <= tol, std::string(sc.name) << (rig->uber ? " [ubershader" : " [specialised") << (rig->CustomBorder() ? ", custom border" : ", emulated border")
                                            << "]: " << where << " (tolerance " << tol << ")");
        CHECK_MESSAGE((sc.exact || sw::g_margin > 0.01), std::string(sc.name) << ": a pixel of the scene is within " << sw::g_margin << " of a decision boundary");
        // The same scene in the two modes, on one backend, gives the same pixels (one step on another device).
        if (!rig->uber) {
            specialised_pixels = pixels;
        } else {
            bool same = specialised_pixels.size() == pixels.size();
            for (size_t i = 0; same && i < pixels.size(); ++i) same = std::abs(int(pixels[i]) - int(specialised_pixels[i])) <= 1;
            CHECK_MESSAGE(same, std::string(sc.name) << ": the ubershader and the specialised shader differ");
        }
    }
}

}  // namespace

// ---- texture types and the row order ---------------------------------------------------------------------------------------------

namespace {

Scene Plain2D(const char* name, sw::Texture tex = Single(PatternLevel(8, 8, 0))) {
    Scene sc;
    sc.name = name;
    sc.unit[0].enabled = true;
    sc.unit[0].tex = std::move(tex);
    sc.tc = {kIdentity, kIdentity, kIdentity};
    sc.w = Coord{1, 0, 0, 0, 0, 0};
    return sc;
}

}  // namespace

TEST_CASE("sampling/Texture2D_and_Projection2D_show_the_decoders_rows_in_the_software_renderers_order") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    CheckScene(rigs, Plain2D("Texture2D 8x8 at texel centres"));
    CheckScene(rigs, Plain2D("Texture2D 4x8 (width 4, height 8)", Single(PatternLevel(4, 8, 3))));
    Scene wide = Plain2D("Texture2D 16x4 (a quarter texel off the centres)", Single(PatternLevel(16, 4, 5)));
    wide.tc[0] = Coord{0.015625, 1, 0, 0, 0, 1};
    CheckScene(rigs, wide);

    // Projection2D: (u, v) / w with the texcoord0_w attribute. Twice the coordinates over a w of 2, and a w of 0.5 over half.
    Scene projection = Plain2D("Projection2D, w = 2");
    projection.unit[0].type = 3;
    projection.tc[0] = Coord{0, 2, 0, 0, 0, 2};
    projection.w = Coord{2, 0, 0, 0, 0, 0};
    CheckScene(rigs, projection);
    projection.name = "Projection2D, w = 0.5";
    projection.tc[0] = Coord{0, 0.5, 0, 0, 0, 0.5};
    projection.w = Coord{0.5, 0, 0, 0, 0, 0};
    CheckScene(rigs, projection);
    // the border test reads the PROJECTED coordinate (the software renderer divides first): u / w is in range everywhere, u is not
    Scene projected_border = Plain2D("Projection2D with ClampToBorder: the test is on u / w");
    projected_border.unit[0].type = 3;
    projected_border.unit[0].wrap_s = projected_border.unit[0].wrap_t = 1;
    projected_border.unit[0].border = {9, 99, 199, 250};
    projected_border.tc[0] = Coord{0, 2, 0, 0, 0, 2};
    projected_border.w = Coord{2, 0, 0, 0, 0, 0};
    projected_border.border = true;
    CheckScene(rigs, projected_border);
    // a w that varies over the target: the coordinates are 0.8125 w at every pixel, so u / w is the constant 0.8125 and every
    // pixel shows one texel; the varying attribute and the division both have to be right
    projection.name = "Projection2D, w varies with x";
    projection.tc[0] = Coord{0.40625, 0.40625, 0, 0.40625, 0.40625, 0};  // u = v = 0.8125 * w with w = 0.5 + 0.5 * x
    projection.w = Coord{0.5, 0.5, 0, 0, 0, 0};
    projection.unit[0].tex = Single(PatternLevel(8, 8, 1));
    CheckScene(rigs, projection);
}

TEST_CASE("sampling/TextureCube_selects_the_face_by_the_largest_component_and_orients_it_like_the_software_renderer") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    // Face f (+X, -X, +Y, -Y, +Z, -Z) is looked up with the direction whose dominant component is +-2 and whose other two
    // components sweep (-1, 1) along x and along y of the target.
    for (int face = 0; face < 6; ++face) {
        Scene sc;
        const char* names[6] = {"cube +X", "cube -X", "cube +Y", "cube -Y", "cube +Z", "cube -Z"};
        sc.name = names[face];
        sc.unit[0].enabled = true;
        sc.unit[0].type = 1;
        for (int f = 0; f < 6; ++f) sc.unit[0].faces[f] = Single(PatternLevel(8, 8, f + 1));
        const int axis = face / 2;
        const double sign = (face % 2) != 0 ? -2.0 : 2.0;
        std::array<Coord, 3> comp{};
        bool first_ramp = true;
        for (int k = 0; k < 3; ++k) {
            if (k == axis) {
                comp[k] = Coord{sign, 0, 0, 0, 0, 0};
            } else if (first_ramp) {
                comp[k] = Coord{-1, 2, 0, 0, 0, 0};  // along x
                first_ramp = false;
            } else {
                comp[k] = Coord{-1, 0, 2, 0, 0, 0};  // along y
            }
        }
        sc.tc[0] = Coord{comp[0].u0, comp[0].ux, comp[0].uy, comp[1].u0, comp[1].ux, comp[1].uy};
        sc.tc[1] = sc.tc[2] = kIdentity;
        sc.w = comp[2];
        CheckScene(rigs, sc);
        // a border wrap mode on the cube: the face coordinates are inside by construction, so no border shows (the direction's
        // components are 2 and the border test must not read them)
        if (face == 0 || face == 3) {
            for (u32 wrap : {1u, 5u}) {
                sc.unit[0].wrap_s = sc.unit[0].wrap_t = wrap;
                sc.unit[0].border = {250, 10, 20, 30};
                sc.border = true;
                static std::string names[2];
                names[wrap == 1 ? 0 : 1] = std::string(sc.name) + " with wrap mode " + std::to_string(wrap);
                Scene copy = sc;
                copy.name = names[wrap == 1 ? 0 : 1].c_str();
                CheckScene(rigs, copy);
            }
        }
    }
}

TEST_CASE("sampling/unit_0_Disabled_gives_zero_and_the_shadow_types_call_shadows_lookups_with_texcoord0_and_w") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    Scene disabled = Plain2D("unit 0 Disabled");
    disabled.unit[0].type = 5;
    CheckScene(rigs, disabled);
    // shadow/'s lookups are N2-12's; here a probe returns vec4(uv, w * 0.25, 1.0), so the arguments sampling/ passes show
    Scene shadow = Plain2D("unit 0 Shadow2D");
    shadow.unit[0].type = 2;
    shadow.w = Coord{2.4, 0, 0, 0, 0, 0};
    CheckScene(rigs, shadow);
    shadow.name = "unit 0 ShadowCube";
    shadow.unit[0].type = 4;
    CheckScene(rigs, shadow);
}

TEST_CASE("sampling/units_1_and_2_are_Texture2D_and_unit_2_reads_texcoord1_where_the_key_says_so") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    Scene one = Plain2D("unit 1");
    one.unit[1].enabled = true;
    one.unit[1].tex = Single(PatternLevel(8, 8, 1));
    one.read = 1;
    one.tc[1] = Coord{1, -1, 0, 0, 0, 1};  // mirrored in u
    CheckScene(rigs, one);

    Scene two = Plain2D("unit 2 on texcoord2");
    two.unit[2].enabled = true;
    two.unit[2].tex = Single(PatternLevel(8, 8, 2));
    two.read = 2;
    two.tc[1] = Coord{1, -1, 0, 0, 0, 1};
    two.tc[2] = Coord{0, 1, 0, 1, 0, -1};  // mirrored in v
    CheckScene(rigs, two);
    two.name = "unit 2 on texcoord1 (texture2_use_coord1)";
    two.coord1 = true;
    CheckScene(rigs, two);
}

TEST_CASE("sampling/unit_3_is_the_procedural_texture_when_enabled_and_vec4_zero_when_not") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    Scene on = Plain2D("unit 3 with the procedural texture on");
    on.read = 3;
    on.proctex = true;
    CheckScene(rigs, on);
    on.name = "unit 3 with the procedural texture off";
    on.proctex = false;
    CheckScene(rigs, on);
}

// ---- wrap modes ----------------------------------------------------------------------------------------------------------------------

TEST_CASE("sampling/every_wrap_mode_on_the_s_axis_the_t_axis_and_both") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    // A 4x4 texture, nearest filters, a ramp that runs from -1.15625 to 1.78125 + ... over the eight pixels: it covers a coordinate below
    // -1 (repeat of a whole width), negative inside one width, in range, and beyond 1 (clamp, border, repeat, mirror).
    const Coord s_ramp{-1.375, 3.5, 0, 0, 0, 1};
    const Coord t_ramp{0, 1, 0, -1.375, 0, 3.5};
    const Coord both{-1.375, 3.5, 0, -1.375, 0, 3.5};
    const char* mode_names[8] = {"ClampToEdge", "ClampToBorder", "Repeat", "MirroredRepeat", "ClampToEdge2", "ClampToBorder2", "Repeat2", "Repeat3"};
    for (u32 m = 0; m < 8; ++m) {
        for (int axis = 0; axis < 3; ++axis) {  // 0: s, 1: t, 2: both
            static std::string names[24];
            std::string& name = names[m * 3 + axis];
            name = std::string(mode_names[m]) + (axis == 0 ? " on s" : axis == 1 ? " on t" : " on s and t");
            Scene sc = Plain2D(name.c_str(), Single(PatternLevel(4, 4, 2)));
            UnitScene& u = sc.unit[0];
            u.border = {230, 120, 60, 200};
            if (axis != 1) u.wrap_s = m;
            if (axis != 0) u.wrap_t = m;
            sc.tc[0] = axis == 0 ? s_ramp : axis == 1 ? t_ramp : both;
            sc.border = m == 1 || m == 5;
            CheckScene(rigs, sc);
        }
    }
}

TEST_CASE("sampling/the_border_colour_has_two_paths_and_the_masked_run_takes_the_shaders") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    // The device as it is may have the capability (lavapipe does) or not (MH4N_RHI_MASK=custom_border_color in the environment,
    // or a device without it); the second backend never has it.
    CHECK_FALSE(rigs.lean->Caps().custom_border_color);
    CHECK_FALSE(rigs.lean_spec->profile.custom_border_color);

    FragmentKey key;
    key.texture.set_texture0_type(re::TextureType::Texture2D);
    key.texture.requested_wrap[0] = {re::WrapMode::ClampToBorder, re::WrapMode::ClampToEdge};
    key.texture.tev_stages[0].sources_raw = StageSources(kSourceTexture0);
    for (u32 i = 1; i < 6; ++i) key.texture.tev_stages[i].sources_raw = kPassSources;
    DeviceProfile with, without;
    with.custom_border_color = true;
    const std::string with_text = GenerateSpecialisedGlsl(key, with, ShaderPass::Normal);
    const std::string without_text = GenerateSpecialisedGlsl(key, without, ShaderPass::Normal);
    // the border test is in the shader of a device without the capability (and not in the other), in both modes' text
    CHECK(without_text.find("return tex_border_color[0];") != std::string::npos);
    CHECK(with_text.find("return tex_border_color[0];") == std::string::npos);
    const std::string uber = GenerateUberGlsl(ShaderPass::Normal, with);
    CHECK(uber.find("return tex_border_color[0];") != std::string::npos);  // ClampToBorder2's positive side is in it

    // the samplers: a border colour and ClampToBorder with the capability, clamp-to-edge and no colour without
    sampling::SamplerParams p;
    p.wrap_s = sampling::kClampToBorder;
    p.border_color = 0x44332211;
    rhi::Capabilities on, off;
    on.custom_border_color = true;
    off.custom_border_color = false;
    const rhi::SamplerDesc d_on = sampling::SamplerDescOf(p, on), d_off = sampling::SamplerDescOf(p, off);
    CHECK(d_on.address_u == rhi::AddressMode::ClampToBorder);
    REQUIRE(d_on.custom_border_color.has_value());
    CHECK((*d_on.custom_border_color)[0] == doctest::Approx(0x11 / 255.0f));
    CHECK((*d_on.custom_border_color)[3] == doctest::Approx(0x44 / 255.0f));
    CHECK(d_off.address_u == rhi::AddressMode::ClampToEdge);
    CHECK_FALSE(d_off.custom_border_color.has_value());
    // and the backend refuses a border colour without the capability (the caller asked for none)
    CHECK(rigs.lean->CreateSampler(d_off).ok());
    CHECK_FALSE(rigs.lean->CreateSampler(d_on).ok());

    // a border scene with a linear filter, where both paths agree: only pixels whose footprint is all inside or all border
    Scene sc = Plain2D("ClampToBorder with a linear filter", Single(PatternLevel(4, 4, 2)));
    sc.unit[0].wrap_s = sc.unit[0].wrap_t = 1;
    sc.unit[0].mag = sc.unit[0].min = 1;
    sc.unit[0].border = {17, 200, 90, 255};
    sc.tc[0] = Coord{-0.8, 2.75, 0, -0.8, 0, 2.75};
    sc.border = true;
    // the pixels in the half-texel band at an edge are where the two paths differ by design: they are not compared
    for (Rig* rig : {rigs.full_spec.get(), rigs.full_uber.get(), rigs.lean_spec.get(), rigs.lean_uber.get()}) {
        const std::vector<u8> pixels = rig->Render(sc);
        int compared = 0, border_pixels = 0;
        for (u32 py = 0; py < kSize; ++py) {
            for (u32 px = 0; px < kSize; ++px) {
                const double x = (px + 0.5) / kSize, y = (py + 0.5) / kSize;
                bool mixed = false;
                for (double c : {sc.tc[0].u0 + sc.tc[0].ux * x, sc.tc[0].v0 + sc.tc[0].vy * y}) {
                    const double f = std::floor(c * 4 - 0.5);
                    mixed = mixed || f == -1 || f == 3;
                }
                if (mixed) continue;
                ++compared;
                const sw::Colord want = Expect(sc, px, py, rig->CustomBorder());
                if (want[0] == 17 && want[1] == 200) ++border_pixels;
                for (int ch = 0; ch < 4; ++ch) {
                    CHECK_MESSAGE(std::fabs(pixels[(py * kSize + px) * 4 + ch] - want[ch]) <= 2,
                                  "pixel (" << px << "," << py << ") channel " << ch << " " << (rig->CustomBorder() ? "custom" : "emulated"));
                }
            }
        }
        CHECK(compared >= 20);
        CHECK(border_pixels >= 4);
    }
}

TEST_CASE("sampling/a_coordinate_of_exactly_1_is_the_border_and_exactly_0_is_texel_0") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    // The border test is `< 0 || >= 1`: the software renderer's `s >= width` at exactly 1.0 (the texel index is the width) and
    // `s < 0` false at exactly 0.0. The coordinate is a constant attribute over the target, so no interpolation can move it
    // (the scene fails if lavapipe's interpolation of a constant is not exact: the model says border or texel 0, nothing between).
    // Nearest filters; ClampToBorder (both the sampler's colour and the shader's emulation, on the second backend) and
    // ClampToBorder2 (always the shader).
    static std::string names[16];
    int n = 0;
    for (u32 mode : {1u, 5u}) {
        for (int axis = 0; axis < 2; ++axis) {
            for (double value : {1.0, 0.0}) {
                std::string& name = names[n++];
                name = "mode " + std::to_string(mode) + " on " + (axis == 0 ? "s" : "t") + " at exactly " + (value == 1.0 ? "1.0" : "0.0");
                Scene sc = Plain2D(name.c_str(), Single(PatternLevel(4, 4, 2)));
                UnitScene& u = sc.unit[0];
                u.border = {230, 120, 60, 200};
                (axis == 0 ? u.wrap_s : u.wrap_t) = mode;
                sc.tc[0] = axis == 0 ? Coord{value, 0, 0, 0, 0, 1} : Coord{0, 1, 0, value, 0, 0};
                sc.border = true;
                sc.exact = true;
                CheckScene(rigs, sc);
                // not vacuous: at 1.0 the model says border, at 0.0 a texel
                const sw::Colord at = Expect(sc, 3, 3, true);
                const bool is_border = at[0] == 230 && at[1] == 120 && at[2] == 60 && at[3] == 200;
                CHECK_MESSAGE(is_border == (value == 1.0), name);
            }
        }
    }
}

TEST_CASE("sampling/a_unit_0_type_the_pica_does_not_define_is_sampled_as_Texture2D") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    for (u32 type : {6u, 7u}) {
        static std::string names[2];
        names[type - 6] = "unit 0 type " + std::to_string(type);
        Scene sc = Plain2D(names[type - 6].c_str(), Single(PatternLevel(8, 8, 4)));
        sc.unit[0].type = type;
        sc.tc[0] = Coord{0, 1, 0, 0, 0, 1};
        sc.w = Coord{3, 0, 0, 0, 0, 0};  // a w that a Projection2D lookup would divide by: the unprojected coordinate is used
        CheckScene(rigs, sc);
    }
}

TEST_CASE("sampling/linear_filtering_with_the_wrap_modes_that_a_sampler_has") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    // 4x4, mag = min = linear; u from -0.4375 to 1.4375 in steps of 0.25: u * 4 - 0.5 = pixel - 2.25 .. a quarter-texel weight
    for (u32 m : {0u, 2u, 3u, 4u, 6u, 7u}) {
        static std::string names[8];
        names[m] = "linear filter, wrap mode " + std::to_string(m);
        Scene sc = Plain2D(names[m].c_str(), Single(PatternLevel(4, 4, 6)));
        sc.unit[0].mag = sc.unit[0].min = 1;
        sc.unit[0].wrap_s = sc.unit[0].wrap_t = m;
        sc.tc[0] = Coord{-0.4375, 2, 0, -0.4375, 0, 2};
        CheckScene(rigs, sc);
    }
}

// ---- filters, levels of detail ---------------------------------------------------------------------------------------------------------

TEST_CASE("sampling/the_mag_and_min_filters_apply_by_the_sign_of_the_level_of_detail") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    // magnified: a 4x4 texture over 8 pixels, lambda = -1; the coordinates are a quarter texel off the centres
    for (u32 mag : {0u, 1u}) {
        for (u32 min : {0u, 1u}) {
            static std::string names[8];
            names[mag * 2 + min] = "magnified, mag " + std::to_string(mag) + " min " + std::to_string(min);
            Scene sc = Plain2D(names[mag * 2 + min].c_str(), Single(PatternLevel(4, 4, 7)));
            sc.unit[0].mag = mag;
            sc.unit[0].min = min;
            CheckScene(rigs, sc);
            // minified: level 1 of a three-level 8x8 chain (lambda = 1), Repeat, a quarter texel off the centres of level 1
            static std::string names2[8];
            names2[mag * 2 + min] = "minified, mag " + std::to_string(mag) + " min " + std::to_string(min);
            Scene sm = Plain2D(names2[mag * 2 + min].c_str(), PatternChain(4));
            sm.unit[0].mag = mag;
            sm.unit[0].min = min;
            sm.unit[0].wrap_s = sm.unit[0].wrap_t = 2;
            sm.unit[0].lod_max = 2;
            sm.tc[0] = Coord{0.0625, 2, 0, 0.0625, 0, 2};
            CheckScene(rigs, sm);
        }
    }
}

TEST_CASE("sampling/the_level_comes_from_the_coordinate_derivatives_the_bias_the_clamps_and_the_mip_filter") {
    Rigs rigs(&ProbeRegistry());
    if (!rigs) return;
    struct Case {
        const char* name;
        double scale;  // the coordinate sweeps this many textures over the target: lod = log2(scale)
        int bias_raw;
        u32 lod_min, lod_max, mip;
    };
    const int minus_one = -256, half = 128, minus_half = -128;
    const Case cases[] = {
        {"lod 0", 1, 0, 0, 3, 0},
        {"lod 1", 2, 0, 0, 3, 0},
        {"lod 2", 4, 0, 0, 3, 0},
        {"lod 3", 8, 0, 0, 3, 0},
        {"lod 0 with bias +1", 1, 256, 0, 3, 0},
        {"lod 2 with bias -1", 4, minus_one, 0, 3, 0},
        {"lod 0, the level clamp min 1", 1, 0, 1, 3, 0},
        {"lod 2, the level clamp max 1", 4, 0, 0, 1, 0},
        {"lod 2, max_level 0 (the register's default)", 4, 0, 0, 0, 0},
        {"lod 0, linear mip filter, bias +0.5", 1, half, 0, 3, 1},
        {"lod 1, linear mip filter, bias -0.5", 2, minus_half, 0, 3, 1},
        {"lod 1, linear mip filter, bias +0.5", 2, half, 0, 3, 1},
        {"lod 3, linear mip filter, bias +0.5 (the last level)", 8, half, 0, 3, 1},
    };
    for (const Case& c : cases) {
        Scene sc = Plain2D(c.name, FlatChain());
        UnitScene& u = sc.unit[0];
        u.wrap_s = u.wrap_t = 2;
        u.bias_raw = c.bias_raw;
        u.lod_min = c.lod_min;
        u.lod_max = c.lod_max;
        u.mip = c.mip;
        sc.tc[0] = Coord{0, c.scale, 0, 0, 0, c.scale};
        CheckScene(rigs, sc);
    }
}

// ---- the registers: uniforms, samplers, keys ------------------------------------------------------------------------------------------

TEST_CASE("sampling/the_uniforms_come_from_the_registers_and_are_written_when_they_change") {
    pica::PicaState state;
    state.regs.fill(0);
    state.dirty.SetAll();
    const u32 border[3] = {0x80402010u, 0xFF00FF00u, 0x01020304u};
    const int bias_raw[3] = {-640, 0, 1000};  // -2.5, 0, +3.90625 levels
    state.regs[kRegMain] = 0x7;
    for (u32 u = 0; u < 3; ++u) {
        state.regs[kRegTex[u] + 0] = border[u];
        state.regs[kRegTex[u] + 3] = static_cast<u32>(bias_raw[u] & 0x1FFF) | (3u << 16);
        state.regs[kRegTex[u] + 4] = 0x1000 * (u + 1);
    }
    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    UniformDirty dirty;
    dirty.all = true;
    FillUniforms(state, block, dirty, {});
    CHECK(dirty.changed);
    CHECK(block.uniforms.tex_lod_bias.x == -2.5f);
    CHECK(block.uniforms.tex_lod_bias.y == 0.0f);
    CHECK(block.uniforms.tex_lod_bias.z == 1000 / 256.0f);
    // the colour is Azahar's ColorRGBA8 of the register word: r in bits 0-7, a in bits 24-31
    CHECK(block.uniforms.tex_border_color[0].x == doctest::Approx(0x10 / 255.0f));
    CHECK(block.uniforms.tex_border_color[0].y == doctest::Approx(0x20 / 255.0f));
    CHECK(block.uniforms.tex_border_color[0].z == doctest::Approx(0x40 / 255.0f));
    CHECK(block.uniforms.tex_border_color[0].w == doctest::Approx(0x80 / 255.0f));
    CHECK(block.uniforms.tex_border_color[2].x == doctest::Approx(0x04 / 255.0f));
    CHECK(sampling::LodBiasOf(0x1FFF) == -1.0f / 256.0f);
    CHECK(sampling::LodBiasOf(0x1000) == -16.0f);

    // nothing dirty: nothing is written and the block is not marked
    state.dirty.Reset();
    UniformDirty quiet;
    FillUniforms(state, block, quiet, {});
    CHECK_FALSE(quiet.changed);
    // one register dirty: only that unit changes
    state.regs[kRegTex[1] + 3] = static_cast<u32>(256 & 0x1FFF);
    state.dirty.Set(kRegTex[1] + 3);
    UniformDirty one;
    FillUniforms(state, block, one, {});
    CHECK(one.changed);
    CHECK(block.uniforms.tex_lod_bias.x == -2.5f);
    CHECK(block.uniforms.tex_lod_bias.y == 1.0f);
    // a unit that is off gives transparent black as its border, an enabled one with address 0 opaque black (the software
    // renderer returns those before the border test)
    state.regs[kRegMain] = 0x5;
    state.dirty.Set(kRegMain);
    UniformDirty off;
    FillUniforms(state, block, off, {});
    CHECK(off.changed);
    CHECK(block.uniforms.tex_border_color[1].w == 0.0f);
    CHECK(block.uniforms.tex_border_color[1].x == 0.0f);
    CHECK(block.uniforms.tex_border_color[0].w == doctest::Approx(0x80 / 255.0f));
    state.regs[kRegTex[2] + 4] = 0;
    state.dirty.Set(kRegTex[2] + 4);
    UniformDirty no_address;
    FillUniforms(state, block, no_address, {});
    CHECK(block.uniforms.tex_border_color[2].w == 1.0f);
    CHECK(block.uniforms.tex_border_color[2].x == 0.0f);
    // a dirty bit whose value is the same marks nothing
    state.dirty.Set(kRegTex[1] + 3);
    state.dirty.Reset();
    state.dirty.Set(kRegTex[1] + 3);
    UniformDirty same;
    FillUniforms(state, block, same, {});
    CHECK_FALSE(same.changed);
}

TEST_CASE("sampling/the_sampler_follows_the_filter_wrap_and_lod_registers_by_the_tables") {
    // pica_to_vk.h's wrap table, with the two differences the feature owns (4 and 5 are clamp-to-edge samplers, 1 is clamp-to-edge
    // without the capability)
    rhi::Capabilities on, off;
    on.custom_border_color = true;
    off.custom_border_color = false;
    using A = rhi::AddressMode;
    const A with[8] = {A::ClampToEdge, A::ClampToBorder, A::Repeat, A::MirroredRepeat, A::ClampToEdge, A::ClampToEdge, A::Repeat, A::Repeat};
    const A without[8] = {A::ClampToEdge, A::ClampToEdge, A::Repeat, A::MirroredRepeat, A::ClampToEdge, A::ClampToEdge, A::Repeat, A::Repeat};
    for (u32 m = 0; m < 8; ++m) {
        sampling::SamplerParams p;
        p.wrap_s = m;
        p.wrap_t = 7 - m;
        const rhi::SamplerDesc a = sampling::SamplerDescOf(p, on), b = sampling::SamplerDescOf(p, off);
        CHECK_MESSAGE(a.address_u == with[m], "s mode " << m);
        CHECK_MESSAGE(a.address_v == with[7 - m], "t mode " << 7 - m);
        CHECK_MESSAGE(b.address_u == without[m], "s mode " << m);
        CHECK_MESSAGE(b.address_v == without[7 - m], "t mode " << 7 - m);
    }
    sampling::SamplerParams p;
    p.mag_filter = 1;
    p.min_filter = 0;
    p.mip_filter = 1;
    p.lod_min = 2;
    p.lod_max = 1;  // min above max: raised
    const rhi::SamplerDesc d = sampling::SamplerDescOf(p, on);
    CHECK(d.mag_filter == rhi::Filter::Linear);
    CHECK(d.min_filter == rhi::Filter::Nearest);
    CHECK(d.mip_filter == rhi::Filter::Linear);
    CHECK(d.min_lod == 2.0f);
    CHECK(d.max_lod == 2.0f);
    CHECK(d.mip_lod_bias == 0.0f);  // the bias is the shader's tex_lod_bias
    CHECK(d.max_anisotropy == 1.0f);
    CHECK_FALSE(d.custom_border_color.has_value());

    // out of the registers: border word, filters (bit 1 mag, bit 2 min, bit 24 mip), wraps (t 8-10, s 12-14), lod (max 16-19, min 24-27)
    std::array<u32, 0x300> regs{};
    regs[0x91 + 0] = 0xAABBCCDD;
    regs[0x91 + 2] = (1u << 1) | (1u << 24) | (3u << 8) | (5u << 12);
    regs[0x91 + 3] = (7u << 16) | (2u << 24);
    const sampling::SamplerParams q = sampling::SamplerParamsOf(regs.data(), 1);
    CHECK(q.border_color == 0xAABBCCDD);
    CHECK(q.mag_filter == 1);
    CHECK(q.min_filter == 0);
    CHECK(q.mip_filter == 1);
    CHECK(q.wrap_t == 3);
    CHECK(q.wrap_s == 5);
    CHECK(q.lod_max == 7);
    CHECK(q.lod_min == 2);
}

TEST_CASE("sampling/sampler_objects_are_made_on_first_use_and_cached_by_value") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    sampling::SamplerCache cache(backend);
    CHECK(cache.size() == 0);  // nothing made by the constructor
    sampling::SamplerParams a;
    a.mag_filter = 1;
    auto first = cache.Get(a);
    REQUIRE_MESSAGE(first.ok(), first.error());
    auto again = cache.Get(a);
    REQUIRE(again.ok());
    CHECK(first->get() == again->get());
    CHECK(cache.creates() == 1);
    CHECK(cache.hits() == 1);
    // a border colour that no ClampToBorder axis uses is not part of the value
    sampling::SamplerParams b = a;
    b.border_color = 0x11223344;
    CHECK(cache.Get(b)->get() == first->get());
    // another filter is another sampler
    sampling::SamplerParams c = a;
    c.min_filter = 1;
    CHECK(cache.Get(c)->get() != first->get());
    // the border colour of a ClampToBorder axis is part of the value where the device takes one
    sampling::SamplerParams d = a, e = a;
    d.wrap_s = e.wrap_s = sampling::kClampToBorder;
    d.border_color = 1;
    e.border_color = 2;
    const bool differ = cache.Get(d)->get() != cache.Get(e)->get();
    CHECK(differ == backend->Caps().custom_border_color);
    // the sampler cache made on a thread other than the one that draws (the capture player's --queue) makes its objects where Get is called
    std::shared_ptr<rhi::Sampler> from_thread;
    std::thread other([&] {
        sampling::SamplerCache c2(backend);
        sampling::SamplerParams p;
        p.wrap_s = sampling::kRepeat;
        auto r = c2.Get(p);
        if (r.ok()) from_thread = *r;
    });
    other.join();
    CHECK(from_thread != nullptr);
}

TEST_CASE("sampling/nothing_is_reported_as_a_stub_for_a_draw_that_reads_the_texture_units") {
    FragmentKey key;
    key.texture.set_texture0_type(re::TextureType::Texture2D);
    for (u32 source : {3u, 4u, 5u}) {
        key = FragmentKey{};
        key.texture.tev_stages[0].sources_raw = StageSources(source);
        for (u32 i = 1; i < 6; ++i) key.texture.tev_stages[i].sources_raw = kPassSources;
        const FeatureModule* sampling_feature = FeatureRegistry::Global().Find("sampling");
        REQUIRE(sampling_feature != nullptr);
        CHECK(sampling_feature->Stubbed(key) == nullptr);
        std::array<const char*, 8> names{};
        const size_t n = StubbedFeatures(key, FeatureRegistry::Global(), names);
        for (size_t i = 0; i < n; ++i) CHECK(std::string(names[i]) != "sampling");
        CHECK(sampling_feature->NeedsOf(key, DeviceProfile{}).texture_units == source - 2);
    }
    // a bump map reads sampleTexUnit{n}() too, and that is real now
    FragmentKey lit;
    lit.lighting.set_enable(1);
    lit.lighting.set_bump_mode(re::LightingBumpMode::NormalMap);
    CHECK(FeatureRegistry::Global().Find("sampling")->Stubbed(lit) == nullptr);
}

TEST_CASE("sampling/the_generated_text_routes_unit_3_and_unit_0_Disabled_as_azahars_generator_does") {
    FragmentKey key;
    key.texture.tev_stages[0].sources_raw = StageSources(6);
    for (u32 i = 1; i < 6; ++i) key.texture.tev_stages[i].sources_raw = kPassSources;
    DeviceProfile profile;
    std::string text = GenerateSpecialisedGlsl(key, profile, ShaderPass::Normal);
    CHECK(text.find("vec4 sampleTexUnit3() {\nreturn vec4(0.0);\n}") != std::string::npos);
    key.proctex.set_enable(1);
    text = GenerateSpecialisedGlsl(key, profile, ShaderPass::Normal);
    CHECK(text.find("vec4 sampleTexUnit3() {\nreturn ProcTex();\n}") != std::string::npos);
    key.texture.set_texture0_type(re::TextureType::Disabled);
    text = GenerateSpecialisedGlsl(key, profile, ShaderPass::Normal);
    CHECK(text.find("vec4 sampleTexUnit0() {\nreturn vec4(0.0);\n}") != std::string::npos);
    // Azahar's lookups: textureLod with getLod and the bias for the 2D units, textureProj for the projection, a cube lookup on
    // (texcoord0, texcoord0_w)
    key.texture.set_texture0_type(re::TextureType::Texture2D);
    text = GenerateSpecialisedGlsl(key, profile, ShaderPass::Normal);
    CHECK(text.find("textureLod(sampler2D(tex0, samp0), tcw, lod)") != std::string::npos);
    CHECK(text.find("getLod(tc * vec2(textureSize(tex2, 0))) + tex_lod_bias[2]") != std::string::npos);
    key.texture.set_texture0_type(re::TextureType::Projection2D);
    CHECK(GenerateSpecialisedGlsl(key, profile, ShaderPass::Normal).find("textureProj(sampler2D(tex0, samp0), vec3(tc, texcoord0_w))") != std::string::npos);
    key.texture.set_texture0_type(re::TextureType::TextureCube);
    CHECK(GenerateSpecialisedGlsl(key, profile, ShaderPass::Normal).find("texture(samplerCube(tex0_cube, samp0), vec3(texcoord0, texcoord0_w))") != std::string::npos);
    // both modes compile for these keys
    for (re::TextureType type : {re::TextureType::Texture2D, re::TextureType::TextureCube, re::TextureType::Shadow2D}) {
        key.texture.set_texture0_type(type);
        const Generated generated = GenerateSpecialised(key, profile, ShaderPass::Normal);
        CHECK_MESSAGE(generated.ok(), generated.error);
    }
    const Generated uber = GenerateUber(ShaderPass::Normal, profile);
    CHECK_MESSAGE(uber.ok(), uber.error);
}

// ---- `--gpu native`: the textures of a draw from guest memory ----------------------------------------------------------------------

namespace {

constexpr u32 kFcram = mh4n::gpu_capture::kFcramPaddr;
constexpr u32 kVertexBase = kFcram + 0x100000;
constexpr u32 kTextureBase = kFcram + 0x200000;
constexpr u32 kColorBuffer = mh4n::gpu_capture::kVramPaddr + 0x10000;
constexpr u32 kListAddress = kFcram + 0x1000;
constexpr u32 kRegUseGs = 0x229, kRegTopology = 0x25E, kRegOutTotal = 0x4F, kRegOutMap = 0x50;
constexpr u32 kRegColorAddress = 0x11D, kRegColorFormat = 0x117, kRegDepthFormat = 0x116, kRegFbSize = 0x11E;
constexpr u32 kVs = 0x2B0;

namespace pi {
// nihstro's field positions: opcode 26-31, destination 21-25, source 1 12-18, source 2 7-11, operand descriptor 0-6.
constexpr u32 MOV = 0x13, END = 0x22;
constexpr u32 V(u32 n) { return n; }
constexpr u32 C(u32 n) { return 0x20 + n; }
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
    CmdList& Uniforms(u32 base, u32 first, const std::vector<std::array<float, 4>>& values) {
        Write(base + 0x10, first | (1u << 31));
        for (const auto& v : values) {
            for (int c = 3; c >= 0; --c) {
                u32 bits;
                std::memcpy(&bits, &v[static_cast<size_t>(c)], 4);
                Write(base + 0x11, bits);
            }
        }
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

// The registers every draw of these cases shares: an 8x8 RGBA8 colour buffer, a viewport over it, no depth, no blending, stage 0
// replacing with texture unit 0, the texture unit as the arguments say.
struct TextureRegs {
    u32 wrap_s = 0, wrap_t = 0, mag = 0, min = 0;
    u32 border = 0;
    u32 type = 0;
    u32 lod = 0;  // the LOD word (max_level in bits 16-19)
};
void DrawRegisters(CmdList& l, const TextureRegs& t) {
    l.Write(kRegColorAddress, kColorBuffer >> 3);
    l.Write(kRegColorFormat, 0);
    l.Write(kRegDepthFormat, 0);
    l.Write(0x11C, 0);
    l.Write(kRegFbSize, kSize | ((kSize - 1) << 12) | (1u << 24));
    l.Write(0x113, 0xF);
    l.Write(0x115, 0);
    l.Write(0x100, 0);
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
        l.Write(base + 0, base == 0xC0 ? StageSources(kSourceTexture0) : kPassSources);
        l.Write(base + 1, 0);
        l.Write(base + 2, 0);
    }
    // texture unit 0: enabled, Texture2D, 8x8 RGBA8 at kTextureBase
    l.Write(kRegMain, 1);
    l.Write(0x81, t.border);
    l.Write(0x82, kSize | (kSize << 16));
    l.Write(0x83, (t.mag << 1) | (t.min << 2) | (t.wrap_t << 8) | (t.wrap_s << 12) | (t.type << 28));
    l.Write(0x84, t.lod);
    l.Write(0x85, kTextureBase >> 3);
    l.Write(0x8E, 0);
}

// The vertex program: o0 = v0 (position), o1 = v1 (colour), o2 = c0 (the texture coordinate, a constant).
void VertexProgram(CmdList& l, std::array<float, 2> texcoord, u32 vertex_count = 3) {
    using namespace pi;
    const std::vector<u32> code = {Ins(MOV, O(0), V(0), 0), Ins(MOV, O(1), V(1), 0), Ins(MOV, O(2), C(0), 0), kEnd};
    l.Program(kVs, code, {IdentityDescriptor()});
    l.Uniforms(kVs, 0, {{texcoord[0], texcoord[1], 0, 0}});
    l.Write(kVs + 0x09, 1);
    l.Write(kVs + 0x0A, 0);
    l.Write(kVs + 0x0B, 0x76543210u);
    l.Write(kVs + 0x0C, 0xFEDCBA98u);
    l.Write(kVs + 0x0D, 0x7);
    l.Write(kRegOutTotal, 3);
    using namespace shader::semantic;
    auto map = [](u32 x, u32 y, u32 z, u32 w) { return x | (y << 8) | (z << 16) | (w << 24); };
    l.Write(kRegOutMap + 0, map(kPositionX, kPositionY, kPositionZ, kPositionW));
    l.Write(kRegOutMap + 1, map(kColorR, kColorG, kColorB, kColorA));
    l.Write(kRegOutMap + 2, map(kTexcoord0U, kTexcoord0V, kInvalid, kInvalid));
    l.Write(0x24A, 2);
    l.Write(0x251, 2);
    l.Write(0x200, (kVertexBase >> 4) << 1);
    l.Write(0x201, 0xFFu);
    l.Write(0x202, 1u << 28);
    l.Write(0x203, 0);
    l.Write(0x204, 0x10u);
    l.Write(0x205, (32u << 16) | (2u << 28));
    l.Write(kRegUseGs, 0);
    l.Write(kRegTopology, 0);
    l.Write(0x228, vertex_count).Write(0x22A, 0).Write(0x22E, 0);
}

std::vector<u8> FloatBytes(const std::vector<std::array<float, 4>>& vectors) {
    std::vector<u8> bytes(vectors.size() * 16);
    std::memcpy(bytes.data(), vectors.data(), bytes.size());
    return bytes;
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
    std::vector<u8> ColorBuffer() {
        gpu->WriteBackColorBuffers();
        const u8* memory = host.PhysicalPointer(kColorBuffer, kSize * kSize * 4);
        REQUIRE(memory != nullptr);
        std::vector<u8> rgba;
        REQUIRE(texture::DecodeToRgba8(memory, kSize * kSize * 4, kSize, kSize, texture::Format::RGBA8, rgba));
        return rgba;
    }
    u64 Counter(const std::string& name) {
        for (const mh4n::gpu::Counter& c : gpu->PeekCounters()) {
            if (c.name == name) return c.value;
        }
        FAIL("no counter " << name);
        return 0;
    }
    // The texture memory of an 8x8 RGBA8 texture holding `texels` (the decoder's order), written by inverting the decoder: a probe
    // whose byte i is i tells which byte of the tile each channel of each texel is.
    void WriteTexture(const std::vector<sw::Color>& texels) {
        std::vector<u8> probe(256);
        for (size_t i = 0; i < 256; ++i) probe[i] = static_cast<u8>(i);
        std::vector<u8> decoded;
        REQUIRE(texture::DecodeToRgba8(probe.data(), probe.size(), 8, 8, texture::Format::RGBA8, decoded));
        std::vector<u8> memory(256);
        for (size_t t = 0; t < 64; ++t) {
            for (int c = 0; c < 4; ++c) memory[decoded[t * 4 + c]] = static_cast<u8>(texels[t][c]);
        }
        Write(kTextureBase, memory);
    }
};

std::array<int, 4> At(const std::vector<u8>& rgba, u32 x, u32 y) {
    const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

bool Uniform(const std::vector<u8>& rgba, std::array<int, 4> want, std::string* why) {
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            if (At(rgba, x, y) != want) {
                *why = "pixel (" + std::to_string(x) + "," + std::to_string(y) + ") is " + std::to_string(At(rgba, x, y)[0]) + "," +
                       std::to_string(At(rgba, x, y)[1]) + "," + std::to_string(At(rgba, x, y)[2]) + "," + std::to_string(At(rgba, x, y)[3]);
                return false;
            }
        }
    }
    return true;
}

std::vector<u8> Triangle() {
    const std::array<float, 4> c = {1, 1, 1, 1};
    return FloatBytes({{-1, -1, -0.5f, 1}, c, {3, -1, -0.5f, 1}, c, {-1, 3, -0.5f, 1}, c});
}

std::vector<sw::Color> Pattern8() {
    std::vector<sw::Color> texels;
    for (const sw::Color& c : PatternLevel(8, 8, 9).texel) texels.push_back(c);
    return texels;
}

}  // namespace

TEST_CASE("native/a_draw_binds_the_texture_its_registers_name_from_guest_memory") {
    if (!test::RequireBackend()) return;
    Sim sim;
    const std::vector<sw::Color> texels = Pattern8();
    sim.WriteTexture(texels);
    sim.Write(kVertexBase, Triangle());
    TextureRegs regs;
    CmdList first;
    DrawRegisters(first, regs);
    VertexProgram(first, {3.5f / 8, 5.5f / 8});  // texel centre (3, 5) of the picture
    sim.Submit(first);
    // the software renderer: picture row 5 is the decoder's row 8 - 1 - 5 = 2
    const sw::Color want = texels[2 * 8 + 3];
    std::string why;
    CHECK_MESSAGE(Uniform(sim.ColorBuffer(), {want[0], want[1], want[2], want[3]}, &why), why);
    CHECK(sim.Counter("native_draws") == 1);
    CHECK(sim.Counter("native_draws_dropped") == 0);
    CHECK(sim.Counter("stubbed:sampling") == 0);
    CHECK(sim.Counter("texture_binds") == 1);
    CHECK(sim.Counter("texture_uploads") == 1);
    CHECK(sim.Counter("texture_unavailable") == 0);
    CHECK(sim.Counter("sampler_creates") == 1);

    // another texel, the same texture: not uploaded again, the sampler is the same
    CmdList second;
    second.Uniforms(kVs, 0, {{6.5f / 8, 0.5f / 8, 0, 0}});
    second.Write(0x22E, 0);
    sim.Submit(second);
    const sw::Color want2 = texels[7 * 8 + 6];  // picture row 0 is the decoder's row 7
    CHECK_MESSAGE(Uniform(sim.ColorBuffer(), {want2[0], want2[1], want2[2], want2[3]}, &why), why);
    CHECK(sim.Counter("texture_binds") == 2);
    CHECK(sim.Counter("texture_uploads") == 1);
    CHECK(sim.Counter("sampler_creates") == 1);

    // the CPU writes the texture and tells the GPU (a flush): the next draw sees the new contents
    std::vector<sw::Color> changed = texels;
    for (sw::Color& c : changed) c = {255 - c[0], 255 - c[1], 255 - c[2], c[3]};
    sim.WriteTexture(changed);
    sim.gpu->FlushRegion(0x14000000, 256);
    sim.Submit(second);
    const sw::Color want3 = changed[7 * 8 + 6];
    CHECK_MESSAGE(Uniform(sim.ColorBuffer(), {want3[0], want3[1], want3[2], want3[3]}, &why), why);
    CHECK(sim.Counter("texture_uploads") == 2);

    // mip levels: a level-0 texture counted nothing; a draw whose unit has max_level 1 is counted (the level is not uploaded: the
    // picture is level 0), and it is a plain counter, not a stub
    CHECK(sim.Counter("texture_levels_missing") == 0);
    TextureRegs with_levels;
    with_levels.lod = 1u << 16;
    CmdList levels;
    DrawRegisters(levels, with_levels);
    levels.Write(0x22E, 0);
    sim.Submit(levels);
    CHECK(sim.Counter("texture_levels_missing") == 1);
    CHECK(sim.Counter("stubbed:sampling") == 0);
}

TEST_CASE("native/a_unit_0_type_the_pica_does_not_define_binds_its_texture_as_Texture2D") {
    if (!test::RequireBackend()) return;
    Sim sim;
    const std::vector<sw::Color> texels = Pattern8();
    sim.WriteTexture(texels);
    sim.Write(kVertexBase, Triangle());
    TextureRegs regs;
    regs.type = 6;
    CmdList l;
    DrawRegisters(l, regs);
    VertexProgram(l, {3.5f / 8, 5.5f / 8});
    sim.Submit(l);
    const sw::Color want = texels[2 * 8 + 3];
    std::string why;
    CHECK_MESSAGE(Uniform(sim.ColorBuffer(), {want[0], want[1], want[2], want[3]}, &why), why);
    CHECK(sim.Counter("texture_binds") == 1);
    CHECK(sim.Counter("texture_unavailable") == 0);
}

TEST_CASE("native/a_wrap_mode_and_a_unit_that_is_off_show_what_the_software_renderer_shows") {
    if (!test::RequireBackend()) return;
    Sim sim;
    const std::vector<sw::Color> texels = Pattern8();
    sim.WriteTexture(texels);
    sim.Write(kVertexBase, Triangle());
    std::string why;
    // Repeat: u = 1.0 + 2.5 / 8 is texel 2 (the decoder's row 8 - 1 - 4 for v = 4.5 / 8)
    TextureRegs repeat;
    repeat.wrap_s = repeat.wrap_t = 2;
    CmdList l;
    DrawRegisters(l, repeat);
    VertexProgram(l, {1.0f + 2.5f / 8, 4.5f / 8});
    sim.Submit(l);
    const sw::Color want = texels[3 * 8 + 2];
    CHECK_MESSAGE(Uniform(sim.ColorBuffer(), {want[0], want[1], want[2], want[3]}, &why), why);

    // ClampToBorder: beyond the edge the register's colour (red 0x10, green 0x20, blue 0x40, alpha 0x80)
    TextureRegs border;
    border.wrap_s = border.wrap_t = 1;
    border.border = 0x80402010u;
    CmdList b;
    DrawRegisters(b, border);
    b.Uniforms(kVs, 0, {{1.5f, 0.5f, 0, 0}});
    b.Write(0x22E, 0);
    sim.Submit(b);
    CHECK_MESSAGE(Uniform(sim.ColorBuffer(), {0x10, 0x20, 0x40, 0x80}, &why), why);

    // unit 0 off: transparent black
    CmdList off;
    off.Write(kRegMain, 0);
    off.Write(0x22E, 0);
    sim.Submit(off);
    CHECK_MESSAGE(Uniform(sim.ColorBuffer(), {0, 0, 0, 0}, &why), why);
    CHECK(sim.Counter("stubbed:sampling") == 0);
}

TEST_CASE("native/the_gpu_made_on_one_thread_makes_its_images_and_samplers_on_the_thread_that_executes") {
    if (!test::RequireBackend()) return;
    std::unique_ptr<Sim> sim;
    std::thread maker([&] { sim = std::make_unique<Sim>(); });  // the capture player's --queue builds the GPU on another thread
    maker.join();
    const std::vector<sw::Color> texels = Pattern8();
    sim->WriteTexture(texels);
    sim->Write(kVertexBase, Triangle());
    CmdList l;
    DrawRegisters(l, TextureRegs{});
    VertexProgram(l, {0.5f / 8, 0.5f / 8});
    std::thread renderer([&] { sim->Submit(l); sim->gpu->WriteBackColorBuffers(); });
    renderer.join();
    const sw::Color want = texels[7 * 8 + 0];
    std::string why;
    const u8* memory = sim->host.PhysicalPointer(kColorBuffer, kSize * kSize * 4);
    REQUIRE(memory != nullptr);
    std::vector<u8> rgba;
    REQUIRE(texture::DecodeToRgba8(memory, kSize * kSize * 4, kSize, kSize, texture::Format::RGBA8, rgba));
    CHECK_MESSAGE(Uniform(rgba, {want[0], want[1], want[2], want[3]}, &why), why);
    CHECK(sim->Counter("texture_binds") == 1);
}

// ---- N2-41: a colour target loaded from guest memory is written back in the rows it was loaded from ----------------------------

namespace {

constexpr u32 kReloadSize = 16;  // the target is 16x16: two tile rows and two tile columns

u32 ReloadBytesPerPixel(u32 format) { return format == 0 ? 4 : (format == 1 ? 3 : 2); }

// The registers of one draw into a kReloadSize square colour buffer of `format` at kColorBuffer: bit 24 of the size register set,
// a viewport over the whole target, no depth, no blending, stage 0 replacing with the primary colour, the texture units off.
void ReloadRegisters(CmdList& l, u32 format) {
    l.Write(kRegColorAddress, kColorBuffer >> 3);
    l.Write(kRegColorFormat, format << 16);
    l.Write(kRegDepthFormat, 0);
    l.Write(0x11C, 0);
    l.Write(kRegFbSize, kReloadSize | ((kReloadSize - 1) << 12) | (1u << 24));
    l.Write(0x113, 0xF);
    l.Write(0x115, 0);
    l.Write(0x100, 0);
    l.Write(0x102, 3);
    l.Write(0x107, 0xF00);
    l.Write(0x104, 0);
    l.Write(0x1C6, 1);
    l.Write(0x41, F24(kReloadSize / 2.0f));
    l.Write(0x43, F24(kReloadSize / 2.0f));
    l.Write(0x68, 0);
    l.Write(0x4D, F24(-1.0f));
    l.Write(0x4E, 0);
    l.Write(0x6D, 1);
    l.Write(0x65, 0);
    l.Write(0x40, 0);
    for (u32 base : {0xC0u, 0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u}) {
        l.Write(base + 0, base == 0xC0 ? StageSources(0) : kPassSources);  // source 0: the primary colour
        l.Write(base + 1, 0);
        l.Write(base + 2, 0);
    }
    l.Write(kRegMain, 0);
}

}  // namespace

TEST_CASE("native/a_colour_target_loaded_from_guest_memory_is_written_back_in_its_own_rows_for_every_colour_format") {
    if (!test::RequireBackend()) return;
    constexpr u32 h = kReloadSize;
    // The window rectangle the draw covers: columns [x0, x1), rows [y0, y1) of the window (y up from the bottom, as the viewport's
    // corner and the vertices' clip coordinates say). The software renderer's DrawPixel puts window row y into row
    // framebuffer.height - y of the buffer, with the register holding h - 1 as the height: so row h - 1 - y of the decoded picture,
    // which keeps the memory's order (row 0 is the first row of tiles). Rows [y0, y1) = [2, 7) land in decoded rows [h - 7, h - 2)
    // = [9, 14).
    constexpr u32 x0 = 3, x1 = 11, y0 = 2, y1 = 7;
    const char* names[5] = {"RGBA8", "RGB8", "RGB5A1", "RGB565", "RGBA4"};
    for (u32 format = 0; format < 5; ++format) {
        CAPTURE(std::string(names[format]));
        Sim sim;
        const u32 bytes = kReloadSize * kReloadSize * ReloadBytesPerPixel(format);
        // a pattern whose rows all differ (a linear congruential sequence: no two rows or pixels repeat)
        std::vector<u8> pattern(bytes);
        u32 seed = 0x2468ACE1u + format;
        for (u8& b : pattern) {
            seed = seed * 1664525u + 1013904223u;
            b = static_cast<u8>(seed >> 24);
        }
        sim.Write(kColorBuffer, pattern);
        std::vector<u8> before;
        REQUIRE(texture::DecodeToRgba8(pattern.data(), bytes, kReloadSize, kReloadSize, static_cast<texture::Format>(format), before));

        // two triangles over the rectangle, in red: clip x = 2 x / 16 - 1, clip y = 2 y / 16 - 1
        auto cx = [](u32 x) { return 2.0f * static_cast<float>(x) / kReloadSize - 1.0f; };
        const std::array<float, 4> red = {1, 0, 0, 1};
        const std::array<float, 4> a = {cx(x0), cx(y0), -0.5f, 1}, b = {cx(x1), cx(y0), -0.5f, 1};
        const std::array<float, 4> c = {cx(x1), cx(y1), -0.5f, 1}, d = {cx(x0), cx(y1), -0.5f, 1};
        sim.Write(kVertexBase, FloatBytes({a, red, b, red, c, red, a, red, c, red, d, red}));
        CmdList l;
        ReloadRegisters(l, format);
        VertexProgram(l, {0, 0}, 6);
        sim.Submit(l);
        sim.gpu->WriteBackColorBuffers();

        CHECK(sim.Counter("native_draws") == 1);
        CHECK(sim.Counter("native_draws_dropped") == 0);
        const u8* memory = sim.host.PhysicalPointer(kColorBuffer, bytes);
        REQUIRE(memory != nullptr);
        std::vector<u8> after;
        REQUIRE(texture::DecodeToRgba8(memory, bytes, kReloadSize, kReloadSize, static_cast<texture::Format>(format), after));
        u32 wrong = 0;
        std::string first;
        for (u32 row = 0; row < h; ++row) {
            for (u32 col = 0; col < kReloadSize; ++col) {
                const size_t i = (static_cast<size_t>(row) * kReloadSize + col) * 4;
                const bool covered = col >= x0 && col < x1 && row >= h - y1 && row < h - y0;
                const std::array<int, 4> got = {after[i], after[i + 1], after[i + 2], after[i + 3]};
                const std::array<int, 4> want = covered ? std::array<int, 4>{255, 0, 0, 255}
                                                        : std::array<int, 4>{before[i], before[i + 1], before[i + 2], before[i + 3]};
                if (got != want && wrong++ == 0) {
                    first = "decoded row " + std::to_string(row) + " column " + std::to_string(col) + (covered ? " (covered)" : " (untouched)");
                }
            }
        }
        CHECK_MESSAGE(wrong == 0, wrong << " pixels differ, the first at " << first);
    }
}
