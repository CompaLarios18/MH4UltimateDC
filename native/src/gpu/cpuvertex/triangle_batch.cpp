// Derived from Azahar src/video_core/rasterizer_accelerated.cpp, rasterizer_accelerated.h,
// common/vector_math.h, renderer_vulkan/vk_rasterizer.cpp and vk_graphics_pipeline.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp and
// vk_graphics_pipeline.cpp say "Licensed under GPLv3 or any later version"; the others say GPLv2 or
// any later version).
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_accelerated.cpp,
//   rasterizer_accelerated.h, vk_rasterizer.cpp, vk_graphics_pipeline.cpp)
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
#include "gpu/cpuvertex/triangle_batch.h"

#include <cstddef>
#include <cstring>

namespace mh4n::gpu::cpuvertex {

rhi::VertexLayout MakeBatchVertexLayout() {
    rhi::VertexLayout layout;
    layout.bindings.push_back({0, kHardwareVertexStride, false});
    const struct {
        rhi::Format format;
        u32 offset;
    } attributes[8] = {
        {rhi::Format::Rgba32Float, static_cast<u32>(offsetof(HardwareVertex, position))},
        {rhi::Format::Rgba32Float, static_cast<u32>(offsetof(HardwareVertex, color))},
        {rhi::Format::Rg32Float, static_cast<u32>(offsetof(HardwareVertex, tex_coord0))},
        {rhi::Format::Rg32Float, static_cast<u32>(offsetof(HardwareVertex, tex_coord1))},
        {rhi::Format::Rg32Float, static_cast<u32>(offsetof(HardwareVertex, tex_coord2))},
        {rhi::Format::R32Float, static_cast<u32>(offsetof(HardwareVertex, tex_coord0_w))},
        {rhi::Format::Rgba32Float, static_cast<u32>(offsetof(HardwareVertex, normquat))},
        {rhi::Format::Rgb32Float, static_cast<u32>(offsetof(HardwareVertex, view))},
    };
    for (u32 i = 0; i < 8; ++i) {
        layout.attributes.push_back({i, 0, attributes[i].format, attributes[i].offset});
    }
    return layout;
}

HardwareVertex MakeHardwareVertex(const OutputVertex& v, bool flip_quaternion) {
    HardwareVertex out;
    for (int i = 0; i < 4; ++i) {
        out.position[i] = v.pos[i];
        out.color[i] = v.color[i];
        // -x also flips the sign of a zero, as `normquat = -normquat` does on Azahar's Vec4f.
        out.normquat[i] = flip_quaternion ? -v.quat[i] : v.quat[i];
    }
    for (int i = 0; i < 2; ++i) {
        out.tex_coord0[i] = v.tc0[i];
        out.tex_coord1[i] = v.tc1[i];
        out.tex_coord2[i] = v.tc2[i];
    }
    out.tex_coord0_w = v.tc0_w;
    for (int i = 0; i < 3; ++i) {
        out.view[i] = v.view[i];
    }
    return out;
}

bool AreQuaternionsOpposite(const float (&a)[4], const float (&b)[4]) {
    // a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w, left to right, each product rounded to float
    // (no fused multiply-add, which would change the rounding of a dot product that is close to 0).
#if defined(__clang__)
#pragma clang fp contract(off)
    using Float = float;
#else
    using Float = volatile float; // the stores keep the compiler from fusing the products
#endif
    const Float xx = a[0] * b[0];
    const Float yy = a[1] * b[1];
    const Float zz = a[2] * b[2];
    const Float ww = a[3] * b[3];
    const float dot = ((xx + yy) + zz) + ww;
    return dot < 0.f;
}

void TriangleBatch::AddTriangle(const OutputVertex& v0, const OutputVertex& v1, const OutputVertex& v2) {
    vertices_.push_back(MakeHardwareVertex(v0, false));
    vertices_.push_back(MakeHardwareVertex(v1, AreQuaternionsOpposite(v0.quat, v1.quat)));
    vertices_.push_back(MakeHardwareVertex(v2, AreQuaternionsOpposite(v0.quat, v2.quat)));
}

rhi::Result<StreamedBatch> UploadBatch(rhi::Ring& ring, const TriangleBatch& batch) {
    StreamedBatch out;
    if (batch.TriangleCount() == 0) {
        return out;
    }
    const u64 size = static_cast<u64>(batch.VertexCount()) * sizeof(HardwareVertex);
    // Azahar maps with the vertex size as alignment (88); the ring wants a power of two, and a
    // vertex buffer offset must be a multiple of the attributes' component size (4): 16 covers it.
    rhi::Result<rhi::RingAllocation> allocation = ring.Map(size, 16);
    if (!allocation.ok()) {
        return rhi::Error{allocation.error()};
    }
    std::memcpy(allocation->data.data(), batch.Data(), static_cast<size_t>(size));
    ring.GetBuffer()->Flush(allocation->offset, size);
    rhi::Result<void> committed = ring.Commit(size);
    if (!committed.ok()) {
        return rhi::Error{committed.error()};
    }
    out.offset = allocation->offset;
    out.vertex_count = static_cast<u32>(batch.VertexCount());
    return out;
}

} // namespace mh4n::gpu::cpuvertex
