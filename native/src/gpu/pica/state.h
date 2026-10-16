// Derived from Azahar src/video_core/pica/pica_core.h, pica_core.cpp, shader_setup.h,
// shader_setup.cpp, packed_attribute.h, geometry_pipeline.cpp, dirty_regs.h,
// src/video_core/pica_types.h and
// src/video_core/shader/shader.h, shader_interpreter.cpp, shader_jit.cpp (read for the entry-point
// rule of ShaderEngine::SetupBatch only) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version"). src/common/vector_math.h was read for the
// semantics of Vec4::operator== (a memcmp); no code of it is used. What is derived: the state
// the command processor keeps (the shader setups, the look-up tables, the default attributes,
// the immediate-mode state, the geometry pipeline), the uniform and program upload rules, the
// packed attribute queue, the float24 decoding and the GPU-time estimate for interrupts.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, pica_core.cpp,
//   shader_setup.h, shader_setup.cpp, packed_attribute.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (geometry_pipeline.cpp)
// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project (dirty_regs.h)
// Copyright 2015 Citra Emulator Project (shader.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (shader_interpreter.cpp)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (shader_jit.cpp)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (vector_math.h)
// Copyright 2014 Tony Wasserka (vector_math.h, second notice block)
//
// The PICA state behind the register file (N2-04). Plain data: a Backend that wants to keep a
// draw's state copies the PicaState (about 105 KB).
#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include "gpu/pica/regs.h"
#include "mh4n/types.h"

namespace mh4n::gpu::pica {

// One float24 value is held as a float, like Azahar's Pica::Float (the INIT record stores them
// the same way). Comparison is bitwise, as Azahar's Vec4::operator== (memcmp) is: its "did this
// uniform change" test is prev != uniform, so +0 and -0 differ and a NaN equals itself.
struct Vec4f {
    float c[4]{}; // x, y, z, w

    friend bool operator==(const Vec4f& a, const Vec4f& b) {
        return std::memcmp(a.c, b.c, sizeof(a.c)) == 0;
    }
};

// Pica::Float<16, 7>::FromRaw: a 24-bit PICA float (1 sign, 7 exponent, 16 mantissa bits) to a
// float. Denormals are renormalised so the value is exact.
float Float24ToFloat(u32 raw24);

// Uniforms and fixed attributes arrive packed: four float24 values in three words, or (for
// uniforms in float32 mode) four float32 values in four words.
struct PackedAttribute {
    std::array<u32, 4> buffer{};
    u32 index = 0;

    // True when the queue is full. (The index is kept below 4 so a damaged state cannot write
    // outside the buffer.)
    bool Push(u32 word, bool is_float32 = false);
    void Reset() {
        index = 0;
    }
    // Decodes the queue and resets it. Float24 mode: x is buffer[2] & 0xFFFFFF, y is
    // ((buffer[1] & 0xFFFF) << 8) | (buffer[2] >> 24), z is ((buffer[0] & 0xFF) << 16) |
    // (buffer[1] >> 16) and w is buffer[0] >> 8. Float32 mode: component 3 - i is buffer[i].
    Vec4f Get(bool is_float32 = false);
};

inline constexpr u32 kMaxProgramWords = 4096;
inline constexpr u32 kMaxSwizzleWords = 4096;
inline constexpr u32 kNumFloatUniforms = 96;

// ShaderSetup: a shader unit configuration (the vertex shader's, or the geometry shader's).
struct ShaderSetup {
    std::array<Vec4f, kNumFloatUniforms> uniform_f{};
    std::array<bool, 16> uniform_b{};
    std::array<std::array<u8, 4>, 4> uniform_i{}; // x, y, z, w
    PackedAttribute uniform_queue;
    // The entry point the shader starts at: Azahar's DrawImmediate and LoadVertices (which
    // DrawArrays calls unless the draw is accelerated) pass the vertex unit's main_offset register
    // to ShaderEngine::SetupBatch, GeometryPipeline::Setup passes the geometry unit's, and both
    // engines store it.
    u32 entry_point = 0;
    std::array<u32, kMaxProgramWords> program_code{};
    std::array<u32, kMaxSwizzleWords> swizzle_data{};
    // One more than the highest word that was ever changed.
    u32 biggest_program_size = 0;
    u32 biggest_swizzle_size = 0;
    // Changed since the previous draw returned.
    bool program_dirty = true;
    bool swizzle_dirty = true;
    bool uniforms_dirty = true;

    void WriteUniformBoolReg(u32 value);
    void WriteUniformIntReg(u32 index, const std::array<u8, 4>& values);

    // `index_reg` is the unit's uniform-setup register (index bits 0-6, float32 format bit 31);
    // the index in it advances with each vector written. Returns the index written.
    std::optional<u32> WriteUniformFloatReg(u32& index_reg, u32 value);

    struct UniformWriteRange {
        u32 first_index;
        u32 count;
    };
    // The same for a run of words written to the one register (a batch).
    std::optional<UniformWriteRange> WriteUniformFloatRegRange(u32& index_reg,
                                                               std::span<const u32> values);

    void UpdateProgramCode(u32 offset, u32 value);
    void UpdateProgramCodeRange(u32 offset, std::span<const u32> values);
    void UpdateSwizzleData(u32 offset, u32 value);
    void UpdateSwizzleDataRange(u32 offset, std::span<const u32> values);

    void ClearDirty() {
        program_dirty = swizzle_dirty = uniforms_dirty = false;
    }
};

// The raw entries of the tables, as the registers deliver them (the bit layout of each entry is
// documented in mgpu-format.md section 4).
struct Lighting {
    static constexpr u32 kTables = 24;
    static constexpr u32 kEntries = 256;
    static constexpr u32 kAllDirty = 0xFFFFFF;
    std::array<std::array<u32, kEntries>, kTables> luts{};
    u32 lut_dirty = kAllDirty; // bit n: table n changed
};

struct Fog {
    static constexpr u32 kEntries = 128;
    std::array<u32, kEntries> lut{};
    bool lut_dirty = true;
};

struct ProcTex {
    static constexpr u8 kAllDirty = 0xFF;
    std::array<u32, 128> noise_table{};
    std::array<u32, 128> color_map_table{};
    std::array<u32, 128> alpha_map_table{};
    std::array<u32, 256> color_table{};
    std::array<u32, 256> color_diff_table{};
    u8 table_dirty = kAllDirty; // bit n: the table with ProcTexLutTable value n changed
};

// ImmediateModeState.
struct ImmediateState {
    std::array<Vec4f, 16> input_vertex{};
    u32 current_attribute = 0;
    bool reset_geometry_pipeline = true;
    PackedAttribute queue;

    void Reset() {
        current_attribute = 0;
        reset_geometry_pipeline = true;
        queue.Reset();
    }
};

// The active backend of Azahar's GeometryPipeline, in the layout of the INIT record's block 11
// (mgpu-format.md). The command processor sets it up (Reconfigure) at every draw; the cursors
// (buffer_index, need_index, the vertex counts and the attribute buffer) only move while vertices
// run through the vertex shader, which is the executor's job.
enum class GeometryMode : u32 { None = 0, Point = 1, VariablePrimitive = 2, FixedPrimitive = 3 };

struct GeometryPipelineState {
    GeometryMode mode = GeometryMode::None;
    u32 vs_output_num = 0;
    u32 buffer_index = 0;  // Point: into the attribute buffer; Variable and Fixed: float uniform index
    u32 buffer_size = 0;   // Point: gs inputs; Fixed: float uniform index of the end of the buffer
    u32 buffer_offset = 0; // Fixed: float uniform index of the start of the buffer
    bool need_index = false;
    u32 main_vertex_num = 0;
    u32 total_vertex_num = 0;
    std::array<Vec4f, 16> attribute_buffer{};
};

// DirtyRegs: one bit per internal register.
struct DirtyRegs {
    std::array<u64, 12> qwords{};

    void Set(u32 id) {
        qwords[id >> 6] |= 1ull << (id & 0x3F);
    }
    void SetRange(u32 first, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            Set(first + i);
        }
    }
    bool Test(u32 id) const {
        return ((qwords[id >> 6] >> (id & 0x3F)) & 1u) != 0;
    }
    void SetAll() {
        qwords.fill(~0ull);
    }
    void Reset() {
        qwords.fill(0);
    }
    bool Any() const {
        for (u64 q : qwords) {
            if (q != 0) {
                return true;
            }
        }
        return false;
    }
};

// Everything the command processor holds about the PICA, and what a draw call refers to.
struct PicaState {
    std::array<u32, kNumRegs> regs{};
    DirtyRegs dirty;
    ShaderSetup vs;
    ShaderSetup gs;
    Lighting lighting;
    Fog fog;
    ProcTex proctex;
    std::array<Vec4f, 16> default_attributes{};
    ImmediateState immediate;
    GeometryPipelineState geometry;

    // The values GSP leaves in the registers (PicaCore::InitializeRegs), and everything dirty.
    PicaState();

    // What a backend consumes at a draw is clean afterwards: the register bits, the look-up
    // tables and the shader setups changed since the previous draw returned.
    void ClearDirty();
};

// DelayGenerator: the time the GPU would take for the commands and triangles since the last
// interrupt, which the interrupt carries (the runtime ignores it, D25).
class DelayGenerator {
public:
    void AddCommands(size_t commands) {
        command_count_ += commands;
    }
    void AddVertices(size_t vertices, u32 topology);
    u64 CalculateAndResetDelay();
    void Reset() {
        command_count_ = triangle_count_ = 0;
    }

private:
    size_t triangle_count_ = 0;
    size_t command_count_ = 0;
};

} // namespace mh4n::gpu::pica
