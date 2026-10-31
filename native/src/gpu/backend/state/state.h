// Derived from Azahar src/video_core/shader/generator/pica_fs_config.cpp, pica_fs_config.h and
// src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: all three say "Licensed under GPLv3 or any later version"): which of the logic ops
// and of the min/max blend halves the pipeline does and which the shader does (FramebufferConfig::ApplyProfile,
// BlendConfig::UsesNoOpFactors), and the colour write mask 0 that stands for the emulated NoOp
// (PipelineInfo::GetFinalColorWriteMask). The rest is ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_fs_config.cpp, pica_fs_config.h,
//   vk_graphics_pipeline.cpp)
//
// The backend's state module (N2-13), beside rhi.h: what rhi.h does not declare because only the code that builds
// pipelines uses it. Namespace mh4n::gpu::rhi::state. The members of Recorder this module defines
// (SetBlendConstants, SetStencilReference, SetStencilMasks) are documented in rhi.h and docs/native/rhi.md.
#pragma once

#include <array>

#include "gpu/backend/rhi.h"

namespace mh4n::gpu::rhi::state {

// True for the eleven logic ops that read the destination colour (And, AndReverse, Invert, Nand, Or, Nor, Xor, Equiv,
// AndInverted, OrReverse, OrInverted); false for Clear, Copy, Set, CopyInverted and NoOp.
bool LogicOpNeedsDestination(LogicOp op);

// What a draw's fixed-function state becomes on this device, and what the shader must do for the rest.
struct PipelinePlan {
    // For GraphicsPipelineDesc::state.
    FixedFunctionState built;
    // The pipeline has blending off (no hardware logic op) and the shader applies the requested logic op to its
    // colour (mh4n_logic_op, all 16 ops). NoOp is not in the shader: the colour write mask is 0 (the depth write stays).
    bool shader_logic_op = false;
    // The pipeline blends this half ONE, ZERO, ADD and the shader computes it. Three cases, none depends on the device:
    //   - a half whose equation is Min or Max and whose factors are not both One (Vulkan's MIN and MAX ignore the
    //     factors, the PICA's do not): mh4n_minmax_blend with the requested equation for that half;
    //   - the colour half, whatever its equation, when the alpha half is the shader's and the colour factors read the
    //     source alpha (SrcAlpha, OneMinusSrcAlpha, SrcAlphaSaturate): the blender would read the alpha the shader
    //     replaced;
    //   - the colour half when it has a factor of each constant pair (ConstantColor and ConstantAlpha): one constant
    //     colour cannot give both, the shader reads the real blend colour.
    // The last two (and a Min or Max colour half too, if the caller prefers) are computed by mh4n_blend_rgb, from the
    // ORIGINAL source colour (rgb_uses_blend_rgb says when it is needed); the alpha half is always mh4n_minmax_blend.
    bool rgb_in_shader = false;
    bool alpha_in_shader = false;
    // rgb_in_shader and the requested colour equation is not Min or Max (Add, Subtract, ReverseSubtract): the colour
    // half needs mh4n_blend_rgb, because mh4n_minmax_blend leaves such a half alone. A Min or Max colour half needs
    // only mh4n_minmax_blend.
    bool rgb_uses_blend_rgb = false;
    // The shader reads the destination colour: FeedbackCopy the target before the draw and bind the twin.
    bool needs_destination_copy = false;
    // The vendored backend expresses only the constant-colour pair of blend factors, so a ConstantAlpha factor of the
    // colour half was rewritten as the ConstantColor one: pass (a, a, a, a) to SetBlendConstants
    // (EffectiveBlendConstants does it).
    bool constants_from_alpha = false;
};

// The request is the form fbops::MakeFixedFunctionState gives: the registers' blend equations and factors, the logic
// op as logic_op_enabled (blending off and an op other than Copy), the colour write mask. Applies the device profile:
//   - a requested logic op is the hardware's when caps.logic_op is on, else the shader's with blending off, and for
//     NoOp the colour write mask is 0;
//   - a Min or Max half whose factors are not both One is blended ONE, ZERO, ADD and the shader does it (and the
//     colour half too when the alpha half is the shader's and its factors read the source alpha);
//   - ConstantAlpha and OneMinusConstantAlpha, which core/ refuses, are rewritten as ConstantColor and
//     OneMinusConstantColor in the alpha half (where both read the constant's alpha) and in the colour half when no
//     ConstantColor factor is there too (then the constants are (a, a, a, a)); with both pairs in the colour half it
//     goes to the shader (constants_from_alpha stays false).
// Errors: a logic op with blending on (they are exclusive). A colour half with a factor of each constant pair
// (ConstantColor and ConstantAlpha) is computed by the shader (rgb_in_shader, mh4n_blend_rgb, with the real blend
// colour), since one constant colour cannot be both. A plume patch for the constant-alpha pair would make both this
// route and the rewrite unnecessary; until then the mixed half pays the destination copy. The blend state stays
// pipeline state on every device.
Result<PipelinePlan> PlanPipeline(const FixedFunctionState& requested, const Capabilities& caps);

// The same rule as a function of the flag, for code that holds only the flag.
std::array<float, 4> EffectiveBlendConstants(bool constants_from_alpha, const std::array<float, 4>& requested);

// The constants to give SetBlendConstants for the plan: the requested ones, or (a, a, a, a) when the plan rewrote the
// ConstantAlpha factors.
std::array<float, 4> EffectiveBlendConstants(const PipelinePlan& plan, const std::array<float, 4>& requested);

}  // namespace mh4n::gpu::rhi::state
