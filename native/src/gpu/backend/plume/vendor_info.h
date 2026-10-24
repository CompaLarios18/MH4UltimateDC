// What plume the renderer's Vulkan backend was built from (N2-25), for logs.
#pragma once

#include <span>
#include <string_view>

// Note: inside this namespace `plume::` names this namespace, not plume's own `::plume`; spell plume's
// types as `::plume::VulkanDevice`.
namespace mh4n::gpu::backend::plume {

struct VendorInfoData {
    // The commit of github.com/renderbag/plume the pristine copy is at.
    std::string_view commit;
    // The patch files applied to it, in order ("01-....diff").
    std::span<const char* const> patches;
};

// Read from native/third_party/rhi/plume/patched/mh4n_patch_list.h, written by tools/native/rhi_patches.py.
const VendorInfoData& VendorInfo();

}  // namespace mh4n::gpu::backend::plume
