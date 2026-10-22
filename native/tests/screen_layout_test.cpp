// screen_layout_test.cpp - N2b-04.
//
// mh4n_screen_layout against what Azahar's own code prints. screen_layout_expected.inc is generated
// by tools/native/layout_oracle/layout_oracle.py: it compiles Azahar's framebuffer_layout.cpp and
// emu_window.cpp (with the port's patch) and runs every case of its cases.txt; every expected value
// below came out of that run, none is written by hand. A case is a window (mode, size, the top
// screen's width, the aspect ratio, the overlay) and the touches made on it; the test checks the
// rectangles, opacities, crops and views of the layout, and the touch point after each step.
#include <doctest/doctest.h>

#if !__has_include("platform.h")
TEST_CASE("screen_layout_test needs the port's platform.h (the repository around native/)" * doctest::skip()) {}
#else

#include <array>
#include <cstring>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "screen_layout/screen_layout.h"

namespace {

using namespace mh4n;
using namespace mh4n::screen_layout;

struct ViewIn {
    float height, x, y;
    std::array<float, 4> crop;
};

struct ViewOut {
    std::array<u32, 4> rect;
    std::array<float, 4> crop;
};

struct LayoutOut {
    u32 width, height;
    int top_shown, bottom_shown;
    std::array<u32, 4> top, bottom;
    float top_opacity, bottom_opacity;
    std::array<float, 4> bottom_crop;
    u32 view_count;
    std::vector<ViewOut> views;
};

struct Step {
    char op;  // P press, M move, R release
    u32 x, y;
    int handled;  // a press: whether Azahar's TouchPressed returned true; -1 for a move or release
    int pressed;
    float fx, fy;  // Azahar's touch state (what the HID module reads)
    u32 px, py;    // the touch point in bottom-screen pixels, as the HID module makes it
};

struct Case {
    const char* name;
    const char* mode;
    u32 window_w, window_h, top_width;
    const char* aspect;
    int corner;
    float overlay_height, opacity;
    std::array<float, 4> crop;
    std::array<float, 2> at;
    std::vector<ViewIn> views;
    LayoutOut expected;
    std::vector<Step> steps;
};

#define MH4N_CASE(...) {__VA_ARGS__},
const std::vector<Case> kCases = {
#include "screen_layout_expected.inc"
};
#undef MH4N_CASE

Inputs InputsOf(const Case& c) {
    Inputs in;
    in.window_w = c.window_w;
    in.window_h = c.window_h;
    in.mode = ParseMode(c.mode);
    in.top_width = c.top_width;
    in.aspect_ratio = c.aspect;
    auto& o = in.overlay;
    o.corner = static_cast<Port::ScreenOverlay::Corner>(c.corner);
    o.height = c.overlay_height;
    o.opacity = c.opacity;
    o.crop_left = c.crop[0];
    o.crop_top = c.crop[1];
    o.crop_right = c.crop[2];
    o.crop_bottom = c.crop[3];
    o.x = c.at[0];
    o.y = c.at[1];
    for (std::size_t i = 0; i < c.views.size(); ++i) {
        o.views[i] = {c.views[i].height, c.views[i].x, c.views[i].y,
                      c.views[i].crop[0], c.views[i].crop[1], c.views[i].crop[2], c.views[i].crop[3]};
    }
    return in;
}

std::array<u32, 4> RectOf(const Rect& r) { return {r.left, r.top, r.right, r.bottom}; }
std::array<float, 4> CropOf(const RectF& r) { return {r.left, r.top, r.right, r.bottom}; }

std::string Show(const std::array<u32, 4>& r) {
    return "{" + std::to_string(r[0]) + "," + std::to_string(r[1]) + "," + std::to_string(r[2]) + "," +
           std::to_string(r[3]) + "}";
}

// Returns the first difference, "" for none.
std::string CompareLayout(const Layout& got, const LayoutOut& want) {
    if (got.width != want.width || got.height != want.height) {
        return "size " + std::to_string(got.width) + "x" + std::to_string(got.height) + " != " +
               std::to_string(want.width) + "x" + std::to_string(want.height);
    }
    if (got.top_shown != (want.top_shown != 0) || got.bottom_shown != (want.bottom_shown != 0)) {
        return "which screens are shown";
    }
    if (RectOf(got.top) != want.top) {
        return "top " + Show(RectOf(got.top)) + " != " + Show(want.top);
    }
    if (RectOf(got.bottom) != want.bottom) {
        return "bottom " + Show(RectOf(got.bottom)) + " != " + Show(want.bottom);
    }
    if (got.top_opacity != want.top_opacity || got.bottom_opacity != want.bottom_opacity) {
        return "opacity";
    }
    if (CropOf(got.bottom_crop) != want.bottom_crop) {
        return "bottom crop";
    }
    if (got.view_count != want.view_count || want.views.size() != want.view_count) {
        return "view count " + std::to_string(got.view_count) + " != " + std::to_string(want.view_count);
    }
    for (u32 i = 0; i < got.view_count; ++i) {
        if (RectOf(got.views[i].screen) != want.views[i].rect) {
            return "view " + std::to_string(i) + " " + Show(RectOf(got.views[i].screen)) + " != " +
                   Show(want.views[i].rect);
        }
        if (CropOf(got.views[i].crop) != want.views[i].crop) {
            return "view " + std::to_string(i) + " crop";
        }
    }
    return {};
}

}  // namespace

TEST_CASE("the table is the generated one, and covers the cases the WP asks for") {
    REQUIRE(kCases.size() > 100);
    std::set<std::tuple<std::string, u32, u32, u32>> modes;
    std::set<std::string> aspects, zero_width_modes;
    std::size_t overlapping_views = 0;
    std::set<int> corners;
    std::size_t placed = 0, cropped = 0, two_views = 0, touches = 0, releases = 0, view_presses = 0,
                clipped_moves = 0, outside_presses = 0;
    for (const auto& c : kCases) {
        modes.insert({c.mode, c.window_w, c.window_h, c.top_width});
        if (std::string(c.mode) == "single") {
            aspects.insert(c.aspect);
        }
        if (std::string(c.mode) == "overlay" && c.overlay_height > 0.0f) {
            corners.insert(c.corner);
            placed += c.at[0] >= 0.0f && c.at[1] >= 0.0f;
            cropped += c.crop != std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};
            two_views += c.views.size() == 2;
        }
        if (c.top_width == 0) {
            zero_width_modes.insert(c.mode);
        }
        if (c.expected.views.size() == 2) {
            const auto& a = c.expected.views[0].rect;
            const auto& b = c.expected.views[1].rect;
            overlapping_views += a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
        }
        touches += !c.steps.empty();
        for (const auto& s : c.steps) {
            releases += s.op == 'R';
            outside_presses += s.op == 'P' && s.handled == 0;
            clipped_moves += s.op == 'M' && s.pressed == 1;
            view_presses += s.op == 'P' && s.handled == 1 && !c.expected.views.empty();
        }
    }
    for (const char* mode : {"overlay", "stacked", "single", "side"}) {
        for (auto [w, h] : {std::pair<u32, u32>{800, 480}, {1920, 1080}, {3440, 1440}, {1280, 1024}}) {
            for (u32 top_width : {400u, 427u, 573u}) {
                CHECK_MESSAGE(modes.count({mode, w, h, top_width}) == 1,
                              mode << " " << w << "x" << h << " top " << top_width);
            }
        }
    }
    CHECK(aspects == std::set<std::string>{"default", "stretch", "4:3", "16:9", "16:10", "21:9"});
    CHECK(corners == std::set<int>{0, 1, 2, 3});
    CHECK(placed >= 3);
    CHECK(cropped >= 3);
    CHECK(two_views >= 1);
    CHECK(zero_width_modes == std::set<std::string>{"overlay", "stacked", "single", "side"});  // 0 means 400
    CHECK(overlapping_views >= 1);
    CHECK(touches >= 10);
    CHECK(releases >= 10);
    CHECK(outside_presses >= 5);
    CHECK(clipped_moves >= 10);
    CHECK(view_presses >= 5);
}

TEST_CASE("Compute reproduces the layouts Azahar's code makes") {
    std::size_t failures = 0;
    for (const auto& c : kCases) {
        const Layout got = Compute(InputsOf(c));
        const std::string diff = CompareLayout(got, c.expected);
        if (!diff.empty() && ++failures <= 10) {
            FAIL_CHECK(std::string(c.name) << ": " << diff);
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("TouchTracker reproduces the touch points Azahar's code makes") {
    std::size_t steps = 0, failures = 0;
    for (const auto& c : kCases) {
        if (c.steps.empty()) {
            continue;
        }
        const Layout layout = Compute(InputsOf(c));
        TouchTracker tracker;
        std::size_t index = 0;
        for (const auto& s : c.steps) {
            ++steps;
            int handled = -1;
            if (s.op == 'P') {
                handled = tracker.Press(layout, s.x, s.y) ? 1 : 0;
            } else if (s.op == 'M') {
                tracker.Move(layout, s.x, s.y);
            } else {
                tracker.Release();
            }
            const auto point = tracker.Point();
            // the table keeps what the HID module reads; it never reads past the last pixel
            REQUIRE_MESSAGE(s.px < 320, std::string(c.name));
            REQUIRE_MESSAGE(s.py < 240, std::string(c.name));
            bool ok = handled == s.handled && point.has_value() == (s.pressed != 0);
            if (ok && point) {
                ok = point->fx == s.fx && point->fy == s.fy && point->x == s.px && point->y == s.py;
            }
            if (!ok && ++failures <= 10) {
                FAIL_CHECK(std::string(c.name) << " step " << index << " (" << s.op << " " << s.x << "," << s.y << ")");
            }
            ++index;
        }
    }
    CHECK(steps > 100);
    CHECK(failures == 0);
}

TEST_CASE("mode names: the four of the settings, anything else is the overlay") {
    for (const Mode mode : {Mode::Overlay, Mode::Stacked, Mode::Single, Mode::Side}) {
        bool known = false;
        CHECK(ParseMode(ModeName(mode), &known) == mode);
        CHECK(known);
    }
    for (const char* name : {"windows", "", "Overlay", "stack", "separate"}) {
        bool known = true;
        CHECK(ParseMode(name, &known) == Mode::Overlay);
        CHECK_FALSE(known);
    }
    CHECK(ParseMode("windows") == Mode::Overlay);
}

TEST_CASE("a touch tracker holds nothing before a press and after a release") {
    Inputs in;
    in.window_w = 1920;
    in.window_h = 1080;
    in.mode = Mode::Stacked;
    const Layout layout = Compute(in);
    TouchTracker tracker;
    CHECK_FALSE(tracker.Point().has_value());
    tracker.Move(layout, 960, 810);  // a move without a press does nothing
    CHECK_FALSE(tracker.Point().has_value());
    CHECK(tracker.Press(layout, 960, 810));
    REQUIRE(tracker.Point().has_value());
    CHECK(tracker.Point()->x == 160);
    CHECK(tracker.Point()->y == 120);
    tracker.Release();
    CHECK_FALSE(tracker.Point().has_value());
    CHECK_FALSE(tracker.Press(layout, 960, 100));  // the top screen is not the touch screen
}

TEST_CASE("the touch point never passes the last pixel, whatever float the crop makes") {
    Inputs in;
    in.window_w = 800;
    in.window_h = 480;
    in.mode = Mode::Overlay;
    in.overlay.height = 0.5f;
    Layout layout = Compute(in);
    // a crop reaching past the screen, which no clamped overlay makes: the fraction is above 1
    layout.bottom_crop = {0.5f, 0.5f, 1.5f, 1.5f};
    TouchTracker tracker;
    REQUIRE(tracker.Press(layout, layout.bottom.right - 1, layout.bottom.bottom - 1));
    REQUIRE(tracker.Point().has_value());
    CHECK(tracker.Point()->fx > 1.0f);
    CHECK(tracker.Point()->x == 319);
    CHECK(tracker.Point()->y == 239);
}

TEST_CASE("a touch on a view when the layout changes under it") {
    Inputs in;
    in.window_w = 1920;
    in.window_h = 1080;
    in.mode = Mode::Overlay;
    in.overlay.height = 0.3f;
    in.overlay.views[0] = {0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f};
    const Layout with_view = Compute(in);
    REQUIRE(with_view.view_count == 1);
    TouchTracker tracker;
    REQUIRE(tracker.Press(with_view, 10, 10));
    const auto before = tracker.Point();
    REQUIRE(before.has_value());

    // the view is gone from the new layout: the touch is held and does not move (Azahar's rule)
    in.overlay.views[0] = {};
    const Layout without_view = Compute(in);
    REQUIRE(without_view.view_count == 0);
    tracker.Move(without_view, 1700, 900);
    REQUIRE(tracker.Point().has_value());
    CHECK(tracker.Point()->fx == before->fx);
    CHECK(tracker.Point()->fy == before->fy);
    tracker.Release();
    CHECK_FALSE(tracker.Point().has_value());
}

#endif
