// Derived from Azahar src/video_core/renderer_vulkan/pica_to_vk.h, vk_rasterizer.cpp (SyncDrawState),
// src/video_core/pica/regs_framebuffer.h, regs_rasterizer.h and regs_internal.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp says "Licensed under GPLv3 or any
// later version"; pica_to_vk.h, regs_framebuffer.h, regs_rasterizer.h and regs_internal.h say "GPLv2 or any later
// version"): the positions and fields of the framebuffer, rasterizer and output-merger registers, the tables from
// the PICA's enumerators to Vulkan's and the rules that make a draw's depth test, depth write, stencil test, colour
// write mask and dynamic state from them. The rest is ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_to_vk.h, vk_rasterizer.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h, regs_internal.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
#include "gpu/fbops/fixed_function.h"

namespace mh4n::gpu::fbops {

using namespace rhi;

namespace {

using pica::BitsOf;

// Register ids (RegsInternal: rasterizer at 0x40, FramebufferRegs at 0x100, its framebuffer block at 0x110).
constexpr u32 kCullMode = 0x40;
constexpr u32 kOutputMergerMode = 0x100;   // fragment_operation_mode 0-1, alphablend_enable 8
constexpr u32 kBlendEquations = 0x101;     // rgb eq 0-2, alpha eq 8-10, source rgb 16-19, dest rgb 20-23, source a 24-27, dest a 28-31
constexpr u32 kLogicOp = 0x102;            // 0-3
constexpr u32 kBlendConst = 0x103;         // r 0-7, g 8-15, b 16-23, a 24-31
constexpr u32 kStencilFunc = 0x105;        // enable 0, func 4-6, write mask 8-15, reference 16-23, input mask 24-31
constexpr u32 kStencilOp = 0x106;          // fail 0-2, depth fail 4-6, depth pass 8-10
constexpr u32 kDepthColorMask = 0x107;     // depth test 0, func 4-6, r g b a 8-11, depth write 12
constexpr u32 kAllowColorWrite = 0x113;    // bits 0-3
constexpr u32 kAllowDepthStencil = 0x115;  // bits 0-1
constexpr u32 kDepthFormat = 0x116;        // bits 0-1
constexpr u32 kFramebufferSize = 0x11E;    // flip bit 24

constexpr u32 kShadowMode = 3;     // FragmentOperationMode::Shadow
constexpr u32 kDepthFormatD24S8 = 3;

// pica_to_vk.h's tables, in the PICA's order. An index past the end is what Azahar's code returns for it: Add for an
// equation and One for a factor.
constexpr std::array<BlendOp, 5> kBlendOps = {BlendOp::Add, BlendOp::Subtract, BlendOp::ReverseSubtract, BlendOp::Min,
                                              BlendOp::Max};
constexpr std::array<BlendFactor, 15> kBlendFactors = {
    BlendFactor::Zero,        BlendFactor::One,
    BlendFactor::SrcColor,    BlendFactor::OneMinusSrcColor,
    BlendFactor::DstColor,    BlendFactor::OneMinusDstColor,
    BlendFactor::SrcAlpha,    BlendFactor::OneMinusSrcAlpha,
    BlendFactor::DstAlpha,    BlendFactor::OneMinusDstAlpha,
    BlendFactor::ConstantColor, BlendFactor::OneMinusConstantColor,
    BlendFactor::ConstantAlpha, BlendFactor::OneMinusConstantAlpha,
    BlendFactor::SrcAlphaSaturate,
};
constexpr std::array<LogicOp, 16> kLogicOps = {
    LogicOp::Clear, LogicOp::And,  LogicOp::AndReverse, LogicOp::Copy,        LogicOp::Set,       LogicOp::CopyInverted,
    LogicOp::NoOp,  LogicOp::Invert, LogicOp::Nand,     LogicOp::Or,          LogicOp::Nor,       LogicOp::Xor,
    LogicOp::Equiv, LogicOp::AndInverted, LogicOp::OrReverse, LogicOp::OrInverted,
};
constexpr std::array<CompareOp, 8> kCompareOps = {
    CompareOp::Never, CompareOp::Always,  CompareOp::Equal,   CompareOp::NotEqual,
    CompareOp::Less,  CompareOp::LessEqual, CompareOp::Greater, CompareOp::GreaterEqual,
};
constexpr std::array<StencilOp, 8> kStencilOps = {
    StencilOp::Keep,       StencilOp::Zero,        StencilOp::Replace,   StencilOp::IncrementClamp,
    StencilOp::DecrementClamp, StencilOp::Invert, StencilOp::IncrementWrap, StencilOp::DecrementWrap,
};

BlendOp ToBlendOp(u32 value) {
    return value < kBlendOps.size() ? kBlendOps[value] : BlendOp::Add;
}

BlendFactor ToBlendFactor(u32 value) {
    return value < kBlendFactors.size() ? kBlendFactors[value] : BlendFactor::One;
}

}  // namespace

FixedFunctionState MakeFixedFunctionState(const pica::PicaState& state) {
    const auto& regs = state.regs;
    FixedFunctionState out;

    // Cull mode and front face (pica_to_vk.h CullMode and FrontFace): the keep-one-winding modes cull the back, or
    // the front when the framebuffer is flipped.
    const u32 cull = BitsOf(regs[kCullMode], 0, 2);
    const bool flipped = BitsOf(regs[kFramebufferSize], 24, 1) == 0;
    out.cull = (cull == 1 || cull == 2) ? (flipped ? CullMode::Front : CullMode::Back) : CullMode::None;
    out.front_face = cull == 2 ? FrontFace::Clockwise : FrontFace::CounterClockwise;

    if (BitsOf(regs[kOutputMergerMode], 0, 2) == kShadowMode) {
        return out;
    }

    // Blending and the logic op.
    out.blend_enabled = BitsOf(regs[kOutputMergerMode], 8, 1) != 0;
    if (out.blend_enabled) {
        const u32 word = regs[kBlendEquations];
        out.color_op = ToBlendOp(BitsOf(word, 0, 3));
        out.alpha_op = ToBlendOp(BitsOf(word, 8, 3));
        out.src_color = ToBlendFactor(BitsOf(word, 16, 4));
        out.dst_color = ToBlendFactor(BitsOf(word, 20, 4));
        out.src_alpha = ToBlendFactor(BitsOf(word, 24, 4));
        out.dst_alpha = ToBlendFactor(BitsOf(word, 28, 4));
    } else {
        const LogicOp op = kLogicOps[BitsOf(regs[kLogicOp], 0, 4)];
        if (op != LogicOp::Copy) {
            out.logic_op_enabled = true;
            out.logic_op = op;
        }
    }

    // The colour write mask (SyncDrawState): the output merger's four bits, nothing when colour writes are off.
    const u32 depth_color = regs[kDepthColorMask];
    out.color_write_mask = BitsOf(regs[kAllowColorWrite], 0, 4) != 0 ? static_cast<u8>(BitsOf(depth_color, 8, 4)) : 0;

    // Depth (SyncDepthTest, SyncDepthWriteMask).
    const bool depth_test = BitsOf(depth_color, 0, 1) != 0;
    const bool depth_write = BitsOf(depth_color, 12, 1) != 0;
    if (depth_test || depth_write) {
        out.depth_test = true;
        out.depth_compare = depth_test ? kCompareOps[BitsOf(depth_color, 4, 3)] : CompareOp::Always;
        out.depth_write = BitsOf(regs[kAllowDepthStencil], 0, 2) != 0 && depth_write;
    }

    // Stencil (SyncStencilTest): only with a D24S8 depth buffer.
    const u32 func = regs[kStencilFunc];
    if (BitsOf(func, 0, 1) != 0 && BitsOf(regs[kDepthFormat], 0, 2) == kDepthFormatD24S8) {
        const u32 ops = regs[kStencilOp];
        StencilFaceState face;
        face.compare = kCompareOps[BitsOf(func, 4, 3)];
        face.fail = kStencilOps[BitsOf(ops, 0, 3)];
        face.depth_fail = kStencilOps[BitsOf(ops, 4, 3)];
        face.pass = kStencilOps[BitsOf(ops, 8, 3)];
        out.stencil_test = true;
        out.stencil_front = face;
        out.stencil_back = face;
    }
    return out;
}

DynamicState MakeDynamicState(const pica::PicaState& state) {
    const auto& regs = state.regs;
    DynamicState out;
    const u32 constant = regs[kBlendConst];
    for (u32 channel = 0; channel < 4; ++channel) {
        out.blend_constants[channel] = static_cast<float>(BitsOf(constant, 8 * channel, 8)) / 255.0f;
    }
    const u32 func = regs[kStencilFunc];
    out.stencil_reference = BitsOf(func, 16, 8);
    out.stencil_compare_mask = BitsOf(func, 24, 8);
    out.stencil_write_mask = BitsOf(regs[kAllowDepthStencil], 0, 2) != 0 ? BitsOf(func, 8, 8) : 0;
    return out;
}

void ApplyDynamicState(Recorder& recorder, const DynamicState& dynamic, bool constants_from_alpha) {
    std::array<float, 4> constants = dynamic.blend_constants;
    if (constants_from_alpha) {
        constants = {constants[3], constants[3], constants[3], constants[3]};
    }
    recorder.SetBlendConstants(constants);
    recorder.SetStencilReference(dynamic.stencil_reference);
    recorder.SetStencilMasks(dynamic.stencil_compare_mask, dynamic.stencil_write_mask);
}

}  // namespace mh4n::gpu::fbops
