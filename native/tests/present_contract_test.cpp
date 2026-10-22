// mh4n/present.h: the presentation contract (N2b-26). The header comes first on purpose: it must
// compile with nothing but the standard library and mh4n/ headers.
#include "mh4n/present.h"

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace mh4n;

namespace {

// A GPU that shows its screens: Publish goes to the sinks, ReadScreen answers CPU pixels at once
// and a backend image later, from another thread.
class TestDisplayGpu final : public gpu::Gpu, public present::Display {
public:
    ~TestDisplayGpu() override {
        if (reader_.joinable()) {
            reader_.join();
        }
    }

    void Execute(const gpu::GxCommand&) override {}
    void WriteReg(u32, u32) override {}
    u32 ReadReg(u32) override {
        return 0;
    }
    void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32 screen) override {
        if (screen != 0) {
            return;
        }
        present::DisplayFrame frame;
        frame.sequence = next_sequence_++;
        frame.game_frame = static_cast<u32>(frame.sequence) + 100;
        frame.tick = 1000 + frame.sequence;
        frame.top = TopScreen();
        for (present::ScreenSink* sink : sinks_) {
            sink->Publish(frame);
        }
    }

    void AddScreenSink(present::ScreenSink* sink) override {
        sinks_.push_back(sink);
    }
    void RemoveScreenSink(present::ScreenSink* sink) override {
        std::erase(sinks_, sink);
    }
    void ReadScreen(const present::ScreenImage& screen,
                    std::function<void(present::RgbImage)> done) override {
        if (!screen.Valid()) {
            done({});
            return;
        }
        if (screen.IsCpu()) {
            // Drops the alpha, synchronously.
            present::RgbImage out;
            out.width = screen.width;
            out.height = screen.height;
            for (size_t i = 0; i + 3 < screen.pixels->size(); i += 4) {
                out.rgb.push_back((*screen.pixels)[i]);
                out.rgb.push_back((*screen.pixels)[i + 1]);
                out.rgb.push_back((*screen.pixels)[i + 2]);
            }
            done(std::move(out));
            return;
        }
        // A backend image: answered later, on another thread.
        if (reader_.joinable()) {
            reader_.join();
        }
        reader_ = std::thread([screen, done = std::move(done)] {
            present::RgbImage out;
            out.width = screen.width;
            out.height = screen.height;
            out.rgb.assign(static_cast<size_t>(out.width) * out.height * 3, 0x7F);
            done(std::move(out));
        });
    }

    static present::ScreenImage CpuScreen(u32 width, u32 height) {
        present::ScreenImage screen;
        auto pixels = std::make_shared<std::vector<u8>>(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < pixels->size(); ++i) {
            (*pixels)[i] = static_cast<u8>(i % 4 == 3 ? 0xFF : i);
        }
        screen.pixels = pixels;
        screen.width = width;
        screen.height = height;
        screen.region = {0, 0, width, height};
        return screen;
    }

    static present::ScreenImage TopScreen() {
        return CpuScreen(present::kTopWidth, present::kTopHeight);
    }

    size_t SinkCount() const {
        return sinks_.size();
    }

private:
    std::vector<present::ScreenSink*> sinks_;
    u64 next_sequence_ = 0;
    std::thread reader_;
};

class CountingSink final : public present::ScreenSink {
public:
    void Publish(const present::DisplayFrame& frame) override {
        frames.push_back(frame);
    }
    std::vector<present::DisplayFrame> frames;
};

} // namespace

// What a module does next to its GpuFactory (the factory is left out: nothing here makes a GPU by name).
MH4N_REGISTER(present::DisplayGpu, "present-test-display", {});

TEST_CASE("a Display is found with dynamic_cast on the Gpu") {
    std::unique_ptr<gpu::Gpu> gpu = std::make_unique<TestDisplayGpu>();
    present::Display* display = present::FindDisplay(gpu.get());
    REQUIRE(display != nullptr);
    CHECK(dynamic_cast<present::Display*>(gpu.get()) == display);

    // A Gpu that does not show its screens has none.
    class PlainGpu final : public gpu::Gpu {
    public:
        void Execute(const gpu::GxCommand&) override {}
        void WriteReg(u32, u32) override {}
        u32 ReadReg(u32) override {
            return 0;
        }
        void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {}
        void SetColorFill(u32) override {}
        void FlushRegion(u32, u32) override {}
        void InvalidateRegion(u32, u32) override {}
        void VBlank(u32) override {}
    } plain;
    CHECK(present::FindDisplay(&plain) == nullptr);
}

TEST_CASE("IsDisplayGpu is true for a registered name and false for another") {
    CHECK(present::IsDisplayGpu("present-test-display"));
    CHECK_FALSE(present::IsDisplayGpu("null"));
    CHECK_FALSE(present::IsDisplayGpu("present-test-display2"));
    CHECK_FALSE(present::IsDisplayGpu(""));
}

TEST_CASE("a sink receives frames only between AddScreenSink and RemoveScreenSink") {
    TestDisplayGpu gpu;
    present::Display& display = gpu;
    CountingSink sink;
    CountingSink other;

    gpu.VBlank(0); // before
    CHECK(sink.frames.empty());

    display.AddScreenSink(&sink);
    gpu.VBlank(0);
    display.AddScreenSink(&other);
    gpu.VBlank(0);
    display.RemoveScreenSink(&sink);
    gpu.VBlank(0);
    display.RemoveScreenSink(&other);
    gpu.VBlank(0); // after

    REQUIRE(sink.frames.size() == 2);
    REQUIRE(other.frames.size() == 2);
    CHECK(gpu.SinkCount() == 0);
    // The frames are what the VBlanks showed: sequence 1 and 2 for `sink`, 2 and 3 for `other`.
    CHECK(sink.frames[0].sequence == 1);
    CHECK(sink.frames[1].sequence == 2);
    CHECK(other.frames[0].sequence == 2);
    CHECK(other.frames[1].sequence == 3);
    CHECK(sink.frames[0].game_frame == 101);
    CHECK(sink.frames[0].tick == 1001);
    REQUIRE(sink.frames[0].top.has_value());
    CHECK_FALSE(sink.frames[0].bottom.has_value()); // the bottom screen is not set up
    CHECK_FALSE(sink.frames[0].gas);
    CHECK(sink.frames[0].top->Valid());
    CHECK(sink.frames[0].top->width == present::kTopWidth);

    // Removing a sink that is not there does nothing.
    display.RemoveScreenSink(&sink);
    CHECK(gpu.SinkCount() == 0);
}

TEST_CASE("ReadScreen calls done synchronously for CPU pixels") {
    TestDisplayGpu gpu;
    present::Display& display = gpu;
    const present::ScreenImage screen = TestDisplayGpu::CpuScreen(4, 2);
    REQUIRE(screen.Valid());
    REQUIRE(screen.IsCpu());

    bool called = false;
    present::RgbImage got;
    display.ReadScreen(screen, [&](present::RgbImage image) {
        called = true;
        got = std::move(image);
    });
    REQUIRE(called); // before ReadScreen returned
    CHECK(got.width == 4);
    CHECK(got.height == 2);
    REQUIRE(got.rgb.size() == 4 * 2 * 3);
    CHECK(got.rgb[0] == 0);
    CHECK(got.rgb[1] == 1);
    CHECK(got.rgb[2] == 2);
    CHECK(got.rgb[3] == 4); // the next pixel: the alpha byte (index 3) was dropped

    // A screen that is not valid gives an empty image, and done still runs.
    called = false;
    display.ReadScreen(present::ScreenImage{}, [&](present::RgbImage image) {
        called = true;
        got = std::move(image);
    });
    CHECK(called);
    CHECK(got.empty());
}

TEST_CASE("ReadScreen may call done later, on another thread") {
    TestDisplayGpu gpu;
    present::Display& display = gpu;

    present::ScreenImage screen;
    screen.image = std::make_shared<int>(7); // stands for the backend's image
    screen.width = present::kBottomWidth;
    screen.height = present::kBottomHeight;
    screen.scale = 2;
    screen.orientation = present::Orientation::Rotated90;
    screen.timeline_value = 41;
    screen.region = {0, 0, 480, 640}; // stored rotated: as wide as the screen is high, times 2
    REQUIRE(screen.Valid());
    REQUIRE(screen.IsBackendImage());

    std::atomic<bool> done_called{false};
    std::thread::id done_thread;
    present::RgbImage got;
    const std::thread::id caller = std::this_thread::get_id();
    display.ReadScreen(screen, [&](present::RgbImage image) {
        got = std::move(image);
        done_thread = std::this_thread::get_id();
        done_called = true;
    });
    // The test GPU joins its reader before it is destroyed; wait for it here.
    while (!done_called) {
        std::this_thread::yield();
    }
    CHECK(done_thread != caller);
    CHECK(got.width == present::kBottomWidth);
    CHECK(got.height == present::kBottomHeight);
    CHECK(got.rgb.size() == 320u * 240u * 3u);
}

TEST_CASE("a ScreenImage keeps its backend image alive and checks its sizes") {
    auto backend_image = std::make_shared<int>(1);
    std::weak_ptr<int> watch = backend_image;

    present::ScreenImage screen;
    screen.image = backend_image;
    backend_image.reset();
    screen.width = present::kTopWidth;
    screen.height = present::kTopHeight;
    screen.scale = 3;
    screen.orientation = present::Orientation::Rotated90;

    // The region is in the stored orientation: 240 * 3 wide, 400 * 3 high.
    CHECK(screen.StoredWidth() == 720);
    CHECK(screen.StoredHeight() == 1200);
    CHECK_FALSE(screen.Valid()); // no region yet
    screen.region = {16, 32, 720, 1200};
    CHECK(screen.Valid());

    present::DisplayFrame frame;
    frame.top = screen;
    screen = {};
    CHECK_FALSE(watch.expired()); // the frame's copy holds it
    frame = {};
    CHECK(watch.expired());

    // Upright: the stored size is the displayed one.
    present::ScreenImage upright = TestDisplayGpu::TopScreen();
    CHECK(upright.StoredWidth() == 400);
    CHECK(upright.StoredHeight() == 240);
    CHECK(upright.Valid());
    upright.scale = 2; // CPU pixels are at 1x
    CHECK_FALSE(upright.Valid());
    upright.scale = 1;
    upright.image = std::make_shared<int>(2); // both forms set
    CHECK_FALSE(upright.Valid());
}

TEST_CASE("a rotated image stores each displayed column as a row, bottom to top") {
    const present::PixelPos top_left =
        present::ToImagePixel(present::Orientation::Rotated90, 240, 0, 0);
    CHECK(top_left == present::PixelPos{239, 0});
    const present::PixelPos bottom_right =
        present::ToImagePixel(present::Orientation::Rotated90, 240, 399, 239);
    CHECK(bottom_right == present::PixelPos{0, 399});
    CHECK(present::ToImagePixel(present::Orientation::Upright, 240, 5, 6) ==
          present::PixelPos{5, 6});
    // At 2x the displayed height is 480 image pixels.
    CHECK(present::ToImagePixel(present::Orientation::Rotated90, 480, 10, 3) ==
          present::PixelPos{476, 10});
}

TEST_CASE("the shared backend slot is empty by default and is set and read from two threads") {
    CHECK(present::SharedBackend() == nullptr);

    auto backend = std::make_shared<int>(42);
    std::weak_ptr<int> watch = backend;

    std::atomic<bool> set_done{false};
    std::thread setter([&] {
        present::SetSharedBackend(backend);
        set_done = true;
    });
    std::shared_ptr<void> seen;
    std::thread reader([&] {
        while (!set_done) {
            std::this_thread::yield();
        }
        seen = present::SharedBackend();
    });
    setter.join();
    reader.join();

    REQUIRE(seen != nullptr);
    CHECK(seen.get() == static_cast<void*>(backend.get()));
    CHECK(*static_cast<int*>(seen.get()) == 42);

    // Many readers and writers at once do not tear the pointer: every value read is empty or
    // one of the two that were set.
    auto other = std::make_shared<int>(43);
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            while (!stop) {
                std::shared_ptr<void> got = present::SharedBackend();
                if (got != nullptr && got.get() != backend.get() && got.get() != other.get()) {
                    ++bad;
                }
            }
        });
    }
    std::thread writer([&] {
        for (int i = 0; i < 2000; ++i) {
            present::SetSharedBackend(i % 3 == 0 ? nullptr
                                                 : i % 3 == 1 ? std::shared_ptr<void>(backend)
                                                              : std::shared_ptr<void>(other));
        }
    });
    writer.join();
    stop = true;
    for (auto& t : readers) {
        t.join();
    }
    CHECK(bad == 0);

    // The owner empties the slot; nothing of the slot keeps the backend alive afterwards.
    present::SetSharedBackend({});
    CHECK(present::SharedBackend() == nullptr);
    seen.reset();
    backend.reset();
    CHECK(watch.expired());
}
