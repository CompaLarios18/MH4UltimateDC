// present_test.cpp - N2b-05, the compositor (native/src/present/, docs/native/presentation.md).
//
// Synthetic DisplayFrames (backend images and CPU pixels, scale 1 and 3, both orientations) are composed into
// offscreen targets and read back with Backend::ReadImage. Run on whatever Vulkan device the machine has (lavapipe
// in the cloud and in Linux CI); a case that needs a device skips with a message without one, and fails under
// MH4N_REQUIRE_VULKAN=1 (the helper logs the device name).
//
// Where the expected pixels come from, never from the compositor: the layouts' rectangles are the ones Azahar's own
// code printed (screen_layout_expected.inc, N2b-04's table); the source pixels are a function of the screen and the
// displayed pixel, made here (Pix); and the rule that places them is nearest-neighbour sampling worked out from
// the definition (the destination pixel's centre, mapped through the rectangle and the crop to a displayed pixel).
// A destination pixel whose centre falls within 2% of a texel boundary accepts either neighbouring texel (the sampler may go either
// way there; a blended one there is not compared); every other pixel of the whole target is compared exactly. An opacity blends as src * a + dst * (1 - a), compared
// within 1/255.
//
// The backend's owner-rule checks (MH4N_RHI_CHECKS) are on in every build: every case installs a handler that
// counts breaches and requires zero. Run under the validation layer with
//     python3 tools/native/vk_layer_run.py --build build/native -R '^present_test$'
#include <doctest/doctest.h>

#if !__has_include("platform.h")
TEST_CASE("present_test needs the port's platform.h (the repository around native/)" * doctest::skip()) {}
#else

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu/backend/targets/upload.h"
#include "present/compositor.h"
#include "present/shader_compile.h"
#include "screen_layout/screen_layout.h"
#include "support/vulkan_device.h"

namespace {

namespace rhi = ::mh4n::gpu::rhi;
namespace sl = ::mh4n::screen_layout;
namespace pr = ::mh4n::present;
using mh4n::u32;
using mh4n::u64;
using mh4n::u8;

// ---- N2b-04's table (the layouts' rectangles, from Azahar's own code) ---------------------------------------------

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
    char op;
    u32 x, y;
    int handled;
    int pressed;
    float fx, fy;
    u32 px, py;
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

sl::Inputs InputsOf(const Case& c) {
    sl::Inputs in;
    in.window_w = c.window_w;
    in.window_h = c.window_h;
    in.mode = sl::ParseMode(c.mode);
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
        o.views[i] = {c.views[i].height, c.views[i].x, c.views[i].y, c.views[i].crop[0],
                      c.views[i].crop[1], c.views[i].crop[2], c.views[i].crop[3]};
    }
    return in;
}

// ---- what the reference draws -----------------------------------------------------------------------------------

// A layout as plain rectangles: the oracle's, or a computed Layout's.
struct Geo {
    u32 width = 0, height = 0;
    bool top_shown = false, bottom_shown = false;
    sl::Rect top, bottom;
    float top_opacity = 1.0f, bottom_opacity = 1.0f;
    sl::RectF bottom_crop;
    std::vector<sl::View> views;
};

sl::Rect RectOf(const std::array<u32, 4>& r) {
    return sl::Rect{r[0], r[1], r[2], r[3]};
}
sl::RectF CropOf(const std::array<float, 4>& r) {
    return sl::RectF{r[0], r[1], r[2], r[3]};
}

Geo GeoOf(const LayoutOut& l) {
    Geo g;
    g.width = l.width;
    g.height = l.height;
    g.top_shown = l.top_shown != 0;
    g.bottom_shown = l.bottom_shown != 0;
    g.top = RectOf(l.top);
    g.bottom = RectOf(l.bottom);
    g.top_opacity = l.top_opacity;
    g.bottom_opacity = l.bottom_opacity;
    g.bottom_crop = CropOf(l.bottom_crop);
    for (const ViewOut& v : l.views) {
        g.views.push_back(sl::View{RectOf(v.rect), CropOf(v.crop)});
    }
    return g;
}

Geo GeoOf(const sl::Layout& l) {
    Geo g;
    g.width = l.width;
    g.height = l.height;
    g.top_shown = l.top_shown;
    g.bottom_shown = l.bottom_shown;
    g.top = l.top;
    g.bottom = l.bottom;
    g.top_opacity = l.top_opacity;
    g.bottom_opacity = l.bottom_opacity;
    g.bottom_crop = l.bottom_crop;
    for (u32 i = 0; i < l.view_count; ++i) {
        g.views.push_back(l.views[i]);
    }
    return g;
}

struct Rgb {
    u8 r, g, b;
    bool operator==(const Rgb&) const = default;
};

// The colour of the displayed pixel (x, y) (in scaled displayed pixels) of screen `id`. Neighbouring pixels differ
// in every channel, so a pixel taken from the wrong place shows.
Rgb Pix(u32 id, u32 x, u32 y) {
    return Rgb{static_cast<u8>((x * 7 + y * 13 + id * 50 + 3) & 255), static_cast<u8>((x * 11 + y * 5 + id * 97 + 40) & 255),
               static_cast<u8>((x * 3 + y * 17 + id * 29 + 90) & 255)};
}

// A screen of a test frame.
struct Screen {
    u32 id = 0;
    u32 w = 400, h = 240;  // displayed size at 1x
    u32 scale = 1;
    pr::Orientation orientation = pr::Orientation::Upright;
    bool cpu = false;
    u32 ScaledW() const { return w * scale; }
    u32 ScaledH() const { return h * scale; }
};

struct Ref {
    std::vector<u8> expected;  // RGBA
    std::vector<u8> checked;   // 1: compared
    // Pixels whose sample point falls on a texel boundary (or within 2% of one): the colours the neighbouring texels
    // would give; the sampler may take either, so the pixel matches the expected colour or any of these.
    std::unordered_map<std::size_t, std::vector<Rgb>> alternatives;
    u32 width = 0, height = 0;
    u64 covered = 0;
    int tolerance = 0;
};

Ref NewRef(u32 w, u32 h) {
    Ref r;
    r.width = w;
    r.height = h;
    r.expected.assign(static_cast<std::size_t>(w) * h * 4, 0);
    r.checked.assign(static_cast<std::size_t>(w) * h, 1);
    for (std::size_t i = 3; i < r.expected.size(); i += 4) {
        r.expected[i] = 255;
    }
    return r;
}

// The texel indices a sample at `pos` (in texels) may take: one, or two when `pos` is within 2% of a boundary.
std::vector<u32> TexelsAt(double pos, u32 count) {
    const auto clamp_index = [&](double v) { return static_cast<u32>(std::clamp(std::floor(v), 0.0, count - 1.0)); };
    const u32 lo = clamp_index(pos - 0.02), hi = clamp_index(pos + 0.02);
    if (lo == hi) {
        return {lo};
    }
    return {lo, hi};
}

// Draws `s` into `rect` (clipped to the target) with `crop` and opacity `a`, as nearest sampling of the displayed screen.
void RefDraw(Ref& r, const Screen& s, const sl::Rect& rect, const sl::RectF& crop, double a) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    if (a < 1.0) {
        r.tolerance = 1;
    }
    const double rw = rect.right - rect.left;
    const double rh = rect.bottom - rect.top;
    const u32 y0 = std::min(rect.top, r.height), y1 = std::min(rect.bottom, r.height);
    const u32 x0 = std::min(rect.left, r.width), x1 = std::min(rect.right, r.width);
    for (u32 y = y0; y < y1; ++y) {
        const double fy = crop.top + ((y + 0.5 - rect.top) / rh) * (crop.bottom - crop.top);
        const std::vector<u32> iys = TexelsAt(fy * s.ScaledH(), s.ScaledH());
        for (u32 x = x0; x < x1; ++x) {
            const double fx = crop.left + ((x + 0.5 - rect.left) / rw) * (crop.right - crop.left);
            const std::vector<u32> ixs = TexelsAt(fx * s.ScaledW(), s.ScaledW());
            const std::size_t at = (static_cast<std::size_t>(y) * r.width + x);
            ++r.covered;
            const bool ambiguous = iys.size() * ixs.size() > 1;
            const bool dst_ambiguous = r.alternatives.count(at) != 0;
            if (a < 1.0 && (ambiguous || dst_ambiguous)) {
                r.checked[at] = 0;  // a blend of a colour that could be either: not compared
                continue;
            }
            const Rgb src = Pix(s.id, ixs[0], iys[0]);
            u8* dst = &r.expected[at * 4];
            const u8 channels[3] = {src.r, src.g, src.b};
            for (int c = 0; c < 3; ++c) {
                dst[c] = static_cast<u8>(std::lround(channels[c] * a + dst[c] * (1.0 - a)));
            }
            if (a >= 1.0) {
                r.alternatives.erase(at);
                if (ambiguous) {
                    std::vector<Rgb>& alt = r.alternatives[at];
                    for (u32 iy : iys) {
                        for (u32 ix : ixs) {
                            alt.push_back(Pix(s.id, ix, iy));
                        }
                    }
                }
            }
        }
    }
}

// The reference of a frame in a layout: the top, the bottom with its opacity and crop, the views over it.
Ref Reference(const Geo& g, const Screen* top, const Screen* bottom, u32 w, u32 h) {
    Ref r = NewRef(w, h);
    if (g.top_shown && top != nullptr) {
        RefDraw(r, *top, g.top, sl::RectF{0, 0, 1, 1}, std::clamp<double>(g.top_opacity, 0.0, 1.0));
    }
    if (g.bottom_shown && bottom != nullptr) {
        RefDraw(r, *bottom, g.bottom, g.bottom_crop, std::clamp<double>(g.bottom_opacity, 0.0, 1.0));
        for (const sl::View& v : g.views) {
            RefDraw(r, *bottom, v.screen, v.crop, std::clamp<double>(g.bottom_opacity, 0.0, 1.0));
        }
    }
    return r;
}

// ---- the device -----------------------------------------------------------------------------------------------

struct BreachCapture {
    std::mutex mutex;
    std::vector<rhi::OwnerRule> rules;
    rhi::OwnerRuleHandler previous;
    BreachCapture() {
        previous = rhi::SetOwnerRuleHandler([this](const rhi::OwnerRuleBreach& breach) {
            std::lock_guard lock(mutex);
            rules.push_back(breach.rule);
        });
    }
    ~BreachCapture() { rhi::SetOwnerRuleHandler(previous); }
    std::size_t Total() {
        std::lock_guard lock(mutex);
        return rules.size();
    }
    int Count(rhi::OwnerRule rule) {
        std::lock_guard lock(mutex);
        return static_cast<int>(std::count(rules.begin(), rules.end(), rule));
    }
};

struct Env {
    BreachCapture breaches;
    std::shared_ptr<rhi::Backend> backend;
    std::unique_ptr<pr::Compositor> compositor;

    ~Env() {
        compositor.reset();
        if (backend) {
            backend->WaitIdle();
        }
    }
};

// nullptr after a skip (no device).
std::unique_ptr<Env> MakeEnv() {
    auto env = std::make_unique<Env>();
    env->backend = mh4n::test::RequireBackend();
    if (!env->backend) {
        return nullptr;
    }
    rhi::Result<std::unique_ptr<pr::Compositor>> compositor = pr::Compositor::Create(env->backend);
    REQUIRE_MESSAGE(compositor.ok(), compositor.error());
    env->compositor = std::move(*compositor);
    return env;
}

std::shared_ptr<rhi::Image> MakeImage(rhi::Backend& backend, rhi::Format format, u32 w, u32 h, const char* name) {
    rhi::ImageDesc desc;
    desc.format = format;
    desc.width = w;
    desc.height = h;
    desc.name = name;
    rhi::Result<std::shared_ptr<rhi::Image>> image = backend.CreateImage(desc);
    REQUIRE_MESSAGE(image.ok(), image.error());
    return *image;
}

// The padding around a backend image's region, so that a read outside the region shows.
constexpr u32 kRegionX = 3, kRegionY = 2, kPadRight = 2, kPadBottom = 2;
constexpr u8 kGarbage[4] = {255, 0, 255, 255};  // magenta

// A ScreenImage of `s`: CPU pixels, or a backend image filled by a submission whose timeline value is the screen's
// (not waited for here: the compositor's submission waits for it). Call with no recorder open on this thread.
pr::ScreenImage MakeScreen(rhi::Backend& backend, const Screen& s) {
    pr::ScreenImage out;
    out.width = s.w;
    out.height = s.h;
    out.scale = s.scale;
    out.orientation = s.orientation;
    if (s.cpu) {
        REQUIRE(s.scale == 1);
        auto pixels = std::make_shared<std::vector<u8>>(static_cast<std::size_t>(s.w) * s.h * 4);
        for (u32 y = 0; y < s.h; ++y) {
            for (u32 x = 0; x < s.w; ++x) {
                const Rgb p = Pix(s.id, x, y);
                u8* at = &(*pixels)[(static_cast<std::size_t>(y) * s.w + x) * 4];
                at[0] = p.r;
                at[1] = p.g;
                at[2] = p.b;
                at[3] = 17;  // the alpha of a screen is ignored
            }
        }
        out.pixels = pixels;
        return out;
    }
    const u32 stored_w = s.orientation == pr::Orientation::Rotated90 ? s.ScaledH() : s.ScaledW();
    const u32 stored_h = s.orientation == pr::Orientation::Rotated90 ? s.ScaledW() : s.ScaledH();
    const u32 image_w = kRegionX + stored_w + kPadRight;
    const u32 image_h = kRegionY + stored_h + kPadBottom;
    std::shared_ptr<rhi::Image> image = MakeImage(backend, rhi::Format::Rgba8Unorm, image_w, image_h, "screen image");
    std::vector<u8> bytes(static_cast<std::size_t>(image_w) * image_h * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        std::memcpy(&bytes[i], kGarbage, 4);
    }
    for (u32 y = 0; y < s.ScaledH(); ++y) {
        for (u32 x = 0; x < s.ScaledW(); ++x) {
            const pr::PixelPos at = pr::ToImagePixel(s.orientation, s.ScaledH(), x, y);
            const Rgb p = Pix(s.id, x, y);
            u8* dst = &bytes[((static_cast<std::size_t>(kRegionY + at.y)) * image_w + kRegionX + at.x) * 4];
            dst[0] = p.r;
            dst[1] = p.g;
            dst[2] = p.b;
            dst[3] = 255;
        }
    }
    rhi::Recorder rec = backend.BeginRecording();
    rhi::BufferImageRegion region;
    region.image_rect = rhi::Rect{0, 0, image_w, image_h};
    rhi::Result<void> uploaded = rhi::UploadToImage(backend, rec, *image, region, bytes);
    REQUIRE_MESSAGE(uploaded.ok(), uploaded.error());
    const rhi::TimelineValue value = backend.Submit(std::move(rec));
    out.image = image;
    out.region = pr::ImageRegion{kRegionX, kRegionY, stored_w, stored_h};
    out.timeline_value = value;
    return out;
}

// The validation layer (1.3.275) does not count a host wait on a timeline semaphore as ordering: memory that a readback
// copy wrote and that the allocator then hands to a staging buffer of the next upload shows as a hazard between the two
// GPU commands, though the host waited for the first. vkDeviceWaitIdle is a wait the layer understands, so the tests
// make one after every readback. (The compositor's own submissions are ordered by the waits and fences it uses.)
void LayerSees(rhi::Backend& backend) {
    vkDeviceWaitIdle(rhi::Handles(backend).device);
}

struct Composed {
    std::vector<u8> pixels;  // RGBA (BGRA for a Bgra8Unorm target), rows packed
    std::vector<rhi::Wait> waits;
};

Composed ComposeToImage(Env& env, u32 w, u32 h, const pr::DisplayFrame& frame, const sl::Layout& layout,
                        const pr::ComposeOptions& options = {}, rhi::Format format = rhi::Format::Rgba8Unorm) {
    Composed out;
    std::shared_ptr<rhi::Image> target = MakeImage(*env.backend, format, w, h, "target");
    rhi::Recorder rec = env.backend->BeginRecording();
    pr::ComposeResult result = env.compositor->Compose(rec, *target, frame, layout, options);
    REQUIRE(result.drawn);
    out.waits = result.waits;
    const rhi::TimelineValue value = env.compositor->Submit(std::move(rec), std::move(result));
    env.backend->WaitIdle(value);
    rhi::Result<std::vector<u8>> read = env.backend->ReadImage(*target, 0, 0, w, h);
    REQUIRE_MESSAGE(read.ok(), read.error());
    LayerSees(*env.backend);
    out.pixels = *read;
    REQUIRE(out.pixels.size() == static_cast<std::size_t>(w) * h * 4);
    return out;
}

struct Compared {
    u64 mismatches = 0;
    u64 compared = 0;
    std::string first;
};

Compared Compare(const std::vector<u8>& got, const Ref& ref) {
    Compared out;
    for (u32 y = 0; y < ref.height; ++y) {
        for (u32 x = 0; x < ref.width; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * ref.width + x;
            if (!ref.checked[at]) {
                continue;
            }
            ++out.compared;
            bool same = true;
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(static_cast<int>(got[at * 4 + c]) - static_cast<int>(ref.expected[at * 4 + c]));
                same = same && d <= ref.tolerance;
            }
            if (!same) {
                const auto found = ref.alternatives.find(at);
                if (found != ref.alternatives.end() && got[at * 4 + 3] == 255) {
                    for (const Rgb& alt : found->second) {
                        same = same || (got[at * 4] == alt.r && got[at * 4 + 1] == alt.g && got[at * 4 + 2] == alt.b);
                    }
                }
            }
            if (!same) {
                if (out.mismatches++ == 0) {
                    char text[200];
                    std::snprintf(text, sizeof text, "(%u,%u) got %u,%u,%u,%u want %u,%u,%u,%u", x, y, got[at * 4],
                                  got[at * 4 + 1], got[at * 4 + 2], got[at * 4 + 3], ref.expected[at * 4],
                                  ref.expected[at * 4 + 1], ref.expected[at * 4 + 2], ref.expected[at * 4 + 3]);
                    out.first = text;
                }
            }
        }
    }
    return out;
}

std::array<u8, 4> PixelAt(const std::vector<u8>& pixels, u32 w, u32 x, u32 y) {
    const std::size_t at = (static_cast<std::size_t>(y) * w + x) * 4;
    return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
}
std::array<u8, 4> Opaque(const Rgb& c) {
    return {c.r, c.g, c.b, 255};
}

// The four ways of putting the two screens on the device, so that every case covers CPU pixels and backend images,
// scale 1 and 3 and both orientations.
struct Variant {
    Screen top, bottom;
};
Variant VariantOf(int index, u32 top_width) {
    Variant v;
    v.top.id = 1;
    v.top.w = top_width;
    v.top.h = 240;
    v.bottom.id = 2;
    v.bottom.w = 320;
    v.bottom.h = 240;
    switch (index & 3) {
    case 0:  // renderer-like: scale 3 rotated, and CPU pixels
        v.top.scale = 3;
        v.top.orientation = pr::Orientation::Rotated90;
        v.bottom.cpu = true;
        break;
    case 1:
        v.top.cpu = true;
        v.bottom.scale = 1;
        v.bottom.orientation = pr::Orientation::Upright;
        break;
    case 2:
        v.top.scale = 1;
        v.top.orientation = pr::Orientation::Rotated90;
        v.bottom.scale = 3;
        v.bottom.orientation = pr::Orientation::Upright;
        break;
    default:
        v.top.scale = 3;
        v.top.orientation = pr::Orientation::Upright;
        v.bottom.scale = 1;
        v.bottom.orientation = pr::Orientation::Rotated90;
        break;
    }
    return v;
}

pr::DisplayFrame FrameOf(rhi::Backend& backend, const Variant& v, bool with_top = true, bool with_bottom = true) {
    pr::DisplayFrame frame;
    if (with_top) {
        frame.top = MakeScreen(backend, v.top);
    }
    if (with_bottom) {
        frame.bottom = MakeScreen(backend, v.bottom);
    }
    return frame;
}

// The corner and centre pixels of every shown screen at its destination rectangle (the top, the bottom, each view),
// checked on their own and strictly: the exact colour of the source texel under the pixel's centre, no tolerance.
// Where the sample point falls within 2% of a texel boundary (exact integer ratios put it on a texel edge) the
// sampler may take either neighbour, so the pixel must be one of the neighbours' colours; those points are counted
// as ambiguous. Points that are blended (opacity < 1), hidden under a later layer or outside the target are not
// counted (`excluded`).
struct PointStats {
    u64 strict = 0, ambiguous = 0, excluded = 0;
    void Add(const PointStats& o) {
        strict += o.strict;
        ambiguous += o.ambiguous;
        excluded += o.excluded;
    }
};

PointStats CheckCornersAndCentres(const Geo& g, const Screen* top, const Screen* bottom, const std::vector<u8>& pixels,
                                  u32 w, u32 h) {
    struct Layer {
        const Screen* screen;
        sl::Rect rect;
        sl::RectF crop;
        double opacity;
    };
    std::vector<Layer> layers;
    if (g.top_shown && top != nullptr) {
        layers.push_back({top, g.top, sl::RectF{0, 0, 1, 1}, std::clamp<double>(g.top_opacity, 0.0, 1.0)});
    }
    if (g.bottom_shown && bottom != nullptr) {
        const double a = std::clamp<double>(g.bottom_opacity, 0.0, 1.0);
        layers.push_back({bottom, g.bottom, g.bottom_crop, a});
        for (const sl::View& v : g.views) {
            layers.push_back({bottom, v.screen, v.crop, a});
        }
    }
    PointStats stats;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& l = layers[i];
        if (l.rect.right <= l.rect.left || l.rect.bottom <= l.rect.top) {
            continue;
        }
        const std::array<std::pair<u32, u32>, 5> points = {{{l.rect.left, l.rect.top},
                                                             {l.rect.right - 1, l.rect.top},
                                                             {l.rect.left, l.rect.bottom - 1},
                                                             {l.rect.right - 1, l.rect.bottom - 1},
                                                             {(l.rect.left + l.rect.right) / 2, (l.rect.top + l.rect.bottom) / 2}}};
        for (const auto& [x, y] : points) {
            bool hidden = x >= w || y >= h || l.opacity < 1.0;
            for (std::size_t j = i + 1; j < layers.size() && !hidden; ++j) {
                hidden = layers[j].rect.Contains(x, y);
            }
            if (hidden) {
                ++stats.excluded;
                continue;
            }
            const double fx = l.crop.left + ((x + 0.5 - l.rect.left) / l.rect.Width()) * (l.crop.right - l.crop.left);
            const double fy = l.crop.top + ((y + 0.5 - l.rect.top) / l.rect.Height()) * (l.crop.bottom - l.crop.top);
            const std::vector<u32> ixs = TexelsAt(fx * l.screen->ScaledW(), l.screen->ScaledW());
            const std::vector<u32> iys = TexelsAt(fy * l.screen->ScaledH(), l.screen->ScaledH());
            const auto got = PixelAt(pixels, w, x, y);
            if (ixs.size() == 1 && iys.size() == 1) {
                ++stats.strict;
                CHECK_MESSAGE(got == Opaque(Pix(l.screen->id, ixs[0], iys[0])),
                              "layer " << i << " point (" << x << "," << y << ") got " << int(got[0]) << "," << int(got[1]) << ","
                                       << int(got[2]) << "," << int(got[3]));
            } else {
                ++stats.ambiguous;
                bool any = false;
                for (u32 iy : iys) {
                    for (u32 ix : ixs) {
                        any = any || got == Opaque(Pix(l.screen->id, ix, iy));
                    }
                }
                CHECK_MESSAGE(any, "layer " << i << " ambiguous point (" << x << "," << y << ")");
            }
        }
    }
    return stats;
}

// Composes one case of N2b-04's table and checks every pixel of the target against the reference built from the
// ORACLE's rectangles (the compositor gets the layout N2b-04's code computes).
PointStats RunOracleCase(Env& env, const Case& c, int variant, bool linear = false) {
    INFO("case " << c.name << " variant " << variant);
    const sl::Inputs in = InputsOf(c);
    const sl::Layout layout = sl::Compute(in);
    REQUIRE(layout.width == c.expected.width);
    REQUIRE(layout.height == c.expected.height);
    const u32 top_width = c.top_width == 0 ? 400 : c.top_width;
    const Variant v = VariantOf(variant, top_width);
    pr::DisplayFrame frame = FrameOf(*env.backend, v);
    pr::ComposeOptions options;
    options.linear_filter = linear;
    const Composed done = ComposeToImage(env, layout.width, layout.height, frame, layout, options);
    const Ref ref = Reference(GeoOf(c.expected), &v.top, &v.bottom, layout.width, layout.height);
    const Compared cmp = Compare(done.pixels, ref);
    CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " of " << cmp.compared << " pixels differ; first " << cmp.first);
    // The reference must not have skipped most of what was drawn.
    CHECK_MESSAGE((ref.covered == 0 || cmp.compared > 0), "nothing compared");
    u64 skipped = 0;
    for (u8 f : ref.checked) {
        skipped += f == 0;
    }
    CHECK_MESSAGE(skipped * 4 <= static_cast<u64>(layout.width) * layout.height, skipped << " pixels are on a texel boundary");
    // The corners and the centre of each screen's rectangle, strictly (see CheckCornersAndCentres).
    return CheckCornersAndCentres(GeoOf(c.expected), &v.top, &v.bottom, done.pixels, layout.width, layout.height);
}

const Case* FindCase(const char* name) {
    for (const Case& c : kCases) {
        if (std::string(c.name) == name) {
            return &c;
        }
    }
    return nullptr;
}

bool Contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

// ---- the modes -----------------------------------------------------------------------------------------------

TEST_CASE("compose/every_mode_at_every_size_of_the_layout_table") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    std::size_t ran = 0;
    int variant = 0;
    for (const Case& c : kCases) {
        if (!Contains(c.name, "modes_")) {
            continue;
        }
        RunOracleCase(*env, c, variant++);
        ++ran;
    }
    CHECK(ran == 48);  // 4 modes x 4 sizes x 3 top widths
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/corners_and_centres_of_every_screen_land_exactly_in_the_tables_rectangles") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    // The 48 mode/size cases of N2b-04's table, each with the four ways of giving the screens: CPU pixels, backend
    // images at scale 1 and 3, upright and rotated.
    PointStats total;
    std::size_t cases = 0;
    for (const Case& c : kCases) {
        if (!Contains(c.name, "modes_")) {
            continue;
        }
        for (int variant = 0; variant < 4; ++variant) {
            total.Add(RunOracleCase(*env, c, variant));
            ++cases;
        }
    }
    CHECK(cases == 192);
    const u64 points = total.strict + total.ambiguous;
    std::printf("corner and centre points: %llu strict, %llu ambiguous (sample point within 2%% of a texel boundary: "
                "either neighbour accepted), %llu not counted (hidden, blended or outside)\n",
                static_cast<unsigned long long>(total.strict), static_cast<unsigned long long>(total.ambiguous),
                static_cast<unsigned long long>(total.excluded));
    std::fflush(stdout);
    CHECK(points > 0);
    CHECK_MESSAGE(total.strict * 10 >= points * 9, total.strict << " strict of " << points << " points");
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/a_non_integer_ratio_layout_has_every_corner_and_centre_strict") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    // 1920x1080 and 1280x1024: the screens are scaled by ratios that are not whole numbers, and each of the four
    // ways of giving them (a scale-3 rotated backend image among them) has all of its points strict. (Other table
    // cases have a point or two on a texel edge, e.g. modes_stacked_1280x1024_400: they are in the count above.)
    for (const char* name : {"modes_overlay_1920x1080_400", "modes_stacked_1920x1080_573", "modes_side_1280x1024_573", "modes_stacked_1280x1024_427", "modes_overlay_1920x1080_573"}) {
        const Case* c = FindCase(name);
        REQUIRE(c != nullptr);
        for (int variant = 0; variant < 4; ++variant) {
            INFO(name << " variant " << variant);
            const PointStats stats = RunOracleCase(*env, *c, variant);
            CHECK(stats.ambiguous == 0);
            CHECK(stats.strict >= 8);
        }
    }
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/the_four_ways_of_giving_screens_at_one_size") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    // The widened top screen (573) in the stacked mode at 1280x1024, and the standard one in the overlay mode, each
    // with the four variants: CPU pixels, backend images at scale 1 and 3, both orientations.
    for (const char* name : {"modes_stacked_1280x1024_573", "modes_overlay_1280x1024_400", "modes_side_800x480_427"}) {
        const Case* c = FindCase(name);
        REQUIRE(c != nullptr);
        for (int variant = 0; variant < 4; ++variant) {
            RunOracleCase(*env, *c, variant);
        }
    }
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/a_widened_top_screen_fills_its_rectangle_corners_and_centre_exact") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    // (1) In the layout the table has for a 573-wide top screen: every pixel of the target (the reference is built
    // from Azahar's rectangle), in each of the four ways of giving the screens.
    const Case* c = FindCase("modes_stacked_800x480_573");
    REQUIRE(c != nullptr);
    REQUIRE(c->top_width == 573);
    for (int variant : {0, 1, 2, 3}) {
        RunOracleCase(*env, *c, variant);
    }
    // (2) A rectangle the size of the screen (573x240 at 1:1): the corners and the centre are the screen's own pixels,
    // exactly, and the pixels around the rectangle are black.
    sl::Layout layout;
    layout.width = 600;
    layout.height = 260;
    layout.top_shown = true;
    layout.top = sl::Rect{10, 10, 10 + 573, 10 + 240};
    for (const int variant : {1, 2}) {  // the top screen at scale 1: CPU pixels, and a rotated backend image
        INFO("variant " << variant);
        const Variant v = VariantOf(variant, 573);
        REQUIRE(v.top.scale == 1);
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout);
        CHECK(PixelAt(done.pixels, 600, 10, 10) == Opaque(Pix(1, 0, 0)));
        CHECK(PixelAt(done.pixels, 600, 10 + 572, 10) == Opaque(Pix(1, 572, 0)));
        CHECK(PixelAt(done.pixels, 600, 10, 10 + 239) == Opaque(Pix(1, 0, 239)));
        CHECK(PixelAt(done.pixels, 600, 10 + 572, 10 + 239) == Opaque(Pix(1, 572, 239)));
        CHECK(PixelAt(done.pixels, 600, 10 + 286, 10 + 120) == Opaque(Pix(1, 286, 120)));
        const std::array<u8, 4> black = {0, 0, 0, 255};
        CHECK(PixelAt(done.pixels, 600, 9, 10) == black);
        CHECK(PixelAt(done.pixels, 600, 10, 9) == black);
        CHECK(PixelAt(done.pixels, 600, 10 + 573, 10 + 100) == black);
        CHECK(PixelAt(done.pixels, 600, 10 + 100, 10 + 240) == black);
        const Ref ref = Reference(GeoOf(layout), &v.top, nullptr, 600, 260);
        const Compared cmp = Compare(done.pixels, ref);
        CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " pixels differ; first " << cmp.first);
    }
    CHECK(env->breaches.Total() == 0);
}

// ---- the overlay: opacity, crop, views, an overlay with no height -------------------------------------------------

TEST_CASE("compose/overlay_opacity_blends_src_a_plus_dst_one_minus_a") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    for (const float opacity : {0.25f, 0.5f, 0.75f, 1.0f}) {
        INFO("opacity " << opacity);
        sl::Inputs in;
        in.window_w = 800;
        in.window_h = 480;
        in.mode = sl::Mode::Overlay;
        in.top_width = 400;
        in.overlay.corner = Port::ScreenOverlay::Corner::BottomRight;
        in.overlay.height = 0.6f;  // big: the bottom screen covers much of the top one
        in.overlay.opacity = opacity;
        const sl::Layout layout = sl::Compute(in);
        REQUIRE(layout.bottom_opacity == opacity);
        const Variant v = VariantOf(1, 400);  // top CPU pixels, bottom a backend image
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout);
        const Ref ref = Reference(GeoOf(layout), &v.top, &v.bottom, layout.width, layout.height);
        const Compared cmp = Compare(done.pixels, ref);
        CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " of " << cmp.compared << " pixels differ; first " << cmp.first);
        // One pixel by hand: the centre of the bottom screen's rectangle over the top screen, 1/255.
        REQUIRE(layout.bottom.Contains(layout.bottom.left + 1, layout.bottom.top + 1));
        const u32 x = layout.bottom.left + 1, y = layout.bottom.top + 1;
        REQUIRE(layout.top.Contains(x, y));
        const double a = opacity;
        const auto got = PixelAt(done.pixels, layout.width, x, y);
        // The source pixels: the top screen (400x240, scale 1) and the bottom screen, nearest.
        const auto sample = [&](const sl::Rect& r, const Screen& s, u32 px, u32 py) {
            const double X = (px + 0.5 - r.left) / r.Width() * s.ScaledW();
            const double Y = (py + 0.5 - r.top) / r.Height() * s.ScaledH();
            return Pix(s.id, static_cast<u32>(X), static_cast<u32>(Y));
        };
        const Rgb src = sample(layout.bottom, v.bottom, x, y);
        const Rgb dst = sample(layout.top, v.top, x, y);
        CHECK(std::abs(got[0] - (src.r * a + dst.r * (1 - a))) <= 1.0);
        CHECK(std::abs(got[1] - (src.g * a + dst.g * (1 - a))) <= 1.0);
        CHECK(std::abs(got[2] - (src.b * a + dst.b * (1 - a))) <= 1.0);
        CHECK(got[3] == 255);
    }
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/further_views_are_drawn_at_the_bottom_screens_opacity_over_the_other_screens") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    // platform.h: the further views are drawn "at the same opacity" as the overlay.
    sl::Inputs in;
    in.window_w = 1280;
    in.window_h = 720;
    in.mode = sl::Mode::Overlay;
    in.top_width = 400;
    in.overlay.corner = Port::ScreenOverlay::Corner::BottomRight;
    in.overlay.height = 0.3f;
    in.overlay.opacity = 0.5f;
    in.overlay.crop_left = 0.0f;
    in.overlay.crop_top = 0.0f;
    in.overlay.crop_right = 0.5f;
    in.overlay.crop_bottom = 1.0f;
    in.overlay.views[0] = {0.3f, 0.5f, 0.0f, 0.5f, 0.0f, 1.0f, 0.5f};
    in.overlay.views[1] = {0.2f, 1.0f, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f};
    const sl::Layout layout = sl::Compute(in);
    REQUIRE(layout.view_count == 2);
    REQUIRE(layout.bottom_opacity == 0.5f);
    for (const int variant : {1, 2}) {
        const Variant v = VariantOf(variant, 400);
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout);
        const Ref ref = Reference(GeoOf(layout), &v.top, &v.bottom, layout.width, layout.height);
        const Compared cmp = Compare(done.pixels, ref);
        CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " of " << cmp.compared << " pixels differ; first " << cmp.first);
        CHECK(cmp.compared > 100000);
        // a view's own corner over the top screen is the blend of the two, not the bottom screen's pixel
        const sl::View& view = layout.views[0];
        REQUIRE(layout.top.Contains(view.screen.left, view.screen.top));
        const auto got = PixelAt(done.pixels, layout.width, view.screen.left + 2, view.screen.top + 2);
        const Rgb top_px = Pix(1, static_cast<u32>((view.screen.left + 2.5 - layout.top.left) / layout.top.Width() * v.top.ScaledW()),
                               static_cast<u32>((view.screen.top + 2.5 - layout.top.top) / layout.top.Height() * v.top.ScaledH()));
        CHECK_FALSE(got == Opaque(top_px));
    }
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/a_crop_and_two_views_show_the_right_parts") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const char* names[] = {"crop_right_half", "crop_middle", "crop_thin", "views_one", "views_two", "views_overlap_two",
                           "views_clamps", "views_small_window"};
    int variant = 0;
    for (const char* name : names) {
        const Case* c = FindCase(name);
        REQUIRE_MESSAGE(c != nullptr, name);
        // the table must really have the feature the case is named for
        if (Contains(name, "crop_")) {
            CHECK(c->expected.bottom_crop != std::array<float, 4>{0, 0, 1, 1});
        } else if (std::string(name) == "views_two" || std::string(name) == "views_overlap_two") {
            CHECK(c->expected.view_count == 2);
        }
        RunOracleCase(*env, *c, variant++);
        RunOracleCase(*env, *c, variant++);
    }
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/a_view_is_the_cropped_part_of_the_bottom_screen_at_its_rectangle") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("views_two");
    REQUIRE(c != nullptr);
    REQUIRE(c->expected.view_count == 2);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const Variant v = VariantOf(2, 400);  // bottom: a backend image at scale 3
    pr::DisplayFrame frame = FrameOf(*env->backend, v);
    const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout);
    for (const ViewOut& view : c->expected.views) {
        const std::array<u32, 4>& r = view.rect;
        const auto& crop = view.crop;
        // The view's first and last pixel are the crop's corners: displayed pixel under (left, top) is
        // floor(crop.left * 960 + small), checked by the nearest rule on the rectangle.
        const double rw = r[2] - r[0], rh = r[3] - r[1];
        for (const auto& [x, y] : {std::pair<u32, u32>{r[0], r[1]}, {r[2] - 1, r[3] - 1}, {(r[0] + r[2]) / 2, (r[1] + r[3]) / 2}}) {
            const double fx = crop[0] + (x + 0.5 - r[0]) / rw * (crop[2] - crop[0]);
            const double fy = crop[1] + (y + 0.5 - r[1]) / rh * (crop[3] - crop[1]);
            const double X = fx * v.bottom.ScaledW(), Y = fy * v.bottom.ScaledH();
            const auto edge = [](double q) { return q - std::floor(q) < 0.01 || q - std::floor(q) > 0.99; };
            if (edge(X) || edge(Y)) {
                continue;
            }
            // the topmost view at this pixel wins: only check pixels no later view covers
            bool covered_later = false;
            bool after = false;
            for (const ViewOut& other : c->expected.views) {
                if (&other == &view) {
                    after = true;
                    continue;
                }
                covered_later = covered_later || (after && x >= other.rect[0] && x < other.rect[2] && y >= other.rect[1] &&
                                                  y < other.rect[3]);
            }
            if (!covered_later) {
                CHECK(PixelAt(done.pixels, layout.width, x, y) == Opaque(Pix(2, static_cast<u32>(X), static_cast<u32>(Y))));
            }
        }
    }
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/an_overlay_with_no_height_has_rectangles_outside_the_target_and_is_clipped") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    int variant = 0;
    std::size_t outside = 0;
    for (const char* name : {"overlay_off", "overlay_off_small", "small_overlay_1x1_400", "small_overlay_200x100_573",
                             "small_overlay_399x239_400", "small_stacked_1x1_573", "small_single_1x1_400", "small_side_1x1_573",
                             "small_stacked_399x239_400", "small_side_399x239_573"}) {
        const Case* c = FindCase(name);
        REQUIRE_MESSAGE(c != nullptr, name);
        const LayoutOut& l = c->expected;
        outside += l.top[2] > l.width || l.top[3] > l.height || l.bottom[2] > l.width || l.bottom[3] > l.height;
        RunOracleCase(*env, *c, variant++);
    }
    CHECK_MESSAGE(outside >= 1, "the table has an overlay-off layout whose rectangles lie outside the window");
    {
        const LayoutOut& off = FindCase("overlay_off_small")->expected;
        CHECK(off.top[2] > off.width);  // the top screen's rectangle is wider than the layout
        CHECK(off.bottom[1] >= off.height);  // and the bottom screen's is entirely below it
    }
    // Rectangles far outside (a 4 billion wide one) are clipped too, and draw nothing out of bounds.
    sl::Layout wild = sl::Compute(InputsOf(*FindCase("modes_stacked_800x480_400")));
    wild.top = sl::Rect{100, 50, 4000000000u, 4000000000u};
    wild.bottom = sl::Rect{0, 0, 0, 0};  // empty
    wild.bottom_shown = true;
    const Variant v = VariantOf(1, 400);
    pr::DisplayFrame frame = FrameOf(*env->backend, v);
    const Composed done = ComposeToImage(*env, wild.width, wild.height, frame, wild);
    CHECK(PixelAt(done.pixels, wild.width, 99, 60) == std::array<u8, 4>{0, 0, 0, 255});
    CHECK(PixelAt(done.pixels, wild.width, 100, 49) == std::array<u8, 4>{0, 0, 0, 255});
    // the screen is stretched over a rectangle of 4e9 pixels: the pixel at (100, 50) is its first pixel's neighbourhood
    CHECK(PixelAt(done.pixels, wild.width, 100, 50) == Opaque(Pix(1, 0, 0)));
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/a_missing_screen_leaves_black") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const Variant v = VariantOf(2, 400);
    for (int which = 0; which < 4; ++which) {
        const bool top = (which & 1) == 0, bottom = (which & 2) == 0;
        INFO("top " << top << " bottom " << bottom);
        pr::DisplayFrame frame = FrameOf(*env->backend, v, top, bottom);
        const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout);
        const Ref ref = Reference(GeoOf(c->expected), top ? &v.top : nullptr, bottom ? &v.bottom : nullptr, layout.width, layout.height);
        const Compared cmp = Compare(done.pixels, ref);
        CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " of " << cmp.compared << " pixels differ; first " << cmp.first);
        // the missing screen's rectangle is black, opaque
        const std::array<u32, 4>& gone = top ? c->expected.bottom : c->expected.top;
        if (!top || !bottom) {
            for (const auto& [x, y] : {std::pair<u32, u32>{gone[0], gone[1]}, {gone[2] - 1, gone[3] - 1}, {(gone[0] + gone[2]) / 2, (gone[1] + gone[3]) / 2}}) {
                if (!top && !bottom) {
                    CHECK(PixelAt(done.pixels, layout.width, x, y) == std::array<u8, 4>{0, 0, 0, 255});
                } else if ((!top) != (!bottom)) {
                    if (!top) {
                        CHECK(PixelAt(done.pixels, layout.width, c->expected.top[0], c->expected.top[1]) == std::array<u8, 4>{0, 0, 0, 255});
                    } else {
                        CHECK(PixelAt(done.pixels, layout.width, c->expected.bottom[0], c->expected.bottom[1]) == std::array<u8, 4>{0, 0, 0, 255});
                    }
                }
            }
        }
    }
    // Single mode shows the top screen only: the bottom screen is not drawn even when the frame has it.
    const Case* single = FindCase("modes_single_800x480_400");
    REQUIRE(single != nullptr);
    RunOracleCase(*env, *single, 0);
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/a_screen_that_is_not_valid_is_drawn_black") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const Variant v = VariantOf(1, 400);
    pr::DisplayFrame frame = FrameOf(*env->backend, v);
    // top: CPU pixels whose size disagrees with the displayed size; bottom: a region outside its image
    frame.top->pixels = std::make_shared<std::vector<u8>>(16);
    REQUIRE_FALSE(frame.top->Valid());
    frame.bottom->region.x += 100;  // the region now reaches past the image (and the sizes still agree)
    const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout);
    const Ref ref = Reference(GeoOf(c->expected), nullptr, nullptr, layout.width, layout.height);
    const Compared cmp = Compare(done.pixels, ref);
    CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " pixels differ; first " << cmp.first);
    CHECK(env->breaches.Total() == 0);
}

// ---- filtering ----------------------------------------------------------------------------------------------

TEST_CASE("compose/linear_filtering_blends_neighbouring_texels_and_nearest_does_not") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    // A 400x240 CPU screen of vertical stripes (even columns black, odd columns white), drawn twice as large.
    pr::DisplayFrame frame;
    pr::ScreenImage top;
    top.width = 400;
    top.height = 240;
    auto pixels = std::make_shared<std::vector<u8>>(400 * 240 * 4);
    for (u32 y = 0; y < 240; ++y) {
        for (u32 x = 0; x < 400; ++x) {
            const u8 v = (x & 1) ? 255 : 0;
            u8* at = &(*pixels)[(static_cast<std::size_t>(y) * 400 + x) * 4];
            at[0] = at[1] = at[2] = v;
            at[3] = 255;
        }
    }
    top.pixels = pixels;
    frame.top = top;
    sl::Layout layout;
    layout.width = 800;
    layout.height = 480;
    layout.top_shown = true;
    layout.top = sl::Rect{0, 0, 800, 480};
    for (const bool linear : {false, true}) {
        INFO("linear " << linear);
        pr::ComposeOptions options;
        options.linear_filter = linear;
        const Composed done = ComposeToImage(*env, 800, 480, frame, layout, options);
        // Destination x maps to the source position (x + 0.5) / 2 (in texels, centres at k + 0.5).
        for (u32 x = 2; x < 30; ++x) {
            const double pos = (x + 0.5) / 2.0;          // texel units
            const double f = pos - 0.5;                    // between the centres of texel floor(f) and the next
            const u32 k = static_cast<u32>(std::floor(f));
            const double t = f - k;
            const double lo = (k & 1) ? 255 : 0, hi = ((k + 1) & 1) ? 255 : 0;
            const double want = linear ? lo * (1 - t) + hi * t : ((static_cast<u32>(std::floor(pos)) & 1) ? 255 : 0);
            const u8 got = PixelAt(done.pixels, 800, x, 100)[0];
            CHECK_MESSAGE(std::abs(got - want) <= 2.5, "x " << x << " got " << int(got) << " want " << want);
            CHECK(PixelAt(done.pixels, 800, x, 100)[0] == PixelAt(done.pixels, 800, x, 100)[1]);
        }
    }
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/linear_filtering_never_reads_outside_a_regions_edge") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    // The backend image has magenta all round its region: a linear sample at the region's edge must not bleed it.
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    for (int variant = 0; variant < 4; ++variant) {
        const Variant v = VariantOf(variant, 400);
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        pr::ComposeOptions options;
        options.linear_filter = true;
        const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout, options);
        for (u32 y = 0; y < layout.height; ++y) {
            for (u32 x = 0; x < layout.width; ++x) {
                const auto p = PixelAt(done.pixels, layout.width, x, y);
                // magenta (255,0,255) mixed with a pattern colour is still recognisable by the garbage never being
                // exactly reached unless the whole sample was garbage; a pure magenta pixel is a failure
                if (p[0] == 255 && p[1] == 0 && p[2] == 255) {
                    FAIL("magenta at (" << x << "," << y << ") variant " << variant);
                }
            }
        }
    }
    CHECK(env->breaches.Total() == 0);
}

// ---- the UI layer ---------------------------------------------------------------------------------------------

namespace {

// A UiLayer that records through the Recorder interface: a pipeline of its own, a scissor, one draw.
class DrawingLayer : public pr::UiLayer {
public:
    DrawingLayer(rhi::Backend& backend, rhi::Format target_format, rhi::Rect area) : area_(area) {
        std::vector<u32> vert, frag;
        std::string error;
        REQUIRE(pr::CompileGlsl(pr::GlslStage::Vertex, R"(#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})", vert, error));
        REQUIRE(pr::CompileGlsl(pr::GlslStage::Fragment, R"(#version 450
layout(location = 0) out vec4 o;
void main() { o = vec4(0.0, 0.0, 1.0, 1.0); })", frag, error));
        vertex_ = vert;
        fragment_ = frag;
        rhi::GraphicsPipelineDesc desc;
        desc.vertex.spirv = vertex_;
        desc.fragment.spirv = fragment_;
        desc.color_format = target_format;
        desc.name = "test ui";
        rhi::Result<std::shared_ptr<rhi::Pipeline>> made = backend.CreateGraphicsPipeline(desc);
        REQUIRE_MESSAGE(made.ok(), made.error());
        pipeline_ = *made;
    }
    void Record(void* recorder, u32 width, u32 height) override {
        ++calls;
        seen_recorder = recorder;
        seen_width = width;
        seen_height = height;
        rhi::Recorder& rec = *static_cast<rhi::Recorder*>(recorder);
        rec.SetPipeline(*pipeline_);
        rhi::Viewport viewport;
        viewport.width = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        rec.SetViewport(viewport);
        rec.SetScissor(area_);
        rec.Draw(3);
    }
    int calls = 0;
    void* seen_recorder = nullptr;
    u32 seen_width = 0, seen_height = 0;

private:
    rhi::Rect area_;
    std::vector<u32> vertex_, fragment_;
    std::shared_ptr<rhi::Pipeline> pipeline_;
};

// A UiLayer that records raw Vulkan commands into the recorder's open pass, as Dear ImGui's Vulkan backend does.
class RawLayer : public pr::UiLayer {
public:
    explicit RawLayer(rhi::Rect area) : area_(area) {}
    void Record(void* recorder, u32 width, u32 height) override {
        ++calls;
        seen_width = width;
        seen_height = height;
        VkCommandBuffer commands = rhi::CommandBuffer(*static_cast<rhi::Recorder*>(recorder));
        VkClearAttachment attachment = {};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color = {{0.0f, 1.0f, 0.0f, 1.0f}};
        VkClearRect rect = {};
        rect.rect = {{area_.x, area_.y}, {area_.width, area_.height}};
        rect.layerCount = 1;
        vkCmdClearAttachments(commands, 1, &attachment, 1, &rect);
    }
    int calls = 0;
    u32 seen_width = 0, seen_height = 0;

private:
    rhi::Rect area_;
};

void Paint(Ref& ref, const rhi::Rect& area, Rgb colour) {
    for (u32 y = area.y; y < area.y + area.height && y < ref.height; ++y) {
        for (u32 x = area.x; x < area.x + area.width && x < ref.width; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * ref.width + x;
            ref.expected[at * 4] = colour.r;
            ref.expected[at * 4 + 1] = colour.g;
            ref.expected[at * 4 + 2] = colour.b;
            ref.checked[at] = 1;
            ref.alternatives.erase(at);
        }
    }
}

}  // namespace

TEST_CASE("compose/a_ui_layers_draw_lands_over_the_screens") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const Variant v = VariantOf(0, 400);
    for (const bool raw : {false, true}) {
        INFO("raw Vulkan commands " << raw);
        const rhi::Rect area{150, 100, 300, 200};  // across both screens
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        DrawingLayer drawing(*env->backend, rhi::Format::Rgba8Unorm, area);
        RawLayer raw_layer(area);
        pr::UiLayer* layer = raw ? static_cast<pr::UiLayer*>(&raw_layer) : &drawing;
        pr::ComposeOptions options;
        options.ui = layer;
        const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout, options);
        Ref ref = Reference(GeoOf(c->expected), &v.top, &v.bottom, layout.width, layout.height);
        Paint(ref, area, raw ? Rgb{0, 255, 0} : Rgb{0, 0, 255});
        const Compared cmp = Compare(done.pixels, ref);
        CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " of " << cmp.compared << " pixels differ; first " << cmp.first);
        CHECK(PixelAt(done.pixels, layout.width, 200, 150) == (raw ? std::array<u8, 4>{0, 255, 0, 255} : std::array<u8, 4>{0, 0, 255, 255}));
        if (raw) {
            CHECK(raw_layer.calls == 1);
            CHECK(raw_layer.seen_width == layout.width);
            CHECK(raw_layer.seen_height == layout.height);
        } else {
            CHECK(drawing.calls == 1);
            CHECK(drawing.seen_width == layout.width);
            CHECK(drawing.seen_height == layout.height);
            CHECK(drawing.seen_recorder != nullptr);
        }
    }
    CHECK(env->breaches.Total() == 0);
}

namespace {

// A UiLayer that sets neither a viewport nor a scissor: it draws a quad in the target's top-left corner (the first
// tenth in each direction) with the state the compositor leaves.
class BareLayer : public pr::UiLayer {
public:
    explicit BareLayer(rhi::Backend& backend) {
        std::string error;
        REQUIRE(pr::CompileGlsl(pr::GlslStage::Vertex, R"(#version 450
void main() {
    vec2 p = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(-1.0 + p * 0.2, 0.0, 1.0);
})", vertex_, error));
        REQUIRE(pr::CompileGlsl(pr::GlslStage::Fragment, R"(#version 450
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0, 1.0, 0.0, 1.0); })", fragment_, error));
        rhi::GraphicsPipelineDesc desc;
        desc.vertex.spirv = vertex_;
        desc.fragment.spirv = fragment_;
        desc.topology = rhi::Topology::TriangleStrip;
        desc.color_format = rhi::Format::Rgba8Unorm;
        desc.name = "test bare ui";
        rhi::Result<std::shared_ptr<rhi::Pipeline>> made = backend.CreateGraphicsPipeline(desc);
        REQUIRE_MESSAGE(made.ok(), made.error());
        pipeline_ = *made;
    }
    void Record(void* recorder, u32, u32) override {
        rhi::Recorder& rec = *static_cast<rhi::Recorder*>(recorder);
        rec.SetPipeline(*pipeline_);
        rec.Draw(4);
    }

private:
    std::vector<u32> vertex_, fragment_;
    std::shared_ptr<rhi::Pipeline> pipeline_;
};

}  // namespace

TEST_CASE("compose/the_ui_layer_gets_the_whole_target_as_viewport_and_scissor_not_the_last_screens") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    // the last screen drawn is the bottom one, at {240,240,560,480}; the layer draws in the opposite corner
    REQUIRE(layout.bottom.left > 100);
    BareLayer layer(*env->backend);
    pr::ComposeOptions options;
    options.ui = &layer;
    const Variant v = VariantOf(1, 400);
    pr::DisplayFrame frame = FrameOf(*env->backend, v);
    const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout, options);
    const std::array<u8, 4> yellow = {255, 255, 0, 255};
    CHECK(PixelAt(done.pixels, layout.width, 2, 2) == yellow);
    CHECK(PixelAt(done.pixels, layout.width, 79, 47) == yellow);
    CHECK(PixelAt(done.pixels, layout.width, 81, 47) != yellow);
    CHECK(PixelAt(done.pixels, layout.width, 79, 49) != yellow);
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/a_target_that_cannot_be_a_colour_attachment_draws_nothing") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    if (env->backend->QueryFormat(rhi::Format::Bc1Unorm).attachment) {
        mh4n::test::SkipCase("this device can render to BC1");
        return;
    }
    std::shared_ptr<rhi::Image> target = MakeImage(*env->backend, rhi::Format::Bc1Unorm, 64, 64, "block-compressed target");
    // With the bc7 bit masked off the backend stores the "BC1" image as Rgba8Unorm (ChooseFormat), which is a colour
    // target: the case then has nothing to assert.
    if (target->GetFormat() != rhi::Format::Bc1Unorm || env->backend->QueryFormat(target->GetFormat()).attachment) {
        mh4n::test::SkipCase("the backend stores the BC1 image as a format that can be a colour attachment (bc7 masked off)");
        return;
    }
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const Variant v = VariantOf(1, 400);
    pr::DisplayFrame frame = FrameOf(*env->backend, v);
    rhi::Recorder rec = env->backend->BeginRecording();
    pr::ComposeResult result = env->compositor->Compose(rec, *target, frame, layout, {});
    CHECK_FALSE(result.drawn);
    CHECK(result.waits.empty());
    env->backend->WaitIdle();  // the recorder is dropped unsubmitted
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/the_ui_layer_is_called_with_no_screens_and_its_raw_commands_have_a_pass") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const rhi::Rect area{10, 20, 50, 40};
    RawLayer layer(area);
    pr::ComposeOptions options;
    options.ui = &layer;
    pr::DisplayFrame frame;  // both screens off
    const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout, options);
    CHECK(layer.calls == 1);
    Ref ref = NewRef(layout.width, layout.height);
    Paint(ref, area, Rgb{0, 255, 0});
    const Compared cmp = Compare(done.pixels, ref);
    CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " pixels differ; first " << cmp.first);
    CHECK(env->breaches.Total() == 0);
}

// ---- targets and frames --------------------------------------------------------------------------------------

TEST_CASE("compose/a_bgra_target_gets_the_same_picture_in_its_own_byte_order") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const Variant v = VariantOf(0, 400);
    pr::DisplayFrame frame = FrameOf(*env->backend, v);
    const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout, {}, rhi::Format::Bgra8Unorm);
    Ref ref = Reference(GeoOf(c->expected), &v.top, &v.bottom, layout.width, layout.height);
    for (std::size_t i = 0; i < ref.expected.size(); i += 4) {
        std::swap(ref.expected[i], ref.expected[i + 2]);  // the target stores B, G, R, A
    }
    for (auto& entry : ref.alternatives) {
        for (Rgb& alt : entry.second) {
            std::swap(alt.r, alt.b);
        }
    }
    const Compared cmp = Compare(done.pixels, ref);
    CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " of " << cmp.compared << " pixels differ; first " << cmp.first);
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/the_waits_are_one_fragment_stage_wait_on_the_highest_value") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const auto compose = [&](pr::DisplayFrame frame) {
        std::shared_ptr<rhi::Image> target = MakeImage(*env->backend, rhi::Format::Rgba8Unorm, layout.width, layout.height, "target");
        rhi::Recorder rec = env->backend->BeginRecording();
        pr::ComposeResult result = env->compositor->Compose(rec, *target, frame, layout, {});
        const std::vector<rhi::Wait> waits = result.waits;
        const rhi::TimelineValue value = env->compositor->Submit(std::move(rec), std::move(result));
        env->backend->WaitIdle(value);
        return waits;
    };
    {   // two backend images (two submissions): one wait, the later value
        const Variant v = VariantOf(2, 400);
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        REQUIRE(frame.top->timeline_value != 0);
        REQUIRE(frame.bottom->timeline_value > frame.top->timeline_value);
        const std::vector<rhi::Wait> waits = compose(frame);
        REQUIRE(waits.size() == 1);
        CHECK(waits[0].stage == rhi::Stage::FragmentShader);
        CHECK(waits[0].value == frame.bottom->timeline_value);
    }
    {   // CPU pixels only: nothing to wait for
        Variant v = VariantOf(1, 400);
        v.bottom.cpu = true;
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        CHECK(compose(frame).empty());
    }
    {   // a backend image whose timeline value is 0: nothing to wait for
        const Variant v = VariantOf(2, 400);
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        env->backend->WaitIdle();
        frame.top->timeline_value = 0;
        frame.bottom->timeline_value = 0;
        CHECK(compose(frame).empty());
    }
    CHECK(env->breaches.Total() == 0);
}

TEST_CASE("compose/frames_one_after_another_reuse_the_upload_textures") {
    std::unique_ptr<Env> env = MakeEnv();
    if (!env) return;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    for (u32 i = 0; i < 6; ++i) {
        Variant v = VariantOf(1, 400);
        v.top.id = 10 + i;  // a different picture each frame
        v.bottom = VariantOf(0, 400).bottom;
        v.bottom.id = 20 + i;
        pr::DisplayFrame frame = FrameOf(*env->backend, v);
        const Composed done = ComposeToImage(*env, layout.width, layout.height, frame, layout);
        const Ref ref = Reference(GeoOf(c->expected), &v.top, &v.bottom, layout.width, layout.height);
        const Compared cmp = Compare(done.pixels, ref);
        CHECK_MESSAGE(cmp.mismatches == 0, "frame " << i << ": " << cmp.mismatches << " pixels differ; first " << cmp.first);
    }
    // each frame was waited for, so two textures (the top's and the bottom's) are enough for all of them
    CHECK(env->compositor->TexturesCreated() == 2);
    CHECK(env->compositor->PipelineCount() >= 1);
    CHECK(env->breaches.Total() == 0);
}

// ---- backend images from another thread ---------------------------------------------------------------------------

TEST_CASE("compose/an_image_produced_by_another_thread_is_handed_over_and_waited_for_at_the_fragment_stage") {
    std::shared_ptr<rhi::Backend> backend = mh4n::test::RequireBackend();
    if (!backend) return;
    BreachCapture breaches;
    const Case* c = FindCase("modes_stacked_800x480_573");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const Variant v = VariantOf(3, 573);  // top: scale 3 upright; bottom: scale 1 rotated: both backend images

    struct Outcome {
        std::vector<u8> pixels;
        std::vector<rhi::Wait> waits;
        std::string error;
    };
    std::promise<std::thread::id> composer_id;
    std::promise<pr::DisplayFrame> frame_promise;
    std::promise<Outcome> outcome_promise;
    std::thread composer([&] {
        composer_id.set_value(std::this_thread::get_id());
        pr::DisplayFrame frame = frame_promise.get_future().get();
        Outcome out;
        rhi::Result<std::unique_ptr<pr::Compositor>> compositor = pr::Compositor::Create(backend);
        if (!compositor.ok()) {
            out.error = compositor.error();
            outcome_promise.set_value(std::move(out));
            return;
        }
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = layout.width;
        desc.height = layout.height;
        desc.name = "target";
        std::shared_ptr<rhi::Image> target = *backend->CreateImage(desc);
        rhi::Recorder rec = backend->BeginRecording();
        pr::ComposeResult result = (*compositor)->Compose(rec, *target, frame, layout, {});
        out.waits = result.waits;
        const rhi::TimelineValue value = (*compositor)->Submit(std::move(rec), std::move(result));
        backend->WaitIdle(value);
        rhi::Result<std::vector<u8>> read = backend->ReadImage(*target, 0, 0, layout.width, layout.height);
        LayerSees(*backend);
        if (read.ok()) {
            out.pixels = *read;
        } else {
            out.error = read.error();
        }
        compositor->reset();
        outcome_promise.set_value(std::move(out));
    });
    const std::thread::id composer_thread = composer_id.get_future().get();

    // This thread is the producer: it draws the screens (here: uploads them) and submits, which gives the value the
    // screens carry, then hands the images over to the composer with that value at the stage that reads them.
    pr::DisplayFrame frame = FrameOf(*backend, v);
    REQUIRE(frame.top->timeline_value != 0);
    REQUIRE(frame.bottom->timeline_value != 0);
    for (pr::ScreenImage* screen : {&*frame.top, &*frame.bottom}) {
        rhi::HandOver(*const_cast<rhi::Image*>(static_cast<const rhi::Image*>(screen->image.get())), composer_thread,
                      rhi::Wait{screen->timeline_value, rhi::Stage::FragmentShader});
    }
    const rhi::TimelineValue expected_wait = std::max(frame.top->timeline_value, frame.bottom->timeline_value);
    frame_promise.set_value(frame);
    Outcome outcome = outcome_promise.get_future().get();
    composer.join();
    REQUIRE_MESSAGE(outcome.error.empty(), outcome.error);

    REQUIRE(outcome.waits.size() == 1);
    CHECK(outcome.waits[0].stage == rhi::Stage::FragmentShader);
    CHECK(outcome.waits[0].value == expected_wait);
    const Ref ref = Reference(GeoOf(c->expected), &v.top, &v.bottom, layout.width, layout.height);
    const Compared cmp = Compare(outcome.pixels, ref);
    CHECK_MESSAGE(cmp.mismatches == 0, cmp.mismatches << " of " << cmp.compared << " pixels differ; first " << cmp.first);
    // the backend's checks (RequiredWait, ForeignThread, HandOver, ...) did not fire
    CHECK(breaches.Total() == 0);
    frame = pr::DisplayFrame{};
    backend->WaitIdle();
}

TEST_CASE("compose/a_submission_that_drops_the_waits_of_a_handed_over_image_is_a_breach_the_checks_see") {
    // The control for the case above: the backend's owner-rule check is live. Not run under the validation layer, which
    // may report the hazard the missing wait leaves (the point of the control is the backend's own check).
    if (mh4n::test::ValidationEnabled()) {
        mh4n::test::SkipCase("run without the validation layer: the missing wait is a real hazard it may report");
        return;
    }
    std::shared_ptr<rhi::Backend> backend = mh4n::test::RequireBackend();
    if (!backend) return;
    BreachCapture breaches;
    const Case* c = FindCase("modes_stacked_800x480_400");
    REQUIRE(c != nullptr);
    const sl::Layout layout = sl::Compute(InputsOf(*c));
    const Variant v = VariantOf(2, 400);
    std::promise<std::thread::id> composer_id;
    std::promise<pr::DisplayFrame> frame_promise;
    std::promise<int> done_promise;
    std::thread composer([&] {
        composer_id.set_value(std::this_thread::get_id());
        pr::DisplayFrame frame = frame_promise.get_future().get();
        rhi::Result<std::unique_ptr<pr::Compositor>> compositor = pr::Compositor::Create(backend);
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = layout.width;
        desc.height = layout.height;
        desc.name = "target";
        std::shared_ptr<rhi::Image> target = *backend->CreateImage(desc);
        rhi::Recorder rec = backend->BeginRecording();
        pr::ComposeResult result = (*compositor)->Compose(rec, *target, frame, layout, {});
        backend->WaitIdle(backend->Submit(std::move(rec)));  // the waits of `result` are NOT passed
        compositor->reset();
        done_promise.set_value(1);
    });
    const std::thread::id composer_thread = composer_id.get_future().get();
    pr::DisplayFrame frame = FrameOf(*backend, v);
    backend->WaitIdle();  // the producer's work has finished: no real hazard
    for (pr::ScreenImage* screen : {&*frame.top, &*frame.bottom}) {
        rhi::HandOver(*const_cast<rhi::Image*>(static_cast<const rhi::Image*>(screen->image.get())), composer_thread,
                      rhi::Wait{screen->timeline_value, rhi::Stage::FragmentShader});
    }
    frame_promise.set_value(frame);
    done_promise.get_future().get();
    composer.join();
    CHECK(breaches.Count(rhi::OwnerRule::RequiredWait) >= 1);
    frame = pr::DisplayFrame{};
    backend->WaitIdle();
}

#endif
