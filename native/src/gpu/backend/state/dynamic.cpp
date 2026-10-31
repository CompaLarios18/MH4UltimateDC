// The dynamic state of a recorder (N2-13): the blend constants, the stencil reference and the two stencil masks
// are never baked into a pipeline (the pipelines of core/ declare them dynamic), so a different value never makes
// another pipeline. Vulkan wants each set once per command buffer before the first draw that uses it; the core sets
// no defaults, so the caller does (rhi.h, the comment on FixedFunctionState).
#include "gpu/backend/core/impl.h"

namespace mh4n::gpu::rhi {

void Recorder::SetBlendConstants(const std::array<float, 4>& rgba) {
    impl_->vulkan_list()->setBlendConstants(rgba.data());
}

void Recorder::SetStencilReference(u32 reference) {
    impl_->vulkan_list()->setStencilReference(reference);
}

void Recorder::SetStencilMasks(u32 compare_mask, u32 write_mask) {
    impl_->vulkan_list()->setStencilCompareMask(compare_mask);
    impl_->vulkan_list()->setStencilWriteMask(write_mask);
}

}  // namespace mh4n::gpu::rhi
