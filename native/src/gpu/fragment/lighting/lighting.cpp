// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (FragmentModule::Generate: WriteLighting is called only when config.lighting.enable) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
//
// The STUB of the `lighting/` feature (N2-08; the feature is N2-09's): nothing is emitted at its points, so
// `primary_fragment_color` and `secondary_fragment_color` stay zero as they do in Azahar with lighting off, and a draw
// that enables lighting is counted `stubbed:lighting`. The fix-up stage of the quaternion (design 5.5) is N2-09's too.
#include "gpu/fragment/feature.h"

namespace mh4n::gpu::fragment {
namespace {

class LightingStub final : public FeatureModule {
public:
    const char* Name() const override { return "lighting"; }

    const char* Stubbed(const FragmentKey& key) const override {
        return key.lighting.enable() != 0 ? "lighting" : nullptr;
    }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(LightingStub)
}  // namespace mh4n::gpu::fragment
