// Not derived from Azahar: the rules are the specification's (VK_KHR_surface, VK_KHR_swapchain); the preference for the
// present mode, the extent clamp and the image count were read in Azahar's vk_swapchain.cpp and written again here.
#include "swapchain/policy.h"

#include <algorithm>
#include <limits>

namespace mh4n::swapchain {

namespace {
bool Offers(std::span<const VkPresentModeKHR> offered, VkPresentModeKHR mode) {
    return std::find(offered.begin(), offered.end(), mode) != offered.end();
}
bool IsCompositorFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM;
}
} // namespace

VkPresentModeKHR ChoosePresentMode(std::span<const VkPresentModeKHR> offered, bool vsync) {
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (Offers(offered, VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (Offers(offered, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

const char* PresentModeName(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "other";
    }
}

std::optional<VkSurfaceFormatKHR> ChooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> offered) {
    if (offered.size() == 1 && offered[0].format == VK_FORMAT_UNDEFINED) {
        return VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }
    for (const VkSurfaceFormatKHR& format : offered) {
        if (IsCompositorFormat(format.format) && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const VkSurfaceFormatKHR& format : offered) {
        if (IsCompositorFormat(format.format)) {
            return format;
        }
    }
    return std::nullopt;
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, u32 requested_width, u32 requested_height) {
    constexpr u32 kUndefined = std::numeric_limits<u32>::max();
    if (caps.currentExtent.width != kUndefined || caps.currentExtent.height != kUndefined) {
        return caps.currentExtent;
    }
    VkExtent2D extent;
    extent.width = std::clamp(requested_width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(requested_height, caps.minImageExtent.height, caps.maxImageExtent.height);
    // A request of 0 stays 0 (nothing to present), whatever the surface's smallest size is.
    if (requested_width == 0) {
        extent.width = 0;
    }
    if (requested_height == 0) {
        extent.height = 0;
    }
    return extent;
}

u32 ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps) {
    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        count = std::min(count, caps.maxImageCount);
    }
    return count;
}

VkSurfaceTransformFlagBitsKHR ChooseTransform(const VkSurfaceCapabilitiesKHR& caps) {
    if ((caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
        return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    return caps.currentTransform;
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps) {
    if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    for (VkCompositeAlphaFlagBitsKHR bit :
         {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if ((caps.supportedCompositeAlpha & bit) != 0) {
            return bit;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

DisplayTimeChoice ChooseDisplayTime(bool present_wait_measures_display, bool display_timing_available,
                                    std::string_view registered_name) {
    if (present_wait_measures_display) {
        return {DisplayTimeKind::PresentWait, {}};
    }
    if (display_timing_available) {
        return {DisplayTimeKind::DisplayTiming, {}};
    }
    if (!registered_name.empty()) {
        return {DisplayTimeKind::Registered, std::string(registered_name)};
    }
    return {DisplayTimeKind::None, {}};
}

std::string DisplayTimeLabel(const DisplayTimeChoice& choice, std::string_view source_name) {
    switch (choice.kind) {
    case DisplayTimeKind::PresentWait: return "present_wait";
    case DisplayTimeKind::DisplayTiming: return "display_timing";
    case DisplayTimeKind::Registered: return std::string(source_name.empty() ? std::string_view(choice.registered) : source_name);
    case DisplayTimeKind::None: break;
    }
    return "none";
}

} // namespace mh4n::swapchain
