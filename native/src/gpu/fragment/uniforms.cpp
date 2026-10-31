// Derived from Azahar src/video_core/rasterizer_accelerated.cpp (SyncDrawUniforms, ColorRGBA8),
// src/video_core/pica/dirty_regs.h (CheckDepth, CheckBlend, CheckTexEnv: which registers set which
// uniform), src/video_core/pica_types.h (f24::FromRaw) and src/video_core/renderer_vulkan/vk_rasterizer.cpp
// (the scissor update at 587-596), and the fields of src/video_core/pica/regs_framebuffer.h (the alpha test
// reference), regs_rasterizer.h (the scissor and depth range words) and regs_texturing.h (the TEV constant colour
// words), at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp says "Licensed under GPLv3
// or any later version"; the others say "GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_accelerated.cpp,
//   vk_rasterizer.cpp)
// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project (dirty_regs.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
#include "gpu/fragment/uniforms.h"

#include <cstring>

#include "gpu/fragment/feature.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
Vec4f ColorRGBA8(u32 word) {
    return {static_cast<float>(word & 0xFF) / 255.0f, static_cast<float>((word >> 8) & 0xFF) / 255.0f,
            static_cast<float>((word >> 16) & 0xFF) / 255.0f, static_cast<float>((word >> 24) & 0xFF) / 255.0f};
}

void FillCoreUniforms(const pica::PicaState& state, FragmentUniforms& uniforms, UniformDirty& dirty,
                      const UniformContext& context) {
    const auto& regs = state.regs;

    // Depth testing uniforms (CheckDepth).
    if (dirty.all || state.dirty.Test(freg::kViewportDepthRange) || state.dirty.Test(freg::kViewportDepthNearPlane)) {
        uniforms.depth_scale = pica::Float24ToFloat(Bits(regs[freg::kViewportDepthRange], 0, 24));
        uniforms.depth_offset = pica::Float24ToFloat(Bits(regs[freg::kViewportDepthNearPlane], 0, 24));
        dirty.Mark();
    }

    // Alpha testing and blending uniforms (CheckBlend).
    if (dirty.all || state.dirty.Test(freg::kBlendConst) || state.dirty.Test(freg::kAlphaTest)) {
        uniforms.alphatest_ref = static_cast<int>(Bits(regs[freg::kAlphaTest], 8, 8));
        uniforms.blend_color = ColorRGBA8(regs[freg::kBlendConst]);
        dirty.Mark();
    }

    // Texenv uniforms (CheckTexEnv): the combiner buffer colour and the six constants.
    static constexpr u32 kStageBase[6] = {freg::kTevStage0, freg::kTevStage1, freg::kTevStage2,
                                          freg::kTevStage3, freg::kTevStage4, freg::kTevStage5};
    bool texenv = dirty.all || state.dirty.Test(freg::kTevCombinerBufferColor);
    for (u32 stage : kStageBase) {
        texenv = texenv || state.dirty.Test(stage + freg::kTevConstColor);
    }
    if (texenv) {
        for (u32 i = 0; i < 6; ++i) {
            uniforms.const_color[i] = ColorRGBA8(regs[kStageBase[i] + freg::kTevConstColor]);
        }
        uniforms.tev_combiner_buffer_color = ColorRGBA8(regs[freg::kTevCombinerBufferColor]);
        dirty.Mark();
    }

    // The scale and the scissor rectangle depend on the surface cache's context, so they are compared with what the
    // block holds (Azahar compares the four values the same way).
    UniformContext::Scissor scissor;
    if (context.scissor) {
        scissor = *context.scissor;
    } else {
        const u32 xy1 = regs[freg::kScissorXY1];
        const u32 xy2 = regs[freg::kScissorXY2];
        const int scale = context.framebuffer_scale;
        scissor.x1 = static_cast<int>(Bits(xy1, 0, 10)) * scale;
        scissor.y1 = static_cast<int>(Bits(xy1, 16, 10)) * scale;
        scissor.x2 = (static_cast<int>(Bits(xy2, 0, 10)) + 1) * scale;
        scissor.y2 = (static_cast<int>(Bits(xy2, 16, 10)) + 1) * scale;
    }
    if (uniforms.scissor_x1 != scissor.x1 || uniforms.scissor_y1 != scissor.y1 || uniforms.scissor_x2 != scissor.x2 ||
        uniforms.scissor_y2 != scissor.y2) {
        uniforms.scissor_x1 = scissor.x1;
        uniforms.scissor_y1 = scissor.y1;
        uniforms.scissor_x2 = scissor.x2;
        uniforms.scissor_y2 = scissor.y2;
        dirty.Mark();
    }
    if (uniforms.framebuffer_scale != context.framebuffer_scale) {
        uniforms.framebuffer_scale = context.framebuffer_scale;
        dirty.Mark();
    }
}

void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty, const UniformContext& context) {
    FillUniforms(state, block, dirty, context, FeatureRegistry::Global());
}

void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty, const UniformContext& context,
                  const FeatureRegistry& features) {
    FillCoreUniforms(state, block.uniforms, dirty, context);
    for (const FeatureModule* feature : features.All()) {
        feature->FillUniforms(state, block, dirty);
    }
}

bool StoreKey(const FragmentKey& key, FragmentBlock& block) {
    if (block.cfg == key) {
        return false;
    }
    block.cfg = key;
    return true;
}

} // namespace mh4n::gpu::fragment
