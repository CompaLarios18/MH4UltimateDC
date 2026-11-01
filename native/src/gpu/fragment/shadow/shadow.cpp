// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (the names and signatures of shadowTexture and shadowTextureCube, DefineShadowHelpers, WriteShadow) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
//
// The STUB of the `shadow/` feature (N2-08; the feature is N2-12's): the shadow lookups give 1.0 (lit), the write tail
// stores nothing, and a draw in the shadow-write mode, or one that looks a shadow texture up, is counted `stubbed:shadow`.
#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"

namespace mh4n::gpu::fragment {
namespace {

class ShadowStub final : public FeatureModule {
public:
    const char* Name() const override { return "shadow"; }

    void Emit(Point point, EmitContext& ctx) const override {
        if (point == Point::Declarations) {
            ctx.emitter->Raw(
                "vec4 shadowTexture(vec2 uv, float w) { return vec4(1.0); }\n"
                "vec4 shadowTextureCube(vec2 uv, float w) { return vec4(1.0); }\n");
        }
    }

    const char* Stubbed(const FragmentKey& key) const override {
        const auto type = key.texture.texture0_type();
        const bool lookup = (type == regenum::TextureType::Shadow2D || type == regenum::TextureType::ShadowCube) &&
                            TevUsesSource(key, 3);
        return key.framebuffer.shadow_rendering() != 0 || lookup ? "shadow" : nullptr;
    }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(ShadowStub)
}  // namespace mh4n::gpu::fragment
