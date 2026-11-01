// Where the time a frame was shown comes from when the swapchain extensions cannot say (N2b-06, D57).
//
// The present log's displayed_us column takes the first source that applies (presenter.h): VK_KHR_present_wait
// when the backend says its wait measures the display, VK_GOOGLE_display_timing's past presentation times, a
// DisplayTimeSource registered by name, none. The registered one is D57's second source, the operating system's
// presentation statistics on Windows (the RX 6700 XT has neither extension); N2b-23 chooses ETW or DWM with its
// measurement, implements the source in a module of its own and registers it, once, under the name "platform":
//
//     MH4N_REGISTER(swapchain::DisplayTimeSourceFactory, "platform",
//                   {[] { return std::unique_ptr<swapchain::DisplayTimeSource>(new MySource); }});
//
// (Options::display_time_source names the registration the presenter looks for; its default is "platform".)
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "mh4n/registry.h"
#include "mh4n/types.h"

namespace mh4n::swapchain {

// One source of display times. The presenter makes one with the factory when it starts and calls it on the present
// thread only (so it needs no lock of its own), once per presented frame, in order, a little after the frame was
// presented.
class DisplayTimeSource {
public:
    virtual ~DisplayTimeSource() = default;

    // The text that goes after `display_time=` in the present log's header ("dwm", "etw", ...).
    virtual std::string_view Name() const = 0;

    // The host time, in microseconds of the steady clock (the clock of the log's other columns), at which the frame
    // with this presentation id (1, 2, 3 ... in presentation order; the same id VK_KHR_present_id would carry) was
    // shown, or nullopt when it cannot be told (the log then leaves the field empty). `present_host_us` is when
    // vkQueuePresentKHR returned for it. May block briefly; must not wait for a frame that was not presented yet.
    virtual std::optional<u64> DisplayedUs(u64 present_id, u64 present_host_us) = 0;
};

struct DisplayTimeSourceFactory {
    std::function<std::unique_ptr<DisplayTimeSource>()> make;
};

} // namespace mh4n::swapchain
