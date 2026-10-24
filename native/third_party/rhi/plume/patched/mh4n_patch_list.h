// Written by tools/native/rhi_patches.py apply. Do not edit.
#pragma once

namespace mh4n_rhi_plume {
inline constexpr const char* kPlumeCommit = "0ce184b19270e49236afbb74052a6b267494b86b";
inline constexpr unsigned kPatchCount = 12;
inline constexpr const char* kPatchFiles[] = {
    "01-p7-mutable-format-zero-attachment.diff",
    "02-p2-fragment-shader-interlock.diff",
    "03-p3-per-wait-stage-mask.diff",
    "04-p7b-general-attachment-layout.diff",
    "05-p3b-timeline-semaphores.diff",
    "06-p4-pipeline-cache-renderpass-cache.diff",
    "07-p5-blend-constants.diff",
    "08-surfaces-and-device-requirements.diff",
    "09-device-and-enabled-features.diff",
    "10-dynamic-stencil-state.diff",
    "11-formats-and-format-query.diff",
    "12-custom-border-colour.diff"
};
}  // namespace mh4n_rhi_plume
