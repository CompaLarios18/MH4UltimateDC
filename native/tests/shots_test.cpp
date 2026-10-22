// shots_test.cpp - N2b-14.
//
// The native runner's screenshots: the schedule (frame-keyed and millisecond) on a fake frame counter
// and clock, the file names, gas.txt, and a `done` called inside ReadScreen or from another thread. The
// last cases go through the runtime with the `null-display` GPU. Synthetic data only; no game files.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/present.h"
#include "mh4n/runtime.h"
#include "shots/shots.h"

#if MH4N_HAVE_SCHEDULE

using namespace mh4n;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("mh4n_shots_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& Path() const { return path_; }
    std::string Str() const { return path_.string(); }

private:
    fs::path path_;
};

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::set<std::string> Files(const fs::path& dir) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        names.insert(entry.path().filename().string());
    }
    return names;
}

Schedule::ShotSchedule MakeSchedule(const std::string& from, const std::string& every) {
    Schedule::Plan plan;
    std::string error;
    if (!from.empty()) {
        REQUIRE_MESSAGE(Schedule::ParseShotsFrom(from, plan, error), error);
    }
    if (!every.empty()) {
        REQUIRE_MESSAGE(Schedule::ParseShotEvery(every, plan, error), error);
    }
    return Schedule::ShotSchedule(plan);
}

// A screen of CPU pixels: every pixel (r, g, b, 255).
present::ScreenImage Screen(u32 width, u32 height, u8 r, u8 g, u8 b) {
    present::ScreenImage s;
    s.width = width;
    s.height = height;
    auto pixels = std::make_shared<std::vector<u8>>(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        (*pixels)[4 * i + 0] = r;
        (*pixels)[4 * i + 1] = g;
        (*pixels)[4 * i + 2] = b;
        (*pixels)[4 * i + 3] = 255;
    }
    s.pixels = std::move(pixels);
    s.region = {0, 0, width, height};
    return s;
}

// How a fake Display answers ReadScreen.
enum class Answer { Now, OtherThread, Never };

// Reads CPU pixels (alpha dropped), the way null-display does, but answers as asked.
class FakeDisplay final : public present::Display {
public:
    Answer answer = Answer::Now;
    std::atomic<bool> release{true};  // OtherThread: the callbacks wait until this is true
    std::atomic<int> reads{0};
    std::vector<std::thread> threads;
    std::vector<std::thread::id> callback_threads;
    std::mutex mutex;

    ~FakeDisplay() override {
        for (auto& t : threads) {
            t.join();
        }
    }
    void AddScreenSink(present::ScreenSink*) override {}
    void RemoveScreenSink(present::ScreenSink*) override {}
    void ReadScreen(const present::ScreenImage& screen, std::function<void(present::RgbImage)> done) override {
        ++reads;
        present::RgbImage out;
        if (screen.Valid() && screen.IsCpu()) {
            out.width = screen.width;
            out.height = screen.height;
            for (std::size_t i = 0; i < static_cast<std::size_t>(screen.width) * screen.height; ++i) {
                out.rgb.insert(out.rgb.end(), screen.pixels->begin() + 4 * i, screen.pixels->begin() + 4 * i + 3);
            }
        }
        switch (answer) {
        case Answer::Now: {
            {
                std::lock_guard lock(mutex);
                callback_threads.push_back(std::this_thread::get_id());
            }
            done(std::move(out));
            break;
        }
        case Answer::OtherThread: {
            std::lock_guard lock(mutex);
            threads.emplace_back([this, done = std::move(done), out = std::move(out)]() mutable {
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                {
                    std::lock_guard inner(mutex);
                    callback_threads.push_back(std::this_thread::get_id());
                }
                done(std::move(out));
            });
            break;
        }
        case Answer::Never:
            break;
        }
    }
};

present::DisplayFrame Frame(u32 game_frame, double ms, bool gas = false, u8 shade = 1) {
    present::DisplayFrame f;
    f.game_frame = game_frame;
    // Rounded up, so that the milliseconds computed back from the tick are not below `ms`.
    f.tick = static_cast<u64>(std::ceil(ms * (static_cast<double>(kTicksPerSecond) / 1000.0)));
    f.gas = gas;
    f.top = Screen(400, 240, shade, 2, 3);
    f.bottom = Screen(320, 240, 4, shade, 6);
    return f;
}

// One frame per 1/60 s, the frame counter starting at `first_frame`, until `last_frame`.
void RunFrames(shots::ShotSink& sink, u32 first_frame, u32 last_frame,
               const std::function<bool(u32)>& gas = [](u32) { return false; }) {
    for (u32 frame = first_frame; frame <= last_frame; ++frame) {
        sink.Publish(Frame(frame, 1000.0 * frame / 60.0, gas(frame)));
    }
}

}  // namespace

TEST_CASE("shots: the PPM file is P6 with the size and the pixels") {
    TempDir dir;
    present::RgbImage image;
    image.width = 2;
    image.height = 1;
    image.rgb = {1, 2, 3, 250, 251, 252};
    REQUIRE(shots::WritePpm(dir.Path() / "a.ppm", image));
    CHECK(ReadFile(dir.Path() / "a.ppm") == std::string("P6\n2 1\n255\n") + std::string("\x01\x02\x03\xFA\xFB\xFC", 6));
    CHECK_FALSE(shots::WritePpm(dir.Path() / "missing" / "a.ppm", image));
}

TEST_CASE("shots: frame-keyed shots are named by the frame, every N frames from M") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("f100", "f60"), dir.Path(), display);
    RunFrames(sink, 1, 300);
    CHECK(sink.Finish(std::chrono::seconds(5)));
    CHECK(Files(dir.Path()) ==
          std::set<std::string>{"f0000100_bottom.ppm", "f0000100_top.ppm", "f0000160_bottom.ppm", "f0000160_top.ppm",
                                "f0000220_bottom.ppm", "f0000220_top.ppm", "f0000280_bottom.ppm", "f0000280_top.ppm",
                                "gas.txt"});
    CHECK(sink.ShotsTaken() == 4);
    CHECK(sink.FilesWritten() == 8);
    const std::string top = ReadFile(dir.Path() / "f0000100_top.ppm");
    CHECK(top.rfind("P6\n400 240\n255\n", 0) == 0);
    CHECK(top.size() == 15 + 400 * 240 * 3);
    CHECK(ReadFile(dir.Path() / "f0000100_bottom.ppm").rfind("P6\n320 240\n255\n", 0) == 0);
}

TEST_CASE("shots: --shot-every fN alone starts at frame N") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("", "f30"), dir.Path(), display);
    RunFrames(sink, 1, 70);
    sink.Finish(std::chrono::seconds(5));
    CHECK(Files(dir.Path()) == std::set<std::string>{"f0000030_bottom.ppm", "f0000030_top.ppm", "f0000060_bottom.ppm",
                                                     "f0000060_top.ppm", "gas.txt"});
}

TEST_CASE("shots: a frame the counter skipped has no shot, and a shot is taken at the first frame at or past it") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("f10", "f10"), dir.Path(), display);
    // The counter reads 9, then 12 (f10 is taken at 12), then 31 (f20 is taken at 31, named f20, and f30,
    // which the counter went past, has no shot), then 40.
    for (const u32 frame : {9u, 12u, 31u, 40u}) {
        sink.Publish(Frame(frame, 10.0 * frame));
    }
    sink.Finish(std::chrono::seconds(5));
    CHECK(Files(dir.Path()) == std::set<std::string>{"f0000010_bottom.ppm", "f0000010_top.ppm", "f0000020_bottom.ppm",
                                                     "f0000020_top.ppm", "f0000040_bottom.ppm", "f0000040_top.ppm",
                                                     "gas.txt"});
}

TEST_CASE("shots: millisecond shots default to every 1000 ms, named by the scheduled time") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("", ""), dir.Path(), display);
    RunFrames(sink, 1, 190);  // 3.17 s
    sink.Finish(std::chrono::seconds(5));
    CHECK(Files(dir.Path()) == std::set<std::string>{"0001000_bottom.ppm", "0001000_top.ppm", "0002000_bottom.ppm",
                                                     "0002000_top.ppm", "0003000_bottom.ppm", "0003000_top.ppm",
                                                     "gas.txt"});
}

TEST_CASE("shots: --shots-from and --shot-every in milliseconds") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("500", "500"), dir.Path(), display);
    RunFrames(sink, 1, 70);  // 1.17 s
    sink.Finish(std::chrono::seconds(5));
    CHECK(Files(dir.Path()) == std::set<std::string>{"0000500_bottom.ppm", "0000500_top.ppm", "0001000_bottom.ppm",
                                                     "0001000_top.ppm", "gas.txt"});
}

TEST_CASE("shots: the game clock decides, not the number of frames") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("", "250"), dir.Path(), display);
    // 249.9 ms: not yet; 250.0 ms is due (the reference's `now_ms >= next`).
    sink.Publish(Frame(0, 249.9));
    CHECK(sink.ShotsTaken() == 0);
    sink.Publish(Frame(0, 250.0));
    CHECK(sink.ShotsTaken() == 1);
    sink.Publish(Frame(0, 400.0));
    CHECK(sink.ShotsTaken() == 1);
    sink.Publish(Frame(0, 800.0));
    CHECK(sink.ShotsTaken() == 2);
    sink.Finish(std::chrono::seconds(5));
    CHECK(Files(dir.Path()).count("0000250_top.ppm") == 1);
    CHECK(Files(dir.Path()).count("0000500_top.ppm") == 1);
}

TEST_CASE("shots: a frame-keyed first shot with millisecond shots after it") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("f30", "400"), dir.Path(), display);
    RunFrames(sink, 1, 120);  // 2 s; frame 30 is at 500 ms
    sink.Finish(std::chrono::seconds(5));
    // Taken at frame 30 (500 ms), the next one an interval after it: 900 ms, then 1300, 1700.
    CHECK(Files(dir.Path()) ==
          std::set<std::string>{"f0000030_bottom.ppm", "f0000030_top.ppm", "0000900_bottom.ppm", "0000900_top.ppm",
                                "0001300_bottom.ppm", "0001300_top.ppm", "0001700_bottom.ppm", "0001700_top.ppm",
                                "gas.txt"});
}

TEST_CASE("shots: gas.txt lists the shots that follow a gas frame since the previous shot") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("f60", "f60"), dir.Path(), display);
    // Gas at 70 (between shots 60 and 120, so 120 is listed), none before 180 except at 180 itself
    // (the frame the shot is taken from counts), none between 180 and 240, and at 241 (so 300 is listed).
    RunFrames(sink, 1, 300, [](u32 f) { return f == 70 || f == 180 || f == 241; });
    sink.Finish(std::chrono::seconds(5));
    CHECK(ReadFile(dir.Path() / "gas.txt") == "120\n180\n300\n");
}

TEST_CASE("shots: gas.txt is there and empty when there was no gas, and counts milliseconds for ms shots") {
    {
        TempDir dir;
        FakeDisplay display;
        shots::ShotSink sink(MakeSchedule("", "f10"), dir.Path(), display);
        RunFrames(sink, 1, 30);
        sink.Finish(std::chrono::seconds(5));
        REQUIRE(fs::exists(dir.Path() / "gas.txt"));
        CHECK(ReadFile(dir.Path() / "gas.txt").empty());
    }
    {
        TempDir dir;
        FakeDisplay display;
        shots::ShotSink sink(MakeSchedule("", "1000"), dir.Path(), display);
        RunFrames(sink, 1, 130, [](u32 f) { return f == 100; });
        sink.Finish(std::chrono::seconds(5));
        CHECK(ReadFile(dir.Path() / "gas.txt") == "2000\n");
    }
}

TEST_CASE("shots: a done called from another thread is waited for by Finish") {
    TempDir dir;
    FakeDisplay display;
    display.answer = Answer::OtherThread;
    display.release = false;
    {
        shots::ShotSink sink(MakeSchedule("f10", "f10"), dir.Path(), display);
        RunFrames(sink, 1, 25, [](u32 f) { return f == 12; });
        // Publish returned and nothing is written: the callbacks wait for the gate.
        CHECK(sink.FilesWritten() == 0);
        CHECK_FALSE(sink.Finish(std::chrono::milliseconds(20)));
        display.release = true;
        CHECK(sink.Finish(std::chrono::seconds(5)));
        CHECK(sink.FilesWritten() == 4);
    }
    CHECK(Files(dir.Path()) == std::set<std::string>{"f0000010_bottom.ppm", "f0000010_top.ppm", "f0000020_bottom.ppm",
                                                     "f0000020_top.ppm", "gas.txt"});
    CHECK(ReadFile(dir.Path() / "gas.txt") == "20\n");
    REQUIRE(display.callback_threads.size() == 4);
    for (const auto& id : display.callback_threads) {
        CHECK(id != std::this_thread::get_id());
    }
}

TEST_CASE("shots: a done called inside ReadScreen does not wait for anything") {
    TempDir dir;
    FakeDisplay display;
    shots::ShotSink sink(MakeSchedule("f10", "f10"), dir.Path(), display);
    RunFrames(sink, 1, 10);
    // Written before Publish returned, on the publishing thread.
    CHECK(sink.FilesWritten() == 2);
    CHECK(fs::exists(dir.Path() / "f0000010_top.ppm"));
    for (const auto& id : display.callback_threads) {
        CHECK(id == std::this_thread::get_id());
    }
}

TEST_CASE("shots: a callback that never comes does not hang Finish, and a late one is harmless") {
    TempDir dir;
    FakeDisplay display;
    display.answer = Answer::Never;
    SetLogFilter(LogFilter(LogLevel::Critical));
    shots::ShotSink sink(MakeSchedule("f1", "f1"), dir.Path(), display);
    sink.Publish(Frame(1, 20.0));
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(sink.Finish(std::chrono::milliseconds(50)));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(fs::exists(dir.Path() / "gas.txt"));
    SetLogFilter(LogFilter(LogLevel::Info));
}

TEST_CASE("shots: a screen that is off or unreadable has no file; the other screen is still written") {
    TempDir dir;
    FakeDisplay display;
    SetLogFilter(LogFilter(LogLevel::Critical));
    shots::ShotSink sink(MakeSchedule("f1", "f1"), dir.Path(), display);
    present::DisplayFrame frame = Frame(1, 20.0);
    frame.bottom.reset();
    sink.Publish(frame);
    frame = Frame(2, 40.0);
    frame.top = present::ScreenImage{};  // not Valid(): ReadScreen answers with an empty image
    sink.Publish(frame);
    sink.Finish(std::chrono::seconds(5));
    SetLogFilter(LogFilter(LogLevel::Info));
    CHECK(Files(dir.Path()) == std::set<std::string>{"f0000001_top.ppm", "f0000002_bottom.ppm", "gas.txt"});
    CHECK(sink.EmptyScreens() == 2);
}

// ---- through the runtime ----------------------------------------------------------------------

namespace {

struct RuntimeFixture {
    ManualTimeSource* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    Options options;
    bool prepared = false;

    explicit RuntimeFixture(const std::vector<std::string>& args) {
        auto source = std::make_unique<ManualTimeSource>();
        time = source.get();
        RuntimeConfig config;
        config.time = std::move(source);
        runtime = std::make_unique<Runtime>(std::move(config));
        SetLogFilter(LogFilter(LogLevel::Critical));
        options = ParseOptions("runner", args);
        REQUIRE_MESSAGE(options.ok(), options.error());
        prepared = runtime->Prepare(options, {Phase::Devices, Phase::Services, Phase::Tracing});
    }
    ~RuntimeFixture() {
        runtime.reset();
        SetLogFilter(LogFilter(LogLevel::Info));
    }
};

}  // namespace

TEST_CASE("shots: the options are registered in the runner scope") {
    const std::string usage = Usage("runner");
    CHECK(usage.find("--shots ") != std::string::npos);
    CHECK(usage.find("--shot-every ") != std::string::npos);
    CHECK(usage.find("--shots-from ") != std::string::npos);
    CHECK(ParseOptions("runner", std::vector<std::string>{"--shots", "d", "--shot-every", "f60", "--shots-from", "500"}).ok());
}

TEST_CASE("shots: --shots without a GPU that shows its screens ends the run with status 2") {
    TempDir dir;
    RuntimeFixture f({"--shots", (dir.Path() / "shots").string()});  // the default GPU is `null`
    REQUIRE(f.prepared);
    REQUIRE(f.runtime->ExitStatus().has_value());
    CHECK(*f.runtime->ExitStatus() == 2);
    CHECK_FALSE(fs::exists(dir.Path() / "shots"));
}

TEST_CASE("shots: a bad schedule ends the run with status 2, with or without --shots") {
    TempDir dir;
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"--gpu", "null-display", "--shots", dir.Str(), "--shot-every", "f0"},
          std::vector<std::string>{"--gpu", "null-display", "--shots", dir.Str(), "--shots-from", "soon"},
          std::vector<std::string>{"--gpu", "null-display", "--shot-every", "often"}}) {
        RuntimeFixture f(args);
        REQUIRE(f.runtime->ExitStatus().has_value());
        CHECK(*f.runtime->ExitStatus() == 2);
    }
}

TEST_CASE("shots: no --shots, no sink: the run is as before") {
    RuntimeFixture f({"--gpu", "null-display", "--shot-every", "500"});
    REQUIRE(f.prepared);
    CHECK_FALSE(f.runtime->ExitStatus().has_value());
}

TEST_CASE("shots: under null-display the shots are written as the GPU publishes, and the folder is made") {
    TempDir dir;
    const std::string shots = (dir.Path() / "a" / "b").string();
    {
        RuntimeFixture f({"--gpu", "null-display", "--shots", shots, "--shots-from", "100", "--shot-every", "100"});
        REQUIRE(f.prepared);
        REQUIRE_FALSE(f.runtime->ExitStatus().has_value());
        REQUIRE(fs::is_directory(shots));
        gpu::Gpu* gpu = f.runtime->Interfaces().gpu;
        REQUIRE(gpu != nullptr);
        // The top screen's default framebuffer is black; the clock moves by hand.
        for (int i = 0; i < 5; ++i) {
            f.time->Advance(61'000'000);  // 61 ms
            gpu->VBlank(0);
        }
        // 61, 122, 183, 244, 305 ms: the shots at 100, 200 and 300 ms.
        CHECK(Files(shots) == std::set<std::string>{"0000100_bottom.ppm", "0000100_top.ppm", "0000200_bottom.ppm",
                                                    "0000200_top.ppm", "0000300_bottom.ppm", "0000300_top.ppm"});
        const std::string top = ReadFile(fs::path(shots) / "0000100_top.ppm");
        CHECK(top.rfind("P6\n400 240\n255\n", 0) == 0);
        CHECK(top.size() == 15 + 400 * 240 * 3);
    }
    // The runtime's end writes gas.txt.
    CHECK(fs::exists(fs::path(shots) / "gas.txt"));
    CHECK(ReadFile(fs::path(shots) / "gas.txt").empty());
}

#else  // MH4N_HAVE_SCHEDULE

TEST_CASE("shots: needs port/src/schedule.cpp") {
    CHECK(true);
}

#endif
