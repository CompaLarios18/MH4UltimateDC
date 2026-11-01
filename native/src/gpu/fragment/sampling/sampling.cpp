// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (the names and signatures of sampleTexUnit0 to sampleTexUnit3, DefineTexUnitSampler) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
//
// The STUB of the `sampling/` feature (N2-08; the feature is N2-23's): the four texture units give opaque white, so a TEV
// stage that samples gives the same result for every draw, and a draw that reads a texture unit is counted
// `stubbed:sampling`. N2-23 replaces this file's body (it keeps the class name's role: one FeatureModule named "sampling").
#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"

namespace mh4n::gpu::fragment {
namespace {

class SamplingStub final : public FeatureModule {
public:
    const char* Name() const override { return "sampling"; }

    // The prototypes of sampleTexUnit0 .. 3 (core's prototype block) defined with a neutral value.
    void Emit(Point point, EmitContext& ctx) const override {
        if (point != Point::Declarations) {
            return;
        }
        ctx.emitter->Raw(
            "vec4 sampleTexUnit0() { return vec4(1.0); }\n"
            "vec4 sampleTexUnit1() { return vec4(1.0); }\n"
            "vec4 sampleTexUnit2() { return vec4(1.0); }\n"
            "vec4 sampleTexUnit3() { return vec4(1.0); }\n");
    }

    // A texture unit is read by a TEV stage, or lighting reads a bump map.
    const char* Stubbed(const FragmentKey& key) const override {
        const bool tev = TevUsesSource(key, 3) || TevUsesSource(key, 4) || TevUsesSource(key, 5);
        const bool bump = key.lighting.enable() != 0 && key.lighting.bump_mode() != regenum::LightingBumpMode::None;
        return tev || bump ? "sampling" : nullptr;
    }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(SamplingStub)
}  // namespace mh4n::gpu::fragment
