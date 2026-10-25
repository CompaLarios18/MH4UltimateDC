// GLSL to SPIR-V with glslang as a library (N2-06; native/third_party/glslang). Not derived from
// Azahar: Azahar's Vulkan renderer compiles its generated GLSL the same way (vk_shader_util.cpp), this
// file is written from glslang's public API.
#include <chrono>
#include <mutex>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include "gpu/shader/vertex_shader.h"

namespace mh4n::gpu::shader {

namespace {

// glslang keeps process-wide tables; they are made once and never torn down (the process ends).
void InitializeGlslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

} // namespace

bool CompileGlslToSpirv(const std::string& source, std::vector<u32>& spirv, std::string& error,
                        double* milliseconds) {
    const auto start = std::chrono::steady_clock::now();
    InitializeGlslang();
    spirv.clear();
    error.clear();

    glslang::TShader shader(EShLangVertex);
    const char* strings[1] = {source.c_str()};
    shader.setStrings(strings, 1);
    // Vulkan 1.2 and SPIR-V 1.5 (the backend asks for a Vulkan 1.2 instance).
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangVertex, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        error = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        error = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }
    const glslang::TIntermediate* intermediate = program.getIntermediate(EShLangVertex);
    if (intermediate == nullptr) {
        error = "glslang produced no vertex stage";
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true; // glslang is built without spirv-tools
    options.validate = false;        // the tests run spirv-val
    std::vector<unsigned int> words;
    glslang::GlslangToSpv(*intermediate, words, &options);
    spirv.assign(words.begin(), words.end());
    if (milliseconds != nullptr) {
        *milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    return !spirv.empty();
}

} // namespace mh4n::gpu::shader
