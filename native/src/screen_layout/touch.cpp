// Derived from Azahar src/core/frontend/emu_window.cpp, src/core/frontend/framebuffer_layout.cpp and src/core/hle/service/hid/hid.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): EmuWindow::TouchPressed, TouchMoved, TouchReleased and ClipToTouchScreen, the port's patch for them (TouchInView, bottom_crop), FramebufferLayout::IsWithinTouchscreen and BottomViewAt, and how the HID module turns the touch state into pixels.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (emu_window.cpp)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (framebuffer_layout.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.cpp)
//
// The touch state of one window. Same types and order of operations as Azahar's, so that the
// points are the ones its code makes (native/tests/screen_layout_test.cpp compares them); no
// contraction of multiply and add, which a host with fused operations could otherwise make.
#include "screen_layout/screen_layout.h"

#include <algorithm>

#pragma STDC FP_CONTRACT OFF

namespace mh4n::screen_layout {

namespace {
constexpr u32 kBottomWidth = Port::kBottomScreenWidth;
constexpr u32 kBottomHeight = Port::kBottomScreenHeight;
}  // namespace

// A touch at (x, y) in the layout's further view `view` of the bottom screen: the point in that
// view's part of the bottom screen.
void TouchTracker::PressInView(const Layout& layout, int view, u32 x, u32 y) {
    const auto& [r, crop] = layout.views[static_cast<std::size_t>(view)];
    x = std::clamp(x, r.left, std::max(r.left, r.right - 1));
    y = std::clamp(y, r.top, std::max(r.top, r.bottom - 1));
    const float fx = static_cast<float>(x - r.left) / std::max(1u, r.Width());
    const float fy = static_cast<float>(y - r.top) / std::max(1u, r.Height());
    x_ = crop.left + fx * (crop.right - crop.left);
    y_ = crop.top + fy * (crop.bottom - crop.top);
    pressed_ = true;
    view_ = view;
}

bool TouchTracker::Press(const Layout& layout, u32 x, u32 y) {
    // the further views are drawn over everything else: a new touch on one is a touch on its part (a
    // touch moving from elsewhere stays there)
    const int view = layout.ViewAt(x, y);
    if (view >= 0 && (!pressed_ || view_ >= 0)) {
        PressInView(layout, view, x, y);
        return true;
    }
    if (layout.IsWithinTouchscreen(x, y)) {
        view_ = -1;
    }
    if (!layout.IsWithinTouchscreen(x, y)) {
        return false;
    }

    x_ = static_cast<float>(x - layout.bottom.left) / (layout.bottom.right - layout.bottom.left);
    y_ = static_cast<float>(y - layout.bottom.top) / (layout.bottom.bottom - layout.bottom.top);

    // a bottom screen showing only part of itself (the overlay's crop)
    const RectF& crop = layout.bottom_crop;
    x_ = crop.left + x_ * (crop.right - crop.left);
    y_ = crop.top + y_ * (crop.bottom - crop.top);

    pressed_ = true;
    return true;
}

void TouchTracker::Release() {
    pressed_ = false;
    x_ = 0;
    y_ = 0;
    view_ = -1;
}

void TouchTracker::Move(const Layout& layout, u32 x, u32 y) {
    if (!pressed_) {
        return;
    }
    // a touch that started on one of the further views stays on it
    if (view_ >= 0) {
        if (static_cast<u32>(view_) < layout.view_count) {
            PressInView(layout, view_, x, y);
        }
        return;
    }
    if (!layout.IsWithinTouchscreen(x, y)) {
        // ClipToTouchScreen
        x = std::max(x, layout.bottom.left);
        x = std::min(x, layout.bottom.right - 1);
        y = std::max(y, layout.bottom.top);
        y = std::min(y, layout.bottom.bottom - 1);
    }
    Press(layout, x, y);
}

std::optional<TouchPoint> TouchTracker::Point() const {
    if (!pressed_) {
        return std::nullopt;
    }
    TouchPoint point;
    point.fx = x_;
    point.fy = y_;
    // hid.cpp: touch_entry.x = static_cast<u16>(x * Core::kScreenBottomWidth), and the same for y;
    // never past the last pixel (a point at the very edge of the screen is 319 or 239 either way)
    point.x = std::min<u32>(static_cast<u16>(x_ * kBottomWidth), kBottomWidth - 1);
    point.y = std::min<u32>(static_cast<u16>(y_ * kBottomHeight), kBottomHeight - 1);
    return point;
}

}  // namespace mh4n::screen_layout
