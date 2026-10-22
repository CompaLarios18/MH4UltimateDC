// What a Vulkan device reported, before anything is judged (N2-24): filled from a live device
// (live.cpp) or from `vulkaninfo --json` (input.cpp), judged by Evaluate (report.h).
#pragma once

#include <map>
#include <set>
#include <string>

#include "mh4n/types.h"

namespace mh4n::vkprobe {

// A fact is known to be true, known to be false, or not reported. Absent is never "no".
enum class Tri { Unknown, No, Yes };

struct FormatSupport {
    Tri depth_stencil_attachment = Tri::Unknown;  // optimal tiling: VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
    Tri sampled_image = Tri::Unknown;             // optimal tiling: VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
};

struct Facts {
    std::string device_name;
    std::string device_type;  // "discrete GPU", "integrated GPU", "virtual GPU", "CPU", "other", or ""
    std::string driver_name;  // VkPhysicalDeviceDriverProperties, "" when not reported
    std::string driver_info;
    u32 api_version = 0;      // 0 when not reported
    u32 vendor_id = 0;
    u32 device_id = 0;

    // The device extensions. When `extensions_known` is false the list was not in the input, and
    // an extension that is not in `extensions` is unknown, not missing.
    bool extensions_known = false;
    std::set<std::string> extensions;

    // Feature bits by the name of the member of the Vulkan structure ("logicOp",
    // "fragmentShaderPixelInterlock", "triangleFans" ...). A member that is not in the map was not
    // reported. Structures that report the same member (VkPhysicalDeviceVulkan12Features and
    // VkPhysicalDeviceTimelineSemaphoreFeatures) count as one: true when any says true.
    std::map<std::string, Tri, std::less<>> features;

    // Formats by name without the "VK_FORMAT_" prefix ("D24_UNORM_S8_UINT"). A format that is not in
    // the map was not reported.
    std::map<std::string, FormatSupport, std::less<>> formats;
};

// "major.minor.patch" of a Vulkan version number ("" for 0).
std::string ApiVersionString(u32 version);

}  // namespace mh4n::vkprobe
