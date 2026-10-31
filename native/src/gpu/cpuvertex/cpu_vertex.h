// Derived from Azahar src/video_core/pica/pica_core.cpp (DrawArrays, LoadVertices, DrawImmediate),
// pica_core.h, output_vertex.h, output_vertex.cpp, shader_unit.h, shader_setup.cpp, regs_pipeline.h,
// regs_rasterizer.h and regs_shader.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later;
// every one of those files says "Licensed under GPLv2 or any later version"): the rule that picks
// the draws that are not accelerated, the draw loops with the vertex cache, the output vertex and
// its semantic layout, and the names of the registers they read.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp, pica_core.h,
//   shader_unit.h, shader_setup.cpp)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (output_vertex.h)
// Copyright 2023 Citra Emulator Project (output_vertex.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_pipeline.h, regs_shader.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
//
// The CPU shader runner (N2-36). It runs the vertex shader, and the geometry shader if the game
// uses one, on the CPU for the draws Azahar does not accelerate: what Azahar's
// `PicaCore::DrawArrays` (`LoadVertices`) and `DrawImmediate` do, up to the point where a vertex
// would go to the primitive assembler. Assembling primitives, the fix-up and batching the result
// for the GPU are N2-37's; choosing the path and drawing are N2-08's. There is no device here.
//
// ---- API (what N2-37, N2-08 and N2-16 call) -----------------------------------------------------
//
//   CpuReason CpuPathReason(draw, assembler_empty); bool NeedsCpuPath(draw, assembler_empty);
//       Azahar's `accelerate_draw` test (pica_core.cpp, DrawArrays, with `use_hw_shader` on): the draw
//       takes the CPU path when it uses the geometry shader (use_gs is Yes), when it is an Immediate
//       draw, or when it is an Arrays/Indexed draw that `accelerate_draw` refuses: the primitive
//       assembler is not empty, or the topology is List or Shader and the vertex count is not a
//       multiple of 3. The reason is the first that applies, in that order. `assembler_empty` is
//       N2-37's primitive assembler's `IsEmpty()` after it has applied the draw's register side
//       effects (topology write: Reconfigure, restart_primitive write: Reset); N2-04 keeps no
//       assembler state. Azahar also falls back when `AccelerateDrawBatch` fails (a vertex program
//       that does not translate): that is N2-06's and N2-08's to detect, and N2-08 then calls
//       `Run` as for any selected draw.
//
//   CpuVertexRunner runner;                       // one per renderer; it holds the persistent state
//   RunStatus Run(host, draw, mode = RunMode::Full);   // Backend::Draw time (draw.state is valid)
//   const OutputStream& output() const;           // what the last Run produced
//   void NoteDraw(draw);                          // for draws that are not run (see below)
//   const Counters& counters() const;             // and the names kCounter* of the published ones
//   const std::string& last_error() const;        // text of the last non-Ok status
//   void Reset();                                 // forget the persistent state (a new capture)
//
// `Run` shades one draw. RunMode::Full is the whole of Azahar's path: the vertex unit, then the
// geometry pipeline and the geometry unit if the draw's `state->geometry` says a geometry shader
// is in use. RunMode::VertexOnly is for N2-16's fallback (ii): the vertex unit alone, on any draw,
// and every vertex it produces goes to the output (the geometry pipeline, the geometry unit and
// their persistent state are not touched).
//
// ---- The output ----------------------------------------------------------------------------------
//
// `output().vertices` is the stream of vertices that Azahar's geometry pipeline would hand to its
// primitive assembler (`PrimitiveAssembler::SubmitVertex`), in the same order:
//  - Arrays / Indexed draws: one per input vertex, in draw order (an index appearing twice gives
//    two outputs; the vertex cache only saves the shading), or, with a geometry shader, the
//    vertices its EMITs produce: three per EMIT whose SETEMIT had prim_emit set (the geometry
//    emitter always sends its three buffered vertices).
//  - Immediate draws: one per call (it is one vertex), or the vertices of a geometry shader run
//    that this vertex completed (none while the input registers are still being filled).
// `output().winding_before` lists, in increasing order, the positions p in `vertices` for which
// Azahar calls `PrimitiveAssembler::SetWinding()` just before it submits vertices[p]: the EMIT
// whose SETEMIT had the winding flag set. N2-37 feeds the stream to its assembler and calls its
// SetWinding at those positions. (It only has an effect for the Shader topology, as in Azahar.)
// An `OutputVertex` is Azahar's `OutputVertex`: the output attributes placed by the semantics the
// rasterizer registers give (vs_output_total, vs_output_attributes), slots no attribute maps to
// are 1.0, and the colour is |c| saturated to 1 ("the hardware takes the absolute and saturates
// vertex colors, before doing interpolation"). Its layout is fixed (24 floats, slot = the
// semantic number: position 0-3, quaternion 4-7, colour 8-11, tc0 12-13, tc1 14-15, tc0_w 16,
// view 18-20, tc2 22-23; 17 and 21 are padding), so a consumer can read the array as it is.
// The vertices live in vectors the runner reuses: copy nothing, but read them before the next
// `Run` (or `Reset`). There is no allocation per vertex once the vectors have grown.
//
// ---- Status, the counters and what is dropped ------------------------------------------------------
//
// RunStatus::Ok: the draw ran to its end (a draw of no vertices is Ok with an empty output).
// RunStatus::LoadFailed: the loader (N2-05) could not read the draw's vertex or index data (outside
//   guest memory, or an Arrays draw of over 16 million vertices); counted in cpu_path_load_failed.
// RunStatus::Unhandled: an instruction or a mode the runner does not handle. Logged once per
//   program (MH4N_LOG, category "CpuVertex"; never a value of the game), the draw is dropped (empty
//   output) and counted in cpu_path_unhandled. What is unhandled: DST, DSTI and LIT (Azahar's
//   interpreter has no handler for them: it logs and goes on, which would be a silent
//   approximation), an opcode that does not exist, a CMP with compare mode 6 or 7, EMIT or SETEMIT
//   on the vertex unit, an EMIT with vertex id 3, a program that does not leave a loop (4M
//   instructions), an entry point beyond the program memory, and the cases where Azahar asserts: an
//   inconsistent geometry pipeline set-up, a variable-primitive geometry shader on an Arrays or
//   Immediate draw (it needs an index buffer), more vertices than fit the geometry shader's input
//   registers or uniforms.
// Counters (cumulative, see `counters()`): kCounterDraws "cpu_path_draws" (draws given to Run),
// kCounterVertices "cpu_path_vertices" (vertices submitted to the vertex unit, cache hits included),
// kCounterUnhandled "cpu_path_unhandled", kCounterLoadFailed "cpu_path_load_failed"; the struct
// also has vertices_out and the number of vertex and geometry shader invocations. The caller
// publishes them under those names.
//
// ---- Inputs and persistent state ---------------------------------------------------------------------
//
// The vertices of an Arrays / Indexed draw come from N2-05's VertexLoader, built with the caps that
// leave the PICA's own layout (fans and 8-bit indices native, stride alignment 1, no restart value):
// the runner sees the draw's vertices in the order and with the indexing the PICA gives them. It
// converts the loader's description to the shader's float inputs the way Azahar's
// `VertexLoader::LoadVertex` and `ShaderUnit::LoadInput` do: BYTE/UBYTE/SHORT are not normalised,
// the lanes an attribute does not have are 0 (w: 1), a default attribute is the PICA's default
// value. An immediate-mode vertex is `state->immediate.input_vertex`. No second loader.
// What Azahar keeps between draws and the runner therefore keeps: the geometry shader unit
// (temporaries, address registers, condition codes, the two output banks and the bank switch, the
// emitter's SETEMIT state and its three buffered vertices), the geometry pipeline (its cursors and
// buffers: the input registers filled so far in Point mode, the variable primitive's remaining
// vertex counts), and the geometry shader's uniforms with the bits the pipeline changed (b15 is set
// to true after every geometry shader invocation, and stays so until the game writes the
// geometry unit's bool uniforms register; the float uniforms the pipeline buffers vertices in are
// overwritten by those vertices). The pipeline is reconfigured at the draws that carry
// `geometry_reconfigured` (every Arrays/Indexed draw; an Immediate draw only for the first vertex
// after a write to register 0x232); a runner that has not been configured yet takes the
// cursors and buffers of `state->geometry` (the capture's INIT record). The processor's dirty flags
// tell the runner which uniforms the game wrote since the last draw returned; they are cleared after
// every draw, also those the runner does not see, so the caller should call `NoteDraw` for each draw
// it does not `Run` (an accelerated draw), or a rewrite of the same value of b15 between two
// geometry-shader draws would be missed.
// The vertex unit is new for every draw (Azahar: a local ShaderUnit in LoadVertices) and for every
// immediate vertex (DrawImmediate), so its temporaries start at 0 for each.
//
// Thread: only the thread that owns the PICA state calls it (principle 6).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gpu/pica/backend.h"
#include "gpu/vertex/vertex_loader.h"
#include "mh4n/gpu.h"
#include "mh4n/types.h"

namespace mh4n::gpu::cpuvertex {

// Pica::OutputVertex: 24 floats; the slot of a value is the number of its semantic.
struct OutputVertex {
    float pos[4];
    float quat[4];
    float color[4];
    float tc0[2];
    float tc1[2];
    float tc0_w;
    float padding0;
    float view[3];
    float padding1;
    float tc2[2];
};
static_assert(sizeof(OutputVertex) == 24 * sizeof(float), "OutputVertex has invalid size");

// Why a draw takes the CPU path (the first that applies).
enum class CpuReason : u8 {
    None,           // accelerated (the hardware vertex shader draws it)
    GeometryShader, // use_gs is Yes
    Immediate,      // an immediate-mode vertex
    Refused,        // accelerate_draw says no: the assembler is not empty, or List/Shader topology
                    // with a vertex count that is not a multiple of 3
};
const char* ToString(CpuReason reason);

CpuReason CpuPathReason(const pica::DrawCall& draw, bool assembler_empty);
inline bool NeedsCpuPath(const pica::DrawCall& draw, bool assembler_empty) {
    return CpuPathReason(draw, assembler_empty) != CpuReason::None;
}

enum class RunMode : u8 {
    Full,       // vertex unit, geometry pipeline and geometry unit, as Azahar
    VertexOnly, // the vertex unit alone, on any draw (N2-16's fallback (ii))
};

enum class RunStatus : u8 { Ok, LoadFailed, Unhandled };

// The counters the caller publishes under these names (as vertex_loader.h's kCounterRestartWidened).
inline constexpr const char* kCounterDraws = "cpu_path_draws";
inline constexpr const char* kCounterVertices = "cpu_path_vertices";
inline constexpr const char* kCounterUnhandled = "cpu_path_unhandled";
inline constexpr const char* kCounterLoadFailed = "cpu_path_load_failed";

struct Counters {
    u64 draws = 0;        // kCounterDraws
    u64 vertices = 0;     // kCounterVertices: vertices submitted to the vertex unit
    u64 unhandled = 0;    // kCounterUnhandled
    u64 load_failed = 0;  // kCounterLoadFailed
    u64 vertices_out = 0; // OutputVertex entries produced by draws that ran to their end
    u64 vs_invocations = 0;
    u64 gs_invocations = 0;
};

struct OutputStream {
    std::vector<OutputVertex> vertices;
    std::vector<u32> winding_before; // see "The output"
};

class CpuVertexRunner {
public:
    CpuVertexRunner();
    ~CpuVertexRunner();
    CpuVertexRunner(const CpuVertexRunner&) = delete;
    CpuVertexRunner& operator=(const CpuVertexRunner&) = delete;

    RunStatus Run(gpu::Host& host, const pica::DrawCall& draw, RunMode mode = RunMode::Full);
    void NoteDraw(const pica::DrawCall& draw);
    void Reset();

    const OutputStream& output() const;
    const Counters& counters() const;
    const std::string& last_error() const;
    // The loader's own counters (draws_loaded, draws_dropped, ...).
    const vertex::VertexCounters& loader_counters() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mh4n::gpu::cpuvertex
