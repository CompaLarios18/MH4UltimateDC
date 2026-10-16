// The interface between the PICA command processor and whatever draws (N2-04). A draw call is
// what Azahar's PicaCore hands its rasterizer when a draw trigger register is written, or when
// the last attribute of an immediate-mode vertex arrives, minus the work Azahar does itself
// (loading the vertices and running the shaders): the register state, the shader setups and
// tables, and where the vertices are.
//
// Once merged this header is a contract for the WPs that draw (N2-05 onwards): a change goes
// through the orchestrator.
#pragma once

#include "gpu/pica/state.h"
#include "mh4n/types.h"

namespace mh4n::gpu::pica {

enum class DrawKind : u32 {
    // GPUREG_DRAWARRAYS: `num_vertices` vertices from the attribute loaders, starting at vertex
    // `vertex_offset`.
    Arrays,
    // GPUREG_DRAWELEMENTS: `num_vertices` indices from the index buffer at
    // `vertex_base_address + index_offset` (u16 or u8 per `index_u16`), and the vertices they
    // name. The start offset is not used.
    Indexed,
    // One vertex whose attributes were written to the fixed-attribute registers with the index
    // register at 0xF: state->immediate.input_vertex[0 .. max_input_attrib_index].
    Immediate,
};

struct DrawCall {
    DrawKind kind = DrawKind::Arrays;
    // 0 for the first draw the command processor issued, then counting up.
    u32 sequence = 0;

    // Where the vertices come from (Arrays and Indexed; Immediate: num_vertices is 1).
    u32 vertex_base_address = 0; // physical: GetPhysicalBaseAddress()
    u32 num_vertices = 0;
    u32 vertex_offset = 0;       // Arrays
    u32 index_offset = 0;        // Indexed: relative to vertex_base_address
    bool index_u16 = false;      // Indexed

    // The geometry pipeline was set up afresh for this draw: always for Arrays and Indexed, and
    // for Immediate when this is the first vertex since the fixed-attribute index register was
    // written. state->geometry holds the configuration.
    bool geometry_reconfigured = false;

    // The processor's state at the moment of the draw. Valid only during Backend::Draw; a
    // backend that keeps it copies it. The dirty flags in it (registers, tables, shader setups)
    // tell what changed since the previous draw returned; the processor clears them afterwards.
    const PicaState* state = nullptr;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void Draw(const DrawCall& draw) = 0;
};

} // namespace mh4n::gpu::pica
