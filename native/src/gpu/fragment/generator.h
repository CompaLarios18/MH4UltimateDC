// Written for this package from the design: nothing is copied from Azahar in this header (the generator is generator.cpp).
//
// The fragment generator (N2-08; docs/native/gpu-fragment-design.md 4.4 and 4.8, option 1): the GLSL of the PICA's fragment
// stage, in two modes from one source (emitter.h):
//
//   GenerateSpecialised(key, profile, pass)   one shader for one fragment key, every field a literal
//   GenerateUber(pass, profile)               the ubershader: the same code reading the key from `cfg` at run time, one
//                                             module per pass kind (`uber_color` for Normal, `uber_shadow_write` for
//                                             ShadowWrite), built once per device profile (callers cache it)
//
// The shaders are Vulkan GLSL 450 with `GL_EXT_samplerless_texture_functions`, compiled to SPIR-V with N2-06's glslang
// (`shader::CompileGlslToSpirv`). They use the one pipeline layout of native/bindings.h: set 0 the vertex stage's two
// uniform buffers (0, 1), the fragment block `fs_data` (2) and the three texel buffers (3 to 5); set 1 the textures and
// samplers; set 2 the shadow image (the ShadowWrite pass only).
//
// WHAT CORE EMITS, in Azahar's order (`FragmentModule::Generate`), calling the registered features at each `Point`:
//   Declarations  the extensions, the 7 varyings, the block, the bindings, Azahar's helper functions and the PROTOTYPE BLOCK
//                 (`sampleTexUnit0` .. `3`, `shadowTexture`, `shadowTextureCube`, `ProcTex`); then each feature's own
//                 declarations and definitions (the stubs define every prototype)
//   Prologue      the primary colour's rounding; discard everything when the alpha test is Never; the features' Prologue
//   ScissorDepth  `WriteScissor`, `WriteDepth`; the features' ScissorDepth
//   Lighting      the features (primary_fragment_color, secondary_fragment_color)
//   Tev           the combiner variables, the features' Tev, then the six stages
//   AlphaTest     the alpha test; the features' AlphaTest
//   FogGas        the features (the fog stub: transparent black and an early return for the Gas mode)
//   Output        Normal pass: the depth write, byte rounding, `mh4n_minmax_blend`, `mh4n_logic_op` where the profile says
//                 emulate; ShadowWrite pass: nothing of core's; then the features' Output (shadow/'s write tail)
//
// Variables the features may use inside `main` (all global or local to main, named as Azahar names them): `rounded_primary_color`,
// `primary_fragment_color`, `secondary_fragment_color`, `combiner_buffer`, `combiner_output`, `depth`, `color`,
// `texture_buffer_lut_lf` / `_rg` / `_rgba`, the members of `fs_data`, `cfg` (the key as `uvec4 cfg[18]`), `tex_color`,
// `shadow_buffer` (ShadowWrite pass), and in the ShadowWrite pass with the `interlock` bit `beginInvocationInterlock()` and
// `endInvocationInterlock()`.
#pragma once

#include <string>
#include <vector>

#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/key.h"

namespace mh4n::gpu::fragment {

// The GLSL text.
std::string GenerateSpecialisedGlsl(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
                                    const FeatureRegistry& features);
std::string GenerateSpecialisedGlsl(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass);
std::string GenerateUberGlsl(ShaderPass pass, const DeviceProfile& profile, const FeatureRegistry& features);
std::string GenerateUberGlsl(ShaderPass pass, const DeviceProfile& profile);

// Compiles fragment-stage Vulkan GLSL (#version 450) to SPIR-V 1.5 with glslang. On failure `error` has glslang's log.
bool CompileFragmentGlslToSpirv(const std::string& source, std::vector<u32>& spirv, std::string& error,
                                double* milliseconds = nullptr);

// The text compiled to SPIR-V (glslang, on the calling thread: milliseconds for a specialised shader).
struct Generated {
    std::string glsl;
    std::vector<u32> spirv;
    std::string error;  // glslang's log when spirv is empty
    double compile_ms = 0.0;
    bool ok() const { return !spirv.empty(); }
};
Generated GenerateSpecialised(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass,
                              const FeatureRegistry& features);
Generated GenerateSpecialised(const FragmentKey& key, const DeviceProfile& profile, ShaderPass pass);
Generated GenerateUber(ShaderPass pass, const DeviceProfile& profile, const FeatureRegistry& features);
Generated GenerateUber(ShaderPass pass, const DeviceProfile& profile);

// ---- helpers the stubs and the tests share ----------------------------------------------------------------------------

// Azahar's IsPassThroughTevStage on the key's words.
bool TevStageIsPassThrough(const TevStageConfigRaw& stage);

// Some stage that is not a pass-through reads `source` (Azahar's TevStageConfig::Source value: Texture0 is 3, Texture3
// (the procedural slot) is 6) as one of the sources its operation uses.
bool TevUsesSource(const FragmentKey& key, u32 source);

}  // namespace mh4n::gpu::fragment
