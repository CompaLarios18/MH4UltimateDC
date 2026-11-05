// Derived from Azahar src/video_core/shader/generator/glsl_shader_gen.cpp (GenerateFixedGeometryShader, GetGSCommonSource and
// the vertex interface it declares: the fixed geometry shader of lit draws) and src/video_core/shader/generator/shader_gen.h and
// shader_gen.cpp (PicaGSConfigState::GetSemanticMaps) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of those
// files says "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_shader_gen.cpp, shader_gen.h, shader_gen.cpp)
//
// The fixed geometry shader of the `lighting/` feature (N2-09; docs/native/gpu-fragment-design.md 5.5): the quaternion fix-up
// stage of a lit draw whose vertex stage is a translated one, on a device with geometry shaders. `lighting.cpp` registers
// `GenerateFixedGeometryShader` with N2-33's slot (`native::FixupGeneratorRegistry`) at start-up.
#pragma once

#include <string>
#include <vector>

#include "gpu/native/compose.h"
#include "mh4n/types.h"

namespace mh4n::gpu::fragment::lighting {

// The GLSL of the fixed geometry shader for a vertex stage's outputs (`triangles` in, `triangle_strip` out, three vertices).
// The vertex stage was translated with `ConfigOptions::geometry_stage`: it writes `vs_out_attr<n>` at locations 0 to
// `num_outputs - 1` and nothing else (vertex_shader.h). This stage writes what the vertex stage writes without it: `gl_Position`
// and the two clip distances (the renderer asks for clip planes) and the seven varyings at locations 1 to 7, each from the
// output attribute and component `semantic_map` assigns to its semantic (1.0 when none does), with the same SanitizeVertex, the
// same viewport flip and the same colour saturation. The only addition is the fix-up: vertex 1's and vertex 2's quaternions are
// negated when their dot product with vertex 0's is negative.
std::string GenerateFixedGeometryGlsl(const native::VertexStageOutputs& outputs);

// Compiles geometry-stage Vulkan GLSL (#version 450) to SPIR-V 1.5 with glslang. On failure `error` has glslang's log.
bool CompileGeometryGlslToSpirv(const std::string& source, std::vector<u32>& spirv, std::string& error);

// GLSL then SPIR-V; empty when either step fails (the pipeline builder then reports the build as failed).
std::vector<u32> GenerateFixedGeometryShader(const native::VertexStageOutputs& outputs);

}  // namespace mh4n::gpu::fragment::lighting
