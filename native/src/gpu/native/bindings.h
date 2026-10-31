// The one pipeline layout of the fragment pipeline (N2-33; docs/native/gpu-fragment-design.md 5.6 and 5.10).
//
// Written for this package from the design: nothing is copied from Azahar here (Azahar's binding numbers were read
// for the order, `DefineBindingsVK` in glsl_fs_shader_gen.cpp).
//
// ONE layout for every module, the superset of 5.6 and 5.10: the specialised shaders and the ubershaders use the
// same sets, so a switch from one to the other never changes a binding. A shader declares the bindings it uses with
// the numbers below (`layout(set = S, binding = B)`); the ones it does not use stay bound to neutral resources.
//
//   set 0  uniforms and tables (the frame-rate set: rewritten when an offset changes)
//     0  uniform buffer   PicaUniformData (1,616 bytes)   the vertex stage's (N2-06)
//     1  uniform buffer   VertexUniformData (32 bytes)    the vertex stage's (N2-06)
//     2  uniform buffer   the fragment block (FragmentBlock: uniforms, cfg, gas), Azahar's binding for it
//     3  texel buffer     lut_lf    R32G32_SFLOAT       lighting and fog tables
//     4  texel buffer     lut_rg    R32G32_SFLOAT       procedural noise, colour map, alpha map
//     5  texel buffer     lut_rgba  R32G32B32A32_SFLOAT procedural colour table and difference table
//   set 1  textures (per draw)
//     0  sampled image    tex0 (2D)         unit 0 as texture2D (also projection)
//     1  sampled image    tex0_cube         unit 0 as textureCube
//     2  sampled image    tex0_shadow       unit 0 as the shadow map's R32_UINT view (utexture2D; texelFetch only)
//     3  sampler          samp0             unit 0's sampler (2D and cube; the shadow class has none)
//     4  sampled image    tex1              unit 1
//     5  sampler          samp1
//     6  sampled image    tex2              unit 2
//     7  sampler          samp2
//     8  sampled image    tex_color         the destination copy (FeedbackCopy's twin; texelFetch only)
//   set 2  the shadow write
//     0  storage image    shadow_out        the shadow source's R32_UINT view
//
// Unit 0's three classes are separate bindings; the draw binds a one-texel dummy for the classes it does not use,
// so one module serves every texture-0 type (design 4.4.1). A feature that samples reads through the prototype block
// `vec4 sampleTexUnit0()` .. `3`, never these names directly.
#pragma once

#include <vector>

#include "gpu/backend/rhi.h"

namespace mh4n::gpu::native {

namespace set {
inline constexpr u32 kUniforms = 0;
inline constexpr u32 kTextures = 1;
inline constexpr u32 kShadow = 2;
}  // namespace set

namespace binding {
// set 0
inline constexpr u32 kVsPicaUniforms = 0;
inline constexpr u32 kVsUniforms = 1;
inline constexpr u32 kFragmentBlock = 2;
inline constexpr u32 kLutLf = 3;
inline constexpr u32 kLutRg = 4;
inline constexpr u32 kLutRgba = 5;
// set 1
inline constexpr u32 kTex0 = 0;
inline constexpr u32 kTex0Cube = 1;
inline constexpr u32 kTex0Shadow = 2;
inline constexpr u32 kSampler0 = 3;
inline constexpr u32 kTex1 = 4;
inline constexpr u32 kSampler1 = 5;
inline constexpr u32 kTex2 = 6;
inline constexpr u32 kSampler2 = 7;
inline constexpr u32 kTexColor = 8;
// set 2
inline constexpr u32 kShadowOut = 0;
}  // namespace binding

// The three set shapes, in set order (what `GraphicsPipelineDesc::sets` holds).
inline const std::vector<rhi::BindingSetDesc>& PipelineLayoutSets() {
    static const std::vector<rhi::BindingSetDesc> sets = {
        rhi::BindingSetDesc{{
            {binding::kVsPicaUniforms, rhi::BindingType::UniformBuffer},
            {binding::kVsUniforms, rhi::BindingType::UniformBuffer},
            {binding::kFragmentBlock, rhi::BindingType::UniformBuffer},
            {binding::kLutLf, rhi::BindingType::TexelBuffer},
            {binding::kLutRg, rhi::BindingType::TexelBuffer},
            {binding::kLutRgba, rhi::BindingType::TexelBuffer},
        }},
        rhi::BindingSetDesc{{
            {binding::kTex0, rhi::BindingType::SampledImage},
            {binding::kTex0Cube, rhi::BindingType::SampledImage},
            {binding::kTex0Shadow, rhi::BindingType::SampledImage},
            {binding::kSampler0, rhi::BindingType::Sampler},
            {binding::kTex1, rhi::BindingType::SampledImage},
            {binding::kSampler1, rhi::BindingType::Sampler},
            {binding::kTex2, rhi::BindingType::SampledImage},
            {binding::kSampler2, rhi::BindingType::Sampler},
            {binding::kTexColor, rhi::BindingType::SampledImage},
        }},
        rhi::BindingSetDesc{{
            {binding::kShadowOut, rhi::BindingType::StorageImage},
        }},
    };
    return sets;
}

}  // namespace mh4n::gpu::native
