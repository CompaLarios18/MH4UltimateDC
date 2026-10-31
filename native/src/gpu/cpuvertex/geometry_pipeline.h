// Derived from Azahar src/video_core/pica/geometry_pipeline.h and geometry_pipeline.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; both files say "Licensed under GPLv2
// or any later version"): the three input modes of the pipeline between the vertex shader and the
// geometry shader, their buffers and cursors.
// Copyright 2017 Citra Emulator Project (geometry_pipeline.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (geometry_pipeline.cpp)
//
// The geometry pipeline (N2-36). Internal to the module: the public API is cpu_vertex.h.
//
// As Azahar's GeometryPipeline: in the Point mode the vertex outputs fill the geometry shader's
// input registers, in the VariablePrimitive and FixedPrimitive modes they are buffered in the
// geometry shader's float uniforms; when the buffer is full the geometry shader runs and the
// uniform b15 is set to true. With no geometry shader the vertex outputs go straight to the sink.
//
// What differs, on purpose:
//  - The configuration is read from the PicaState's `geometry` (the command processor did
//    Azahar's Reconfigure at the draw), not re-derived from the registers; the registers are
//    read where Azahar reads them at run time (the geometry shader's input map and entry point,
//    the variable primitive's main vertex count).
//  - Cursors are indices, not pointers, into storage the runner owns (the geometry shader's
//    uniforms), so the pipeline can live between draws.
//  - Where Azahar asserts (Reconfigure's ASSERTs and those of the backends' constructors:
//    use_gs, the unit 3 shader mode, the two output counts, variable_primitive and input_to_uniform
//    per mode, the Point input/output divisibility, the FixedPrimitive stride; vertices that do
//    not fit the registers or uniforms; variable primitive mode without an index buffer) the
//    pipeline reports an error (Unhandled::Geometry*) and the runner drops the draw.
//  - Reconfigure over a buffer that still holds a partial input (Azahar: ASSERT(IsEmpty())) throws
//    the partial input away and says so with `discarded_partial_input()`.
#pragma once

#include <memory>

#include "gpu/cpuvertex/shader_interpreter.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::cpuvertex {

class GeometryPipelineBackend;

class GeometryPipeline {
public:
    // `gs_unit` and `gs_uniforms` belong to the runner; the pipeline writes the uniforms.
    GeometryPipeline(GeometryShaderUnit& gs_unit, Uniforms& gs_uniforms);
    ~GeometryPipeline();
    GeometryPipeline(const GeometryPipeline&) = delete;
    GeometryPipeline& operator=(const GeometryPipeline&) = delete;

    // Where vertices go when no geometry shader is in use.
    void SetVertexSink(VertexSink* sink) {
        vertex_sink_ = sink;
    }

    // The state of the draw being run: the registers and the geometry shader's program. Call it at
    // the start of every run.
    void SetState(const pica::PicaState& state, const InterpreterEngine* engine) {
        state_ = &state;
        engine_ = engine;
    }

    // Reconfigure(): builds the backend `state.geometry` describes (none for GeometryMode::None).
    // With `adopt_cursors` the backend also takes the cursors and buffers of `state.geometry` (the
    // state of a capture's INIT record, for a pipeline that has not been configured yet). False if
    // the configuration is inconsistent (the error is in error()); the pipeline is then
    // unconfigured.
    bool Reconfigure(bool adopt_cursors);

    // The pipeline has a backend, i.e. a geometry shader is in use.
    bool Active() const {
        return backend_ != nullptr;
    }
    bool IsEmpty() const;
    bool discarded_partial_input() const {
        return discarded_partial_input_;
    }

    // Checks if the pipeline needs a direct input from index buffer.
    bool NeedIndexInput() const;
    // Submits an index from index buffer. Call this only when NeedIndexInput returns true.
    bool SubmitIndex(u32 val);
    // Submits vertex attributes output from vertex shader. False if the geometry shader stopped or
    // the vertex does not fit (error()).
    bool SubmitVertex(const AttributeBuffer& input);

    // Drops the backend and what it holds (a draw was abandoned).
    void Reset();

    // How many times the geometry shader has run.
    u64 invocations() const {
        return invocations_;
    }

    const RunError& error() const {
        return error_;
    }

private:
    GeometryShaderUnit& gs_unit_;
    Uniforms& gs_;
    VertexSink* vertex_sink_ = nullptr;
    const pica::PicaState* state_ = nullptr;
    const InterpreterEngine* engine_ = nullptr;
    std::unique_ptr<GeometryPipelineBackend> backend_;
    RunError error_;
    bool discarded_partial_input_ = false;
    u64 invocations_ = 0;
};

} // namespace mh4n::gpu::cpuvertex
