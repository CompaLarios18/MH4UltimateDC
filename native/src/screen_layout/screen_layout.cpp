// Derived from Azahar src/core/frontend/framebuffer_layout.cpp, src/core/frontend/framebuffer_layout.h, src/core/frontend/emu_window.cpp and src/core/frontend/top_screen.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the layout functions (DefaultFrameLayout, LargeFrameLayout, SingleFrameLayout, CustomFrameLayout), the port's patch for them (TopScreenWidth, OverlayFrameLayout, SetScreenOverlay's clamps), GetMinimumSizeFromLayout, and the layout dispatch of EmuWindow::UpdateCurrentFramebufferLayout, for the four layouts of the app.
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (framebuffer_layout.cpp, framebuffer_layout.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (emu_window.cpp)
// Copyright 2026 Azahar Emulator Project (top_screen.h, the port's patch)
//
// The same arithmetic in the same types and the same order (u32 and float, std::round and
// std::lround), so that the rectangles are Azahar's to the pixel: native/tests/screen_layout_test.cpp
// compares them with what Azahar's own code prints (tools/native/layout_oracle/). What the app never
// sets is left out: the swapped and upright layouts, stereo 3D, the screen gap, integer scaling and
// the stretch paddings (their settings keep Azahar's defaults), the hybrid, large-screen and portrait
// layouts, and Android's secondary display.
#include "screen_layout/screen_layout.h"

#include <algorithm>
#include <cmath>

#pragma STDC FP_CONTRACT OFF

namespace mh4n::screen_layout {
namespace {

constexpr u32 kTopWidth = 400;  // Core::kScreenTopWidth, the 3DS's
constexpr u32 kTopHeight = 240;
constexpr u32 kBottomWidth = Port::kBottomScreenWidth;
constexpr u32 kBottomHeight = Port::kBottomScreenHeight;

Rect TranslateX(const Rect& r, u32 x) { return {r.left + x, r.top, r.right + x, r.bottom}; }
Rect TranslateY(const Rect& r, u32 y) { return {r.left, r.top + y, r.right, r.bottom + y}; }

u32 TopScreenWidth(const Inputs& in) { return in.top_width ? in.top_width : kTopWidth; }

// The largest subrectangle of window_area that keeps the aspect ratio (height / width), at its
// upper-left corner.
Rect MaxRectangleAspect(const Rect& window_area, float window_aspect_ratio) {
    const float scale = std::min(static_cast<float>(window_area.Width()),
                                 window_area.Height() / window_aspect_ratio);
    return {window_area.left, window_area.top,
            window_area.left + static_cast<u32>(std::round(scale)),
            window_area.top + static_cast<u32>(std::round(scale * window_aspect_ratio))};
}

// The largest copy of inner_window (its shape) that fits in bounding_window, at its upper-left corner.
Rect MaxRectangle(const Rect& bounding_window, const Rect& inner_window) {
    const float scale = std::min(static_cast<float>(bounding_window.Width()) / inner_window.Width(),
                                 static_cast<float>(bounding_window.Height()) / inner_window.Height());
    return {bounding_window.left, bounding_window.top,
            bounding_window.left + static_cast<u32>(inner_window.Width() * scale),
            bounding_window.top + static_cast<u32>(inner_window.Height() * scale)};
}

float AspectRatioValue(std::string_view name) {  // FramebufferLayout::GetAspectRatioValue
    if (name == "16:9") {
        return 9.0f / 16.0f;
    }
    if (name == "4:3") {
        return 3.0f / 4.0f;
    }
    if (name == "21:9") {
        return 9.0f / 21.0f;
    }
    if (name == "16:10") {
        return 10.0f / 16.0f;
    }
    return 1.0f;  // Azahar's arbitrary fallback value
}

// SingleFrameLayout(width, height, swapped = false, upright = false): the top screen only.
Layout SingleFrameLayout(const Inputs& in, u32 width, u32 height) {
    Layout res;
    res.width = width;
    res.height = height;
    res.top_shown = true;
    res.bottom_shown = false;

    const Rect screen_window_area{0, 0, width, height};
    Rect top_screen{0, 0, TopScreenWidth(in), kTopHeight};
    Rect bot_screen{0, 0, kBottomWidth, kBottomHeight};

    const float window_aspect_ratio = static_cast<float>(height) / static_cast<float>(width);
    if (in.aspect_ratio == "stretch") {
        top_screen = MaxRectangleAspect(screen_window_area, window_aspect_ratio);
        bot_screen = MaxRectangleAspect(screen_window_area, window_aspect_ratio);
    } else if (in.aspect_ratio == "4:3" || in.aspect_ratio == "16:9" || in.aspect_ratio == "16:10" ||
               in.aspect_ratio == "21:9") {
        const float emulation_aspect_ratio = AspectRatioValue(in.aspect_ratio);
        top_screen = MaxRectangleAspect(screen_window_area, emulation_aspect_ratio);
        bot_screen = MaxRectangleAspect(screen_window_area, emulation_aspect_ratio);
    } else {  // default (and any other name)
        top_screen = MaxRectangle(screen_window_area, top_screen);
        bot_screen = MaxRectangle(screen_window_area, bot_screen);
    }

    res.top = TranslateY(TranslateX(top_screen, (width - top_screen.Width()) / 2),
                         (height - top_screen.Height()) / 2);
    res.bottom = TranslateY(TranslateX(bot_screen, (width - bot_screen.Width()) / 2),
                            (height - bot_screen.Height()) / 2);
    return res;
}

enum class SmallPosition { MiddleRight, BelowLarge };

// LargeFrameLayout(width, height, swapped = false, upright = false, scale_factor, position): the top
// screen large and the bottom one small, beside it (MiddleRight) or under it (BelowLarge). The
// stacked layout is scale 1 BelowLarge (DefaultFrameLayout), the side one scale 1 MiddleRight.
Layout LargeFrameLayout(const Inputs& in, u32 width, u32 height, float scale_factor,
                        SmallPosition position) {
    Layout res;
    res.width = width;
    res.height = height;
    res.top_shown = true;
    res.bottom_shown = true;
    const bool vertical = position == SmallPosition::BelowLarge;

    const u32 large_height = kTopHeight;
    const u32 small_height = static_cast<u32>(kBottomHeight / scale_factor);
    const u32 large_width = TopScreenWidth(in);
    const u32 small_width = static_cast<u32>(kBottomWidth / scale_factor);

    u32 emulation_width;
    u32 emulation_height;
    if (vertical) {
        emulation_width = std::max(large_width, small_width);
        emulation_height = large_height + small_height;
    } else {
        emulation_width = large_width + small_width;
        emulation_height = std::max(large_height, small_height);
    }

    const Rect screen_window_area{0, 0, width, height};
    Rect total_rect{0, 0, emulation_width, emulation_height};
    total_rect = MaxRectangle(screen_window_area, total_rect);
    total_rect = TranslateY(TranslateX(total_rect, (width - total_rect.Width()) / 2),
                            (height - total_rect.Height()) / 2);

    const float scale_amount = static_cast<float>(total_rect.Height()) / emulation_height;

    Rect large_screen{total_rect.left, total_rect.top,
                      static_cast<u32>(large_width * scale_amount + total_rect.left),
                      static_cast<u32>(large_height * scale_amount + total_rect.top)};
    Rect small_screen{total_rect.left, total_rect.top,
                      static_cast<u32>(small_width * scale_amount + total_rect.left),
                      static_cast<u32>(small_height * scale_amount + total_rect.top)};

    switch (position) {
    case SmallPosition::MiddleRight:
        // the small screen to the centre right
        small_screen = TranslateX(small_screen, large_screen.Width());
        small_screen = TranslateY(small_screen, ((large_screen.Height() - small_screen.Height()) / 2) +
                                                    large_screen.top - small_screen.top);
        break;
    case SmallPosition::BelowLarge:
        // the small screen down and over to the centre; the large one centred if it is narrower
        if (large_screen.Width() < total_rect.Width()) {
            large_screen = TranslateX(large_screen, (total_rect.Width() - large_screen.Width()) / 2);
        }
        small_screen = TranslateY(small_screen, large_screen.Height());
        small_screen = TranslateX(small_screen, (large_screen.left - total_rect.left) +
                                                    large_screen.Width() / 2 - small_screen.Width() / 2);
        break;
    }
    res.top = large_screen;
    res.bottom = small_screen;
    return res;
}

// The size in pixels of the part `crop` of the bottom screen.
Rect BottomPart(const RectF& crop) {
    return {0, 0,
            std::max(1u, static_cast<u32>(std::lround(kBottomWidth * (crop.right - crop.left)))),
            std::max(1u, static_cast<u32>(std::lround(kBottomHeight * (crop.bottom - crop.top))))};
}

// A screen, or a part of one (`shape`: its size), `fraction` times the window's height at its aspect
// ratio, placed at (x, y): fractions of the room the window leaves beside and above it.
Rect PlaceInWindow(u32 width, u32 height, const Rect& shape, float fraction, float x, float y) {
    const u32 h = std::min(height, std::max(1u, static_cast<u32>(std::lround(height * fraction))));
    const u32 w = std::min(width, std::max(1u, h * shape.Width() / shape.Height()));
    const u32 left = static_cast<u32>(std::lround(std::clamp(x, 0.0f, 1.0f) * (width - w)));
    const u32 top = static_cast<u32>(std::lround(std::clamp(y, 0.0f, 1.0f) * (height - h)));
    return {left, top, left + w, top + h};
}

// The custom layout with a screen overlay: the top screen as large as the window allows at its
// aspect ratio, centred, and the bottom one over it in a corner (or where the overlay places it),
// cropped, with the overlay's further views of the bottom screen.
Layout OverlayFrameLayout(const Inputs& in, u32 width, u32 height, const Port::ScreenOverlay& overlay) {
    Layout res;
    res.width = width;
    res.height = height;
    res.top_shown = true;
    res.bottom_shown = true;
    const Rect top{0, 0, TopScreenWidth(in), kTopHeight};
    const RectF crop{overlay.crop_left, overlay.crop_top, overlay.crop_right, overlay.crop_bottom};
    const Rect bottom_part = BottomPart(crop);

    Rect big = MaxRectangle(Rect{0, 0, width, height}, top);
    big = TranslateY(TranslateX(big, (width - big.Width()) / 2), (height - big.Height()) / 2);

    using Corner = Port::ScreenOverlay::Corner;
    const bool placed = overlay.x >= 0.0f && overlay.y >= 0.0f;
    const bool left = overlay.corner == Corner::TopLeft || overlay.corner == Corner::BottomLeft;
    const bool up = overlay.corner == Corner::TopLeft || overlay.corner == Corner::TopRight;
    const float x = placed ? overlay.x : left ? 0.0f : 1.0f;
    const float y = placed ? overlay.y : up ? 0.0f : 1.0f;
    const Rect corner = PlaceInWindow(width, height, bottom_part, overlay.height, x, y);

    res.top = big;
    res.bottom = corner;
    res.bottom_crop = crop;
    // further parts of the bottom screen, each where it is placed
    for (const auto& view : overlay.views) {
        if (view.height <= 0.0f || res.view_count >= res.views.size()) {
            continue;
        }
        const RectF part{view.crop_left, view.crop_top, view.crop_right, view.crop_bottom};
        res.views[res.view_count++] = {
            PlaceInWindow(width, height, BottomPart(part), view.height, view.x, view.y), part};
    }
    res.bottom_opacity = overlay.opacity;
    return res;
}

// CustomFrameLayout(width, height, swapped = false) with the overlay off: the custom layout's own
// positions (Azahar's defaults of custom_top_* and custom_bottom_*), whatever the window's size.
Layout PlainCustomFrameLayout(u32 width, u32 height) {
    Layout res;
    res.width = width;
    res.height = height;
    res.top_shown = true;
    res.bottom_shown = true;
    res.top = {0, 0, 0 + 800, 0 + 480};
    res.bottom = {80, 500, 80 + 640, 500 + 480};
    return res;
}

}  // namespace

Mode ParseMode(std::string_view name, bool* known) {
    Mode mode = Mode::Overlay;
    bool ok = true;
    if (name == "overlay") {
        mode = Mode::Overlay;
    } else if (name == "stacked") {
        mode = Mode::Stacked;
    } else if (name == "single") {
        mode = Mode::Single;
    } else if (name == "side") {
        mode = Mode::Side;
    } else {
        ok = false;
    }
    if (known) {
        *known = ok;
    }
    return mode;
}

const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Overlay: return "overlay";
    case Mode::Stacked: return "stacked";
    case Mode::Single: return "single";
    case Mode::Side: return "side";
    }
    return "overlay";
}

Port::ScreenOverlay ClampOverlay(const Port::ScreenOverlay& overlay) {
    Port::ScreenOverlay out = overlay;
    out.height = std::clamp(overlay.height, 0.0f, 1.0f);
    out.opacity = std::clamp(overlay.opacity, 0.0f, 1.0f);
    // a crop of at least 1% each way, inside the screen
    out.crop_left = std::clamp(overlay.crop_left, 0.0f, 0.99f);
    out.crop_top = std::clamp(overlay.crop_top, 0.0f, 0.99f);
    out.crop_right = std::clamp(overlay.crop_right, out.crop_left + 0.01f, 1.0f);
    out.crop_bottom = std::clamp(overlay.crop_bottom, out.crop_top + 0.01f, 1.0f);
    for (auto& view : out.views) {
        view.height = std::clamp(view.height, 0.0f, 1.0f);
        view.x = std::clamp(view.x, 0.0f, 1.0f);
        view.y = std::clamp(view.y, 0.0f, 1.0f);
        view.crop_left = std::clamp(view.crop_left, 0.0f, 0.99f);
        view.crop_top = std::clamp(view.crop_top, 0.0f, 0.99f);
        view.crop_right = std::clamp(view.crop_right, view.crop_left + 0.01f, 1.0f);
        view.crop_bottom = std::clamp(view.crop_bottom, view.crop_top + 0.01f, 1.0f);
    }
    return out;
}

std::pair<u32, u32> MinimumSize(const Inputs& in) {
    const u32 top_width = TopScreenWidth(in);
    switch (in.mode) {
    case Mode::Overlay:
        if (in.overlay.height > 0.0f) {
            // GetMinimumSizeFromLayout: the overlay layout is read as the single-screen one
            return {top_width, kBottomHeight};
        }
        // the plain custom layout is not in the function's switch: the default case, the stacked one
        return {top_width, kTopHeight + kBottomHeight};
    case Mode::Single:
        return {top_width, kBottomHeight};
    case Mode::Side:
        return {top_width + kBottomWidth, kBottomHeight};
    case Mode::Stacked:
        break;
    }
    return {top_width, kTopHeight + kBottomHeight};
}

Layout Compute(const Inputs& in) {
    // EmuWindow::UpdateCurrentFramebufferLayout: the window is at least the layout's smallest size
    const Port::ScreenOverlay overlay = ClampOverlay(in.overlay);
    Inputs clamped = in;
    clamped.overlay = overlay;
    const auto [min_width, min_height] = MinimumSize(clamped);
    const u32 width = std::max(in.window_w, min_width);
    const u32 height = std::max(in.window_h, min_height);

    switch (in.mode) {
    case Mode::Single:
        return SingleFrameLayout(in, width, height);
    case Mode::Side:
        return LargeFrameLayout(in, width, height, 1.0f, SmallPosition::MiddleRight);
    case Mode::Stacked:
        return LargeFrameLayout(in, width, height, 1.0f, SmallPosition::BelowLarge);
    case Mode::Overlay:
        break;
    }
    if (overlay.height > 0.0f) {
        return OverlayFrameLayout(in, width, height, overlay);
    }
    return PlainCustomFrameLayout(width, height);
}

int Layout::ViewAt(u32 x, u32 y) const {
    if (!bottom_shown) {
        return -1;
    }
    const std::size_t count = std::min<std::size_t>(view_count, views.size());
    for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
        if (views[static_cast<std::size_t>(i)].screen.Contains(x, y)) {
            return i;
        }
    }
    return -1;
}

bool Layout::IsWithinTouchscreen(u32 x, u32 y) const {
    return bottom_shown && bottom.Contains(x, y);
}

}  // namespace mh4n::screen_layout
