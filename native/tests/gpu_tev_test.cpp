// Derived from Azahar src/video_core/renderer_software/sw_rasterizer.cpp (RasterizerSoftware::WriteTevConfig,
// DoAlphaTest, PixelColor, WriteFog), sw_texturing.cpp (GetColorModifier, GetAlphaModifier, ColorCombine, AlphaCombine),
// sw_framebuffer.cpp (EvaluateBlendEquation, LogicOp), src/video_core/pica/regs_texturing.h (TevStageConfig::
// GetColorMultiplier) and externals/nihstro/include/nihstro/shader_bytecode.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (the first four are GPL-2.0-or-later: each says "Licensed under GPLv2 or any
// later version"; shader_bytecode.h belongs to the submodule externals/nihstro that Azahar pins at
// f4d8659decbfe5d234f04134b5002b82dc515a44 and is NOT GPL: its licence is the BSD 3-clause licence of
// externals/nihstro/license.txt, reproduced below because it asks for that: the instruction encoder below places the
// fields where shader_bytecode.h puts them); the shader generator's text is checked against
// src/video_core/shader/generator/glsl_fs_shader_gen.cpp (GPL-3.0-or-later: "Licensed under GPLv3 or any later version"),
// so this file is GPL-3.0-or-later.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp)
// Copyright 2017 Citra Emulator Project (sw_texturing.cpp, sw_framebuffer.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
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
// The fragment generator, the ubershader, TEV and the alpha test (N2-08), on whatever Vulkan device the machine has.
// The device is chosen as every GPU test does (MH4N_VULKAN_DEVICE); a case that needs one skips with a message without it
// and fails under MH4N_REQUIRE_VULKAN=1.
//
// WHERE THE EXPECTED PIXELS COME FROM. They are computed here, on the CPU, by an independent implementation of the SOFTWARE
// renderer's formulas (`sw` below: WriteTevConfig and the helpers it calls, with Azahar's integer arithmetic on u8 values,
// then DoAlphaTest and PixelColor's rules for the output), never from the generator's output. The generator follows
// Azahar's GLSL (float arithmetic with a byte rounding at each stage), so the two agree within the rounding, not bit for
// bit. The oracle deviates from sw_rasterizer.cpp in two places, both because the GPU path is Azahar's GLSL
// (glsl_fs_shader_gen.cpp), which is what the game was checked against: (1) at stage 0 an alpha source that says Previous
// is alpha_source3, as AppendAlphaModifier forces it (the software renderer forces only the colour sources); (2) the
// software renderer has no pass-through skip, so a stage-0 Replace with source 1 = Previous and source 3 != Previous gives
// source 3 there and nothing (the previous output) in the GLSL: no case here has that shape. Each operation has the
// tolerance the formulas allow (`Tolerance`, in 8-bit steps), worked out from the two arithmetics and measured on lavapipe:
//   - Replace, Add, Subtract, AddThenMultiply's clamp: exact;
//   - Modulate, Lerp, MultiplyThenAdd, AddThenMultiply, AddSigned (the software renderer subtracts 128/255 where the GLSL
//     subtracts 0.5): one step (integer division truncates where the GLSL rounds);
//   - Dot3_RGB and Dot3_RGBA: three steps (the software renderer divides by 256 per channel; its own comment says
//     "Worst case scenario seems to yield a +/-3 error");
//   - with a scale of 2 or 4, the stage's tolerance times the scale.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
#include "mh4n/options.h"
#include "mh4n/registry.h"
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
namespace test = mh4n::test;

namespace {

// ---- keys -------------------------------------------------------------------------------------------------------------

// Azahar's TevStageConfig::Source and the modifiers, for building stage words.
enum Source : u32 {
    kPrimaryColor = 0, kPrimaryFragmentColor = 1, kSecondaryFragmentColor = 2, kTexture0 = 3, kTexture1 = 4, kTexture2 = 5,
    kTexture3 = 6, kPreviousBuffer = 13, kConstant = 14, kPrevious = 15,
};
enum ColorMod : u32 {
    kSourceColor = 0, kOneMinusSourceColor = 1, kSourceAlpha = 2, kOneMinusSourceAlpha = 3, kSourceRed = 4,
    kOneMinusSourceRed = 5, kSourceGreen = 8, kOneMinusSourceGreen = 9, kSourceBlue = 12, kOneMinusSourceBlue = 13,
};
enum AlphaMod : u32 {
    kAlphaSourceAlpha = 0, kAlphaOneMinusSourceAlpha = 1, kAlphaSourceRed = 2, kAlphaOneMinusSourceRed = 3,
    kAlphaSourceGreen = 4, kAlphaOneMinusSourceGreen = 5, kAlphaSourceBlue = 6, kAlphaOneMinusSourceBlue = 7,
};
enum Op : u32 {
    kReplace = 0, kModulate = 1, kAdd = 2, kAddSigned = 3, kLerp = 4, kSubtract = 5, kDot3Rgb = 6, kDot3Rgba = 7,
    kMultiplyThenAdd = 8, kAddThenMultiply = 9,
};

struct Stage {
    u32 color_src[3] = {kPrevious, kPrevious, kPrevious};
    u32 color_mod[3] = {0, 0, 0};
    u32 alpha_src[3] = {kPrevious, kPrevious, kPrevious};
    u32 alpha_mod[3] = {0, 0, 0};
    u32 color_op = kReplace, alpha_op = kReplace;
    u32 color_scale = 0, alpha_scale = 0;

    TevStageConfigRaw Raw() const {
        TevStageConfigRaw raw;
        raw.sources_raw = color_src[0] | (color_src[1] << 4) | (color_src[2] << 8) | (alpha_src[0] << 16) | (alpha_src[1] << 20) |
                          (alpha_src[2] << 24);
        raw.modifiers_raw = color_mod[0] | (color_mod[1] << 4) | (color_mod[2] << 8) | (alpha_mod[0] << 12) | (alpha_mod[1] << 16) |
                            (alpha_mod[2] << 20);
        raw.ops_raw = color_op | (alpha_op << 16);
        raw.scales_raw = color_scale | (alpha_scale << 16);
        return raw;
    }
};

// The stage a draw configures when the game leaves it at its default: pass-through.
FragmentKey DefaultKey() {
    FragmentKey key;
    key.framebuffer.set_alpha_test_func(re::CompareFunc::Always);
    key.framebuffer.set_logic_op(re::LogicOp::Copy);
    key.framebuffer.requested_logic_op = re::LogicOp::Copy;
    key.framebuffer.set_depthmap_enable(re::DepthBuffering::ZBuffering);
    key.texture.set_texture0_type(re::TextureType::Texture2D);
    return key;
}

const DeviceProfile kFullProfile = [] {
    DeviceProfile p;
    p.logic_op = true;
    p.custom_border_color = true;
    p.interlock = true;
    p.geometry_shader = true;
    return p;
}();
const DeviceProfile kLeanProfile = DeviceProfile{};  // every emulation on

}  // namespace

// ---- the generator, without a device ------------------------------------------------------------------------------------

TEST_CASE("generator/the_layout_constants_agree_with_the_composition") {
    // The generator spells the bindings of native/bindings.h as numbers; this is the check that they are the same.
    const std::string uber = GenerateUberGlsl(ShaderPass::Normal, kFullProfile);
    auto has = [&](u32 set, u32 binding, const char* name) {
        const std::string want = "layout(set = " + std::to_string(set) + ", binding = " + std::to_string(binding) + ")";
        const size_t at = uber.find(want);
        return at != std::string::npos && uber.find(name, at) < uber.find('\n', at);
    };
    CHECK(has(native::set::kUniforms, native::binding::kLutLf, "texture_buffer_lut_lf"));
    CHECK(has(native::set::kUniforms, native::binding::kLutRg, "texture_buffer_lut_rg"));
    CHECK(has(native::set::kUniforms, native::binding::kLutRgba, "texture_buffer_lut_rgba"));
    CHECK(has(native::set::kTextures, native::binding::kTex0, "tex0;"));
    CHECK(has(native::set::kTextures, native::binding::kTex0Cube, "tex0_cube;"));
    CHECK(has(native::set::kTextures, native::binding::kTex0Shadow, "tex0_shadow;"));
    CHECK(has(native::set::kTextures, native::binding::kSampler0, "samp0;"));
    CHECK(has(native::set::kTextures, native::binding::kTex1, "tex1;"));
    CHECK(has(native::set::kTextures, native::binding::kSampler1, "samp1;"));
    CHECK(has(native::set::kTextures, native::binding::kTex2, "tex2;"));
    CHECK(has(native::set::kTextures, native::binding::kSampler2, "samp2;"));
    CHECK(has(native::set::kTextures, native::binding::kTexColor, "tex_color;"));
    CHECK(uber.find("layout (set = 0, binding = 2, std140) uniform fs_data") != std::string::npos);
    CHECK(native::binding::kFragmentBlock == 2);
    const std::string shadow = GenerateUberGlsl(ShaderPass::ShadowWrite, kFullProfile);
    CHECK(shadow.find("layout(set = 2, binding = 0, r32ui) uniform uimage2D shadow_buffer;") != std::string::npos);
    CHECK(native::set::kShadow == 2);
    CHECK(native::binding::kShadowOut == 0);
    CHECK(uber.find("shadow_buffer") == std::string::npos);  // the normal pass declares no storage image
}

TEST_CASE("generator/the_varyings_are_declared_at_the_locations_of_azahars_DefineInterface") {
    const std::string glsl = GenerateSpecialisedGlsl(DefaultKey(), kFullProfile, ShaderPass::Normal);
    for (const char* line :
         {"layout (location = 1) in vec4 primary_color;", "layout (location = 2) in vec2 texcoord0;",
          "layout (location = 3) in vec2 texcoord1;", "layout (location = 4) in vec2 texcoord2;",
          "layout (location = 5) in float texcoord0_w;", "layout (location = 6) in vec4 normquat;",
          "layout (location = 7) in vec3 view;", "layout (location = 0) out vec4 color;"}) {
        CHECK_MESSAGE(glsl.find(line) != std::string::npos, line);
    }
}

TEST_CASE("generator/every_helper_compiles_in_both_modes_and_both_passes") {
    for (const DeviceProfile& profile : {kFullProfile, kLeanProfile}) {
        for (ShaderPass pass : {ShaderPass::Normal, ShaderPass::ShadowWrite}) {
            Generated uber = GenerateUber(pass, profile);
            INFO(uber.error);
            CHECK(uber.ok());
            Generated spec = GenerateSpecialised(DefaultKey(), profile, pass);
            INFO(spec.error);
            CHECK(spec.ok());
        }
    }
}

// ---- the software renderer's formulas, on the CPU (the oracle) --------------------------------------------------------------
//
// sw_rasterizer.cpp's WriteTevConfig and the helpers of sw_texturing.cpp, with Azahar's arithmetic: u8 values, products and
// quotients in int, the division truncating. Nothing here calls the generator or reads its text.

namespace sw {

using V3 = std::array<int, 3>;
using V4 = std::array<int, 4>;  // r, g, b, a in 0..255

V3 ColorModifier(u32 factor, const V4& v) {  // GetColorModifier
    switch (factor) {
    case kSourceColor: return {v[0], v[1], v[2]};
    case kOneMinusSourceColor: return {255 - v[0], 255 - v[1], 255 - v[2]};
    case kSourceAlpha: return {v[3], v[3], v[3]};
    case kOneMinusSourceAlpha: return {255 - v[3], 255 - v[3], 255 - v[3]};
    case kSourceRed: return {v[0], v[0], v[0]};
    case kOneMinusSourceRed: return {255 - v[0], 255 - v[0], 255 - v[0]};
    case kSourceGreen: return {v[1], v[1], v[1]};
    case kOneMinusSourceGreen: return {255 - v[1], 255 - v[1], 255 - v[1]};
    case kSourceBlue: return {v[2], v[2], v[2]};
    case kOneMinusSourceBlue: return {255 - v[2], 255 - v[2], 255 - v[2]};
    }
    return {0, 0, 0};
}

int AlphaModifier(u32 factor, const V4& v) {  // GetAlphaModifier
    switch (factor) {
    case kAlphaSourceAlpha: return v[3];
    case kAlphaOneMinusSourceAlpha: return 255 - v[3];
    case kAlphaSourceRed: return v[0];
    case kAlphaOneMinusSourceRed: return 255 - v[0];
    case kAlphaSourceGreen: return v[1];
    case kAlphaOneMinusSourceGreen: return 255 - v[1];
    case kAlphaSourceBlue: return v[2];
    case kAlphaOneMinusSourceBlue: return 255 - v[2];
    }
    return 0;
}

V3 ColorCombine(u32 op, const std::array<V3, 3>& in) {  // ColorCombine
    V3 r{};
    for (int c = 0; c < 3; ++c) {
        const int a = in[0][c], b = in[1][c], d = in[2][c];
        switch (op) {
        case kReplace: r[c] = a; break;
        case kModulate: r[c] = (a * b) / 255; break;
        case kAdd: r[c] = std::min(255, a + b); break;
        case kAddSigned: r[c] = std::clamp(a + b - 128, 0, 255); break;
        case kLerp: r[c] = (a * d + b * (255 - d)) / 255; break;
        case kSubtract: r[c] = std::max(0, a - b); break;
        case kMultiplyThenAdd: r[c] = std::min(255, (a * b + 255 * d) / 255); break;
        case kAddThenMultiply: r[c] = (std::min(255, a + b) * d) / 255; break;
        default: break;
        }
    }
    if (op == kDot3Rgb || op == kDot3Rgba) {
        // Not fully accurate. Worst case scenario seems to yield a +/-3 error (the comment of sw_texturing.cpp).
        int result = ((in[0][0] * 2 - 255) * (in[1][0] * 2 - 255) + 128) / 256 +
                     ((in[0][1] * 2 - 255) * (in[1][1] * 2 - 255) + 128) / 256 +
                     ((in[0][2] * 2 - 255) * (in[1][2] * 2 - 255) + 128) / 256;
        result = std::clamp(result, 0, 255);
        r = {result, result, result};
    }
    return r;
}

int AlphaCombine(u32 op, const std::array<int, 3>& in) {  // AlphaCombine
    switch (op) {
    case kReplace: return in[0];
    case kModulate: return in[0] * in[1] / 255;
    case kAdd: return std::min(255, in[0] + in[1]);
    case kAddSigned: return std::clamp(in[0] + in[1] - 128, 0, 255);
    case kLerp: return (in[0] * in[2] + in[1] * (255 - in[2])) / 255;
    case kSubtract: return std::max(0, in[0] - in[1]);
    case kMultiplyThenAdd: return std::min(255, (in[0] * in[1] + 255 * in[2]) / 255);
    case kAddThenMultiply: return (std::min(255, in[0] + in[1]) * in[2]) / 255;
    default: return 0;
    }
}

// GetColorMultiplier / GetAlphaMultiplier.
int Multiplier(u32 scale) {
    return scale < 3 ? (1 << scale) : 1;
}

struct Inputs {
    V4 primary{};
    V4 primary_fragment{};     // lighting is off (or a stub): zero
    V4 secondary_fragment{};
    std::array<V4, 4> texture{{{255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}}};
    // N2-08's texture units are stubs that give opaque white: that is what the generator's output must equal, not the
    // software renderer's zero for a unit nobody enabled.
    std::array<V4, 6> constant{};
    V4 combiner_buffer_color{};
    u32 buffer_input = 0;  // update_mask_rgb | update_mask_a << 4 (the key's combiner_buffer_input)
};

// WriteTevConfig.
V4 Tev(const std::array<Stage, 6>& stages, const Inputs& in) {
    V4 combiner_output = {0, 0, 0, 0};
    V4 combiner_buffer = {0, 0, 0, 0};
    V4 next_combiner_buffer = in.combiner_buffer_color;
    for (u32 index = 0; index < 6; ++index) {
        const Stage& stage = stages[index];
        auto get_source = [&](u32 source) -> V4 {
            switch (source) {
            case kPrimaryColor: return in.primary;
            case kPrimaryFragmentColor: return in.primary_fragment;
            case kSecondaryFragmentColor: return in.secondary_fragment;
            case kTexture0: return in.texture[0];
            case kTexture1: return in.texture[1];
            case kTexture2: return in.texture[2];
            case kTexture3: return in.texture[3];
            case kPreviousBuffer: return combiner_buffer;
            case kConstant: return in.constant[index];
            case kPrevious: return combiner_output;
            }
            return {0, 0, 0, 0};
        };
        const u32 source1 = index == 0 && stage.color_src[0] == kPrevious ? stage.color_src[2] : stage.color_src[0];
        const u32 source2 = index == 0 && stage.color_src[1] == kPrevious ? stage.color_src[2] : stage.color_src[1];
        const std::array<V3, 3> color_result = {ColorModifier(stage.color_mod[0], get_source(source1)),
                                                ColorModifier(stage.color_mod[1], get_source(source2)),
                                                ColorModifier(stage.color_mod[2], get_source(stage.color_src[2]))};
        const V3 color_output = ColorCombine(stage.color_op, color_result);
        int alpha_output;
        if (stage.color_op == kDot3Rgba) {
            alpha_output = color_output[0];
        } else {
            // DEVIATION from sw_rasterizer.cpp, which does not force the alpha sources: the GPU path is Azahar's GLSL
            // (AppendAlphaModifier), which takes alpha_source3 for a Previous alpha source at stage 0.
            const u32 alpha_source1 = index == 0 && stage.alpha_src[0] == kPrevious ? stage.alpha_src[2] : stage.alpha_src[0];
            const u32 alpha_source2 = index == 0 && stage.alpha_src[1] == kPrevious ? stage.alpha_src[2] : stage.alpha_src[1];
            const std::array<int, 3> alpha_result = {AlphaModifier(stage.alpha_mod[0], get_source(alpha_source1)),
                                                     AlphaModifier(stage.alpha_mod[1], get_source(alpha_source2)),
                                                     AlphaModifier(stage.alpha_mod[2], get_source(stage.alpha_src[2]))};
            alpha_output = AlphaCombine(stage.alpha_op, alpha_result);
        }
        for (int c = 0; c < 3; ++c) {
            combiner_output[c] = std::min(255, color_output[c] * Multiplier(stage.color_scale));
        }
        combiner_output[3] = std::min(255, alpha_output * Multiplier(stage.alpha_scale));
        combiner_buffer = next_combiner_buffer;
        if (index < 4 && (in.buffer_input & (1u << index))) {  // TevStageUpdatesCombinerBufferColor
            for (int c = 0; c < 3; ++c) next_combiner_buffer[c] = combiner_output[c];
        }
        if (index < 4 && ((in.buffer_input >> 4) & (1u << index))) {  // ...Alpha
            next_combiner_buffer[3] = combiner_output[3];
        }
    }
    return combiner_output;
}

// DoAlphaTest: true when the fragment is kept.
bool AlphaTest(re::CompareFunc func, int alpha, int ref) {
    switch (func) {
    case re::CompareFunc::Never: return false;
    case re::CompareFunc::Always: return true;
    case re::CompareFunc::Equal: return alpha == ref;
    case re::CompareFunc::NotEqual: return alpha != ref;
    case re::CompareFunc::LessThan: return alpha < ref;
    case re::CompareFunc::LessThanOrEqual: return alpha <= ref;
    case re::CompareFunc::GreaterThan: return alpha > ref;
    case re::CompareFunc::GreaterThanOrEqual: return alpha >= ref;
    }
    return false;
}

// The tolerance of an operation, in 8-bit steps, per the analysis in the header of this file.
int Tolerance(u32 op) {
    switch (op) {
    case kReplace:
    case kAdd:
    case kSubtract: return 0;
    case kDot3Rgb:
    case kDot3Rgba: return 3;
    default: return 1;
    }
}

}  // namespace sw

// ---- the rig: draws through the composition with the real generator -------------------------------------------------------

namespace {

constexpr u32 kSize = 8;  // the targets are 8x8

using cpuvertex::OutputVertex;

OutputVertex Vertex(float x, float y, float z, float w, std::array<float, 4> color) {
    OutputVertex v{};
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = z; v.pos[3] = w;
    v.quat[0] = 0; v.quat[1] = 0; v.quat[2] = 0; v.quat[3] = 1;
    for (int i = 0; i < 4; ++i) v.color[i] = color[i];
    v.tc0_w = 1.0f;
    return v;
}

float B(int byte) { return static_cast<float>(byte) / 255.0f; }

// One triangle that covers the whole target, with a constant colour (bytes) and a PICA z (in [-1, 0]) and w.
struct Tri {
    OutputVertex v[3];
};
Tri FullTriangle(std::array<int, 4> color, float z = -0.5f, float w = 1.0f) {
    const std::array<float, 4> c = {B(color[0]), B(color[1]), B(color[2]), B(color[3])};
    // clip-space coordinates multiplied by w, so that the screen position does not depend on w
    return Tri{{Vertex(-1 * w, -1 * w, z * w, w, c), Vertex(3 * w, -1 * w, z * w, w, c), Vertex(-1 * w, 3 * w, z * w, w, c)}};
}

rhi::VertexLayout BatchLayout() {
    return cpuvertex::MakeBatchVertexLayout();
}

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

// What one draw of a test sets.
struct DrawSpec {
    FragmentKey key = DefaultKey();
    FragmentUniforms uniforms{};
    rhi::FixedFunctionState state{};
    Tri tri = FullTriangle({255, 255, 255, 255});
    fbops::DynamicState dynamic{};
    bool use_depth = false;
};

struct Rig {
    std::shared_ptr<rhi::Backend> backend;
    DeviceProfile profile;
    std::unique_ptr<native::PipelineBuilder> builder;
    std::unique_ptr<native::DefaultPipelineProvider> provider;
    std::unique_ptr<native::DrawComposer> composer;
    std::unique_ptr<FragmentUniformRing> uniforms;
    std::unique_ptr<TableRings> tables;
    std::unique_ptr<rhi::Ring> vertex_ring, vs_ring;
    std::shared_ptr<rhi::Image> color, depth;
    std::vector<native::PipelineKey> built;
    native::DrawResult last;

    // `mask` turns capability bits off (the paths of the M2 run on any device); `force_uber` is --force-ubershader.
    const FeatureRegistry* registry;

    Rig(std::shared_ptr<rhi::Backend> b, bool force_uber, const FeatureRegistry* features = &FeatureRegistry::Global())
        : backend(std::move(b)), registry(features) {
        profile = ProfileOf(backend->Caps());
        builder = std::make_unique<native::PipelineBuilder>(backend, *registry);
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
        auto vr = backend->CreateRing(1 << 20, rhi::BufferUsage::Vertex, "tev vertices");
        auto sr = backend->CreateRing(1 << 20, rhi::BufferUsage::Uniform, "tev vs uniforms");
        REQUIRE(vr.ok());
        REQUIRE(sr.ok());
        vertex_ring = std::move(*vr);
        vs_ring = std::move(*sr);
        color = MakeImage(rhi::Format::Rgba8Unorm, "tev colour");
        depth = MakeImage(rhi::Format::D24S8, "tev depth");
    }

    std::shared_ptr<rhi::Image> MakeImage(rhi::Format format, const char* name) {
        rhi::ImageDesc desc;
        desc.format = format;
        desc.width = desc.height = kSize;
        desc.name = name;
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        return *image;
    }

    // Clears the target to `clear` (bytes) and the depth to 1.0, runs the draws in one recorder, returns the colour pixels.
    std::vector<u8> Render(const std::vector<DrawSpec>& draws, std::array<int, 4> clear = {10, 20, 30, 40}) {
        rhi::Recorder rec = backend->BeginRecording();
        composer->BeginRecording(rec);
        uniforms->BeginRecording();
        bool first = true;
        for (const DrawSpec& spec : draws) {
            FragmentBlock block;
            std::memset(&block, 0, sizeof(block));
            block.uniforms = spec.uniforms;
            block.cfg = spec.key;
            auto alloc = uniforms->Write(block, true);
            REQUIRE_MESSAGE(alloc.ok(), alloc.error());

            cpuvertex::TriangleBatch batch;
            batch.AddTriangle(spec.tri.v[0], spec.tri.v[1], spec.tri.v[2]);
            auto streamed = cpuvertex::UploadBatch(*vertex_ring, batch);
            REQUIRE_MESSAGE(streamed.ok(), streamed.error());

            auto vs = vs_ring->Map(2048, std::max<u64>(256, backend->Caps().uniform_buffer_offset_alignment));
            REQUIRE_MESSAGE(vs.ok(), vs.error());
            std::memset(vs->data.data(), 0, 2048);  // PicaUniformData (unread by the trivial stage), VertexUniformData at +1792
            REQUIRE(vs_ring->Commit(2048).ok());

            rhi::TargetPassDesc pass;
            pass.color = color.get();
            pass.depth = spec.use_depth ? depth.get() : nullptr;
            pass.area = rhi::Rect{0, 0, kSize, kSize};
            if (first) {
                rhi::TargetPassDesc clear_pass = pass;
                clear_pass.color_load = rhi::LoadOp::Clear;
                clear_pass.clear_color = {B(clear[0]), B(clear[1]), B(clear[2]), B(clear[3])};
                clear_pass.depth_load = rhi::LoadOp::Clear;
                clear_pass.clear_depth = 1.0f;
                clear_pass.clear_stencil = 0;
                composer->BeginTargetPass(rec, clear_pass);
                fbops::ApplyDynamicState(rec, spec.dynamic, false);
                first = false;
            }
            native::DrawDesc d;
            d.vertex.kind = native::VertexStageKind::Trivial;
            d.vertex.input = InputKey();
            d.fragment = &spec.key;
            d.needs = NeedsOf(spec.key, profile, *registry);
            d.state = spec.state;
            d.color_format = rhi::Format::Rgba8Unorm;
            d.depth_format = spec.use_depth ? rhi::Format::D24S8 : rhi::Format::Undefined;
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
            last = *result;
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

// True when every pixel is within `tolerance` of `want` (bytes) in every channel.
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

// The two modes (and the forced ubershader) are the same shader in two forms: their pixels agree within one step of 8 bits per
// channel. On lavapipe they are identical; another device may contract a multiply and an add in one form and not in the other,
// and the byte rounding after each stage then differs by one at most.
bool SameWithin(const std::vector<u8>& a, const std::vector<u8>& b, int tolerance = 1) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > tolerance) return false;
    }
    return true;
}

void SetConstant(FragmentUniforms& uniforms, u32 index, std::array<int, 4> c) {
    uniforms.const_color[index] = Vec4f{B(c[0]), B(c[1]), B(c[2]), B(c[3])};
}

}  // namespace

TEST_CASE("tev/a_pass_through_key_gives_the_primary_colour_of_the_last_stage_that_is_not_one") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend, false);
    DrawSpec spec;
    spec.key.texture.tev_stages[0] = Stage{}.Raw();
    Stage s0;
    s0.color_src[0] = kPrimaryColor;
    s0.alpha_src[0] = kPrimaryColor;
    spec.key.texture.tev_stages[0] = s0.Raw();
    spec.tri = FullTriangle({100, 150, 200, 250});
    std::vector<u8> pixels = rig.Render({spec});
    std::string why;
    CHECK_MESSAGE(AllNear(pixels, {100, 150, 200, 250}, 0, &why), why);
    CHECK(rig.built.size() == 1);
    CHECK_FALSE(rig.last.fallback);
}

// ---- TEV cases --------------------------------------------------------------------------------------------------------------

namespace {

// The inputs of one draw of a TEV case.
struct ColorSet {
    std::array<int, 4> primary{};
    std::array<int, 4> constants[6] = {};
    std::array<int, 4> buffer{};
    int alpha_ref = 0;
};

struct TevCase {
    std::string label;
    std::array<Stage, 6> stages{};
    u32 buffer_input = 0;
    re::CompareFunc alpha_func = re::CompareFunc::Always;
    int tolerance = 0;
    // The case must not pass vacuously: over the colour sets, some colour channel of the expected pixel is neither 0 nor 255
    // (so a wrong factor or a missing term cannot hide in a clamp); `require_alpha_midrange` asks the same of the alpha channel.
    // Off for the cases that saturate on purpose (a scale of 2 or 4).
    bool require_midrange = false;
    bool require_alpha_midrange = false;
};

const ColorSet kColorSets[4] = {
    {{255, 255, 255, 255}, {{0, 0, 0, 0}, {128, 128, 128, 128}, {255, 0, 128, 64}, {1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}}, {0, 0, 0, 0}, 0},
    {{10, 200, 90, 60}, {{250, 30, 128, 200}, {77, 177, 33, 99}, {200, 100, 50, 25}, {40, 50, 60, 70}, {80, 90, 100, 110}, {120, 130, 140, 150}}, {30, 60, 90, 120}, 100},
    {{127, 128, 129, 130}, {{255, 0, 255, 0}, {1, 254, 2, 253}, {128, 127, 126, 125}, {3, 3, 3, 3}, {4, 4, 4, 4}, {5, 5, 5, 5}}, {250, 200, 150, 100}, 255},
    // operands near 0.8 and 0.7 (primary and the stage-0 constant): Dot3 gives about 40 to 220 in every channel, and so does
    // the alpha of Dot3_RGBA; the other operations give mid-range values too
    {{204, 190, 210, 230}, {{178, 170, 160, 200}, {90, 120, 150, 180}, {60, 70, 80, 90}, {100, 110, 120, 130}, {140, 150, 160, 170}, {30, 60, 90, 120}}, {70, 140, 210, 100}, 50},
};

FragmentKey KeyOfCase(const TevCase& c) {
    FragmentKey key = DefaultKey();
    for (u32 i = 0; i < 6; ++i) {
        TevStageConfigRaw raw = c.stages[i].Raw();
        if (raw.color_op() == re::TevOperation::Dot3_RGBA) {  // MakeKey's canonical form (alpha is the colour result)
            raw.sources_raw &= 0xFFF;
            raw.modifiers_raw &= 0xFFF;
            raw.ops_raw &= 0xF;
        }
        key.texture.tev_stages[i] = raw;
    }
    key.texture.set_combiner_buffer_input(c.buffer_input);
    key.framebuffer.set_alpha_test_func(c.alpha_func);
    return key;
}

DrawSpec SpecOf(const FragmentKey& key, const ColorSet& set) {
    DrawSpec spec;
    spec.key = key;
    spec.tri = FullTriangle(set.primary);
    for (u32 i = 0; i < 6; ++i) SetConstant(spec.uniforms, i, set.constants[i]);
    spec.uniforms.tev_combiner_buffer_color = Vec4f{B(set.buffer[0]), B(set.buffer[1]), B(set.buffer[2]), B(set.buffer[3])};
    spec.uniforms.alphatest_ref = set.alpha_ref;
    return spec;
}

std::array<int, 4> ExpectedPixel(const TevCase& c, const ColorSet& set, std::array<int, 4> clear) {
    sw::Inputs in;
    in.primary = {set.primary[0], set.primary[1], set.primary[2], set.primary[3]};
    for (u32 i = 0; i < 6; ++i) in.constant[i] = {set.constants[i][0], set.constants[i][1], set.constants[i][2], set.constants[i][3]};
    in.combiner_buffer_color = {set.buffer[0], set.buffer[1], set.buffer[2], set.buffer[3]};
    in.buffer_input = c.buffer_input;
    const sw::V4 out = sw::Tev(c.stages, in);
    if (!sw::AlphaTest(c.alpha_func, out[3], set.alpha_ref)) {
        return clear;
    }
    return {out[0], out[1], out[2], out[3]};
}

// Runs a case on the colour sets in both modes: the specialised result is within the case's tolerance of the software
// renderer's formulas, the ubershader's result is the specialised result.
void CheckTev(Rig& spec_rig, Rig& uber_rig, const TevCase& c) {
    const FragmentKey key = KeyOfCase(c);
    bool midrange = false, alpha_midrange = false;
    for (const ColorSet& set : kColorSets) {
        const std::array<int, 4> clear = {10, 20, 30, 40};
        const std::array<int, 4> want = ExpectedPixel(c, set, clear);
        for (int ch = 0; ch < 3; ++ch) midrange = midrange || (want[ch] > 0 && want[ch] < 255);
        alpha_midrange = alpha_midrange || (want[3] > 0 && want[3] < 255);
        const DrawSpec spec = SpecOf(key, set);
        const std::vector<u8> specialised = spec_rig.Render({spec}, clear);
        std::string why;
        CHECK_MESSAGE(AllNear(specialised, want, c.tolerance, &why), c.label << ": " << why);
        CHECK_FALSE(spec_rig.last.fallback);
        const std::vector<u8> uber = uber_rig.Render({spec}, clear);
        CHECK_MESSAGE(uber_rig.last.fallback, c.label);
        CHECK_MESSAGE(SameWithin(uber, specialised), c.label << ": the ubershader's pixels differ from the specialised shader's: " << Pixel(uber, 0, 0)[0]
                                                    << "," << Pixel(uber, 0, 0)[1] << "," << Pixel(uber, 0, 0)[2] << ","
                                                    << Pixel(uber, 0, 0)[3] << " against " << Pixel(specialised, 0, 0)[0] << ","
                                                    << Pixel(specialised, 0, 0)[1] << "," << Pixel(specialised, 0, 0)[2] << ","
                                                    << Pixel(specialised, 0, 0)[3]);
    }
    if (c.require_midrange) CHECK_MESSAGE(midrange, c.label << ": every expected colour channel is 0 or 255 in every set (a vacuous case)");
    if (c.require_alpha_midrange) CHECK_MESSAGE(alpha_midrange, c.label << ": every expected alpha is 0 or 255 in every set");
}

// Stage 0 gives the stage-0 constant (colour and alpha); stage 1 is `stage`; the rest pass through.
std::array<Stage, 6> TwoStages(const Stage& stage) {
    std::array<Stage, 6> stages{};
    Stage s0;
    s0.color_src[0] = kConstant;
    s0.alpha_src[0] = kConstant;
    stages[0] = s0;
    stages[1] = stage;
    return stages;
}

}  // namespace

TEST_CASE("tev/every_operation_of_both_combiners_with_each_scale") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    for (u32 op = 0; op <= 9; ++op) {
        for (u32 scale : {0u, 1u, 2u, 3u}) {
            TevCase c;
            c.label = "operation " + std::to_string(op) + " scale " + std::to_string(scale);
            Stage s;
            // sources: the primary colour, the previous stage (the stage-0 constant) and this stage's own constant
            s.color_src[0] = kPrimaryColor; s.color_src[1] = kPrevious; s.color_src[2] = kConstant;
            s.alpha_src[0] = kPrimaryColor; s.alpha_src[1] = kPrevious; s.alpha_src[2] = kConstant;
            s.color_op = op;
            s.alpha_op = op;
            s.color_scale = scale;
            s.alpha_scale = scale;
            c.stages = TwoStages(s);
            c.tolerance = sw::Tolerance(op) * sw::Multiplier(scale);
            // a scale of 1 (x2) or 2 (x4) saturates on purpose
            c.require_midrange = sw::Multiplier(scale) == 1;
            c.require_alpha_midrange = c.require_midrange && op != kDot3Rgb;  // Dot3_RGB has no alpha combiner: its alpha is 0
            CheckTev(spec_rig, uber_rig, c);
        }
    }
}

TEST_CASE("tev/every_colour_modifier_and_every_alpha_modifier_of_each_operand") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    const u32 color_mods[] = {kSourceColor, kOneMinusSourceColor, kSourceAlpha, kOneMinusSourceAlpha, kSourceRed,
                              kOneMinusSourceRed, kSourceGreen, kOneMinusSourceGreen, kSourceBlue, kOneMinusSourceBlue};
    for (u32 operand = 0; operand < 3; ++operand) {
        for (u32 m : color_mods) {
            TevCase c;
            c.label = "colour modifier " + std::to_string(m) + " on operand " + std::to_string(operand);
            Stage s;
            s.color_src[0] = kPrimaryColor; s.color_src[1] = kConstant; s.color_src[2] = kPrevious;
            s.color_mod[operand] = m;
            s.color_op = kLerp;  // uses all three operands
            c.stages = TwoStages(s);
            c.tolerance = 1;
            CheckTev(spec_rig, uber_rig, c);
        }
        for (u32 m = 0; m < 8; ++m) {
            TevCase c;
            c.label = "alpha modifier " + std::to_string(m) + " on operand " + std::to_string(operand);
            Stage s;
            s.alpha_src[0] = kPrimaryColor; s.alpha_src[1] = kConstant; s.alpha_src[2] = kPrevious;
            s.alpha_mod[operand] = m;
            s.alpha_op = kLerp;
            c.stages = TwoStages(s);
            c.tolerance = 1;
            CheckTev(spec_rig, uber_rig, c);
        }
    }
}

TEST_CASE("tev/every_source_of_a_stage_the_stubbed_ones_included") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    // PrimaryFragmentColor and SecondaryFragmentColor are zero (lighting is off or a stub); the four texture units give
    // opaque white (the stubs); PreviousBuffer is the combiner buffer after stage 0.
    for (u32 source : {kPrimaryColor, kPrimaryFragmentColor, kSecondaryFragmentColor, kTexture0, kTexture1, kTexture2, kTexture3,
                       kPreviousBuffer, kConstant, kPrevious}) {
        for (u32 slot = 0; slot < 3; ++slot) {
            TevCase c;
            c.label = "source " + std::to_string(source) + " in operand " + std::to_string(slot);
            Stage s;
            s.color_src[0] = kPrimaryColor; s.color_src[1] = kConstant; s.color_src[2] = kPrevious;
            s.alpha_src[0] = kPrimaryColor; s.alpha_src[1] = kConstant; s.alpha_src[2] = kPrevious;
            s.color_src[slot] = source;
            s.alpha_src[slot] = source;
            s.color_op = kLerp;
            s.alpha_op = kLerp;
            c.stages = TwoStages(s);
            c.tolerance = 1;
            CheckTev(spec_rig, uber_rig, c);
        }
    }
}

TEST_CASE("tev/stage_zero_reads_its_third_source_where_the_first_two_say_Previous") {
    // WriteTevStage's force_source3: Previous at stage 0 is the third source (the previous stage does not exist), for the colour
    // sources (AppendColorModifier) and for the alpha sources (AppendAlphaModifier: the GLSL forces them too, the software
    // renderer does not: see the header of this file).
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    {
        TevCase c;
        c.label = "force_source3, colour";
        std::array<Stage, 6> stages{};
        Stage s;
        s.color_src[0] = kPrevious; s.color_src[1] = kPrevious; s.color_src[2] = kConstant;
        s.color_op = kAdd;
        s.alpha_src[0] = kPrimaryColor; s.alpha_src[1] = kPrimaryColor; s.alpha_src[2] = kConstant;
        s.alpha_op = kModulate;
        stages[0] = s;
        c.stages = stages;
        CheckTev(spec_rig, uber_rig, c);
    }
    {
        // the alpha sources say Previous and the third is the constant; the colour third source is another source, so that a
        // generator that read the wrong third source would give another alpha
        TevCase c;
        c.label = "force_source3, alpha";
        std::array<Stage, 6> stages{};
        Stage s;
        s.color_src[0] = kPrimaryColor; s.color_src[1] = kPrimaryColor; s.color_src[2] = kPrimaryColor;
        s.color_op = kReplace;
        s.alpha_src[0] = kPrevious; s.alpha_src[1] = kPrevious; s.alpha_src[2] = kConstant;
        s.alpha_op = kLerp;
        stages[0] = s;
        c.stages = stages;
        c.tolerance = 1;
        CheckTev(spec_rig, uber_rig, c);
        c.label = "force_source3, alpha, one operand";
        stages[0].alpha_src[1] = kPrimaryColor;
        stages[0].alpha_op = kSubtract;
        c.stages = stages;
        CheckTev(spec_rig, uber_rig, c);
    }
}

TEST_CASE("tev/a_stage_is_skipped_as_a_pass_through_only_when_every_part_of_the_predicate_holds") {
    // IsPassThroughTevStage: Replace and Replace, the first colour and alpha sources Previous, SourceColor and SourceAlpha for the
    // first modifiers, and a scale of 1 for both (a scale field of 0 or 3). Stage 1 changes one thing at a time; the oracle has
    // no skip and gives the same where the predicate holds.
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    struct Variation {
        const char* label;
        std::function<void(Stage&)> change;
    };
    std::vector<Variation> variations = {
        {"nothing changed", [](Stage&) {}},
        {"colour scale 0 spelled 3", [](Stage& s) { s.color_scale = 3; }},
        {"alpha scale 0 spelled 3", [](Stage& s) { s.alpha_scale = 3; }},
    };
    for (u32 m : {1u, 2u, 3u, 4u, 5u, 8u, 9u, 12u, 13u}) {
        variations.push_back({"colour modifier 1", [m](Stage& s) { s.color_mod[0] = m; }});
    }
    for (u32 m = 1; m < 8; ++m) {
        variations.push_back({"alpha modifier 1", [m](Stage& s) { s.alpha_mod[0] = m; }});
    }
    for (u32 scale : {1u, 2u}) {
        variations.push_back({"colour scale", [scale](Stage& s) { s.color_scale = scale; }});
        variations.push_back({"alpha scale", [scale](Stage& s) { s.alpha_scale = scale; }});
    }
    for (u32 op : {1u, 2u, 3u, 5u, 8u, 9u}) {
        // sources 1 and 2 are the stage-0 output, source 3 the stage-1 constant: Replace is the only operation that gives the
        // previous output back
        variations.push_back({"colour operation", [op](Stage& s) { s.color_op = op; }});
        variations.push_back({"alpha operation", [op](Stage& s) { s.alpha_op = op; }});
    }
    for (const Variation& v : variations) {
        TevCase c;
        Stage s;  // Previous everywhere, Replace, SourceColor, scale 1
        s.color_src[2] = kConstant;
        s.alpha_src[2] = kConstant;
        v.change(s);
        c.stages = TwoStages(s);
        c.label = std::string("pass-through predicate: ") + v.label + " (colour mod " + std::to_string(s.color_mod[0]) + ", alpha mod " +
                  std::to_string(s.alpha_mod[0]) + ", scales " + std::to_string(s.color_scale) + "/" + std::to_string(s.alpha_scale) +
                  ", ops " + std::to_string(s.color_op) + "/" + std::to_string(s.alpha_op) + ")";
        c.tolerance = 1;
        CheckTev(spec_rig, uber_rig, c);
    }
}

TEST_CASE("tev/the_combiner_buffer_is_updated_by_the_stages_the_masks_name_and_read_one_stage_later") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    // stage 0: the stage-0 constant; stage 1: the stage-1 constant; stage 2: modulate PreviousBuffer with Previous; stage 3 the
    // same with the primary colour; stage 4: add PreviousBuffer and Previous; stage 5: PreviousBuffer. The masks decide which stage's output reaches the buffer.
    for (u32 rgb_mask = 0; rgb_mask < 16; rgb_mask += 3) {
        for (u32 alpha_mask = 0; alpha_mask < 16; alpha_mask += 5) {
            TevCase c;
            c.label = "buffer masks rgb " + std::to_string(rgb_mask) + " alpha " + std::to_string(alpha_mask);
            c.buffer_input = rgb_mask | (alpha_mask << 4);
            std::array<Stage, 6> stages{};
            for (u32 i = 0; i < 2; ++i) {
                stages[i].color_src[0] = kConstant;
                stages[i].alpha_src[0] = kConstant;
            }
            stages[2].color_src[0] = kPreviousBuffer; stages[2].color_src[1] = kPrevious; stages[2].color_op = kModulate;
            stages[2].alpha_src[0] = kPreviousBuffer; stages[2].alpha_src[1] = kPrevious; stages[2].alpha_op = kModulate;
            stages[3].color_src[0] = kPreviousBuffer; stages[3].color_src[1] = kPrimaryColor; stages[3].color_op = kModulate;
            stages[3].alpha_src[0] = kPreviousBuffer; stages[3].alpha_src[1] = kPrimaryColor; stages[3].alpha_op = kModulate;
            stages[4].color_src[0] = kPreviousBuffer; stages[4].color_src[1] = kPrevious; stages[4].color_op = kAdd;
            stages[4].alpha_src[0] = kPreviousBuffer; stages[4].alpha_src[1] = kPrevious; stages[4].alpha_op = kAdd;
            // stage 5 gives the buffer as it is then, so that the updates of stages 0 to 3 can be told apart (stage 3's is only
            // seen here)
            stages[5].color_src[0] = kPreviousBuffer; stages[5].color_op = kReplace;
            stages[5].alpha_src[0] = kPreviousBuffer; stages[5].alpha_op = kReplace;
            c.stages = stages;
            // Three stages that round: a Modulate (error at most 1 from truncation against the GLSL's rounding), a Modulate of a
            // value that may carry that error (at most 1 + 1: the second factor is at most 255/255) and an Add of two values
            // that carry up to 2 each (the Add itself is exact): 4 at most. Measured on lavapipe: 2.
            c.tolerance = 4;
            CheckTev(spec_rig, uber_rig, c);
        }
    }
}

TEST_CASE("alpha_test/every_function_against_the_software_renderers_comparison") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    const re::CompareFunc funcs[] = {re::CompareFunc::Never, re::CompareFunc::Always, re::CompareFunc::Equal,
                                     re::CompareFunc::NotEqual, re::CompareFunc::LessThan, re::CompareFunc::LessThanOrEqual,
                                     re::CompareFunc::GreaterThan, re::CompareFunc::GreaterThanOrEqual};
    const int alphas[] = {0, 1, 100, 127, 128, 200, 254, 255};
    const int refs[] = {0, 100, 128, 255};
    for (re::CompareFunc func : funcs) {
        TevCase c;
        c.label = "alpha test " + std::to_string(static_cast<int>(func));
        std::array<Stage, 6> stages{};
        stages[0].color_src[0] = kPrimaryColor;
        stages[0].alpha_src[0] = kPrimaryColor;
        c.stages = stages;
        c.alpha_func = func;
        const FragmentKey key = KeyOfCase(c);
        for (int alpha : alphas) {
            for (int ref : refs) {
                ColorSet set;
                set.primary = {200, 100, 50, alpha};
                set.alpha_ref = ref;
                const std::array<int, 4> clear = {10, 20, 30, 40};
                const std::array<int, 4> want = ExpectedPixel(c, set, clear);
                const DrawSpec spec = SpecOf(key, set);
                const std::vector<u8> a = spec_rig.Render({spec}, clear);
                const std::vector<u8> b = uber_rig.Render({spec}, clear);
                std::string why;
                CHECK_MESSAGE(AllNear(a, want, 0, &why), c.label << " alpha " << alpha << " ref " << ref << ": " << why);
                CHECK_MESSAGE(SameWithin(a, b), c.label << " alpha " << alpha << " ref " << ref << ": the ubershader differs");
            }
        }
    }
}

TEST_CASE("alpha_test/Never_discards_every_fragment_before_anything_else_runs") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    DrawSpec spec;
    spec.key.framebuffer.set_alpha_test_func(re::CompareFunc::Never);
    Stage s;
    s.color_src[0] = kPrimaryColor;
    s.alpha_src[0] = kPrimaryColor;
    spec.key.texture.tev_stages[0] = s.Raw();
    spec.tri = FullTriangle({1, 2, 3, 4});
    std::string why;
    CHECK_MESSAGE(AllNear(spec_rig.Render({spec}), {10, 20, 30, 40}, 0, &why), why);
    CHECK_MESSAGE(AllNear(uber_rig.Render({spec}), {10, 20, 30, 40}, 0, &why), why);
}

TEST_CASE("stubs/the_gas_stub_writes_azahars_transparent_black_and_returns") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    DrawSpec spec;
    spec.key.texture.set_fog_mode(re::FogMode::Gas);
    Stage s;
    s.color_src[0] = kPrimaryColor;
    s.alpha_src[0] = kPrimaryColor;
    spec.key.texture.tev_stages[0] = s.Raw();
    spec.tri = FullTriangle({100, 100, 100, 255});
    std::string why;
    CHECK_MESSAGE(AllNear(spec_rig.Render({spec}), {0, 0, 0, 0}, 0, &why), why);
    CHECK_MESSAGE(AllNear(uber_rig.Render({spec}), {0, 0, 0, 0}, 0, &why), why);
}

TEST_CASE("stubs/a_draw_that_needs_a_stubbed_feature_is_reported_by_name") {
    FragmentKey key = DefaultKey();
    std::array<const char*, 8> names{};
    auto stubbed = [&](const FragmentKey& k) {
        std::vector<std::string> out;
        const size_t n = StubbedFeatures(k, FeatureRegistry::Global(), names);
        for (size_t i = 0; i < n; ++i) out.push_back(names[i]);
        return out;
    };
    // a pass-through key needs nothing
    CHECK(stubbed(key).empty());
    // a stage that reads texture unit 1
    Stage s;
    s.color_src[0] = kTexture1;
    key.texture.tev_stages[2] = s.Raw();
    CHECK(stubbed(key) == std::vector<std::string>{"sampling"});
    // the same source in a pass-through stage is not read
    key.texture.tev_stages[2] = Stage{}.Raw();
    CHECK(stubbed(key).empty());
    // lighting, fog (and gas), shadow rendering, the procedural texture
    FragmentKey lit = DefaultKey();
    lit.lighting.set_enable(1);
    CHECK(stubbed(lit) == std::vector<std::string>{"lighting"});
    FragmentKey fog = DefaultKey();
    fog.texture.set_fog_mode(re::FogMode::Fog);
    CHECK(stubbed(fog) == std::vector<std::string>{"fog"});
    fog.texture.set_fog_mode(re::FogMode::Gas);
    CHECK(stubbed(fog) == std::vector<std::string>{"gas"});
    FragmentKey shadow = DefaultKey();
    shadow.framebuffer.set_shadow_rendering(1);
    CHECK(stubbed(shadow) == std::vector<std::string>{"shadow"});
    FragmentKey proc = DefaultKey();
    proc.proctex.set_enable(1);
    Stage p;
    p.color_src[0] = kTexture3;
    proc.texture.tev_stages[0] = p.Raw();
    CHECK(stubbed(proc) == std::vector<std::string>{"proctex"});  // unit 3 is the procedural slot, not a texture unit
}

// A TEV key with two active stages, and the generator's output for it in both modes and both passes: non-empty GLSL that glslang
// compiles. With MH4N_TEV_DUMP_GLSL=<directory> set, the case also writes the shaders there (for reading them next to Azahar's
// glsl_fs_shader_gen.cpp) and prints the compile times.
TEST_CASE("generator/the_shaders_of_a_tev_key_are_generated_and_compile_and_can_be_dumped_for_reading") {
    TevCase c;
    Stage s0;
    s0.color_src[0] = kConstant; s0.alpha_src[0] = kConstant;
    Stage s1;
    s1.color_src[0] = kPrimaryColor; s1.color_src[1] = kPrevious; s1.color_src[2] = kConstant; s1.color_op = kLerp;
    s1.alpha_src[0] = kTexture0; s1.alpha_src[1] = kPrevious; s1.alpha_src[2] = kConstant; s1.alpha_op = kMultiplyThenAdd;
    s1.color_scale = 2;
    c.stages[0] = s0;
    c.stages[1] = s1;
    c.buffer_input = 0x13;
    c.alpha_func = re::CompareFunc::GreaterThan;
    const std::string specialised_glsl = GenerateSpecialisedGlsl(KeyOfCase(c), kLeanProfile, ShaderPass::Normal);
    const std::string uber_glsl = GenerateUberGlsl(ShaderPass::Normal, kLeanProfile);
    const std::string shadow_glsl = GenerateUberGlsl(ShaderPass::ShadowWrite, kFullProfile);
    CHECK(specialised_glsl.find("void main()") != std::string::npos);
    CHECK(specialised_glsl.find("mix(color_results_2, color_results_1, color_results_3)") != std::string::npos);  // the Lerp stage's text
    CHECK(specialised_glsl.find("sampleTexUnit0().a") != std::string::npos);
    CHECK(uber_glsl.find("tev_color_combiner(") != std::string::npos);
    CHECK(uber_glsl.find("cfg[") != std::string::npos);
    CHECK(shadow_glsl.find("shadow_buffer") != std::string::npos);
    for (const std::string* glsl : {&specialised_glsl, &uber_glsl, &shadow_glsl}) {
        std::vector<u32> spirv;
        std::string error;
        CHECK_MESSAGE(CompileFragmentGlslToSpirv(*glsl, spirv, error), error);
        CHECK_FALSE(spirv.empty());
    }
    const char* dir = std::getenv("MH4N_TEV_DUMP_GLSL");
    if (dir == nullptr) return;
    const std::string base = std::string(dir) + "/";
    auto write = [&](const std::string& name, const std::string& text) {
        FILE* f = std::fopen((base + name).c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    };
    write("specialised.frag", specialised_glsl);
    write("uber_color.frag", uber_glsl);
    write("uber_shadow_write.frag", shadow_glsl);
    // and what glslang makes of them (words of SPIR-V, milliseconds on this machine, a build with optimisation)
    GenerateSpecialised(DefaultKey(), kLeanProfile, ShaderPass::Normal);  // glslang's one-time set-up is not a compile
    const Generated spec = GenerateSpecialised(KeyOfCase(c), kLeanProfile, ShaderPass::Normal);
    const Generated uber = GenerateUber(ShaderPass::Normal, kLeanProfile);
    const Generated shadow = GenerateUber(ShaderPass::ShadowWrite, kFullProfile);
    std::printf("specialised: %zu words, %.1f ms; uber_color: %zu words, %.1f ms; uber_shadow_write: %zu words, %.1f ms\n",
                spec.spirv.size(), spec.compile_ms, uber.spirv.size(), uber.compile_ms, shadow.spirv.size(), shadow.compile_ms);
}

// ---- cases driven by registers: the output stage, the scissor, the depth ------------------------------------------------------

namespace {

namespace pica = mh4n::gpu::pica;

constexpr u32 kRegDepthRange = 0x4D, kRegDepthNear = 0x4E, kRegDepthmap = 0x6D;
constexpr u32 kRegScissorMode = 0x65, kRegScissorXY1 = 0x66, kRegScissorXY2 = 0x67;
constexpr u32 kRegOutputMerger = 0x100, kRegBlendEquations = 0x101, kRegLogicOp = 0x102, kRegBlendConst = 0x103;
constexpr u32 kRegDepthColorMask = 0x107, kRegAllowColor = 0x113, kRegAllowDepth = 0x115;
constexpr u32 kRegTev0 = 0xC0;

// Float24 (1 sign, 7 exponent, 16 mantissa bits) of a normal float.
u32 F24(float f) {
    if (f == 0.0f) return 0;
    u32 bits;
    std::memcpy(&bits, &f, 4);
    const u32 sign = bits >> 31;
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 63;
    return (sign << 23) | (static_cast<u32>(exponent) << 16) | ((bits >> 7) & 0xFFFF);
}

// A PICA state with stage 0 = the primary colour, the stages after it pass through, colour writes on, no depth, no blending.
std::unique_ptr<pica::PicaState> BaseState() {
    auto state = std::make_unique<pica::PicaState>();
    state->regs.fill(0);
    state->dirty.SetAll();
    Stage s0;
    s0.color_src[0] = kPrimaryColor;
    s0.alpha_src[0] = kPrimaryColor;
    const TevStageConfigRaw raw = s0.Raw();
    state->regs[kRegTev0 + 0] = raw.sources_raw;
    state->regs[kRegTev0 + 1] = raw.modifiers_raw;
    state->regs[kRegTev0 + 2] = raw.ops_raw;
    // the stages 1 to 5 hold the pass-through
    for (u32 base : {0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u}) {
        const TevStageConfigRaw pass = Stage{}.Raw();
        state->regs[base + 0] = pass.sources_raw;
        state->regs[base + 1] = pass.modifiers_raw;
        state->regs[base + 2] = pass.ops_raw;
    }
    state->regs[kRegLogicOp] = static_cast<u32>(re::LogicOp::Copy);
    state->regs[kRegDepthColorMask] = 0xF00;  // red, green, blue, alpha writes
    state->regs[kRegAllowColor] = 0xF;
    state->regs[kRegAllowDepth] = 0;
    state->regs[0x1C6] = 1;  // lighting disabled
    state->regs[kRegDepthRange] = F24(-1.0f);
    state->regs[kRegDepthNear] = 0;
    state->regs[kRegDepthmap] = 1;  // Z-buffering
    return state;
}

// The draw the registers describe (the key, the fixed-function state, the uniforms as the real code makes them).
DrawSpec SpecOfState(const pica::PicaState& state, Tri tri) {
    DrawSpec spec;
    spec.key = MakeKey(state);
    spec.state = fbops::MakeFixedFunctionState(state);
    spec.dynamic = fbops::MakeDynamicState(state);
    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    UniformDirty dirty;
    dirty.all = true;
    FillUniforms(state, block, dirty, {});
    spec.uniforms = block.uniforms;
    spec.tri = tri;
    return spec;
}

u8 LogicOpOf(u32 op, u8 src, u8 dest) {  // sw_framebuffer.cpp's LogicOp
    switch (static_cast<re::LogicOp>(op)) {
    case re::LogicOp::Clear: return 0;
    case re::LogicOp::And: return src & dest;
    case re::LogicOp::AndReverse: return src & ~dest;
    case re::LogicOp::Copy: return src;
    case re::LogicOp::Set: return 255;
    case re::LogicOp::CopyInverted: return ~src;
    case re::LogicOp::NoOp: return dest;
    case re::LogicOp::Invert: return ~dest;
    case re::LogicOp::Nand: return ~(src & dest);
    case re::LogicOp::Or: return src | dest;
    case re::LogicOp::Nor: return ~(src | dest);
    case re::LogicOp::Xor: return src ^ dest;
    case re::LogicOp::Equiv: return ~(src ^ dest);
    case re::LogicOp::AndInverted: return ~src & dest;
    case re::LogicOp::OrReverse: return src | ~dest;
    case re::LogicOp::OrInverted: return ~src | dest;
    }
    return 0;
}

// sw_rasterizer.cpp's PixelColor blend factors, on u8 values; `channel` 3 is alpha.
int BlendFactorOf(u32 factor, int channel, const std::array<int, 4>& src, const std::array<int, 4>& dst,
                  const std::array<int, 4>& constant) {
    switch (static_cast<re::BlendFactor>(factor)) {
    case re::BlendFactor::Zero: return 0;
    case re::BlendFactor::One: return 255;
    case re::BlendFactor::SourceColor: return src[channel];
    case re::BlendFactor::OneMinusSourceColor: return 255 - src[channel];
    case re::BlendFactor::DestColor: return dst[channel];
    case re::BlendFactor::OneMinusDestColor: return 255 - dst[channel];
    case re::BlendFactor::SourceAlpha: return src[3];
    case re::BlendFactor::OneMinusSourceAlpha: return 255 - src[3];
    case re::BlendFactor::DestAlpha: return dst[3];
    case re::BlendFactor::OneMinusDestAlpha: return 255 - dst[3];
    case re::BlendFactor::ConstantColor: return constant[channel];
    case re::BlendFactor::OneMinusConstantColor: return 255 - constant[channel];
    case re::BlendFactor::ConstantAlpha: return constant[3];
    case re::BlendFactor::OneMinusConstantAlpha: return 255 - constant[3];
    case re::BlendFactor::SourceAlphaSaturate: return channel == 3 ? 255 : std::min(src[3], 255 - dst[3]);
    }
    return 255;
}

// sw_framebuffer.cpp's EvaluateBlendEquation for one channel.
int BlendOf(u32 equation, int src, int src_factor, int dst, int dst_factor) {
    const int a = src * src_factor, b = dst * dst_factor;
    int result = 0;
    switch (static_cast<re::BlendEquation>(equation)) {
    case re::BlendEquation::Add: result = (a + b) / 255; break;
    case re::BlendEquation::Subtract: result = (a - b) / 255; break;
    case re::BlendEquation::ReverseSubtract: result = (b - a) / 255; break;
    case re::BlendEquation::Min: result = std::min(a, b) / 255; break;
    case re::BlendEquation::Max: result = std::max(a, b) / 255; break;
    }
    return std::clamp(result, 0, 255);
}

}  // namespace

TEST_CASE("output/every_logic_op_is_the_hardware_op_or_the_shaders_and_both_are_the_software_renderers") {
    // The device's own logic op, and the path of the M2 (the bit masked off): the shader applies the op to the 8-bit values
    // read from the feedback copy of the target.
    rhi::BackendOptions masked_options;
    masked_options.mask = rhi::CapabilityMask{}.With(rhi::Cap::LogicOp);
    auto full = test::RequireBackend();
    if (!full) return;
    auto lean = test::RequireBackend(masked_options);
    if (!lean) return;
    CHECK(full->Caps().logic_op);
    CHECK_FALSE(lean->Caps().logic_op);
    Rig rigs[4] = {Rig(full, false), Rig(full, true), Rig(lean, false), Rig(lean, true)};
    const std::array<int, 4> source = {0x69, 0xC3, 0x55, 0x1E};
    const std::array<int, 4> dest = {0xA5, 0x3C, 0x0F, 0xF0};
    for (u32 op = 0; op < 16; ++op) {
        auto state = BaseState();
        state->regs[kRegLogicOp] = op;
        const DrawSpec spec = SpecOfState(*state, FullTriangle(source));
        std::array<int, 4> want{};
        for (int c = 0; c < 4; ++c) want[c] = LogicOpOf(op, static_cast<u8>(source[c]), static_cast<u8>(dest[c]));
        std::vector<u8> first;
        for (int r = 0; r < 4; ++r) {
            const std::vector<u8> pixels = rigs[r].Render({spec}, dest);
            std::string why;
            CHECK_MESSAGE(AllNear(pixels, want, 0, &why), "logic op " << op << " rig " << r << ": " << why);
            if (r == 0) first = pixels;
            CHECK_MESSAGE(pixels == first, "logic op " << op << " rig " << r);
        }
        // the emulated path asks for the destination only for the ops that read it
        const bool reads = op != 0 && op != 3 && op != 4 && op != 5 && op != 6;
        CHECK_MESSAGE(rigs[2].last.dst_copied == reads, "logic op " << op);
        CHECK_FALSE(rigs[0].last.dst_copied);
    }
}

TEST_CASE("output/blending_with_min_and_max_factors_the_hardware_cannot_apply_is_done_in_the_shader") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    struct Blend {
        u32 eq_rgb, eq_a, src_rgb, dst_rgb, src_a, dst_a;
        bool shader_rgb, shader_alpha;
    };
    using B = re::BlendFactor;
    using E = re::BlendEquation;
    const Blend blends[] = {
        // Min of rgb with factors, Max of alpha with both factors One (the hardware's MAX)
        {u32(E::Min), u32(E::Max), u32(B::SourceAlpha), u32(B::DestAlpha), u32(B::One), u32(B::One), true, false},
        {u32(E::Max), u32(E::Min), u32(B::DestColor), u32(B::OneMinusSourceColor), u32(B::SourceAlpha), u32(B::OneMinusDestAlpha), true, true},
        {u32(E::Min), u32(E::Add), u32(B::One), u32(B::One), u32(B::SourceAlpha), u32(B::OneMinusSourceAlpha), false, false},
        {u32(E::Add), u32(E::Add), u32(B::SourceAlpha), u32(B::OneMinusSourceAlpha), u32(B::One), u32(B::Zero), false, false},
        {u32(E::Subtract), u32(E::ReverseSubtract), u32(B::One), u32(B::SourceColor), u32(B::One), u32(B::One), false, false},
        {u32(E::Max), u32(E::Max), u32(B::SourceAlphaSaturate), u32(B::ConstantColor), u32(B::SourceAlphaSaturate), u32(B::OneMinusConstantColor), true, true},
    };
    const std::array<int, 4> source = {200, 90, 30, 160};
    const std::array<int, 4> dest = {40, 180, 230, 100};
    const std::array<int, 4> constant = {64, 128, 192, 32};
    for (const Blend& b : blends) {
        auto state = BaseState();
        state->regs[kRegOutputMerger] = 1u << 8;  // alphablend_enable
        state->regs[kRegBlendEquations] = b.eq_rgb | (b.eq_a << 8) | (b.src_rgb << 16) | (b.dst_rgb << 20) | (b.src_a << 24) | (b.dst_a << 28);
        state->regs[kRegBlendConst] = constant[0] | (constant[1] << 8) | (constant[2] << 16) | (constant[3] << 24);
        const DrawSpec spec = SpecOfState(*state, FullTriangle(source));
        std::array<int, 4> want{};
        for (int c = 0; c < 3; ++c) {
            want[c] = BlendOf(b.eq_rgb, source[c], BlendFactorOf(b.src_rgb, c, source, dest, constant), dest[c],
                              BlendFactorOf(b.dst_rgb, c, source, dest, constant));
        }
        want[3] = BlendOf(b.eq_a, source[3], BlendFactorOf(b.src_a, 3, source, dest, constant), dest[3],
                          BlendFactorOf(b.dst_a, 3, source, dest, constant));
        std::string label = "eq " + std::to_string(b.eq_rgb) + "/" + std::to_string(b.eq_a) + " factors " + std::to_string(b.src_rgb) + "," +
                            std::to_string(b.dst_rgb) + "/" + std::to_string(b.src_a) + "," + std::to_string(b.dst_a);
        const std::vector<u8> a = spec_rig.Render({spec}, dest);
        // one step: the hardware rounds where the software renderer truncates
        std::string why;
        CHECK_MESSAGE(AllNear(a, want, 1, &why), label << ": " << why);
        CHECK_MESSAGE(spec_rig.last.dst_copied == (b.shader_rgb || b.shader_alpha), label);
        const std::vector<u8> u = uber_rig.Render({spec}, dest);
        CHECK_MESSAGE(SameWithin(a, u), label << ": the ubershader differs");
    }
}

TEST_CASE("scissor/include_and_exclude_modes_keep_the_rectangle_or_everything_outside_it") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    for (u32 mode : {0u, 1u, 3u}) {
        auto state = BaseState();
        state->regs[kRegScissorMode] = mode;
        state->regs[kRegScissorXY1] = 2 | (1 << 16);  // x1 = 2, y1 = 1
        state->regs[kRegScissorXY2] = 5 | (4 << 16);  // x2 = 5, y2 = 4 (the rectangle includes them)
        const DrawSpec spec = SpecOfState(*state, FullTriangle({200, 100, 50, 255}));
        for (Rig* rig : {&spec_rig, &uber_rig}) {
            const std::vector<u8> pixels = rig->Render({spec});
            for (u32 y = 0; y < kSize; ++y) {
                for (u32 x = 0; x < kSize; ++x) {
                    const bool inside = x >= 2 && x <= 5 && y >= 1 && y <= 4;
                    // Disabled keeps every pixel; Exclude (1) drops the pixels inside; Include (3) drops those outside
                    const bool kept = mode == 0 ? true : (mode == 1 ? !inside : inside);
                    const std::array<int, 4> want = kept ? std::array<int, 4>{200, 100, 50, 255} : std::array<int, 4>{10, 20, 30, 40};
                    CHECK_MESSAGE(Pixel(pixels, x, y) == want, "mode " << mode << " pixel " << x << "," << y);
                }
            }
        }
    }
}

TEST_CASE("depth/the_shader_writes_the_depth_the_registers_scale_and_w_buffering_divides_by_w") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    // depth = z * scale + offset with z in [-1, 0] and scale -1: the triangle at z = -0.5 is at depth 0.5, the one at -0.25 at 0.25.
    // The depth test is LessThan against a buffer cleared to 1.0, and the first draw writes depth.
    auto make = [&](bool w_buffering, u32 compare) {
        auto state = BaseState();
        state->regs[kRegDepthmap] = w_buffering ? 0 : 1;
        state->regs[kRegAllowDepth] = 3;
        state->regs[kRegDepthColorMask] = 0xF00 | 1u | (compare << 4) | (1u << 12);  // depth test on, write on
        state->regs[0x116] = 3;                                                       // D24S8
        return state;
    };
    const std::array<int, 4> red = {255, 0, 0, 255}, blue = {0, 0, 255, 255};
    const std::array<int, 4> clear = {10, 20, 30, 40};
    for (Rig* rig : {&spec_rig, &uber_rig}) {
        // Z-buffering, LessThan (4): near (0.25) after far (0.5) wins; far after near is rejected
        auto z = make(false, 4);
        DrawSpec far_draw = SpecOfState(*z, FullTriangle(red, -0.5f));
        DrawSpec near_draw = SpecOfState(*z, FullTriangle(blue, -0.25f));
        far_draw.use_depth = near_draw.use_depth = true;
        std::string why;
        CHECK_MESSAGE(AllNear(rig->Render({far_draw, near_draw}, clear), blue, 0, &why), why);
        CHECK_MESSAGE(AllNear(rig->Render({near_draw, far_draw}, clear), blue, 0, &why), why);
        // Always (1): the last draw wins
        auto always = make(false, 1);
        far_draw = SpecOfState(*always, FullTriangle(red, -0.5f));
        near_draw = SpecOfState(*always, FullTriangle(blue, -0.25f));
        far_draw.use_depth = near_draw.use_depth = true;
        CHECK_MESSAGE(AllNear(rig->Render({near_draw, far_draw}, clear), red, 0, &why), why);
        // Equal (2): depth 0.5 against a cleared 1.0 never passes
        auto equal = make(false, 2);
        DrawSpec e = SpecOfState(*equal, FullTriangle(red, -0.5f));
        e.use_depth = true;
        CHECK_MESSAGE(AllNear(rig->Render({e}, clear), clear, 0, &why), why);
        // W-buffering: the same z at w = 1 and at w = 2: depth is divided by gl_FragCoord.w, which is 1 / w, so the w = 2 triangle is
        // at depth 0.4 (z = -0.2) and the other at 0.2; the nearer one wins whatever the order. In Z-buffering the two are equal and
        // LessThan rejects the second.
        auto wbuf = make(true, 4);
        DrawSpec w1 = SpecOfState(*wbuf, FullTriangle(red, -0.2f, 1.0f));
        DrawSpec w2 = SpecOfState(*wbuf, FullTriangle(blue, -0.2f, 2.0f));
        w1.use_depth = w2.use_depth = true;
        CHECK_MESSAGE(AllNear(rig->Render({w2, w1}, clear), red, 0, &why), why);
        CHECK_MESSAGE(AllNear(rig->Render({w1, w2}, clear), red, 0, &why), why);
        DrawSpec z1 = SpecOfState(*z, FullTriangle(red, -0.2f, 1.0f));
        DrawSpec z2 = SpecOfState(*z, FullTriangle(blue, -0.2f, 2.0f));
        z1.use_depth = z2.use_depth = true;
        CHECK_MESSAGE(AllNear(rig->Render({z2, z1}, clear), blue, 0, &why), why);
        CHECK_MESSAGE(AllNear(rig->Render({z1, z2}, clear), red, 0, &why), why);
    }
}

// ---- PICA shader programs, for the cases that go through N2-06 and the CPU path -----------------------------------------------
//
// The instruction words are placed where Azahar's nihstro puts them (externals/nihstro/include/nihstro/shader_bytecode.h,
// BSD-3-Clause; the same field positions as the encoder of gpu_cpuvertex_test.cpp): arithmetic and MOV: opcode 26-31, dest
// 21-25, src1 12-18, src2 7-11, operand descriptor 0-6; SETEMIT: vertex id 24-25, prim_emit 23, winding 22.

namespace {

namespace pi {

constexpr u32 ADD = 0x00, MOV = 0x13, END = 0x22, EMIT = 0x2A, SETEMIT = 0x2B;

constexpr u32 V(u32 n) { return n; }
constexpr u32 C(u32 n) { return 0x20 + n; }
constexpr u32 O(u32 n) { return n; }

constexpr u32 Ins(u32 op, u32 dest, u32 s1, u32 s2) { return (op << 26) | (dest << 21) | (s1 << 12) | (s2 << 7); }  // descriptor 0
constexpr u32 SetEmitOp(u32 vertex_id, u32 prim_emit, u32 winding) {
    return (SETEMIT << 26) | (vertex_id << 24) | (prim_emit << 23) | (winding << 22);
}
constexpr u32 kEnd = END << 26;
constexpr u32 kEmit = EMIT << 26;

// Operand descriptor 0: all four lanes written, the three sources unswizzled and not negated.
constexpr u32 IdentityDescriptor() {
    u32 w = 0xF;  // destination mask
    for (u32 c = 0; c < 4; ++c) {
        w |= c << (11 - 2 * c);  // source 1 lane selectors
        w |= c << (20 - 2 * c);  // source 2
        w |= c << (29 - 2 * c);  // source 3
    }
    return w;
}

}  // namespace pi

// The value 0..255 as the float that gives that byte back.
std::array<float, 4> Bytes4(int r, int g, int b, int a) {
    return {B(r), B(g), B(b), B(a)};
}

}  // namespace

TEST_CASE("varyings/a_translated_vertex_stage_feeds_each_of_the_seven_varyings_of_the_fragment_stage") {
    auto backend = test::RequireBackend();
    if (!backend) return;

    // The vertex program: o0 = v0 (the position, from the vertex buffer) and o1 .. o5 = c1 .. c5 (constants).
    pica::PicaState state;
    state.regs.fill(0);
    state.vs.program_code.fill(0);
    state.vs.swizzle_data.fill(0);
    const u32 program[] = {pi::Ins(pi::MOV, pi::O(0), pi::V(0), 0), pi::Ins(pi::MOV, pi::O(1), pi::C(1), 0),
                           pi::Ins(pi::MOV, pi::O(2), pi::C(2), 0), pi::Ins(pi::MOV, pi::O(3), pi::C(3), 0),
                           pi::Ins(pi::MOV, pi::O(4), pi::C(4), 0), pi::Ins(pi::MOV, pi::O(5), pi::C(5), 0), pi::kEnd};
    std::copy(std::begin(program), std::end(program), state.vs.program_code.begin());
    state.vs.swizzle_data[0] = pi::IdentityDescriptor();
    state.vs.biggest_program_size = static_cast<u32>(std::size(program));
    state.vs.biggest_swizzle_size = 1;
    state.regs[pica::reg::kVs + pica::reg::kShaderMainOffset] = 0;
    state.regs[pica::reg::kVs + pica::reg::kShaderOutputMask] = 0x3F;  // o0 .. o5
    state.regs[shader::reg::kVsOutputTotal] = 6;
    // The output map: o0 the position, o1 the colour, o2 texcoord0 and texcoord1, o3 texcoord2 and texcoord0_w, o4 the
    // quaternion, o5 the view vector. The 31 (INVALID) maps nothing.
    auto map = [](u32 x, u32 y, u32 z, u32 w) { return x | (y << 8) | (z << 16) | (w << 24); };
    using namespace shader::semantic;
    auto set_map = [&](bool tc1_unassigned) {
        state.regs[shader::reg::kVsOutputAttributes + 0] = map(kPositionX, kPositionY, kPositionZ, kPositionW);
        state.regs[shader::reg::kVsOutputAttributes + 1] = map(kColorR, kColorG, kColorB, kColorA);
        state.regs[shader::reg::kVsOutputAttributes + 2] =
            map(kTexcoord0U, kTexcoord0V, tc1_unassigned ? kInvalid : kTexcoord1U, tc1_unassigned ? kInvalid : kTexcoord1V);
        state.regs[shader::reg::kVsOutputAttributes + 3] = map(kTexcoord2U, kTexcoord2V, kTexcoord0W, kInvalid);
        state.regs[shader::reg::kVsOutputAttributes + 4] = map(kQuaternionX, kQuaternionY, kQuaternionZ, kQuaternionW);
        state.regs[shader::reg::kVsOutputAttributes + 5] = map(kViewX, kViewY, kViewZ, kInvalid);
    };
    auto uniform = [&](u32 i, std::array<float, 4> v) { state.vs.uniform_f[i].c[0] = v[0], state.vs.uniform_f[i].c[1] = v[1],
                                                         state.vs.uniform_f[i].c[2] = v[2], state.vs.uniform_f[i].c[3] = v[3]; };
    uniform(1, Bytes4(51, 102, 153, 204));  // colour
    uniform(2, Bytes4(26, 77, 128, 179));   // texcoord0 (u, v), texcoord1 (u, v)
    uniform(3, Bytes4(230, 38, 90, 7));     // texcoord2 (u, v), texcoord0_w; the w lane maps nothing
    uniform(4, Bytes4(64, 115, 166, 217));  // quaternion
    uniform(5, Bytes4(13, 140, 191, 3));    // view; the w lane maps nothing

    // The test fragment stage writes the varying `which` (a member of the first word of the fragment block) to the colour.
    const char* kFragment = R"(#version 450
layout (location = 1) in vec4 primary_color;
layout (location = 2) in vec2 texcoord0;
layout (location = 3) in vec2 texcoord1;
layout (location = 4) in vec2 texcoord2;
layout (location = 5) in float texcoord0_w;
layout (location = 6) in vec4 normquat;
layout (location = 7) in vec3 view;
layout (location = 0) out vec4 color;
layout (set = 0, binding = 2, std140) uniform Select { int which; };
void main() {
    switch (which) {
    case 0: color = primary_color; break;
    case 1: color = vec4(texcoord0, 0.0, 1.0); break;
    case 2: color = vec4(texcoord1, 0.0, 1.0); break;
    case 3: color = vec4(texcoord2, 0.0, 1.0); break;
    case 4: color = vec4(texcoord0_w, 0.0, 0.0, 1.0); break;
    case 5: color = normquat; break;
    default: color = vec4(view, 1.0); break;
    }
}
)";
    std::vector<u32> fragment_spirv;
    std::string error;
    REQUIRE_MESSAGE(CompileFragmentGlslToSpirv(kFragment, fragment_spirv, error), error);

    shader::VertexShaderCache cache;
    auto draw_all = [&](bool tc1_unassigned) {
        set_map(tc1_unassigned);
        shader::ConfigOptions options;
        const shader::VertexShaderConfig config = shader::MakeVertexShaderConfig(state, nullptr, options);
        shader::TranslateResult translated = cache.Get(state.vs, config);
        REQUIRE_MESSAGE(translated.ok(), translated.error);

        // The vertex layout: input register 0 from one binding of float4 positions (the translated stage declares all 16
        // locations it reads, the others are not read).
        native::VertexInputKey input;
        input.binding_count = 1;
        input.bindings[0].stride = 16;
        input.attribute_count = 1;
        input.attributes[0] = {0, 0, rhi::Format::Rgba32Float, 0};

        native::DefaultPipelineProvider provider([&](const native::PipelineKey& key) {
            rhi::GraphicsPipelineDesc desc = native::BaseDescOf(key, *backend);
            desc.vertex.spirv = translated.shader->spirv;
            desc.fragment.spirv = fragment_spirv;
            return backend->CreateGraphicsPipeline(desc);
        });
        auto composer = native::DrawComposer::Create(backend, provider);
        REQUIRE_MESSAGE(composer.ok(), composer.error());
        auto uniform_ring = FragmentUniformRing::Create(*backend);
        auto tables = TableRings::Create(*backend);
        auto vertex_ring = backend->CreateRing(1 << 16, rhi::BufferUsage::Vertex, "varyings vertices");
        auto vs_ring = backend->CreateRing(1 << 16, rhi::BufferUsage::Uniform, "varyings vs uniforms");
        REQUIRE(uniform_ring.ok());
        REQUIRE(tables.ok());
        REQUIRE(vertex_ring.ok());
        REQUIRE(vs_ring.ok());

        rhi::ImageDesc image_desc;
        image_desc.format = rhi::Format::Rgba8Unorm;
        image_desc.width = image_desc.height = kSize;
        image_desc.name = "varyings";
        auto image = backend->CreateImage(image_desc);
        REQUIRE(image.ok());

        std::vector<std::array<int, 4>> out;
        for (int which = 0; which < 7; ++which) {
            rhi::Recorder rec = backend->BeginRecording();
            (*composer)->BeginRecording(rec);
            (*uniform_ring)->BeginRecording();

            FragmentBlock block;
            std::memset(&block, 0, sizeof(block));
            block.uniforms.framebuffer_scale = which;  // the first word of the block: the test fragment shader's selector
            block.cfg = DefaultKey();
            auto alloc = (*uniform_ring)->Write(block, true);
            REQUIRE(alloc.ok());

            // three positions: clip-space coordinates of a triangle that covers the target
            const float positions[3][4] = {{-1, -1, -0.5f, 1}, {3, -1, -0.5f, 1}, {-1, 3, -0.5f, 1}};
            auto v = (*vertex_ring)->Map(sizeof(positions), 16);
            REQUIRE(v.ok());
            std::memcpy(v->data.data(), positions, sizeof(positions));
            REQUIRE((*vertex_ring)->Commit(sizeof(positions)).ok());

            auto vs = (*vs_ring)->Map(2048, std::max<u64>(256, backend->Caps().uniform_buffer_offset_alignment));
            REQUIRE(vs.ok());
            std::memset(vs->data.data(), 0, 2048);
            const shader::PicaUniformData pica_data = shader::MakePicaUniformData(state.vs);
            const shader::VertexUniformData vs_data = shader::MakeVertexUniformData(state, false);
            std::memcpy(vs->data.data(), &pica_data, sizeof(pica_data));
            std::memcpy(vs->data.data() + 1792, &vs_data, sizeof(vs_data));
            REQUIRE((*vs_ring)->Commit(2048).ok());

            native::DrawDesc d;
            d.vertex.kind = native::VertexStageKind::Translated;
            d.vertex.program = config.Hash();
            d.vertex.input = input;
            const FragmentKey key = DefaultKey();
            d.fragment = &key;
            d.color_format = rhi::Format::Rgba8Unorm;
            d.target.color = image->get();
            d.target.area = rhi::Rect{0, 0, kSize, kSize};
            d.vs_pica_uniforms = native::UniformRange{(*vs_ring)->GetBuffer().get(), vs->offset, 1616};
            d.vs_uniforms = native::UniformRange{(*vs_ring)->GetBuffer().get(), vs->offset + 1792, 32};
            d.fragment_uniforms = *alloc;
            d.tables = tables->get();
            const rhi::VertexBufferBinding binding{(*vertex_ring)->GetBuffer().get(), v->offset};
            d.vertex_buffers = std::span<const rhi::VertexBufferBinding>(&binding, 1);
            d.count = 3;
            d.viewport = rhi::Viewport{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
            d.scissor = rhi::Rect{0, 0, kSize, kSize};
            rhi::TargetPassDesc clear;
            clear.color = image->get();
            clear.color_load = rhi::LoadOp::Clear;
            clear.clear_color = {0, 0, 0, 0};
            clear.area = d.target.area;
            (*composer)->BeginTargetPass(rec, clear);
            auto result = (*composer)->Draw(rec, d);
            REQUIRE_MESSAGE(result.ok(), result.error());
            backend->WaitIdle(backend->Submit(std::move(rec)));
            auto pixels = backend->ReadImage(**image, 0, 0, kSize, kSize);
            REQUIRE(pixels.ok());
            // every pixel is the same: the varyings are constant over the triangle
            for (u32 i = 1; i < kSize * kSize; ++i) {
                REQUIRE(std::memcmp(pixels->data(), pixels->data() + 4 * i, 4) == 0);
            }
            out.push_back(Pixel(*pixels, 0, 0));
        }
        return out;
    };

    auto near = [](std::array<int, 4> got, std::array<int, 4> want) {
        for (int c = 0; c < 4; ++c) {
            if (std::abs(got[c] - want[c]) > 1) return false;
        }
        return true;
    };

    // Every semantic assigned: each varying shows the constants of its output register.
    const auto all = draw_all(false);
    REQUIRE(all.size() == 7);
    CHECK_MESSAGE(near(all[0], {51, 102, 153, 204}), "primary_color " << all[0][0] << "," << all[0][1] << "," << all[0][2] << "," << all[0][3]);
    CHECK_MESSAGE(near(all[1], {26, 77, 0, 255}), "texcoord0 " << all[1][0] << "," << all[1][1]);
    CHECK_MESSAGE(near(all[2], {128, 179, 0, 255}), "texcoord1 " << all[2][0] << "," << all[2][1]);
    CHECK_MESSAGE(near(all[3], {230, 38, 0, 255}), "texcoord2 " << all[3][0] << "," << all[3][1]);
    CHECK_MESSAGE(near(all[4], {90, 0, 0, 255}), "texcoord0_w " << all[4][0]);
    CHECK_MESSAGE(near(all[5], {64, 115, 166, 217}), "normquat " << all[5][0] << "," << all[5][1] << "," << all[5][2] << "," << all[5][3]);
    CHECK_MESSAGE(near(all[6], {13, 140, 191, 255}), "view " << all[6][0] << "," << all[6][1] << "," << all[6][2]);

    // texcoord1 left unassigned reads 1.0 (Azahar's vertex generator: `semantic()` returns "1.0"), the others are unchanged.
    const auto missing = draw_all(true);
    CHECK_MESSAGE(near(missing[2], {255, 255, 0, 255}), "texcoord1 unassigned " << missing[2][0] << "," << missing[2][1]);
    CHECK(near(missing[1], {26, 77, 0, 255}));
    CHECK(near(missing[0], {51, 102, 153, 204}));

    // A colour output above 1 or below 0 is min(abs(c), 1).
    uniform(1, {1.5f, -B(51), B(153), -2.0f});
    const auto saturated = draw_all(false);
    CHECK_MESSAGE(near(saturated[0], {255, 51, 153, 255}), "colour " << saturated[0][0] << "," << saturated[0][1] << "," << saturated[0][2] << ","
                                                                    << saturated[0][3]);
}

// ---- `--gpu native` ---------------------------------------------------------------------------------------------------------

namespace {

constexpr u32 kFcram = mh4n::gpu_capture::kFcramPaddr;
constexpr u32 kVertexBase = kFcram + 0x100000;
constexpr u32 kColorBuffer = mh4n::gpu_capture::kVramPaddr + 0x10000;
constexpr u32 kListAddress = kFcram + 0x1000;

constexpr u32 kRegUseGs = 0x229, kRegTopology = 0x25E, kRegOutTotal = 0x4F, kRegOutMap = 0x50;
constexpr u32 kRegColorAddress = 0x11D, kRegColorFormat = 0x117, kRegDepthFormat = 0x116, kRegFbSize = 0x11E;
constexpr u32 kRegAlphaTestWord = 0x104;
constexpr u32 kGs = 0x280, kVs = 0x2B0;

// A command list as the command processor reads it: [value, header] pairs (a header is the register id and the four-byte mask).
class CmdList {
public:
    CmdList& Write(u32 id, u32 value) {
        words_.push_back(value);
        words_.push_back(id | (0xFu << 16));
        return *this;
    }
    // A program and its operand descriptors into a shader unit (`base` is kVs or kGs).
    CmdList& Program(u32 base, const std::vector<u32>& code, const std::vector<u32>& descs) {
        Write(base + 0x1B, 0);
        for (u32 w : code) Write(base + 0x1C, w);
        Write(base + 0x25, 0);
        for (u32 w : descs) Write(base + 0x26, w);
        return *this;
    }
    // float32 uniforms from index `first`: four words per vector, w first.
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

// The registers every draw of these cases shares: an 8x8 RGBA8 colour buffer, a viewport over it, no depth, no blending, stage 0 the
// primary colour (the stages after it hold what a zero register holds: the primary colour again), lighting off.
void FramebufferRegisters(CmdList& l, u32 alpha_test_word = 0) {
    l.Write(kRegColorAddress, kColorBuffer >> 3);
    l.Write(kRegColorFormat, 0);  // RGBA8
    l.Write(kRegDepthFormat, 0);
    l.Write(0x11C, 0);  // no depth buffer
    l.Write(kRegFbSize, kSize | ((kSize - 1) << 12) | (1u << 24));  // width, height - 1, not flipped
    l.Write(0x113, 0xF);
    l.Write(0x115, 0);
    l.Write(0x100, 0);                           // default fragment operation, no blending
    l.Write(0x102, 3);                           // logic op Copy
    l.Write(0x107, 0xF00);                       // colour writes
    l.Write(kRegAlphaTestWord, alpha_test_word);
    l.Write(0x1C6, 1);                           // lighting disabled
    l.Write(0x41, F24(kSize / 2.0f));            // viewport half-width and half-height
    l.Write(0x43, F24(kSize / 2.0f));
    l.Write(0x68, 0);                            // viewport corner
    l.Write(0x4D, F24(-1.0f));                   // depth range and offset
    l.Write(0x4E, 0);
    l.Write(0x6D, 1);                            // Z-buffering
    l.Write(0x65, 0);                            // no scissor
    l.Write(0x40, 0);                            // cull mode: keep all
    const Stage pass{};
    for (u32 base : {0xC0u, 0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u}) {
        const TevStageConfigRaw raw = base == 0xC0 ? [] { Stage s; s.color_src[0] = kPrimaryColor; s.alpha_src[0] = kPrimaryColor; return s.Raw(); }() : pass.Raw();
        l.Write(base + 0, raw.sources_raw);
        l.Write(base + 1, raw.modifiers_raw);
        l.Write(base + 2, raw.ops_raw);
    }
}

// The vertex program: o0 = v0 (position), o1 = v1 (colour); the rasterizer maps o0 to the position and o1 to the colour.
void VertexProgram(CmdList& l, u32 outputs) {
    using namespace pi;
    std::vector<u32> code = {Ins(MOV, O(0), V(0), 0), Ins(MOV, O(1), V(1), 0), kEnd};
    l.Program(kVs, code, {IdentityDescriptor()});
    l.Write(kVs + 0x09, 1);                       // two input attributes
    l.Write(kVs + 0x0A, 0);                       // main offset
    l.Write(kVs + 0x0B, 0x76543210u);             // attribute n feeds input register n
    l.Write(kVs + 0x0C, 0xFEDCBA98u);
    l.Write(kVs + 0x0D, (1u << outputs) - 1);     // the outputs o0 and o1
    l.Write(kRegOutTotal, 2);
    l.Write(kRegOutMap + 0, 0u | (1u << 8) | (2u << 16) | (3u << 24));      // o0: the position
    l.Write(kRegOutMap + 1, 8u | (9u << 8) | (10u << 16) | (11u << 24));    // o1: the colour
    l.Write(0x24A, outputs - 1);
    l.Write(0x251, outputs - 1);
    // the vertex arrays: two float4 attributes in one loader
    l.Write(0x200, (kVertexBase >> 4) << 1);
    l.Write(0x201, 0xFFu);
    l.Write(0x202, 1u << 28);
    l.Write(0x203, 0);
    l.Write(0x204, 0x10u);
    l.Write(0x205, (32u << 16) | (2u << 28));
}

// The geometry shader of a draw that cannot be accelerated: a Point-mode program that turns each point into the triangle
// (P + c0, P + c1, P + c2), with the point's colour.
void GeometryProgram(CmdList& l) {
    using namespace pi;
    std::vector<u32> code = {SetEmitOp(0, 0, 0), Ins(ADD, O(0), C(0), V(0)), Ins(MOV, O(1), V(1), 0), kEmit,
                             SetEmitOp(1, 0, 0), Ins(ADD, O(0), C(1), V(0)), Ins(MOV, O(1), V(1), 0), kEmit,
                             SetEmitOp(2, 1, 0), Ins(ADD, O(0), C(2), V(0)), Ins(MOV, O(1), V(1), 0), kEmit, kEnd};
    l.Program(kGs, code, {IdentityDescriptor()});
    l.Uniforms(kGs, 0, {{-1, -1, 0, 0}, {3, -1, 0, 0}, {-1, 3, 0, 0}});
    l.Write(0x252, 0u | (0u << 8) | (1u << 12));  // Point mode: fixed vertex count unused, two outputs per vertex
    l.Write(0x254, 0);
    l.Write(kGs + 0x09, 1u | (0x08u << 24));       // two input registers (a vertex's two outputs), geometry shader mode
    l.Write(kGs + 0x0A, 0);
    l.Write(kGs + 0x0B, 0x76543210u);
    l.Write(kGs + 0x0C, 0xFEDCBA98u);
    l.Write(kGs + 0x0D, 0x3);                      // the geometry shader writes o0 and o1
    l.Write(kRegTopology, 3u << 8);                // the Shader topology
}

std::vector<u8> FloatBytes(const std::vector<std::array<float, 4>>& vectors) {
    std::vector<u8> bytes(vectors.size() * 16);
    std::memcpy(bytes.data(), vectors.data(), bytes.size());
    return bytes;
}

struct Sim {
    mh4n::gpu_capture::CaptureHost host;
    std::unique_ptr<native::NativeGpu> gpu;

    explicit Sim(native::NativeOptions options = {}) : gpu(native::MakeNativeGpu(host, options)) {}

    void Write(u32 paddr, const std::vector<u8>& bytes) {
        REQUIRE(host.WriteBoth(paddr, bytes.data(), static_cast<u32>(bytes.size())));
    }
    // Runs a command list through the GX command SubmitCmdList.
    void Submit(const CmdList& list) {
        const std::vector<u8> bytes = list.Bytes();
        Write(kListAddress, bytes);
        mh4n::gpu::GxCommand command{};
        command.words[0] = 1;  // SubmitCmdList
        command.words[1] = kListAddress;
        command.words[2] = static_cast<u32>(bytes.size());
        gpu->Execute(command);
    }
    // The colour buffer as RGBA8 bytes, after the GPU wrote it back.
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
};

// The vertices of the full-screen triangle (position, colour) as the vertex arrays hold them.
std::vector<u8> TriangleVertices(std::array<int, 4> colour) {
    const std::array<float, 4> c = Bytes4(colour[0], colour[1], colour[2], colour[3]);
    return FloatBytes({{-1, -1, -0.5f, 1}, c, {3, -1, -0.5f, 1}, c, {-1, 3, -0.5f, 1}, c});
}

bool UniformColour(const std::vector<u8>& rgba, std::array<int, 4> want, std::string* why = nullptr) {
    return AllNear(rgba, want, 0, why);
}

}  // namespace

TEST_CASE("native/registered_with_its_option_and_makes_no_device_in_the_constructor") {
    CHECK(Registry<mh4n::gpu::GpuFactory>::Instance().Find("native") != nullptr);
    CHECK(Usage("capture-player").find("--force-ubershader") != std::string::npos);
    // Constructed with no device touched: this runs (and passes) on a machine without Vulkan, and nothing was drawn.
    Sim sim;
    CHECK(sim.Counter("native_draws") == 0);
}

TEST_CASE("native/a_geometry_shader_draw_is_drawn_by_the_cpu_path_with_the_expected_pixels") {
    if (!test::RequireBackend()) return;
    Sim sim;
    CmdList l;
    l.Write(kRegUseGs, 2u);  // first: the geometry shader on, so that the unit is not a mirror of the vertex shader's
    FramebufferRegisters(l);
    VertexProgram(l, 2);
    GeometryProgram(l);
    sim.Write(kVertexBase, FloatBytes({{0, 0, -0.5f, 1}, Bytes4(51, 102, 153, 204)}));  // one point: position, colour
    l.Write(0x228, 1).Write(0x22A, 0).Write(0x22E, 0);                                   // DrawArrays of one vertex
    sim.Submit(l);
    const std::vector<u8> pixels = sim.ColorBuffer();
    std::string why;
    CHECK_MESSAGE(UniformColour(pixels, {51, 102, 153, 204}, &why), why);
    CHECK(sim.Counter("cpu_path_draws") == 1);
    CHECK(sim.Counter("cpu_path_unhandled") == 0);
    CHECK(sim.Counter("native_draws") == 1);
    CHECK(sim.Counter("native_draws_dropped") == 0);
    CHECK(sim.Counter("pipeline_sync_builds") >= 1);  // the default provider builds on a miss
}

TEST_CASE("native/an_accelerated_draw_takes_the_translated_vertex_stage_and_matches_the_cpu_path") {
    if (!test::RequireBackend()) return;
    Sim sim;
    CmdList l;
    FramebufferRegisters(l);
    VertexProgram(l, 2);
    l.Write(kRegUseGs, 0);
    l.Write(kRegTopology, 0);  // List
    sim.Write(kVertexBase, TriangleVertices({51, 102, 153, 204}));
    l.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    sim.Submit(l);
    std::string why;
    CHECK_MESSAGE(UniformColour(sim.ColorBuffer(), {51, 102, 153, 204}, &why), why);
    CHECK(sim.Counter("cpu_path_draws") == 0);
    CHECK(sim.Counter("translate_failures") == 0);
    CHECK(sim.Counter("native_draws") == 1);
}

TEST_CASE("native/force_ubershader_gives_the_same_pixels_and_counts_the_fallback") {
    if (!test::RequireBackend()) return;
    native::NativeOptions forced;
    forced.force_ubershader = true;
    Sim normal, uber(forced);
    for (Sim* sim : {&normal, &uber}) {
        CmdList l;
        FramebufferRegisters(l);
        VertexProgram(l, 2);
        l.Write(kRegUseGs, 0);
        l.Write(kRegTopology, 0);
        sim->Write(kVertexBase, TriangleVertices({200, 100, 50, 255}));
        l.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
        sim->Submit(l);
    }
    const std::vector<u8> a = normal.ColorBuffer(), b = uber.ColorBuffer();
    std::string why;
    CHECK_MESSAGE(UniformColour(a, {200, 100, 50, 255}, &why), why);
    CHECK(a == b);
    CHECK(normal.Counter("pipeline_forced_uber") == 0);
    CHECK(normal.Counter("fallback_draws") == 0);
    CHECK(uber.Counter("pipeline_forced_uber") == 1);
    CHECK(uber.Counter("fallback_draws") == 1);
}

TEST_CASE("native/a_draw_that_needs_a_stubbed_feature_is_counted") {
    if (!test::RequireBackend()) return;
    Sim sim;
    CmdList l;
    FramebufferRegisters(l);
    VertexProgram(l, 2);
    l.Write(kRegUseGs, 0);
    l.Write(kRegTopology, 0);
    l.Write(0x1C6, 0);   // lighting on
    l.Write(0xE0, 5);    // fog mode Fog
    sim.Write(kVertexBase, TriangleVertices({1, 2, 3, 4}));
    l.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    sim.Submit(l);
    CHECK(sim.Counter("stubbed:lighting") == 1);
    CHECK(sim.Counter("stubbed:fog") == 1);
    CHECK(sim.Counter("stubbed:sampling") == 0);
    CHECK(sim.Counter("native_draws") == 1);
}

TEST_CASE("native/the_colour_buffer_is_loaded_again_after_the_cpu_wrote_its_memory") {
    if (!test::RequireBackend()) return;
    Sim sim;
    CmdList l;
    FramebufferRegisters(l);
    VertexProgram(l, 2);
    l.Write(kRegUseGs, 0);
    l.Write(kRegTopology, 0);
    sim.Write(kVertexBase, TriangleVertices({200, 100, 50, 255}));
    l.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    sim.Submit(l);
    std::string why;
    CHECK_MESSAGE(UniformColour(sim.ColorBuffer(), {200, 100, 50, 255}, &why), why);

    // the CPU fills the buffer with 0x5A bytes and flushes; a draw whose every fragment fails the alpha test (Never) leaves them
    sim.Write(kColorBuffer, std::vector<u8>(kSize * kSize * 4, 0x5A));
    sim.gpu->FlushRegion(0x1F000000 + (kColorBuffer - mh4n::gpu_capture::kVramPaddr), kSize * kSize * 4);  // the VRAM window
    CmdList second;
    second.Write(kRegAlphaTestWord, 1u | (0u << 4));  // enabled, Never
    second.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    sim.Submit(second);
    CHECK_MESSAGE(UniformColour(sim.ColorBuffer(), {0x5A, 0x5A, 0x5A, 0x5A}, &why), why);

    // and a draw that passes covers it again
    CmdList third;
    third.Write(kRegAlphaTestWord, 0);
    third.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    sim.Submit(third);
    CHECK_MESSAGE(UniformColour(sim.ColorBuffer(), {200, 100, 50, 255}, &why), why);
}

TEST_CASE("native/the_gpu_made_on_one_thread_creates_its_images_on_the_thread_that_executes") {
    // Under the capture player's --queue the GPU is constructed on the player's thread and Execute runs on the renderer thread.
    if (!test::RequireBackend()) return;
    Sim sim;
    CmdList l;
    FramebufferRegisters(l);
    VertexProgram(l, 2);
    l.Write(kRegUseGs, 0);
    l.Write(kRegTopology, 0);
    sim.Write(kVertexBase, TriangleVertices({10, 20, 30, 40}));
    l.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    std::thread renderer([&] { sim.Submit(l); sim.gpu->WriteBackColorBuffers(); });
    renderer.join();
    const u8* memory = sim.host.PhysicalPointer(kColorBuffer, kSize * kSize * 4);
    REQUIRE(memory != nullptr);
    std::vector<u8> rgba;
    REQUIRE(texture::DecodeToRgba8(memory, kSize * kSize * 4, kSize, kSize, texture::Format::RGBA8, rgba));
    std::string why;
    CHECK_MESSAGE(UniformColour(rgba, {10, 20, 30, 40}, &why), why);
}

TEST_CASE("native/the_frame_counters_and_the_gpu_time_are_reported_per_frame_and_reset") {
    if (!test::RequireBackend()) return;
    Sim sim;
    CmdList l;
    FramebufferRegisters(l);
    VertexProgram(l, 2);
    l.Write(kRegUseGs, 0);
    l.Write(kRegTopology, 0);
    sim.Write(kVertexBase, TriangleVertices({1, 2, 3, 4}));
    l.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    l.Write(0x22E, 0);  // a second draw of the same vertices
    sim.Submit(l);
    sim.gpu->WriteBackColorBuffers();
    const std::vector<mh4n::gpu::Counter> counters = sim.gpu->TakeFrameCounters();
    auto value = [&](const std::string& name) -> long {
        for (const mh4n::gpu::Counter& c : counters) {
            if (c.name == name) return static_cast<long>(c.value);
        }
        return -1;
    };
    CHECK(value("native_draws") == 2);
    CHECK(value("pipeline_sync_builds") == 1);  // the second draw finds the pipeline the first one built
    CHECK(value("cpu_path_draws") == 0);
    CHECK(value("vertex_restart_widened") == 0);
    CHECK(value("stubbed:fixup") == 0);
    CHECK(value("stubbed:lighting") == 0);
    CHECK(value("stubbed:gas") == 0);
    // reading resets them
    for (const mh4n::gpu::Counter& c : sim.gpu->TakeFrameCounters()) CHECK_MESSAGE(c.value == 0, c.name);
    // the GPU time of the frame: present once, then none until the next frame draws
    const std::optional<u64> time = sim.gpu->TakeFrameGpuTimeNs();
    CHECK(time.has_value());
    CHECK_FALSE(sim.gpu->TakeFrameGpuTimeNs().has_value());
}

// ---- the emitter's helpers, as a feature uses them -------------------------------------------------------------------------------

namespace {

// A feature written the way N2-09 to N2-12 will write theirs: it reads key fields through the emitter, so that the same code
// makes the specialised shader and the ubershader. At the AlphaTest point it computes `acc` from a loop over a count, a switch and
// a select, and writes it to the red channel of the stage output.
class EmitterProbe final : public FeatureModule {
public:
    const char* Name() const override { return "lighting"; }  // takes a name of the five: the private registry is the test's
    void Emit(Point point, EmitContext& ctx) const override {
        if (point != Point::AlphaTest) return;
        Emitter& e = *ctx.emitter;
        e.Raw("float acc = 0.0;\n");
        // the lights: src_num of the lighting part (bits 1-4 of its first word) lights, each adds 0.05 * (index + 1)
        const Value count = e.Field(keyword::kLightRaw, 1, 4);
        e.ForEach(count, 8, "probe_i", [&](const std::string& index) { e.Line("acc += 0.05 * float({} + 1u);", index); });
        // a switch on the bump mode (bits 5-6): 0 adds 0.1, 1 adds 0.2, anything else 0.3
        const Value mode = e.Field(keyword::kLightRaw, 5, 2);
        e.Switch(
            mode, {{0, [&] { e.Raw("acc += 0.1;\n"); }}, {1, [&] { e.Raw("acc += 0.2;\n"); }}}, [&] { e.Raw("acc += 0.3;\n"); });
        // an if on a bit (clamp_highlights, bit 10), and a select between two numbers
        e.If(e.Ne(e.Field(keyword::kLightRaw, 10, 1), 0), [&] { e.Raw("acc += 0.01;\n"); }, [&] { e.Raw("acc += 0.02;\n"); });
        const Value which = e.Select(e.Eq(e.Field(keyword::kLightRaw, 7, 2), 2), Emitter::Const(7), Emitter::Const(9));
        e.Raw(fmt::format("acc += float({}) * 0.001;\n", which.glsl));
        e.Raw("combiner_output.r = clamp(acc, 0.0, 1.0);\n");
    }
};

}  // namespace

TEST_CASE("emitter/a_feature_written_with_the_helpers_gives_the_same_pixels_in_both_modes") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    EmitterProbe probe;
    FeatureRegistry registry;
    REQUIRE(registry.Add(probe));  // in the place of the lighting stub; the other four are the stubs, which define the prototypes
    for (const char* name : {"sampling", "fog", "proctex", "shadow"}) {
        REQUIRE(FeatureRegistry::Global().Find(name) != nullptr);
        REQUIRE(registry.Add(*FeatureRegistry::Global().Find(name)));
    }
    Rig spec_rig(backend, false, &registry), uber_rig(backend, true, &registry);
    struct Case {
        u32 lights, bump, selector, clamp_highlights;
        double want;  // acc
    };
    const Case cases[] = {
        {3, 0, 2, 1, 0.05 * (1 + 2 + 3) + 0.1 + 0.01 + 0.007},
        {1, 1, 0, 0, 0.05 * 1 + 0.2 + 0.02 + 0.009},
        {8, 2, 2, 0, 0.05 * 36 + 0.3 + 0.02 + 0.007},
        {0, 0, 1, 1, 0.0 + 0.1 + 0.01 + 0.009},
    };
    for (const Case& c : cases) {
        DrawSpec spec;
        spec.key.lighting.set_src_num(c.lights);
        spec.key.lighting.set_bump_mode(static_cast<re::LightingBumpMode>(c.bump));
        spec.key.lighting.set_bump_selector(c.selector);
        spec.key.lighting.set_clamp_highlights(c.clamp_highlights);
        Stage s;
        s.color_src[0] = kPrimaryColor;
        s.alpha_src[0] = kPrimaryColor;
        spec.key.texture.tev_stages[0] = s.Raw();
        spec.tri = FullTriangle({10, 20, 30, 255});
        const int want_red = static_cast<int>(std::round(std::min(1.0, c.want) * 255.0));
        const std::vector<u8> a = spec_rig.Render({spec});
        const std::vector<u8> b = uber_rig.Render({spec});
        CHECK_MESSAGE(std::abs(Pixel(a, 0, 0)[0] - want_red) <= 1, "lights " << c.lights << " bump " << c.bump << ": red " << Pixel(a, 0, 0)[0]
                                                                          << " expected " << want_red);
        CHECK(Pixel(a, 0, 0)[1] == 20);
        CHECK_MESSAGE(SameWithin(a, b), "lights " << c.lights << " bump " << c.bump);
        CHECK(uber_rig.last.fallback);
    }
}

TEST_CASE("native/a_depth_buffer_is_written_back_in_the_pica_layout") {
    if (!test::RequireBackend()) return;
    Sim sim;
    const u32 depth_buffer = mh4n::gpu_capture::kVramPaddr + 0x80000;
    CmdList l;
    FramebufferRegisters(l);
    VertexProgram(l, 2);
    l.Write(kRegUseGs, 0);
    l.Write(kRegTopology, 0);
    l.Write(0x11C, depth_buffer >> 3);
    l.Write(kRegDepthFormat, 3);              // D24S8
    l.Write(0x115, 3);                        // depth and stencil writes allowed
    l.Write(0x107, 0xF00 | 1u | (1u << 4) | (1u << 12));  // depth test Always, depth write
    sim.Write(kVertexBase, TriangleVertices({1, 2, 3, 4}));
    l.Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    sim.Submit(l);
    sim.gpu->WriteBackColorBuffers();
    const u8* memory = sim.host.PhysicalPointer(depth_buffer, kSize * kSize * 4);
    REQUIRE(memory != nullptr);
    // z = -0.5 with the depth scale -1 and offset 0 is depth 0.5; D24S8 holds the depth in the low 24 bits, the stencil in the high 8
    for (u32 i = 0; i < kSize * kSize; ++i) {
        u32 word;
        std::memcpy(&word, memory + 4 * i, 4);
        const u32 depth = word & 0xFFFFFF;
        CHECK_MESSAGE((depth >= 0x7FFFFE && depth <= 0x800000), "pixel word " << i << " is " << word);
        CHECK((word >> 24) == 0);
    }
}

TEST_CASE("native/indexed_draws_with_8_and_16_bit_indices_and_a_strip_are_drawn") {
    if (!test::RequireBackend()) return;
    for (int variant = 0; variant < 3; ++variant) {
        Sim sim;
        CmdList l;
        FramebufferRegisters(l);
        VertexProgram(l, 2);
        l.Write(kRegUseGs, 0);
        sim.Write(kVertexBase, TriangleVertices({33, 66, 99, 255}));
        if (variant == 0) {  // 8-bit indices, a list
            sim.Write(kVertexBase + 0x80000, {2, 1, 0});
            l.Write(kRegTopology, 0).Write(0x227, 0x80000).Write(0x228, 3).Write(0x22F, 0);
        } else if (variant == 1) {  // 16-bit indices, a list
            sim.Write(kVertexBase + 0x80000, {2, 0, 1, 0, 0, 0});
            l.Write(kRegTopology, 0).Write(0x227, 0x80000 | (1u << 31)).Write(0x228, 3).Write(0x22F, 0);
        } else {  // a strip of the three vertices (a strip never leaves the assembler empty: the next list draw would be refused)
            l.Write(kRegTopology, 1u << 8).Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
        }
        sim.Submit(l);
        std::string why;
        CHECK_MESSAGE(UniformColour(sim.ColorBuffer(), {33, 66, 99, 255}, &why), "variant " << variant << ": " << why);
        CHECK(sim.Counter("native_draws_dropped") == 0);
    }
}
