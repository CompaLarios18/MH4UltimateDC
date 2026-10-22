// display_mem_test.cpp - N2b-07.
//
// The `null-display` GPU through the runtime's own gpu::Host: synthetic pictures written into VRAM
// (the top screen's default framebuffer, a bottom one set by SetBufferSwap), published as CPU pixels
// at VBlank(0). Synthetic data only; no game files.
#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "display_mem/display_mem.h"
#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/options.h"
#include "mh4n/present.h"
#include "mh4n/runtime.h"
#if MH4N_DISPLAY_MEM_PLATFORM
#include "platform/platform_native.h"
#endif

using namespace mh4n;

namespace {

constexpr u32 kTopDefault = 0x181E6000;     // the null GPU's default top framebuffer (physical)
constexpr u32 kFramePage = 0x01057000;

struct Pixel {
    u8 r, g, b;
    bool operator==(const Pixel&) const = default;
};

// Collects the frames.
struct Collector final : present::ScreenSink {
    std::vector<present::DisplayFrame> frames;
    void Publish(const present::DisplayFrame& frame) override { frames.push_back(frame); }
};

struct Fixture {
    ManualTimeSource* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    Fixture() {
        auto source = std::make_unique<ManualTimeSource>();
        time = source.get();
        RuntimeConfig config;
        config.time = std::move(source);
        runtime = std::make_unique<Runtime>(std::move(config));
        SetLogFilter(LogFilter(LogLevel::Error));
        const Options options = ParseOptions("runner", {"--gpu", "null-display"});
        REQUIRE_MESSAGE(options.ok(), options.error());
        REQUIRE(runtime->Prepare(options, {Phase::Devices, Phase::Services}));
        display = present::FindDisplay(runtime->Interfaces().gpu);
        REQUIRE(display != nullptr);
    }
    ~Fixture() {
        runtime.reset();
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    gpu::Gpu& Gpu() { return *runtime->Interfaces().gpu; }
    u8* Vram(u32 paddr) { return runtime->Memory().Base() + kVramVaddr + (paddr - kVramPaddr); }

    // Stores one RGB8 pixel (bytes B, G, R) of a framebuffer at `paddr`, `index` pixels in.
    void PutRgb8(u32 paddr, u32 index, Pixel p) {
        u8* at = Vram(paddr) + 3 * index;
        at[0] = p.b;
        at[1] = p.g;
        at[2] = p.r;
    }
    // The pixel at displayed (column, row) of a 240-row screen is stored at c * 240 + 240 - r.
    static u32 IndexOf(u32 column, u32 row) { return column * 240 + 240 - row; }

    // One VBlank(0) with a sink attached; the frame it published.
    present::DisplayFrame Frame() {
        Collector sink;
        display->AddScreenSink(&sink);
        Gpu().VBlank(0);
        display->RemoveScreenSink(&sink);
        REQUIRE(sink.frames.size() == 1);
        return sink.frames[0];
    }
    static Pixel At(const present::ScreenImage& s, u32 x, u32 y) {
        const u8* p = s.pixels->data() + 4 * (static_cast<std::size_t>(y) * s.width + x);
        CHECK(p[3] == 0xFF);
        return {p[0], p[1], p[2]};
    }

    present::Display* display = nullptr;
};

}  // namespace

TEST_CASE("display_mem: null-display is a registered GPU, a display GPU, and null is not") {
    CHECK(present::IsDisplayGpu("null-display"));
    CHECK_FALSE(present::IsDisplayGpu("null"));
    CHECK(Registry<gpu::GpuFactory>::Instance().Find("null-display") != nullptr);
}

TEST_CASE("display_mem: the top screen at the initial registers shows its VRAM framebuffer, upright") {
    Fixture f;
    f.PutRgb8(kTopDefault, Fixture::IndexOf(0, 1), {10, 20, 30});      // column 0, row 1
    f.PutRgb8(kTopDefault, Fixture::IndexOf(399, 239), {1, 2, 3});     // right edge, last row
    f.PutRgb8(kTopDefault, Fixture::IndexOf(5, 100), {200, 100, 50});  // column 5, row 100
    const present::DisplayFrame frame = f.Frame();
    REQUIRE(frame.top.has_value());
    const present::ScreenImage& top = *frame.top;
    CHECK(top.Valid());
    CHECK(top.IsCpu());
    CHECK(top.width == 400);
    CHECK(top.height == 240);
    CHECK(Fixture::At(top, 0, 1) == Pixel{10, 20, 30});
    CHECK(Fixture::At(top, 399, 239) == Pixel{1, 2, 3});
    CHECK(Fixture::At(top, 5, 100) == Pixel{200, 100, 50});
    CHECK(Fixture::At(top, 6, 100) == Pixel{0, 0, 0});
    // The bottom screen's defaults are in VRAM too, and blank.
    REQUIRE(frame.bottom.has_value());
    CHECK(frame.bottom->width == 320);
    CHECK(frame.bottom->height == 240);
    CHECK(frame.sequence == 0);
}

TEST_CASE("display_mem: a SetBufferSwap moves the bottom screen to an RGB8 buffer elsewhere") {
    Fixture f;
    constexpr u32 kNewPaddr = kVramPaddr + 0x300000;
    f.PutRgb8(kNewPaddr, Fixture::IndexOf(7, 50), {9, 8, 7});
    gpu::FrameBufferInfo info{};
    info.active_fb = 1;
    info.address_left = kVramVaddr + 0x300000;
    info.address_right = kVramVaddr + 0x300000;
    info.stride = 3 * 240;
    info.format = 1;  // RGB8
    info.shown_fb = 1;
    f.Gpu().SetBufferSwap(1, info);
    const present::DisplayFrame frame = f.Frame();
    REQUIRE(frame.bottom.has_value());
    CHECK(frame.bottom->width == 320);
    CHECK(Fixture::At(*frame.bottom, 7, 50) == Pixel{9, 8, 7});
    // The top screen was not touched.
    REQUIRE(frame.top.has_value());
    CHECK(Fixture::At(*frame.top, 7, 50) == Pixel{0, 0, 0});
}

TEST_CASE("display_mem: another pixel format and a register write change what is shown") {
    Fixture f;
    // RGB565 on the top screen: swap in the same address with stride 2 * 240.
    u8* at = f.Vram(kTopDefault) + 2 * Fixture::IndexOf(3, 10);
    const u16 pixel = static_cast<u16>((31u << 11) | (0u << 5) | 31u);  // R 31, G 0, B 31
    at[0] = static_cast<u8>(pixel);
    at[1] = static_cast<u8>(pixel >> 8);
    gpu::FrameBufferInfo info{};
    info.active_fb = 0;
    info.address_left = kVramVaddr + (kTopDefault - kVramPaddr);
    info.address_right = info.address_left;
    info.stride = 2 * 240;
    info.format = 2;  // RGB565
    info.shown_fb = 0;
    f.Gpu().SetBufferSwap(0, info);
    CHECK(Fixture::At(*f.Frame().top, 3, 10) == Pixel{255, 0, 255});

    // WriteReg on the first buffer's address register: a picture at the bottom screen's default
    // address now shows on the top screen. (Word 0x1A of the top framebuffer block, physical address.)
    constexpr u32 kBottomDefault = 0x1848F000;
    f.PutRgb8(kBottomDefault, Fixture::IndexOf(2, 2), {77, 66, 55});
    info.stride = 3 * 240;
    info.format = 1;
    f.Gpu().SetBufferSwap(0, info);
    f.Gpu().WriteReg(0x1EF00400 + 4 * 0x1A, kBottomDefault);
    CHECK(f.Gpu().ReadReg(0x1EF00400 + 4 * 0x1A) == kBottomDefault);  // the inner GPU saw it
    CHECK(Fixture::At(*f.Frame().top, 2, 2) == Pixel{77, 66, 55});
}

TEST_CASE("display_mem: a colour fill shows the fill, and ends when the fill is switched off") {
    Fixture f;
    f.PutRgb8(kTopDefault, Fixture::IndexOf(1, 1), {5, 5, 5});
    f.Gpu().SetColorFill((1u << 24) | (0x33u << 16) | (0x22u << 8) | 0x11u);  // enable, B 33, G 22, R 11
    present::DisplayFrame frame = f.Frame();
    REQUIRE(frame.top.has_value());
    REQUIRE(frame.bottom.has_value());
    for (const present::ScreenImage* screen : {&*frame.top, &*frame.bottom}) {
        CHECK(Fixture::At(*screen, 0, 0) == Pixel{0x11, 0x22, 0x33});
        CHECK(Fixture::At(*screen, screen->width - 1, screen->height - 1) == Pixel{0x11, 0x22, 0x33});
        CHECK(Fixture::At(*screen, 1, 1) == Pixel{0x11, 0x22, 0x33});
    }
    f.Gpu().SetColorFill(0x00332211);
    CHECK(Fixture::At(*f.Frame().top, 1, 1) == Pixel{5, 5, 5});
}

TEST_CASE("display_mem: a sink gets the frames between its Add and Remove, numbered from 0") {
    Fixture f;
    Collector sink;
    Collector other;
    f.Gpu().VBlank(0);  // before any sink
    f.display->AddScreenSink(&sink);
    f.display->AddScreenSink(&other);
    f.Gpu().VBlank(0);
    f.Gpu().VBlank(1);  // the bottom screen's refresh publishes nothing
    f.Gpu().VBlank(0);
    f.display->RemoveScreenSink(&other);
    f.Gpu().VBlank(0);
    f.display->RemoveScreenSink(&sink);
    f.Gpu().VBlank(0);  // after
    REQUIRE(sink.frames.size() == 3);
    CHECK(other.frames.size() == 2);
    CHECK(sink.frames[0].sequence == 0);
    CHECK(sink.frames[1].sequence == 1);
    CHECK(sink.frames[2].sequence == 2);
}

TEST_CASE("display_mem: game_frame follows the frame counter, 0 when the program has none, tick is the clock") {
    Fixture f;
    // The null-display GPU is wrapped around the null GPU: its VBlanks still count there.
    auto* display_gpu = dynamic_cast<display_mem::DisplayMemGpu*>(&f.Gpu());
    REQUIRE(display_gpu != nullptr);
    CHECK(f.Frame().game_frame == 0);

    REQUIRE(IsSuccess(f.runtime->Memory().Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
    REQUIRE(f.runtime->Memory().Write<u32>(kFrameCounterAddress, 41));
    f.runtime->PollFrame();
    f.time->Advance(5'000'000);
    const present::DisplayFrame frame = f.Frame();
    CHECK(frame.game_frame == 41);
    CHECK(frame.tick == f.runtime->Clock().Now());
    CHECK_FALSE(frame.gas);
}

TEST_CASE("display_mem: ReadScreen gives the CPU pixels as RGB at once, and nothing for a screen it cannot read") {
    Fixture f;
    f.PutRgb8(kTopDefault, Fixture::IndexOf(4, 4), {11, 22, 33});
    const present::DisplayFrame frame = f.Frame();
    int calls = 0;
    present::RgbImage image;
    f.display->ReadScreen(*frame.top, [&](present::RgbImage result) {
        ++calls;
        image = std::move(result);
    });
    REQUIRE(calls == 1);  // before ReadScreen returned
    CHECK(image.width == 400);
    CHECK(image.height == 240);
    REQUIRE(image.rgb.size() == 400u * 240u * 3u);
    const std::size_t at = 3 * (4u * 400u + 4u);
    CHECK(image.rgb[at] == 11);
    CHECK(image.rgb[at + 1] == 22);
    CHECK(image.rgb[at + 2] == 33);

    present::ScreenImage bad;  // not Valid()
    present::RgbImage got;
    got.width = 99;
    f.display->ReadScreen(bad, [&](present::RgbImage result) { got = std::move(result); });
    CHECK(got.empty());
}

TEST_CASE("display_mem: a screen whose framebuffer is not in memory is not shown, the other still is") {
    Fixture f;
    gpu::FrameBufferInfo info{};
    info.active_fb = 0;
    info.address_left = 0x00100000;  // application memory, not an area the GPU can read
    info.address_right = 0x00100000;
    info.stride = 3 * 240;
    info.format = 1;
    info.shown_fb = 0;
    f.Gpu().SetBufferSwap(0, info);
    const present::DisplayFrame frame = f.Frame();
    CHECK_FALSE(frame.top.has_value());
    CHECK(frame.bottom.has_value());
}

#if MH4N_DISPLAY_MEM_PLATFORM
TEST_CASE("display_mem: the top screen is as wide as the hooks' rows say") {
    Fixture f;
    CHECK(Fixture::At(*f.Frame().top, 0, 0) == Pixel{0, 0, 0});
    CHECK(f.Frame().top->width == 400);
    const auto platform = platform::InstallPlatform(*f.runtime);
    REQUIRE(platform != nullptr);
    f.PutRgb8(kTopDefault, Fixture::IndexOf(500, 9), {4, 5, 6});
    platform->SetTopScreenRows(0);  // non-zero only
    CHECK(f.Frame().top->width == 400);
    platform->SetTopScreenRows(800);
    const present::DisplayFrame frame = f.Frame();
    REQUIRE(frame.top.has_value());
    CHECK(frame.top->width == 800);
    CHECK(frame.top->height == 240);
    CHECK(frame.top->Valid());
    CHECK(Fixture::At(*frame.top, 500, 9) == Pixel{4, 5, 6});
    // The bottom screen is always 320 wide.
    CHECK(frame.bottom->width == 320);
}
#endif
