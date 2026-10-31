// The vertex-input key from N2-05's description (N2-33).
//
// Written for this package: nothing is copied from Azahar here.
#pragma once

#include "gpu/native/pipeline_key.h"
#include "gpu/vertex/vertex_loader.h"

namespace mh4n::gpu::native {

// The host format of one of N2-05's vertex formats (they were chosen to exist in rhi.h).
rhi::Format ToRhiFormat(vertex::VertexFormat format);

// The topology N2-05 returns as a pipeline topology. The "shader" topology (the primitives come from the geometry
// shader) is drawn from CPU-assembled triangle lists by N2-36 and N2-37: it maps to TriangleList.
rhi::Topology ToRhiTopology(vertex::Topology topology);

// The bindings (one per buffer of the loaded draw: stride and per-instance flag) and the 16 attributes (one per
// input register: location, binding, format, offset) of `draw`.
VertexInputKey MakeVertexInput(const vertex::LoadedDraw& draw);

}  // namespace mh4n::gpu::native
