// Derived from Azahar src/video_core/renderer_software/sw_lighting.cpp (ComputeFragmentsColors and LookupLightingLut: the
// formulas the CPU oracle below ports), sw_clipper.cpp (FlipQuaternionIfOpposite) and sw_rasterizer.cpp (where the flip is applied
// and how the quaternion, the view vector and the texture colours reach ComputeFragmentsColors), src/video_core/pica/regs_lighting.h
// (the register layout of the lights and the lighting block, the sampler numbers, IsLightingSamplerSupported and GetScale),
// src/video_core/pica/regs_texturing.h and regs_framebuffer.h and regs_rasterizer.h and regs_internal.h (the positions of the TEV
// stage, output-merger, depth-range and vertex-output registers the base state writes), src/video_core/pica/pica_core.h
// (Lighting::LutEntry: the bit fields of a table entry), src/video_core/pica_types.h (the 16-bit, 20-bit and 24-bit float layouts
// the helpers below encode), src/video_core/rasterizer_accelerated.cpp (the light uniforms: a colour over 255, the spot vector over
// 2047), src/common/quaternion.h (QuaternionRotate and the quaternion's Normalized), src/common/vector_math.h (the Vec3 operations the
// oracle restates), src/video_core/shader/generator/glsl_fs_shader_gen.cpp and glsl_shader_gen.cpp (the text of the sample calls and
// of the fixed geometry shader that some cases look for) and externals/nihstro/include/nihstro/shader_bytecode.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (the first group is GPL-2.0-or-later: each says "Licensed under GPLv2 or any later
// version", except glsl_fs_shader_gen.cpp, "Licensed under GPLv3 or any later version"; shader_bytecode.h belongs to the submodule
// externals/nihstro that Azahar pins at f4d8659decbfe5d234f04134b5002b82dc515a44 and is NOT GPL: its licence is the BSD 3-clause
// licence of externals/nihstro/license.txt, reproduced below because it asks for that: the instruction encoder below places the
// fields where shader_bytecode.h puts them). So this file is GPL-3.0-or-later.
// Copyright 2017 Citra Emulator Project (sw_lighting.cpp, regs_lighting.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (sw_clipper.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp, pica_types.h)
// Copyright 2016 Citra Emulator Project (quaternion.h)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (vector_math.h; it carries the same "Copyright 2014 Tony
//   Wasserka" BSD-3-Clause notice as shader_bytecode.h, reproduced below)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h, regs_internal.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, rasterizer_accelerated.cpp,
//   glsl_fs_shader_gen.cpp, glsl_shader_gen.cpp)
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
// Fragment lighting (N2-09), on whatever Vulkan device the machine has (MH4N_VULKAN_DEVICE; a case that needs one skips with a
// message without it and fails under MH4N_REQUIRE_VULKAN=1).
//
// THE ORACLE. `sw::ComputeFragmentsColors` below is a CPU port of the SOFTWARE renderer's function of that name (float arithmetic,
// the tables' {value, difference} pairs read from the raw entries, the results cut to bytes), reading the lighting registers of a
// PicaState itself; nothing in it calls the generator or reads its text. The generator follows Azahar's GLSL, so the two agree
// within the rounding of the byte write (the software renderer truncates, the shader's write rounds) plus float noise: 3 steps.
// Every signed table lookup of the reference records how far its input was from zero, where the signed tables jump (`g_margin`): a
// case whose data puts an input within 1e-3 of it fails, so the fixtures cannot become flaky on another device.
//
// WHAT RUNS WHERE. Every lit draw is rendered in the specialised shader and in the ubershader (they must agree within one step), on
// the CPU-batch path (the trivial vertex stage; the quaternion fix-up is done by the batch, N2-37) and on the translated path (a
// vertex program run through N2-06's translator, so that the fix-up is the fixed geometry shader). The quaternion cases run on the
// device as it is and on a second backend with `geometry_shader` masked: a draw whose quaternions need the flip is expected to
// differ there, and the test asserts that it does. The whole file also passes under MH4N_RHI_MASK=geometry_shader.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gpu/cpuvertex/cpu_vertex.h"
#include "gpu/cpuvertex/triangle_batch.h"
#include "gpu/fbops/fixed_function.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"
#include "gpu/fragment/key.h"
#include "gpu/fragment/lighting/lighting.h"
#include "gpu/fragment/profile.h"
#include "gpu/fragment/sampling/sampling.h"
#include "gpu/fragment/state.h"
#include "gpu/fragment/tables.h"
#include "gpu/fragment/uniforms.h"
#include "gpu/native/bindings.h"
#include "gpu/native/compose.h"
#include "gpu/native/default_provider.h"
#include "gpu/native/pipeline_builder.h"
#include "gpu/pica/regs.h"
#include "gpu/pica/state.h"
#include "gpu/shader/vertex_shader.h"
#include "support/vulkan_device.h"

using namespace mh4n;
using namespace mh4n::gpu::fragment;
namespace re = mh4n::gpu::fragment::regenum;
namespace native = mh4n::gpu::native;
namespace cpuvertex = mh4n::gpu::cpuvertex;
namespace fbops = mh4n::gpu::fbops;
namespace pica = mh4n::gpu::pica;
namespace shader = mh4n::gpu::shader;
namespace rhi = mh4n::gpu::rhi;
namespace test = mh4n::test;
namespace lighting_gen = mh4n::gpu::fragment::lighting;

namespace {

constexpr u32 kSize = 8;  // the targets are 8x8

// ---- registers (regs_lighting.h and the neighbours; PicaState::regs word indices) --------------------------------------------

constexpr u32 kRegDepthRange = 0x4D, kRegDepthNear = 0x4E, kRegDepthmap = 0x6D;
constexpr u32 kRegTexture0 = 0x81;  // unit 0's five words: border colour, size, params (type 28-30), lod, address
constexpr u32 kRegTev0 = 0xC0;
constexpr u32 kRegLogicOp = 0x102, kRegDepthColorMask = 0x107, kRegAllowColor = 0x113, kRegAllowDepth = 0x115;
constexpr u32 kRegLight0 = 0x140, kRegLightStride = 0x10;
constexpr u32 kRegGlobalAmbient = 0x1C0, kRegMaxLightIndex = 0x1C2, kRegConfig0 = 0x1C3, kRegConfig1 = 0x1C4, kRegLightingDisable = 0x1C6;
constexpr u32 kRegAbsLutInput = 0x1D0, kRegLutInput = 0x1D1, kRegLutScale = 0x1D2, kRegLightEnable = 0x1D9;

// LightingRegs::LightingSampler.
constexpr u32 kSamplerD0 = 0, kSamplerD1 = 1, kSamplerFr = 3, kSamplerRb = 4, kSamplerRg = 5, kSamplerRr = 6;
constexpr u32 kSamplerSp = 8, kSamplerDa = 16;

// LightingLutInput.
constexpr u32 kInNH = 0, kInVH = 1, kInNV = 2, kInLN = 3, kInSP = 4, kInCP = 5;

// ---- the encoders of the register formats -------------------------------------------------------------------------------------

// A float with a zero low part, as the register's 16-bit float (5 exponent bits, bias 15) and 20-bit float (7 exponent bits, bias
// 63; pica_types.h's Float<10, 5> and Float<12, 7>). Only values that are exact in the format are used.
u32 FloatBits(float f) {
    u32 bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}
u32 F16(float f) {
    if (f == 0.0f) return 0;
    const u32 bits = FloatBits(f);
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    REQUIRE(exponent > 0);
    REQUIRE(exponent < 31);
    REQUIRE((bits & 0x1FFF) == 0);
    return ((bits >> 31) << 15) | (static_cast<u32>(exponent) << 10) | ((bits >> 13) & 0x3FF);
}
u32 F20(float f) {
    if (f == 0.0f) return 0;
    const u32 bits = FloatBits(f);
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 63;
    REQUIRE(exponent > 0);
    REQUIRE(exponent < 127);
    REQUIRE((bits & 0x7FF) == 0);
    return ((bits >> 31) << 19) | (static_cast<u32>(exponent) << 12) | ((bits >> 11) & 0xFFF);
}
// Float24 (1 sign, 7 exponent, 16 mantissa bits) of a normal float.
u32 F24(float f) {
    if (f == 0.0f) return 0;
    const u32 bits = FloatBits(f);
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 63;
    return ((bits >> 31) << 23) | (static_cast<u32>(exponent) << 16) | ((bits >> 7) & 0xFFFF);
}

// LightColor: b 0-9, g 10-19, r 20-29.
u32 Color10(int r, int g, int b) { return (static_cast<u32>(r) << 20) | (static_cast<u32>(g) << 10) | static_cast<u32>(b); }

// ---- the lighting setup of a case --------------------------------------------------------------------------------------------

struct LightRegs {
    u32 specular_0 = 0, specular_1 = 0, diffuse = 0, ambient = 0;  // Color10
    std::array<float, 3> position = {0, 0, 0};
    std::array<int, 3> spot = {0, 0, 0};  // the raw signed 13-bit fields
    bool directional = false, two_sided = false, geo0 = false, geo1 = false;
    float dist_bias = 0.0f, dist_scale = 0.0f;
};

struct LutSel {
    bool enabled = false;
    u32 input = kInNH;
    bool abs_input = true;  // false: the index is signed (the table's second half holds the negative inputs)
    u32 scale = 0;          // LightingScale: 0 1x, 1 2x, 2 4x, 3 8x, 6 0.25x, 7 0.5x
};

struct LitCase {
    std::string label;
    bool lit = true;
    u32 config = 0;  // LightingConfig (Config7 is 8)
    u32 bump_mode = 0, bump_selector = 0;
    bool renorm = true;
    bool clamp_highlights = false, primary_alpha = false, secondary_alpha = false;
    bool shadow = false, shadow_primary = false, shadow_secondary = false, shadow_invert = false, shadow_alpha = false;
    u32 shadow_selector = 0;
    std::vector<u32> slots;                  // light number of slot 0, 1, ...
    std::array<LightRegs, 8> lights{};
    u32 spot_enabled = 0, dist_enabled = 0, shadow_enabled = 0;  // bit n: light n has it (the registers hold the disable bits)
    LutSel d0, d1, sp, fr, rr, rg, rb;
    u32 ambient = Color10(0, 0, 0);
    std::array<float, 4> quat = {0.12f, 0.24f, -0.18f, 0.95f};  // x y z w, not normalised
    std::array<float, 3> view = {0.3f, -0.2f, 0.9f};
    std::array<std::array<int, 4>, 3> units = {{{200, 100, 220, 255}, {60, 180, 150, 128}, {130, 127, 255, 90}}};
    u32 unit0_type = 0;  // TextureType of unit 0 (2: Shadow2D)
};

float LutScaleOf(u32 code) {
    switch (code) {
    case 0: return 1.0f;
    case 1: return 2.0f;
    case 2: return 4.0f;
    case 3: return 8.0f;
    case 6: return 0.25f;
    case 7: return 0.5f;
    }
    return 0.0f;
}

// A table entry (Lighting::LutEntry): value bits 0-11 over 4095, difference bits 12-22 over 2047, negative when bit 23 is set.
u32 LutEntry(int value, int difference) {
    return static_cast<u32>(value) | (static_cast<u32>(std::abs(difference)) << 12) | (difference < 0 ? (1u << 23) : 0u);
}

// The 24 tables: a smooth curve of its own per table (so that a wrong table number shows), the difference the step to the next
// entry in the units of the format (over 2047 against the value's over 4095).
void FillLuts(pica::PicaState& state) {
    for (u32 t = 0; t < 24; ++t) {
        auto value = [&](u32 i) { return static_cast<int>(2048.0 + 1900.0 * std::sin(0.022 * static_cast<double>(i + 1) * (1.0 + t % 5) + 0.7 * t)); };
        for (u32 i = 0; i < 256; ++i) {
            const int v = std::clamp(value(i), 0, 4095);
            const int next = i == 255 ? v : std::clamp(value(i + 1), 0, 4095);
            const int d = std::clamp((next - v) * 2047 / 4095, -2047, 2047);
            state.lighting.luts[t][i] = LutEntry(v, d);
        }
    }
    state.lighting.lut_dirty = pica::Lighting::kAllDirty;
}

// A PICA state with stage 0 = the primary fragment colour (or the secondary), the other stages pass-through, colour writes on, no
// depth, no blending (logic op Copy), lighting disabled; plus a vertex program that passes six input registers through to six
// output registers (the translated path).
std::unique_ptr<pica::PicaState> BaseState(u32 source = 1) {
    auto state = std::make_unique<pica::PicaState>();
    state->regs.fill(0);
    state->dirty.SetAll();
    state->regs[kRegTev0 + 0] = source | (15u << 4) | (15u << 8) | (source << 16) | (15u << 20) | (15u << 24);
    state->regs[kRegTev0 + 1] = 0;
    state->regs[kRegTev0 + 2] = 0;
    for (u32 base : {0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u}) {
        state->regs[base + 0] = 15u | (15u << 4) | (15u << 8) | (15u << 16) | (15u << 20) | (15u << 24);
        state->regs[base + 1] = 0;
        state->regs[base + 2] = 0;
    }
    state->regs[kRegLogicOp] = static_cast<u32>(re::LogicOp::Copy);
    state->regs[kRegDepthColorMask] = 0xF00;  // red, green, blue, alpha writes
    state->regs[kRegAllowColor] = 0xF;
    state->regs[kRegAllowDepth] = 0;
    state->regs[kRegLightingDisable] = 1;
    state->regs[kRegDepthRange] = F24(-1.0f);
    state->regs[kRegDepthNear] = 0;
    state->regs[kRegDepthmap] = 1;

    // The vertex program (translated path): o_i = v_i for i = 0..5 (see the semantic map below).
    state->vs.program_code.fill(0);
    state->vs.swizzle_data.fill(0);
    return state;
}

// Writes the lighting registers and tables of a case.
void ApplyCase(const LitCase& c, pica::PicaState& state) {
    auto& r = state.regs;
    FillLuts(state);
    r[kRegLightingDisable] = c.lit ? 0 : 1;
    r[kRegGlobalAmbient] = c.ambient;
    r[kRegMaxLightIndex] = c.slots.empty() ? 0 : static_cast<u32>(c.slots.size() - 1);
    u32 enable_slots = 0;
    for (size_t i = 0; i < c.slots.size(); ++i) enable_slots |= c.slots[i] << (4 * i);
    r[kRegLightEnable] = enable_slots;

    for (u32 n = 0; n < 8; ++n) {
        const LightRegs& l = c.lights[n];
        const u32 base = kRegLight0 + n * kRegLightStride;
        r[base + 0] = l.specular_0;
        r[base + 1] = l.specular_1;
        r[base + 2] = l.diffuse;
        r[base + 3] = l.ambient;
        r[base + 4] = F16(l.position[0]) | (F16(l.position[1]) << 16);
        r[base + 5] = F16(l.position[2]);
        r[base + 6] = (static_cast<u32>(l.spot[0]) & 0x1FFF) | ((static_cast<u32>(l.spot[1]) & 0x1FFF) << 16);
        r[base + 7] = static_cast<u32>(l.spot[2]) & 0x1FFF;
        r[base + 9] = (l.directional ? 1u : 0u) | (l.two_sided ? 2u : 0u) | (l.geo0 ? 4u : 0u) | (l.geo1 ? 8u : 0u);
        r[base + 10] = F20(l.dist_bias);
        r[base + 11] = F20(l.dist_scale);
    }

    u32 config0 = (c.shadow ? 1u : 0u) | (c.primary_alpha ? 4u : 0u) | (c.secondary_alpha ? 8u : 0u) | (c.config << 4);
    config0 |= (c.shadow_primary ? 1u << 16 : 0u) | (c.shadow_secondary ? 1u << 17 : 0u) | (c.shadow_invert ? 1u << 18 : 0u) |
               (c.shadow_alpha ? 1u << 19 : 0u);
    config0 |= (c.bump_selector << 22) | (c.shadow_selector << 24) | (c.clamp_highlights ? 1u << 27 : 0u) | (c.bump_mode << 28) |
               (c.renorm ? 0u : 1u << 30);
    r[kRegConfig0] = config0;

    // config1: the disable bits. disable_shadow 0-7, disable_spot_atten 8-15, disable_lut_d0 16, d1 17, (18: a dummy bit that is
    // always set), fr 19, rr 20, rg 21, rb 22, disable_dist_atten 24-31.
    u32 config1 = (~c.shadow_enabled & 0xFFu) | ((~c.spot_enabled & 0xFFu) << 8) | (1u << 18) | ((~c.dist_enabled & 0xFFu) << 24);
    auto disable = [&](const LutSel& s, u32 bit) { return s.enabled ? 0u : 1u << bit; };
    config1 |= disable(c.d0, 16) | disable(c.d1, 17) | disable(c.fr, 19) | disable(c.rr, 20) | disable(c.rg, 21) | disable(c.rb, 22);
    r[kRegConfig1] = config1;

    // abs_lut_input.disable_*: d0 1, d1 5, sp 9, fr 13, rb 17, rg 21, rr 25; lut_input / lut_scale fields: d0 0, d1 4, sp 8, fr 12,
    // rb 16, rg 20, rr 24.
    struct Field {
        const LutSel& sel;
        u32 abs_bit, field;
    };
    const Field fields[] = {{c.d0, 1, 0}, {c.d1, 5, 4}, {c.sp, 9, 8}, {c.fr, 13, 12}, {c.rb, 17, 16}, {c.rg, 21, 20}, {c.rr, 25, 24}};
    u32 abs_disable = 0, input = 0, scale = 0;
    for (const Field& f : fields) {
        abs_disable |= f.sel.abs_input ? 0u : 1u << f.abs_bit;
        input |= f.sel.input << f.field;
        scale |= f.sel.scale << f.field;
    }
    r[kRegAbsLutInput] = abs_disable;
    r[kRegLutInput] = input;
    r[kRegLutScale] = scale;

    // unit 0's type (texture0 params, bits 28-30)
    r[kRegTexture0 + 2] = c.unit0_type << 28;
}

// ---- the oracle: sw_lighting.cpp, on the CPU ------------------------------------------------------------------------------

namespace sw {

double g_margin = 1e9;  // the smallest |input * 128| of a signed table lookup (in entries)

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length2(V3 a) { return Dot(a, a); }
V3 Normalized(V3 a) {
    const float length = std::sqrt(Length2(a));
    return {a.x / length, a.y / length, a.z / length};
}
// Common::QuaternionRotate: v + 2 * cross(q.xyz, cross(q.xyz, v) + v * q.w).
V3 Rotate(const std::array<float, 4>& q, V3 v) {
    const V3 u{q[0], q[1], q[2]};
    return v + Cross(u, Cross(u, v) + v * q[3]) * 2.0f;
}

float HalfToFloat(u32 h) {
    const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float v;
    if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
    else v = std::ldexp(static_cast<float>(1024 + mantissa), exponent - 25);
    return sign != 0 ? -v : v;
}
float F20ToFloat(u32 h) {
    const int sign = (h >> 19) & 1, exponent = (h >> 12) & 127, mantissa = h & 4095;
    float v;
    if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -12 - 62);
    else v = std::ldexp(static_cast<float>(4096 + mantissa), exponent - 63 - 12);
    return sign != 0 ? -v : v;
}
u32 Bits(u32 w, u32 first, u32 count) { return (w >> first) & ((1u << count) - 1u); }
V3 Color(u32 w) { return {static_cast<float>(Bits(w, 20, 10)) / 255.0f, static_cast<float>(Bits(w, 10, 10)) / 255.0f, static_cast<float>(Bits(w, 0, 10)) / 255.0f}; }
int Signed13(u32 w, u32 first) { return static_cast<int>(Bits(w, first, 13) << 19) >> 19; }

bool Supported(u32 config, u32 sampler) {
    switch (sampler) {
    case kSamplerD0: return config != 1;
    case kSamplerD1: return config != 0 && config != 1 && config != 5;
    case kSamplerSp: return config != 2 && config != 3;
    case kSamplerFr: return config != 0 && config != 2 && config != 4;
    case kSamplerRr: return config != 3;
    default: return config == 4 || config == 5 || config == 8;  // reflect green and blue
    }
}

// LookupLightingLut: lut_value + lut_diff * delta.
float Lookup(const pica::PicaState& st, u32 lut, u32 index, float delta) {
    const u32 raw = st.lighting.luts[lut][index];
    const float value = static_cast<float>(Bits(raw, 0, 12)) / 4095.0f;
    const float diff = static_cast<float>(Bits(raw, 12, 11)) / 2047.0f;
    return value + (Bits(raw, 23, 1) != 0 ? -diff : diff) * delta;
}

// The tables are continuous (each difference is the step to the next value), so an index that sits on the boundary of an entry
// reads the same from either side; the one jump is the signed tables' change from entry 127 to entry 255 at an input of zero.
void SignedMargin(double result_times_128) {
    g_margin = std::min(g_margin, std::abs(result_times_128));
}

using Rgba = std::array<int, 4>;

// ComputeFragmentsColors. `tex` is the four texture colours (bytes). Returns {primary, secondary}.
std::pair<Rgba, Rgba> ComputeFragmentsColors(const pica::PicaState& st, const std::array<float, 4>& normquat, V3 view,
                                            const std::array<Rgba, 4>& tex) {
    const auto& r = st.regs;
    const u32 config0 = r[kRegConfig0], config1 = r[kRegConfig1];
    const u32 config = Bits(config0, 4, 4);
    const u32 max_light_index = Bits(r[kRegMaxLightIndex], 0, 3);

    std::array<float, 4> shadow = {1, 1, 1, 1};
    if (Bits(config0, 0, 1) != 0) {
        const Rgba& t = tex[Bits(config0, 24, 2)];
        for (int i = 0; i < 4; ++i) shadow[i] = static_cast<float>(t[i]) / 255.0f;
        if (Bits(config0, 18, 1) != 0) {
            for (float& s : shadow) s = 1.0f - s;
        }
    }

    V3 surface_normal{}, surface_tangent{};
    const u32 bump_mode = Bits(config0, 28, 2);
    if (bump_mode != 0) {
        const Rgba& t = tex[Bits(config0, 22, 2)];
        V3 perturbation = V3{static_cast<float>(t[0]), static_cast<float>(t[1]), static_cast<float>(t[2])} * (1.0f / 127.5f) - V3{1, 1, 1};
        if (bump_mode == 1) {
            if (Bits(config0, 30, 1) == 0) {
                const float z_square = 1 - (perturbation.x * perturbation.x + perturbation.y * perturbation.y);
                perturbation.z = std::sqrt(std::max(z_square, 0.0f));
            }
            surface_normal = perturbation;
            surface_tangent = {1, 0, 0};
        } else {
            surface_normal = {0, 0, 1};
            surface_tangent = perturbation;
        }
    } else {
        surface_normal = {0, 0, 1};
        surface_tangent = {1, 0, 0};
    }

    // The quaternion is normalised before it is used.
    std::array<float, 4> q = normquat;
    const float qlen = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& c : q) c /= qlen;
    const V3 normal = Rotate(q, surface_normal);
    const V3 tangent = Rotate(q, surface_tangent);

    std::array<float, 4> diffuse_sum = {0, 0, 0, 1}, specular_sum = {0, 0, 0, 1};

    for (u32 light_index = 0; light_index <= max_light_index; ++light_index) {
        const u32 num = Bits(r[kRegLightEnable], 4 * light_index, 3);
        const u32 base = kRegLight0 + num * kRegLightStride;
        const V3 position = {HalfToFloat(Bits(r[base + 4], 0, 16)), HalfToFloat(Bits(r[base + 4], 16, 16)), HalfToFloat(Bits(r[base + 5], 0, 16))};
        const u32 light_config = r[base + 9];
        const bool directional = Bits(light_config, 0, 1) != 0, two_sided = Bits(light_config, 1, 1) != 0;
        const bool geo0 = Bits(light_config, 2, 1) != 0, geo1 = Bits(light_config, 3, 1) != 0;

        V3 light_vector = directional ? position : position + view;
        const float length = std::sqrt(Length2(light_vector));
        light_vector = Normalized(light_vector);
        const V3 norm_view = Normalized(view);
        const V3 half_vector = norm_view + light_vector;

        float dist_atten = 1.0f;
        if (Bits(config1, 24 + num, 1) == 0) {
            const float scale = F20ToFloat(Bits(r[base + 11], 0, 20));
            const float bias = F20ToFloat(Bits(r[base + 10], 0, 20));
            const float sample_loc = std::clamp(scale * length + bias, 0.0f, 1.0f);
            const u32 index = static_cast<u32>(std::clamp(std::floor(sample_loc * 256.0f), 0.0f, 255.0f));
            const float delta = sample_loc * 256 - static_cast<float>(index);
            dist_atten = Lookup(st, kSamplerDa + num, index, delta);
        }

        const V3 spot_dir = {static_cast<float>(Signed13(r[base + 6], 0)) / 2047.0f, static_cast<float>(Signed13(r[base + 6], 16)) / 2047.0f,
                             static_cast<float>(Signed13(r[base + 7], 0)) / 2047.0f};

        // (input, abs-disable bit, scale shift) of a table, then the lookup.
        auto get_lut_value = [&](u32 input_shift, u32 abs_disable_bit, u32 sampler) {
            const u32 input = Bits(r[kRegLutInput], input_shift, 3);
            const bool abs = Bits(r[kRegAbsLutInput], abs_disable_bit, 1) == 0;
            float result = 0.0f;
            switch (input) {
            case kInNH: result = Dot(normal, Normalized(half_vector)); break;
            case kInVH: result = Dot(norm_view, Normalized(half_vector)); break;
            case kInNV: result = Dot(normal, norm_view); break;
            case kInLN: result = Dot(light_vector, normal); break;
            case kInSP: result = Dot(light_vector, spot_dir); break;
            case kInCP:
                if (config == 8) {
                    const V3 norm_half_vector = Normalized(half_vector);
                    const V3 half_vector_proj = norm_half_vector - normal * Dot(normal, norm_half_vector);
                    result = Dot(half_vector_proj, tangent);
                } else {
                    result = 0.0f;
                }
                break;
            }
            u32 index;
            float delta;
            if (abs) {
                result = two_sided ? std::abs(result) : std::max(result, 0.0f);
                const float flr = std::floor(result * 256.0f);
                index = static_cast<u32>(std::clamp(flr, 0.0f, 255.0f));
                delta = result * 256 - static_cast<float>(index);
            } else {
                SignedMargin(result * 128.0);
                const float flr = std::floor(result * 128.0f);
                const int signed_index = static_cast<int>(std::clamp(flr, -128.0f, 127.0f));
                delta = result * 128.0f - static_cast<float>(signed_index);
                index = static_cast<u32>(signed_index) & 0xFF;
            }
            const float scale = LutScaleOf(Bits(r[kRegLutScale], input_shift, 3));
            return scale * Lookup(st, sampler, index, delta);
        };

        float spot_atten = 1.0f;
        if (Bits(config1, 8 + num, 1) == 0 && Supported(config, kSamplerSp)) {
            spot_atten = get_lut_value(8, 9, kSamplerSp + num);
        }

        float d0_lut_value = 1.0f;
        if (Bits(config1, 16, 1) == 0 && Supported(config, kSamplerD0)) d0_lut_value = get_lut_value(0, 1, kSamplerD0);
        V3 specular_0 = Color(r[base + 0]) * d0_lut_value;

        V3 refl_value{};
        if (Bits(config1, 20, 1) == 0 && Supported(config, kSamplerRr)) refl_value.x = get_lut_value(24, 25, kSamplerRr);
        else refl_value.x = 1.0f;
        if (Bits(config1, 21, 1) == 0 && Supported(config, kSamplerRg)) refl_value.y = get_lut_value(20, 21, kSamplerRg);
        else refl_value.y = refl_value.x;
        if (Bits(config1, 22, 1) == 0 && Supported(config, kSamplerRb)) refl_value.z = get_lut_value(16, 17, kSamplerRb);
        else refl_value.z = refl_value.x;

        float d1_lut_value = 1.0f;
        if (Bits(config1, 17, 1) == 0 && Supported(config, kSamplerD1)) d1_lut_value = get_lut_value(4, 5, kSamplerD1);
        V3 specular_1 = refl_value * Color(r[base + 1]) * d1_lut_value;

        // Fresnel: only the last entry in the light slots applies it.
        if (light_index == max_light_index && Bits(config1, 19, 1) == 0 && Supported(config, kSamplerFr)) {
            const float lut_value = get_lut_value(12, 13, kSamplerFr);
            if (Bits(config0, 2, 1) != 0) diffuse_sum[3] = lut_value;
            if (Bits(config0, 3, 1) != 0) specular_sum[3] = lut_value;
        }

        float dot_product = Dot(light_vector, normal);
        dot_product = two_sided ? std::abs(dot_product) : std::max(dot_product, 0.0f);

        float clamp_highlights = 1.0f;
        if (Bits(config0, 27, 1) != 0) clamp_highlights = dot_product == 0.0f ? 0.0f : 1.0f;

        if (geo0 || geo1) {
            float geo_factor = Length2(half_vector);
            geo_factor = geo_factor == 0.0f ? 0.0f : std::min(dot_product / geo_factor, 1.0f);
            if (geo0) specular_0 = specular_0 * geo_factor;
            if (geo1) specular_1 = specular_1 * geo_factor;
        }

        const bool shadow_disabled = Bits(config1, num, 1) != 0;
        const bool shadow_primary_enable = Bits(config0, 16, 1) != 0 && !shadow_disabled;
        const bool shadow_secondary_enable = Bits(config0, 17, 1) != 0 && !shadow_disabled;
        const V3 shadow_rgb = {shadow[0], shadow[1], shadow[2]};
        const V3 shadow_primary = shadow_primary_enable ? shadow_rgb : V3{1, 1, 1};
        const V3 shadow_secondary = shadow_secondary_enable ? shadow_rgb : V3{1, 1, 1};

        const V3 diffuse = (Color(r[base + 2]) * dot_product * shadow_primary + Color(r[base + 3])) * (dist_atten * spot_atten);
        const V3 specular = (specular_0 + specular_1) * (clamp_highlights * dist_atten * spot_atten) * shadow_secondary;
        diffuse_sum[0] += diffuse.x, diffuse_sum[1] += diffuse.y, diffuse_sum[2] += diffuse.z;
        specular_sum[0] += specular.x, specular_sum[1] += specular.y, specular_sum[2] += specular.z;
    }

    if (Bits(config0, 19, 1) != 0) {
        if (Bits(config0, 2, 1) != 0) diffuse_sum[3] *= shadow[3];
        if (Bits(config0, 3, 1) != 0) specular_sum[3] *= shadow[3];
    }
    const V3 ambient = Color(r[kRegGlobalAmbient]);
    diffuse_sum[0] += ambient.x, diffuse_sum[1] += ambient.y, diffuse_sum[2] += ambient.z;

    auto cast = [](const std::array<float, 4>& v) {
        Rgba out;
        for (int i = 0; i < 4; ++i) out[i] = static_cast<int>(std::clamp(v[i], 0.0f, 1.0f) * 255);
        return out;
    };
    return {cast(diffuse_sum), cast(specular_sum)};
}

}  // namespace sw

// ---- the rig ---------------------------------------------------------------------------------------------------------------

using cpuvertex::OutputVertex;

OutputVertex MakeVertex(float x, float y, std::array<float, 4> quat, std::array<float, 3> view) {
    OutputVertex v{};
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = -0.5f; v.pos[3] = 1.0f;
    for (int i = 0; i < 4; ++i) v.quat[i] = quat[i];
    for (int i = 0; i < 3; ++i) v.view[i] = view[i];
    for (int i = 0; i < 4; ++i) v.color[i] = 1.0f;
    v.tc0[0] = 0.5f; v.tc0[1] = 0.5f;
    v.tc1[0] = 0.5f; v.tc1[1] = 0.5f;
    v.tc2[0] = 0.5f; v.tc2[1] = 0.5f;
    v.tc0_w = 1.0f;
    return v;
}

// A triangle that covers the whole target: (-1,-1), (3,-1), (-1,3), with the given quaternions at its vertices.
struct Tri {
    OutputVertex v[3];
};
Tri FullTriangle(std::array<std::array<float, 4>, 3> quats, std::array<float, 3> view) {
    return Tri{{MakeVertex(-1, -1, quats[0], view), MakeVertex(3, -1, quats[1], view), MakeVertex(-1, 3, quats[2], view)}};
}

native::VertexInputKey TrivialInput() {
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

// The translated path's vertex layout: six float4 per vertex (position, colour, texcoords 0 and 1, quaternion, view and
// texcoord0_w, texcoord2), input registers 0 to 5.
constexpr u32 kTranslatedStride = 6 * 16;
native::VertexInputKey TranslatedInput() {
    native::VertexInputKey key;
    key.binding_count = 1;
    key.bindings[0].stride = kTranslatedStride;
    key.attribute_count = 6;
    for (u32 i = 0; i < 6; ++i) key.attributes[i] = {static_cast<u8>(i), 0, rhi::Format::Rgba32Float, i * 16};
    return key;
}

// The instruction words are placed where Azahar's nihstro puts them (externals/nihstro/include/nihstro/shader_bytecode.h,
// BSD-3-Clause): MOV: opcode 26-31, dest 21-25, src1 12-18, src2 7-11, operand descriptor 0-6.
namespace pi {
constexpr u32 MOV = 0x13, END = 0x22;
constexpr u32 V(u32 n) { return n; }
constexpr u32 O(u32 n) { return n; }
constexpr u32 Ins(u32 op, u32 dest, u32 s1, u32 s2) { return (op << 26) | (dest << 21) | (s1 << 12) | (s2 << 7); }
constexpr u32 kEnd = END << 26;
// Operand descriptor 0: all four lanes written, the three sources unswizzled and not negated.
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

// The vertex program and its output map into `state` (o_i = v_i; attribute 0 the position, 1 the colour, 2 texcoords 0 and 1,
// 3 the quaternion, 4 the view vector and texcoord0_w, 5 texcoord2).
void SetVertexProgram(pica::PicaState& state) {
    std::vector<u32> program;
    for (u32 i = 0; i < 6; ++i) program.push_back(pi::Ins(pi::MOV, pi::O(i), pi::V(i), 0));
    program.push_back(pi::kEnd);
    std::copy(program.begin(), program.end(), state.vs.program_code.begin());
    state.vs.swizzle_data[0] = pi::IdentityDescriptor();
    state.vs.biggest_program_size = static_cast<u32>(program.size());
    state.vs.biggest_swizzle_size = 1;
    state.regs[pica::reg::kVs + pica::reg::kShaderMainOffset] = 0;
    state.regs[pica::reg::kVs + pica::reg::kShaderOutputMask] = 0x3F;
    state.regs[shader::reg::kVsOutputTotal] = 6;
    auto map = [](u32 x, u32 y, u32 z, u32 w) { return x | (y << 8) | (z << 16) | (w << 24); };
    using namespace shader::semantic;
    state.regs[shader::reg::kVsOutputAttributes + 0] = map(kPositionX, kPositionY, kPositionZ, kPositionW);
    state.regs[shader::reg::kVsOutputAttributes + 1] = map(kColorR, kColorG, kColorB, kColorA);
    state.regs[shader::reg::kVsOutputAttributes + 2] = map(kTexcoord0U, kTexcoord0V, kTexcoord1U, kTexcoord1V);
    state.regs[shader::reg::kVsOutputAttributes + 3] = map(kQuaternionX, kQuaternionY, kQuaternionZ, kQuaternionW);
    state.regs[shader::reg::kVsOutputAttributes + 4] = map(kViewX, kViewY, kViewZ, kTexcoord0W);
    state.regs[shader::reg::kVsOutputAttributes + 5] = map(kTexcoord2U, kTexcoord2V, kInvalid, kInvalid);
}

enum class Path { Batch, Translated };

struct DrawSpec {
    std::shared_ptr<pica::PicaState> state;
    Tri tri;
};

struct Rig {
    std::shared_ptr<rhi::Backend> backend;
    DeviceProfile profile;
    std::unique_ptr<native::PipelineBuilder> builder;
    std::unique_ptr<native::DefaultPipelineProvider> provider;
    std::unique_ptr<native::DrawComposer> composer;
    std::unique_ptr<FragmentUniformRing> uniforms;
    std::unique_ptr<TableRings> tables;
    std::unique_ptr<rhi::Ring> vertex_ring, vs_ring, unit_staging;
    std::shared_ptr<rhi::Image> color;
    std::array<std::shared_ptr<rhi::Image>, 3> unit_images;
    shader::VertexShaderCache vs_cache;
    std::vector<native::PipelineKey> built;
    native::DrawResult last;
    u64 fixups = 0, draws = 0;

    Rig(std::shared_ptr<rhi::Backend> b, bool force_uber) : backend(std::move(b)) {
        profile = ProfileOf(backend->Caps());
        builder = std::make_unique<native::PipelineBuilder>(backend, FeatureRegistry::Global());
        provider = std::make_unique<native::DefaultPipelineProvider>(
            [this](const native::PipelineKey& key) {
                built.push_back(key);
                return builder->Build(key);
            },
            native::DefaultProviderOptions{force_uber});
        auto made = native::DrawComposer::Create(backend, *provider);
        REQUIRE_MESSAGE(made.ok(), made.error());
        composer = std::move(*made);
        auto ring = FragmentUniformRing::Create(*backend);
        REQUIRE_MESSAGE(ring.ok(), ring.error());
        uniforms = std::move(*ring);
        auto table_rings = TableRings::Create(*backend);
        REQUIRE_MESSAGE(table_rings.ok(), table_rings.error());
        tables = std::move(*table_rings);
        auto vr = backend->CreateRing(1 << 20, rhi::BufferUsage::Vertex, "lighting vertices");
        auto sr = backend->CreateRing(1 << 20, rhi::BufferUsage::Uniform, "lighting vs uniforms");
        auto ur = backend->CreateRing(64 << 10, rhi::BufferUsage::Transfer, "lighting unit colours");
        REQUIRE(vr.ok());
        REQUIRE(sr.ok());
        REQUIRE(ur.ok());
        vertex_ring = std::move(*vr);
        vs_ring = std::move(*sr);
        unit_staging = std::move(*ur);
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = desc.height = kSize;
        desc.name = "lighting colour";
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        color = *image;
        for (u32 u = 0; u < 3; ++u) {
            rhi::ImageDesc unit;
            unit.format = rhi::Format::Rgba8Unorm;
            unit.width = unit.height = 1;
            unit.name = "lighting unit";
            auto made_unit = backend->CreateImage(unit);
            REQUIRE_MESSAGE(made_unit.ok(), made_unit.error());
            unit_images[u] = *made_unit;
        }
    }

    // One texel per unit 0 to 2 (bytes), bound with every draw from then on (the composer's nearest, clamped sampler).
    void BindUnitColours(const std::array<std::array<int, 4>, 3>& colours) {
        rhi::Recorder rec = backend->BeginRecording();
        for (u32 u = 0; u < 3; ++u) {
            const std::array<u8, 4> texel = {static_cast<u8>(colours[u][0]), static_cast<u8>(colours[u][1]),
                                             static_cast<u8>(colours[u][2]), static_cast<u8>(colours[u][3])};
            auto r = sampling::UploadRgba8(rec, *unit_staging, *unit_images[u], 0, 0, 1, 1, texel);
            REQUIRE_MESSAGE(r.ok(), r.error());
        }
        backend->WaitIdle(backend->Submit(std::move(rec)));
    }

    // Clears the target to zero, runs the draws in one recorder through FragmentState (the key from the registers, the uniforms,
    // the tables), and returns the colour pixels. `path` Batch: the CPU path (the batch does the quaternion fix-up); Translated: the
    // vertex program of the state, and the fixed geometry shader where the device and the composer allow it.
    std::vector<u8> Render(const std::vector<DrawSpec>& specs, Path path = Path::Batch) {
        rhi::Recorder rec = backend->BeginRecording();
        composer->BeginRecording(rec);
        uniforms->BeginRecording();
        FragmentState fragment(profile, FeatureRegistry::Global());
        bool first = true;
        for (const DrawSpec& spec : specs) {
            auto prepared = fragment.Prepare(*spec.state, {}, tables.get());
            REQUIRE_MESSAGE(prepared.ok(), prepared.error());
            auto alloc = uniforms->Write(fragment.block(), prepared->block_changed);
            REQUIRE_MESSAGE(alloc.ok(), alloc.error());

            native::DrawDesc d;
            const auto vs = vs_ring->Map(2048, std::max<u64>(256, backend->Caps().uniform_buffer_offset_alignment));
            REQUIRE_MESSAGE(vs.ok(), vs.error());
            std::memset(vs->data.data(), 0, 2048);
            rhi::VertexBufferBinding binding{};
            if (path == Path::Batch) {
                cpuvertex::TriangleBatch batch;
                batch.AddTriangle(spec.tri.v[0], spec.tri.v[1], spec.tri.v[2]);
                auto streamed = cpuvertex::UploadBatch(*vertex_ring, batch);
                REQUIRE_MESSAGE(streamed.ok(), streamed.error());
                binding = rhi::VertexBufferBinding{vertex_ring->GetBuffer().get(), streamed->offset};
                d.count = streamed->vertex_count;
                d.vertex.kind = native::VertexStageKind::Trivial;
                d.vertex.input = TrivialInput();
            } else {
                // The vertices as six float4 each; the program passes them through.
                auto mapped = vertex_ring->Map(3 * kTranslatedStride, 16);
                REQUIRE_MESSAGE(mapped.ok(), mapped.error());
                for (u32 i = 0; i < 3; ++i) {
                    const OutputVertex& v = spec.tri.v[i];
                    const float data[6][4] = {{v.pos[0], v.pos[1], v.pos[2], v.pos[3]},
                                              {v.color[0], v.color[1], v.color[2], v.color[3]},
                                              {v.tc0[0], v.tc0[1], v.tc1[0], v.tc1[1]},
                                              {v.quat[0], v.quat[1], v.quat[2], v.quat[3]},
                                              {v.view[0], v.view[1], v.view[2], v.tc0_w},
                                              {v.tc2[0], v.tc2[1], 0.0f, 0.0f}};
                    std::memcpy(mapped->data.data() + i * kTranslatedStride, data, sizeof(data));
                }
                REQUIRE(vertex_ring->Commit(3 * kTranslatedStride).ok());
                binding = rhi::VertexBufferBinding{vertex_ring->GetBuffer().get(), mapped->offset};
                d.count = 3;
                // The stage is translated with the geometry output exactly when the composer will put the fix-up stage after it.
                shader::ConfigOptions options;
                options.use_clip_planes = true;
                options.geometry_stage = native::DecideFixup(fragment.needs().fixup_gs, native::VertexStageKind::Translated,
                                                             backend->Caps().geometry_shader,
                                                             native::FixupGeneratorRegistry::Global().Has()) == native::FixupDecision::Applied;
                const shader::VertexShaderConfig config = shader::MakeVertexShaderConfig(*spec.state, nullptr, options);
                shader::TranslateResult translated = vs_cache.Get(spec.state->vs, config);
                REQUIRE_MESSAGE(translated.ok(), translated.error);
                builder->AddVertexStage(config.Hash(), translated.shader);
                const shader::PicaUniformData pica_data = shader::MakePicaUniformData(spec.state->vs);
                const shader::VertexUniformData vs_data = shader::MakeVertexUniformData(*spec.state, false);
                std::memcpy(vs->data.data(), &pica_data, sizeof(pica_data));
                std::memcpy(vs->data.data() + 1792, &vs_data, sizeof(vs_data));
                d.vertex.kind = native::VertexStageKind::Translated;
                d.vertex.program = config.Hash();
                d.vertex.input = TranslatedInput();
            }
            REQUIRE(vs_ring->Commit(2048).ok());

            rhi::TargetPassDesc pass;
            pass.color = color.get();
            pass.area = rhi::Rect{0, 0, kSize, kSize};
            const fbops::DynamicState dynamic = fbops::MakeDynamicState(*spec.state);
            if (first) {
                rhi::TargetPassDesc clear_pass = pass;
                clear_pass.color_load = rhi::LoadOp::Clear;
                clear_pass.clear_color = {0, 0, 0, 0};
                composer->BeginTargetPass(rec, clear_pass);
                fbops::ApplyDynamicState(rec, dynamic, false);
                first = false;
            }
            const FragmentKey key = fragment.key();
            d.fragment = &key;
            d.needs = fragment.needs();
            d.state = fbops::MakeFixedFunctionState(*spec.state);
            d.color_format = rhi::Format::Rgba8Unorm;
            d.depth_format = rhi::Format::Undefined;
            d.target = pass;
            d.vs_pica_uniforms = native::UniformRange{vs_ring->GetBuffer().get(), vs->offset, 1616};
            d.vs_uniforms = native::UniformRange{vs_ring->GetBuffer().get(), vs->offset + 1792, 32};
            d.fragment_uniforms = *alloc;
            d.tables = tables.get();
            d.vertex_buffers = std::span<const rhi::VertexBufferBinding>(&binding, 1);
            d.viewport = rhi::Viewport{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
            d.scissor = rhi::Rect{0, 0, kSize, kSize};
            for (u32 u = 0; u < 3; ++u) d.units[u] = native::TextureBinding{unit_images[u].get(), rhi::ImageView::Sampled, nullptr};
            // A Shadow2D unit 0 is bound to the composer's own white shadow dummy (a plain image has no shadow view).
            if (key.texture.texture0_type() == re::TextureType::Shadow2D) d.units[0] = native::TextureBinding{};
            auto result = composer->Draw(rec, d);
            REQUIRE_MESSAGE(result.ok(), result.error());
            last = *result;
            fixups += result->fixup ? 1 : 0;
            ++draws;
        }
        backend->WaitIdle(backend->Submit(std::move(rec)));
        auto pixels = backend->ReadImage(*color, 0, 0, kSize, kSize);
        REQUIRE_MESSAGE(pixels.ok(), pixels.error());
        return *pixels;
    }
};

std::array<int, 4> Pixel(const std::vector<u8>& pixels, u32 x, u32 y) {
    const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
    return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
}

// Every pixel within `tolerance` of `want` in every channel.
bool AllNear(const std::vector<u8>& pixels, std::array<int, 4> want, int tolerance, std::string* why = nullptr) {
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const std::array<int, 4> p = Pixel(pixels, x, y);
            for (int c = 0; c < 4; ++c) {
                if (std::abs(p[c] - want[c]) > tolerance) {
                    if (why != nullptr) {
                        *why = "pixel (" + std::to_string(x) + "," + std::to_string(y) + ") channel " + std::to_string(c) + " is " +
                               std::to_string(p[c]) + ", expected " + std::to_string(want[c]) + " +- " + std::to_string(tolerance);
                    }
                    return false;
                }
            }
        }
    }
    return true;
}

bool SameWithin(const std::vector<u8>& a, const std::vector<u8>& b, int tolerance) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > tolerance) return false;
    }
    return true;
}

constexpr int kTolerance = 3;

// A draw of a case: the state with the case's registers, the colour source (1 primary, 2 secondary), the triangle with the case's
// quaternion and view vector at every vertex.
DrawSpec DrawOf(const LitCase& c, u32 source) {
    std::shared_ptr<pica::PicaState> state = BaseState(source);
    SetVertexProgram(*state);
    ApplyCase(c, *state);
    DrawSpec spec;
    spec.state = std::move(state);
    spec.tri = FullTriangle({c.quat, c.quat, c.quat}, c.view);
    return spec;
}

// The software renderer's colours for a case: the four texture colours are the three units' and (unit 3, never read here) zero.
std::pair<sw::Rgba, sw::Rgba> Reference(const LitCase& c, const pica::PicaState& state) {
    if (!c.lit) return {sw::Rgba{0, 0, 0, 0}, sw::Rgba{0, 0, 0, 0}};  // `if (!regs.lighting.disable)`: both colours stay zero
    std::array<sw::Rgba, 4> tex = {};
    for (u32 u = 0; u < 3; ++u) tex[u] = c.units[u];
    if (c.unit0_type == 2) tex[0] = {255, 255, 255, 255};  // a Shadow2D unit 0 on the composer's white dummy
    return sw::ComputeFragmentsColors(state, c.quat, sw::V3{c.view[0], c.view[1], c.view[2]}, tex);
}

// ---- the cases' data -------------------------------------------------------------------------------------------------------

LutSel Lut(u32 input, bool abs_input = true, u32 scale = 0) {
    LutSel s;
    s.enabled = true;
    s.input = input;
    s.abs_input = abs_input;
    s.scale = scale;
    return s;
}

LightRegs Directional(std::array<float, 3> direction, u32 diffuse, u32 ambient, u32 specular_0 = 0, u32 specular_1 = 0) {
    LightRegs l;
    l.directional = true;
    l.position = direction;
    l.diffuse = diffuse;
    l.ambient = ambient;
    l.specular_0 = specular_0;
    l.specular_1 = specular_1;
    return l;
}

LightRegs Positional(std::array<float, 3> position, u32 diffuse, u32 ambient, u32 specular_0 = 0, u32 specular_1 = 0) {
    LightRegs l = Directional(position, diffuse, ambient, specular_0, specular_1);
    l.directional = false;
    return l;
}

// One directional light, no table.
LitCase Basic() {
    LitCase c;
    c.label = "one directional light, no tables";
    c.slots = {0};
    c.lights[0] = Directional({0.25f, 0.5f, 0.75f}, Color10(200, 150, 100), Color10(10, 20, 30), Color10(255, 255, 255));
    c.ambient = Color10(5, 6, 7);
    return c;
}

// Three lights (numbers 5, 2, 7 in the slots, so that the slot and the light number differ) and every table, configuration 7.
LitCase Full(u32 config) {
    LitCase c;
    c.label = "three lights, every table, configuration " + std::to_string(config);
    c.config = config;
    c.slots = {5, 2, 7};
    c.lights[5] = Positional({0.5f, -0.25f, 0.125f}, Color10(180, 120, 60), Color10(8, 4, 2), Color10(220, 200, 180), Color10(90, 100, 110));
    c.lights[5].spot = {1000, -500, 1500};
    c.lights[5].dist_bias = 0.25f;
    c.lights[5].dist_scale = 0.5f;
    c.lights[5].geo0 = true;
    c.lights[2] = Directional({-0.375f, 0.25f, 0.5f}, Color10(100, 160, 220), Color10(2, 3, 4), Color10(140, 150, 160), Color10(250, 200, 150));
    c.lights[2].geo1 = true;
    c.lights[2].spot = {-700, 900, 1200};
    c.lights[7] = Positional({-0.125f, 0.75f, -0.5f}, Color10(150, 150, 150), Color10(6, 6, 6), Color10(255, 128, 64), Color10(64, 128, 255));
    c.lights[7].two_sided = true;
    c.lights[7].spot = {300, 300, 1800};
    c.lights[7].dist_bias = 0.125f;
    c.lights[7].dist_scale = 0.25f;
    c.spot_enabled = (1u << 5) | (1u << 7);
    c.dist_enabled = (1u << 5) | (1u << 7);
    c.d0 = Lut(kInNH, true, 0);
    c.d1 = Lut(kInVH, true, 1);
    c.sp = Lut(kInSP, true, 0);
    c.fr = Lut(kInNV, false, 0);
    c.rr = Lut(kInLN, true, 0);
    c.rg = Lut(kInCP, true, 7);
    c.rb = Lut(kInNH, false, 6);
    c.primary_alpha = true;
    c.secondary_alpha = true;
    c.clamp_highlights = true;
    c.ambient = Color10(12, 10, 8);
    return c;
}

std::vector<LitCase> FormulaCases() {
    std::vector<LitCase> out;
    {
        LitCase c;
        c.label = "lighting disabled";
        c.lit = false;
        out.push_back(c);
    }
    out.push_back(Basic());
    {
        LitCase c = Basic();
        c.label = "one positional light with the distance and spot tables, configuration 0";
        c.slots = {3};
        c.lights[3] = Positional({0.5f, -0.25f, 0.125f}, Color10(200, 150, 100), Color10(10, 20, 30), Color10(255, 220, 200));
        c.lights[3].spot = {1000, -500, 1500};
        c.lights[3].dist_bias = 0.25f;
        c.lights[3].dist_scale = 0.5f;
        c.lights[3].geo0 = true;
        c.spot_enabled = 1u << 3;
        c.dist_enabled = 1u << 3;
        c.d0 = Lut(kInNH, true, 1);
        c.rr = Lut(kInNV, true, 0);
        c.sp = Lut(kInSP, true, 0);
        out.push_back(c);
    }
    for (u32 config : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u}) out.push_back(Full(config));
    {
        LitCase c = Full(8);
        c.label = "the same with signed inputs on d0, d1, sp and rr";
        c.d0.abs_input = c.d1.abs_input = c.sp.abs_input = c.rr.abs_input = false;
        c.rg = Lut(kInNV, true, 1);
        out.push_back(c);
    }
    {
        LitCase c = Full(8);
        c.label = "eight lights";
        c.slots = {0, 1, 2, 3, 4, 5, 6, 7};
        for (u32 n : {0u, 1u, 3u, 4u, 6u}) {
            c.lights[n] = Positional({0.125f * static_cast<float>(n + 1), -0.25f, 0.5f - 0.0625f * static_cast<float>(n)}, Color10(30 + 5 * n, 40, 50 + 3 * n),
                                     Color10(1, 2, 3), Color10(100 + 10 * n, 90, 80), Color10(20, 30 + 10 * n, 40));
            c.lights[n].dist_bias = 0.25f;
            c.lights[n].dist_scale = 0.375f;
        }
        c.dist_enabled = 0xFF;
        c.spot_enabled = 0x55;
        out.push_back(c);
    }
    {
        LitCase c = Full(8);
        c.label = "a normal map from unit 1, renormalised";
        c.bump_mode = 1;
        c.bump_selector = 1;
        out.push_back(c);
    }
    {
        LitCase c = Full(8);
        c.label = "a normal map from unit 2, not renormalised";
        c.bump_mode = 1;
        c.bump_selector = 2;
        c.renorm = false;
        c.units[2] = {170, 90, 200, 255};
        out.push_back(c);
    }
    {
        LitCase c = Full(8);
        c.label = "a tangent map from unit 0 (the CP input uses the tangent)";
        c.bump_mode = 2;
        c.bump_selector = 0;
        out.push_back(c);
    }
    {
        LitCase c = Full(8);
        c.label = "the shadow term from unit 1: primary and secondary, alpha";
        c.shadow = c.shadow_primary = c.shadow_secondary = c.shadow_alpha = true;
        c.shadow_selector = 1;
        c.shadow_enabled = (1u << 5) | (1u << 7);  // light 2 is not shadowed
        out.push_back(c);
    }
    {
        LitCase c = Full(0);
        c.label = "the inverted shadow term from unit 2, primary only";
        c.shadow = c.shadow_primary = c.shadow_invert = true;
        c.shadow_selector = 2;
        c.shadow_enabled = 0xFF;
        out.push_back(c);
    }
    {
        LitCase c = Full(8);
        c.label = "a normal map and a shadow term from the same unit";
        c.bump_mode = 1;
        c.bump_selector = 1;
        c.shadow = c.shadow_primary = c.shadow_secondary = true;
        c.shadow_selector = 1;
        c.shadow_enabled = 0xFF;
        out.push_back(c);
    }
    // A lit draw whose shadow selector reads a Shadow2D unit 0 calls N2-12's real `shadowTexture` (FeatureRegistry::Global()). Here the
    // unit is the composer's white dummy (the word 0xFFFFFFFF: depth 0xFFFFFF, intensity 255, cleared in BeginRecording), so the lookup
    // is 1.0 in every channel (the texel passes the compare for any depth; no border wrap brings in a border colour). Through
    // `--gpu native` the same draw binds the decoded map or the 1x1 black shadow image and the lookup is 0.0 for the black image.
    {
        LitCase c = Basic();
        c.label = "the shadow term from a Shadow2D unit 0 (the composer's white dummy: 1.0), inverted";
        c.unit0_type = 2;
        c.shadow = c.shadow_primary = c.shadow_secondary = c.shadow_invert = true;
        c.shadow_selector = 0;
        c.shadow_enabled = 0xFF;
        out.push_back(c);
    }
    {
        LitCase c = Basic();
        c.label = "the shadow term from a Shadow2D unit 0 (the composer's white dummy: 1.0)";
        c.unit0_type = 2;
        c.shadow = c.shadow_primary = c.shadow_secondary = true;
        c.shadow_selector = 0;
        c.shadow_enabled = 0xFF;
        out.push_back(c);
    }
    return out;
}

}  // namespace

// ---- without a device -------------------------------------------------------------------------------------------------------

TEST_CASE("lighting/the_feature_is_real_and_registered_with_its_fixup_generator") {
    const FeatureModule* lighting = FeatureRegistry::Global().Find("lighting");
    REQUIRE(lighting != nullptr);
    FragmentKey off{};
    FragmentKey on{};
    on.lighting.set_enable(1);
    // Not a stub any more: nothing is counted `stubbed:lighting`.
    CHECK(lighting->Stubbed(on) == nullptr);
    std::array<const char*, 5> stubbed{};
    CHECK(StubbedFeatures(on, FeatureRegistry::Global(), stubbed) == 0);

    // The needs: the tables of lut_lf and the units the bump and shadow terms read; the fix-up is core's.
    CHECK(lighting->NeedsOf(off, DeviceProfile{}) == Needs{});
    Needs needs = lighting->NeedsOf(on, DeviceProfile{});
    CHECK(needs.lut_lf);
    CHECK(needs.texture_units == 0);
    CHECK_FALSE(needs.fixup_gs);
    CHECK(CoreNeedsOf(on, DeviceProfile{}).fixup_gs);
    on.lighting.set_bump_mode(re::LightingBumpMode::NormalMap);
    on.lighting.set_bump_selector(1);
    CHECK(lighting->NeedsOf(on, DeviceProfile{}).texture_units == 2);
    on.lighting.set_enable_shadow(1);
    on.lighting.set_shadow_selector(2);
    CHECK(lighting->NeedsOf(on, DeviceProfile{}).texture_units == 3);
    on.lighting.set_shadow_selector(3);  // the procedural unit: not a texture unit to bind
    CHECK(lighting->NeedsOf(on, DeviceProfile{}).texture_units == 2);

    // The fix-up generator is registered with N2-33's slot, and makes a geometry stage.
    CHECK(native::FixupGeneratorRegistry::Global().Has());
    native::VertexStageOutputs outputs;
    outputs.num_outputs = 1;
    outputs.semantic_map.assign(24, {16, 0});
    outputs.semantic_map[0] = {0, 0};
    CHECK_FALSE(native::FixupGeneratorRegistry::Global().Generate(outputs).empty());
}

TEST_CASE("lighting/the_uniforms_are_azahars_light_src_and_global_ambient") {
    auto state = BaseState();
    LitCase c = Full(8);
    c.lights[5].specular_0 = Color10(255, 128, 64);
    ApplyCase(c, *state);

    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    UniformDirty dirty;
    dirty.all = true;
    FillUniforms(*state, block, dirty);
    CHECK(dirty.changed);

    const LightSrc& l5 = block.uniforms.light_src[5];
    CHECK(l5.specular_0.x == doctest::Approx(1.0f));
    CHECK(l5.specular_0.y == doctest::Approx(128.0f / 255.0f));
    CHECK(l5.specular_0.z == doctest::Approx(64.0f / 255.0f));
    CHECK(l5.specular_1.x == doctest::Approx(90.0f / 255.0f));
    CHECK(l5.diffuse.x == doctest::Approx(180.0f / 255.0f));
    CHECK(l5.ambient.z == doctest::Approx(2.0f / 255.0f));
    CHECK(l5.position.x == 0.5f);
    CHECK(l5.position.y == -0.25f);
    CHECK(l5.position.z == 0.125f);
    CHECK(l5.spot_direction.x == doctest::Approx(1000.0f / 2047.0f));  // not negated
    CHECK(l5.spot_direction.y == doctest::Approx(-500.0f / 2047.0f));
    CHECK(l5.spot_direction.z == doctest::Approx(1500.0f / 2047.0f));
    CHECK(l5.dist_atten_bias == 0.25f);
    CHECK(l5.dist_atten_scale == 0.5f);
    CHECK(block.uniforms.light_src[2].spot_direction.x == doctest::Approx(-700.0f / 2047.0f));
    CHECK(block.uniforms.lighting_global_ambient.x == doctest::Approx(12.0f / 255.0f));
    CHECK(block.uniforms.lighting_global_ambient.y == doctest::Approx(10.0f / 255.0f));
    CHECK(block.uniforms.lighting_global_ambient.z == doctest::Approx(8.0f / 255.0f));

    // Only a dirty register writes, and only when the value changed.
    state->dirty.Reset();
    UniformDirty quiet;
    FillUniforms(*state, block, quiet);
    CHECK_FALSE(quiet.changed);
    state->regs[kRegLight0 + 5 * kRegLightStride + 2] = Color10(10, 20, 30);
    FillUniforms(*state, block, quiet);
    CHECK_FALSE(quiet.changed);  // the register is not marked dirty: nothing is read
    state->dirty.Set(kRegLight0 + 5 * kRegLightStride + 2);
    FillUniforms(*state, block, quiet);
    CHECK(quiet.changed);
    CHECK(block.uniforms.light_src[5].diffuse.x == doctest::Approx(10.0f / 255.0f));
    CHECK(block.uniforms.light_src[5].diffuse.y == doctest::Approx(20.0f / 255.0f));
    quiet.changed = false;
    FillUniforms(*state, block, quiet);  // dirty again, same value
    CHECK_FALSE(quiet.changed);
    state->regs[kRegGlobalAmbient] = Color10(1, 2, 3);
    state->dirty.Set(kRegGlobalAmbient);
    FillUniforms(*state, block, quiet);
    CHECK(quiet.changed);
    CHECK(block.uniforms.lighting_global_ambient.z == doctest::Approx(3.0f / 255.0f));
}

TEST_CASE("lighting/the_key_is_read_from_the_registers_as_the_cases_write_them") {
    // The test's register writer and N2-33's MakeKey agree (so the oracle and the generator read the same lights).
    LitCase c = Full(8);
    c.bump_mode = 2;
    c.bump_selector = 2;
    c.shadow = c.shadow_primary = c.shadow_invert = true;
    c.shadow_selector = 1;
    c.shadow_enabled = (1u << 5) | (1u << 2);
    auto state = BaseState();
    ApplyCase(c, *state);
    const FragmentKey key = MakeKey(*state);
    const LightConfig& l = key.lighting;
    CHECK(l.enable() == 1);
    CHECK(l.src_num() == 3);
    CHECK(l.config() == re::LightingConfig::Config7);
    CHECK(l.bump_mode() == re::LightingBumpMode::TangentMap);
    CHECK(l.bump_selector() == 2);
    CHECK(l.shadow_selector() == 1);
    CHECK(l.enable_shadow() == 1);
    CHECK(l.shadow_invert() == 1);
    CHECK(l.clamp_highlights() == 1);
    CHECK(l.enable_primary_alpha() == 1);
    CHECK(l.enable_secondary_alpha() == 1);
    CHECK(l.lights[0].num() == 5);
    CHECK(l.lights[1].num() == 2);
    CHECK(l.lights[2].num() == 7);
    CHECK(l.lights[0].geometric_factor_0() == 1);
    CHECK(l.lights[1].geometric_factor_1() == 1);
    CHECK(l.lights[2].two_sided_diffuse() == 1);
    CHECK(l.lights[0].shadow_enable() == 1);
    CHECK(l.lights[2].shadow_enable() == 0);
    CHECK(l.lut_rg.type() == re::LightingLutInput::CP);
    CHECK(l.lut_fr.abs_input() == 0);
    CHECK(l.lut_rb.GetScale() == 0.25f);
}

TEST_CASE("generator/the_fixed_geometry_shader_follows_the_output_map") {
    // Three outputs: attribute 0 the position, 1 the colour (and the quaternion's x in lane 3 is not mapped), 2 texcoord0 and the
    // quaternion's z, w.
    native::VertexStageOutputs outputs;
    outputs.num_outputs = 3;
    outputs.semantic_map.assign(24, {16, 0});
    for (u32 i = 0; i < 4; ++i) outputs.semantic_map[shader::semantic::kPositionX + i] = {0, static_cast<u8>(i)};
    for (u32 i = 0; i < 4; ++i) outputs.semantic_map[shader::semantic::kColorR + i] = {1, static_cast<u8>(i)};
    outputs.semantic_map[shader::semantic::kTexcoord0U] = {2, 0};
    outputs.semantic_map[shader::semantic::kTexcoord0V] = {2, 1};
    outputs.semantic_map[shader::semantic::kQuaternionZ] = {2, 2};
    outputs.semantic_map[shader::semantic::kQuaternionW] = {2, 3};
    const std::string glsl = lighting_gen::GenerateFixedGeometryGlsl(outputs);
    auto has = [&](const char* text) { return glsl.find(text) != std::string::npos; };
    CHECK(has("layout(triangles) in;"));
    CHECK(has("layout(triangle_strip, max_vertices = 3) out;"));
    CHECK(has("layout (location = 0) in vec4 vs_out_attr0[];"));
    CHECK(has("layout (location = 2) in vec4 vs_out_attr2[];"));
    CHECK_FALSE(has("vs_out_attr3"));
    for (const char* varying : {"layout (location = 1) out vec4 primary_color;", "layout (location = 2) out vec2 texcoord0;",
                                "layout (location = 3) out vec2 texcoord1;", "layout (location = 4) out vec2 texcoord2;",
                                "layout (location = 5) out float texcoord0_w;", "layout (location = 6) out vec4 normquat;",
                                "layout (location = 7) out vec3 view;"}) {
        CHECK_MESSAGE(has(varying), varying);
    }
    // An unmapped semantic reads 1.0; a mapped one its attribute and lane.
    CHECK(has("return vec4(1.0, 1.0, vtx.attributes[2].z, vtx.attributes[2].w);"));
    CHECK(has("vec4 vtx_pos = vec4(vtx.attributes[0].x, vtx.attributes[0].y, vtx.attributes[0].z, vtx.attributes[0].w);"));
    CHECK(has("texcoord0_w = 1.0;"));
    CHECK(has("view = vec3(1.0, 1.0, 1.0);"));
    // The fix-up: vertices 1 and 2 against vertex 0.
    CHECK(has("EmitVtx(vtx1, AreQuaternionsOpposite(GetVertexQuaternion(vtx0), GetVertexQuaternion(vtx1)));"));
    CHECK(has("EmitVtx(vtx2, AreQuaternionsOpposite(GetVertexQuaternion(vtx0), GetVertexQuaternion(vtx2)));"));
    // The vertex stage's own rules.
    CHECK(has("vtx_pos = SanitizeVertex(vtx_pos);"));
    CHECK(has("gl_Position = vec4(vtx_pos.x, vtx_pos.y, -vtx_pos.z, vtx_pos.w);"));
    CHECK(has("gl_ClipDistance[0] = out_clip.x;"));
    CHECK(has("primary_color = mix(vec4(1.0), vtx_color, lessThan(vtx_color, vec4(1.0)));"));

    std::vector<u32> spirv;
    std::string error;
    CHECK_MESSAGE(lighting_gen::CompileGeometryGlslToSpirv(glsl, spirv, error), error);
    CHECK(spirv.size() > 5);
    CHECK(spirv[0] == 0x07230203u);

    // Every size of output map compiles: none, one, the most (16), with the position in a late attribute.
    for (u32 count : {0u, 1u, 7u, 16u}) {
        native::VertexStageOutputs o;
        o.num_outputs = count;
        o.semantic_map.assign(24, {16, 0});
        if (count > 0) {
            for (u32 i = 0; i < 4; ++i) o.semantic_map[shader::semantic::kPositionX + i] = {static_cast<u8>(count - 1), static_cast<u8>(i)};
            o.semantic_map[shader::semantic::kQuaternionX] = {0, 3};
        }
        const std::vector<u32> made = lighting_gen::GenerateFixedGeometryShader(o);
        CHECK_MESSAGE(!made.empty(), "an output map of " << count << " attributes");
    }
    // A semantic map shorter than 24 entries is read as unmapped past its end.
    native::VertexStageOutputs shortmap;
    shortmap.num_outputs = 2;
    shortmap.semantic_map.assign(4, {0, 0});
    CHECK_FALSE(lighting_gen::GenerateFixedGeometryShader(shortmap).empty());
}

TEST_CASE("generator/the_bump_and_shadow_terms_sample_once_per_unit_in_the_ubershader_and_as_azahar_does_in_the_specialised_shader") {
    // Specialised: Azahar's text. A normal map from unit 1 and a shadow term from unit 1 are two calls (one per term).
    FragmentKey key{};
    key.framebuffer.set_alpha_test_func(re::CompareFunc::Always);  // (Never discards everything before the lighting)
    key.framebuffer.set_logic_op(re::LogicOp::Copy);
    key.framebuffer.requested_logic_op = re::LogicOp::Copy;
    key.lighting.set_enable(1);
    key.lighting.set_src_num(1);
    key.lighting.set_bump_mode(re::LightingBumpMode::NormalMap);
    key.lighting.set_bump_selector(1);
    key.lighting.set_enable_shadow(1);
    key.lighting.set_shadow_selector(2);
    key.lighting.set_shadow_invert(1);
    const DeviceProfile profile{};
    const std::string spec = GenerateSpecialisedGlsl(key, profile, ShaderPass::Normal);
    auto count = [](const std::string& text, const std::string& what) {
        size_t n = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
        return n;
    };
    const size_t spec_begin = spec.find("vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0);");
    const size_t spec_end = spec.find("secondary_fragment_color = clamp(specular_sum");
    REQUIRE(spec_begin != std::string::npos);
    REQUIRE(spec_end != std::string::npos);
    const std::string block = spec.substr(spec_begin, spec_end - spec_begin);
    CHECK_MESSAGE(block.find("surface_normal = 2.0 * (sampleTexUnit1()).rgb - 1.0;") != std::string::npos, block);
    CHECK_MESSAGE(block.find("shadow = vec4(1.0) - sampleTexUnit2();") != std::string::npos, block);
    CHECK(block.find("surface_tangent = 2.0 * (") == std::string::npos);  // the other bump mode is not emitted
    CHECK(count(block, "sampleTexUnit") == 2);
    CHECK(spec.find("mh4n_lt") == std::string::npos);
    CHECK(spec.find("mh4n_lut") == std::string::npos);
    CHECK(spec.find("LookupLightingLUTUnsigned") != std::string::npos);
    // Selector 3 is the procedural unit: both terms compile (`sampleTexUnit3()` calls `ProcTex()`).
    FragmentKey proc = key;
    proc.lighting.set_bump_selector(3);
    proc.lighting.set_shadow_selector(3);
    const Generated proc_spec = GenerateSpecialised(proc, profile, ShaderPass::Normal);
    CHECK_MESSAGE(proc_spec.ok(), proc_spec.error);
    CHECK(proc_spec.glsl.find("surface_normal = 2.0 * (sampleTexUnit3()).rgb - 1.0;") != std::string::npos);
    // A key without lighting has none of it.
    FragmentKey off = key;
    off.lighting = LightConfig{};
    const std::string unlit = GenerateSpecialisedGlsl(off, profile, ShaderPass::Normal);
    CHECK(unlit.find("LookupLightingLUT") == std::string::npos);
    CHECK(unlit.find("normalized_normquat") == std::string::npos);

    // The ubershader: each unit sampled at most once, four call sites in the lighting block, each under a condition on cfg.
    const std::string uber = GenerateUberGlsl(ShaderPass::Normal, profile);
    for (u32 unit = 0; unit < 4; ++unit) {
        const std::string call = "mh4n_lt" + std::to_string(unit) + " = sampleTexUnit" + std::to_string(unit) + "();";
        CHECK_MESSAGE(count(uber, call) == 1, call);
    }
    CHECK(count(uber, "mh4n_bump_texel") >= 2);
    // The lighting block never reads core's `mh4n_tex0` .. `3` (they hold a unit only when a TEV stage reads it, and are set later).
    const size_t block_begin = uber.find("vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0);");
    const size_t block_end = uber.find("secondary_fragment_color = clamp(specular_sum");
    REQUIRE(block_begin != std::string::npos);
    REQUIRE(block_end != std::string::npos);
    REQUIRE(block_begin < block_end);
    CHECK(uber.substr(block_begin, block_end - block_begin).find("mh4n_tex") == std::string::npos);
    CHECK(uber.find("mh4n_tex0") != std::string::npos);  // core's, in tev_source
    CHECK(count(uber, "LookupLightingLUT(lut_index, index, delta)") == 1);  // one call, in mh4n_lut
}

// ---- the SPIR-V call graph of the ubershader --------------------------------------------------------------------------------

namespace {

constexpr u32 kOpName = 5, kOpEntryPoint = 15, kOpFunction = 54, kOpFunctionEnd = 56, kOpFunctionCall = 57;

struct CallGraph {
    u32 entry = 0;
    std::map<u32, std::string> names;
    std::vector<u32> functions;
    std::map<u32, std::map<u32, u64>> calls;  // function -> callee -> call sites

    std::vector<u32> Named(const std::string& name) const {
        std::vector<u32> out;
        for (u32 id : functions) {
            const auto it = names.find(id);
            if (it != names.end() && it->second == name) out.push_back(id);
        }
        return out;
    }
};

// The walk of gpu_tev_test.cpp's (N2-43), without the image check.
CallGraph ReadCallGraph(const std::vector<u32>& spirv) {
    CallGraph g;
    u32 current = 0;
    for (size_t i = 5; i < spirv.size();) {
        const u32 count = spirv[i] >> 16, opcode = spirv[i] & 0xFFFF;
        if (count == 0 || i + count > spirv.size()) break;
        if (opcode == kOpEntryPoint && count > 2) {
            g.entry = spirv[i + 2];
        } else if (opcode == kOpName && count > 2) {
            std::string name;
            for (size_t w = i + 2; w < i + count; ++w) {
                bool end = false;
                for (u32 b = 0; b < 4; ++b) {
                    const char ch = static_cast<char>((spirv[w] >> (8 * b)) & 0xFF);
                    if (ch == '\0') {
                        end = true;
                        break;
                    }
                    name += ch;
                }
                if (end) break;
            }
            g.names[spirv[i + 1]] = name.substr(0, name.find('('));
        } else if (opcode == kOpFunction && count > 2) {
            current = spirv[i + 2];
            g.functions.push_back(current);
        } else if (opcode == kOpFunctionEnd) {
            current = 0;
        } else if (opcode == kOpFunctionCall && count > 3 && current != 0) {
            ++g.calls[current][spirv[i + 3]];
        }
        i += count;
    }
    return g;
}

u64 WeightedCalls(const CallGraph& g, u32 from, u32 to, std::set<u32>& on_path) {
    if (from == to) return 1;
    if (!on_path.insert(from).second) return 0;
    u64 total = 0;
    const auto it = g.calls.find(from);
    if (it != g.calls.end()) {
        for (const auto& [callee, sites] : it->second) total += sites * WeightedCalls(g, callee, to, on_path);
    }
    on_path.erase(from);
    return total;
}

}  // namespace

TEST_CASE("generator/the_ubershaders_lighting_calls_do_not_grow_with_the_lights") {
    const DeviceProfile profile{};
    for (ShaderPass pass : {ShaderPass::Normal, ShaderPass::ShadowWrite}) {
        const Generated uber = GenerateUber(pass, profile);
        REQUIRE_MESSAGE(uber.ok(), uber.error);
        const CallGraph g = ReadCallGraph(uber.spirv);
        REQUIRE(g.entry != 0);
        auto reached = [&](const char* name) {
            const std::vector<u32> ids = g.Named(name);
            REQUIRE_MESSAGE(ids.size() == 1, name << ": " << ids.size() << " functions of that name");
            std::set<u32> on_path;
            return WeightedCalls(g, g.entry, ids[0], on_path);
        };
        // One call of the lookup per table in the light loop body (the spot and distance tables, D0, the three reflection tables, D1
        // and Fresnel: eight), whatever the number of lights; the bump and shadow samples are four calls at the top level (one per
        // unit), and core's own four (the stages): two paths to each unit.
        CHECK(reached("mh4n_lut") == 8);
        CHECK(reached("LookupLightingLUT") == 8);
        for (const char* unit : {"sampleTexUnit0", "sampleTexUnit1", "sampleTexUnit2", "sampleTexUnit3"}) CHECK_MESSAGE(reached(unit) == 2, unit);
        CHECK(reached("ProcTex") == 2);
        CHECK(reached("shadowTexture") == 2);
    }
}

// ---- the tables ------------------------------------------------------------------------------------------------------------

TEST_CASE("lighting/the_tables_are_uploaded_by_need_whatever_the_dirty_flags_say") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    auto rings = TableRings::Create(*backend);
    REQUIRE_MESSAGE(rings.ok(), rings.error());

    auto state = BaseState();
    FillLuts(*state);
    state->lighting.lut_dirty = 0;  // no flag says anything changed
    state->fog.lut_dirty = false;

    auto upload = [&](const TableDirty& need, FragmentBlock& block, UniformDirty& dirty) {
        rhi::Recorder rec = backend->BeginRecording();
        auto summary = UploadTables(*state, **rings, block, dirty, need, FeatureRegistry::Global());
        REQUIRE_MESSAGE(summary.ok(), summary.error());
        backend->WaitIdle(backend->Submit(std::move(rec)));
        return *summary;
    };
    auto offset = [](FragmentBlock& block, u32 table) { return (&block.uniforms.lighting_lut_offset[table / 4].x)[table % 4]; };

    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    for (u32 t = 0; t < 24; ++t) (&block.uniforms.lighting_lut_offset[t / 4].x)[t % 4] = -1;
    UniformDirty dirty;

    // need names tables 3 and 16: exactly those two are written, each offset from its slice, in table order; the flags are not read.
    TableDirty need;
    need.lighting = (1u << 3) | (1u << 16);
    UploadSummary summary = upload(need, block, dirty);
    CHECK(summary.uploaded);
    CHECK(dirty.changed);
    for (u32 t = 0; t < 24; ++t) {
        if (t == 3 || t == 16) continue;
        CHECK_MESSAGE(offset(block, t) == -1, "table " << t << " was written");
    }
    const int first = offset(block, 3), second = offset(block, 16);
    CHECK(first >= 0);
    CHECK(second == first + 256);  // 256 entries of two floats, in elements of the R32G32 view
    CHECK(block.uniforms.fog_lut_offset == 0);

    // The same request again (no flag, no state of its own): written again, and the offset can only have moved forward.
    dirty = {};
    summary = upload(need, block, dirty);
    CHECK(summary.uploaded);
    CHECK(dirty.changed);
    CHECK(offset(block, 3) >= first);

    // An empty request opens no session and touches nothing.
    FragmentBlock untouched;
    std::memset(&untouched, 0xAB, sizeof(untouched));
    FragmentBlock copy = untouched;
    dirty = {};
    summary = upload(TableDirty{}, untouched, dirty);
    CHECK_FALSE(summary.uploaded);
    CHECK_FALSE(dirty.changed);
    CHECK(std::memcmp(&untouched, &copy, sizeof(copy)) == 0);

    // Every lighting table at once: 24 slices, 2 KiB each, ascending.
    need.lighting = pica::Lighting::kAllDirty;
    summary = upload(need, block, dirty);
    CHECK(summary.uploaded);
    for (u32 t = 1; t < 24; ++t) CHECK(offset(block, t) == offset(block, 0) + static_cast<int>(256 * t));
}

// ---- the formulas, on lavapipe -------------------------------------------------------------------------------------------------

TEST_CASE("lighting/lit_draws_match_the_software_renderers_formulas_in_both_modes") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    const std::vector<LitCase> cases = FormulaCases();
    u32 index = 0;
    for (const LitCase& c : cases) {
        INFO(c.label);
        spec_rig.BindUnitColours(c.units);
        uber_rig.BindUnitColours(c.units);
        for (Path path : {Path::Batch, Path::Translated}) {
            // The translated path through the geometry stage on the full device; spec on the batch path and uber on both keeps the
            // number of pipelines down.
            if (path == Path::Translated && (index % 3) != 0 && c.lit) continue;
            for (u32 source : {1u, 2u}) {
                if (path == Path::Translated && source == 2 && (index % 3) != 0) continue;
                DrawSpec draw = DrawOf(c, source);
                sw::g_margin = 1e9;
                const auto [primary, secondary] = Reference(c, *draw.state);
                const sw::Rgba want = source == 1 ? primary : secondary;
                const bool margin_ok = sw::g_margin > 1e-3 || !c.lit;
                CHECK_MESSAGE(margin_ok, c.label << ": a signed table input within 1e-3 of zero: " << sw::g_margin);
                const std::vector<u8> uber = uber_rig.Render({draw}, path);
                const std::vector<u8> spec = spec_rig.Render({draw}, path);
                std::string why;
                CHECK_MESSAGE(AllNear(uber, want, kTolerance, &why), c.label << " (uber, source " << source << ", path " << static_cast<int>(path) << "): " << why);
                CHECK_MESSAGE(AllNear(spec, want, kTolerance, &why), c.label << " (specialised, source " << source << ", path " << static_cast<int>(path) << "): " << why);
                CHECK_MESSAGE(SameWithin(spec, uber, 1), c.label << ": the specialised shader and the ubershader differ");
                CHECK_MESSAGE(uber_rig.last.fallback, c.label);
                CHECK_MESSAGE(!spec_rig.last.fallback, c.label);
                if (path == Path::Translated && c.lit && backend->Caps().geometry_shader) {
                    CHECK_MESSAGE(spec_rig.last.fixup, c.label << ": the translated lit draw ran without the fix-up stage");
                    CHECK_MESSAGE(uber_rig.last.fixup, c.label);
                }
            }
        }
        ++index;
    }
    // The fixture exercises what it should: the Fresnel alpha of configuration 7 is neither 0 nor 255 in either colour, the
    // reflection tables make the three channels of the secondary colour differ, and the lights make the diffuse colour differ from
    // the ambient alone.
    {
        const LitCase full = Full(8);
        DrawSpec draw = DrawOf(full, 1);
        const auto [primary, secondary] = Reference(full, *draw.state);
        CHECK((primary[3] > 5 && primary[3] < 250));
        CHECK((secondary[3] > 5 && secondary[3] < 250));
        CHECK(secondary[0] != secondary[1]);
        CHECK(secondary[1] != secondary[2]);
        CHECK(primary[0] > 30);
    }
    // The ubershader is one pipeline for every case of a path (the key is in the uniforms).
    CHECK(uber_rig.built.size() <= 3);
    // The translated lit draws ran with the fixed geometry stage on a device that has geometry shaders, and without it otherwise.
    if (backend->Caps().geometry_shader) {
        CHECK(spec_rig.fixups >= 6);
        CHECK(uber_rig.fixups >= 6);
        bool geometry_pipeline = false;
        for (const native::PipelineKey& key : uber_rig.built) geometry_pipeline = geometry_pipeline || key.fixup == native::FixupStage::FixedGeometry;
        CHECK(geometry_pipeline);
    } else {
        CHECK(spec_rig.fixups == 0);
        CHECK(uber_rig.fixups == 0);
    }
}

// ---- the quaternion fix-up ---------------------------------------------------------------------------------------------------

namespace {

// A single directional light that leans on the normal, so that a flipped quaternion changes the colour a lot.
LitCase FlipCase() {
    LitCase c = Basic();
    c.label = "quaternions that need the flip";
    c.lights[0] = Directional({0.0f, -0.875f, 0.375f}, Color10(220, 180, 140), Color10(4, 4, 4), Color10(255, 255, 255));
    return c;
}

// The reference with the software renderer's order: vertex 1 and vertex 2 are flipped against vertex 0 FIRST, then the quaternion
// is interpolated (barycentric weights at the pixel centre; w = 1 everywhere, so perspective-correct is linear), then normalised.
std::array<float, 4> FlipQuaternionIfOpposite(std::array<float, 4> a, const std::array<float, 4>& b) {
    float dot = 0;
    for (int i = 0; i < 4; ++i) dot += a[i] * b[i];
    if (dot < 0) {
        for (float& x : a) x *= -1.0f;
    }
    return a;
}

std::vector<u8> ReferenceFlip(const LitCase& c, const pica::PicaState& state, const std::array<std::array<float, 4>, 3>& quats, bool flip, u32 source) {
    std::array<std::array<float, 4>, 3> q = quats;
    if (flip) {
        q[1] = FlipQuaternionIfOpposite(q[1], q[0]);
        q[2] = FlipQuaternionIfOpposite(q[2], q[0]);
    }
    std::vector<u8> out(kSize * kSize * 4);
    std::array<sw::Rgba, 4> tex = {};
    for (u32 u = 0; u < 3; ++u) tex[u] = c.units[u];
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            // The triangle (-1,-1), (3,-1), (-1,3): the weights of vertices 1 and 2 are the pixel centre's offsets from the corner
            // over 16 (the target spans 8 pixels over 2 units of the 4-unit legs).
            const float l1 = (static_cast<float>(x) + 0.5f) / 16.0f, l2 = (static_cast<float>(y) + 0.5f) / 16.0f, l0 = 1.0f - l1 - l2;
            std::array<float, 4> interpolated;
            for (int i = 0; i < 4; ++i) interpolated[i] = l0 * q[0][i] + l1 * q[1][i] + l2 * q[2][i];
            const auto [primary, secondary] = sw::ComputeFragmentsColors(state, interpolated, sw::V3{c.view[0], c.view[1], c.view[2]}, tex);
            const sw::Rgba v = source == 1 ? primary : secondary;
            for (int k = 0; k < 4; ++k) out[(y * kSize + x) * 4 + k] = static_cast<u8>(v[k]);
        }
    }
    return out;
}

bool MaxDifferenceAtMost(const std::vector<u8>& a, const std::vector<u8>& b, int tolerance, int* worst = nullptr) {
    int max = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) max = std::max(max, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    if (worst != nullptr) *worst = max;
    return a.size() == b.size() && max <= tolerance;
}

// The largest difference of any channel of any pixel.
int MaxDifference(const std::vector<u8>& a, const std::vector<u8>& b) {
    int worst = 0;
    MaxDifferenceAtMost(a, b, 0, &worst);
    return worst;
}

}  // namespace

TEST_CASE("lighting/the_quaternion_fixup_is_the_fixed_geometry_shader_and_without_it_a_flipped_draw_differs") {
    auto full = test::RequireBackend();
    if (!full) return;
    rhi::BackendOptions masked_options;
    masked_options.mask = rhi::CapabilityMask{}.With(rhi::Cap::GeometryShader);
    auto masked = test::RequireBackend(masked_options);
    if (!masked) return;

    // The masked backend really has no geometry shaders; the full one has them unless the whole run is masked (MH4N_RHI_MASK).
    CHECK_FALSE(masked->Caps().geometry_shader);
    const bool env_masked = full->Caps().masked.Has(rhi::Cap::GeometryShader);
    if (!env_masked) {
        CHECK(full->Caps().geometry_shader);
    } else {
        CHECK_FALSE(full->Caps().geometry_shader);
    }
    const bool fixup_available = full->Caps().geometry_shader;

    const LitCase c = FlipCase();
    // Vertex 0 the identity; vertex 1 the opposite of a 20 degree rotation about x (the same rotation with the sign of the whole
    // quaternion flipped); vertex 2 the identity. The flip makes the interpolation a smooth turn; without it the quaternion passes
    // through nearly zero in the middle and the normal turns over.
    const float s = std::sin(0.17453292519943295f / 2.0f), k = std::cos(0.17453292519943295f / 2.0f);
    const std::array<std::array<float, 4>, 3> quats = {{{0, 0, 0, 1}, {-s, 0, 0, -k}, {0, 0, 0, 1}}};
    DrawSpec draw;
    {
        std::shared_ptr<pica::PicaState> state = BaseState(1);
        SetVertexProgram(*state);
        ApplyCase(c, *state);
        draw.state = state;
        draw.tri = FullTriangle(quats, c.view);
    }
    const std::vector<u8> flipped = ReferenceFlip(c, *draw.state, quats, true, 1);
    const std::vector<u8> unflipped = ReferenceFlip(c, *draw.state, quats, false, 1);
    // The fixture: the two references differ a lot (so that "differs" below means something).
    REQUIRE(MaxDifference(flipped, unflipped) > 40);

    Rig full_spec(full, false), full_uber(full, true), masked_spec(masked, false), masked_uber(masked, true);
    for (Rig* rig : {&full_spec, &full_uber, &masked_spec, &masked_uber}) rig->BindUnitColours(c.units);

    // The CPU path does the flip in the batch (N2-37): equal to the flipped reference on both devices, in both modes.
    for (Rig* rig : {&full_spec, &full_uber, &masked_spec, &masked_uber}) {
        const std::vector<u8> pixels = rig->Render({draw}, Path::Batch);
        int worst = 0;
        CHECK_MESSAGE(MaxDifferenceAtMost(pixels, flipped, kTolerance, &worst), "the CPU path's pixels differ from the flipped reference by " << worst);
        CHECK_FALSE(rig->last.fixup);  // a trivial vertex stage: no fix-up stage
    }

    // The translated path on the device as it is: the fixed geometry shader, equal to the flipped reference.
    std::vector<u8> as_is_pixels[2];
    int index = 0;
    for (Rig* rig : {&full_spec, &full_uber}) {
        as_is_pixels[index] = rig->Render({draw}, Path::Translated);
        int worst = 0;
        if (fixup_available) {
            CHECK_MESSAGE(MaxDifferenceAtMost(as_is_pixels[index], flipped, kTolerance, &worst),
                          "the translated path with the fix-up differs from the flipped reference by " << worst);
            CHECK(rig->last.fixup);
            bool geometry_pipeline = false;
            for (const native::PipelineKey& key : rig->built) geometry_pipeline = geometry_pipeline || key.fixup == native::FixupStage::FixedGeometry;
            CHECK(geometry_pipeline);
        } else {
            // The whole run is masked: this is the masked route.
            CHECK_FALSE(rig->last.fixup);
        }
        ++index;
    }
    CHECK(SameWithin(as_is_pixels[0], as_is_pixels[1], 1));

    // The translated path without geometry shaders: no fix-up, and the draw differs from the flipped reference (it matches the
    // reference that never flips, which is what an interpolation of the vertex quaternions as they are gives).
    index = 0;
    for (Rig* rig : {&masked_spec, &masked_uber}) {
        const std::vector<u8> pixels = rig->Render({draw}, Path::Translated);
        CHECK_FALSE(rig->last.fixup);
        const int worst = MaxDifference(pixels, flipped);
        CHECK_MESSAGE(worst > 40, "the masked draw (no fix-up stage) should differ from the flipped reference, by " << worst);
        int unflipped_worst = 0;
        CHECK_MESSAGE(MaxDifferenceAtMost(pixels, unflipped, kTolerance, &unflipped_worst), "the masked draw differs from the unflipped reference by " << unflipped_worst);
        if (fixup_available) {
            CHECK_MESSAGE(MaxDifference(pixels, as_is_pixels[index]) > 40, "the masked draw should differ from the draw with the fix-up");
        }
        ++index;
    }

    // A draw whose quaternions need no flip is the same with and without the stage.
    DrawSpec calm = draw;
    calm.tri = FullTriangle({c.quat, c.quat, c.quat}, c.view);
    const std::vector<u8> calm_full = full_uber.Render({calm}, Path::Translated);
    const std::vector<u8> calm_masked = masked_uber.Render({calm}, Path::Translated);
    CHECK(SameWithin(calm_full, calm_masked, 1));
}
