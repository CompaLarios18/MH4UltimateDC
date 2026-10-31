// Derived from Azahar src/video_core/shader/generator/glsl_shader_gen.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later, as the header file of this module says): the SPIR-V in the
// included file is generated from shaders/trivial.vert, which carries the provenance.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_shader_gen.cpp)
#include "gpu/cpuvertex/trivial_vertex.h"

#include "gpu/cpuvertex/trivial_vertex_spv.inc"

namespace mh4n::gpu::cpuvertex {

std::span<const u32> TrivialVertexShader(bool use_clip_planes) {
    return use_clip_planes ? std::span<const u32>(spv_trivial_vertex_clip)
                           : std::span<const u32>(spv_trivial_vertex_noclip);
}

} // namespace mh4n::gpu::cpuvertex
