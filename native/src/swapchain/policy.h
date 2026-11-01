// Derived from Azahar src/video_core/renderer_vulkan/vk_swapchain.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: the file says "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
//
// This header documents the rules policy.cpp has from Azahar (see its first lines for which, and for what is
// this port's own).
//
// The decisions of the swapchain and the present thread that need no device (N2b-06, docs/native/presentation.md):
// present mode, surface format, extent and image count from what the surface offers; where the displayed time comes
// from; the one-slot mailbox. Pure functions and a plain class, so tests drive them with synthetic data.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "gpu/backend/vulkan_access.h"
#include "mh4n/present.h"
#include "mh4n/types.h"

namespace mh4n::swapchain {

// ---- the swapchain's parameters --------------------------------------------------------------------------------------

// FIFO when `vsync` is on. Off: MAILBOX when the surface offers it (the newest frame replaces a waiting one, no
// tearing), else IMMEDIATE (Apple's surfaces offer no MAILBOX, rhi-spike.md X9), else FIFO, which every surface offers.
VkPresentModeKHR ChoosePresentMode(std::span<const VkPresentModeKHR> offered, bool vsync);
const char* PresentModeName(VkPresentModeKHR mode);

// The first offered format that is B8G8R8A8_UNORM or R8G8B8A8_UNORM (the two the compositor has pipelines for, N2b-05)
// with the sRGB non-linear colour space (the game's pixels are not converted); failing that the first of the two
// formats in any colour space. nullopt: neither format is offered. A list with one VK_FORMAT_UNDEFINED entry means
// "any" (the old rule of the specification): R8G8B8A8_UNORM.
std::optional<VkSurfaceFormatKHR> ChooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> offered);

// The extent of the swapchain. A surface whose currentExtent is 0xFFFFFFFF in both dimensions (a headless surface, and
// a window system that lets the swapchain choose) takes the requested size clamped to [minImageExtent,
// maxImageExtent]; any other surface dictates currentExtent (the requested size is then ignored: the window system
// has already resized). A zero in the result (a minimised window reports 0x0) means "nothing can be presented".
VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, u32 requested_width, u32 requested_height);

// minImageCount + 1 (one image to work on while the presentation engine holds another), at most maxImageCount when the
// surface has a maximum (0 means none).
u32 ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps);

// Identity when supported, else the surface's current transform.
VkSurfaceTransformFlagBitsKHR ChooseTransform(const VkSurfaceCapabilitiesKHR& caps);
// Opaque when supported, else the first supported bit (inherit).
VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps);

// ---- where displayed_us comes from -----------------------------------------------------------------------------------

enum class DisplayTimeKind : u8 { PresentWait, DisplayTiming, Registered, None };

struct DisplayTimeChoice {
    DisplayTimeKind kind = DisplayTimeKind::None;
    std::string registered;  // the registration name, for Registered
    friend bool operator==(const DisplayTimeChoice&, const DisplayTimeChoice&) = default;
};

// D57's order, the first that applies: VK_KHR_present_wait when PresentWaitMeasuresDisplay() (not merely
// PresentWaitAvailable(): MoltenVK releases the wait at command-buffer completion, rhi-spike.md 7.3); VK_GOOGLE_display_timing
// when DisplayTimingAvailable(); a DisplayTimeSource registered under `registered_name` (empty: none is); none.
DisplayTimeChoice ChooseDisplayTime(bool present_wait_measures_display, bool display_timing_available,
                                    std::string_view registered_name);

// The word after `display_time=` in the log header: present_wait, display_timing, the source's own name (`source_name`,
// or the registration name when that is empty), none.
std::string DisplayTimeLabel(const DisplayTimeChoice& choice, std::string_view source_name = {});

// ---- the mailbox -----------------------------------------------------------------------------------------------------

// The one slot between Publish and the present thread: a newer frame replaces an older one nobody took. Not
// synchronised: the presenter holds its mutex around every call.
class FrameMailbox {
public:
    struct Entry {
        present::DisplayFrame frame;
        u64 published_us = 0;
    };

    // Stores the frame and returns the one it replaced (nullopt: the slot was empty). A replaced frame will never be
    // shown: the caller counts it as dropped, and lets it die outside its lock (it may be the last reference to a
    // backend image).
    std::optional<Entry> Exchange(present::DisplayFrame frame, u64 published_us) {
        std::optional<Entry> old = std::move(entry_);
        entry_ = Entry{std::move(frame), published_us};
        return old;
    }
    // True when a frame that was not taken was replaced.
    bool Put(present::DisplayFrame frame, u64 published_us) {
        return Exchange(std::move(frame), published_us).has_value();
    }
    bool Has() const {
        return entry_.has_value();
    }
    std::optional<Entry> Take() {
        std::optional<Entry> out = std::move(entry_);
        entry_.reset();
        return out;
    }

private:
    std::optional<Entry> entry_;
};

} // namespace mh4n::swapchain
