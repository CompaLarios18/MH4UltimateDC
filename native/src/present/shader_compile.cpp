// GLSL to SPIR-V with glslang as a library (N2b-05). Not derived from Azahar: written from glslang's public
// API, the way gpu/shader/spirv_compile.cpp (N2-06) does it for the vertex stage.
#include "present/shader_compile.h"

#include <mutex>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

namespace mh4n::present {

namespace {

// glslang keeps process-wide tables; they are made once and never torn down (the process ends).
void InitializeGlslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

} // namespace

bool CompileGlsl(GlslStage stage, const std::string& source, std::vector<u32>& spirv, std::string& error) {
    InitializeGlslang();
    spirv.clear();
    error.clear();
    const EShLanguage language = stage == GlslStage::Vertex ? EShLangVertex : EShLangFragment;

    glslang::TShader shader(language);
    const char* strings[1] = {source.c_str()};
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
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
    const glslang::TIntermediate* intermediate = program.getIntermediate(language);
    if (intermediate == nullptr) {
        error = "glslang produced no stage";
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true; // glslang is built without spirv-tools
    options.validate = false;
    std::vector<unsigned int> words;
    glslang::GlslangToSpv(*intermediate, words, &options);
    spirv.assign(words.begin(), words.end());
    return !spirv.empty();
}

} // namespace mh4n::present
