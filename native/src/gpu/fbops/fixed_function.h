// Derived from Azahar src/video_core/renderer_vulkan/pica_to_vk.h, vk_rasterizer.cpp (SyncDrawState),
// src/video_core/pica/regs_framebuffer.h, regs_rasterizer.h and regs_internal.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp says "Licensed under GPLv3 or any
// later version"; pica_to_vk.h, regs_framebuffer.h, regs_rasterizer.h and regs_internal.h say "GPLv2 or any later
// version"): the positions and fields of the framebuffer, rasterizer and output-merger registers, the tables from
// the PICA's enumerators to Vulkan's (blend equations and factors, logic ops, compare functions, stencil actions,
// cull modes) and the rules that make a draw's depth test, depth write, stencil test, colour write mask and
// dynamic state from them. The rest is ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_to_vk.h, vk_rasterizer.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h, regs_internal.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
//
// The framebuffer operations of the registers (N2-13): rhi.h's FixedFunctionState in its requested form, and the
// dynamic state a draw sets before it. Namespace mh4n::gpu::fbops; this header names rhi.h types only.
#pragma once

#include <array>

#include "gpu/backend/rhi.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fbops {

// The state the registers request, which is the form N2-33's PipelineKey stores (the device profile is applied by
// rhi::state::PlanPipeline when the pipeline is built):
//   - blending: the enable bit; with it on, the two equations, the four factors as the registers have them (the
//     Min and Max equations and the constant-alpha factors included); with it off, none of them (the defaults), so
//     draws that differ in dead registers share a key;
//   - the logic op: logic_op_enabled when blending is off and the op is not Copy, with the op; otherwise off with the
//     default op;
//   - the colour write mask: the output merger's four bits, 0 when the framebuffer's colour write is off;
//   - depth: the test is on when the register's test or write is on (Vulkan writes depth only with the test on),
//     comparing with the register's function, or Always when only the write is on; the write also needs the
//     framebuffer's depth write; with neither, all three are the defaults;
//   - stencil: on only when the register says so and the depth format is D24S8; the function and the three actions
//     (Increment and Decrement clamp), the same for both faces; off, they are the defaults;
//   - the cull mode and front face as Azahar maps them, with the framebuffer's flip bit (flipped when it is clear);
//   - no depth bias (the PICA has none).
// A draw in shadow mode (fragment_operation_mode 3) renders into a pass with no attachments: its state is inert
// (no blend, logic op, depth or stencil), only the cull mode and front face stay.
rhi::FixedFunctionState MakeFixedFunctionState(const pica::PicaState& state);

// The values of the dynamic-state calls (rhi.h: always dynamic, never in a pipeline).
struct DynamicState {
    // The blend_const register as 0..1 floats (r, g, b, a).
    std::array<float, 4> blend_constants{};
    // The stencil registers; the write mask is 0 when the framebuffer's depth/stencil write is off.
    u32 stencil_reference = 0;
    u32 stencil_compare_mask = 0;
    u32 stencil_write_mask = 0;

    bool operator==(const DynamicState&) const = default;
};
DynamicState MakeDynamicState(const pica::PicaState& state);

// Calls SetBlendConstants, SetStencilReference and SetStencilMasks on the recorder (rhi.h: a draw whose pipeline blends
// with a constant factor or tests the stencil must be preceded by them in its recorder; call it once per recorder
// and again when a value changes). `constants_from_alpha` is state::PipelinePlan::constants_from_alpha: the blend
// constants then are (a, a, a, a).
void ApplyDynamicState(rhi::Recorder& recorder, const DynamicState& dynamic, bool constants_from_alpha = false);

}  // namespace mh4n::gpu::fbops
