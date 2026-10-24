#include "gpu/backend/plume/vendor_info.h"

#include "mh4n_patch_list.h"

namespace mh4n::gpu::backend::plume {

const VendorInfoData& VendorInfo() {
    static const VendorInfoData info{
        mh4n_rhi_plume::kPlumeCommit,
        std::span<const char* const>(mh4n_rhi_plume::kPatchFiles, mh4n_rhi_plume::kPatchCount),
    };
    return info;
}

}  // namespace mh4n::gpu::backend::plume
