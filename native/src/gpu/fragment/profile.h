// The device profile of the fragment stage from the backend's capabilities (N2-33).
//
// Written for this package from the design (docs/native/gpu-fragment-design.md 4.7, 5.3): nothing is
// copied from Azahar here. See key.h for `DeviceProfile` itself.
#pragma once

#include "gpu/backend/rhi.h"
#include "gpu/fragment/key.h"

namespace mh4n::gpu::fragment {

// From `rhi::Capabilities` after the mask (a masked bit is already false there): `logic_op`,
// `custom_border_color`, `interlock` and `geometry_shader`; `has_blend_minmax_factor` is false on every
// device (Vulkan's MIN and MAX ignore the factors, design 5.3).
inline DeviceProfile ProfileOf(const rhi::Capabilities& caps) {
    DeviceProfile profile;
    profile.logic_op = caps.logic_op;
    profile.custom_border_color = caps.custom_border_color;
    profile.interlock = caps.interlock;
    profile.geometry_shader = caps.geometry_shader;
    profile.has_blend_minmax_factor = false;
    return profile;
}

} // namespace mh4n::gpu::fragment
