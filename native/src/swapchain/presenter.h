// The swapchain and the present thread (N2b-06, docs/native/presentation.md): principle 4 for presentation.
//
//     auto presenter = swapchain::Presenter::Create(backend, swapchain::Target::Window(surface, w, h), layout_source);
//     gpu_display->AddScreenSink(presenter->get());        // a present::ScreenSink: Publish never blocks
//     ...
//     (*presenter)->Resize(w, h);  (*presenter)->SetVsync(false);  (*presenter)->SetUiLayer(&imgui_layer);
//     (*presenter)->Stop();
//
// A present::ScreenSink whose Publish stores the frame in a one-slot mailbox (a newer frame replaces one that was not
// shown, counted as dropped) and wakes the present thread. That thread takes the newest frame, asks the LayoutSource
// for the current layout, composes the frame with mh4n_present's Compositor into the next swapchain image (or into an
// offscreen target), presents it, and logs when each frame was shown (present_log.h).
//
// Threads. Everything of the Vulkan side runs on the present thread, which is the one thread that records, submits and
// presents: the swapchain, its images (wrapped with WrapExternalImage on that thread) and the offscreen target belong
// to it, the vkAcquireNextImageKHR / vkQueuePresentKHR / vkWaitForPresentKHR / vkGetPastPresentationTimingGOOGLE /
// vkDestroySwapchainKHR calls are all on it (the specification makes the swapchain externally synchronised in each of
// them), and every use of the VkQueue (the present, the empty submission that makes it wait for the composition, the
// idle wait) is under rhi::QueueLock, never around Backend::Submit. With MH4N_RHI_CHECKS on, an own recording,
// submission or present on another thread calls the owner-rule handler (OwnerRule::WrongThread). The public calls
// below are thread-safe and only hand state over; none waits for the GPU or the display, except Stop, SetUiLayer(nullptr)
// (it waits for a composition in progress to finish) and the destructor.
//
// The surface of a Window target is the caller's: made on the backend's instance, given to BackendOptions, and
// destroyed by the caller after the presenter.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu/backend/rhi.h"
#include "mh4n/present.h"
#include "mh4n/types.h"
#include "present/compositor.h"
#include "screen_layout/screen_layout.h"
#include "swapchain/display_time.h"

namespace mh4n::swapchain {

// What the present thread draws a frame with, asked once per frame on the present thread: the layout's inputs (the
// window size is the presenter's own: Inputs::window_w/h are overwritten with the target's size) and the compositor's
// options (ComposeOptions::ui is overwritten with the layer given to SetUiLayer when there is one).
struct LayoutRequest {
    screen_layout::Inputs inputs;
    present::ComposeOptions options;
};
using LayoutSource = std::function<LayoutRequest()>;

// Where the frames go: a surface the caller made (a swapchain is made on it), or an offscreen image.
struct Target {
    gpu::rhi::SurfaceHandle surface;  // none: offscreen
    u32 width = 0;                    // the first size; Resize changes it (a surface that dictates its size ignores it)
    u32 height = 0;

    static Target Window(gpu::rhi::SurfaceHandle surface, u32 width, u32 height) {
        return Target{surface, width, height};
    }
    static Target Offscreen(u32 width, u32 height) {
        return Target{{}, width, height};
    }
    bool IsOffscreen() const {
        return !surface;
    }
};

struct Options {
    bool vsync = true;  // FIFO; off: MAILBOX when offered, else IMMEDIATE (policy.h)
    // The DisplayTimeSource registration used when neither present_wait nor display_timing measures the display
    // (display_time.h); "platform" is the name N2b-23 registers. Empty: never.
    std::string display_time_source = "platform";
};

struct Stats {
    u64 published = 0;           // Publish calls
    u64 presented = 0;           // presents (a redraw is one, an offscreen "present" too)
    u64 composed = 0;            // compositions (a swapchain image or the offscreen target drawn)
    u64 dropped = 0;             // frames never shown: replaced in the mailbox, discarded while minimised or unpresentable
    u64 redraws = 0;             // presents of the last frame again (RequestRedraw, a resize, leaving minimised)
    u32 swapchain_creations = 0; // swapchains made (1 after Create; +1 per recreation)
    u32 out_of_date = 0;         // VK_ERROR_OUT_OF_DATE_KHR (or SURFACE_LOST) seen from an acquire or a present
    u32 suboptimal = 0;          // VK_SUBOPTIMAL_KHR seen from an acquire or a present
    u32 acquire_timeouts = 0;    // acquire slices that timed out (the thread tries again, checking Stop)
    u32 extent_width = 0;        // the size of the swapchain or offscreen target in use
    u32 extent_height = 0;
    s32 present_mode = -1;       // VkPresentModeKHR of the swapchain in use; -1 offscreen or none
    std::string display_time;    // the label of the display-time source (policy.h DisplayTimeLabel)
};

// Hooks for tests. Every function runs on the present thread unless it says otherwise; all are optional. A hook may
// block (a test holds the present thread inside a composition); it should end its wait when StopRequested().
struct TestHooks {
    // Inside a composition: Compose has recorded `frame` into the target, the recorder is still open, nothing has been
    // submitted. (Not under any lock of the presenter.)
    std::function<void(const present::DisplayFrame& frame, u32 target_width, u32 target_height)> in_composition;
    // Before each acquire: 0 asks the driver. VK_TIMEOUT or VK_NOT_READY (1, 2): pretend the acquire timed out;
    // VK_ERROR_OUT_OF_DATE_KHR or VK_ERROR_SURFACE_LOST_KHR: pretend it said so (nothing is acquired);
    // VK_SUBOPTIMAL_KHR: really acquire, and report suboptimal.
    std::function<s32()> acquire_result;
    // Before each present: 0 asks the driver. VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR: really present, then
    // report that result.
    std::function<s32()> present_result;
    // Before each vkWaitForPresentKHR slice: 0 asks the driver; any other value replaces its result (VK_TIMEOUT: the
    // frame is never shown).
    std::function<s32()> present_wait_result;
    // The frame's recording, submission and present run as `run(work)`: the default calls work(). A test passes a
    // runner that runs it on another thread, to see the owner-rule check.
    std::function<void(const std::function<void()>& work)> frame_runner;
    // Choose the display-time source as if the device had neither extension.
    bool disable_present_wait = false;
    bool disable_display_timing = false;
    // Choose the display-time source as if the device's present wait measured the display, when it does not (lavapipe has
    // none). No presentation id is sent; the wait for a frame is present_wait_result's answer (VK_SUCCESS when unset), so
    // the thread's waiting, its slices, its limit and Stop are exercised.
    bool pretend_present_wait = false;
    // The same for VK_GOOGLE_display_timing: when set, the display timing path is chosen and this function stands in for
    // vkGetPastPresentationTimingGOOGLE: the (presentID, actualPresentTime in ns) of the frames shown since its last call.
    std::function<std::vector<std::pair<u64, u64>>()> past_presentation_times;
    // A swapchain was made: size, VkPresentModeKHR, image count.
    std::function<void(u32 width, u32 height, s32 present_mode, u32 image_count)> swapchain_created;
};

class Presenter final : public present::ScreenSink {
public:
    // Starts the present thread and waits until it has made the compositor and (for a window) the first swapchain, so
    // an error (no surface on the backend, no usable surface format, shaders that do not compile) is the result.
    static gpu::rhi::Result<std::unique_ptr<Presenter>> Create(std::shared_ptr<gpu::rhi::Backend> backend, Target target,
                                                               LayoutSource layout_source, Options options = {},
                                                               TestHooks hooks = {});
    // Stop(), then the swapchain, its images, the semaphores and the fence are destroyed after the device is idle.
    ~Presenter() override;
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // present::ScreenSink. Any thread; returns at once, whatever the present thread is doing.
    void Publish(const present::DisplayFrame& frame) override;

    // The layer the compositor calls after the screens (N2b-11). The layer must stay alive until SetUiLayer(nullptr)
    // returns: that call waits for a composition in progress, after which the layer is not called again.
    void SetUiLayer(present::UiLayer* layer);
    // Compose the last frame again (the UI changed while the game shows nothing new). Nothing before a first frame.
    void RequestRedraw();
    // The layout of the frame composed last (a copy); all zero before the first.
    screen_layout::Layout CurrentLayout() const;

    void SetVsync(bool vsync);
    // The new size of the window or offscreen target; 0 in either dimension presents nothing (dropped, D60).
    void Resize(u32 width, u32 height);
    // True: nothing is presented, published frames are dropped (D60); false: the last frame is drawn again.
    void SetMinimised(bool minimised);

    // The display's refresh rate for the log's header, when the app knows it (SDL's display mode); call before
    // SetPresentLog. VK_GOOGLE_display_timing's answer is used when it is not called.
    void SetRefreshHz(double hz);
    // Opens the present log (present_log.h), writing the header now. An error when the file cannot be opened.
    gpu::rhi::Result<void> SetPresentLog(const std::string& path);

    // Offscreen targets only: write every `every_n`-th composed image to `dir` as presented_<sequence>.ppm (sequence of
    // its DisplayFrame), and at Stop the last composed one as last.ppm. `dir` is created. An error for a window.
    gpu::rhi::Result<void> DumpTo(const std::string& dir, u32 every_n);

    // The thread finishes the frame it has and exits; a frame waiting in the mailbox is dropped. Returns when the thread
    // has gone and the device resources are destroyed. Idempotent. Later Publish calls are counted as dropped.
    void Stop();
    bool StopRequested() const;

    Stats GetStats() const;

private:
    struct Impl;
    explicit Presenter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mh4n::swapchain
