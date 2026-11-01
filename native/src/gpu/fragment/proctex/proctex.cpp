// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (the names and signature of ProcTex, DefineProcTexSampler) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
//
// The STUB of the `proctex/` feature (N2-08; the feature is N2-11's): the procedural texture gives opaque white, and a draw
// whose TEV reads the procedural slot with the procedural texture enabled is counted `stubbed:proctex`.
#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"

namespace mh4n::gpu::fragment {
namespace {

class ProcTexStub final : public FeatureModule {
public:
    const char* Name() const override { return "proctex"; }

    void Emit(Point point, EmitContext& ctx) const override {
        if (point == Point::Declarations) {
            ctx.emitter->Raw("vec4 ProcTex() { return vec4(1.0); }\n");
        }
    }

    const char* Stubbed(const FragmentKey& key) const override {
        return key.proctex.enable() != 0 && TevUsesSource(key, 6) ? "proctex" : nullptr;
    }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(ProcTexStub)
}  // namespace mh4n::gpu::fragment
