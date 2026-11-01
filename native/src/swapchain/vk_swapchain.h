// One VkSwapchainKHR with its wrapped images, the semaphores and the fence of the acquire/present protocol (N2b-06).
// Internal to mh4n_swapchain; every call is on the present thread (the specification makes the swapchain externally
// synchronised in vkAcquireNextImageKHR, vkQueuePresentKHR, vkWaitForPresentKHR, vkGetPastPresentationTimingGOOGLE and
// vkDestroySwapchainKHR). The synchronisation, and the passages of the specification it rests on, are in
// docs/native/presentation.md (N2b-06).
#pragma once

#include <functional>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu/backend/rhi.h"
#include "gpu/backend/vulkan_access.h"
#include "mh4n/types.h"

namespace mh4n::swapchain::detail {

namespace rhi = ::mh4n::gpu::rhi;

enum class CreateStatus : u8 { Ok, NotPresentable, SurfaceLost, Failed };
enum class AcquireStatus : u8 { Acquired, Timeout, OutOfDate, Failed };
enum class PresentStatus : u8 { Ok, Suboptimal, OutOfDate, Failed };

struct AcquireResult {
    AcquireStatus status = AcquireStatus::Failed;
    u32 index = 0;
    bool suboptimal = false;
};

class VkSwapchain {
public:
    struct Made {
        CreateStatus status = CreateStatus::Failed;
        std::string message;
        std::unique_ptr<VkSwapchain> swapchain;
    };
    // Makes a swapchain on `surface` for a size (the surface may dictate its own: ChooseExtent), with the present mode
    // `vsync` asks for, and wraps its images (on the calling thread, which owns them). NotPresentable: the extent is 0.
    static Made Create(const std::shared_ptr<rhi::Backend>& backend, VkSurfaceKHR surface, bool vsync, u32 width,
                       u32 height);

    // Does not wait for anything: call Destroy first (the device must be idle).
    ~VkSwapchain();
    VkSwapchain(const VkSwapchain&) = delete;
    VkSwapchain& operator=(const VkSwapchain&) = delete;

    // Waits for the device (the backend's timeline, then vkQueueWaitIdle under QueueLock), for the acquire's fence if
    // one is pending (bounded), and destroys the wrapped images, the semaphores, the fence and the swapchain.
    void Destroy();

    // One acquire attempt of at most `timeout_ns`, with the acquire's fence (reset first) and no semaphore: see the
    // note in presentation.md. `forced`: 0 asks the driver; otherwise TestHooks::acquire_result's meaning.
    AcquireResult Acquire(u64 timeout_ns, s32 forced);
    // After an Acquired result: waits at most `timeout_ns` for the fence. True: the image may be used.
    bool WaitAcquired(u64 timeout_ns);
    // True: WaitAcquired polls vkGetFenceStatus under QueueLock instead of blocking in vkWaitForFences (Options::
    // acquire_wait_under_queue_lock). Off by default.
    void SetObserveUnderQueueLock(bool on) {
        observe_under_queue_lock_ = on;
    }

    struct PresentParams {
        u32 index = 0;
        u64 present_id = 0;     // sent with VK_KHR_present_id when `use_present_id`, with display timing when `use_display_timing`
        bool use_present_id = false;
        bool use_display_timing = false;
        s32 forced = 0;         // TestHooks::present_result
        // TestHooks::queue_locked: called with QueueLock held, after the signalling submission and before vkQueuePresentKHR.
        const std::function<void()>* queue_locked = nullptr;
    };
    // Under QueueLock: an empty submission that signals this image's binary semaphore (its first synchronisation scope
    // covers every submission earlier on the queue, the composition's included), then vkQueuePresentKHR waiting on it.
    PresentStatus SubmitAndPresent(const PresentParams& params);

    // vkWaitForPresentKHR for `present_id`: the VkResult (VK_SUCCESS, VK_TIMEOUT, an error).
    s32 WaitForPresent(u64 present_id, u64 timeout_ns);
    // vkGetPastPresentationTimingGOOGLE: (presentID, actualPresentTime in ns) of the frames shown since the last call.
    std::vector<std::pair<u64, u64>> PastPresentationTimes();
    // vkGetRefreshCycleDurationGOOGLE in ns, 0 when unknown.
    u64 RefreshCycleNs();

    rhi::Image& Image(u32 index) const {
        return *images_[index];
    }
    u32 ImageCount() const {
        return static_cast<u32>(images_.size());
    }
    u32 Width() const {
        return extent_.width;
    }
    u32 Height() const {
        return extent_.height;
    }
    bool Vsync() const {
        return vsync_;
    }
    VkPresentModeKHR PresentMode() const {
        return mode_;
    }

private:
    VkSwapchain() = default;

    std::shared_ptr<rhi::Backend> backend_;
    rhi::VulkanHandles handles_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_ = {};
    VkPresentModeKHR mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool vsync_ = true;
    std::vector<std::shared_ptr<rhi::Image>> images_;
    std::vector<VkImage> vk_images_;
    std::vector<VkSemaphore> render_done_;  // per image index; reused when that index is acquired again
    VkFence acquire_fence_ = VK_NULL_HANDLE;
    bool observe_under_queue_lock_ = false;
    bool fence_pending_ = false;            // an acquire's fence signal has not been seen yet
    bool destroyed_ = false;
};

} // namespace mh4n::swapchain::detail
