// Derived from Azahar src/video_core/shader/generator/pica_fs_config.cpp, pica_fs_config.h and
// src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: all three say "Licensed under GPLv3 or any later version"): the device profile applied to
// the requested logic op and blend halves (FramebufferConfig::ApplyProfile, BlendConfig::UsesNoOpFactors), the
// ONE, ZERO, ADD blend of an emulated min/max half and the colour write mask 0 of an emulated NoOp
// (GraphicsPipeline::Build, PipelineInfo::GetFinalColorWriteMask). The rest is ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_fs_config.cpp, pica_fs_config.h,
//   vk_graphics_pipeline.cpp)
#include "gpu/backend/state/state.h"

namespace mh4n::gpu::rhi::state {

namespace {

bool IsMinMax(BlendOp op) {
    return op == BlendOp::Min || op == BlendOp::Max;
}

// A half whose equation is Min or Max and whose factors are not both One goes to the shader; the pipeline then blends
// ONE, ZERO, ADD, which leaves the shader's colour as it is.
bool MoveMinMaxToShader(BlendOp& op, BlendFactor& source, BlendFactor& destination) {
    if (!IsMinMax(op) || (source == BlendFactor::One && destination == BlendFactor::One)) {
        return false;
    }
    op = BlendOp::Add;
    source = BlendFactor::One;
    destination = BlendFactor::Zero;
    return true;
}

bool ReadsSourceAlpha(BlendFactor factor) {
    return factor == BlendFactor::SrcAlpha || factor == BlendFactor::OneMinusSrcAlpha ||
           factor == BlendFactor::SrcAlphaSaturate;
}

bool IsColorPair(BlendFactor factor) {
    return factor == BlendFactor::ConstantColor || factor == BlendFactor::OneMinusConstantColor;
}

bool IsAlphaPair(BlendFactor factor) {
    return factor == BlendFactor::ConstantAlpha || factor == BlendFactor::OneMinusConstantAlpha;
}

BlendFactor AsColorPair(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::ConstantAlpha: return BlendFactor::ConstantColor;
    case BlendFactor::OneMinusConstantAlpha: return BlendFactor::OneMinusConstantColor;
    default: return factor;
    }
}

}  // namespace

bool LogicOpNeedsDestination(LogicOp op) {
    switch (op) {
    case LogicOp::Clear:
    case LogicOp::Copy:
    case LogicOp::Set:
    case LogicOp::CopyInverted:
    case LogicOp::NoOp: return false;
    default: return true;
    }
}

Result<PipelinePlan> PlanPipeline(const FixedFunctionState& requested, const Capabilities& caps) {
    if (requested.logic_op_enabled && requested.blend_enabled) {
        return Error{"a logic op and blending are exclusive"};
    }
    PipelinePlan plan;
    FixedFunctionState& built = plan.built;
    built = requested;
    built.color_write_mask = requested.color_write_mask & kWriteAll;

    // The logic op. Copy is the colour as it is, so it is neither.
    built.logic_op_enabled = false;
    built.logic_op = FixedFunctionState{}.logic_op;
    if (requested.logic_op_enabled && requested.logic_op != LogicOp::Copy) {
        if (caps.logic_op) {
            built.logic_op_enabled = true;
            built.logic_op = requested.logic_op;
        } else if (requested.logic_op == LogicOp::NoOp) {
            built.color_write_mask = 0;  // the colour is discarded, the depth write stays
        } else {
            plan.shader_logic_op = true;
        }
    }

    if (built.blend_enabled) {
        plan.rgb_in_shader = MoveMinMaxToShader(built.color_op, built.src_color, built.dst_color);
        plan.alpha_in_shader = MoveMinMaxToShader(built.alpha_op, built.src_alpha, built.dst_alpha);
        if (plan.alpha_in_shader && !plan.rgb_in_shader &&
            (ReadsSourceAlpha(built.src_color) || ReadsSourceAlpha(built.dst_color))) {
            // The shader writes the alpha half into the colour's alpha, and the blender would take that for the source
            // alpha of its factors: the colour half goes to the shader too.
            plan.rgb_in_shader = true;
            built.color_op = BlendOp::Add;
            built.src_color = BlendFactor::One;
            built.dst_color = BlendFactor::Zero;
        }

        // The alpha half reads the constant's alpha through either pair.
        built.src_alpha = AsColorPair(built.src_alpha);
        built.dst_alpha = AsColorPair(built.dst_alpha);
        const bool colors = IsColorPair(built.src_color) || IsColorPair(built.dst_color);
        const bool alphas = IsAlphaPair(built.src_color) || IsAlphaPair(built.dst_color);
        if (colors && alphas) {
            return Error{"the colour half uses ConstantColor and ConstantAlpha factors together: the vendored backend "
                         "expresses only the constant-colour pair, which cannot be both"};
        }
        if (alphas) {
            built.src_color = AsColorPair(built.src_color);
            built.dst_color = AsColorPair(built.dst_color);
            plan.constants_from_alpha = true;
        }
    }

    plan.needs_destination_copy = (plan.shader_logic_op && LogicOpNeedsDestination(requested.logic_op)) ||
                                  plan.rgb_in_shader || plan.alpha_in_shader;
    return plan;
}

std::array<float, 4> EffectiveBlendConstants(const PipelinePlan& plan, const std::array<float, 4>& requested) {
    if (!plan.constants_from_alpha) {
        return requested;
    }
    return {requested[3], requested[3], requested[3], requested[3]};
}

}  // namespace mh4n::gpu::rhi::state
