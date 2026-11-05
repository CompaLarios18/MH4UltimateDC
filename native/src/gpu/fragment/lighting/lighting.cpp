// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (FragmentModule::WriteLighting, DefineLightingHelpers and the
// lighting call in FragmentModule::Generate), src/video_core/shader/generator/glsl_shader_gen.cpp (GenerateFixedGeometryShader,
// GetGSCommonSource, SanitizeVertex and the vertex interface: the fixed geometry shader), src/video_core/shader/generator/shader_gen.h
// and shader_gen.cpp (PicaGSConfigState::GetSemanticMaps: an unmapped semantic is attribute 16, read as 1.0),
// src/video_core/renderer_software/sw_lighting.cpp (ComputeFragmentsColors: the oracle of the formulas, and how the shadow, bump and LUT
// terms are defined), src/video_core/renderer_software/sw_clipper.cpp and sw_rasterizer.cpp (FlipQuaternionIfOpposite and where it is
// applied: vertex 1 and vertex 2 against vertex 0 before clipping), src/video_core/rasterizer_accelerated.cpp (SyncDrawUniforms: the
// light and global-ambient uniforms) and src/video_core/pica/dirty_regs.h (CheckLight and CheckLightingAmbient: the registers that
// dirty them), src/video_core/renderer_vulkan/vk_rasterizer.cpp (SyncAndUploadLUTsLF: the lighting tables in lut_lf as {value,
// difference} pairs and the offset kept in the uniforms), src/video_core/pica/pica_core.h (Lighting::LutEntry: the entry's bit fields),
// src/video_core/pica_types.h (Float::FromRaw: the 16-bit and 20-bit floats of the light registers) and
// src/video_core/pica/regs_lighting.h (the register layout, the sampler numbers and IsLightingSamplerSupported) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: glsl_fs_shader_gen.cpp and vk_rasterizer.cpp say "Licensed under GPLv3 or any
// later version"; glsl_shader_gen.cpp, shader_gen.h, shader_gen.cpp, sw_lighting.cpp, sw_clipper.cpp, sw_rasterizer.cpp,
// rasterizer_accelerated.cpp, dirty_regs.h, pica_core.h, pica_types.h and regs_lighting.h say "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp, glsl_shader_gen.cpp, shader_gen.h,
//   shader_gen.cpp, vk_rasterizer.cpp, rasterizer_accelerated.cpp, pica_core.h)
// Copyright 2017 Citra Emulator Project (sw_lighting.cpp, regs_lighting.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (sw_clipper.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp, pica_types.h)
// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project (dirty_regs.h)
//
// The `lighting/` feature (N2-09): up to eight lights, the lookup tables (D0, D1, Fresnel, the three reflection channels, spot and
// distance attenuation), bump and tangent mapping, the highlight clamp, the shadow term, and the fixed geometry shader that fixes the
// interpolation of the normal-quaternion.
//
// THE CODE is Azahar's WriteLighting, in both modes (emitter.h): the specialised shader evaluates the key and writes the lights out
// one by one; the ubershader reads the key from `cfg` and loops over `src_num` lights. It runs at the `Lighting` point, in a block of its
// own, and ends by writing the globals `primary_fragment_color` and `secondary_fragment_color`. Its uniforms are `light_src[8]`,
// `lighting_global_ambient` and `lighting_lut_offset[6]`; its tables the 24 of `lut_lf`.
//
// WHERE THIS DIFFERS FROM AZAHAR'S GLSL, ON PURPOSE:
//  - TEXTURE SAMPLES (N2-43, the ubershader only). Azahar calls `sampleTexUnit{bump_selector}()` once per bump mode and
//    `sampleTexUnit{shadow_selector}()` once for the shadow. The ubershader emits both bump modes and the drivers compile every call
//    site, so there it samples each unit the bump and shadow terms read AT MOST ONCE, before the bump-mode branch and the light loop,
//    into `mh4n_lt0` .. `3`: four call sites at the top level of main, each under a condition that reads only `cfg` (uniform, so
//    derivatives stay defined), and then picks by selector (`mh4n_bump_texel`, `mh4n_shadow_texel`, no call). Selector 3 is the
//    procedural unit, whose `sampleTexUnit3()` calls `ProcTex()`: it is sampled once whatever the two selectors say. The specialised
//    shader keeps Azahar's text (one bump mode is emitted there: `sampleTexUnit{n}()` in the bump term and in the shadow term).
//    Core's `mh4n_tex0` .. `3` are not read: they hold a unit only when a TEV stage reads it and are set after this point.
//  - THE LUT CALLS (the ubershader only). Azahar's `LookupLightingLUTUnsigned` and `LookupLightingLUTSigned` each call
//    `LookupLightingLUT`; the ubershader reads the table's type and absolute-input bit from `cfg`, so a call written with both would
//    be two call sites per table. `mh4n_lut` has the two bodies' arithmetic in one function with one call of `LookupLightingLUT`
//    (once per LUT in the light loop, as in Azahar), and `mh4n_lut_input` picks the LUT's input from values computed once per light
//    (`mh4n_in_*`; Azahar writes the one expression the key names). The specialised shader has Azahar's helpers and expressions.
//  - Locals instead of inline expressions: `spot_atten`, `dist_atten`, `d0_lut_value`, `d1_lut_value` are declared and assigned in
//    both modes (Azahar embeds the strings); `surface_normal` and `surface_tangent` are declared with the no-bump values and assigned
//    by the bump cases (Azahar declares them in each case); the Fresnel value is a local used by the two alpha statements.
//  - `user.use_custom_normal` (the HD pack's normal texture, `tex_normal`) is not implemented: the user word is the HD pack's (N2-17).
//  - The quaternion route is Azahar's non-barycentric one (`normquat` as the interpolated varying); the barycentric route
//    (`use_fragment_shader_barycentric`) is not offered by the vendored backend (design 5.5).
//  - UNIFORMS. `light_src[i].spot_direction` is the register's spot vector over 2047, not negated (rasterizer_accelerated.cpp at the
//    pinned commit; uniforms.h's comment says "negated", which is the older text; sw_lighting.cpp's `Dot(light_vector, spot_dir)` agrees
//    with the unnegated vector).
//
// THE FIXED GEOMETRY SHADER (design 5.5; Azahar's GenerateFixedGeometryShader): see lighting.h. Registered with N2-33's slot
// (`native::FixupGeneratorRegistry`) at start-up. NOTE FOR N1'S DEPENDENCY RULE: this is the one place `fragment/` includes
// `gpu/native/` (compose.h, for `VertexStageOutputs` and the registry). `mh4n_gpu_native` depends on `mh4n_gpu_fragment`, so the
// module cannot name it in DEPENDS (a cycle); it links because every binary that links `mh4n_gpu_fragment` gets
// `FixupGeneratorRegistry::Global()` through `mh4n::all`, which links every module whole-archive.
#include "gpu/fragment/lighting/lighting.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/regs.h"
#include "gpu/fragment/tables.h"
#include "gpu/fragment/uniforms.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

namespace kw = keyword;

// ---- the key's words ---------------------------------------------------------------------------------------------------------

// LightConfig: raw, then seven LutConfigs of two words (raw, scale bits) in the order d0, d1, sp, fr, rr, rg, rb, then the eight
// lights as u16 each (two per word).
constexpr size_t kLutD0 = kw::kLighting + offsetof(LightConfig, lut_d0) / 4;
constexpr size_t kLutD1 = kw::kLighting + offsetof(LightConfig, lut_d1) / 4;
constexpr size_t kLutSp = kw::kLighting + offsetof(LightConfig, lut_sp) / 4;
constexpr size_t kLutFr = kw::kLighting + offsetof(LightConfig, lut_fr) / 4;
constexpr size_t kLutRr = kw::kLighting + offsetof(LightConfig, lut_rr) / 4;
constexpr size_t kLutRg = kw::kLighting + offsetof(LightConfig, lut_rg) / 4;
constexpr size_t kLutRb = kw::kLighting + offsetof(LightConfig, lut_rb) / 4;
constexpr size_t kLightsWord = kw::kLighting + offsetof(LightConfig, lights) / 4;
static_assert(kLutD0 == 43 && kLutD1 == 45 && kLutSp == 47 && kLutFr == 49 && kLutRr == 51 && kLutRg == 53 && kLutRb == 55 &&
              kLightsWord == 57);

// LightingRegs::LightingSampler.
constexpr u32 kSamplerD0 = 0, kSamplerD1 = 1, kSamplerFresnel = 3, kSamplerRb = 4, kSamplerRg = 5, kSamplerRr = 6;
constexpr u32 kSamplerSpot = 8, kSamplerDist = 16;

// LightingRegs::LightingConfig (Config7 is 8: "'8' is intentional").
constexpr u32 kConfig0 = 0, kConfig1 = 1, kConfig2 = 2, kConfig3 = 3, kConfig4 = 4, kConfig5 = 5, kConfig7 = 8;

constexpr u32 kBumpNone = 0, kBumpNormalMap = 1, kBumpTangentMap = 2;

// LightingRegs::LightingLutInput.
constexpr u32 kInputNH = 0, kInputVH = 1, kInputNV = 2, kInputLN = 3, kInputSP = 4, kInputCP = 5;

// ---- the GLSL helpers --------------------------------------------------------------------------------------------------------

// Azahar's DefineLightingHelpers (without the barycentric helper).
constexpr const char* kSpecialisedHelpers = R"(
float LookupLightingLUT(int lut_index, int index, float delta) {
    vec2 entry = texelFetch(texture_buffer_lut_lf, lighting_lut_offset[lut_index >> 2][lut_index & 3] + index).rg;
    return entry.r + entry.g * delta;
}

float LookupLightingLUTUnsigned(int lut_index, float pos) {
    int index = int(clamp(floor(pos * 256.0), 0.f, 255.f));
    float delta = pos * 256.0 - float(index);
    return LookupLightingLUT(lut_index, index, delta);
}

float LookupLightingLUTSigned(int lut_index, float pos) {
    int index = int(clamp(floor(pos * 128.0), -128.f, 127.f));
    float delta = pos * 128.0 - float(index);
    if (index < 0) index += 256;
    return LookupLightingLUT(lut_index, index, delta);
}
)";

// The ubershader's: one LookupLightingLUT call in `mh4n_lut`, the arithmetic of the two bodies above, and the input picker.
constexpr const char* kUberHelpers = R"(
float LookupLightingLUT(int lut_index, int index, float delta) {
    vec2 entry = texelFetch(texture_buffer_lut_lf, lighting_lut_offset[lut_index >> 2][lut_index & 3] + index).rg;
    return entry.r + entry.g * delta;
}

float mh4n_lut(int lut_index, float pos, bool abs_input, bool two_sided) {
    int index;
    float delta;
    if (abs_input) {
        pos = two_sided ? abs(pos) : max(pos, 0.0);
        index = int(clamp(floor(pos * 256.0), 0.f, 255.f));
        delta = pos * 256.0 - float(index);
    } else {
        index = int(clamp(floor(pos * 128.0), -128.f, 127.f));
        delta = pos * 128.0 - float(index);
        if (index < 0) index += 256;
    }
    return LookupLightingLUT(lut_index, index, delta);
}

float mh4n_lut_input(uint type, float nh, float vh, float nv, float ln, float sp, float cp) {
    switch (type) {
    case 0u: return nh;
    case 1u: return vh;
    case 2u: return nv;
    case 3u: return ln;
    case 4u: return sp;
    case 5u: return cp;
    default: return 0.0;
    }
}
)";

// GLSL text of a float constant (the scale of a LUT).
std::string FloatLiteral(float value) {
    if (!std::isfinite(value)) {
        return fmt::format("uintBitsToFloat({}u)", std::bit_cast<u32>(value));
    }
    std::string text = fmt::format("{}", value);
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return text;
}

// ---- the emitter -------------------------------------------------------------------------------------------------------------

// One light's key fields, as the mode sees them.
struct LightFields {
    std::string index;      // "3u", or the loop variable
    Value index_value;      // the same as a Value
    Value num;              // the register-file number of the light (0..7)
    Cond directional, two_sided, dist_atten, spot_atten, geo0, geo1, shadow;
};

class LightingCode {
public:
    explicit LightingCode(Emitter& emitter) : e_(emitter) {}

    // The Lighting point.
    void Emit() {
        e_.If(e_.Eq(Field(0, 1), 1), [&] {
            e_.Raw("{\n");
            Locals();
            if (!e_.specialised()) {
                Samples();
            }
            Surface();
            Quaternion();
            Shadow();
            const Value src_num = Field(1, 4);
            e_.ForEach(src_num, 8, "mh4n_light", [&](const std::string& index) { OneLight(index, src_num); });
            Finish();
            e_.Raw("}\n");
        });
    }

private:
    Emitter& e_;

    Value Field(u32 first, u32 count) const { return e_.Field(kw::kLightRaw, first, count); }
    Cond Flag(u32 bit) const { return e_.Eq(Field(bit, 1), 1); }

    void Locals() {
        e_.Raw("vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0);\n"
               "vec4 specular_sum = vec4(0.0, 0.0, 0.0, 1.0);\n"
               "vec3 light_vector = vec3(0.0);\n"
               "float light_distance = 0.0;\n"
               "vec3 refl_value = vec3(0.0);\n"
               "vec3 spot_dir = vec3(0.0);\n"
               "vec3 half_vector = vec3(0.0);\n"
               "float dot_product = 0.0;\n"
               "float clamp_highlights = 1.0;\n"
               "float geo_factor = 1.0;\n");
    }

    // ---- the texture samples ----------------------------------------------------------------------------------------------

    // The ubershader: each unit the bump or shadow term reads, sampled once.
    void Samples() {
        e_.Raw("vec4 mh4n_lt0 = vec4(0.0);\n"
               "vec4 mh4n_lt1 = vec4(0.0);\n"
               "vec4 mh4n_lt2 = vec4(0.0);\n"
               "vec4 mh4n_lt3 = vec4(0.0);\n");
        const Value bump_selector = Field(7, 2);
        const Value shadow_selector = Field(22, 2);
        for (u32 unit = 0; unit < 4; ++unit) {
            const Cond bump = e_.And(e_.Ne(Field(5, 2), kBumpNone), e_.Eq(bump_selector, unit));
            const Cond shadow = e_.And(Flag(17), e_.Eq(shadow_selector, unit));
            e_.If(e_.Or(bump, shadow), [&] { e_.Raw(fmt::format("mh4n_lt{0} = sampleTexUnit{0}();\n", unit)); });
        }
        e_.Raw(fmt::format("vec4 mh4n_bump_texel = {};\n", Pick(bump_selector)));
        e_.Raw(fmt::format("vec4 mh4n_shadow_texel = {};\n", Pick(shadow_selector)));
    }

    static std::string Pick(const Value& selector) {
        return fmt::format("(({0}) == 0u ? mh4n_lt0 : (({0}) == 1u ? mh4n_lt1 : (({0}) == 2u ? mh4n_lt2 : mh4n_lt3)))", selector.glsl);
    }

    // Azahar's `sampleTexUnit{selector}()` (specialised) or the picked sample (ubershader).
    std::string BumpTexel() const {
        const Value selector = Field(7, 2);
        return e_.specialised() ? fmt::format("sampleTexUnit{}()", selector.literal) : std::string("mh4n_bump_texel");
    }
    std::string ShadowTexel() const {
        const Value selector = Field(22, 2);
        return e_.specialised() ? fmt::format("sampleTexUnit{}()", selector.literal) : std::string("mh4n_shadow_texel");
    }

    // ---- the fragment's normal and tangent ---------------------------------------------------------------------------------

    void Surface() {
        e_.Raw("vec3 surface_normal = vec3(0.0, 0.0, 1.0);\n"
               "vec3 surface_tangent = vec3(1.0, 0.0, 0.0);\n");
        e_.Switch(Field(5, 2),
                  {{kBumpNormalMap,
                    [&] {
                        // Bump mapping is enabled using a normal map.
                        e_.Raw(fmt::format("surface_normal = 2.0 * ({}).rgb - 1.0;\n", BumpTexel()));
                        // Recompute the Z component of the perturbation if 'renorm' is enabled: a higher precision result.
                        e_.If(Flag(9), [&] {
                            e_.Raw("surface_normal.z = sqrt(max((1.0 - (surface_normal.x*surface_normal.x + "
                                   "surface_normal.y*surface_normal.y)), 0.0));\n");
                        });
                    }},
                   {kBumpTangentMap,
                    [&] {
                        // Bump mapping is enabled using a tangent map (the normal stays the unit vector).
                        e_.Raw(fmt::format("surface_tangent = 2.0 * ({}).rgb - 1.0;\n", BumpTexel()));
                    }}});
    }

    // Rotate the surface-local normal by the interpolated normal quaternion to convert it to eyespace.
    void Quaternion() {
        e_.Raw("vec4 normalized_normquat = normalize(normquat);\n"
               "vec3 normal = quaternion_rotate(normalized_normquat, surface_normal);\n"
               "vec3 tangent = quaternion_rotate(normalized_normquat, surface_tangent);\n");
    }

    void Shadow() {
        e_.Raw("vec4 shadow = vec4(1.0);\n");
        e_.If(Flag(17), [&] {
            const std::string texel = ShadowTexel();
            e_.If(
                Flag(20), [&] { e_.Raw(fmt::format("shadow = vec4(1.0) - {};\n", texel)); },
                [&] { e_.Raw(fmt::format("shadow = {};\n", texel)); });
        });
    }

    // ---- the lookup tables -------------------------------------------------------------------------------------------------

    // IsLightingSamplerSupported(config, sampler).
    Cond Supported(u32 sampler) const {
        const Value config = Field(11, 4);
        auto ne = [&](u32 n) { return e_.Ne(config, n); };
        auto eq = [&](u32 n) { return e_.Eq(config, n); };
        switch (sampler) {
        case kSamplerD0: return ne(kConfig1);
        case kSamplerD1: return e_.And(ne(kConfig0), e_.And(ne(kConfig1), ne(kConfig5)));
        case kSamplerSpot: return e_.And(ne(kConfig2), ne(kConfig3));
        case kSamplerFresnel: return e_.And(ne(kConfig0), e_.And(ne(kConfig2), ne(kConfig4)));
        case kSamplerRr: return ne(kConfig3);
        default: return e_.Or(eq(kConfig4), e_.Or(eq(kConfig5), eq(kConfig7)));  // reflect green and blue
        }
    }

    // The LUT's input as an index expression (Azahar's switch over `input` in get_lut_value).
    std::string InputExpr(const Value& type) const {
        if (!e_.specialised()) {
            return fmt::format("mh4n_lut_input({}, mh4n_in_nh, mh4n_in_vh, mh4n_in_nv, mh4n_in_ln, mh4n_in_sp, mh4n_in_cp)", type.glsl);
        }
        switch (type.literal) {
        case kInputNH: return "dot(normal, normalize(half_vector))";
        case kInputVH: return "dot(normalize(view), normalize(half_vector))";
        case kInputNV: return "dot(normal, normalize(view))";
        case kInputLN: return "dot(light_vector, normal)";
        case kInputSP: return "dot(light_vector, spot_dir)";
        case kInputCP:
            // CP input is only available with configuration 7.
            if (Field(11, 4).literal == kConfig7) {
                // The half angle vector projection is not normalised before the dot product, and it is projected with the
                // (bump-modified) normal.
                return "dot(normalize(half_vector) - normal * dot(normal, normalize(half_vector)), tangent)";
            }
            return "0.0";
        default: return "0.0";
        }
    }

    // `scale * LookupLightingLUT...(sampler, input)` for the table whose LutConfig starts at `word`.
    std::string Lookup(size_t word, const std::string& sampler, const LightFields& light) const {
        const Value type = e_.Field(word, 2, 3);
        const Cond abs_input = e_.Eq(e_.Field(word, 1, 1), 1);
        const std::string input = InputExpr(type);
        const Value scale_bits = e_.Word(word + 1);
        const std::string scale = scale_bits.known ? FloatLiteral(std::bit_cast<float>(scale_bits.literal))
                                                   : fmt::format("uintBitsToFloat({})", scale_bits.glsl);
        std::string call;
        if (abs_input.known && light.two_sided.known) {
            if (abs_input.value) {
                // LUT index is in the range of (0.0, 1.0).
                call = fmt::format("LookupLightingLUTUnsigned({}, {})", sampler,
                                   light.two_sided.value ? fmt::format("abs({})", input) : fmt::format("max({}, 0.0)", input));
            } else {
                // LUT index is in the range of (-1.0, 1.0).
                call = fmt::format("LookupLightingLUTSigned({}, {})", sampler, input);
            }
        } else {
            call = fmt::format("mh4n_lut({}, {}, {}, {})", sampler, input, abs_input.glsl, light.two_sided.glsl);
        }
        return fmt::format("({} * {})", scale, call);
    }

    // ---- one light ---------------------------------------------------------------------------------------------------------

    LightFields LightOf(const std::string& index) {
        LightFields light;
        light.index = index;
        Value raw;
        if (e_.specialised()) {
            const u32 i = static_cast<u32>(std::stoul(index));
            light.index_value = Value::Known(i);
            raw = Value::Known(fragment::Bits(e_.KnownWord(kLightsWord + i / 2), (i & 1) * 16, 16));
        } else {
            light.index_value = Value{false, 0, index};
            const std::string word = fmt::format("({}u + ({} >> 1u))", kLightsWord, index);
            e_.Raw(fmt::format("uint mh4n_lb = (cfg[{0} >> 2u][{0} & 3u] >> (({1} & 1u) * 16u)) & 0xFFFFu;\n", word, index));
            raw = Value{false, 0, "mh4n_lb"};
        }
        auto flag = [&](u32 bit) { return e_.Eq(e_.Slice(raw, bit, 1), 1); };
        light.num = e_.Slice(raw, 0, 3);
        light.directional = flag(3);
        light.two_sided = flag(4);
        light.dist_atten = flag(5);
        light.spot_atten = flag(6);
        light.geo0 = flag(7);
        light.geo1 = flag(8);
        light.shadow = flag(9);
        return light;
    }

    void OneLight(const std::string& index, const Value& src_num) {
        const LightFields light = LightOf(index);
        const std::string num = e_.specialised() ? fmt::format("{}", light.num.literal) : light.num.glsl;
        const std::string light_src = fmt::format("light_src[{}]", num);
        // The sampler number of a per-light table: `base + num`.
        auto sampler_of = [&](u32 base) {
            return e_.specialised() ? fmt::format("{}", base + light.num.literal) : fmt::format("int({}u + {})", base, light.num.glsl);
        };

        // Compute the light vector (directional or positional).
        e_.If(
            light.directional, [&] { e_.Raw(fmt::format("light_vector = {}.position;\n", light_src)); },
            [&] { e_.Raw(fmt::format("light_vector = {}.position + view;\n", light_src)); });
        e_.Raw("light_distance = length(light_vector);\n"
               "light_vector = normalize(light_vector);\n");
        e_.Raw(fmt::format("spot_dir = {}.spot_direction;\n", light_src));
        e_.Raw("half_vector = normalize(view) + light_vector;\n");

        // Compute the dot product of light_vector and normal, adjusted if the lighting is one-sided or two-sided.
        e_.If(
            light.two_sided, [&] { e_.Raw("dot_product = abs(dot(light_vector, normal));\n"); },
            [&] { e_.Raw("dot_product = max(dot(light_vector, normal), 0.0);\n"); });

        // If enabled, clamp the specular component if the lighting result is zero.
        e_.If(Flag(10), [&] { e_.Raw("clamp_highlights = sign(dot_product);\n"); });

        if (!e_.specialised()) {
            // The inputs a LUT can name, once per light (the ubershader picks by the table's type).
            e_.Raw("float mh4n_in_nh = dot(normal, normalize(half_vector));\n"
                   "float mh4n_in_vh = dot(normalize(view), normalize(half_vector));\n"
                   "float mh4n_in_nv = dot(normal, normalize(view));\n"
                   "float mh4n_in_ln = dot(light_vector, normal);\n"
                   "float mh4n_in_sp = dot(light_vector, spot_dir);\n");
            e_.Raw(fmt::format("float mh4n_in_cp = ({} == {}u) ? dot(normalize(half_vector) - normal * dot(normal, normalize(half_vector)), tangent) : 0.0;\n",
                               Field(11, 4).glsl, kConfig7));
        }

        // If enabled, compute the spot light attenuation value.
        e_.Raw("float spot_atten = 1.0;\n");
        e_.If(e_.And(light.spot_atten, Supported(kSamplerSpot)), [&] {
            e_.Raw(fmt::format("spot_atten = {};\n", Lookup(kLutSp, sampler_of(kSamplerSpot), light)));
        });

        // If enabled, compute the distance attenuation value.
        e_.Raw("float dist_atten = 1.0;\n");
        e_.If(light.dist_atten, [&] {
            const std::string pos = fmt::format("clamp({0}.dist_atten_scale * light_distance + {0}.dist_atten_bias, 0.0, 1.0)", light_src);
            const std::string sampler = sampler_of(kSamplerDist);
            e_.Raw(fmt::format("dist_atten = {};\n", e_.specialised() ? fmt::format("LookupLightingLUTUnsigned({}, {})", sampler, pos)
                                                                      : fmt::format("mh4n_lut({}, {}, true, false)", sampler, pos)));
        });

        e_.If(e_.Or(light.geo0, light.geo1), [&] {
            e_.Raw("geo_factor = dot(half_vector, half_vector);\n"
                   "geo_factor = geo_factor == 0.0 ? 0.0 : min(dot_product / geo_factor, 1.0);\n");
        });

        // Specular 0 component.
        e_.Raw("float d0_lut_value = 1.0;\n");
        e_.If(e_.And(e_.Eq(e_.Field(kLutD0, 0, 1), 1), Supported(kSamplerD0)), [&] {
            e_.Raw(fmt::format("d0_lut_value = {};\n", Lookup(kLutD0, std::to_string(kSamplerD0), light)));
        });
        e_.Raw(fmt::format("vec3 specular_0 = d0_lut_value * {}.specular_0;\n", light_src));
        e_.If(light.geo0, [&] { e_.Raw("specular_0 *= geo_factor;\n"); });

        // If enabled, lookup ReflectRed, otherwise 1.0 is used; ReflectGreen and ReflectBlue fall back to ReflectRed.
        e_.If(
            e_.And(e_.Eq(e_.Field(kLutRr, 0, 1), 1), Supported(kSamplerRr)),
            [&] { e_.Raw(fmt::format("refl_value.r = {};\n", Lookup(kLutRr, std::to_string(kSamplerRr), light))); },
            [&] { e_.Raw("refl_value.r = 1.0;\n"); });
        e_.If(
            e_.And(e_.Eq(e_.Field(kLutRg, 0, 1), 1), Supported(kSamplerRg)),
            [&] { e_.Raw(fmt::format("refl_value.g = {};\n", Lookup(kLutRg, std::to_string(kSamplerRg), light))); },
            [&] { e_.Raw("refl_value.g = refl_value.r;\n"); });
        e_.If(
            e_.And(e_.Eq(e_.Field(kLutRb, 0, 1), 1), Supported(kSamplerRb)),
            [&] { e_.Raw(fmt::format("refl_value.b = {};\n", Lookup(kLutRb, std::to_string(kSamplerRb), light))); },
            [&] { e_.Raw("refl_value.b = refl_value.r;\n"); });

        // Specular 1 component.
        e_.Raw("float d1_lut_value = 1.0;\n");
        e_.If(e_.And(e_.Eq(e_.Field(kLutD1, 0, 1), 1), Supported(kSamplerD1)), [&] {
            e_.Raw(fmt::format("d1_lut_value = {};\n", Lookup(kLutD1, std::to_string(kSamplerD1), light)));
        });
        e_.Raw(fmt::format("vec3 specular_1 = d1_lut_value * refl_value * {}.specular_1;\n", light_src));
        e_.If(light.geo1, [&] { e_.Raw("specular_1 *= geo_factor;\n"); });

        // Fresnel. Note: only the last entry in the light slots applies the Fresnel factor.
        const Value last = e_.specialised() ? Value::Known(src_num.literal - 1) : Value{false, 0, fmt::format("({} - 1u)", src_num.glsl)};
        const Cond is_last = e_.Eq(light.index_value, last);
        e_.If(e_.And(is_last, e_.And(e_.Eq(e_.Field(kLutFr, 0, 1), 1), Supported(kSamplerFresnel))), [&] {
            e_.Raw(fmt::format("float fresnel_value = {};\n", Lookup(kLutFr, std::to_string(kSamplerFresnel), light)));
            // Enabled for the diffuse lighting alpha component, and for the specular lighting alpha component.
            e_.If(Flag(15), [&] { e_.Raw("diffuse_sum.a = fresnel_value;\n"); });
            e_.If(Flag(16), [&] { e_.Raw("specular_sum.a = fresnel_value;\n"); });
        });

        const Cond shadow_primary = e_.And(Flag(18), light.shadow);
        const Cond shadow_secondary = e_.And(Flag(19), light.shadow);

        // Compute the primary fragment color (diffuse lighting) function.
        e_.If(
            shadow_primary,
            [&] {
                e_.Raw(fmt::format("diffuse_sum.rgb += (({0}.diffuse * dot_product * shadow.rgb) + {0}.ambient) * dist_atten * spot_atten;\n", light_src));
            },
            [&] { e_.Raw(fmt::format("diffuse_sum.rgb += (({0}.diffuse * dot_product) + {0}.ambient) * dist_atten * spot_atten;\n", light_src)); });

        // Compute the secondary fragment color (specular lighting) function.
        e_.If(
            shadow_secondary,
            [&] { e_.Raw("specular_sum.rgb += (specular_0 + specular_1) * clamp_highlights * dist_atten * spot_atten * shadow.rgb;\n"); },
            [&] { e_.Raw("specular_sum.rgb += (specular_0 + specular_1) * clamp_highlights * dist_atten * spot_atten;\n"); });
    }

    void Finish() {
        // Apply the shadow attenuation to the alpha components if enabled.
        e_.If(Flag(21), [&] {
            e_.If(Flag(15), [&] { e_.Raw("diffuse_sum.a *= shadow.a;\n"); });
            e_.If(Flag(16), [&] { e_.Raw("specular_sum.a *= shadow.a;\n"); });
        });
        // Sum the final lighting result.
        e_.Raw("diffuse_sum.rgb += lighting_global_ambient;\n"
               "primary_fragment_color = clamp(diffuse_sum, vec4(0.0), vec4(1.0));\n"
               "secondary_fragment_color = clamp(specular_sum, vec4(0.0), vec4(1.0));\n");
    }
};

// ---- the register decoders -----------------------------------------------------------------------------------------------

// Pica::Float<M, E>::FromRaw(hex).ToFloat32() (pica_types.h): mantissa M bits, exponent E bits, then the sign; a denormal is
// renormalised exactly, zero keeps its sign, an infinity or a NaN keeps its mantissa.
template <u32 M, u32 E>
float PicaFloat(u32 hex) {
    constexpr s32 bias = 127 - ((1 << (E - 1)) - 1);
    constexpr u32 mantissa_mask = (1u << M) - 1;
    constexpr u32 exponent_mask = (1u << E) - 1;
    s32 exponent = static_cast<s32>((hex >> M) & exponent_mask);
    u32 mantissa = hex & mantissa_mask;
    const u32 sign = ((hex >> (E + M)) & 1u) << 31;
    u32 bits;
    if (exponent == 0 && mantissa == 0) {
        bits = sign;
    } else if (exponent == 0) {
        exponent = bias + 1;
        while ((mantissa & (1u << M)) == 0) {
            --exponent;
            mantissa <<= 1;
        }
        mantissa &= mantissa_mask;
        bits = sign | (static_cast<u32>(exponent) << 23) | (mantissa << (23 - M));
    } else if (static_cast<u32>(exponent) == exponent_mask) {
        bits = sign | (0xFFu << 23) | (mantissa << (23 - M));
    } else {
        bits = sign | (static_cast<u32>(bias + exponent) << 23) | (mantissa << (23 - M));
    }
    return std::bit_cast<float>(bits);
}

// LightColor: b 0-9, g 10-19, r 20-29; "255 corresponds to 1.0f for each color component".
Vec3f LightColor(u32 word) {
    return Vec3f{static_cast<float>(Bits(word, 20, 10)) / 255.0f, static_cast<float>(Bits(word, 10, 10)) / 255.0f,
                 static_cast<float>(Bits(word, 0, 10)) / 255.0f};
}

// A signed field of `count` bits.
float Signed(u32 word, u32 first, u32 count) {
    const u32 shift = 32 - count;
    return static_cast<float>(static_cast<s32>(Bits(word, first, count) << shift) >> shift);
}

LightSrc LightSrcOf(const u32* regs, u32 light) {
    const u32* r = regs + freg::kLight0 + light * freg::kLightStride;
    LightSrc src{};
    src.specular_0 = LightColor(r[freg::kLightSpecular0]);
    src.specular_1 = LightColor(r[freg::kLightSpecular1]);
    src.diffuse = LightColor(r[freg::kLightDiffuse]);
    src.ambient = LightColor(r[freg::kLightAmbient]);
    src.position = Vec3f{PicaFloat<10, 5>(Bits(r[freg::kLightPositionXY], 0, 16)), PicaFloat<10, 5>(Bits(r[freg::kLightPositionXY], 16, 16)),
                         PicaFloat<10, 5>(Bits(r[freg::kLightPositionZ], 0, 16))};
    src.spot_direction = Vec3f{Signed(r[freg::kLightSpotXY], 0, 13) / 2047.0f, Signed(r[freg::kLightSpotXY], 16, 13) / 2047.0f,
                               Signed(r[freg::kLightSpotZ], 0, 13) / 2047.0f};
    src.dist_atten_bias = PicaFloat<12, 7>(Bits(r[freg::kLightDistAttenBias], 0, 20));
    src.dist_atten_scale = PicaFloat<12, 7>(Bits(r[freg::kLightDistAttenScale], 0, 20));
    return src;
}

// ---- the feature -----------------------------------------------------------------------------------------------------------

class LightingFeature final : public FeatureModule {
public:
    const char* Name() const override { return "lighting"; }

    void Emit(Point point, EmitContext& ctx) const override {
        Emitter& e = *ctx.emitter;
        if (point == Point::Declarations) {
            // In the specialised mode the helpers exist only for a key that lights.
            if (!e.specialised()) {
                e.Raw(kUberHelpers);
            } else if (ctx.key.lighting.enable() != 0) {
                e.Raw(kSpecialisedHelpers);
            }
        } else if (point == Point::Lighting) {
            LightingCode(e).Emit();
        }
    }

    // ---- uniforms: Azahar's SyncDrawUniforms, lighting part --------------------------------------------------------------

    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        FragmentUniforms& uniforms = block.uniforms;
        if (dirty.all || state.dirty.Test(freg::kLightingGlobalAmbient)) {
            const Vec3f ambient = LightColor(state.regs[freg::kLightingGlobalAmbient]);
            if (dirty.all || std::memcmp(&ambient, &uniforms.lighting_global_ambient, sizeof(ambient)) != 0) {
                uniforms.lighting_global_ambient = ambient;
                dirty.Mark();
            }
        }
        for (u32 light = 0; light < 8; ++light) {
            const u32 base = freg::kLight0 + light * freg::kLightStride;
            bool changed = dirty.all;
            for (u32 word = 0; word <= freg::kLightDistAttenScale && !changed; ++word) {
                changed = state.dirty.Test(base + word);
            }
            if (!changed) {
                continue;
            }
            const LightSrc src = LightSrcOf(state.regs.data(), light);
            if (dirty.all || std::memcmp(&src, &uniforms.light_src[light], sizeof(src)) != 0) {
                std::memcpy(&uniforms.light_src[light], &src, sizeof(src));
                dirty.Mark();
            }
        }
    }

    // ---- the tables in lut_lf: Azahar's SyncAndUploadLUTsLF, lighting part -------------------------------------------------

    // Uploads exactly the tables `upload.need` names (whatever the dirty flags of PicaState say), each as 256 {value, difference}
    // pairs, and writes the offset of each from its slice (in elements of the R32G32 view). The session was opened for the worst
    // case of the whole ring, so `Take` cannot run out of room; if it did, the offsets of the tables not taken stay as they were.
    void UploadTables(TableUpload& upload) const override {
        u32 need = upload.need.lighting & pica::Lighting::kAllDirty;
        while (need != 0) {
            const u32 index = static_cast<u32>(std::countr_zero(need));
            need &= need - 1;
            rhi::Result<TableSlice> slice = upload.rings.Take(TableRing::Lf, pica::Lighting::kEntries * sizeof(Vec2f));
            if (!slice.ok()) {
                return;
            }
            const auto& source = upload.state.lighting.luts[index];
            for (u32 i = 0; i < pica::Lighting::kEntries; ++i) {
                // Lighting::LutEntry: value is bits 0-11 over 4095; difference is bits 12-22 over 2047, negative when bit 23 is set.
                const u32 raw = source[i];
                const float diff = static_cast<float>(Bits(raw, 12, 11)) / 2047.0f;
                const Vec2f entry{static_cast<float>(Bits(raw, 0, 12)) / 4095.0f, Bits(raw, 23, 1) != 0 ? -diff : diff};
                std::memcpy(slice->data.data() + i * sizeof(Vec2f), &entry, sizeof(entry));
            }
            (&upload.block.uniforms.lighting_lut_offset[index / 4].x)[index % 4] = slice->ElementOffset(sizeof(Vec2f));
            upload.dirty.Mark();
        }
    }

    // ---- the draw's needs ----------------------------------------------------------------------------------------------

    // The lighting tables; and the units the bump and shadow terms sample (the TEV sources' are sampling/'s). The quaternion fix-up
    // (`fixup_gs`) is core's.
    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        Needs needs;
        const LightConfig& light = key.lighting;
        if (light.enable() == 0) {
            return needs;
        }
        needs.lut_lf = true;
        auto unit = [&](u32 selector) {
            if (selector < 3) {
                needs.texture_units = std::max<u8>(needs.texture_units, static_cast<u8>(selector + 1));
            }
        };
        if (light.bump_mode() != regenum::LightingBumpMode::None) {
            unit(light.bump_selector());
        }
        if (light.enable_shadow() != 0) {
            unit(light.shadow_selector());
        }
        return needs;
    }

    const char* Stubbed(const FragmentKey&) const override { return nullptr; }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(LightingFeature)

// ---- the fixed geometry shader (Azahar's GenerateFixedGeometryShader) -------------------------------------------------------

namespace lighting {
namespace {

// glsl_shader_gen.cpp's VSUniformBlockDef as N2-06's vertex stage declares it (vertex_shader.cpp: the booleans are uints).
constexpr const char* kVertexData = R"(layout (set = 0, binding = 1, std140) uniform vs_data {
    uint enable_clip1;
    uint flip_viewport;
    vec4 clip_coef;
};

const vec2 EPSILON_Z = vec2(0.000001f, -1.00001f);

vec4 SanitizeVertex(vec4 vtx_pos) {
    float ndc_z = vtx_pos.z / vtx_pos.w;
    if (ndc_z > 0.f && ndc_z < EPSILON_Z[0]) {
        vtx_pos.z = 0.f;
    }
    if (ndc_z < -1.f && ndc_z > EPSILON_Z[1]) {
        vtx_pos.z = -vtx_pos.w;
    }
    return vtx_pos;
}
)";

// RasterizerRegs::VSOutputAttributes::Semantic.
constexpr u32 kPositionX = 0, kPositionY = 1, kPositionZ = 2, kPositionW = 3;
constexpr u32 kQuaternionX = 4, kQuaternionY = 5, kQuaternionZ = 6, kQuaternionW = 7;
constexpr u32 kColorR = 8, kColorG = 9, kColorB = 10, kColorA = 11;
constexpr u32 kTexcoord0U = 12, kTexcoord0V = 13, kTexcoord1U = 14, kTexcoord1V = 15, kTexcoord0W = 16;
constexpr u32 kViewX = 18, kViewY = 19, kViewZ = 20, kTexcoord2U = 22, kTexcoord2V = 23;

void InitializeGlslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

}  // namespace

std::string GenerateFixedGeometryGlsl(const native::VertexStageOutputs& outputs) {
    const u32 count = std::min<u32>(outputs.num_outputs, 16);
    std::string out;
    out += "#version 450\n"
           "layout(triangles) in;\n"
           "layout(triangle_strip, max_vertices = 3) out;\n\n";
    // The vertex interface (GetVertexInterfaceDeclaration): Azahar's attribute locations.
    out += "layout (location = 1) out vec4 primary_color;\n"
           "layout (location = 2) out vec2 texcoord0;\n"
           "layout (location = 3) out vec2 texcoord1;\n"
           "layout (location = 4) out vec2 texcoord2;\n"
           "layout (location = 5) out float texcoord0_w;\n"
           "layout (location = 6) out vec4 normquat;\n"
           "layout (location = 7) out vec3 view;\n\n";
    out += kVertexData;
    out += '\n';
    for (u32 i = 0; i < count; ++i) {
        out += fmt::format("layout (location = {0}) in vec4 vs_out_attr{0}[];\n", i);
    }
    out += "\nstruct Vertex {\n";
    out += fmt::format("    vec4 attributes[{}];\n", std::max<u32>(count, 1));
    out += "};\n\n";

    const auto semantic = [&](u32 slot) -> std::string {
        if (slot < outputs.semantic_map.size()) {
            const native::VertexStageOutputs::Semantic& s = outputs.semantic_map[slot];
            if (s.attribute < count) {
                return fmt::format("vtx.attributes[{}].{}", s.attribute, "xyzw"[s.component & 3]);
            }
        }
        return "1.0";
    };
    const auto list = [&](std::initializer_list<u32> slots) {
        std::string text;
        for (u32 slot : slots) {
            text += (text.empty() ? "" : ", ") + semantic(slot);
        }
        return text;
    };

    out += "vec4 GetVertexQuaternion(Vertex vtx) {\n";
    out += "    return vec4(" + list({kQuaternionX, kQuaternionY, kQuaternionZ, kQuaternionW}) + ");\n";
    out += "}\n\n";

    out += "void EmitVtx(Vertex vtx, bool quats_opposite) {\n";
    out += "    vec4 vtx_pos = vec4(" + list({kPositionX, kPositionY, kPositionZ, kPositionW}) + ");\n";
    out += "    vtx_pos = SanitizeVertex(vtx_pos);\n";
    out += "    if (flip_viewport != 0u) {\n";
    out += "        vtx_pos.y = -vtx_pos.y;\n";
    out += "    }\n";
    out += "    gl_Position = vec4(vtx_pos.x, vtx_pos.y, -vtx_pos.z, vtx_pos.w);\n";
    out += "    vec2 out_clip = vec2(0.0);\n";
    out += "    out_clip.x = -vtx_pos.z;\n";  // fixed PICA clipping plane z <= 0
    out += "    if (enable_clip1 != 0u) {\n";
    out += "        out_clip.y = dot(clip_coef, vtx_pos);\n";
    out += "    }\n";
    out += "    gl_ClipDistance[0] = out_clip.x;\n";
    out += "    gl_ClipDistance[1] = out_clip.y;\n\n";

    out += "    vec4 vtx_quat = GetVertexQuaternion(vtx);\n";
    out += "    normquat = mix(vtx_quat, -vtx_quat, bvec4(quats_opposite));\n\n";

    // As N2-06's vertex stage: the absolute value, saturated (a NaN gives 1).
    out += "    vec4 vtx_color = abs(vec4(" + list({kColorR, kColorG, kColorB, kColorA}) + "));\n";
    out += "    primary_color = mix(vec4(1.0), vtx_color, lessThan(vtx_color, vec4(1.0)));\n\n";

    out += "    texcoord0 = vec2(" + list({kTexcoord0U, kTexcoord0V}) + ");\n";
    out += "    texcoord1 = vec2(" + list({kTexcoord1U, kTexcoord1V}) + ");\n";
    out += "    texcoord0_w = " + semantic(kTexcoord0W) + ";\n";
    out += "    view = vec3(" + list({kViewX, kViewY, kViewZ}) + ");\n";
    out += "    texcoord2 = vec2(" + list({kTexcoord2U, kTexcoord2V}) + ");\n\n";

    out += "    EmitVertex();\n";
    out += "}\n";

    out += R"(
bool AreQuaternionsOpposite(vec4 qa, vec4 qb) {
    return (dot(qa, qb) < 0.0);
}

void EmitPrim(Vertex vtx0, Vertex vtx1, Vertex vtx2) {
    EmitVtx(vtx0, false);
    EmitVtx(vtx1, AreQuaternionsOpposite(GetVertexQuaternion(vtx0), GetVertexQuaternion(vtx1)));
    EmitVtx(vtx2, AreQuaternionsOpposite(GetVertexQuaternion(vtx0), GetVertexQuaternion(vtx2)));
    EndPrimitive();
}

void main() {
    Vertex prim_buffer[3];
)";
    for (u32 vtx = 0; vtx < 3; ++vtx) {
        out += fmt::format("    prim_buffer[{}].attributes = vec4[{}](", vtx, std::max<u32>(count, 1));
        if (count == 0) {
            out += "vec4(0.0)";
        }
        for (u32 i = 0; i < count; ++i) {
            out += fmt::format("{}vs_out_attr{}[{}]", i == 0 ? "" : ", ", i, vtx);
        }
        out += ");\n";
    }
    out += "    EmitPrim(prim_buffer[0], prim_buffer[1], prim_buffer[2]);\n";
    out += "}\n";
    return out;
}

bool CompileGeometryGlslToSpirv(const std::string& source, std::vector<u32>& spirv, std::string& error) {
    InitializeGlslang();
    spirv.clear();
    error.clear();

    glslang::TShader shader(EShLangGeometry);
    const char* strings[1] = {source.c_str()};
    shader.setStrings(strings, 1);
    // Vulkan 1.2 and SPIR-V 1.5, as the other stages.
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangGeometry, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        error = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        error = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }
    const glslang::TIntermediate* intermediate = program.getIntermediate(EShLangGeometry);
    if (intermediate == nullptr) {
        error = "glslang produced no geometry stage";
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true;  // glslang is built without spirv-tools
    options.validate = false;
    std::vector<unsigned int> words;
    glslang::GlslangToSpv(*intermediate, words, &options);
    spirv.assign(words.begin(), words.end());
    return !spirv.empty();
}

std::vector<u32> GenerateFixedGeometryShader(const native::VertexStageOutputs& outputs) {
    std::vector<u32> spirv;
    std::string error;
    if (!CompileGeometryGlslToSpirv(GenerateFixedGeometryGlsl(outputs), spirv, error)) {
        return {};
    }
    return spirv;
}

}  // namespace lighting

namespace {

// Registers the generator with N2-33's slot at start-up (the registry is a function-local static: no order problem).
[[maybe_unused]] const bool kFixupGeneratorRegistered = [] {
    native::FixupGeneratorRegistry::Global().Set(lighting::GenerateFixedGeometryShader);
    return true;
}();

}  // namespace

}  // namespace mh4n::gpu::fragment
