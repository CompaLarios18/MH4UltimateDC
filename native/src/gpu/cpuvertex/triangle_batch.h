// Derived from Azahar src/video_core/rasterizer_accelerated.cpp and rasterizer_accelerated.h
// (HardwareVertex, AreQuaternionsOpposite, AddTriangle), pica/pica_core.cpp (submit_vertex),
// common/vector_math.h (Dot on Vec4f, the operation order), renderer_vulkan/vk_rasterizer.cpp
// (DrawTriangles, MakeSoftwareVertexLayout, the stream buffer upload of the vertex batch) and
// renderer_vulkan/vk_graphics_pipeline.cpp (the vertex input of the software layout) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6. The licence of this file is GPL-3.0-or-later:
// vk_rasterizer.cpp and vk_graphics_pipeline.cpp say "Licensed under GPLv3 or any later version";
// rasterizer_accelerated.cpp, rasterizer_accelerated.h, pica_core.cpp and vector_math.h say
// "Licensed under GPLv2 or any later version".
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_accelerated.cpp,
//   rasterizer_accelerated.h, pica_core.cpp, vk_rasterizer.cpp, vk_graphics_pipeline.cpp)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (vector_math.h)
// vector_math.h also carries this notice:
//   Copyright 2014 Tony Wasserka
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are met:
//
//       * Redistributions of source code must retain the above copyright
//         notice, this list of conditions and the following disclaimer.
//       * Redistributions in binary form must reproduce the above copyright
//         notice, this list of conditions and the following disclaimer in the
//         documentation and/or other materials provided with the distribution.
//       * Neither the name of the owner nor the names of its contributors may
//         be used to endorse or promote products derived from this software
//         without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
//   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// The triangle batch (N2-37): the triangles the primitive assembler builds from N2-36's output
// vertices, with the quaternion fix-up done on the CPU, as a triangle list that a trivial vertex
// stage (trivial_vertex.h) draws from a streamed vertex buffer. This is Azahar's software path to its
// hardware rasterizer: `AddTriangle` into `vertex_batch`, the batch memcpy'd into the stream buffer,
// `DrawTriangles`.
//
// ---- How N2-08 uses it (per draw, on the thread that owns the PICA state) -----------------------------
//
//   1. assembler.ApplyRegisterWrites(*draw.state);          // primitive_assembly.h
//   2. if (!NeedsCpuPath(draw, assembler.IsEmpty())) { runner.NoteDraw(draw); <accelerated path>; return; }
//   3. if (runner.Run(host, draw) != RunStatus::Ok) return; // the draw is dropped (counted by the runner)
//   4. batch.Clear();
//      assembler.SubmitStream(runner.output(), [&](auto& v0, auto& v1, auto& v2) {
//          batch.AddTriangle(v0, v1, v2);                   // the fix-up is in here
//      });
//   5. if (batch.TriangleCount() == 0) return;              // nothing to draw: no ring allocation
//      with the thread's recorder open and the pass begun:
//      Result<StreamedBatch> s = UploadBatch(ring, batch);  // Ring::Map + copy + Flush + Commit
//      rec.SetPipeline(<a pipeline whose vertex stage is TrivialVertexShader(use_clip_planes),
//                       vertex_layout = MakeBatchVertexLayout(), topology TriangleList>);
//      rec.Bind(<set 0>, 0);
//      const rhi::VertexBufferBinding b{ring.GetBuffer().get(), s->offset};
//      rec.SetVertexBuffers(0, {&b, 1});
//      rec.Draw(s->vertex_count);
//
// The pipeline: `TrivialVertexShader(true)` when the pipelines of the translated vertex stage are made
// with `use_clip_planes` (N2-06's default; N2-08 sets it from the device's shaderClipDistance), else
// `TrivialVertexShader(false)`; the same flag for both stages keeps the fragment stage's pipelines
// shared. Set 0 is the translated stage's: binding 0 `PicaUniformData` (the trivial stage does not
// read it) and binding 1 `VertexUniformData` (32 bytes, std140: enable_clip1, flip_viewport, clip_coef),
// so one pipeline layout fits both vertex stages. The varyings are at locations 1 to 7, as the
// fragment stage declares them (N2-06's "Interface to the pipeline"). The batch is a triangle list
// (rhi::Topology::TriangleList): the primitive assembler has already turned Strip, Fan and Shader
// topologies into triangles.
//
// The batch's space in the ring: UploadBatch calls Ring::Map and Ring::Commit, so the draw's recorder
// must be open on the calling thread; the ring then reuses that space once the submission of that
// recorder has passed (rhi.h, Ring). Do not touch the space again; do not keep the buffer's pointer
// past the submission. A ring too small for one batch fails with the Ring's message (a batch is
// 88 bytes per vertex, 264 per triangle: size the ring for the largest draw, at least 16M vertices
// are not expected). Check the device's `Capabilities::stride_alignment` divides
// kHardwareVertexStride (88): true for 1, 2, 4 and 8.
//
// The fix-up (Azahar's AddTriangle): vertex 0 of a triangle is never flagged; vertex 1 and vertex 2
// are flagged when the dot product of their quaternion with vertex 0's quaternion is below 0. The dot
// product is Azahar's `Common::Dot` on `Vec4f`, in float: ((a.x*b.x + a.y*b.y) + a.z*b.z) + a.w*b.w
// with no fused multiply-add (the x86 and generic path; Azahar's AArch64 path sums the four products
// pairwise, which can differ in the last bit and so only for a dot product that rounds to 0). A zero
// dot product and a NaN are not flagged. A flagged vertex has its quaternion negated lane by lane in
// the batch (HardwareVertex's `normquat = -normquat`; a 0 lane becomes -0).
#pragma once

#include <cstddef>
#include <vector>

#include "gpu/backend/rhi.h"
#include "gpu/cpuvertex/cpu_vertex.h"
#include "mh4n/types.h"

namespace mh4n::gpu::cpuvertex {

// Azahar's RasterizerAccelerated::HardwareVertex: 22 floats, no padding.
struct HardwareVertex {
    float position[4];
    float color[4];
    float tex_coord0[2];
    float tex_coord1[2];
    float tex_coord2[2];
    float tex_coord0_w;
    float normquat[4];
    float view[3];
};
static_assert(sizeof(HardwareVertex) == 22 * sizeof(float), "HardwareVertex has invalid size");

inline constexpr u32 kHardwareVertexStride = sizeof(HardwareVertex); // 88

// The vertex attribute locations of the batch (Azahar's Attributes enumeration; the trivial vertex
// shader's inputs): 0 position vec4, 1 color vec4, 2 texcoord0 vec2, 3 texcoord1 vec2, 4 texcoord2 vec2,
// 5 texcoord0_w float, 6 normquat vec4, 7 view vec3. One binding (0), stride kHardwareVertexStride,
// float formats, the offsets of the fields of HardwareVertex.
rhi::VertexLayout MakeBatchVertexLayout();

// HardwareVertex::HardwareVertex(const OutputVertex&, bool flip_quaternion).
HardwareVertex MakeHardwareVertex(const OutputVertex& v, bool flip_quaternion);

// AreQuaternionsOpposite: Dot(a, b) < 0 in float (see the header comment).
bool AreQuaternionsOpposite(const float (&a)[4], const float (&b)[4]);

// The triangles of one draw. Clear() keeps the storage, so after the first draws that grew it, a
// draw allocates nothing (no allocation per vertex either way: the vector grows geometrically).
class TriangleBatch {
public:
    void Clear() { vertices_.clear(); }
    // RasterizerAccelerated::AddTriangle.
    void AddTriangle(const OutputVertex& v0, const OutputVertex& v1, const OutputVertex& v2);

    size_t VertexCount() const { return vertices_.size(); }
    size_t TriangleCount() const { return vertices_.size() / 3; }
    bool Empty() const { return vertices_.empty(); }
    const HardwareVertex* Data() const { return vertices_.data(); }
    const std::vector<HardwareVertex>& Vertices() const { return vertices_; }

private:
    std::vector<HardwareVertex> vertices_;
};

// Where UploadBatch put a batch.
struct StreamedBatch {
    u64 offset = 0;       // in Ring::GetBuffer()
    u32 vertex_count = 0; // 0: nothing was written and nothing is to be drawn
};

// Writes the batch into a ring allocation (Map, copy, Flush, Commit). A batch of no triangles makes no
// ring allocation and returns vertex_count 0. Needs the calling thread's recorder open (Ring::Commit).
rhi::Result<StreamedBatch> UploadBatch(rhi::Ring& ring, const TriangleBatch& batch);

} // namespace mh4n::gpu::cpuvertex
