// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (FragmentModule::DefineProcTexSampler,
// AppendProcTexShiftOffset, AppendProcTexClamp, AppendProcTexCombineAndMap: the text of ProcTexLookupLUT, the noise functions,
// SampleProcTexColor and ProcTex), src/video_core/renderer_vulkan/vk_rasterizer.cpp (SyncAndUploadLUTs: the five procedural
// tables, their order, their sizes and the offsets kept in the uniforms), src/video_core/pica/pica_core.h (ProcTex::ValueEntry,
// ColorEntry and ColorDifferenceEntry: the bit fields and the divisions by 4095 and 255), src/video_core/rasterizer_accelerated.cpp
// and src/video_core/pica/dirty_regs.h (SyncDrawUniforms and CheckProctex: the noise, phase, amplitude and bias uniforms and the
// six registers that dirty them), src/video_core/pica/regs_texturing.h (the procedural texture registers), and
// src/video_core/pica_types.h (Float::FromRaw: how a half is read) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// glsl_fs_shader_gen.cpp and vk_rasterizer.cpp say "Licensed under GPLv3 or any later version"; pica_core.h,
// rasterizer_accelerated.cpp, dirty_regs.h, regs_texturing.h and pica_types.h each say "Licensed under GPLv2 or any later
// version"). src/video_core/renderer_software/sw_proctex.cpp (GPLv2 or later, "Copyright 2017 Citra Emulator Project") was read
// as the reference semantics; nothing of it is copied here (the test file ports it).
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp, vk_rasterizer.cpp, pica_core.h,
//   rasterizer_accelerated.cpp)
// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project (dirty_regs.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
// Copyright 2017 Citra Emulator Project (sw_proctex.cpp)
//
// The `proctex/` feature (N2-11): the procedural texture unit (noise, the colour-map and alpha-map lookups, the shift and clamp
// modes, the colour table and its LOD levels), reached through `sampleTexUnit3()` of sampling/ (the procedural slot, Texture3).
//
// ONE SOURCE, TWO MODES. `EmitProcTex` writes the definitions once with the emitter's helpers (emitter.h): in the specialised mode
// the helpers keep only the chosen branch and the numbers are literals (Azahar's text for the configuration of the key); in the
// ubershader they read `cfg`. Where Azahar's text is written for one configuration, the ubershader's code does ONE call of each
// heavy helper after a switch that computes only the value (the drivers compile every call site as its own copy):
//     ProcTexNoiseCoef    one call (inside `if (noise_enable)`), with Azahar's four ProcTexNoiseRand2D and two noise-table lookups
//     ProcTexLookupLUT    the colour-map lookup and the alpha-map lookup of ProcTex (one call each), and the noise's two
//     SampleProcTexColor  at most two call sites (the second only for the two linear-mipmap filters), whatever the filter
// The combiner, the clamp and the level choices are cheap value computations (a switch, or the helpers ProcTexCombine and
// ProcTexClampCoord built from the same tables as Azahar's inline text). Every branch reads only `cfg` (uniform within a draw),
// so the derivatives of `uv` stay defined.
//
// WHERE THIS DIFFERS FROM AZAHAR'S TEXT, on purpose and without changing a value:
//   - the temporaries of the combiner, clamp and level choices (`lod_i`, `u = <clamp expression>;`), so one body serves both modes;
//   - the second mip level is min(lod_i + 1, 7): Azahar's lut_offsets[8] is read out of range for a level of exactly 7 (its weight
//     fract(lod) is 0 there, so the value is the same);
//   - ProcTex() is defined (as vec4(0.0)) in a specialised shader whose key has the procedural texture off: sampleTexUnit3 does not
//     call it then, and the prototype block declares it.
// WHAT IS NOT IN THE SOFTWARE RENDERER: its ProcTex implements no mipmap ("TODO(wwylele): implement mipmap"), so the LOD and the four
// mipmap filters follow Azahar's GLSL alone. The GLSL's ProcTexLookupLUT clamps its result to [0, 1] where the software
// renderer's LookupLUT does not: this shader keeps the GLSL (gpu_proctex_test keeps its tables inside [0, 1], where the two agree).
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>

#include "gpu/backend/rhi.h"
#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"
#include "gpu/fragment/regs.h"
#include "gpu/fragment/tables.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

namespace kw = keyword;

// The words of ProcTexConfig after `raw` (key.h: raw, lut_width, lut_offset0..3, lod_min and lod_max in one word).
constexpr size_t kWordWidth = kw::kProcTexRaw + offsetof(ProcTexConfig, lut_width) / sizeof(u32);
constexpr size_t kWordOffset0 = kw::kProcTexRaw + offsetof(ProcTexConfig, lut_offset0) / sizeof(u32);
constexpr size_t kWordLod = kw::kProcTexRaw + offsetof(ProcTexConfig, lod_min) / sizeof(u32);
static_assert(offsetof(ProcTexConfig, lut_offset3) == offsetof(ProcTexConfig, lut_offset0) + 12);
static_assert(offsetof(ProcTexConfig, lod_max) == offsetof(ProcTexConfig, lod_min) + 2);

// ProcTexClamp, ProcTexFilter and the table of ProcTexCombiner (the enumerators of regs.h).
constexpr u32 kClampMirroredRepeat = 3;

struct Expr {
    u32 value;
    const char* text;  // {0}: the variable (the clamp), `u` and `v` (the combiner)
};

// AppendProcTexClamp's right-hand sides. The default (an unknown mode) is ToEdge's.
constexpr Expr kClamps[] = {
    {0, "{0} > 1.0 ? 0.0 : {0}"},                                   // ToZero
    {1, "min({0}, 1.0)"},                                           // ToEdge
    {2, "fract({0})"},                                              // SymmetricalRepeat
    {3, "int({0}) % 2 == 0 ? fract({0}) : 1.0 - fract({0})"},       // MirroredRepeat
    {4, "{0} > 0.5 ? 1.0 : 0.0"},                                   // Pulse
};
constexpr const char* kClampDefault = "min({0}, 1.0)";

// AppendProcTexCombineAndMap's combined value. The default (an unknown combiner) is 0.0.
constexpr Expr kCombiners[] = {
    {0, "u"},                                                       // U
    {1, "(u * u)"},                                                 // U2
    {2, "v"},                                                       // V
    {3, "(v * v)"},                                                 // V2
    {4, "((u + v) * 0.5)"},                                         // Add
    {5, "((u * u + v * v) * 0.5)"},                                 // Add2
    {6, "min(sqrt(u * u + v * v), 1.0)"},                           // SqrtAdd2
    {7, "min(u, v)"},                                               // Min
    {8, "max(u, v)"},                                               // Max
    {9, "min(((u + v) * 0.5 + sqrt(u * u + v * v)) * 0.5, 1.0)"},   // RMax
};
constexpr const char* kCombinerDefault = "0.0";

// Azahar's ProcTexLookupLUT: coord = 0.0 is lut[0], 127.0 / 128.0 is lut[127] and 1.0 is lut[127] + diff[127].
constexpr const char* kLookupLut = R"(
float ProcTexLookupLUT(int offset, float coord) {
    coord *= 128.0;
    float index_i = clamp(floor(coord), 0.0, 127.0);
    float index_f = coord - index_i; // fract() cannot be used here because 128.0 needs to be
                                     // extracted as index_i = 127.0 and index_f = 1.0
    vec2 entry = texelFetch(texture_buffer_lut_rg, int(index_i) + offset).rg;
    return clamp(entry.r + entry.g * index_f, 0.0, 1.0);
}
)";

// Azahar's noise functions (see sw_proctex.cpp for what they are): the specialised shader has them only when the key has noise.
constexpr const char* kNoise = R"(
int ProcTexNoiseRand1D(int v) {
    const int table[] = int[](0,4,10,8,4,9,7,12,5,15,13,14,11,15,2,11);
    return ((v % 9 + 2) * 3 & 0xF) ^ table[(v / 9) & 0xF];
}

float ProcTexNoiseRand2D(vec2 point) {
    const int table[] = int[](10,2,15,8,0,7,4,5,5,13,2,6,13,9,3,14);
    int u2 = ProcTexNoiseRand1D(int(point.x));
    int v2 = ProcTexNoiseRand1D(int(point.y));
    v2 += ((u2 & 3) == 1) ? 4 : 0;
    v2 ^= (u2 & 1) * 6;
    v2 += 10 + u2;
    v2 &= 0xF;
    v2 ^= table[u2];
    return -1.0 + float(v2) * (2.0/15.0);
}

float ProcTexNoiseCoef(vec2 x) {
    vec2 grid  = 9.0 * proctex_noise_f * abs(x + proctex_noise_p);
    vec2 point = floor(grid);
    vec2 frac  = grid - point;

    float g0 = ProcTexNoiseRand2D(point) * (frac.x + frac.y);
    float g1 = ProcTexNoiseRand2D(point + vec2(1.0, 0.0)) * (frac.x + frac.y - 1.0);
    float g2 = ProcTexNoiseRand2D(point + vec2(0.0, 1.0)) * (frac.x + frac.y - 1.0);
    float g3 = ProcTexNoiseRand2D(point + vec2(1.0, 1.0)) * (frac.x + frac.y - 2.0);

    float x_noise = ProcTexLookupLUT(proctex_noise_lut_offset, frac.x);
    float y_noise = ProcTexLookupLUT(proctex_noise_lut_offset, frac.y);
    float x0 = mix(g0, g1, x_noise);
    float x1 = mix(g2, g3, x_noise);
    return mix(x0, x1, y_noise);
}
)";

// `{0}` replaced in `pattern` by `var`, `u` and `v` kept.
std::string Substitute(const char* pattern, const char* var) {
    std::string out = pattern;
    for (size_t at = out.find("{0}"); at != std::string::npos; at = out.find("{0}", at + 1)) {
        out.replace(at, 3, var);
    }
    return out;
}

const char* TextOf(const Expr* table, size_t count, const char* fallback, u32 value) {
    for (size_t i = 0; i < count; ++i) {
        if (table[i].value == value) {
            return table[i].text;
        }
    }
    return fallback;
}

// A GLSL int / float expression of a key Value: a literal in the specialised mode.
std::string IntOf(const Value& v) {
    return v.known ? fmt::format("{}", static_cast<s32>(v.literal)) : fmt::format("int({})", v.glsl);
}
std::string FloatOf(const Value& v) {
    return v.known ? fmt::format("{}.0", v.literal) : fmt::format("float({})", v.glsl);
}

// The combined value of `combiner` (u and v in scope): Azahar's inline text in the specialised mode, a call of ProcTexCombine in the
// ubershader (a function of the same table: the switch computes only the value).
std::string CombinedOf(const Emitter& e, const Value& combiner) {
    if (e.specialised()) {
        return TextOf(kCombiners, std::size(kCombiners), kCombinerDefault, combiner.literal);
    }
    return fmt::format("ProcTexCombine({}, u, v)", combiner.glsl);
}

// AppendProcTexShiftOffset: `name = ...;` (name is declared as 0.0 before). `v` is the other coordinate.
void EmitShift(Emitter& e, const char* name, const Value& mode, const Value& clamp, const char* v) {
    const Cond mirrored = e.Eq(clamp, kClampMirroredRepeat);
    const std::string offset = mirrored.known ? (mirrored.value ? "1.0" : "0.5")
                                              : fmt::format("({} ? 1.0 : 0.5)", mirrored.glsl);
    e.Line("float {} = 0.0;", name);
    e.Switch(mode, {{1, [&] { e.Line("{} = {} * float((int({}) / 2) % 2);", name, offset, v); }},
                    {2, [&] { e.Line("{} = {} * float(((int({}) + 1) / 2) % 2);", name, offset, v); }}});
}

// AppendProcTexClamp: `var = <clamp>;`.
void EmitClamp(Emitter& e, const char* var, const Value& mode) {
    if (e.specialised()) {
        e.Line("{} = {};", var, Substitute(TextOf(kClamps, std::size(kClamps), kClampDefault, mode.literal), var));
        return;
    }
    e.Line("{0} = ProcTexClampCoord({1}, {0});", var, mode.glsl);
}

// The two helpers of the ubershader that turn a mode into a value: built from the tables above.
void EmitModeHelpers(Emitter& e) {
    e.Raw("float ProcTexClampCoord(uint mode, float c) {\n    switch (mode) {\n");
    for (const Expr& entry : kClamps) {
        e.Line("    case {}u: return {};", entry.value, Substitute(entry.text, "c"));
    }
    e.Line("    default: return {};", Substitute(kClampDefault, "c"));
    e.Raw("    }\n}\n");
    e.Raw("float ProcTexCombine(uint combiner, float u, float v) {\n    switch (combiner) {\n");
    for (const Expr& entry : kCombiners) {
        e.Line("    case {}u: return {};", entry.value, entry.text);
    }
    e.Line("    default: return {};", kCombinerDefault);
    e.Raw("    }\n}\n");
}

// DefineProcTexSampler.
void EmitProcTex(Emitter& e) {
    const Value width = e.Word(kWordWidth);
    const Value filter = e.Field(kw::kProcTexRaw, 17, 3);
    const Cond linear = e.Or(e.Eq(filter, 1), e.Or(e.Eq(filter, 3), e.Eq(filter, 5)));
    const Cond mip_nearest = e.Or(e.Eq(filter, 2), e.Eq(filter, 3));
    const Cond mip_linear = e.Or(e.Eq(filter, 4), e.Eq(filter, 5));

    e.Raw(kLookupLut);
    if (!e.specialised()) {
        EmitModeHelpers(e);
    }
    // (global scope: no `if` here; the ubershader always has the noise functions)
    if (!e.specialised() || e.Eq(e.Field(kw::kProcTexRaw, 21, 1), 1).value) {
        e.Raw(kNoise);
    }

    // SampleProcTexColor: the colour table at `lut_coord` of the level.
    e.Raw("vec4 SampleProcTexColor(float lut_coord, int level) {\n");
    e.Line("int lut_width = {} >> level;", IntOf(width));
    // Offsets for level 4-7 seem to be hardcoded
    e.Line("int lut_offsets[8] = int[]({}, {}, {}, {}, 0xF0, 0xF8, 0xFC, 0xFE);", IntOf(e.Word(kWordOffset0)),
           IntOf(e.Word(kWordOffset0 + 1)), IntOf(e.Word(kWordOffset0 + 2)), IntOf(e.Word(kWordOffset0 + 3)));
    e.Raw("int lut_offset = lut_offsets[level];\n"
          // For the color lut, coord=0.0 is lut[offset] and coord=1.0 is lut[offset+width-1]
          "lut_coord *= float(lut_width - 1);\n");
    e.If(
        linear,
        [&] {
            e.Raw("int lut_index_i = int(lut_coord) + lut_offset;\n"
                  "float lut_index_f = fract(lut_coord);\n"
                  "return texelFetch(texture_buffer_lut_rgba, lut_index_i + proctex_lut_offset) + "
                  "lut_index_f * texelFetch(texture_buffer_lut_rgba, lut_index_i + proctex_diff_lut_offset);\n");
        },
        [&] {
            e.Raw("lut_coord += float(lut_offset);\n"
                  "return texelFetch(texture_buffer_lut_rgba, int(round(lut_coord)) + proctex_lut_offset);\n");
        });
    e.Raw("}\n");

    e.Raw("vec4 ProcTex() {\nvec2 uv;\n");
    e.Switch(
        e.Field(kw::kProcTexRaw, 1, 2),
        {{0, [&] { e.Raw("uv = abs(texcoord0);\n"); }},
         {1, [&] { e.Raw("uv = abs(texcoord1);\n"); }},
         {2, [&] { e.Raw("uv = abs(texcoord2);\n"); }}},
        [&] { e.Raw("uv = abs(texcoord0);\n"); });

    // This LOD formula is the same as the LOD upper limit defined in OpenGL (Azahar: not the one normal 2D textures use). Unlike
    // normal textures, the bias is inside the log2.
    e.Raw("vec2 duv = max(abs(dFdx(uv)), abs(dFdy(uv)));\n");
    e.Line("float lod = log2(abs(float({}) * proctex_bias) * (duv.x + duv.y));", IntOf(width));
    e.Raw("if (proctex_bias == 0.0) lod = 0.0;\n");
    const Value lod_min = e.Field(kWordLod, 0, 16);
    const Value lod_max = e.Field(kWordLod, 16, 16);
    e.Line("lod = clamp(lod, max(0.0, {}), min(7.0, {}));", FloatOf(lod_min), FloatOf(lod_max));

    // Get shift offset before noise generation.
    const Value u_clamp = e.Field(kw::kProcTexRaw, 3, 3);
    const Value v_clamp = e.Field(kw::kProcTexRaw, 6, 3);
    EmitShift(e, "u_shift", e.Field(kw::kProcTexRaw, 22, 2), u_clamp, "uv.y");
    EmitShift(e, "v_shift", e.Field(kw::kProcTexRaw, 24, 2), v_clamp, "uv.x");

    // Generate noise.
    e.If(e.Eq(e.Field(kw::kProcTexRaw, 21, 1), 1), [&] {
        e.Raw("uv += proctex_noise_a * ProcTexNoiseCoef(uv);\n"
              "uv = abs(uv);\n");
    });

    // Shift, clamp.
    e.Raw("float u = uv.x + u_shift;\n"
          "float v = uv.y + v_shift;\n");
    EmitClamp(e, "u", u_clamp);
    EmitClamp(e, "v", v_clamp);

    // Combine and map.
    e.Line("float lut_coord = ProcTexLookupLUT(proctex_color_map_offset, {});",
           CombinedOf(e, e.Field(kw::kProcTexRaw, 9, 4)));

    // The level (or the lower of two) and, for the linear-mipmap filters, the mix with the next level: at most two call sites.
    e.Raw("int lod_i = 0;\n");
    e.If(mip_nearest, [&] { e.Raw("lod_i = int(round(lod));\n"); });
    e.If(mip_linear, [&] { e.Raw("lod_i = int(lod);\n"); });
    e.Raw("vec4 final_color = SampleProcTexColor(lut_coord, lod_i);\n");
    e.If(mip_linear, [&] {
        e.Raw("final_color = mix(final_color, SampleProcTexColor(lut_coord, min(lod_i + 1, 7)), fract(lod));\n");
    });

    // Note: in separate alpha mode, the alpha channel skips the color LUT look up stage. It uses the output of CombineAndMap
    // directly instead.
    e.If(
        e.Eq(e.Field(kw::kProcTexRaw, 20, 1), 1),
        [&] {
            e.Line("float final_alpha = ProcTexLookupLUT(proctex_alpha_map_offset, {});",
                   CombinedOf(e, e.Field(kw::kProcTexRaw, 13, 4)));
            e.Raw("return vec4(final_color.xyz, final_alpha);\n");
        },
        [&] { e.Raw("return final_color;\n"); });
    e.Raw("}\n");
}

// Pica::f16::FromRaw(half).ToFloat32() (pica_types.h): a normal half exactly, a denormal renormalised exactly, zero with its sign,
// an infinity or a NaN kept (the mantissa moved to the top of fp32's).
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
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The procedural table bits of TableDirty (ProcTexLutTable values).
constexpr u32 kTableNoise = 0, kTableColorMap = 2, kTableAlphaMap = 3, kTableColor = 4, kTableColorDiff = 5;

class ProcTexFeature final : public FeatureModule {
public:
    const char* Name() const override { return "proctex"; }

    void Emit(Point point, EmitContext& ctx) const override {
        if (point != Point::Declarations) {
            return;
        }
        Emitter& e = *ctx.emitter;
        if (e.specialised() && ctx.key.proctex.enable() == 0) {
            // sampleTexUnit3 does not call it (Azahar emits nothing); the prototype block declares it.
            e.Raw("vec4 ProcTex() { return vec4(0.0); }\n");
            return;
        }
        EmitProcTex(e);
    }

    // The procedural texture reads the two texel buffers of the general ring while it is on.
    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        Needs needs;
        needs.lut_rg = key.proctex.enable() != 0;
        needs.lut_rgba = key.proctex.enable() != 0;
        return needs;
    }

    // SyncDrawUniforms' proctex part: the noise frequency, amplitude (over 4095), phase and the bias, when one of the six registers
    // 0xA8 to 0xAD is dirty (CheckProctex).
    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        bool changed = dirty.all;
        for (u32 id = freg::kProcTex; id <= freg::kProcTexLutOffset; ++id) {
            changed = changed || state.dirty.Test(id);
        }
        if (!changed) {
            return;
        }
        const auto& regs = state.regs;
        const u32 noise_u = regs[freg::kProcTexNoiseU];
        const u32 noise_v = regs[freg::kProcTexNoiseV];
        const u32 frequency = regs[freg::kProcTexNoiseFrequency];
        const Vec2f noise_f{HalfToFloat(Bits(frequency, 0, 16)), HalfToFloat(Bits(frequency, 16, 16))};
        const Vec2f noise_a{static_cast<float>(static_cast<s16>(Bits(noise_u, 0, 16))) / 4095.0f,
                            static_cast<float>(static_cast<s16>(Bits(noise_v, 0, 16))) / 4095.0f};
        const Vec2f noise_p{HalfToFloat(Bits(noise_u, 16, 16)), HalfToFloat(Bits(noise_v, 16, 16))};
        const float bias =
            HalfToFloat(Bits(regs[freg::kProcTex], 20, 8) | (Bits(regs[freg::kProcTexLut], 19, 8) << 8));

        FragmentUniforms& uniforms = block.uniforms;
        if (dirty.all || std::memcmp(&noise_f, &uniforms.proctex_noise_f, sizeof(noise_f)) != 0 ||
            std::memcmp(&noise_a, &uniforms.proctex_noise_a, sizeof(noise_a)) != 0 ||
            std::memcmp(&noise_p, &uniforms.proctex_noise_p, sizeof(noise_p)) != 0 ||
            std::memcmp(&bias, &uniforms.proctex_bias, sizeof(bias)) != 0) {
            uniforms.proctex_noise_f = noise_f;
            uniforms.proctex_noise_a = noise_a;
            uniforms.proctex_noise_p = noise_p;
            uniforms.proctex_bias = bias;
            dirty.Mark();
        }
    }

    // SyncAndUploadLUTs' procedural part, in Azahar's order: noise, colour map and alpha map as {value, difference} pairs (R32G32,
    // 128 entries), then the colour table and the colour difference table (R32G32B32A32, 256 entries). Each offset, in elements of
    // its view, is written from the table's own slice. Exactly the tables `need` names are uploaded.
    void UploadTables(TableUpload& upload) const override {
        const u8 need = upload.need.proctex;
        if (need == 0) {
            return;
        }
        const pica::ProcTex& tables = upload.state.proctex;
        UploadValueTable(upload, need, kTableNoise, tables.noise_table, upload.block.uniforms.proctex_noise_lut_offset);
        UploadValueTable(upload, need, kTableColorMap, tables.color_map_table, upload.block.uniforms.proctex_color_map_offset);
        UploadValueTable(upload, need, kTableAlphaMap, tables.alpha_map_table, upload.block.uniforms.proctex_alpha_map_offset);
        UploadColorTable(upload, need, kTableColor, tables.color_table, false, upload.block.uniforms.proctex_lut_offset);
        UploadColorTable(upload, need, kTableColorDiff, tables.color_diff_table, true, upload.block.uniforms.proctex_diff_lut_offset);
    }

private:
    // ValueEntry: value bits 0-11 (over 4095), difference bits 12-23 (signed, over 4095).
    static void UploadValueTable(TableUpload& upload, u8 need, u32 table, const std::array<u32, 128>& source, int& offset) {
        if ((need >> table & 1u) == 0) {
            return;
        }
        rhi::Result<TableSlice> slice = upload.rings.Take(TableRing::General, source.size() * sizeof(Vec2f));
        if (!slice.ok()) {
            return;
        }
        for (size_t i = 0; i < source.size(); ++i) {
            const s32 difference = static_cast<s32>(Bits(source[i], 12, 12) << 20) >> 20;
            const Vec2f entry{static_cast<float>(Bits(source[i], 0, 12)) / 4095.0f, static_cast<float>(difference) / 4095.0f};
            std::memcpy(slice->data.data() + i * sizeof(Vec2f), &entry, sizeof(entry));
        }
        offset = slice->ElementOffset(sizeof(Vec2f));
        upload.dirty.Mark();
    }

    // ColorEntry: four bytes over 255; ColorDifferenceEntry: four signed bytes, each stored as half the difference, so the
    // vector is twice them (ToVector), over 255.
    static void UploadColorTable(TableUpload& upload, u8 need, u32 table, const std::array<u32, 256>& source, bool difference,
                                 int& offset) {
        if ((need >> table & 1u) == 0) {
            return;
        }
        rhi::Result<TableSlice> slice = upload.rings.Take(TableRing::General, source.size() * sizeof(Vec4f));
        if (!slice.ok()) {
            return;
        }
        for (size_t i = 0; i < source.size(); ++i) {
            float c[4];
            for (u32 lane = 0; lane < 4; ++lane) {
                if (difference) {
                    const s32 half = static_cast<s32>(Bits(source[i], lane * 8, 8) << 24) >> 24;
                    c[lane] = static_cast<float>(half * 2) / 255.0f;
                } else {
                    c[lane] = static_cast<float>(Bits(source[i], lane * 8, 8)) / 255.0f;
                }
            }
            const Vec4f entry{c[0], c[1], c[2], c[3]};
            std::memcpy(slice->data.data() + i * sizeof(Vec4f), &entry, sizeof(entry));
        }
        offset = slice->ElementOffset(sizeof(Vec4f));
        upload.dirty.Mark();
    }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(ProcTexFeature)
}  // namespace mh4n::gpu::fragment
