// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (FragmentModule::Generate, WriteScissor,
// WriteDepth, GetSource, AppendColorModifier, AppendAlphaModifier, AppendColorCombiner, AppendAlphaCombiner,
// WriteAlphaTestCondition, WriteTevStage, WriteBlending, WriteLogicOp, DefineExtensions, DefineInterface,
// DefineBindingsVK, DefineHelpers and the FSUniformBlockDef text), src/video_core/shader/generator/pica_fs_config.cpp and
// pica_fs_config.h (FramebufferConfig::ApplyProfile and BlendConfig::UsesNoOpFactors: which blend halves and which logic
// op the shader emulates) and src/video_core/pica/regs_texturing.h (the TEV stage's enumerations and multipliers) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: glsl_fs_shader_gen.cpp, pica_fs_config.cpp and
// pica_fs_config.h say "Licensed under GPLv3 or any later version"; regs_texturing.h says "Licensed under GPLv2 or any
// later version"). The two-mode emitter, the ubershader prelude, the use of N2-13's output-stage functions in
// place of WriteBlending's and WriteLogicOp's text and the separate texture and sampler bindings are ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp, pica_fs_config.cpp,
//   pica_fs_config.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
//
// The fragment generator (N2-08). See generator.h for what core emits and the order of the points, emitter.h for the two modes.
//
// WHERE THIS DIFFERS FROM AZAHAR'S TEXT, ON PURPOSE:
//  - the combiner variables (`rounded_primary_color`, `primary_fragment_color`, `secondary_fragment_color`, `combiner_buffer`,
//    `combiner_output`) are globals, so the ubershader's helper functions (`tev_source`) and the features see them;
//  - Azahar's `WriteBlending` and `WriteLogicOp` text is replaced by N2-13's `mh4n_minmax_blend` and `mh4n_logic_op`
//    (fbops/output_stage.glsl), whose arguments are Azahar's enumerator values; the `~color` of WriteLogicOp's CopyInverted
//    is not copied (it is not valid on a float vector): mh4n_logic_op does the operation on the 8-bit values;
//  - the destination colour (`destFactor` in Azahar) is read only by the draws that need it (a conditional fetch in the
//    ubershader), and the separate texture and sampler objects replace the combined samplers (design 5.6);
//  - an operand that the key holds as a field is emitted by one table for both modes (the tables below), so the text of the
//    specialised shader is Azahar's and the ubershader's `switch` has the same cases.
#include "gpu/fragment/generator.h"

#include <array>
#include <cstring>
#include <utility>

#include "gpu/fbops/output_stage.h"

namespace mh4n::gpu::fragment {
namespace {

namespace kw = keyword;

// ---- the tables (Azahar's switch statements, one entry per enumerator) ----------------------------------------------

struct Entry {
    u32 value;
    const char* text;  // {0}: the operand (the source text), {1}: the stage index
};

// GetSource (TevStageConfig::Source). The default (an unknown value) is "vec4(0.0)".
constexpr Entry kSources[] = {
    {0x0, "rounded_primary_color"},     // PrimaryColor
    {0x1, "primary_fragment_color"},    // PrimaryFragmentColor
    {0x2, "secondary_fragment_color"},  // SecondaryFragmentColor
    {0x3, "sampleTexUnit0()"},          // Texture0
    {0x4, "sampleTexUnit1()"},          // Texture1
    {0x5, "sampleTexUnit2()"},          // Texture2
    {0x6, "sampleTexUnit3()"},          // Texture3
    {0xD, "combiner_buffer"},           // PreviousBuffer
    {0xE, "const_color[{1}]"},          // Constant
    {0xF, "combiner_output"},           // Previous
};
constexpr const char* kSourceDefault = "vec4(0.0)";

// AppendColorModifier (TevStageConfig::ColorModifier). The default is "vec3(0.0)".
constexpr Entry kColorModifiers[] = {
    {0x0, "{0}.rgb"},                 // SourceColor
    {0x1, "vec3(1.0) - {0}.rgb"},     // OneMinusSourceColor
    {0x2, "{0}.aaa"},                 // SourceAlpha
    {0x3, "vec3(1.0) - {0}.aaa"},     // OneMinusSourceAlpha
    {0x4, "{0}.rrr"},                 // SourceRed
    {0x5, "vec3(1.0) - {0}.rrr"},     // OneMinusSourceRed
    {0x8, "{0}.ggg"},                 // SourceGreen
    {0x9, "vec3(1.0) - {0}.ggg"},     // OneMinusSourceGreen
    {0xC, "{0}.bbb"},                 // SourceBlue
    {0xD, "vec3(1.0) - {0}.bbb"},     // OneMinusSourceBlue
};
constexpr const char* kColorModifierDefault = "vec3(0.0)";

// AppendAlphaModifier (TevStageConfig::AlphaModifier). The default is "0.0".
constexpr Entry kAlphaModifiers[] = {
    {0x0, "{0}.a"},          // SourceAlpha
    {0x1, "1.0 - {0}.a"},    // OneMinusSourceAlpha
    {0x2, "{0}.r"},          // SourceRed
    {0x3, "1.0 - {0}.r"},    // OneMinusSourceRed
    {0x4, "{0}.g"},          // SourceGreen
    {0x5, "1.0 - {0}.g"},    // OneMinusSourceGreen
    {0x6, "{0}.b"},          // SourceBlue
    {0x7, "1.0 - {0}.b"},    // OneMinusSourceBlue
};
constexpr const char* kAlphaModifierDefault = "0.0";

// AppendColorCombiner (TevStageConfig::Operation), before the clamp. The default is "vec3(0.0)".
constexpr Entry kColorCombiners[] = {
    {0, "color_results_1"},                                                        // Replace
    {1, "color_results_1 * color_results_2"},                                      // Modulate
    {2, "color_results_1 + color_results_2"},                                      // Add
    {3, "color_results_1 + color_results_2 - vec3(0.5)"},                          // AddSigned
    {4, "mix(color_results_2, color_results_1, color_results_3)"},                 // Lerp
    {5, "color_results_1 - color_results_2"},                                      // Subtract
    {6, "vec3(dot(color_results_1 - vec3(0.5), color_results_2 - vec3(0.5)) * 4.0)"},  // Dot3_RGB
    {7, "vec3(dot(color_results_1 - vec3(0.5), color_results_2 - vec3(0.5)) * 4.0)"},  // Dot3_RGBA
    {8, "fma(color_results_1, color_results_2, color_results_3)"},                 // MultiplyThenAdd
    {9, "min(color_results_1 + color_results_2, vec3(1.0)) * color_results_3"},    // AddThenMultiply
};
constexpr const char* kColorCombinerDefault = "vec3(0.0)";

// AppendAlphaCombiner. Dot3_RGB and Dot3_RGBA have none (the default, "0.0"; Dot3_RGBA does not run the alpha combiner).
constexpr Entry kAlphaCombiners[] = {
    {0, "alpha_results_1"},
    {1, "alpha_results_1 * alpha_results_2"},
    {2, "alpha_results_1 + alpha_results_2"},
    {3, "alpha_results_1 + alpha_results_2 - 0.5"},
    {4, "mix(alpha_results_2, alpha_results_1, alpha_results_3)"},
    {5, "alpha_results_1 - alpha_results_2"},
    {8, "fma(alpha_results_1, alpha_results_2, alpha_results_3)"},
    {9, "min(alpha_results_1 + alpha_results_2, 1.0) * alpha_results_3"},
};
constexpr const char* kAlphaCombinerDefault = "0.0";

// WriteAlphaTestCondition (CompareFunc): the condition under which the fragment is discarded. The default is "false".
constexpr Entry kAlphaTests[] = {
    {0, "true"},                                           // Never
    {1, "false"},                                          // Always
    {2, "int(combiner_output.a * 255.0) != alphatest_ref"},  // Equal
    {3, "int(combiner_output.a * 255.0) == alphatest_ref"},  // NotEqual
    {4, "int(combiner_output.a * 255.0) >= alphatest_ref"},  // LessThan
    {5, "int(combiner_output.a * 255.0) > alphatest_ref"},   // LessThanOrEqual
    {6, "int(combiner_output.a * 255.0) <= alphatest_ref"},  // GreaterThan
    {7, "int(combiner_output.a * 255.0) < alphatest_ref"},   // GreaterThanOrEqual
};
constexpr const char* kAlphaTestDefault = "false";

template <size_t N>
std::string Subst(const Entry (&table)[N], const char* otherwise, u32 value, std::string_view operand, u32 index) {
    const char* text = otherwise;
    for (const Entry& entry : table) {
        if (entry.value == value) {
            text = entry.text;
            break;
        }
    }
    return fmt::format(fmt::runtime(text), operand, index);
}

// The GLSL of a function over the table: `switch (selector)` with one case per entry.
template <size_t N>
void DefineSwitchFunction(std::string& out, std::string_view signature, std::string_view selector, const Entry (&table)[N],
                          const char* otherwise, std::string_view operand, std::string_view index) {
    out += fmt::format("{} {{\nswitch ({}) {{\n", signature, selector);
    for (const Entry& entry : table) {
        std::string text = fmt::format(fmt::runtime(entry.text), operand, index);
        out += fmt::format("case {}u: return {};\n", entry.value, text);
    }
    out += fmt::format("default: return {};\n}}\n}}\n", fmt::format(fmt::runtime(otherwise), operand, index));
}

// ---- layout constants (native/bindings.h; gpu_tev_test checks they agree) -----------------------------------------------

constexpr const char* kFsUniformBlock = R"(
#define NUM_TEV_STAGES 6
#define NUM_LIGHTS 8
#define NUM_LIGHTING_SAMPLERS 24
struct LightSrc {
    vec3 specular_0;
    vec3 specular_1;
    vec3 diffuse;
    vec3 ambient;
    vec3 position;
    vec3 spot_direction;
    float dist_atten_bias;
    float dist_atten_scale;
};
layout (set = 0, binding = 2, std140) uniform fs_data {
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
    ivec4 lighting_lut_offset[NUM_LIGHTING_SAMPLERS / 4];
    vec3 fog_color;
    vec2 proctex_noise_f;
    vec2 proctex_noise_a;
    vec2 proctex_noise_p;
    vec3 lighting_global_ambient;
    LightSrc light_src[NUM_LIGHTS];
    vec4 const_color[NUM_TEV_STAGES];
    vec4 tev_combiner_buffer_color;
    vec3 tex_lod_bias;
    vec4 tex_border_color[3];
    vec4 blend_color;
    uvec4 cfg[18];
    uvec4 gas_regs[4];
    uvec4 gas_table[4];
};
)";

constexpr const char* kBindings = R"(
layout(set = 0, binding = 3) uniform samplerBuffer texture_buffer_lut_lf;
layout(set = 0, binding = 4) uniform samplerBuffer texture_buffer_lut_rg;
layout(set = 0, binding = 5) uniform samplerBuffer texture_buffer_lut_rgba;

layout(set = 1, binding = 0) uniform texture2D tex0;
layout(set = 1, binding = 1) uniform textureCube tex0_cube;
layout(set = 1, binding = 2) uniform utexture2D tex0_shadow;
layout(set = 1, binding = 3) uniform sampler samp0;
layout(set = 1, binding = 4) uniform texture2D tex1;
layout(set = 1, binding = 5) uniform sampler samp1;
layout(set = 1, binding = 6) uniform texture2D tex2;
layout(set = 1, binding = 7) uniform sampler samp2;
layout(set = 1, binding = 8) uniform texture2D tex_color;

)";

// DefineHelpers.
constexpr const char* kHelpers = R"(
vec3 quaternion_rotate(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

float byteround(float x) {
    return round(x * 255.0) * (1.0 / 255.0);
}

vec2 byteround(vec2 x) {
    return round(x * 255.0) * (1.0 / 255.0);
}

vec3 byteround(vec3 x) {
    return round(x * 255.0) * (1.0 / 255.0);
}

vec4 byteround(vec4 x) {
    return round(x * 255.0) * (1.0 / 255.0);
}

float getLod(vec2 coord) {
    vec2 d = max(abs(dFdx(coord)), abs(dFdy(coord)));
    return log2(max(d.x, d.y));
}

uvec2 DecodeShadow(uint pixel) {
    return uvec2(pixel >> 8, pixel & 0xFFu);
}
)";

// The prototype block (design 4.8): every definition is a feature's, the order of definitions does not matter.
constexpr const char* kPrototypes = R"(
vec4 sampleTexUnit0();
vec4 sampleTexUnit1();
vec4 sampleTexUnit2();
vec4 sampleTexUnit3();
vec4 shadowTexture(vec2 uv, float w);
vec4 shadowTextureCube(vec2 uv, float w);
vec4 ProcTex();
)";

constexpr const char* kGlobals = R"(
vec4 rounded_primary_color = vec4(0.0);
vec4 primary_fragment_color = vec4(0.0);
vec4 secondary_fragment_color = vec4(0.0);
vec4 combiner_buffer = vec4(0.0);
vec4 combiner_output = vec4(0.0);
)";

// The values of the enumerators the generator tests itself.
constexpr u32 kSourcePrevious = 0xF;
constexpr u32 kOpReplace = 0, kOpDot3Rgba = 7;
constexpr u32 kCompareNever = 0;
constexpr u32 kScissorDisabled = 0, kScissorInclude = 3;
constexpr u32 kDepthWBuffering = 0;
constexpr u32 kBlendMin = 3, kBlendMax = 4, kFactorOne = 1;
constexpr u32 kLogicCopy = 3, kLogicNoOp = 6;

class Generator {
public:
    Generator(Mode mode, const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
              const FeatureRegistry& features)
        : profile_(profile),
          applied_(Applied(mode, key, profile)),
          e_(mode, applied_, profile, pass),
          ctx_{applied_, profile_, mode, &e_},
          features_(features) {}

    std::string Run() {
        DefineExtensions();
        DefineInterface();
        DefineBindings();
        e_.Raw(kHelpers);
        e_.Raw(kPrototypes);
        e_.Raw(kGlobals);
        if (!e_.specialised()) {
            DefineUberPrelude();
        }
        Features(Point::Declarations);
        GenerateMain();
        return std::move(e_.text());
    }

private:
    static FragmentKey Applied(Mode mode, const FragmentKey& key, const DeviceProfile& profile) {
        if (mode == Mode::Uber) {
            return FragmentKey{};  // the ubershader reads cfg: nothing of the key is known
        }
        FragmentKey applied = key;
        applied.ApplyProfile(profile);
        return applied;
    }

    void Features(Point point) {
        for (const FeatureModule* feature : features_.All()) {
            feature->Emit(point, ctx_);
        }
    }

    // A braced block, so a name declared inside does not reach out (the bodies of an `If` that was decided at generation
    // time have no braces of their own).
    void Block(const Emitter::Body& body) {
        e_.Raw("{\n");
        body();
        e_.Raw("}\n");
    }

    // ---- declarations ----------------------------------------------------------------------------------------------

    void DefineExtensions() {
        e_.Raw("#version 450\n");
        // texelFetch and textureSize on a separate texture (design 5.6)
        e_.Raw("#extension GL_EXT_samplerless_texture_functions : enable\n");
        if (e_.pass() == ShaderPass::ShadowWrite && profile_.interlock) {
            // Azahar's DefineExtensions for the shadow write; the generator declares no pixel_interlock_* layout, and glslang
            // defaults to pixel_interlock_ordered (design 5.1).
            e_.Raw("#extension GL_ARB_fragment_shader_interlock : enable\n"
                   "#define beginInvocationInterlock beginInvocationInterlockARB\n"
                   "#define endInvocationInterlock endInvocationInterlockARB\n");
        }
    }

    // DefineInterface: the seven varyings at the locations of Azahar's `Semantic` enumeration (Color 1 .. View 7).
    void DefineInterface() {
        e_.Raw("layout (location = 1) in vec4 primary_color;\n"
               "layout (location = 2) in vec2 texcoord0;\n"
               "layout (location = 3) in vec2 texcoord1;\n"
               "layout (location = 4) in vec2 texcoord2;\n"
               "layout (location = 5) in float texcoord0_w;\n"
               "layout (location = 6) in vec4 normquat;\n"
               "layout (location = 7) in vec3 view;\n");
        e_.Raw("layout (location = 0) out vec4 color;\n\n");
    }

    // DefineBindingsVK, with the separate textures and samplers of the one pipeline layout.
    void DefineBindings() {
        e_.Raw(kFsUniformBlock);
        e_.Raw(kBindings);
        if (e_.pass() == ShaderPass::ShadowWrite) {
            e_.Raw("layout(set = 2, binding = 0, r32ui) uniform uimage2D shadow_buffer;\n\n");
        }
    }

    // The ubershader's helpers: the same tables as the specialised mode, as functions over the operand's field.
    void DefineUberPrelude() {
        std::string& out = e_.text();
        DefineSwitchFunction(out, "vec4 tev_source(uint source, uint tev_index)", "source", kSources, kSourceDefault, "", "tev_index");
        DefineSwitchFunction(out, "vec3 tev_color_modifier(uint modifier, vec4 s)", "modifier", kColorModifiers, kColorModifierDefault,
                             "s", "0");
        DefineSwitchFunction(out, "float tev_alpha_modifier(uint modifier, vec4 s)", "modifier", kAlphaModifiers, kAlphaModifierDefault,
                             "s", "0");
        DefineSwitchFunction(out, "vec3 tev_color_combiner(uint op, vec3 color_results_1, vec3 color_results_2, vec3 color_results_3)",
                             "op", kColorCombiners, kColorCombinerDefault, "", "0");
        DefineSwitchFunction(out, "float tev_alpha_combiner(uint op, float alpha_results_1, float alpha_results_2, float alpha_results_3)",
                             "op", kAlphaCombiners, kAlphaCombinerDefault, "", "0");
        DefineSwitchFunction(out, "bool alpha_test_discards(uint func)", "func", kAlphaTests, kAlphaTestDefault, "", "0");
    }

    // ---- main ----------------------------------------------------------------------------------------------------------

    void GenerateMain() {
        // We round the interpolated primary color to the nearest 1/255th. This maintains the PICA's 8 bits of precision.
        e_.Raw("void main() {\n"
               "rounded_primary_color = byteround(primary_color);\n"
               "primary_fragment_color = vec4(0.0);\n"
               "secondary_fragment_color = vec4(0.0);\n");

        // Do not do any sort of processing if it's obvious we're not going to pass the alpha test.
        const Value alpha_func = e_.Field(kw::kFbRaw, 0, 3);
        const Cond never = e_.Eq(alpha_func, kCompareNever);
        if (never.known && never.value) {
            e_.Raw("discard; }");
            return;
        }
        if (!never.known) {
            e_.Raw(fmt::format("if ({}) {{ discard; }}\n", never.glsl));
        }
        Features(Point::Prologue);

        WriteScissor();
        WriteDepth();
        Features(Point::ScissorDepth);

        // Write shader source to emulate all enabled PICA lights.
        Features(Point::Lighting);

        e_.Raw("vec4 next_combiner_buffer = tev_combiner_buffer_color;\n"
               "combiner_buffer = vec4(0.0);\n"
               "combiner_output = vec4(0.0);\n");
        e_.Raw("vec3 color_results_1 = vec3(0.0);\n"
               "vec3 color_results_2 = vec3(0.0);\n"
               "vec3 color_results_3 = vec3(0.0);\n");
        e_.Raw("float alpha_results_1 = 0.0;\n"
               "float alpha_results_2 = 0.0;\n"
               "float alpha_results_3 = 0.0;\n");
        Features(Point::Tev);

        // Write shader source to emulate PICA TEV stages.
        for (u32 index = 0; index < 6; ++index) {
            WriteTevStage(index);
        }

        // Append the alpha test condition.
        WriteAlphaTestCondition(alpha_func);
        Features(Point::AlphaTest);

        // Emulate the fog (the gas hook of fog/ may return).
        Features(Point::FogGas);

        if (e_.pass() == ShaderPass::Normal) {
            WriteOutput();
        }
        Features(Point::Output);
        e_.Raw("}");
    }

    // WriteDepth. The PICA depth range is [-1, 0]; the vertex shader outputs the negated Z value; the Vulkan clip range
    // is [0, 1], so `z_over_w = -gl_FragCoord.z` (Azahar's profile without `has_minus_one_to_one_range`).
    void WriteDepth() {
        e_.Raw("float z_over_w = -gl_FragCoord.z;\n");
        e_.Raw("float depth = z_over_w * depth_scale + depth_offset;\n");
        e_.If(e_.Eq(e_.Field(kw::kFbRaw, 5, 1), kDepthWBuffering), [&] { e_.Raw("depth /= gl_FragCoord.w;\n"); });
    }

    // WriteScissor.
    void WriteScissor() {
        const Value mode = e_.Field(kw::kFbRaw, 3, 2);
        const char* inside =
            "(gl_FragCoord.x >= float(scissor_x1) && "
            "gl_FragCoord.y >= float(scissor_y1) && "
            "gl_FragCoord.x < float(scissor_x2) && "
            "gl_FragCoord.y < float(scissor_y2))";
        e_.If(e_.Ne(mode, kScissorDisabled), [&] {
            // Negate the condition if we have to keep only the pixels outside the scissor box.
            e_.If(
                e_.Eq(mode, kScissorInclude), [&] { e_.Raw(fmt::format("if (!{}) discard;\n", inside)); },
                [&] { e_.Raw(fmt::format("if ({}) discard;\n", inside)); });
        });
    }

    // ---- TEV ------------------------------------------------------------------------------------------------------------

    std::string SourceText(const Value& source, u32 index) const {
        if (source.known) {
            return Subst(kSources, kSourceDefault, source.literal, "", index);
        }
        return fmt::format("tev_source({}, {}u)", source.glsl, index);
    }

    std::string ColorModifierText(const Value& modifier, const std::string& source) const {
        if (modifier.known) {
            return Subst(kColorModifiers, kColorModifierDefault, modifier.literal, source, 0);
        }
        return fmt::format("tev_color_modifier({}, {})", modifier.glsl, source);
    }

    std::string AlphaModifierText(const Value& modifier, const std::string& source) const {
        if (modifier.known) {
            return Subst(kAlphaModifiers, kAlphaModifierDefault, modifier.literal, source, 0);
        }
        return fmt::format("tev_alpha_modifier({}, {})", modifier.glsl, source);
    }

    // AppendColorCombiner: the operation, clamped.
    std::string ColorCombinerText(const Value& op) const {
        if (op.known) {
            return fmt::format("clamp({}, vec3(0.0), vec3(1.0))", Subst(kColorCombiners, kColorCombinerDefault, op.literal, "", 0));
        }
        return fmt::format("clamp(tev_color_combiner({}, color_results_1, color_results_2, color_results_3), vec3(0.0), vec3(1.0))",
                           op.glsl);
    }

    std::string AlphaCombinerText(const Value& op) const {
        if (op.known) {
            return fmt::format("clamp({}, 0.0, 1.0)", Subst(kAlphaCombiners, kAlphaCombinerDefault, op.literal, "", 0));
        }
        return fmt::format("clamp(tev_alpha_combiner({}, alpha_results_1, alpha_results_2, alpha_results_3), 0.0, 1.0)", op.glsl);
    }

    // GetColorMultiplier / GetAlphaMultiplier: `(scale < 3) ? (1 << scale) : 1`, as a GLSL float expression.
    std::string MultiplierText(const Value& scale) const {
        if (scale.known) {
            return fmt::format("{}.0", scale.literal < 3 ? (1u << scale.literal) : 1u);
        }
        return fmt::format("float(({0} < 3u) ? (1u << {0}) : 1u)", scale.glsl);
    }

    // The forced source of stage 0: `force_source3 = tev_index == 0 && source == Previous` takes source 3 for it.
    Value ForcedSource(u32 index, const Value& source, const Value& source3) const {
        if (index != 0) {
            return source;
        }
        return e_.Select(e_.Eq(source, kSourcePrevious), source3, source);
    }

    void WriteTevStage(u32 index) {
        const size_t word = kw::kTexTevStages + index * kw::kTevStageWords;
        const Value color_op = e_.Field(word + 2, 0, 4);
        const Value alpha_op = e_.Field(word + 2, 16, 4);
        const Value color_scale = e_.Field(word + 3, 0, 2);
        const Value alpha_scale = e_.Field(word + 3, 16, 2);

        // IsPassThroughTevStage.
        Cond pass_through = e_.Eq(color_op, kOpReplace);
        pass_through = e_.And(pass_through, e_.Eq(alpha_op, kOpReplace));
        pass_through = e_.And(pass_through, e_.Eq(e_.Field(word + 0, 0, 4), kSourcePrevious));
        pass_through = e_.And(pass_through, e_.Eq(e_.Field(word + 0, 16, 4), kSourcePrevious));
        pass_through = e_.And(pass_through, e_.Eq(e_.Field(word + 1, 0, 4), 0));   // SourceColor
        pass_through = e_.And(pass_through, e_.Eq(e_.Field(word + 1, 12, 3), 0));  // SourceAlpha
        pass_through = e_.And(pass_through, e_.Or(e_.Eq(color_scale, 0), e_.Eq(color_scale, 3)));
        pass_through = e_.And(pass_through, e_.Or(e_.Eq(alpha_scale, 0), e_.Eq(alpha_scale, 3)));

        e_.If(e_.Not(pass_through), [&] {
            // The three colour operands: source, modifier.
            const Value color_source3 = e_.Field(word + 0, 8, 4);
            const Value color_sources[3] = {ForcedSource(index, e_.Field(word + 0, 0, 4), color_source3),
                                            ForcedSource(index, e_.Field(word + 0, 4, 4), color_source3),
                                            ForcedSource(index, color_source3, color_source3)};
            const Value color_modifiers[3] = {e_.Field(word + 1, 0, 4), e_.Field(word + 1, 4, 4), e_.Field(word + 1, 8, 4)};
            const Value alpha_source3 = e_.Field(word + 0, 24, 4);
            const Value alpha_sources[3] = {ForcedSource(index, e_.Field(word + 0, 16, 4), alpha_source3),
                                            ForcedSource(index, e_.Field(word + 0, 20, 4), alpha_source3),
                                            ForcedSource(index, alpha_source3, alpha_source3)};
            const Value alpha_modifiers[3] = {e_.Field(word + 1, 12, 3), e_.Field(word + 1, 16, 3), e_.Field(word + 1, 20, 3)};

            for (u32 i = 0; i < 3; ++i) {
                e_.Raw(fmt::format("color_results_{} = {};\n", i + 1,
                                   ColorModifierText(color_modifiers[i], SourceText(color_sources[i], index))));
            }
            // Round the output of each TEV stage to maintain the PICA's 8 bits of precision.
            e_.Raw(fmt::format("vec3 color_output_{} = byteround({});\n", index, ColorCombinerText(color_op)));

            // (declared before the branches: in the ubershader they are blocks of their own)
            e_.Raw(fmt::format("float alpha_output_{} = 0.0;\n", index));
            e_.If(
                e_.Eq(color_op, kOpDot3Rgba),
                [&] {
                    // The result of the Dot3_RGBA operation is also placed to the alpha component.
                    e_.Raw(fmt::format("alpha_output_{0} = color_output_{0}[0];\n", index));
                },
                [&] {
                    for (u32 i = 0; i < 3; ++i) {
                        e_.Raw(fmt::format("alpha_results_{} = {};\n", i + 1,
                                           AlphaModifierText(alpha_modifiers[i], SourceText(alpha_sources[i], index))));
                    }
                    e_.Raw(fmt::format("alpha_output_{} = byteround({});\n", index, AlphaCombinerText(alpha_op)));
                });

            e_.Raw(fmt::format("combiner_output = vec4(clamp(color_output_{0} * {1}, vec3(0.0), vec3(1.0)), "
                               "clamp(alpha_output_{0} * {2}, 0.0, 1.0));\n",
                               index, MultiplierText(color_scale), MultiplierText(alpha_scale)));
        });

        // The combiner buffer: this stage reads the previous stage's `next_combiner_buffer`, and updates it where the
        // fog register's masks say (stages 0 to 3 only).
        e_.Raw("combiner_buffer = next_combiner_buffer;\n");
        if (index < 4) {
            e_.If(e_.Ne(e_.Field(kw::kTexRaw, 4 + index, 1), 0), [&] { e_.Raw("next_combiner_buffer.rgb = combiner_output.rgb;\n"); });
            e_.If(e_.Ne(e_.Field(kw::kTexRaw, 8 + index, 1), 0), [&] { e_.Raw("next_combiner_buffer.a = combiner_output.a;\n"); });
        }
    }

    // WriteAlphaTestCondition.
    void WriteAlphaTestCondition(const Value& func) {
        if (func.known) {
            e_.Raw(fmt::format("if ({}) discard;\n", Subst(kAlphaTests, kAlphaTestDefault, func.literal, "", 0)));
        } else {
            e_.Raw(fmt::format("if (alpha_test_discards({})) discard;\n", func.glsl));
        }
    }

    // ---- the output ----------------------------------------------------------------------------------------------------

    // The Min or Max blend equation of a half, to be computed in the shader: the equation is Min or Max and the factors are
    // not both One (Vulkan's MIN and MAX ignore the factors). The same formula holds on the profile-applied key of the
    // specialised mode (ApplyProfile sets the equation of a half with factors One and One to "disabled") and on the
    // requested key of the ubershader.
    Cond MinMaxEmulated(size_t blend_word) const {
        if (profile_.has_blend_minmax_factor) {
            return Emitter::False();
        }
        const Value eq = e_.Word(blend_word);
        Cond is_min_max = e_.Or(e_.Eq(eq, kBlendMin), e_.Eq(eq, kBlendMax));
        Cond both_one = e_.And(e_.Eq(e_.Word(blend_word + 1), kFactorOne), e_.Eq(e_.Word(blend_word + 2), kFactorOne));
        return e_.And(is_min_max, e_.Not(both_one));
    }

    // FramebufferConfig::logic_op after ApplyProfile.
    Value EffectiveLogicOp() const {
        if (e_.specialised()) {
            return e_.Field(kw::kFbRaw, 6, 4);
        }
        if (profile_.logic_op) {
            return Emitter::Const(kLogicCopy);
        }
        const Cond blending_off = e_.Eq(e_.Field(kw::kFbRaw, 11, 1), 0);
        return e_.Select(blending_off, e_.Word(kw::kFbLogicOp), Emitter::Const(kLogicCopy));
    }

    // The tail of a normal pass: the depth write, byte rounding, the min/max emulation and the logic op. N2-13's functions take
    // Azahar's enumerator values and do the work of WriteBlending and WriteLogicOp.
    void WriteOutput() {
        e_.Raw("gl_FragDepth = depth;\n");
        // Round the final fragment color to maintain the PICA's 8 bits of precision.
        e_.Raw("combiner_output = byteround(combiner_output);\n");
        e_.Raw("color = combiner_output;\n");

        const Cond rgb = MinMaxEmulated(kw::kFbRgbBlend);
        const Cond alpha = MinMaxEmulated(kw::kFbAlphaBlend);
        e_.If(e_.Or(rgb, alpha), [&] {
            Block([&] {
                e_.Raw("vec4 dest_color = texelFetch(tex_color, ivec2(gl_FragCoord.xy), 0);\n");
                const Value eq_rgb = e_.Select(rgb, e_.Word(kw::kFbRgbBlend), Emitter::Const(0));
                const Value eq_alpha = e_.Select(alpha, e_.Word(kw::kFbAlphaBlend), Emitter::Const(0));
                e_.Raw(fmt::format("color = mh4n_minmax_blend({}, {}, {}, {}, {}, {}, combiner_output, dest_color, blend_color);\n",
                                   eq_rgb.glsl, e_.Word(kw::kFbRgbBlend + 1).glsl, e_.Word(kw::kFbRgbBlend + 2).glsl, eq_alpha.glsl,
                                   e_.Word(kw::kFbAlphaBlend + 1).glsl, e_.Word(kw::kFbAlphaBlend + 2).glsl));
            });
        });

        // WriteLogicOp: Copy takes the colour as it is; NoOp is the pipeline's colour write mask of 0 (the depth write stays).
        const Value op = EffectiveLogicOp();
        const Cond emulate = e_.Not(e_.Or(e_.Eq(op, kLogicCopy), e_.Eq(op, kLogicNoOp)));
        e_.If(emulate, [&] {
            Block([&] {
                // Eleven of the sixteen ops read the destination (the feedback copy); the other five do not.
                const Cond reads_destination = e_.Not(
                    e_.Or(e_.Or(e_.Eq(op, 0), e_.Eq(op, 3)), e_.Or(e_.Eq(op, 4), e_.Or(e_.Eq(op, 5), e_.Eq(op, 6)))));
                e_.Raw("vec4 dest_color = vec4(0.0);\n");
                e_.If(reads_destination, [&] { e_.Raw("dest_color = texelFetch(tex_color, ivec2(gl_FragCoord.xy), 0);\n"); });
                e_.Raw(fmt::format("color = mh4n_logic_op({}, color, dest_color);\n", op.glsl));
            });
        });
    }

    DeviceProfile profile_;
    FragmentKey applied_;
    Emitter e_;
    EmitContext ctx_;
    const FeatureRegistry& features_;
};

std::string Build(Mode mode, const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
                  const FeatureRegistry& features) {
    Generator generator(mode, key, profile, pass, features);
    std::string body = generator.Run();
    // The output-stage functions of N2-13 go after the #extension lines (a #version and #extension must come first), at the
    // start of the declarations: insert them before the first "layout" of the interface.
    const std::string_view functions = fbops::OutputStageGlsl();
    const size_t at = body.find("layout (location = 1)");
    if (at != std::string::npos && !functions.empty()) {
        body.insert(at, std::string(functions) + "\n");
    }
    return body;
}

Generated Compile(std::string glsl) {
    Generated out;
    out.glsl = std::move(glsl);
    CompileFragmentGlslToSpirv(out.glsl, out.spirv, out.error, &out.compile_ms);
    if (!out.error.empty() && !out.spirv.empty()) {
        out.error.clear();
    }
    return out;
}

}  // namespace

std::string GenerateSpecialisedGlsl(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
                                    const FeatureRegistry& features) {
    return Build(Mode::Specialised, key, profile, pass, features);
}
std::string GenerateSpecialisedGlsl(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass) {
    return Build(Mode::Specialised, key, profile, pass, FeatureRegistry::Global());
}
std::string GenerateUberGlsl(ShaderPass pass, const DeviceProfile& profile, const FeatureRegistry& features) {
    return Build(Mode::Uber, FragmentKey{}, profile, pass, features);
}
std::string GenerateUberGlsl(ShaderPass pass, const DeviceProfile& profile) {
    return Build(Mode::Uber, FragmentKey{}, profile, pass, FeatureRegistry::Global());
}

Generated GenerateSpecialised(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
                              const FeatureRegistry& features) {
    return Compile(GenerateSpecialisedGlsl(key, profile, pass, features));
}
Generated GenerateSpecialised(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass) {
    return Compile(GenerateSpecialisedGlsl(key, profile, pass));
}
Generated GenerateUber(ShaderPass pass, const DeviceProfile& profile, const FeatureRegistry& features) {
    return Compile(GenerateUberGlsl(pass, profile, features));
}
Generated GenerateUber(ShaderPass pass, const DeviceProfile& profile) {
    return Compile(GenerateUberGlsl(pass, profile));
}

bool TevStageIsPassThrough(const TevStageConfigRaw& stage) {
    return stage.color_op() == regenum::TevOperation::Replace && stage.alpha_op() == regenum::TevOperation::Replace &&
           stage.color_source(0) == kSourcePrevious && stage.alpha_source(0) == kSourcePrevious &&
           Bits(stage.modifiers_raw, 0, 4) == 0 && Bits(stage.modifiers_raw, 12, 3) == 0 &&
           (stage.color_scale() == 0 || stage.color_scale() == 3) && (stage.alpha_scale() == 0 || stage.alpha_scale() == 3);
}

bool TevUsesSource(const FragmentKey& key, u32 source) {
    for (const TevStageConfigRaw& stage : key.texture.tev_stages) {
        if (TevStageIsPassThrough(stage)) {
            continue;
        }
        for (u32 i = 0; i < 3; ++i) {
            if (stage.color_source(i) == source) {
                return true;
            }
            if (stage.color_op() != regenum::TevOperation::Dot3_RGBA && stage.alpha_source(i) == source) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace mh4n::gpu::fragment
