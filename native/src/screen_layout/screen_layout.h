// Derived from Azahar src/core/frontend/framebuffer_layout.h, src/core/frontend/top_screen.h (the port's port/azahar/azahar.patch) and src/core/3ds.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the shape of the layout (the screens' rectangles, opacities, bottom_crop, the further views with their screen and crop, IsWithinTouchscreen) and the overlay's clamps.
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (framebuffer_layout.h)
// Copyright 2026 Azahar Emulator Project (top_screen.h, the port's patch)
// Copyright 2017 Citra Emulator Project (3ds.h)
//
// screen_layout.h - N2b-04.
//
// Where the top and bottom screens go in a window of any size, and which pixel of the bottom screen
// a click is: the arithmetic of the layouts of the Azahar build, as it has them with the port's patch
// (port/azahar/azahar.patch: the top screen's width, the overlay layout and its clamps, the bottom
// screen's crop and further views). The windows and the drawing are somebody else's (N2b-05, N2b-10);
// this is pure arithmetic, checked against what Azahar's own code prints
// (tools/native/layout_oracle/, native/tests/screen_layout_expected.inc).
//
// What is fixed, as in the app: landscape orientation (no "upright" setting), screens not swapped, in
// 2D, with no gap, no integer scaling and no stretch padding. So a Layout is described by the mode, the
// window, the top screen's width, the aspect-ratio option and the overlay.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "mh4n/types.h"
#include "platform.h"  // port/src/platform.h: Port::ScreenOverlay

namespace mh4n::screen_layout {

/// The app's layouts (the settings names `overlay`, `stacked`, `single` and `side`).
enum class Mode { Overlay, Stacked, Single, Side };

/// The mode a settings name stands for: `overlay`, `stacked`, `single`, `side`. Any other name
/// (`windows`, a typo) is Overlay (D62: separate windows fall back to the overlay until N3-15), and
/// `known` (if given) says false: the caller logs it.
Mode ParseMode(std::string_view name, bool* known = nullptr);
const char* ModeName(Mode mode);

/// Everything a layout depends on.
struct Inputs {
    u32 window_w = 0, window_h = 0;
    Mode mode = Mode::Overlay;
    u32 top_width = 0;  ///< the top screen's width at its 240 rows; 0: 400, as on a 3DS
    /// One of the hooks' names: default, stretch, 4:3, 16:9, 16:10, 21:9 (only Single reads it;
    /// any other name reads as default).
    std::string aspect_ratio = "default";
    Port::ScreenOverlay overlay;  ///< Overlay mode only; a height of 0 is off (the custom layout's own places)
};

/// A rectangle in window pixels: left and top inside, right and bottom outside.
struct Rect {
    u32 left = 0, top = 0, right = 0, bottom = 0;
    u32 Width() const { return right - left; }
    u32 Height() const { return bottom - top; }
    bool Contains(u32 x, u32 y) const { return x >= left && x < right && y >= top && y < bottom; }
    bool operator==(const Rect&) const = default;
};

/// A rectangle in fractions of a screen's width and height.
struct RectF {
    float left = 0.0f, top = 0.0f, right = 1.0f, bottom = 1.0f;
    bool operator==(const RectF&) const = default;
};

/// A further part of the bottom screen, drawn over the other screens: where, and which part.
struct View {
    Rect screen;
    RectF crop;
};

struct Layout {
    /// The layout's size: the window's, raised to the layout's smallest size (the app's window
    /// takes that as its minimum, so it is the window's in practice).
    u32 width = 0, height = 0;
    bool top_shown = false, bottom_shown = false;  ///< Single shows the top screen only
    /// The screens' rectangles. A screen that is not shown has the rectangle it would have.
    Rect top, bottom;
    float top_opacity = 1.0f, bottom_opacity = 1.0f;
    /// The part of the bottom screen shown in `bottom` (the overlay's crop).
    RectF bottom_crop;
    /// Further views of parts of the bottom screen, the last drawn on top (Overlay only).
    std::array<View, Port::ScreenOverlay::MAX_VIEWS> views;
    u32 view_count = 0;

    /// The view under the window pixel (x, y), the last drawn first; -1 for none.
    int ViewAt(u32 x, u32 y) const;
    /// Whether the window pixel is on the bottom screen (the touch screen).
    bool IsWithinTouchscreen(u32 x, u32 y) const;
};

/// The layout of `in`: Azahar's layout function for the mode, with the port's changes, and the clamp
/// the window gets to the layout's smallest size.
///
/// Overlay mode with an overlay height of 0 is Azahar's plain custom layout: top {0,0,800,480} and
/// bottom {80,500,720,980} in any window, so those rectangles lie outside Layout::width/height (the
/// window raised only to 400x480). The app always sets a height when its overlay is on.
Layout Compute(const Inputs& in);

/// The layout's smallest size (width, height).
std::pair<u32, u32> MinimumSize(const Inputs& in);

/// The overlay as the host keeps it: Layout::SetScreenOverlay's clamps (heights, opacity and
/// placements to 0..1, crops to at least 1% each way inside the screen).
Port::ScreenOverlay ClampOverlay(const Port::ScreenOverlay& overlay);

/// A touch point on the bottom screen, as the HID module reads it.
struct TouchPoint {
    u32 x = 0, y = 0;     ///< pixels, 0..319 and 0..239
    float fx = 0.0f, fy = 0.0f;  ///< the same as fractions of the screen
};

/// The touch state of one window (Azahar's EmuWindow::TouchPressed, TouchMoved, TouchReleased and
/// ClipToTouchScreen, with the port's patch): a press on one of the layout's further views maps
/// through that view's crop and keeps to the view while held; a press on the bottom screen maps
/// through its crop; a move off the screen is clipped to it. The layout is the one at each call (it
/// may change between a press and a move).
class TouchTracker {
public:
    /// A press at a window pixel. True when it hit the bottom screen or a view (Azahar's return).
    bool Press(const Layout& layout, u32 x, u32 y);
    void Move(const Layout& layout, u32 x, u32 y);
    void Release();

    /// The touch point while held.
    std::optional<TouchPoint> Point() const;

private:
    void PressInView(const Layout& layout, int view, u32 x, u32 y);

    bool pressed_ = false;
    float x_ = 0.0f, y_ = 0.0f;
    int view_ = -1;  ///< the view the touch started on; -1 the screen itself
};

}  // namespace mh4n::screen_layout
