// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (WriteFog and WriteGas, FragmentModule::Generate's fog_mode switch) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
//
// The STUB of the `fog/` feature (N2-08; the feature is N2-10's): the fog is not applied (a draw with fog mode Fog is
// counted `stubbed:fog`); the Gas mode does what Azahar's WriteGas does, which is all anybody draws for gas: the output
// colour is transparent black and the shader returns without writing depth (`stubbed:gas`).
#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"

namespace mh4n::gpu::fragment {
namespace {

constexpr u32 kFogGas = 7;

class FogStub final : public FeatureModule {
public:
    const char* Name() const override { return "fog"; }

    void Emit(Point point, EmitContext& ctx) const override {
        if (point != Point::FogGas) {
            return;
        }
        Emitter& e = *ctx.emitter;
        // WriteGas: replace the output color with a transparent pixel (just discarding the pixel causes graphical issues in
        // some MH games) and return.
        e.If(e.Eq(e.Field(keyword::kTexRaw, 12, 3), kFogGas), [&] { e.Raw("color = vec4(0.0);\nreturn;\n"); });
    }

    const char* Stubbed(const FragmentKey& key) const override {
        switch (key.texture.fog_mode()) {
        case regenum::FogMode::Fog: return "fog";
        case regenum::FogMode::Gas: return "gas";
        default: return nullptr;
        }
    }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(FogStub)
}  // namespace mh4n::gpu::fragment
