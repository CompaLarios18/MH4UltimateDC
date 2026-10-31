// Derived from Azahar src/video_core/pica/pica_core.cpp (DrawArrays, LoadVertices, DrawImmediate),
// pica_core.h, vertex_loader.cpp, vertex_loader.h, output_vertex.h, output_vertex.cpp, shader_unit.h,
// shader_unit.cpp, shader_setup.cpp, primitive_assembly.cpp, regs_pipeline.h, regs_rasterizer.h and
// regs_shader.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those
// files says "Licensed under GPLv2 or any later version"): the accelerate_draw rule, the vertex
// loop of LoadVertices with its 64-entry vertex cache, DrawImmediate, ShaderUnit::LoadInput on the
// loader's output, the OutputVertex constructor and the order in which the pipeline, the
// shader units and the primitive assembler are fed.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp, pica_core.h,
//   shader_unit.h, shader_unit.cpp, shader_setup.cpp)
// Copyright 2023 Citra Emulator Project (vertex_loader.cpp, vertex_loader.h, output_vertex.cpp)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (output_vertex.h)
// Copyright 2014 Citra Emulator Project (primitive_assembly.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_pipeline.h, regs_shader.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
#include "gpu/cpuvertex/cpu_vertex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "gpu/cpuvertex/geometry_pipeline.h"
#include "gpu/cpuvertex/shader_interpreter.h"
#include "gpu/pica/regs.h"
#include "mh4n/log.h"

namespace mh4n::gpu::cpuvertex {

using pica::BitsOf;
namespace reg = pica::reg;

namespace {

constexpr const char* kLog = "CpuVertex";

// RasterizerRegs: vs_output_total at 0x4F, vs_output_attributes at 0x50 (regs_internal.h).
constexpr u32 kRegVsOutputTotal = 0x4F;
constexpr u32 kRegVsOutputAttributes = 0x50;

// The loop's vertex cache (LoadVertices: VERTEX_CACHE_SIZE).
constexpr std::size_t kVertexCacheSize = 64;

Vec4 ToVec4(const pica::Vec4f& v) {
    Vec4 r;
    for (int i = 0; i < 4; ++i) {
        r.c[i] = f24::FromFloat32(v.c[i]);
    }
    return r;
}

// The vertex shader's ShaderRegs, as LoadInput and WriteOutput read them.
ShaderInputConfig VsInputConfig(const std::array<u32, pica::kNumRegs>& regs) {
    ShaderInputConfig config;
    config.max_input_attribute_index = BitsOf(regs[reg::kVs + reg::kShaderInputBufferConfig], 0, 4);
    config.input_attribute_to_register_map =
        (static_cast<u64>(regs[reg::kVs + 0x0C]) << 32) | regs[reg::kVs + 0x0B];
    return config;
}

// The caps that make the loader return the PICA's own layout untouched: fans and 8-bit indices
// native, stride alignment 1, no restart index (so the order and the indexing are the draw's).
// Spelled out because the runner depends on each of them.
vertex::VertexCaps PicaLayoutCaps() {
    vertex::VertexCaps caps;
    caps.triangle_fans = true;
    caps.index_uint8 = true;
    caps.apple = false;
    caps.stride_alignment = 1;
    caps.primitive_restart_always_on = false;
    return caps;
}

u64 Fnv1a(const u32* words, std::size_t count, u64 hash) {
    for (std::size_t i = 0; i < count; ++i) {
        for (int b = 0; b < 4; ++b) {
            hash ^= (words[i] >> (8 * b)) & 0xFF;
            hash *= 0x100000001B3ull;
        }
    }
    return hash;
}

const char* UnhandledText(Unhandled kind) {
    switch (kind) {
    case Unhandled::None: return "none";
    case Unhandled::Instruction: return "an instruction the interpreter has no handler for";
    case Unhandled::CompareMode: return "a CMP with an unknown compare mode";
    case Unhandled::EmitOnVertexShader: return "EMIT or SETEMIT in the vertex shader";
    case Unhandled::EmitVertexId: return "EMIT with vertex id 3";
    case Unhandled::EntryPoint: return "an entry point outside the program memory";
    case Unhandled::InstructionBudget: return "a shader that does not end";
    case Unhandled::GeometryConfig: return "an inconsistent geometry pipeline set-up";
    case Unhandled::GeometryNeedsIndex: return "a variable-primitive geometry shader without an index buffer";
    case Unhandled::GeometryOverflow: return "more vertices than the geometry shader's inputs hold";
    case Unhandled::LoaderBuffer: return "a vertex the loader did not provide";
    }
    return "?";
}

} // namespace

const char* ToString(CpuReason reason) {
    switch (reason) {
    case CpuReason::None: return "accelerated";
    case CpuReason::GeometryShader: return "geometry shader";
    case CpuReason::Immediate: return "immediate";
    case CpuReason::Refused: return "refused by accelerate_draw";
    }
    return "?";
}

CpuReason CpuPathReason(const pica::DrawCall& draw, bool assembler_empty) {
    const auto& regs = draw.state->regs;
    // Geometry shaders cannot be accelerated due to register preservation.
    if (BitsOf(regs[reg::kGeometryStage], 0, 2) == pica::kUseGsYes) {
        return CpuReason::GeometryShader;
    }
    // DrawImmediate always runs the vertex shader on the CPU.
    if (draw.kind == pica::DrawKind::Immediate) {
        return CpuReason::Immediate;
    }

    // TODO (wwylele, in Azahar): for Strip/Fan topology, if the primitive assember is not restarted
    // after this draw call, the buffered vertex from this draw should "leak" to the next draw ...
    bool accelerate_draw = assembler_empty; // Settings::values.use_hw_shader && IsEmpty()
    const u32 topology = BitsOf(regs[reg::kTriangleTopology], 8, 2);
    if (topology == pica::kTopologyShader || topology == pica::kTopologyList) {
        accelerate_draw = accelerate_draw && (draw.num_vertices % 3) == 0;
    }
    return accelerate_draw ? CpuReason::None : CpuReason::Refused;
}

// ---- the runner -------------------------------------------------------------------------------------------

struct CpuVertexRunner::Impl final : public VertexSink {
    Impl() : loader(PicaLayoutCaps()) {
        gs_unit.SetVertexSink(this);
        pipeline.SetVertexSink(this);
    }

    // ---- VertexSink: the geometry pipeline's and the emitter's handlers (PicaCore's submit_vertex) -------
    void SubmitVertex(const AttributeBuffer& buffer) override {
        // OutputVertex::OutputVertex(const RasterizerRegs&, const AttributeBuffer&)
        // Attributes can be used without being set in GPUREG_SH_OUTMAP_Oi.
        // Hardware tests have shown that they are initialized to 1 in this case.
        std::array<float, 32> vertex_slots_overflow;
        vertex_slots_overflow.fill(1.0f);

        for (u32 attrib = 0; attrib < output_attributes; ++attrib) {
            const u32 output_register_map = output_map[attrib];
            vertex_slots_overflow[BitsOf(output_register_map, 0, 5)] = buffer[attrib].c[0].value;
            vertex_slots_overflow[BitsOf(output_register_map, 8, 5)] = buffer[attrib].c[1].value;
            vertex_slots_overflow[BitsOf(output_register_map, 16, 5)] = buffer[attrib].c[2].value;
            vertex_slots_overflow[BitsOf(output_register_map, 24, 5)] = buffer[attrib].c[3].value;
        }

        // Copy to result
        OutputVertex vertex;
        std::memcpy(&vertex, vertex_slots_overflow.data(), sizeof(OutputVertex));

        // The hardware takes the absolute and saturates vertex colors, *before* doing interpolation
        for (u32 i = 0; i < 4; ++i) {
            const float c = std::fabs(vertex.color[i]);
            vertex.color[i] = c < 1.0f ? c : 1.0f;
        }
        out.vertices.push_back(vertex);
    }

    void SetWinding() override {
        out.winding_before.push_back(static_cast<u32>(out.vertices.size()));
    }

    // ---- state -----------------------------------------------------------------------------------------------------
    vertex::VertexLoader loader;
    InterpreterEngine engine;
    GeometryShaderUnit gs_unit;
    Uniforms gs_uniforms; // the geometry shader's uniforms, with what the pipeline changed in them
    Uniforms vs_uniforms; // a copy of the vertex shader's, made at every Run
    GeometryPipeline pipeline{gs_unit, gs_uniforms};
    OutputStream out;
    Counters counters;
    std::string last_error;
    std::unordered_set<u64> logged;

    bool configured = false; // the pipeline has been reconfigured (or adopted the INIT state)

    // The geometry unit's uniforms as the command processor last showed them: the pipeline and the
    // game write the same storage in Azahar, here the game's writes arrive through the PicaState.
    bool gs_synced = false;
    std::array<pica::Vec4f, 96> shadow_f{};
    std::array<bool, 16> shadow_b{};
    std::array<std::array<u8, 4>, 4> shadow_i{};

    // The rasterizer registers that place the shader outputs in an OutputVertex, read at Run.
    u32 output_attributes = 0;
    std::array<u32, 7> output_map{};

    // LoadVertices' state
    AttributeBuffer vs_output{};
    std::array<bool, kVertexCacheSize> vertex_cache_valid{};
    std::array<u16, kVertexCacheSize> vertex_cache_ids{};
    std::array<AttributeBuffer, kVertexCacheSize> vertex_cache{};

    // ---- the pieces of a run --------------------------------------------------------------------------------------
    void ReadOutputMap(const pica::PicaState& s);
    void CopyVsUniforms(const pica::PicaState& s);
    void SyncGsUniforms(const pica::PicaState& s);
    bool ConfigurePipeline(const pica::DrawCall& draw, const pica::PicaState& s, RunMode mode);
    RunStatus Fail(const pica::PicaState& s, RunError error, int unit);
    RunStatus RunImmediate(const pica::DrawCall& draw, const pica::PicaState& s, RunMode mode);
    RunStatus RunLoadVertices(gpu::Host& host, const pica::DrawCall& draw, const pica::PicaState& s,
                              RunMode mode);
    RunStatus Run(gpu::Host& host, const pica::DrawCall& draw, RunMode mode);
};

void CpuVertexRunner::Impl::ReadOutputMap(const pica::PicaState& s) {
    output_attributes = BitsOf(s.regs[kRegVsOutputTotal], 0, 3);
    for (u32 i = 0; i < output_map.size(); ++i) {
        output_map[i] = s.regs[kRegVsOutputAttributes + i];
    }
}

void CpuVertexRunner::Impl::CopyVsUniforms(const pica::PicaState& s) {
    for (std::size_t i = 0; i < vs_uniforms.f.size(); ++i) {
        vs_uniforms.f[i] = ToVec4(s.vs.uniform_f[i]);
    }
    vs_uniforms.b = s.vs.uniform_b;
    vs_uniforms.i = s.vs.uniform_i;
}

// The geometry shader's uniforms. In Azahar the game's uniform writes and the geometry pipeline act on
// the one `gs_setup`; here the game's writes land in the PicaState, so the runner brings them over
// before it uses the uniforms:
//  - every bool uniform the register was written for (the dirty flag of the register is set by
//    every write, also of the same value, which resets b15 to what the game wrote), and every bool
//    whose value differs from the one seen last;
//  - the same for each integer uniform register;
//  - all float uniforms if any float-uniform data register was written (the float uniforms the
//    pipeline buffered vertices in are then replaced by the game's), else those that differ from
//    the value seen last (a write of another value; this also covers writes the dirty flags of a
//    draw this runner did not see have lost).
// Everything the game has not rewritten keeps the pipeline's changes, as in Azahar.
void CpuVertexRunner::Impl::SyncGsUniforms(const pica::PicaState& s) {
    const pica::ShaderSetup& g = s.gs;
    const bool first = !gs_synced;

    const bool bools_written = first || s.dirty.Test(reg::kGs + reg::kShaderBoolUniforms);
    for (std::size_t i = 0; i < gs_uniforms.b.size(); ++i) {
        if (bools_written || g.uniform_b[i] != shadow_b[i]) {
            gs_uniforms.b[i] = g.uniform_b[i];
        }
        shadow_b[i] = g.uniform_b[i];
    }

    for (u32 k = 0; k < 4; ++k) {
        const bool written = first || s.dirty.Test(reg::kGs + reg::kShaderIntUniforms + k);
        if (written || g.uniform_i[k] != shadow_i[k]) {
            gs_uniforms.i[k] = g.uniform_i[k];
        }
        shadow_i[k] = g.uniform_i[k];
    }

    bool floats_written = first;
    for (u32 k = 0; k < 8 && !floats_written; ++k) {
        floats_written = s.dirty.Test(reg::kGs + reg::kShaderUniformData + k);
    }
    for (std::size_t i = 0; i < gs_uniforms.f.size(); ++i) {
        if (floats_written || !(g.uniform_f[i] == shadow_f[i])) {
            gs_uniforms.f[i] = ToVec4(g.uniform_f[i]);
        }
        shadow_f[i] = g.uniform_f[i];
    }
    gs_synced = true;
}

// geometry_pipeline.Reconfigure() where Azahar calls it: at every Arrays/Indexed draw and at the
// first immediate vertex after a write to 0x232, which is what `geometry_reconfigured` says. A
// runner that has never been configured (a capture that starts inside a batch) takes the state of
// the INIT record instead.
bool CpuVertexRunner::Impl::ConfigurePipeline(const pica::DrawCall& draw, const pica::PicaState& s,
                                              RunMode mode) {
    (void)s;
    if (mode != RunMode::Full) {
        return true;
    }
    if (!draw.geometry_reconfigured && configured) {
        return true;
    }
    const bool adopt = !draw.geometry_reconfigured;
    configured = pipeline.Reconfigure(adopt);
    if (pipeline.discarded_partial_input() && logged.insert(1).second) {
        MH4N_LOG(Warning, kLog,
                 "the geometry pipeline was reconfigured with a partial input in it (Azahar asserts); "
                 "the partial input is discarded");
    }
    return configured;
}

// Records the failure, logs it once per program, and drops the draw. `unit`: 0 the vertex shader,
// 1 the geometry shader, 2 none (the failure is not in a program).
RunStatus CpuVertexRunner::Impl::Fail(const pica::PicaState& s, RunError error, int unit) {
    ++counters.unhandled;
    out.vertices.clear();
    out.winding_before.clear();
    // The pipeline may be anywhere: the next draw that is not a reconfiguration starts from the
    // INIT state again.
    configured = false;
    pipeline.Reset();
    if (unit != 0) {
        gs_unit.Reset();
    }

    last_error = UnhandledText(error.kind);
    if (unit == 0 || unit == 1) {
        const std::array<u32, pica::kMaxProgramWords>& code = unit == 0 ? s.vs.program_code : s.gs.program_code;
        const u64 key = Fnv1a(code.data(), code.size(), 0xCBF29CE484222325ull) ^
                        (static_cast<u64>(error.kind) << 56) ^ (static_cast<u64>(unit) << 62);
        const u32 opcode = error.instruction >> 26;
        last_error += " (" + std::string(unit == 0 ? "vertex" : "geometry") + " shader, word " +
                      std::to_string(error.program_counter) + ", opcode " + std::to_string(opcode) + ")";
        if (logged.insert(key).second) {
            MH4N_LOG(Error, kLog, "draw dropped: {} ({} shader, word {}, opcode {:#x})",
                     UnhandledText(error.kind), unit == 0 ? "vertex" : "geometry", error.program_counter,
                     opcode);
        }
    } else {
        const u64 key = 0x100 + static_cast<u64>(error.kind);
        if (logged.insert(key).second) {
            MH4N_LOG(Error, kLog, "draw dropped: {}", UnhandledText(error.kind));
        }
    }
    return RunStatus::Unhandled;
}

// PicaCore::DrawImmediate
RunStatus CpuVertexRunner::Impl::RunImmediate(const pica::DrawCall& draw, const pica::PicaState& s,
                                              RunMode mode) {
    ShaderUnit shader_unit;
    AttributeBuffer output{};

    AttributeBuffer input{};
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = ToVec4(s.immediate.input_vertex[i]);
    }

    // Invoke the vertex shader for the vertex.
    ShaderSetup setup;
    setup.program_code = s.vs.program_code.data();
    setup.swizzle_data = s.vs.swizzle_data.data();
    setup.uniforms = &vs_uniforms;
    setup.entry_point = BitsOf(s.regs[reg::kVs + reg::kShaderMainOffset], 0, 16);
    shader_unit.LoadInput(VsInputConfig(s.regs), input);
    RunError error;
    ++counters.vertices;
    ++counters.vs_invocations;
    if (!engine.Run(setup, shader_unit, error)) {
        return Fail(s, error, 0);
    }
    shader_unit.WriteOutput(BitsOf(s.regs[reg::kVs + reg::kShaderOutputMask], 0, 16), output);

    if (mode == RunMode::VertexOnly) {
        SubmitVertex(output);
        return RunStatus::Ok;
    }

    // Reconfigure geometry pipeline if needed.
    if (!ConfigurePipeline(draw, s, mode)) {
        return Fail(s, pipeline.error(), 2);
    }

    // Send to geometry pipeline.
    if (pipeline.NeedIndexInput()) { // Azahar: ASSERT(!geometry_pipeline.NeedIndexInput())
        return Fail(s, {Unhandled::GeometryNeedsIndex, 0, 0}, 2);
    }
    if (!pipeline.SubmitVertex(output)) {
        const RunError& e = pipeline.error();
        const bool in_program = e.kind != Unhandled::GeometryOverflow && e.kind != Unhandled::GeometryConfig;
        return Fail(s, e, in_program ? 1 : 2);
    }
    return RunStatus::Ok;
}

// PicaCore::LoadVertices
RunStatus CpuVertexRunner::Impl::RunLoadVertices(gpu::Host& host, const pica::DrawCall& draw,
                                                 const pica::PicaState& s, RunMode mode) {
    const bool is_indexed = draw.kind == pica::DrawKind::Indexed;

    // Read and validate vertex information from the loaders (N2-05).
    vertex::LoadedDraw loaded = loader.Load(host, draw);
    if (loaded.status != vertex::LoadStatus::Ok && loaded.status != vertex::LoadStatus::NoVertices) {
        // Azahar returns early only when the index address is unmapped; its loader would crash on
        // an unmapped vertex address.
        ++counters.load_failed;
        last_error = std::string("the vertices could not be loaded: ") + vertex::ToString(loaded.status);
        if (logged.insert(0x200 + static_cast<u64>(loaded.status)).second) {
            MH4N_LOG(Warning, kLog, "draw dropped: {}", last_error);
        }
        return RunStatus::LoadFailed;
    }

    // Setup geometry pipeline in case we are using a geometry shader.
    if (!ConfigurePipeline(draw, s, mode)) {
        return Fail(s, pipeline.error(), 2);
    }
    if (mode == RunMode::Full && pipeline.NeedIndexInput() && !is_indexed) {
        // Azahar: ASSERT(!geometry_pipeline.NeedIndexInput() || is_indexed)
        return Fail(s, {Unhandled::GeometryNeedsIndex, 0, 0}, 2);
    }
    if (loaded.status == vertex::LoadStatus::NoVertices) {
        return RunStatus::Ok;
    }

    // Compile the vertex shader for this batch.
    ShaderUnit shader_unit;
    vs_output = {};
    ShaderSetup setup;
    setup.program_code = s.vs.program_code.data();
    setup.swizzle_data = s.vs.swizzle_data.data();
    setup.uniforms = &vs_uniforms;
    setup.entry_point = BitsOf(s.regs[reg::kVs + reg::kShaderMainOffset], 0, 16);
    const ShaderInputConfig vs_config = VsInputConfig(s.regs);
    const u32 vs_output_mask = BitsOf(s.regs[reg::kVs + reg::kShaderOutputMask], 0, 16);

    // The loader describes the inputs by shader input register (vertex_loader.h). Which registers the
    // draw's attributes feed (ShaderUnit::LoadInput): those of attributes 0 ... max_input_attribute_index.
    // The others keep what the unit has in them (0), where the loader's description has (0, 0, 0, 1).
    struct Slot {
        bool targeted = false;
        bool constant = false; // the loader's constant buffer: the same value for every vertex
        const u8* data = nullptr;
        u32 stride = 0;
        u32 offset = 0;
        u32 count = 0;      // vertices in the buffer
        u32 components = 4;
        vertex::ComponentKind kind = vertex::ComponentKind::F32;
    };
    std::array<Slot, 16> slots{};
    for (u32 attr = 0; attr <= vs_config.max_input_attribute_index; ++attr) {
        slots[vs_config.GetRegisterForAttribute(attr)].targeted = true;
    }
    for (u32 r = 0; r < 16; ++r) {
        if (!slots[r].targeted) {
            continue;
        }
        const vertex::VertexAttribute& va = loaded.attributes[r];
        const vertex::VertexBuffer& buffer = loaded.buffers[va.binding];
        Slot& slot = slots[r];
        slot.constant = buffer.per_instance;
        slot.data = buffer.data.data();
        slot.stride = buffer.stride;
        slot.offset = va.offset;
        slot.count = buffer.vertex_count;
        slot.components = va.components; // 3 for the aliased 4-lane integer formats
        slot.kind = vertex::GetFormatInfo(va.format).kind;
    }

    const auto read_index = [&loaded](u32 i) -> u32 {
        switch (loaded.index_type) {
        case vertex::IndexType::U8:
            return loaded.index_data[i];
        case vertex::IndexType::U16:
            return static_cast<u32>(loaded.index_data[2 * i]) | (static_cast<u32>(loaded.index_data[2 * i + 1]) << 8);
        case vertex::IndexType::U32:
            return static_cast<u32>(loaded.index_data[4 * i]) | (static_cast<u32>(loaded.index_data[4 * i + 1]) << 8) |
                   (static_cast<u32>(loaded.index_data[4 * i + 2]) << 16) |
                   (static_cast<u32>(loaded.index_data[4 * i + 3]) << 24);
        case vertex::IndexType::None:
            break;
        }
        return 0;
    };

    // Simple circular-replacement vertex cache
    vertex_cache_valid.fill(false);
    u32 vertex_cache_pos = 0;

    std::vector<OutputVertex>& vertices = out.vertices;
    // (reserve a draw's worth, at most 64K entries up front: a geometry shader can emit more)
    vertices.reserve(std::max<std::size_t>(vertices.capacity(), std::min<std::size_t>(draw.num_vertices, 65536)));

    for (u32 index = 0; index < draw.num_vertices; ++index) {
        // Indexed rendering doesn't use the start offset
        const u32 vertex = is_indexed ? read_index(index) : (index + draw.vertex_offset);

        bool vertex_cache_hit = false;
        if (is_indexed) {
            if (mode == RunMode::Full && pipeline.NeedIndexInput()) {
                if (!pipeline.SubmitIndex(vertex)) {
                    return Fail(s, {Unhandled::GeometryConfig, 0, 0}, 2);
                }
                continue;
            }

            for (std::size_t i = 0; i < kVertexCacheSize; ++i) {
                if (vertex_cache_valid[i] && vertex == vertex_cache_ids[i]) {
                    vs_output = vertex_cache[i];
                    vertex_cache_hit = true;
                    break;
                }
            }
        }

        ++counters.vertices;
        if (!vertex_cache_hit) {
            // Initialize data for the current vertex: LoadVertex's attributes, placed by LoadInput.
            const s64 buffer_vertex = static_cast<s64>(vertex) - static_cast<s64>(loaded.first_vertex);
            for (u32 r = 0; r < 16; ++r) {
                const Slot& slot = slots[r];
                if (!slot.targeted) {
                    continue;
                }
                if (!slot.constant && (buffer_vertex < 0 || buffer_vertex >= static_cast<s64>(slot.count))) {
                    return Fail(s, {Unhandled::LoaderBuffer, 0, 0}, 2);
                }
                const u8* p = slot.data + slot.offset +
                              (slot.constant ? 0 : static_cast<std::size_t>(buffer_vertex) * slot.stride);
                Vec4& in = shader_unit.input[r];
                for (u32 comp = 0; comp < slot.components; ++comp) {
                    float value;
                    switch (slot.kind) {
                    case vertex::ComponentKind::S8:
                        value = static_cast<float>(static_cast<s8>(p[comp]));
                        break;
                    case vertex::ComponentKind::U8:
                        value = static_cast<float>(p[comp]);
                        break;
                    case vertex::ComponentKind::S16:
                        value = static_cast<float>(static_cast<s16>(static_cast<u16>(p[2 * comp] | (p[2 * comp + 1] << 8))));
                        break;
                    default: // F32
                        std::memcpy(&value, p + 4 * comp, 4);
                        break;
                    }
                    in.c[comp] = f24::FromFloat32(value);
                }
                // Default attribute values set if array elements have < 4 components. This is *not*
                // carried over from the default attribute settings even if they're enabled for this
                // attribute.
                for (u32 comp = slot.components; comp < 4; ++comp) {
                    in.c[comp] = comp == 3 ? f24::One() : f24::Zero();
                }
            }

            // Invoke the vertex shader for this vertex.
            ++counters.vs_invocations;
            RunError error;
            if (!engine.Run(setup, shader_unit, error)) {
                return Fail(s, error, 0);
            }
            shader_unit.WriteOutput(vs_output_mask, vs_output);

            // Cache the vertex when doing indexed rendering.
            if (is_indexed) {
                vertex_cache[vertex_cache_pos] = vs_output;
                vertex_cache_valid[vertex_cache_pos] = true;
                vertex_cache_ids[vertex_cache_pos] = static_cast<u16>(vertex);
                vertex_cache_pos = (vertex_cache_pos + 1) % kVertexCacheSize;
            }
        }

        // Send to geometry pipeline
        if (mode == RunMode::VertexOnly) {
            SubmitVertex(vs_output);
        } else if (!pipeline.SubmitVertex(vs_output)) {
            const RunError& e = pipeline.error();
            const bool in_program = e.kind != Unhandled::GeometryOverflow && e.kind != Unhandled::GeometryConfig;
            return Fail(s, e, in_program ? 1 : 2);
        }
    }
    return RunStatus::Ok;
}

RunStatus CpuVertexRunner::Impl::Run(gpu::Host& host, const pica::DrawCall& draw, RunMode mode) {
    out.vertices.clear();
    out.winding_before.clear();
    last_error.clear();
    ++counters.draws;

    const pica::PicaState& s = *draw.state;
    ReadOutputMap(s);
    CopyVsUniforms(s);
    pipeline.SetState(s, &engine);
    if (mode == RunMode::Full) {
        SyncGsUniforms(s);
    }

    const RunStatus status = draw.kind == pica::DrawKind::Immediate ? RunImmediate(draw, s, mode)
                                                                    : RunLoadVertices(host, draw, s, mode);
    counters.gs_invocations = pipeline.invocations();
    if (status != RunStatus::Ok) {
        out.vertices.clear();
        out.winding_before.clear();
    } else {
        counters.vertices_out += out.vertices.size();
    }
    return status;
}

CpuVertexRunner::CpuVertexRunner() : impl_(std::make_unique<Impl>()) {}
CpuVertexRunner::~CpuVertexRunner() = default;

RunStatus CpuVertexRunner::Run(gpu::Host& host, const pica::DrawCall& draw, RunMode mode) {
    return impl_->Run(host, draw, mode);
}

void CpuVertexRunner::NoteDraw(const pica::DrawCall& draw) {
    impl_->SyncGsUniforms(*draw.state);
}

void CpuVertexRunner::Reset() {
    Impl& m = *impl_;
    m.gs_unit.Reset();
    m.gs_uniforms = Uniforms{};
    m.configured = false;
    m.pipeline.Reset();
    m.gs_synced = false;
    m.out.vertices.clear();
    m.out.winding_before.clear();
    m.last_error.clear();
}

const OutputStream& CpuVertexRunner::output() const {
    return impl_->out;
}

const Counters& CpuVertexRunner::counters() const {
    return impl_->counters;
}

const std::string& CpuVertexRunner::last_error() const {
    return impl_->last_error;
}

const vertex::VertexCounters& CpuVertexRunner::loader_counters() const {
    return impl_->loader.counters();
}

} // namespace mh4n::gpu::cpuvertex
