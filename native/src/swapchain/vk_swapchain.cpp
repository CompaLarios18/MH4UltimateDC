// The Vulkan side of the swapchain (N2b-06). Written for this port from the Vulkan specification (the acquire, submit and
// present protocol of wsi.adoc and synchronization.adoc). Azahar's vk_swapchain.cpp and vk_present_window.cpp were read
// before it was written; no code is copied and the structure differs (Azahar acquires and presents with a pair of
// semaphores per image; here the acquire uses a fence and the present waits on a semaphore signalled by an empty
// submission, see presentation.md). The rules that follow Azahar for the swapchain's parameters are in policy.cpp,
// which has the D6 header and a row in native/THIRD_PARTY.md.
#include "swapchain/vk_swapchain.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <fmt/format.h>

#include "mh4n/log.h"
#include "swapchain/policy.h"

namespace mh4n::swapchain::detail {

namespace {

constexpr const char* kLog = "Swapchain";

rhi::Format RhiFormatOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM: return rhi::Format::Bgra8Unorm;
    case VK_FORMAT_R8G8B8A8_UNORM: return rhi::Format::Rgba8Unorm;
    default: return rhi::Format::Undefined;
    }
}

template <class T, class F>
std::vector<T> Enumerate(F&& call) {
    u32 count = 0;
    call(&count, static_cast<T*>(nullptr));
    std::vector<T> out(count);
    if (count > 0) {
        call(&count, out.data());
        out.resize(count);
    }
    return out;
}

} // namespace

VkSwapchain::Made VkSwapchain::Create(const std::shared_ptr<rhi::Backend>& backend, VkSurfaceKHR surface, bool vsync,
                                      u32 width, u32 height) {
    Made made;
    const rhi::VulkanHandles handles = rhi::Handles(*backend);

    VkSurfaceCapabilitiesKHR caps = {};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(handles.physical_device, surface, &caps);
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        made.status = CreateStatus::SurfaceLost;
        made.message = "the surface was lost";
        return made;
    }
    if (result != VK_SUCCESS) {
        made.message = fmt::format("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed with {}", static_cast<int>(result));
        return made;
    }
    const VkExtent2D extent = ChooseExtent(caps, width, height);
    if (extent.width == 0 || extent.height == 0) {
        made.status = CreateStatus::NotPresentable;
        made.message = "the surface has no area to present to";
        return made;
    }

    const std::vector<VkSurfaceFormatKHR> formats = Enumerate<VkSurfaceFormatKHR>([&](u32* count, VkSurfaceFormatKHR* out) {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(handles.physical_device, surface, count, out);
    });
    const std::vector<VkPresentModeKHR> modes = Enumerate<VkPresentModeKHR>([&](u32* count, VkPresentModeKHR* out) {
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(handles.physical_device, surface, count, out);
    });
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        made.status = CreateStatus::SurfaceLost;
        made.message = "the surface was lost";
        return made;
    }
    const std::optional<VkSurfaceFormatKHR> format = ChooseSurfaceFormat(formats);
    if (!format.has_value()) {
        made.message = "the surface offers neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM";
        return made;
    }
    const rhi::Format rhi_format = RhiFormatOf(format->format);
    if (rhi_format == rhi::Format::Undefined || !backend->QueryFormat(rhi_format).attachment) {
        made.message = "the swapchain format cannot be a colour attachment";
        return made;
    }
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        made.message = "the surface does not support colour-attachment images";
        return made;
    }

    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = ChooseImageCount(caps);
    info.imageFormat = format->format;
    info.imageColorSpace = format->colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;  // the one queue of the backend
    info.preTransform = ChooseTransform(caps);
    info.compositeAlpha = ChooseCompositeAlpha(caps);
    info.presentMode = ChoosePresentMode(modes, vsync);
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;  // the previous swapchain was destroyed (VkSwapchain::Destroy) before this

    std::unique_ptr<VkSwapchain> self(new VkSwapchain);
    self->backend_ = backend;
    self->handles_ = handles;
    self->extent_ = extent;
    self->mode_ = info.presentMode;
    self->vsync_ = vsync;

    result = vkCreateSwapchainKHR(handles.device, &info, nullptr, &self->swapchain_);
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        made.status = CreateStatus::SurfaceLost;
        made.message = "the surface was lost while the swapchain was made";
        return made;
    }
    if (result != VK_SUCCESS) {
        self->swapchain_ = VK_NULL_HANDLE;
        made.message = fmt::format("vkCreateSwapchainKHR failed with {}", static_cast<int>(result));
        return made;
    }

    self->vk_images_ = Enumerate<VkImage>([&](u32* count, VkImage* out) {
        result = vkGetSwapchainImagesKHR(handles.device, self->swapchain_, count, out);
    });
    for (VkImage image : self->vk_images_) {
        // On the calling thread, the present thread: the image's owner (the owner rule, rhi-spike.md 9.4 item 5).
        rhi::Result<std::shared_ptr<rhi::Image>> wrapped =
            rhi::WrapExternalImage(*backend, image, rhi_format, extent.width, extent.height);
        if (!wrapped.ok()) {
            made.message = "WrapExternalImage: " + wrapped.error();
            self->Destroy();
            return made;
        }
        self->images_.push_back(*wrapped);
    }

    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (std::size_t i = 0; i < self->images_.size(); ++i) {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (vkCreateSemaphore(handles.device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            made.message = "vkCreateSemaphore failed";
            self->Destroy();
            return made;
        }
        self->render_done_.push_back(semaphore);
    }
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;  // unsignaled: vkAcquireNextImageKHR needs it so
    if (vkCreateFence(handles.device, &fence_info, nullptr, &self->acquire_fence_) != VK_SUCCESS) {
        made.message = "vkCreateFence failed";
        self->Destroy();
        return made;
    }

    made.status = CreateStatus::Ok;
    made.swapchain = std::move(self);
    return made;
}

VkSwapchain::~VkSwapchain() {
    // Destroy() is the caller's job, because it needs the present thread's care (idle device). A swapchain that was
    // not destroyed is a leak of Vulkan objects, never a use after free.
    if (!destroyed_ && swapchain_ != VK_NULL_HANDLE) {
        MH4N_LOG(Error, kLog, "a swapchain was dropped without Destroy(); its Vulkan objects are leaked");
    }
}

void VkSwapchain::Destroy() {
    if (destroyed_) {
        return;
    }
    destroyed_ = true;
    // Everything the device still runs with these images: the backend's own timeline, then the queue (the presents wait
    // for their semaphores there). The presents are queue operations of this queue, so the idle wait covers them.
    backend_->WaitIdle();
    {
        rhi::QueueLock lock(*backend_);
        vkQueueWaitIdle(handles_.queue);
    }
    if (fence_pending_ && acquire_fence_ != VK_NULL_HANDLE) {
        // The acquire's fence signal is pending until the presentation engine lets the image go; wait for it, bounded.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (vkWaitForFences(handles_.device, 1, &acquire_fence_, VK_TRUE, 20'000'000) == VK_TIMEOUT) {
            if (std::chrono::steady_clock::now() >= deadline) {
                MH4N_LOG(Warning, kLog, "the acquire fence did not signal within 2 s; it is leaked");
                acquire_fence_ = VK_NULL_HANDLE;
                break;
            }
        }
        fence_pending_ = false;
    }
    images_.clear();  // the wrapped images (views) before the swapchain whose images they wrap
    for (VkSemaphore semaphore : render_done_) {
        vkDestroySemaphore(handles_.device, semaphore, nullptr);
    }
    render_done_.clear();
    if (acquire_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(handles_.device, acquire_fence_, nullptr);
        acquire_fence_ = VK_NULL_HANDLE;
    }
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(handles_.device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

AcquireResult VkSwapchain::Acquire(u64 timeout_ns, s32 forced) {
    AcquireResult out;
    if (forced == VK_TIMEOUT || forced == VK_NOT_READY) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // a real acquire would have waited its slice
        out.status = AcquireStatus::Timeout;
        return out;
    }
    if (forced == VK_ERROR_OUT_OF_DATE_KHR || forced == VK_ERROR_SURFACE_LOST_KHR) {
        out.status = AcquireStatus::OutOfDate;
        return out;
    }
    // "the fence must be unsignaled, with no signal operations pending": it was waited for after the last acquire.
    vkResetFences(handles_.device, 1, &acquire_fence_);
    u32 index = 0;
    const VkResult result = vkAcquireNextImageKHR(handles_.device, swapchain_, timeout_ns, VK_NULL_HANDLE, acquire_fence_, &index);
    switch (result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
        fence_pending_ = true;
        out.status = AcquireStatus::Acquired;
        out.index = index;
        out.suboptimal = result == VK_SUBOPTIMAL_KHR || forced == VK_SUBOPTIMAL_KHR;
        return out;
    case VK_TIMEOUT:
    case VK_NOT_READY:
        // "If vkAcquireNextImageKHR does not successfully acquire an image, semaphore and fence are unaffected."
        out.status = AcquireStatus::Timeout;
        return out;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
        out.status = AcquireStatus::OutOfDate;
        return out;
    default:
        MH4N_LOG(Error, kLog, "vkAcquireNextImageKHR failed with {}", static_cast<int>(result));
        out.status = AcquireStatus::Failed;
        return out;
    }
}

bool VkSwapchain::WaitAcquired(u64 timeout_ns) {
    if (!fence_pending_) {
        return true;
    }
    if (observe_under_queue_lock_) {
        // vkGetFenceStatus under QueueLock instead of a blocking wait: the call that sees the signal runs with no
        // vkQueueSubmit of another thread half-way through the validation layer's bookkeeping (presentation.md, "Under
        // the validation layer with a second submitting thread"). A real driver does not care; the poll costs latency.
        const auto end = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
        for (;;) {
            VkResult status;
            {
                rhi::QueueLock lock(*backend_);
                status = vkGetFenceStatus(handles_.device, acquire_fence_);
            }
            if (status == VK_SUCCESS) {
                fence_pending_ = false;
                return true;
            }
            if (std::chrono::steady_clock::now() >= end) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    const VkResult result = vkWaitForFences(handles_.device, 1, &acquire_fence_, VK_TRUE, timeout_ns);
    if (result == VK_SUCCESS) {
        fence_pending_ = false;
        return true;
    }
    return false;
}

PresentStatus VkSwapchain::SubmitAndPresent(const PresentParams& params) {
    const VkSemaphore semaphore = render_done_[params.index];

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &semaphore;

    VkPresentInfoKHR present = {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &semaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &params.index;

    VkPresentIdKHR present_id = {};
    present_id.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    present_id.swapchainCount = 1;
    const u64 id = params.present_id;
    present_id.pPresentIds = &id;
    VkPresentTimeGOOGLE time = {};
    time.presentID = static_cast<uint32_t>(params.present_id);
    VkPresentTimesInfoGOOGLE times = {};
    times.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
    times.swapchainCount = 1;
    times.pTimes = &time;
    const void** tail = &present.pNext;
    if (params.use_present_id) {
        *tail = &present_id;
        tail = &present_id.pNext;
    }
    if (params.use_display_timing) {
        *tail = &times;
    }

    VkResult result;
    {
        // The queue is shared with the renderer thread: both calls under the backend's submit lock. Nothing in this
        // scope submits through the backend (Backend::Submit takes the same lock).
        rhi::QueueLock lock(*backend_);
        result = vkQueueSubmit(handles_.queue, 1, &submit, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            MH4N_LOG(Error, kLog, "vkQueueSubmit (the present's signal) failed with {}", static_cast<int>(result));
            return PresentStatus::Failed;
        }
        if (params.queue_locked != nullptr && *params.queue_locked) {
            (*params.queue_locked)();
        }
        result = vkQueuePresentKHR(handles_.queue, &present);
    }
    if (params.forced == VK_ERROR_OUT_OF_DATE_KHR || params.forced == VK_SUBOPTIMAL_KHR) {
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            result = static_cast<VkResult>(params.forced);
        }
    }
    switch (result) {
    case VK_SUCCESS: return PresentStatus::Ok;
    case VK_SUBOPTIMAL_KHR: return PresentStatus::Suboptimal;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR: return PresentStatus::OutOfDate;
    default:
        MH4N_LOG(Error, kLog, "vkQueuePresentKHR failed with {}", static_cast<int>(result));
        return PresentStatus::Failed;
    }
}

s32 VkSwapchain::WaitForPresent(u64 present_id, u64 timeout_ns) {
    if (vkWaitForPresentKHR == nullptr) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    return static_cast<s32>(vkWaitForPresentKHR(handles_.device, swapchain_, present_id, timeout_ns));
}

std::vector<std::pair<u64, u64>> VkSwapchain::PastPresentationTimes() {
    std::vector<std::pair<u64, u64>> out;
    if (vkGetPastPresentationTimingGOOGLE == nullptr) {
        return out;
    }
    VkPastPresentationTimingGOOGLE timings[32];
    u32 count = 32;
    const VkResult result = vkGetPastPresentationTimingGOOGLE(handles_.device, swapchain_, &count, timings);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return out;
    }
    for (u32 i = 0; i < count; ++i) {
        out.emplace_back(timings[i].presentID, timings[i].actualPresentTime);
    }
    return out;
}

u64 VkSwapchain::RefreshCycleNs() {
    if (vkGetRefreshCycleDurationGOOGLE == nullptr) {
        return 0;
    }
    VkRefreshCycleDurationGOOGLE duration = {};
    if (vkGetRefreshCycleDurationGOOGLE(handles_.device, swapchain_, &duration) != VK_SUCCESS) {
        return 0;
    }
    return duration.refreshDuration;
}

} // namespace mh4n::swapchain::detail
