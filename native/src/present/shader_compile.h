// GLSL to SPIR-V for the compositor's own shaders (N2b-05), with N2-06's glslang (native/third_party/glslang).
// gpu/shader/vertex_shader.h compiles vertex shaders only; the compositor needs a fragment stage too.
#pragma once

#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::present {

enum class GlslStage { Vertex, Fragment };

// Compiles Vulkan GLSL (#version 450) to SPIR-V 1.5 (Vulkan 1.2, the backend's instance). On failure `error`
// has glslang's log. Safe to call from several threads.
bool CompileGlsl(GlslStage stage, const std::string& source, std::vector<u32>& spirv, std::string& error);

} // namespace mh4n::present
