// Derived from Azahar src/video_core/shader/generator/glsl_shader_gen.cpp, glsl_shader_gen.h,
// shader_gen.cpp, shader_gen.h, shader_uniforms.cpp, src/video_core/pica/shader_setup.cpp,
// output_vertex.cpp and rasterizer_accelerated.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; each of those files says "Licensed under GPLv2 or any later version"): the
// vertex shader around the decompiled program (GenerateVertexShader and its interface, the output
// map and semantic map of PicaVSConfigState / PicaGSConfigState), the contents of the two uniform
// blocks, and the primary colour clamp of OutputVertex.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_shader_gen.cpp,
//   shader_gen.cpp, shader_gen.h, rasterizer_accelerated.cpp, shader_setup.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (shader_uniforms.cpp)
// Copyright 2015 Citra Emulator Project (glsl_shader_gen.h)
// Copyright 2023 Citra Emulator Project (output_vertex.cpp)
//
// See vertex_shader.h for the interface this file generates.
#include "gpu/shader/vertex_shader.h"

#include <algorithm>
#include <span>

#include <fmt/format.h>

#include "gpu/pica/regs.h"
#include "gpu/shader/glsl_decompiler.h"

namespace mh4n::gpu::shader {

namespace {

constexpr u64 Mix(u64 z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

u64 Combine(u64 h, u64 v) {
    return Mix(h + 0x9E3779B97F4A7C15ull + v);
}

constexpr std::string_view kPicaUniformBlock = R"(layout (set = 0, binding = 0, std140) uniform vs_pica_data {
    uint b;
    uvec4 i[4];
    vec4 f[96];
} uniforms;
)";

constexpr std::string_view kVertexDataBlock = R"(layout (set = 0, binding = 1, std140) uniform vs_data {
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

std::string_view LoadPrefix(vertex::Conversion c) {
    switch (c) {
    case vertex::Conversion::FromSint:
        return "i";
    case vertex::Conversion::FromUint:
        return "u";
    case vertex::Conversion::None:
        break;
    }
    return "";
}

} // namespace

u64 HashWords(const u32* words, size_t count) {
    u64 h = Mix(count);
    for (size_t i = 0; i < count; ++i) {
        h = Combine(h, words[i]);
    }
    return h;
}

u64 VertexShaderConfig::Hash() const {
    u64 h = Combine(program_hash, swizzle_hash);
    h = Combine(h, (static_cast<u64>(program_words) << 32) | swizzle_words);
    h = Combine(h, (static_cast<u64>(main_offset) << 32) | output_mask);
    h = Combine(h, outmap_total);
    for (u32 word : outmap_attributes) {
        h = Combine(h, word);
    }
    for (const InputRegisterConfig& input : inputs) {
        h = Combine(h, static_cast<u64>(input.conversion) * 2 + (input.alias_w ? 1 : 0));
    }
    h = Combine(h, (use_clip_planes ? 1u : 0u) | (geometry_stage ? 2u : 0u) | (capture_outputs ? 4u : 0u));
    return h;
}

PicaUniformData MakePicaUniformData(const pica::ShaderSetup& setup) {
    PicaUniformData out;
    for (u32 j = 0; j < setup.uniform_b.size(); ++j) {
        out.b |= (setup.uniform_b[j] ? 1u : 0u) << j;
    }
    for (u32 j = 0; j < setup.uniform_i.size(); ++j) {
        for (u32 c = 0; c < 4; ++c) {
            out.i[j][c] = setup.uniform_i[j][c];
        }
    }
    for (u32 j = 0; j < setup.uniform_f.size(); ++j) {
        for (u32 c = 0; c < 4; ++c) {
            out.f[j][c] = setup.uniform_f[j].c[c];
        }
    }
    return out;
}

VertexUniformData MakeVertexUniformData(const pica::PicaState& state, bool flip_viewport) {
    VertexUniformData out;
    out.enable_clip1 = (state.regs[reg::kClipEnable] & 1u) != 0 ? 1u : 0u;
    out.flip_viewport = flip_viewport ? 1u : 0u;
    for (u32 c = 0; c < 4; ++c) {
        out.clip_coef[c] = pica::Float24ToFloat(state.regs[reg::kClipCoef + c] & 0xFFFFFFu);
    }
    return out;
}

VertexShaderConfig MakeVertexShaderConfig(const pica::PicaState& state, const vertex::LoadedDraw* loaded,
                                          const ConfigOptions& options) {
    VertexShaderConfig config;
    const pica::ShaderSetup& vs = state.vs;
    config.program_words = std::min(vs.biggest_program_size, kMaxProgramWords);
    config.swizzle_words = std::min(vs.biggest_swizzle_size, kMaxSwizzleWords);
    config.program_hash = HashWords(vs.program_code.data(), config.program_words);
    config.swizzle_hash = HashWords(vs.swizzle_data.data(), config.swizzle_words);
    config.main_offset = pica::BitsOf(state.regs[pica::reg::kVs + pica::reg::kShaderMainOffset], 0, 16);
    config.output_mask = pica::BitsOf(state.regs[pica::reg::kVs + pica::reg::kShaderOutputMask], 0, 16);
    config.outmap_total = pica::BitsOf(state.regs[reg::kVsOutputTotal], 0, 3);
    for (u32 i = 0; i < config.outmap_total; ++i) {
        config.outmap_attributes[i] = state.regs[reg::kVsOutputAttributes + i];
    }
    if (loaded != nullptr) {
        for (u32 r = 0; r < 16; ++r) {
            config.inputs[r].conversion = loaded->attributes[r].conversion;
            config.inputs[r].alias_w = loaded->attributes[r].alias_w;
        }
    }
    config.use_clip_planes = options.use_clip_planes;
    config.geometry_stage = options.geometry_stage;
    config.capture_outputs = options.capture_outputs;
    return config;
}

namespace {

// The output map and semantic map of a key (PicaVSConfigState::Init, PicaGSConfigState::GetSemanticMaps).
void MakeOutputMaps(const VertexShaderConfig& config, TranslatedVertexShader& out) {
    out.num_outputs = 0;
    out.output_map.fill(16);
    for (u32 r = 0; r < 16; ++r) {
        if ((config.output_mask >> r) & 1u) {
            out.output_map[r] = static_cast<u8>(out.num_outputs++);
        }
    }
    out.semantic_map.fill(SemanticSlot{16, 0});
    for (u32 attrib = 0; attrib < config.outmap_total && attrib < 7; ++attrib) {
        for (u32 comp = 0; comp < 4; ++comp) {
            const u32 s = (config.outmap_attributes[attrib] >> (8 * comp)) & 0x1F;
            if (s < semantic::kCount) {
                out.semantic_map[s] = SemanticSlot{static_cast<u8>(attrib), static_cast<u8>(comp)};
            }
        }
    }
}

} // namespace

TranslateResult GenerateVertexGlsl(const pica::ShaderSetup& setup, const VertexShaderConfig& config) {
    TranslateResult result;
    auto shader = std::make_shared<TranslatedVertexShader>();
    MakeOutputMaps(config, *shader);

    std::array<bool, 16> used_regs{};
    const auto get_input_reg = [&used_regs](u32 reg) {
        used_regs[reg & 15] = true;
        return fmt::format("vs_in_reg{}", reg & 15);
    };
    const auto get_output_reg = [&](u32 reg) -> std::string {
        if (reg < 16 && shader->output_map[reg] < shader->num_outputs) {
            return fmt::format("vs_out_attr{}", shader->output_map[reg]);
        }
        return "";
    };

    DecompileInput input;
    input.program = std::span<const u32>(setup.program_code.data(), std::min(config.program_words, kMaxProgramWords));
    input.swizzle = std::span<const u32>(setup.swizzle_data.data(), std::min(config.swizzle_words, kMaxSwizzleWords));
    input.main_offset = config.main_offset;
    DecompileResult program = DecompileProgram(input, get_input_reg, get_output_reg);
    if (!program.ok) {
        result.error = "vertex program not translated: " + program.error;
        return result;
    }
    shader->notes = std::move(program.notes);
    shader->uses_unverified_instructions = program.uses_unverified_instructions;
    for (u32 r = 0; r < 16; ++r) {
        if (used_regs[r]) {
            shader->used_inputs = static_cast<u16>(shader->used_inputs | (1u << r));
        }
    }

    std::string out = "#version 450\n\n";
    out += kPicaUniformBlock;
    out += kVertexDataBlock;
    if (config.capture_outputs) {
        out += fmt::format("layout (set = 0, binding = {}, std430) writeonly buffer vs_capture {{ vec4 capture_data[]; }};\n",
                           kBindingCapture);
    }
    out += '\n';

    // input attributes declaration
    for (u32 r = 0; r < 16; ++r) {
        if (used_regs[r]) {
            out += fmt::format("layout(location = {0}) in {1}vec4 vs_in_typed_reg{0};\n", r,
                               LoadPrefix(config.inputs[r].conversion));
            out += fmt::format("vec4 vs_in_reg{0};\n", r);
        }
    }
    out += '\n';

    const auto semantic_expr = [&](u32 s) -> std::string {
        const SemanticSlot slot = shader->semantic_map[s];
        if (slot.attribute < shader->num_outputs) {
            return fmt::format("vs_out_attr{}.{}", slot.attribute, "xyzw"[slot.component]);
        }
        return "1.0";
    };
    const auto vec = [&](std::initializer_list<u32> list) {
        std::string s;
        for (u32 id : list) {
            s += (s.empty() ? "" : ", ") + semantic_expr(id);
        }
        return s;
    };

    if (config.geometry_stage) {
        // output attributes declaration
        for (u32 i = 0; i < shader->num_outputs; ++i) {
            out += fmt::format("layout(location = {0}) out vec4 vs_out_attr{0};\n", i);
        }
        out += "void EmitVtx() {\n";
        if (config.capture_outputs) {
            out += "    uint cap = uint(gl_VertexIndex) * " + std::to_string(kCaptureVec4PerVertex) + "u;\n";
            for (u32 k = 0; k < kCaptureVec4PerVertex; ++k) {
                if (k < 16 && k < shader->num_outputs) {
                    out += fmt::format("    capture_data[cap + {0}u] = vs_out_attr{0};\n", k);
                } else {
                    out += fmt::format("    capture_data[cap + {}u] = vec4(0.0);\n", k);
                }
            }
        }
        out += "}\n";
    } else {
        out += fmt::format("layout(location = {}) out vec4 primary_color;\n", kLocationPrimaryColor);
        out += fmt::format("layout(location = {}) out vec2 texcoord0;\n", kLocationTexcoord0);
        out += fmt::format("layout(location = {}) out vec2 texcoord1;\n", kLocationTexcoord1);
        out += fmt::format("layout(location = {}) out vec2 texcoord2;\n", kLocationTexcoord2);
        out += fmt::format("layout(location = {}) out float texcoord0_w;\n", kLocationTexcoord0W);
        out += fmt::format("layout(location = {}) out vec4 normquat;\n", kLocationNormquat);
        out += fmt::format("layout(location = {}) out vec3 view;\n", kLocationView);

        // output attributes declaration
        for (u32 i = 0; i < shader->num_outputs; ++i) {
            out += fmt::format("vec4 vs_out_attr{};\n", i);
        }

        out += "vec4 GetVertexQuaternion() {\n";
        out += "    return vec4(" +
               vec({semantic::kQuaternionX, semantic::kQuaternionY, semantic::kQuaternionZ,
                    semantic::kQuaternionW}) +
               ");\n";
        out += "}\n\n";

        out += "void EmitVtx() {\n";
        out += "    vec4 vtx_pos = vec4(" +
               vec({semantic::kPositionX, semantic::kPositionY, semantic::kPositionZ, semantic::kPositionW}) +
               ");\n";
        out += "    vtx_pos = SanitizeVertex(vtx_pos);\n";
        out += "    if (flip_viewport != 0u) {\n";
        out += "        vtx_pos.y = -vtx_pos.y;\n";
        out += "    }\n";
        out += "    vec4 out_position = vec4(vtx_pos.x, vtx_pos.y, -vtx_pos.z, vtx_pos.w);\n";
        out += "    gl_Position = out_position;\n";
        out += "    vec2 out_clip = vec2(0.0);\n";
        if (config.use_clip_planes) {
            out += "    out_clip.x = -vtx_pos.z;\n"; // fixed PICA clipping plane z <= 0
            out += "    if (enable_clip1 != 0u) {\n";
            out += "        out_clip.y = dot(clip_coef, vtx_pos);\n";
            out += "    }\n";
            out += "    gl_ClipDistance[0] = out_clip.x;\n";
            out += "    gl_ClipDistance[1] = out_clip.y;\n";
        }
        out += "    normquat = GetVertexQuaternion();\n";
        out += "    vec4 vtx_color = abs(vec4(" +
               vec({semantic::kColorR, semantic::kColorG, semantic::kColorB, semantic::kColorA}) + "));\n";
        // The hardware takes the absolute and saturates (c < 1 ? c : 1: a NaN gives 1), as OutputVertex.
        out += "    primary_color = mix(vec4(1.0), vtx_color, lessThan(vtx_color, vec4(1.0)));\n";
        out += "    texcoord0 = vec2(" + vec({semantic::kTexcoord0U, semantic::kTexcoord0V}) + ");\n";
        out += "    texcoord1 = vec2(" + vec({semantic::kTexcoord1U, semantic::kTexcoord1V}) + ");\n";
        out += "    texcoord0_w = " + semantic_expr(semantic::kTexcoord0W) + ";\n";
        out += "    view = vec3(" + vec({semantic::kViewX, semantic::kViewY, semantic::kViewZ}) + ");\n";
        out += "    texcoord2 = vec2(" + vec({semantic::kTexcoord2U, semantic::kTexcoord2V}) + ");\n";
        if (config.capture_outputs) {
            out += "    uint cap = uint(gl_VertexIndex) * " + std::to_string(kCaptureVec4PerVertex) + "u;\n";
            for (u32 k = 0; k < 16; ++k) {
                if (k < shader->num_outputs) {
                    out += fmt::format("    capture_data[cap + {0}u] = vs_out_attr{0};\n", k);
                } else {
                    out += fmt::format("    capture_data[cap + {}u] = vec4(0.0);\n", k);
                }
            }
            out += fmt::format("    capture_data[cap + {}u] = primary_color;\n", kCapturePrimaryColor);
            out += fmt::format("    capture_data[cap + {}u] = vec4(texcoord0, texcoord1);\n", kCaptureTexcoord01);
            out += fmt::format("    capture_data[cap + {}u] = vec4(texcoord2, texcoord0_w, 0.0);\n", kCaptureTexcoord2W);
            out += fmt::format("    capture_data[cap + {}u] = normquat;\n", kCaptureNormquat);
            out += fmt::format("    capture_data[cap + {}u] = vec4(view, 0.0);\n", kCaptureView);
            out += fmt::format("    capture_data[cap + {}u] = out_position;\n", kCapturePosition);
            out += fmt::format("    capture_data[cap + {}u] = vec4(out_clip, 0.0, 0.0);\n", kCaptureClip);
            out += fmt::format("    capture_data[cap + {}u] = vec4(0.0);\n", kCaptureClip + 1);
        }
        out += "}\n";
    }

    out += "bool exec_shader();\n\n";

    out += "\nvoid main() {\n";
    for (u32 r = 0; r < 16; ++r) {
        if (used_regs[r]) {
            out += fmt::format("    vs_in_reg{0} = vec4(vs_in_typed_reg{0});\n", r);
            if (config.inputs[r].alias_w) {
                // The 3-component integer attribute is read as 4 components; lane 3 is not data.
                out += fmt::format("    vs_in_reg{0}.w = 1.0;\n", r);
            }
        }
    }
    for (u32 i = 0; i < shader->num_outputs; ++i) {
        out += fmt::format("    vs_out_attr{} = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
    }
    out += "\n    exec_shader();\n    EmitVtx();\n";
    if (config.capture_outputs) {
        // the tests draw points (every vertex is shaded and no primitive is needed): Vulkan wants the size
        out += "    gl_PointSize = 1.0;\n";
    }
    out += "}\n\n";

    out += program.source;

    shader->glsl = std::move(out);
    result.shader = std::move(shader);
    return result;
}

TranslateResult TranslateVertexShader(const pica::ShaderSetup& setup, const VertexShaderConfig& config) {
    TranslateResult generated = GenerateVertexGlsl(setup, config);
    if (!generated.ok()) {
        return generated;
    }
    // The shader was made here and nobody else holds it yet.
    auto shader = std::const_pointer_cast<TranslatedVertexShader>(generated.shader);
    std::string error;
    if (!CompileGlslToSpirv(shader->glsl, shader->spirv, error, &shader->compile_ms)) {
        TranslateResult failed;
        failed.error = "vertex shader does not compile: " + error;
        return failed;
    }
    return generated;
}

TranslateResult VertexShaderCache::Get(const pica::ShaderSetup& setup, const VertexShaderConfig& config) {
    {
        std::lock_guard lock(mutex_);
        ++stats_.lookups;
        if (auto it = entries_.find(config); it != entries_.end()) {
            ++stats_.hits;
            return it->second;
        }
    }
    TranslateResult result = TranslateVertexShader(setup, config);
    std::lock_guard lock(mutex_);
    ++stats_.compiles;
    if (!result.ok()) {
        ++stats_.failures;
    }
    // A second thread may have inserted the same key meanwhile: keep that one.
    return entries_.try_emplace(config, std::move(result)).first->second;
}

VertexShaderCache::Stats VertexShaderCache::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

size_t VertexShaderCache::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

void VertexShaderCache::Clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

} // namespace mh4n::gpu::shader
