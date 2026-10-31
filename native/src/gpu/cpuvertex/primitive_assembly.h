// Derived from Azahar src/video_core/pica/primitive_assembly.h and primitive_assembly.cpp, and the
// parts of pica_core.cpp (PicaCore::PicaCore's submit_vertex and winding handlers, the register side
// effects of triangle_topology and restart_primitive) and shader_unit.cpp (GeometryEmitter::Emit,
// where SetWinding is called) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one
// of those files says "Licensed under GPLv2 or any later version"): the assembler that builds
// triangles from a series of output vertices, according to the triangle topology.
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (primitive_assembly.h)
// Copyright 2014 Citra Emulator Project (primitive_assembly.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp, shader_unit.cpp)
//
// The primitive assembler (N2-37): PICA output vertices in, triangles out. It is state of the
// renderer, like Azahar's `PicaCore::primitive_assembler`: it persists between draws (a Strip or Fan
// draw can leave a vertex buffered for the next one), and only the draws that take the CPU path
// (cpu_vertex.h) feed it, as in Azahar. One instance per renderer, used by the thread that owns the
// PICA state. Call order per draw: see triangle_batch.h.
//
//   PrimitiveAssembler a;                       // List, empty
//   a.ApplyRegisterWrites(*draw.state);         // at each draw, BEFORE NeedsCpuPath(draw, a.IsEmpty())
//   a.SubmitStream(runner.output(), handler);   // handler(const OutputVertex&, v1, v2) per triangle
//
// Azahar reconfigures the assembler when the game writes `pipeline.triangle_topology` (0x25E, bits
// 8-9) and resets it when the game writes `pipeline.restart_primitive` (0x25F); the PICA command
// processor (N2-04) reports no such event, so ApplyRegisterWrites reads `state.dirty` instead: a
// register's dirty bit is set by every write, whether or not the value changed, and a fresh PicaState
// has everything dirty (the first draw configures the assembler from the register). Both calls empty
// the buffer (and clear the winding flag), so the order of two writes between two draws does not
// matter; the topology is the register's value at the draw. Azahar's `Reconfigure(List)` in the
// PicaCore constructor is the default constructor here.
//
// SubmitStream feeds `OutputStream` the way Azahar's geometry emitter and pipeline do: for each vertex
// at position p of `vertices`, SetWinding() first when p is listed in `winding_before`, then
// SubmitVertex. The winding flag only changes the order of the next triangle in the Shader topology
// (and the flag stays set, as in Azahar, in the other topologies until Reset or Reconfigure).
//
// Triangles per topology (Azahar's SubmitVertex; v0, v1, ... the submitted vertices):
//   List, Shader: (v0, v1, v2), (v3, v4, v5), ...; Shader with the winding flag set when the third
//                 vertex of a triangle arrives: (v1, v0, v2) and the flag is cleared.
//   Strip:        from the third vertex on, (buffer[0], buffer[1], v) where the buffer holds the last
//                 two vertices in slots 0 and 1 and the new vertex replaces the older slot: v0 v1 v2 ->
//                 (v0, v1, v2); v3 -> (v2, v1, v3) (slot 0 now holds v2); v4 -> (v2, v3, v4); ...
//   Fan:          (v0, v1, v2), (v0, v2, v3), (v0, v3, v4), ... (slot 0 keeps v0, slot 1 the last).
// The vertices are passed by reference to the handler; a handler that keeps them copies them.
#pragma once

#include <array>

#include "gpu/cpuvertex/cpu_vertex.h"
#include "gpu/pica/regs.h"
#include "gpu/pica/state.h"
#include "mh4n/types.h"

namespace mh4n::gpu::cpuvertex {

// PipelineRegs::TriangleTopology (the values of the register's bits 8-9).
enum class Topology : u8 { List = 0, Strip = 1, Fan = 2, Shader = 3 };
static_assert(static_cast<u32>(Topology::List) == pica::kTopologyList &&
                  static_cast<u32>(Topology::Strip) == pica::kTopologyStrip &&
                  static_cast<u32>(Topology::Fan) == pica::kTopologyFan &&
                  static_cast<u32>(Topology::Shader) == pica::kTopologyShader,
              "the topology values are the register's");

class PrimitiveAssembler {
public:
    explicit PrimitiveAssembler(Topology topology = Topology::List) : topology_(topology) {}

    // Queues a vertex, builds the primitives the queue now holds and calls
    // triangle_handler(const OutputVertex&, const OutputVertex&, const OutputVertex&) for each.
    template <class Handler>
    void SubmitVertex(const OutputVertex& vtx, Handler&& triangle_handler) {
        switch (topology_) {
        case Topology::List:
        case Topology::Shader:
            if (buffer_index_ < 2) {
                buffer_[static_cast<size_t>(buffer_index_++)] = vtx;
            } else {
                buffer_index_ = 0;
                if (topology_ == Topology::Shader && winding_) {
                    triangle_handler(buffer_[1], buffer_[0], vtx);
                    winding_ = false;
                } else {
                    triangle_handler(buffer_[0], buffer_[1], vtx);
                }
            }
            break;

        case Topology::Strip:
        case Topology::Fan:
            if (strip_ready_) {
                triangle_handler(buffer_[0], buffer_[1], vtx);
            }

            buffer_[static_cast<size_t>(buffer_index_)] = vtx;
            strip_ready_ |= (buffer_index_ == 1);

            if (topology_ == Topology::Strip) {
                buffer_index_ = !buffer_index_;
            } else {
                buffer_index_ = 1;
            }
            break;
        }
    }

    // The vertices of one Run, with SetWinding at the positions of `winding_before`.
    template <class Handler>
    void SubmitStream(const OutputStream& stream, Handler&& triangle_handler) {
        size_t next_winding = 0;
        for (size_t p = 0; p < stream.vertices.size(); ++p) {
            if (next_winding < stream.winding_before.size() && stream.winding_before[next_winding] == p) {
                SetWinding();
                ++next_winding;
            }
            SubmitVertex(stream.vertices[p], triangle_handler);
        }
    }

    // Inverts the vertex order of the next triangle (Shader topology only).
    void SetWinding() noexcept { winding_ = true; }

    // Empties the buffer and clears the winding flag.
    void Reset() {
        buffer_index_ = 0;
        strip_ready_ = false;
        winding_ = false;
    }

    void Reconfigure(Topology topology) {
        Reset();
        topology_ = topology;
    }

    // Azahar's side effects of a write to triangle_topology / restart_primitive, from the dirty bits
    // of `state` (see the header comment). Call it at every draw, before NeedsCpuPath.
    void ApplyRegisterWrites(const pica::PicaState& state);

    bool IsEmpty() const { return buffer_index_ == 0 && !strip_ready_; }
    Topology GetTopology() const { return topology_; }

private:
    Topology topology_;
    int buffer_index_ = 0;
    std::array<OutputVertex, 2> buffer_{};
    bool strip_ready_ = false;
    bool winding_ = false;
};

} // namespace mh4n::gpu::cpuvertex
