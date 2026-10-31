// Derived from Azahar src/video_core/pica/geometry_pipeline.cpp, geometry_pipeline.h, shader_unit.h,
// regs_pipeline.h and regs_shader.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later;
// every one of those files says "Licensed under GPLv2 or any later version"): the three backends
// (Point, VariablePrimitive, FixedPrimitive) and GeometryPipeline::SubmitVertex's b15 rule.
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (geometry_pipeline.cpp)
// Copyright 2017 Citra Emulator Project (geometry_pipeline.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (shader_unit.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_pipeline.h, regs_shader.h)
#include "gpu/cpuvertex/geometry_pipeline.h"

#include <algorithm>

#include "gpu/pica/regs.h"

namespace mh4n::gpu::cpuvertex {

using pica::BitsOf;
namespace reg = pica::reg;

namespace {

ShaderInputConfig GsInputConfig(const pica::PicaState& state) {
    const auto& regs = state.regs;
    ShaderInputConfig config;
    config.max_input_attribute_index =
        BitsOf(regs[reg::kGs + reg::kShaderInputBufferConfig], 0, 4);
    config.input_attribute_to_register_map =
        (static_cast<u64>(regs[reg::kGs + 0x0C]) << 32) | regs[reg::kGs + 0x0B];
    return config;
}

Vec4 ToVec4(const pica::Vec4f& v) {
    Vec4 r;
    for (int i = 0; i < 4; ++i) {
        r.c[i] = f24::FromFloat32(v.c[i]);
    }
    return r;
}

} // namespace

/// An attribute buffering interface for different pipeline modes
class GeometryPipelineBackend {
public:
    virtual ~GeometryPipelineBackend() = default;

    /// Checks if there is no incomplete data transfer
    virtual bool IsEmpty() const = 0;

    /// Checks if the pipeline needs a direct input from index buffer
    virtual bool NeedIndexInput() const = 0;

    /// Submits an index from index buffer
    virtual bool SubmitIndex(u32 val) = 0;

    /**
     * Submits vertex attributes
     * @param input attributes of a vertex output from vertex shader
     * @param full set when the buffer is full and the geometry shader should be invoked
     * @return false if the vertex does not fit (Unhandled::GeometryOverflow)
     */
    virtual bool SubmitVertex(const AttributeBuffer& input, bool& full) = 0;
};

// In the Point mode, vertex attributes are sent to the input registers in the geometry shader unit.
// The size of vertex shader outputs and geometry shader inputs are constants. Geometry shader is
// invoked upon inputs buffer filled up by vertex shader outputs. For example, if we have a geometry
// shader that takes 6 inputs, and the vertex shader outputs 2 attributes, it would take 3 vertices
// for one geometry shader invocation.
// TODO (Azahar): what happens when the input size is not divisible by the output size?
class GeometryPipeline_Point : public GeometryPipelineBackend {
public:
    GeometryPipeline_Point(const pica::PicaState* const& state, GeometryShaderUnit& unit,
                           u32 vs_output_num, u32 gs_input_num)
        : state_(state), unit_(unit), buffer_end_(gs_input_num), vs_output_num_(vs_output_num) {}

    void Adopt(const pica::GeometryPipelineState& g) {
        buffer_cur_ = std::min<u32>(g.buffer_index, buffer_end_);
        for (size_t i = 0; i < attribute_buffer_.size(); ++i) {
            attribute_buffer_[i] = ToVec4(g.attribute_buffer[i]);
        }
    }

    bool IsEmpty() const override {
        return buffer_cur_ == 0;
    }

    bool NeedIndexInput() const override {
        return false;
    }

    bool SubmitIndex(u32) override {
        return false; // Azahar: UNREACHABLE(); the caller does not submit indices in this mode
    }

    bool SubmitVertex(const AttributeBuffer& input, bool& full) override {
        full = false;
        if (buffer_cur_ + vs_output_num_ > buffer_end_) {
            return false;
        }
        std::copy(input.data(), input.data() + vs_output_num_, attribute_buffer_.data() + buffer_cur_);
        buffer_cur_ += vs_output_num_;
        if (buffer_cur_ == buffer_end_) {
            buffer_cur_ = 0;
            unit_.LoadInput(GsInputConfig(*state_), attribute_buffer_);
            full = true;
        }
        return true;
    }

private:
    const pica::PicaState* const& state_; // the pipeline's: the state of the draw being run
    GeometryShaderUnit& unit_;
    AttributeBuffer attribute_buffer_{};
    u32 buffer_cur_ = 0; // index into attribute_buffer_
    u32 buffer_end_;     // the number of geometry shader inputs
    u32 vs_output_num_;
};

// In VariablePrimitive mode, vertex attributes are buffered into the uniform registers in the
// geometry shader unit. The number of vertex is variable, which is specified by the first index
// value in the batch. This mode is usually used for subdivision.
class GeometryPipeline_VariablePrimitive : public GeometryPipelineBackend {
public:
    GeometryPipeline_VariablePrimitive(const pica::PicaState* const& state, Uniforms& setup,
                                       u32 vs_output_num)
        : state_(state), setup_(setup), vs_output_num_(vs_output_num) {}

    void Adopt(const pica::GeometryPipelineState& g) {
        need_index_ = g.need_index;
        main_vertex_num_ = g.main_vertex_num;
        total_vertex_num_ = g.total_vertex_num;
        buffer_cur_ = std::min<u32>(g.buffer_index, static_cast<u32>(setup_.f.size()));
    }

    bool IsEmpty() const override {
        return need_index_;
    }

    bool NeedIndexInput() const override {
        return need_index_;
    }

    bool SubmitIndex(u32 val) override {
        // The number of vertex input is put to the uniform register
        f24 vertex_num = f24::FromFloat32(static_cast<float>(val));
        setup_.f[0] = Vec4{{vertex_num, vertex_num, vertex_num, vertex_num}};

        // The second uniform register and so on are used for receiving input vertices
        buffer_cur_ = 1;

        main_vertex_num_ = state_->regs[reg::kVariableVertexMainNum] + 1;
        total_vertex_num_ = val;
        need_index_ = false;
        return true;
    }

    bool SubmitVertex(const AttributeBuffer& input, bool& full) override {
        full = false;
        const u32 size = static_cast<u32>(setup_.f.size());
        const u32 room = size - std::min(buffer_cur_, size);
        if (main_vertex_num_ != 0) {
            // For main vertices, receive all attributes
            if (vs_output_num_ > room) {
                return false;
            }
            std::copy(input.data(), input.data() + vs_output_num_, setup_.f.data() + buffer_cur_);
            buffer_cur_ += vs_output_num_;
            --main_vertex_num_;
        } else {
            // For other vertices, only receive the first attribute (usually the position)
            if (room < 1) {
                return false;
            }
            setup_.f[buffer_cur_++] = input[0];
        }
        --total_vertex_num_;

        if (total_vertex_num_ == 0) {
            need_index_ = true;
            full = true;
        }
        return true;
    }

private:
    const pica::PicaState* const& state_;
    Uniforms& setup_;
    bool need_index_ = true;
    u32 main_vertex_num_ = 0;
    u32 total_vertex_num_ = 0;
    u32 buffer_cur_ = 0; // index into the float uniforms
    u32 vs_output_num_;
};

// In FixedPrimitive mode, vertex attributes are buffered into the uniform registers in the geometry
// shader unit. The number of vertex per shader invocation is constant. This is usually used for
// particle system.
class GeometryPipeline_FixedPrimitive : public GeometryPipelineBackend {
public:
    GeometryPipeline_FixedPrimitive(Uniforms& setup, u32 vs_output_num, u32 buffer_begin, u32 buffer_end)
        : setup_(setup), buffer_begin_(buffer_begin), buffer_cur_(buffer_begin),
          buffer_end_(buffer_end), vs_output_num_(vs_output_num) {}

    void Adopt(const pica::GeometryPipelineState& g) {
        buffer_cur_ = std::clamp<u32>(g.buffer_index, buffer_begin_, buffer_end_);
    }

    bool IsEmpty() const override {
        return buffer_cur_ == buffer_begin_;
    }

    bool NeedIndexInput() const override {
        return false;
    }

    bool SubmitIndex(u32) override {
        return false; // Azahar: UNREACHABLE()
    }

    bool SubmitVertex(const AttributeBuffer& input, bool& full) override {
        full = false;
        if (buffer_cur_ + vs_output_num_ > buffer_end_) {
            return false;
        }
        std::copy(input.data(), input.data() + vs_output_num_, setup_.f.data() + buffer_cur_);
        buffer_cur_ += vs_output_num_;
        if (buffer_cur_ == buffer_end_) {
            buffer_cur_ = buffer_begin_;
            full = true;
        }
        return true;
    }

private:
    Uniforms& setup_;
    u32 buffer_begin_;
    u32 buffer_cur_;
    u32 buffer_end_;
    u32 vs_output_num_;
};

GeometryPipeline::GeometryPipeline(GeometryShaderUnit& gs_unit, Uniforms& gs_uniforms)
    : gs_unit_(gs_unit), gs_(gs_uniforms) {}

GeometryPipeline::~GeometryPipeline() = default;

bool GeometryPipeline::Reconfigure(bool adopt_cursors) {
    discarded_partial_input_ = backend_ != nullptr && !backend_->IsEmpty();
    backend_.reset();
    error_ = {};

    const pica::PicaState& state = *state_;
    const pica::GeometryPipelineState& g = state.geometry;
    if (g.mode == pica::GeometryMode::None) {
        return true;
    }

    gs_unit_.ConfigOutput(BitsOf(state.regs[reg::kGs + reg::kShaderOutputMask], 0, 16));

    const u32 vs_output_num = g.vs_output_num;
    const u32 uniforms = static_cast<u32>(gs_.f.size());
    auto bad = [this] {
        error_ = {Unhandled::GeometryConfig, 0, 0};
        return false;
    };
    if (vs_output_num == 0 || vs_output_num > 16) {
        return bad();
    }
    switch (g.mode) {
    case pica::GeometryMode::Point: {
        if (g.buffer_size == 0 || g.buffer_size > 16 || g.buffer_size % vs_output_num != 0) {
            return bad(); // Azahar: ASSERT(gs_input_num % vs_output_num == 0)
        }
        auto point = std::make_unique<GeometryPipeline_Point>(state_, gs_unit_, vs_output_num, g.buffer_size);
        if (adopt_cursors) {
            point->Adopt(g);
        }
        backend_ = std::move(point);
        break;
    }
    case pica::GeometryMode::VariablePrimitive: {
        auto variable = std::make_unique<GeometryPipeline_VariablePrimitive>(state_, gs_, vs_output_num);
        if (adopt_cursors) {
            variable->Adopt(g);
        }
        backend_ = std::move(variable);
        break;
    }
    case pica::GeometryMode::FixedPrimitive: {
        if (g.buffer_offset > g.buffer_size || g.buffer_size > uniforms) {
            return bad();
        }
        // ASSERT(vs_output_num == stride_minus_1 + 1) was checked by the command processor's log;
        // the buffer is a whole number of vertices (fixed_vertex_num_minus_1 + 1 of them).
        if ((g.buffer_size - g.buffer_offset) % vs_output_num != 0) {
            return bad();
        }
        auto fixed = std::make_unique<GeometryPipeline_FixedPrimitive>(gs_, vs_output_num, g.buffer_offset,
                                                                       g.buffer_size);
        if (adopt_cursors) {
            fixed->Adopt(g);
        }
        backend_ = std::move(fixed);
        break;
    }
    default:
        return bad();
    }
    return true;
}

void GeometryPipeline::Reset() {
    backend_.reset();
    error_ = {};
    discarded_partial_input_ = false;
}

bool GeometryPipeline::IsEmpty() const {
    return backend_ == nullptr || backend_->IsEmpty();
}

bool GeometryPipeline::NeedIndexInput() const {
    if (!backend_) {
        return false;
    }
    return backend_->NeedIndexInput();
}

bool GeometryPipeline::SubmitIndex(u32 val) {
    return backend_->SubmitIndex(val);
}

bool GeometryPipeline::SubmitVertex(const AttributeBuffer& input) {
    if (!backend_) {
        // No backend means the geometry shader is disabled, so we send the vertex shader output
        // directly to the primitive assembler.
        vertex_sink_->SubmitVertex(input);
        return true;
    }
    bool full = false;
    if (!backend_->SubmitVertex(input, full)) {
        error_ = {Unhandled::GeometryOverflow, 0, 0};
        return false;
    }
    if (full) {
        // Setup(): the geometry shader starts at its main offset.
        ShaderSetup setup;
        setup.program_code = state_->gs.program_code.data();
        setup.swizzle_data = state_->gs.swizzle_data.data();
        setup.uniforms = &gs_;
        setup.entry_point = BitsOf(state_->regs[reg::kGs + reg::kShaderMainOffset], 0, 16);
        ++invocations_;
        if (!engine_->Run(setup, gs_unit_, error_)) {
            return false;
        }

        // The uniform b15 is set to true after every geometry shader invocation. This is useful
        // for the shader to know if this is the first invocation in a batch, if the program set
        // b15 to false first.
        gs_.b[15] = true;
    }
    return true;
}

} // namespace mh4n::gpu::cpuvertex
