// swapchain_test.cpp - N2b-06, the swapchain and the present thread (native/src/swapchain/, docs/native/presentation.md).
//
// Two kinds of case. The first needs no device: the present mode, surface format, extent and image count from what a
// surface offers, the order in which the displayed time's source is chosen, the one-slot mailbox and the present log's
// text, all on synthetic data. The second drives a swapchain::Presenter over a VK_EXT_headless_surface swapchain on
// whatever Vulkan device the machine has (lavapipe in the cloud and in Linux CI; the helper logs the device name): a
// case that needs one skips with a message without it, and fails under MH4N_REQUIRE_VULKAN=1. Headless surfaces never
// answer out-of-date or suboptimal on their own, so TestHooks injects those results.
//
// Where the expectations come from, never from the presenter: the frames are CPU-pixel DisplayFrames of solid colours
// made here; the rectangles are screen_layout::Compute's (N2b-04, tested against Azahar's own output); the present
// modes the surface offers are asked of the driver by the test itself; the rows of the log are checked against the
// sequences the test published and the order the composition hook saw them in.
//
// Every device case installs a counting owner-rule handler (MH4N_RHI_CHECKS is on in every build) and requires zero
// breaches, except the one case that forces a recording onto another thread to see the presenter's own check. Run under
// ThreadSanitizer by the native-tsan job, and under the validation layer with
//     python3 tools/native/vk_layer_run.py --build build/native -R '^swapchain_test$'
#include <doctest/doctest.h>

#if !__has_include("platform.h")
TEST_CASE("swapchain_test needs the port's platform.h (the repository around native/)" * doctest::skip()) {}
#else

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mh4n/registry.h"
#include "swapchain/display_time.h"
#include "swapchain/policy.h"
#include "swapchain/present_log.h"
#include "swapchain/presenter.h"
#include "support/vulkan_device.h"

namespace {

namespace rhi = ::mh4n::gpu::rhi;
namespace test = ::mh4n::test;
namespace sc = ::mh4n::swapchain;
namespace sl = ::mh4n::screen_layout;
namespace pr = ::mh4n::present;
using mh4n::s32;
using mh4n::s64;
using mh4n::u32;
using mh4n::u64;
using mh4n::u8;
using Clock = std::chrono::steady_clock;

// ---- pure: the swapchain's parameters ----------------------------------------------------------------------------------

constexpr VkPresentModeKHR kFifo = VK_PRESENT_MODE_FIFO_KHR;
constexpr VkPresentModeKHR kMailbox = VK_PRESENT_MODE_MAILBOX_KHR;
constexpr VkPresentModeKHR kImmediate = VK_PRESENT_MODE_IMMEDIATE_KHR;
constexpr VkPresentModeKHR kRelaxed = VK_PRESENT_MODE_FIFO_RELAXED_KHR;

TEST_CASE("policy/present_mode_follows_vsync_and_what_the_surface_offers") {
    const std::vector<VkPresentModeKHR> all = {kFifo, kRelaxed, kMailbox, kImmediate};
    const std::vector<VkPresentModeKHR> no_mailbox = {kImmediate, kFifo};  // what Apple's surfaces offer (rhi-spike.md X9)
    const std::vector<VkPresentModeKHR> only_fifo = {kFifo};
    const std::vector<VkPresentModeKHR> mailbox_only_besides = {kMailbox, kFifo};
    const std::vector<VkPresentModeKHR> none;

    // vsync on: FIFO whatever else is offered
    CHECK(sc::ChoosePresentMode(all, true) == kFifo);
    CHECK(sc::ChoosePresentMode(no_mailbox, true) == kFifo);
    CHECK(sc::ChoosePresentMode(only_fifo, true) == kFifo);
    CHECK(sc::ChoosePresentMode(none, true) == kFifo);
    // vsync off: MAILBOX if offered, else IMMEDIATE, else FIFO
    CHECK(sc::ChoosePresentMode(all, false) == kMailbox);
    CHECK(sc::ChoosePresentMode(mailbox_only_besides, false) == kMailbox);
    CHECK(sc::ChoosePresentMode(no_mailbox, false) == kImmediate);
    CHECK(sc::ChoosePresentMode(only_fifo, false) == kFifo);
    CHECK(sc::ChoosePresentMode(none, false) == kFifo);
    // the order of the list does not matter
    CHECK(sc::ChoosePresentMode(std::vector<VkPresentModeKHR>{kImmediate, kMailbox, kFifo}, false) == kMailbox);
    CHECK(std::string(sc::PresentModeName(kMailbox)) == "MAILBOX");
    CHECK(std::string(sc::PresentModeName(kImmediate)) == "IMMEDIATE");
    CHECK(std::string(sc::PresentModeName(kFifo)) == "FIFO");
}

TEST_CASE("policy/surface_format_prefers_the_compositor_formats_in_srgb_nonlinear") {
    const auto format = [](VkFormat f, VkColorSpaceKHR c = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        return VkSurfaceFormatKHR{f, c};
    };
    // the first acceptable entry of the list, as Azahar picks
    auto chosen = sc::ChooseSurfaceFormat(std::vector{format(VK_FORMAT_R16G16B16A16_SFLOAT), format(VK_FORMAT_B8G8R8A8_UNORM),
                                                      format(VK_FORMAT_R8G8B8A8_UNORM)});
    REQUIRE(chosen.has_value());
    CHECK(chosen->format == VK_FORMAT_B8G8R8A8_UNORM);
    chosen = sc::ChooseSurfaceFormat(std::vector{format(VK_FORMAT_R8G8B8A8_UNORM), format(VK_FORMAT_B8G8R8A8_UNORM)});
    REQUIRE(chosen.has_value());
    CHECK(chosen->format == VK_FORMAT_R8G8B8A8_UNORM);
    // the sRGB-encoded formats are not the compositor's: the game's pixels are written as they are
    chosen = sc::ChooseSurfaceFormat(std::vector{format(VK_FORMAT_B8G8R8A8_SRGB), format(VK_FORMAT_R8G8B8A8_UNORM)});
    REQUIRE(chosen.has_value());
    CHECK(chosen->format == VK_FORMAT_R8G8B8A8_UNORM);
    // the colour space wins over the list order; failing it, the format alone
    chosen = sc::ChooseSurfaceFormat(std::vector{format(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_HDR10_ST2084_EXT),
                                                 format(VK_FORMAT_R8G8B8A8_UNORM)});
    REQUIRE(chosen.has_value());
    CHECK(chosen->format == VK_FORMAT_R8G8B8A8_UNORM);
    chosen = sc::ChooseSurfaceFormat(std::vector{format(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_HDR10_ST2084_EXT)});
    REQUIRE(chosen.has_value());
    CHECK(chosen->format == VK_FORMAT_B8G8R8A8_UNORM);
    // neither format: nothing; a single UNDEFINED entry means any: RGBA
    CHECK_FALSE(sc::ChooseSurfaceFormat(std::vector{format(VK_FORMAT_R16G16B16A16_SFLOAT)}).has_value());
    CHECK_FALSE(sc::ChooseSurfaceFormat(std::vector<VkSurfaceFormatKHR>{}).has_value());
    chosen = sc::ChooseSurfaceFormat(std::vector{format(VK_FORMAT_UNDEFINED)});
    REQUIRE(chosen.has_value());
    CHECK(chosen->format == VK_FORMAT_R8G8B8A8_UNORM);
}

TEST_CASE("policy/extent_image_count_transform_and_alpha") {
    VkSurfaceCapabilitiesKHR caps = {};
    caps.minImageCount = 2;
    caps.maxImageCount = 0;  // no maximum
    caps.minImageExtent = {16, 8};
    caps.maxImageExtent = {4096, 2048};
    caps.currentExtent = {0xFFFFFFFFu, 0xFFFFFFFFu};  // the special value of a headless surface: the swapchain chooses
    caps.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    // the special value: the requested size, clamped to the surface's limits
    VkExtent2D extent = sc::ChooseExtent(caps, 800, 480);
    CHECK((extent.width == 800 && extent.height == 480));
    extent = sc::ChooseExtent(caps, 4, 4);
    CHECK((extent.width == 16 && extent.height == 8));
    extent = sc::ChooseExtent(caps, 10000, 10000);
    CHECK((extent.width == 4096 && extent.height == 2048));
    // a request of zero stays zero (minimised: nothing to present), whatever the smallest size
    extent = sc::ChooseExtent(caps, 0, 480);
    CHECK(extent.width == 0);
    extent = sc::ChooseExtent(caps, 640, 0);
    CHECK(extent.height == 0);
    // a surface that dictates its size ignores the request
    caps.currentExtent = {1280, 720};
    extent = sc::ChooseExtent(caps, 800, 480);
    CHECK((extent.width == 1280 && extent.height == 720));
    caps.currentExtent = {0, 0};  // a minimised window
    extent = sc::ChooseExtent(caps, 800, 480);
    CHECK((extent.width == 0 && extent.height == 0));

    CHECK(sc::ChooseImageCount(caps) == 3);
    caps.maxImageCount = 2;
    CHECK(sc::ChooseImageCount(caps) == 2);
    caps.maxImageCount = 8;
    CHECK(sc::ChooseImageCount(caps) == 3);

    CHECK(sc::ChooseTransform(caps) == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    caps.supportedTransforms = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    caps.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    CHECK(sc::ChooseTransform(caps) == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR);

    CHECK(sc::ChooseCompositeAlpha(caps) == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    CHECK(sc::ChooseCompositeAlpha(caps) == VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR);
    caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    CHECK(sc::ChooseCompositeAlpha(caps) == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
}

// ---- pure: where displayed_us comes from ---------------------------------------------------------------------------------

TEST_CASE("policy/display_time_source_order") {
    using Kind = sc::DisplayTimeKind;
    // present_wait only when it measures the display, then display_timing, then a registered source, then none (D57)
    CHECK(sc::ChooseDisplayTime(true, true, "platform").kind == Kind::PresentWait);
    CHECK(sc::ChooseDisplayTime(true, false, "platform").kind == Kind::PresentWait);
    CHECK(sc::ChooseDisplayTime(true, false, "").kind == Kind::PresentWait);
    CHECK(sc::ChooseDisplayTime(false, true, "platform").kind == Kind::DisplayTiming);
    CHECK(sc::ChooseDisplayTime(false, true, "").kind == Kind::DisplayTiming);
    const sc::DisplayTimeChoice registered = sc::ChooseDisplayTime(false, false, "platform");
    CHECK(registered.kind == Kind::Registered);
    CHECK(registered.registered == "platform");
    CHECK(sc::ChooseDisplayTime(false, false, "").kind == Kind::None);

    CHECK(sc::DisplayTimeLabel(sc::ChooseDisplayTime(true, true, "x")) == "present_wait");
    CHECK(sc::DisplayTimeLabel(sc::ChooseDisplayTime(false, true, "x")) == "display_timing");
    CHECK(sc::DisplayTimeLabel(sc::ChooseDisplayTime(false, false, "x")) == "x");
    CHECK(sc::DisplayTimeLabel(sc::ChooseDisplayTime(false, false, "x"), "dwm") == "dwm");
    CHECK(sc::DisplayTimeLabel(sc::ChooseDisplayTime(false, false, "")) == "none");
}

// A source registered by name, as N2b-23 registers its own. The id it was asked about and the host time of the present
// are what the presenter gives it; the answer is the host time plus a fixed delay, so the log can be checked.
constexpr u64 kTestSourceDelayUs = 1234;
std::mutex g_source_mutex;
std::vector<std::pair<u64, u64>> g_source_calls;  // (present_id, present_host_us)
std::set<std::thread::id> g_source_threads;

class TestTimeSource final : public sc::DisplayTimeSource {
public:
    std::string_view Name() const override {
        return "swapchain_test_clock";
    }
    std::optional<u64> DisplayedUs(u64 present_id, u64 present_host_us) override {
        std::lock_guard lock(g_source_mutex);
        g_source_calls.emplace_back(present_id, present_host_us);
        g_source_threads.insert(std::this_thread::get_id());
        return present_host_us + kTestSourceDelayUs;
    }
};
MH4N_REGISTER(sc::DisplayTimeSourceFactory, "swapchain_test_clock",
              {[] { return std::unique_ptr<sc::DisplayTimeSource>(new TestTimeSource); }});

TEST_CASE("policy/a_display_time_source_is_found_by_its_registration_name") {
    const sc::DisplayTimeSourceFactory* factory =
        mh4n::Registry<sc::DisplayTimeSourceFactory>::Instance().Find("swapchain_test_clock");
    REQUIRE(factory != nullptr);
    std::unique_ptr<sc::DisplayTimeSource> source = factory->make();
    REQUIRE(source != nullptr);
    CHECK(source->Name() == "swapchain_test_clock");
    CHECK(*source->DisplayedUs(7, 1000) == 1000 + kTestSourceDelayUs);
    CHECK(mh4n::Registry<sc::DisplayTimeSourceFactory>::Instance().Find("no such source") == nullptr);
    {
        std::lock_guard lock(g_source_mutex);
        g_source_calls.clear();
        g_source_threads.clear();
    }
}

// ---- pure: the mailbox and the log's text -----------------------------------------------------------------------------------

pr::DisplayFrame BareFrame(u64 sequence) {
    pr::DisplayFrame frame;
    frame.sequence = sequence;
    frame.game_frame = static_cast<u32>(sequence + 100);
    return frame;
}

TEST_CASE("mailbox/a_newer_frame_replaces_one_nobody_took") {
    sc::FrameMailbox mailbox;
    CHECK_FALSE(mailbox.Has());
    CHECK_FALSE(mailbox.Take().has_value());
    CHECK_FALSE(mailbox.Put(BareFrame(0), 10));  // nothing replaced
    CHECK(mailbox.Has());
    CHECK(mailbox.Put(BareFrame(1), 20));        // frame 0 is dropped
    CHECK(mailbox.Put(BareFrame(2), 30));        // so is frame 1
    auto taken = mailbox.Take();
    REQUIRE(taken.has_value());
    CHECK(taken->frame.sequence == 2);
    CHECK(taken->frame.game_frame == 102);
    CHECK(taken->published_us == 30);
    CHECK_FALSE(mailbox.Has());
    CHECK_FALSE(mailbox.Take().has_value());
    CHECK_FALSE(mailbox.Put(BareFrame(3), 40));  // taken: nothing to replace
}

TEST_CASE("present_log/header_and_rows") {
    CHECK(sc::FormatPresentHeader("2026-11-01T12:00:00Z", 239.76, "present_wait") ==
          "# present v1 2026-11-01T12:00:00Z refresh_hz=239.76 display_time=present_wait\n"
          "sequence,game_frame,published_us,composed_us,presented_us,displayed_us,dropped_before\n");
    CHECK(sc::FormatPresentHeader("d", std::nullopt, "none") ==
          "# present v1 d refresh_hz=unknown display_time=none\n"
          "sequence,game_frame,published_us,composed_us,presented_us,displayed_us,dropped_before\n");
    CHECK(sc::FormatPresentHeader("d", 0.0, "none").find("refresh_hz=unknown") != std::string::npos);

    sc::PresentRow row;
    row.sequence = 12;
    row.game_frame = 34;
    row.published_us = 1000;
    row.composed_us = 1100;
    row.presented_us = 1200;
    row.displayed_us = 1300;
    row.dropped_before = 2;
    CHECK(sc::FormatPresentRow(row) == "12,34,1000,1100,1200,1300,2\n");
    row.displayed_us.reset();  // not measurable: the field is empty
    CHECK(sc::FormatPresentRow(row) == "12,34,1000,1100,1200,,2\n");

    const std::string date = sc::UtcDate(std::chrono::system_clock::time_point{});  // the epoch
    CHECK(date == "1970-01-01T00:00:00Z");
    CHECK(sc::UtcDate(std::chrono::system_clock::time_point{std::chrono::seconds(1'000'000'000)}) == "2001-09-09T01:46:40Z");
}

// ---- the device cases: the rig -----------------------------------------------------------------------------------------

struct BreachCapture {
    std::mutex mutex;
    std::vector<rhi::OwnerRuleBreach> breaches;
    rhi::OwnerRuleHandler previous;
    BreachCapture() {
        previous = rhi::SetOwnerRuleHandler([this](const rhi::OwnerRuleBreach& breach) {
            std::lock_guard lock(mutex);
            breaches.push_back(breach);
        });
    }
    ~BreachCapture() {
        rhi::SetOwnerRuleHandler(previous);
    }
    std::size_t Total() {
        std::lock_guard lock(mutex);
        return breaches.size();
    }
    int Count(rhi::OwnerRule rule) {
        std::lock_guard lock(mutex);
        return static_cast<int>(std::count_if(breaches.begin(), breaches.end(), [&](const auto& b) { return b.rule == rule; }));
    }
};

struct Rig {
    BreachCapture breaches;
    std::shared_ptr<rhi::Instance> instance;
    std::unique_ptr<test::HeadlessSurface> surface;  // declared before the backend, destroyed after it
    std::shared_ptr<rhi::Backend> backend;
};

// A backend on a headless surface; nullptr after a skip (no device, or no headless surface).
std::unique_ptr<Rig> MakeRig() {
    auto rig = std::make_unique<Rig>();
    rhi::InstanceOptions options;
    options.headless_surface = true;
    rig->instance = test::RequireInstance(options);
    if (!rig->instance) {
        return nullptr;
    }
    rig->surface = test::MakeHeadlessSurface(rig->instance);
    if (!rig->surface) {
        return nullptr;
    }
    rhi::BackendOptions backend_options;
    backend_options.surface = rig->surface->Handle();
    rig->backend = test::RequireBackend(backend_options, rig->instance);
    if (!rig->backend) {
        return nullptr;
    }
    return rig;
}

// What the layout source hands the presenter, changeable from the test thread; the thread that called it is noted.
struct LayoutControl {
    std::mutex mutex;
    sl::Inputs inputs;
    bool linear = false;
    std::set<std::thread::id> callers;
    std::atomic<int> calls{0};

    LayoutControl() {
        inputs.mode = sl::Mode::Stacked;
    }
    void Set(sl::Mode mode) {
        std::lock_guard lock(mutex);
        inputs.mode = mode;
    }
    sc::LayoutSource Source() {
        return [this] {
            std::lock_guard lock(mutex);
            callers.insert(std::this_thread::get_id());
            ++calls;
            sc::LayoutRequest request;
            request.inputs = inputs;
            request.options.linear_filter = linear;
            return request;
        };
    }
};

struct Colour {
    u8 r, g, b;
};

// A frame of two solid CPU-pixel screens. The top colour depends on the sequence, so a row can be tied to its frame.
pr::DisplayFrame MakeFrame(u64 sequence, Colour top = {0, 0, 0}, Colour bottom = {0, 0, 0}) {
    if (top.r == 0 && top.g == 0 && top.b == 0) {
        top = {static_cast<u8>(40 + (sequence * 37) % 200), static_cast<u8>(60 + (sequence * 11) % 150), 200};
    }
    if (bottom.r == 0 && bottom.g == 0 && bottom.b == 0) {
        bottom = {30, static_cast<u8>(90 + (sequence * 7) % 100), static_cast<u8>(50 + (sequence * 3) % 100)};
    }
    const auto solid = [](u32 w, u32 h, Colour c) {
        auto pixels = std::make_shared<std::vector<u8>>(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
            (*pixels)[i * 4 + 0] = c.r;
            (*pixels)[i * 4 + 1] = c.g;
            (*pixels)[i * 4 + 2] = c.b;
            (*pixels)[i * 4 + 3] = 255;
        }
        pr::ScreenImage screen;
        screen.pixels = pixels;
        screen.width = w;
        screen.height = h;
        screen.region = {0, 0, w, h};
        return screen;
    };
    pr::DisplayFrame frame;
    frame.sequence = sequence;
    frame.game_frame = static_cast<u32>(1000 + sequence);
    frame.top = solid(pr::kTopWidth, pr::kTopHeight, top);
    frame.bottom = solid(pr::kBottomWidth, pr::kBottomHeight, bottom);
    return frame;
}

template <class Pred>
bool WaitFor(Pred&& pred, int timeout_ms = 30'000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// Holds the present thread inside a hook until the test opens the gate, or Stop is requested.
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    int entered = 0;
    std::atomic<sc::Presenter*> presenter{nullptr};

    void Hold() {
        std::unique_lock lock(mutex);
        ++entered;
        cv.notify_all();
        while (!open) {
            sc::Presenter* p = presenter.load();
            if (p != nullptr && p->StopRequested()) {
                return;
            }
            cv.wait_for(lock, std::chrono::milliseconds(2));
        }
    }
    bool WaitEntered(int count = 1) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(30), [&] { return entered >= count; });
    }
    void Open() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

// Records what the compositions saw, in order.
struct Seen {
    std::mutex mutex;
    std::vector<u64> sequences;
    std::vector<std::pair<u32, u32>> sizes;
    std::set<std::thread::id> threads;
    std::function<void()> extra;  // called after recording, outside the lock

    sc::TestHooks Hooks() {
        sc::TestHooks hooks;
        hooks.in_composition = [this](const pr::DisplayFrame& frame, u32 w, u32 h) {
            {
                std::lock_guard lock(mutex);
                sequences.push_back(frame.sequence);
                sizes.emplace_back(w, h);
                threads.insert(std::this_thread::get_id());
            }
            if (extra) {
                extra();
            }
        };
        return hooks;
    }
    std::vector<u64> Sequences() {
        std::lock_guard lock(mutex);
        return sequences;
    }
};

std::unique_ptr<sc::Presenter> MakePresenter(Rig& rig, LayoutControl& layout, sc::TestHooks hooks = {},
                                             sc::Options options = {}, u32 width = 160, u32 height = 120) {
    rhi::Result<std::unique_ptr<sc::Presenter>> presenter = sc::Presenter::Create(
        rig.backend, sc::Target::Window(rig.surface->Handle(), width, height), layout.Source(), options, std::move(hooks));
    REQUIRE_MESSAGE(presenter.ok(), presenter.error());
    return std::move(*presenter);
}

// Publishes `frame` and waits until `count` presents have been made in all.
void PublishAndWait(sc::Presenter& presenter, const pr::DisplayFrame& frame, u64 count) {
    presenter.Publish(frame);
    REQUIRE(WaitFor([&] { return presenter.GetStats().presented >= count; }));
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& name) {
        path = std::filesystem::temp_directory_path() /
               ("mh4n_swapchain_test_" + name + "_" + std::to_string(Clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct LogRow {
    u64 sequence = 0, game_frame = 0, published = 0, composed = 0, presented = 0;
    std::optional<u64> displayed;
    u64 dropped = 0;
};
struct ParsedLog {
    std::string comment;
    std::string columns;
    std::vector<LogRow> rows;
};

ParsedLog ReadLog(const std::filesystem::path& path) {
    ParsedLog log;
    std::ifstream file(path);
    REQUIRE(file.good());
    std::getline(file, log.comment);
    std::getline(file, log.columns);
    std::string line;
    while (std::getline(file, line)) {
        std::vector<std::string> fields;
        std::stringstream stream(line);
        std::string field;
        while (std::getline(stream, field, ',')) {
            fields.push_back(field);
        }
        if (!line.empty() && line.back() == ',') {
            fields.emplace_back();
        }
        REQUIRE(fields.size() == 7);
        LogRow row;
        row.sequence = std::stoull(fields[0]);
        row.game_frame = std::stoull(fields[1]);
        row.published = std::stoull(fields[2]);
        row.composed = std::stoull(fields[3]);
        row.presented = std::stoull(fields[4]);
        if (!fields[5].empty()) {
            row.displayed = std::stoull(fields[5]);
        }
        row.dropped = std::stoull(fields[6]);
        log.rows.push_back(row);
    }
    return log;
}

struct Ppm {
    u32 width = 0, height = 0;
    std::vector<u8> rgb;
    Colour At(u32 x, u32 y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
        return {rgb[i], rgb[i + 1], rgb[i + 2]};
    }
};
bool ReadPpm(const std::filesystem::path& path, Ppm* out) {
    std::ifstream file(path, std::ios::binary);
    std::string magic;
    u32 maxval = 0;
    file >> magic >> out->width >> out->height >> maxval;
    file.get();
    if (!file || magic != "P6" || maxval != 255) {
        return false;
    }
    out->rgb.resize(static_cast<std::size_t>(out->width) * out->height * 3);
    file.read(reinterpret_cast<char*>(out->rgb.data()), static_cast<std::streamsize>(out->rgb.size()));
    return static_cast<bool>(file);
}

// ---- the device cases ------------------------------------------------------------------------------------------------

TEST_CASE("presenter/presents_frames_in_order") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    Seen seen;
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, seen.Hooks());
    TempDir dir("order");
    REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());

    constexpr u64 kFrames = 12;
    for (u64 i = 0; i < kFrames; ++i) {
        PublishAndWait(*presenter, MakeFrame(i), i + 1);
    }
    presenter->Stop();

    const sc::Stats stats = presenter->GetStats();
    CHECK(stats.published == kFrames);
    CHECK(stats.presented == kFrames);
    CHECK(stats.composed == kFrames);
    CHECK(stats.dropped == 0);
    CHECK(stats.swapchain_creations == 1);
    CHECK(stats.extent_width == 160);
    CHECK(stats.extent_height == 120);
    std::vector<u64> expected(kFrames);
    for (u64 i = 0; i < kFrames; ++i) expected[i] = i;
    CHECK(seen.Sequences() == expected);

    // everything that records, submits and presents is on one thread, and it is not the test's
    {
        std::lock_guard lock(seen.mutex);
        CHECK(seen.threads.size() == 1);
        CHECK(*seen.threads.begin() != std::this_thread::get_id());
    }
    {
        std::lock_guard lock(layout.mutex);
        CHECK(layout.callers.size() == 1);
        CHECK(*layout.callers.begin() != std::this_thread::get_id());
    }
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/three_frames_published_faster_than_presented_show_only_the_newest") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    Seen seen;
    Gate gate;
    int calls = 0;
    seen.extra = [&] {
        if (calls++ == 0) gate.Hold();  // frame 0: the thread stays inside its composition
    };
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, seen.Hooks());
    gate.presenter = presenter.get();
    TempDir dir("newest");
    REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());

    presenter->Publish(MakeFrame(0));
    REQUIRE(gate.WaitEntered());  // the thread is inside the composition of frame 0
    presenter->Publish(MakeFrame(1));
    presenter->Publish(MakeFrame(2));
    presenter->Publish(MakeFrame(3));
    // frames 1 and 2 were replaced in the mailbox before anyone took them
    CHECK(presenter->GetStats().dropped == 2);
    gate.Open();
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 2; }));
    presenter->Stop();

    CHECK(seen.Sequences() == std::vector<u64>{0, 3});  // 1 and 2 were never shown
    const sc::Stats stats = presenter->GetStats();
    CHECK(stats.published == 4);
    CHECK(stats.presented == 2);
    CHECK(stats.dropped == 2);
    const ParsedLog log = ReadLog(dir.path / "present.csv");
    REQUIRE(log.rows.size() == 2);
    CHECK(log.rows[0].sequence == 0);
    CHECK(log.rows[0].dropped == 0);
    CHECK(log.rows[1].sequence == 3);
    CHECK(log.rows[1].dropped == 2);
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/publish_returns_while_the_present_thread_is_inside_a_composition") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    Seen seen;
    Gate gate;
    int calls = 0;
    seen.extra = [&] {
        if (calls++ == 0) gate.Hold();
    };
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, seen.Hooks());
    gate.presenter = presenter.get();

    presenter->Publish(MakeFrame(0));
    REQUIRE(gate.WaitEntered());
    // The thread is held; Publish, SetUiLayer(an unset layer is fine), RequestRedraw, CurrentLayout and GetStats all return.
    std::atomic<bool> returned{false};
    std::thread caller([&] {
        for (u64 i = 1; i <= 50; ++i) {
            presenter->Publish(MakeFrame(i));
        }
        presenter->RequestRedraw();
        presenter->SetVsync(true);
        presenter->Resize(160, 120);
        presenter->CurrentLayout();
        presenter->GetStats();
        returned = true;
    });
    CHECK(WaitFor([&] { return returned.load(); }, 10'000));
    caller.join();
    CHECK(presenter->GetStats().presented == 0);  // still inside the first composition
    CHECK(presenter->GetStats().published == 51);
    CHECK(presenter->GetStats().dropped == 49);
    gate.Open();
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 2; }));
    presenter->Stop();
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/a_resize_recreates_the_swapchain_and_the_next_frame_has_the_new_size") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    Seen seen;
    std::vector<std::array<u32, 3>> created;  // width, height, image count
    std::mutex created_mutex;
    sc::TestHooks hooks = seen.Hooks();
    hooks.swapchain_created = [&](u32 w, u32 h, s32, u32 count) {
        std::lock_guard lock(created_mutex);
        created.push_back({w, h, count});
    };
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks), {}, 160, 120);

    PublishAndWait(*presenter, MakeFrame(0), 1);
    CHECK(presenter->GetStats().swapchain_creations == 1);
    presenter->Resize(224, 136);
    // the resize draws the last frame again at the new size (the window shows something at once)
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 2; }));
    PublishAndWait(*presenter, MakeFrame(1), 3);
    presenter->Stop();

    const sc::Stats stats = presenter->GetStats();
    CHECK(stats.swapchain_creations == 2);
    CHECK(stats.extent_width == 224);
    CHECK(stats.extent_height == 136);
    {
        std::lock_guard lock(seen.mutex);
        REQUIRE(seen.sizes.size() == 3);
        CHECK(seen.sizes[0] == std::make_pair(160u, 120u));
        CHECK(seen.sizes[1] == std::make_pair(224u, 136u));  // the redraw
        CHECK(seen.sizes[2] == std::make_pair(224u, 136u));  // the next frame
    }
    {
        std::lock_guard lock(created_mutex);
        REQUIRE(created.size() == 2);
        CHECK((created[0][0] == 160 && created[0][1] == 120));
        CHECK((created[1][0] == 224 && created[1][1] == 136));
        CHECK(created[1][2] >= 2);
    }
    // the layout of the last composed frame is the layout of the new size
    const sl::Layout current = presenter->CurrentLayout();
    CHECK(current.width >= 224);
    CHECK(current.height >= 136);
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/minimised_presents_nothing_and_counts_the_frames_as_dropped") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    Seen seen;
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, seen.Hooks());

    PublishAndWait(*presenter, MakeFrame(0), 1);
    presenter->SetMinimised(true);
    for (u64 i = 1; i <= 5; ++i) {
        presenter->Publish(MakeFrame(i));
        // let the thread take each one, so none is only replaced in the mailbox
        REQUIRE(WaitFor([&] { return presenter->GetStats().dropped >= i; }));
    }
    presenter->RequestRedraw();  // nothing is drawn while minimised
    presenter->Resize(160, 120);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(presenter->GetStats().presented == 1);
    CHECK(presenter->GetStats().composed == 1);
    CHECK(presenter->GetStats().dropped == 5);
    CHECK(seen.Sequences() == std::vector<u64>{0});

    // leaving minimised draws the newest frame again
    presenter->SetMinimised(false);
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 2; }));
    CHECK(seen.Sequences() == std::vector<u64>{0, 5});
    CHECK(presenter->GetStats().redraws == 1);

    // a zero extent presents nothing either
    presenter->Resize(0, 120);
    presenter->Publish(MakeFrame(6));
    REQUIRE(WaitFor([&] { return presenter->GetStats().dropped >= 6; }));
    CHECK(presenter->GetStats().presented == 2);
    presenter->Resize(160, 120);
    // the redraw at the restored size; frame 7 is published after it, because a frame published at once could be taken
    // for the redraw's slot (the mailbox is taken first, and the redraw is then not made), which is legal
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 3; }));
    PublishAndWait(*presenter, MakeFrame(7), 4);
    presenter->Stop();
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/stop_returns_with_a_frame_pending_and_with_the_thread_blocked") {
    SUBCASE("a frame sits in the mailbox while the thread is inside a composition") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        Seen seen;
        Gate gate;
        int calls = 0;
        seen.extra = [&] {
            if (calls++ == 0) gate.Hold();
        };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, seen.Hooks());
        gate.presenter = presenter.get();
        presenter->Publish(MakeFrame(0));
        REQUIRE(gate.WaitEntered());
        presenter->Publish(MakeFrame(1));  // pending in the mailbox
        const auto begin = Clock::now();
        presenter->Stop();                 // the hook ends its wait when Stop is requested
        CHECK(Clock::now() - begin < std::chrono::seconds(10));
        CHECK(presenter->StopRequested());
        CHECK(presenter->GetStats().dropped == 1);  // frame 1, left in the mailbox, counts as dropped
        presenter->Publish(MakeFrame(2));  // after Stop: dropped, never shown
        CHECK(presenter->GetStats().dropped == 2);
        CHECK(presenter->GetStats().published == 3);
        CHECK(seen.Sequences() == std::vector<u64>{0});
        presenter->Stop();  // idempotent
    }
    SUBCASE("the thread is waiting in an acquire that never completes") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        sc::TestHooks hooks;
        hooks.acquire_result = [] { return static_cast<s32>(VK_TIMEOUT); };  // FIFO with every image still in use
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        presenter->Publish(MakeFrame(0));
        REQUIRE(WaitFor([&] { return presenter->GetStats().acquire_timeouts >= 3; }));
        const auto begin = Clock::now();
        presenter->Stop();
        CHECK(Clock::now() - begin < std::chrono::seconds(10));
        CHECK(presenter->GetStats().presented == 0);
        CHECK(presenter->GetStats().dropped >= 1);  // the frame it was trying to show
    }
    SUBCASE("the thread is waiting for a frame to be shown") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        sc::TestHooks hooks;
        hooks.pretend_present_wait = true;  // the wait is the hook's: this device may have no present wait (lavapipe has none)
        std::atomic<int> waits{0};
        std::atomic<int> waits_after_stop{0};
        std::atomic<sc::Presenter*> watched{nullptr};
        hooks.present_wait_result = [&] {
            ++waits;
            sc::Presenter* p = watched.load();
            if (p != nullptr && p->StopRequested()) {
                ++waits_after_stop;
            }
            return static_cast<s32>(VK_TIMEOUT);  // never shown
        };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        watched = presenter.get();
        CHECK(presenter->GetStats().display_time == "present_wait");
        presenter->Publish(MakeFrame(0));
        REQUIRE(WaitFor([&] { return waits.load() >= 3; }));
        const auto begin = Clock::now();
        presenter->Stop();  // the wait is cut into slices that look at Stop; it gives up on the frame's time at once
        CHECK(Clock::now() - begin < std::chrono::seconds(10));
        // Not a wall-clock bound (a loaded machine stretches any): the loop looks at Stop before each slice, so at most
        // the one slice that had begun when the flag was set asks the hook after it. A wait that ignored Stop would ask
        // it until its 250 ms limit, hundreds of times.
        CHECK(waits_after_stop.load() <= 1);
    }
    SUBCASE("idle: nothing published") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout);
        const auto begin = Clock::now();
        presenter->Stop();
        CHECK(Clock::now() - begin < std::chrono::seconds(10));
    }
}

TEST_CASE("presenter/the_present_log_rows_match_the_frames_shown") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    Seen seen;
    sc::Options options;
    options.display_time_source = "swapchain_test_clock";
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, seen.Hooks(), options);
    TempDir dir("log");
    const std::filesystem::path path = dir.path / "present.csv";
    presenter->SetRefreshHz(144.0);
    REQUIRE(presenter->SetPresentLog(path.string()).ok());
    CHECK_FALSE(presenter->SetPresentLog((dir.path / "no_such_dir" / "x.csv").string()).ok());
    REQUIRE(presenter->SetPresentLog(path.string()).ok());  // reopened

    constexpr u64 kFrames = 8;
    const u64 before = sc::NowUs();
    for (u64 i = 0; i < kFrames; ++i) {
        PublishAndWait(*presenter, MakeFrame(i * 3), i + 1);  // sequences 0, 3, 6 ... (gaps are not drops)
    }
    presenter->Stop();
    const u64 after = sc::NowUs();

    const ParsedLog log = ReadLog(path);
    CHECK(log.comment.rfind("# present v1 ", 0) == 0);
    CHECK(log.comment.find("refresh_hz=144.00") != std::string::npos);
    CHECK(log.comment.find("display_time=") != std::string::npos);
    CHECK(log.columns == sc::kPresentColumns);
    REQUIRE(log.rows.size() == kFrames);
    u64 previous_presented = 0;
    for (u64 i = 0; i < kFrames; ++i) {
        const LogRow& row = log.rows[i];
        CHECK(row.sequence == i * 3);
        CHECK(row.game_frame == 1000 + i * 3);
        CHECK(row.dropped == 0);
        // the host times are of the steady clock, in the order the frame went through
        CHECK(row.published >= before);
        CHECK(row.composed >= row.published);
        CHECK(row.presented >= row.composed);
        CHECK(row.presented <= after);
        CHECK(row.presented >= previous_presented);
        previous_presented = row.presented;
    }
    // the sequences of the rows are the ones the compositions saw
    std::vector<u64> expected;
    for (const LogRow& row : log.rows) expected.push_back(row.sequence);
    CHECK(seen.Sequences() == expected);
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/vsync_off_picks_mailbox_or_immediate_as_the_surface_offers") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    // what this surface offers, asked of the driver here
    const rhi::VulkanHandles handles = rhi::Handles(*rig->backend);
    u32 count = 0;
    REQUIRE(vkGetPhysicalDeviceSurfacePresentModesKHR(handles.physical_device, rig->surface->surface, &count, nullptr) == VK_SUCCESS);
    std::vector<VkPresentModeKHR> offered(count);
    REQUIRE(vkGetPhysicalDeviceSurfacePresentModesKHR(handles.physical_device, rig->surface->surface, &count, offered.data()) == VK_SUCCESS);
    const auto has = [&](VkPresentModeKHR mode) { return std::find(offered.begin(), offered.end(), mode) != offered.end(); };
    REQUIRE(has(kFifo));  // the one mode every surface has
    std::string text;
    for (VkPresentModeKHR mode : offered) text += std::string(sc::PresentModeName(mode)) + " ";
    MESSAGE("the headless surface offers: " << text);
    // the rule, written out once more from the task's text: MAILBOX if offered, else IMMEDIATE, else FIFO
    const VkPresentModeKHR expected_off = has(kMailbox) ? kMailbox : has(kImmediate) ? kImmediate : kFifo;

    LayoutControl layout;
    sc::Options options;
    options.vsync = false;
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, {}, options);
    CHECK(presenter->GetStats().present_mode == static_cast<s32>(expected_off));
    CHECK((presenter->GetStats().present_mode == static_cast<s32>(kMailbox) ||
           presenter->GetStats().present_mode == static_cast<s32>(kImmediate)));
    PublishAndWait(*presenter, MakeFrame(0), 1);

    presenter->SetVsync(true);  // takes effect with the next present (a redraw is made for it)
    REQUIRE(WaitFor([&] { return presenter->GetStats().present_mode == static_cast<s32>(kFifo); }));
    CHECK(presenter->GetStats().swapchain_creations == 2);
    presenter->SetVsync(false);
    REQUIRE(WaitFor([&] { return presenter->GetStats().present_mode == static_cast<s32>(expected_off); }));
    CHECK(presenter->GetStats().swapchain_creations == 3);
    PublishAndWait(*presenter, MakeFrame(1), presenter->GetStats().presented + 1);
    presenter->Stop();
    CHECK(rig->breaches.Total() == 0);

    // and with vsync on at the start: FIFO
    std::unique_ptr<sc::Presenter> on = MakePresenter(*rig, layout);
    CHECK(on->GetStats().present_mode == static_cast<s32>(kFifo));
}

TEST_CASE("presenter/the_headless_surface_lets_the_swapchain_choose_its_extent") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    const rhi::VulkanHandles handles = rhi::Handles(*rig->backend);
    VkSurfaceCapabilitiesKHR caps = {};
    REQUIRE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(handles.physical_device, rig->surface->surface, &caps) == VK_SUCCESS);
    u32 format_count = 0;
    REQUIRE(vkGetPhysicalDeviceSurfaceFormatsKHR(handles.physical_device, rig->surface->surface, &format_count, nullptr) == VK_SUCCESS);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    REQUIRE(vkGetPhysicalDeviceSurfaceFormatsKHR(handles.physical_device, rig->surface->surface, &format_count, formats.data()) == VK_SUCCESS);
    std::string text;
    for (const VkSurfaceFormatKHR& format : formats) text += std::to_string(format.format) + "/" + std::to_string(format.colorSpace) + " ";
    MESSAGE("headless surface: currentExtent " << caps.currentExtent.width << "x" << caps.currentExtent.height << ", min "
                                               << caps.minImageExtent.width << "x" << caps.minImageExtent.height << ", max "
                                               << caps.maxImageExtent.width << "x" << caps.maxImageExtent.height << ", images "
                                               << caps.minImageCount << ".." << caps.maxImageCount << ", formats (VkFormat/colour space) "
                                               << text);
    // VK_EXT_headless_surface: the special value, so the extent is the one Resize set (clamped to the limits)
    CHECK(caps.currentExtent.width == 0xFFFFFFFFu);
    CHECK(caps.currentExtent.height == 0xFFFFFFFFu);
    CHECK(sc::ChooseSurfaceFormat(formats).has_value());

    LayoutControl layout;
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, {}, {}, 96, 72);
    CHECK(presenter->GetStats().extent_width == std::clamp(96u, caps.minImageExtent.width, caps.maxImageExtent.width));
    // a request beyond the limits is clamped to them
    presenter->Resize(caps.maxImageExtent.width + 1000, 72);
    PublishAndWait(*presenter, MakeFrame(0), 1);
    CHECK(presenter->GetStats().extent_width == caps.maxImageExtent.width);
    CHECK(presenter->GetStats().extent_height == 72);
    presenter->Stop();
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/current_layout_is_the_layout_of_the_frame_composed_last") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, {}, {}, 480, 480);

    const sl::Layout none = presenter->CurrentLayout();  // before the first frame: nothing composed
    CHECK(none.width == 0);
    CHECK(none.height == 0);

    layout.Set(sl::Mode::Stacked);
    PublishAndWait(*presenter, MakeFrame(0), 1);
    sl::Inputs inputs;
    inputs.mode = sl::Mode::Stacked;
    inputs.window_w = 480;
    inputs.window_h = 480;
    const sl::Layout stacked = sl::Compute(inputs);
    sl::Layout current = presenter->CurrentLayout();
    CHECK(current.top == stacked.top);
    CHECK(current.bottom == stacked.bottom);
    CHECK(current.width == stacked.width);
    CHECK(current.height == stacked.height);
    CHECK(current.top_shown == stacked.top_shown);
    CHECK(current.bottom_shown == stacked.bottom_shown);

    layout.Set(sl::Mode::Side);
    PublishAndWait(*presenter, MakeFrame(1), 2);
    inputs.mode = sl::Mode::Side;
    const sl::Layout side = sl::Compute(inputs);
    current = presenter->CurrentLayout();
    CHECK(current.top == side.top);
    CHECK(current.bottom == side.bottom);
    CHECK_FALSE(current.top == stacked.top);  // the two layouts differ, so the check above means something

    // A redraw uses the layout source again: a layout change shows without a new game frame
    layout.Set(sl::Mode::Single);
    presenter->RequestRedraw();
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 3; }));
    inputs.mode = sl::Mode::Single;
    const sl::Layout single = sl::Compute(inputs);
    current = presenter->CurrentLayout();
    CHECK(current.top == single.top);
    CHECK(current.bottom_shown == single.bottom_shown);
    presenter->Stop();
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/out_of_date_and_suboptimal_recreate_the_swapchain_on_the_present_thread") {
    SUBCASE("out of date from an acquire") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        Seen seen;
        std::atomic<int> asked{0};
        sc::TestHooks hooks = seen.Hooks();
        hooks.acquire_result = [&] { return asked++ == 0 ? static_cast<s32>(VK_ERROR_OUT_OF_DATE_KHR) : 0; };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        PublishAndWait(*presenter, MakeFrame(0), 1);
        const sc::Stats stats = presenter->GetStats();
        CHECK(stats.out_of_date == 1);
        CHECK(stats.swapchain_creations == 2);  // the new swapchain was made before the frame was shown
        CHECK(stats.presented == 1);
        CHECK(stats.dropped == 0);              // the frame was not lost
        CHECK(seen.Sequences() == std::vector<u64>{0});
        presenter->Stop();
        CHECK(rig->breaches.Total() == 0);
    }
    SUBCASE("suboptimal from an acquire: the frame is shown, the next one has a new swapchain") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        Seen seen;
        std::atomic<int> asked{0};
        sc::TestHooks hooks = seen.Hooks();
        hooks.acquire_result = [&] { return asked++ == 0 ? static_cast<s32>(VK_SUBOPTIMAL_KHR) : 0; };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        PublishAndWait(*presenter, MakeFrame(0), 1);
        sc::Stats stats = presenter->GetStats();
        CHECK(stats.suboptimal == 1);
        CHECK(stats.presented == 1);            // shown with the swapchain it was acquired from
        CHECK(stats.swapchain_creations == 1);
        PublishAndWait(*presenter, MakeFrame(1), 2);
        stats = presenter->GetStats();
        CHECK(stats.swapchain_creations == 2);  // recreated before the next frame
        CHECK(stats.presented == 2);
        CHECK(stats.dropped == 0);
        CHECK(seen.Sequences() == std::vector<u64>{0, 1});
        presenter->Stop();
        CHECK(rig->breaches.Total() == 0);
    }
    SUBCASE("out of date from a present: the frame is composed again on the new swapchain") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        Seen seen;
        std::atomic<int> asked{0};
        sc::TestHooks hooks = seen.Hooks();
        hooks.present_result = [&] { return asked++ == 0 ? static_cast<s32>(VK_ERROR_OUT_OF_DATE_KHR) : 0; };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        PublishAndWait(*presenter, MakeFrame(0), 1);
        const sc::Stats stats = presenter->GetStats();
        CHECK(stats.out_of_date == 1);
        CHECK(stats.swapchain_creations == 2);
        CHECK(stats.presented == 1);
        CHECK(stats.composed == 2);
        CHECK(seen.Sequences() == std::vector<u64>{0, 0});
        presenter->Stop();
        CHECK(rig->breaches.Total() == 0);
    }
    SUBCASE("suboptimal from a present: shown, then recreated for the next frame") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        std::atomic<int> asked{0};
        sc::TestHooks hooks;
        hooks.present_result = [&] { return asked++ == 0 ? static_cast<s32>(VK_SUBOPTIMAL_KHR) : 0; };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        PublishAndWait(*presenter, MakeFrame(0), 1);
        CHECK(presenter->GetStats().suboptimal == 1);
        CHECK(presenter->GetStats().swapchain_creations == 1);
        PublishAndWait(*presenter, MakeFrame(1), 2);
        CHECK(presenter->GetStats().swapchain_creations == 2);
        CHECK(presenter->GetStats().presented == 2);
        presenter->Stop();
        CHECK(rig->breaches.Total() == 0);
    }
    SUBCASE("a swapchain that keeps answering out of date gives up on the frame, and the next one is tried") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        std::atomic<bool> broken{true};
        sc::TestHooks hooks;
        hooks.acquire_result = [&] { return broken ? static_cast<s32>(VK_ERROR_OUT_OF_DATE_KHR) : 0; };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        presenter->Publish(MakeFrame(0));
        REQUIRE(WaitFor([&] { return presenter->GetStats().dropped >= 1; }));
        CHECK(presenter->GetStats().presented == 0);
        broken = false;
        PublishAndWait(*presenter, MakeFrame(1), 1);
        presenter->Stop();
        CHECK(rig->breaches.Total() == 0);
    }
}

TEST_CASE("presenter/an_offscreen_target_dumps_every_nth_image_and_the_last_at_stop") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    layout.Set(sl::Mode::Stacked);
    TempDir dir("dump");
    rhi::Result<std::unique_ptr<sc::Presenter>> made =
        sc::Presenter::Create(rig->backend, sc::Target::Offscreen(400, 480), layout.Source());
    REQUIRE_MESSAGE(made.ok(), made.error());
    std::unique_ptr<sc::Presenter> presenter = std::move(*made);
    CHECK(presenter->GetStats().present_mode == -1);
    CHECK(presenter->GetStats().display_time == "none");
    REQUIRE(presenter->DumpTo((dir.path / "frames").string(), 2).ok());
    REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());

    constexpr u64 kFrames = 5;
    for (u64 i = 0; i < kFrames; ++i) {
        PublishAndWait(*presenter, MakeFrame(10 + i), i + 1);
    }
    presenter->Stop();

    // every 2nd composed image: the 1st, 3rd and 5th; the last again as last.ppm
    for (u64 i = 0; i < kFrames; ++i) {
        const bool dumped = std::filesystem::exists(dir.path / "frames" / ("presented_" + std::to_string(10 + i) + ".ppm"));
        CHECK(dumped == (i % 2 == 0));
    }
    Ppm last;
    REQUIRE(ReadPpm(dir.path / "frames" / "last.ppm", &last));
    CHECK(last.width == 400);
    CHECK(last.height == 480);

    // the pixels: the centre of each screen's rectangle has that screen's colour (solid, so any sampling gives it)
    sl::Inputs inputs;
    inputs.mode = sl::Mode::Stacked;
    inputs.window_w = 400;
    inputs.window_h = 480;
    const sl::Layout expected = sl::Compute(inputs);
    const pr::DisplayFrame frame = MakeFrame(10 + kFrames - 1);
    const auto centre_matches = [&](const sl::Rect& rect, const pr::ScreenImage& screen) {
        const u32 x = (rect.left + rect.right) / 2;
        const u32 y = (rect.top + rect.bottom) / 2;
        const Colour got = last.At(x, y);
        return got.r == (*screen.pixels)[0] && got.g == (*screen.pixels)[1] && got.b == (*screen.pixels)[2];
    };
    CHECK(centre_matches(expected.top, *frame.top));
    CHECK(centre_matches(expected.bottom, *frame.bottom));
    Ppm first;
    REQUIRE(ReadPpm(dir.path / "frames" / "presented_10.ppm", &first));
    CHECK(first.width == 400);
    const pr::DisplayFrame first_frame = MakeFrame(10);
    const Colour got = first.At((expected.top.left + expected.top.right) / 2, (expected.top.top + expected.top.bottom) / 2);
    CHECK(got.r == (*first_frame.top->pixels)[0]);
    CHECK(got.g == (*first_frame.top->pixels)[1]);
    CHECK(got.b == (*first_frame.top->pixels)[2]);

    // the log: a row per frame, none shown-time (an offscreen target has no display)
    const ParsedLog log = ReadLog(dir.path / "present.csv");
    CHECK(log.comment.find("display_time=none") != std::string::npos);
    REQUIRE(log.rows.size() == kFrames);
    for (const LogRow& row : log.rows) {
        CHECK_FALSE(row.displayed.has_value());
    }
    // a window cannot be dumped
    std::unique_ptr<sc::Presenter> window = MakePresenter(*rig, layout);
    CHECK_FALSE(window->DumpTo((dir.path / "x").string(), 1).ok());
    window->Stop();
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/an_offscreen_target_follows_a_resize") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    TempDir dir("offresize");
    Seen seen;
    rhi::Result<std::unique_ptr<sc::Presenter>> made =
        sc::Presenter::Create(rig->backend, sc::Target::Offscreen(200, 100), layout.Source(), {}, seen.Hooks());
    REQUIRE_MESSAGE(made.ok(), made.error());
    std::unique_ptr<sc::Presenter> presenter = std::move(*made);
    REQUIRE(presenter->DumpTo(dir.path.string(), 1).ok());
    PublishAndWait(*presenter, MakeFrame(0), 1);
    presenter->Resize(300, 150);
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 2; }));
    presenter->Stop();
    Ppm image;
    REQUIRE(ReadPpm(dir.path / "last.ppm", &image));
    CHECK(image.width == 300);
    CHECK(image.height == 150);
    CHECK(presenter->GetStats().extent_width == 300);
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/the_owner_rule_is_checked_on_the_present_thread") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    // The normal path: all of the presenter's own recording, submission and present on the present thread breach nothing.
    {
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout);
        PublishAndWait(*presenter, MakeFrame(0), 1);
        presenter->Stop();
        CHECK(rig->breaches.Total() == 0);
    }
    if (!MH4N_RHI_CHECKS) {
        WARN_MESSAGE(false, "the breach itself is not checked: the owner-rule checks are compiled out");
        return;
    }
    // A recording forced onto another thread (the hook runs the frame's work on a thread of its own) calls the handler.
    std::atomic<int> forced{0};
    sc::TestHooks hooks;
    hooks.frame_runner = [&](const std::function<void()>& work) {
        if (forced++ == 0) {
            std::thread other(work);
            other.join();
        } else {
            work();
        }
    };
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
    PublishAndWait(*presenter, MakeFrame(0), 1);
    presenter->Stop();
    CHECK(rig->breaches.Count(rhi::OwnerRule::WrongThread) >= 3);  // the recording, the submission and the present
    {
        std::lock_guard lock(rig->breaches.mutex);
        bool saw_record = false, saw_submit = false, saw_present = false;
        for (const rhi::OwnerRuleBreach& breach : rig->breaches.breaches) {
            if (breach.rule != rhi::OwnerRule::WrongThread) continue;
            saw_record = saw_record || breach.message.find("recording") != std::string::npos;
            saw_submit = saw_submit || breach.message.find("submission") != std::string::npos;
            saw_present = saw_present || breach.message.find("present") != std::string::npos;
        }
        CHECK(saw_record);
        CHECK(saw_submit);
        CHECK(saw_present);
    }
}

// The layer the compositor calls: counts its calls and the thread.
struct CountingLayer final : pr::UiLayer {
    std::atomic<int> calls{0};
    std::atomic<int> null_recorders{0};  // checked by the test thread: doctest's asserts are not for other threads
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::atomic<u32> width{0}, height{0};
    void Record(void* recorder, u32 w, u32 h) override {
        null_recorders += recorder == nullptr ? 1 : 0;
        ++calls;
        width = w;
        height = h;
        std::lock_guard lock(mutex);
        threads.insert(std::this_thread::get_id());
    }
};

TEST_CASE("presenter/the_ui_layer_is_called_by_the_present_thread_and_a_redraw_shows_a_change_of_ui") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout);
    CountingLayer layer;

    presenter->RequestRedraw();  // nothing to draw before the first frame
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(presenter->GetStats().presented == 0);

    PublishAndWait(*presenter, MakeFrame(0), 1);
    CHECK(layer.calls.load() == 0);
    presenter->SetUiLayer(&layer);
    presenter->RequestRedraw();  // the UI changed while the game shows nothing new
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 2; }));
    CHECK(layer.calls.load() == 1);
    CHECK(layer.null_recorders.load() == 0);
    CHECK(layer.width.load() == 160);
    CHECK(layer.height.load() == 120);
    PublishAndWait(*presenter, MakeFrame(1), 3);
    CHECK(layer.calls.load() == 2);
    CHECK(presenter->GetStats().redraws == 1);
    {
        std::lock_guard lock(layer.mutex);
        REQUIRE(layer.threads.size() == 1);
        CHECK(*layer.threads.begin() != std::this_thread::get_id());
    }
    presenter->SetUiLayer(nullptr);  // returns after any composition in progress; the layer is not called again
    PublishAndWait(*presenter, MakeFrame(2), 4);
    CHECK(layer.calls.load() == 2);
    presenter->Stop();
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/the_displayed_time_comes_from_the_first_source_that_applies") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    // On this device: what the backend says, and what the presenter chose from it (the pure function's answer).
    const bool measures = rig->backend->PresentWaitMeasuresDisplay();
    const bool timing = rig->backend->DisplayTimingAvailable();
    MESSAGE("PresentWaitAvailable=" << rig->backend->PresentWaitAvailable() << " PresentWaitMeasuresDisplay=" << measures
                                    << " DisplayTimingAvailable=" << timing);
    LayoutControl layout;
    {
        sc::Options options;
        options.display_time_source = "swapchain_test_clock";
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, {}, options);
        const sc::DisplayTimeChoice expected = sc::ChooseDisplayTime(measures, timing, "swapchain_test_clock");
        CHECK(presenter->GetStats().display_time == sc::DisplayTimeLabel(expected, "swapchain_test_clock"));
        TempDir dir("auto");
        REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());
        PublishAndWait(*presenter, MakeFrame(0), 1);
        PublishAndWait(*presenter, MakeFrame(1), 2);
        presenter->Stop();
        const ParsedLog log = ReadLog(dir.path / "present.csv");
        CHECK(log.comment.find("display_time=" + presenter->GetStats().display_time) != std::string::npos);
        REQUIRE(log.rows.size() == 2);
        if (expected.kind == sc::DisplayTimeKind::PresentWait) {
            // the wait returned after the present, before the next frame was presented
            for (const LogRow& row : log.rows) {
                if (row.displayed.has_value()) CHECK(*row.displayed >= row.presented);
            }
        }
    }
    // As if the device had neither extension: the registered source, called on the present thread once per presented
    // frame in order, and its answers are the log's displayed_us.
    {
        sc::Options options;
        options.display_time_source = "swapchain_test_clock";
        sc::TestHooks hooks;
        hooks.disable_present_wait = true;
        hooks.disable_display_timing = true;
        {
            std::lock_guard lock(g_source_mutex);
            g_source_calls.clear();
            g_source_threads.clear();
        }
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks), options);
        CHECK(presenter->GetStats().display_time == "swapchain_test_clock");
        TempDir dir("registered");
        REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());
        for (u64 i = 0; i < 4; ++i) {
            PublishAndWait(*presenter, MakeFrame(i), i + 1);
        }
        presenter->Stop();
        const ParsedLog log = ReadLog(dir.path / "present.csv");
        CHECK(log.comment.find("display_time=swapchain_test_clock") != std::string::npos);
        REQUIRE(log.rows.size() == 4);
        std::lock_guard lock(g_source_mutex);
        REQUIRE(g_source_calls.size() == 4);
        CHECK(g_source_threads.size() == 1);
        CHECK(*g_source_threads.begin() != std::this_thread::get_id());
        for (u64 i = 0; i < 4; ++i) {
            CHECK(g_source_calls[i].first == i + 1);                    // presentation ids 1, 2, 3, 4
            CHECK(g_source_calls[i].second == log.rows[i].presented);   // asked with the time the present returned
            REQUIRE(log.rows[i].displayed.has_value());
            CHECK(*log.rows[i].displayed == log.rows[i].presented + kTestSourceDelayUs);
        }
        g_source_calls.clear();
        g_source_threads.clear();
    }
    // As if neither extension and no registered source: none, and the field is empty
    {
        sc::Options options;
        options.display_time_source = "no such source";
        sc::TestHooks hooks;
        hooks.disable_present_wait = true;
        hooks.disable_display_timing = true;
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks), options);
        CHECK(presenter->GetStats().display_time == "none");
        TempDir dir("none");
        REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());
        PublishAndWait(*presenter, MakeFrame(0), 1);
        presenter->Stop();
        const ParsedLog log = ReadLog(dir.path / "present.csv");
        CHECK(log.comment.find("display_time=none") != std::string::npos);
        REQUIRE(log.rows.size() == 1);
        CHECK_FALSE(log.rows[0].displayed.has_value());
    }
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/present_wait_gives_a_displayed_time_after_the_present") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    TempDir dir("pw");
    SUBCASE("the device's own VK_KHR_present_wait") {
        if (!rig->backend->PresentWaitMeasuresDisplay()) {
            test::SkipCase("this device's present wait is not used for displayed times (PresentWaitMeasuresDisplay is false)");
            return;
        }
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout);
        CHECK(presenter->GetStats().display_time == "present_wait");
        REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());
        for (u64 i = 0; i < 5; ++i) {
            PublishAndWait(*presenter, MakeFrame(i), i + 1);
        }
        presenter->Stop();
        const ParsedLog log = ReadLog(dir.path / "present.csv");
        REQUIRE(log.rows.size() == 5);
        for (const LogRow& row : log.rows) {
            REQUIRE(row.displayed.has_value());
            CHECK(*row.displayed >= row.presented);
        }
    }
    SUBCASE("as if it measured the display: the wait is the hook's") {
        sc::TestHooks hooks;
        hooks.pretend_present_wait = true;
        std::atomic<int> waits{0};
        hooks.present_wait_result = [&] {
            // the first slice of each frame times out, the second shows it
            return waits++ % 2 == 0 ? static_cast<s32>(VK_TIMEOUT) : 0;
        };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        CHECK(presenter->GetStats().display_time == "present_wait");
        REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());
        for (u64 i = 0; i < 4; ++i) {
            PublishAndWait(*presenter, MakeFrame(i), i + 1);
        }
        presenter->Stop();
        const ParsedLog log = ReadLog(dir.path / "present.csv");
        CHECK(log.comment.find("display_time=present_wait") != std::string::npos);
        REQUIRE(log.rows.size() == 4);
        for (const LogRow& row : log.rows) {
            REQUIRE(row.displayed.has_value());
            CHECK(*row.displayed >= row.presented);
        }
        CHECK(waits.load() == 8);
    }
    SUBCASE("a wait that is out of date gives no time and marks the swapchain for recreation") {
        sc::TestHooks hooks;
        hooks.pretend_present_wait = true;
        std::atomic<int> waits{0};
        hooks.present_wait_result = [&] { return waits++ == 0 ? static_cast<s32>(VK_ERROR_OUT_OF_DATE_KHR) : 0; };
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
        REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());
        PublishAndWait(*presenter, MakeFrame(0), 1);
        PublishAndWait(*presenter, MakeFrame(1), 2);
        presenter->Stop();
        const ParsedLog log = ReadLog(dir.path / "present.csv");
        REQUIRE(log.rows.size() == 2);
        CHECK_FALSE(log.rows[0].displayed.has_value());
        CHECK(log.rows[1].displayed.has_value());
        CHECK(presenter->GetStats().swapchain_creations == 2);
    }
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/display_timing_times_are_matched_to_their_frames_in_order") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    TempDir dir("dt");
    // The hook stands in for vkGetPastPresentationTimingGOOGLE: it answers for the ids it is told to, once each.
    struct Answers {
        std::mutex mutex;
        std::vector<std::pair<u64, u64>> ready;  // (presentID, ns)
        std::atomic<int> polls{0};
    } answers;
    sc::TestHooks hooks;
    hooks.past_presentation_times = [&] {
        ++answers.polls;
        std::lock_guard lock(answers.mutex);
        return std::exchange(answers.ready, {});
    };
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
    CHECK(presenter->GetStats().display_time == "display_timing");
    REQUIRE(presenter->SetPresentLog((dir.path / "present.csv").string()).ok());

    // frame 1 is shown 700 us after "now" (the poll's time), frame 2's answer is a second and a half away from the
    // present call (a clock that is not the steady clock: not believed), frame 3 is never answered (its row waits, and
    // is written at Stop without a time), frame 4 is answered
    const auto answer = [&](u64 id, s64 offset_us) {
        std::lock_guard lock(answers.mutex);
        answers.ready.emplace_back(id, static_cast<u64>(static_cast<s64>(sc::NowUs()) + offset_us) * 1000);
    };
    presenter->Publish(MakeFrame(0));
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 1; }));
    answer(1, 700);
    presenter->Publish(MakeFrame(1));
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 2; }));
    answer(2, 1'500'000);
    presenter->Publish(MakeFrame(2));
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 3; }));
    presenter->Publish(MakeFrame(3));
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 4; }));
    // frame 3's row (id 3) is unanswered and holds frame 4's back until it ages out or Stop
    answer(4, 900);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    presenter->Stop();
    const ParsedLog log = ReadLog(dir.path / "present.csv");
    REQUIRE(log.rows.size() == 4);
    REQUIRE(log.rows[0].displayed.has_value());
    CHECK(*log.rows[0].displayed >= log.rows[0].presented);
    CHECK(*log.rows[0].displayed < log.rows[0].presented + 1'000'000);
    CHECK_FALSE(log.rows[1].displayed.has_value());   // not believed
    CHECK_FALSE(log.rows[2].displayed.has_value());   // never answered
    REQUIRE(log.rows[3].displayed.has_value());
    CHECK(*log.rows[3].displayed >= log.rows[3].presented);
    for (u64 i = 0; i < 4; ++i) {
        CHECK(log.rows[i].sequence == i);               // in order
    }
    CHECK(answers.polls.load() > 4);                    // polled while rows waited
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/concurrent_publishers_and_state_changes_lose_no_frame_unaccounted") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout);
    CountingLayer layer;
    presenter->SetUiLayer(&layer);

    constexpr int kPublishers = 3;
    constexpr u64 kPerPublisher = 30;
    std::atomic<bool> go{false};
    std::vector<std::thread> publishers;
    for (int p = 0; p < kPublishers; ++p) {
        publishers.emplace_back([&, p] {
            while (!go) std::this_thread::yield();
            for (u64 i = 0; i < kPerPublisher; ++i) {
                presenter->Publish(MakeFrame(p * 1000 + i));
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }
    std::atomic<bool> done{false};
    std::thread toggler([&] {
        while (!go) std::this_thread::yield();
        bool flip = false;
        u32 round = 0;
        // Until the publishers are done and a change of size or mode has been acted on (the first flip changes nothing:
        // it is the state the swapchain was made for), so the check on swapchain_creations does not depend on timing.
        const auto give_up = Clock::now() + std::chrono::seconds(30);
        while (!done || (presenter->GetStats().swapchain_creations < 2 && Clock::now() < give_up)) {
            flip = !flip;
            presenter->SetVsync(flip);
            presenter->Resize(flip ? 160 : 192, flip ? 120 : 128);
            presenter->RequestRedraw();
            presenter->CurrentLayout();
            presenter->GetStats();
            layout.Set(round++ % 2 == 0 ? sl::Mode::Stacked : sl::Mode::Side);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    });
    go = true;
    for (std::thread& thread : publishers) thread.join();
    done = true;
    toggler.join();
    REQUIRE(WaitFor([&] {
        const sc::Stats s = presenter->GetStats();
        return s.published == kPublishers * kPerPublisher;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    presenter->Stop();
    presenter->SetUiLayer(nullptr);

    const sc::Stats stats = presenter->GetStats();
    CHECK(stats.published == kPublishers * kPerPublisher);
    // every published frame was shown or counted dropped; one may still sit in the mailbox when Stop discards it
    const u64 shown = stats.presented - stats.redraws;
    const u64 unaccounted = stats.published - shown - stats.dropped;
    CHECK(shown + stats.dropped <= stats.published);
    CHECK(unaccounted == 0);  // the frame Stop discards from the mailbox is counted as dropped
    CHECK(stats.swapchain_creations >= 2);
    CHECK(rig->breaches.Total() == 0);
}

// Ends the process when a case that could deadlock does not finish: a deadlocked thread cannot be cleaned up, and a test
// that hangs says nothing.
struct Watchdog {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    std::thread thread;
    Watchdog(const char* what, int seconds) {
        thread = std::thread([this, what, seconds] {
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return finished; })) {
                std::fprintf(stderr, "swapchain_test: watchdog: \"%s\" did not finish within %d s (a deadlock)\n", what, seconds);
                std::fflush(stderr);
                std::_Exit(3);
            }
        });
    }
    ~Watchdog() {
        {
            std::lock_guard lock(mutex);
            finished = true;
        }
        cv.notify_all();
        thread.join();
    }
};

// A layer that calls into the presenter from inside Record, on the present thread.
struct ReentrantLayer final : pr::UiLayer {
    enum class Action { Stop, ClearLayer };
    Action action = Action::Stop;
    bool wait_for_other_thread = false;  // Stop: wait until another thread is in Stop (then give it time to take its locks)
    std::atomic<sc::Presenter*> presenter{nullptr};
    std::atomic<bool> other_thread_in_stop{false};
    std::atomic<int> entered{0};
    std::atomic<int> returned{0};  // calls into the presenter that came back
    void Record(void*, u32, u32) override {
        if (entered++ != 0) {
            return;  // only the first composition calls
        }
        if (wait_for_other_thread) {
            const auto deadline = Clock::now() + std::chrono::seconds(20);
            while (!other_thread_in_stop && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));  // it is in Stop now, waiting for this thread
        }
        if (action == Action::Stop) {
            presenter.load()->Stop();
        } else {
            presenter.load()->SetUiLayer(nullptr);
        }
        ++returned;
    }
};

TEST_CASE("presenter/stop_and_set_ui_layer_called_on_the_present_thread_do_not_wait_for_it") {
    Watchdog watchdog("stop_and_set_ui_layer_called_on_the_present_thread", 120);
    SUBCASE("Stop from the layer alone only requests the stop") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout);
        ReentrantLayer layer;
        layer.presenter = presenter.get();
        presenter->SetUiLayer(&layer);
        presenter->Publish(MakeFrame(0));
        REQUIRE(WaitFor([&] { return layer.returned.load() >= 1; }));
        CHECK(presenter->StopRequested());
        presenter->Stop();  // the join: from this thread
        CHECK(presenter->GetStats().composed == 1);
        presenter->Publish(MakeFrame(1));
        CHECK(presenter->GetStats().dropped == 1);
        CHECK(rig->breaches.Total() == 0);
    }
    SUBCASE("Stop from the layer while another thread is inside Stop, joining") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout);
        ReentrantLayer layer;
        layer.wait_for_other_thread = true;
        layer.presenter = presenter.get();
        presenter->SetUiLayer(&layer);
        presenter->Publish(MakeFrame(0));
        REQUIRE(WaitFor([&] { return layer.entered.load() >= 1; }));
        std::thread stopper([&] {
            layer.other_thread_in_stop = true;
            presenter->Stop();
        });
        REQUIRE(WaitFor([&] { return layer.returned.load() >= 1; }, 20'000));
        stopper.join();
        CHECK(presenter->StopRequested());
        CHECK(rig->breaches.Total() == 0);
    }
    SUBCASE("SetUiLayer(nullptr) from the layer does not wait for its own composition") {
        std::unique_ptr<Rig> rig = MakeRig();
        if (!rig) return;
        LayoutControl layout;
        std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout);
        ReentrantLayer layer;
        layer.action = ReentrantLayer::Action::ClearLayer;
        layer.presenter = presenter.get();
        presenter->SetUiLayer(&layer);
        PublishAndWait(*presenter, MakeFrame(0), 1);
        CHECK(layer.returned.load() == 1);
        PublishAndWait(*presenter, MakeFrame(1), 2);
        CHECK(layer.entered.load() == 1);  // it removed itself: not called again
        presenter->Stop();
        CHECK(rig->breaches.Total() == 0);
    }
}

TEST_CASE("presenter/rows_dropped_before_add_up_to_the_dropped_frames_when_taken_frames_are_discarded") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    TempDir dir("dropsum");
    LayoutControl layout;
    Seen seen;
    Gate gate;
    int calls = 0;
    seen.extra = [&] {
        if (calls++ == 0) gate.Hold();
    };
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, seen.Hooks());
    gate.presenter = presenter.get();
    const std::filesystem::path log_path = dir.path / "present.csv";
    REQUIRE(presenter->SetPresentLog(log_path.string()).ok());

    // Frame 0 is held inside its composition; while it is, the window is minimised and frames 1..3 arrive: 1 and 2 are
    // replaced in the mailbox, 3 is taken afterwards and discarded because of the minimised window.
    presenter->Publish(MakeFrame(0));
    REQUIRE(gate.WaitEntered());
    presenter->SetMinimised(true);
    presenter->Publish(MakeFrame(1));
    presenter->Publish(MakeFrame(2));
    presenter->Publish(MakeFrame(3));
    gate.Open();
    REQUIRE(WaitFor([&] { return presenter->GetStats().dropped >= 3; }));
    CHECK(presenter->GetStats().dropped == 3);

    // leaving minimised draws frame 3 again: the row carries the three drops
    presenter->SetMinimised(false);
    REQUIRE(WaitFor([&] { return presenter->GetStats().presented >= 2; }));

    // a zero-extent window: published frames are taken and discarded, or replaced, as the timing goes; the window then
    // gets its size back (a frame taken after that is shown, so how many are dropped is not fixed)
    presenter->Resize(0, 120);
    for (u64 i = 4; i <= 9; ++i) {
        presenter->Publish(MakeFrame(i));
    }
    presenter->Resize(160, 120);
    REQUIRE(WaitFor([&] {
        const sc::Stats stats = presenter->GetStats();
        return stats.presented >= 3 && stats.presented - stats.redraws + stats.dropped == 10;  // frames 0..9 all accounted for
    }));
    presenter->Stop();

    const sc::Stats stats = presenter->GetStats();
    CHECK(stats.dropped >= 3);
    const ParsedLog log = ReadLog(log_path);
    u64 sum = 0;
    for (const LogRow& row : log.rows) {
        sum += row.dropped;
    }
    CHECK(log.rows.size() == stats.presented);
    CHECK(sum == stats.dropped);
    REQUIRE(log.rows.size() >= 3);
    CHECK(log.rows[0].dropped == 0);
    CHECK(log.rows[1].dropped == 3);  // the redraw after leaving minimised
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/the_queue_lock_is_held_from_the_signalling_submission_to_the_end_of_the_present") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;

    // A thread that wants the queue lock (as the renderer's Backend::Submit does) from the moment the hook starts.
    std::atomic<bool> go{false};
    std::atomic<bool> attempting{false};
    std::atomic<bool> got{false};
    std::atomic<bool> blocked_while_hook_ran{false};
    std::atomic<int> hook_calls{0};
    std::thread contender([&] {
        while (!go) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        attempting = true;
        rhi::QueueLock lock(*rig->backend);
        got = true;
    });
    sc::TestHooks hooks;
    hooks.queue_locked = [&] {
        if (hook_calls++ != 0) {
            return;
        }
        go = true;
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (!attempting && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // time enough for it to take a free lock
        blocked_while_hook_ran = !got.load();
    };
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, std::move(hooks));
    PublishAndWait(*presenter, MakeFrame(0), 1);
    CHECK(hook_calls.load() >= 1);
    CHECK(blocked_while_hook_ran.load());             // held across the submission and the present
    CHECK(WaitFor([&] { return got.load(); }));       // and released afterwards
    contender.join();
    presenter->Stop();
    CHECK(rig->breaches.Total() == 0);
}

// Frames presented one by one while a second thread submits small recorders to the same queue as fast as it can.
void PresentWhileAnotherThreadSubmits(const sc::Options& options, u64 frames) {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    std::unique_ptr<sc::Presenter> presenter = MakePresenter(*rig, layout, {}, options);

    // The renderer's side of the shared queue: small recorders submitted as fast as the lock lets it.
    std::atomic<bool> stop{false};
    std::atomic<u64> submits{0};
    std::thread renderer([&] {
        while (!stop) {
            rhi::Recorder recorder = rig->backend->BeginRecording();
            rig->backend->Submit(std::move(recorder));
            ++submits;
        }
    });
    for (u64 i = 0; i < frames; ++i) {
        PublishAndWait(*presenter, MakeFrame(i), i + 1);
    }
    stop = true;
    renderer.join();
    presenter->Stop();

    const sc::Stats stats = presenter->GetStats();
    CHECK(stats.presented == frames);
    CHECK(stats.composed == frames);
    CHECK(submits.load() > 0);
    rig->backend->WaitIdle();
    CHECK(rig->breaches.Total() == 0);
}

TEST_CASE("presenter/another_thread_submitting_to_the_backend_while_frames_are_presented") {
    // Under the validation layer (vk_layer_run.py sets VK_INSTANCE_LAYERS) the acquire's fence is observed under the queue
    // lock: with a second thread submitting, the layer 1.3.275 races its own record of the fence wait against that
    // thread's vkQueueSubmit and reports a hazard that is not there (presentation.md). The case runs under the layer all
    // the same, and without the layer takes the production wait.
    sc::Options options;
    const char* layers = std::getenv("VK_INSTANCE_LAYERS");
    options.acquire_wait_under_queue_lock = layers != nullptr && std::string(layers).find("validation") != std::string::npos;
    PresentWhileAnotherThreadSubmits(options, 120);
}

TEST_CASE("presenter/the_acquire_fence_observed_under_the_queue_lock_presents_the_same_frames") {
    sc::Options options;
    options.acquire_wait_under_queue_lock = true;
    PresentWhileAnotherThreadSubmits(options, 60);
}

TEST_CASE("presenter/create_refuses_what_cannot_work") {
    std::unique_ptr<Rig> rig = MakeRig();
    if (!rig) return;
    LayoutControl layout;
    CHECK_FALSE(sc::Presenter::Create(nullptr, sc::Target::Offscreen(8, 8), layout.Source()).ok());
    CHECK_FALSE(sc::Presenter::Create(rig->backend, sc::Target::Offscreen(8, 8), nullptr).ok());
    // a backend made without a surface cannot present to a window
    std::shared_ptr<rhi::Backend> plain = test::RequireBackend({}, rig->instance);
    if (!plain) return;
    rhi::Result<std::unique_ptr<sc::Presenter>> result =
        sc::Presenter::Create(plain, sc::Target::Window(rig->surface->Handle(), 64, 64), layout.Source());
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().find("surface") != std::string::npos);
}

}  // namespace

#endif  // platform.h
