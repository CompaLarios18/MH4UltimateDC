// Derived from Azahar src/video_core/shader/generator/glsl_shader_gen.cpp (GenerateTrivialVertexShader),
// shader_gen.h and glsl_fs_shader_gen.cpp (DefineInterface) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// through shaders/trivial.vert, which this header exposes compiled (GPL-3.0-or-later: glsl_fs_shader_gen.cpp
// says "Licensed under GPLv3 or any later version"; glsl_shader_gen.cpp and shader_gen.h say GPLv2 or
// any later version).
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_shader_gen.cpp, shader_gen.h,
//   glsl_fs_shader_gen.cpp)
//
// The trivial vertex stage (N2-37) as SPIR-V, compiled by tools/native/spirv_inc.py from
// shaders/trivial.vert into trivial_vertex_spv.inc (committed: no glslang at run time; regenerate with
// the command in shaders/spirv_inc.txt). Use it to draw a TriangleBatch (triangle_batch.h, which says how
// N2-08 wires it). Azahar's trivial shader writes `gl_Position` (y negated when flip_viewport, z negated,
// SanitizeVertex) and, with use_clip_planes, `gl_ClipDistance[0..1]` (-z; dot(clip_coef, pos) when
// enable_clip1, else 0), plus the seven varyings; both variants are here, with the uniform block layout of
// N2-06's `VertexUniformData` (set 0, binding 1, std140, 32 bytes):
//
//   TrivialVertexShader(true)   writes gl_ClipDistance[2]: N2-08 picks it when the translated vertex stage
//                               is made with use_clip_planes (the default; needs the device feature
//                               shaderClipDistance)
//   TrivialVertexShader(false)  does not
//
// Inputs: locations 0 to 7 as MakeBatchVertexLayout(). Outputs: locations 1 to 7 (primary_color,
// texcoord0, texcoord1, texcoord2, texcoord0_w, normquat, view).
#pragma once

#include <span>

#include "mh4n/types.h"

namespace mh4n::gpu::cpuvertex {

std::span<const u32> TrivialVertexShader(bool use_clip_planes);

} // namespace mh4n::gpu::cpuvertex
