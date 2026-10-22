#include "shots/shots.h"

#if MH4N_HAVE_SCHEDULE

#include <fstream>
#include <utility>

#include "../common/utf8.h"
#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"

namespace mh4n::shots {

namespace {

constexpr const char* kLog = "Shots";

// The number in a name stem: "f0000300" is 300, "0001500" is 1500.
std::string StemNumber(const std::string& stem) {
    std::size_t i = stem.empty() || stem[0] != 'f' ? 0 : 1;
    while (i + 1 < stem.size() && stem[i] == '0') {
        ++i;
    }
    return stem.substr(i);
}

}  // namespace

bool WritePpm(const std::filesystem::path& path, const present::RgbImage& image) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
    out.flush();
    return static_cast<bool>(out);
}

ShotSink::ShotSink(const Schedule::ShotSchedule& schedule, std::filesystem::path dir, present::Display& display)
    : schedule_(schedule), display_(display), state_(std::make_shared<State>()) {
    state_->dir = std::move(dir);
}

void ShotSink::Publish(const present::DisplayFrame& frame) {
    gas_since_last_shot_ = gas_since_last_shot_ || frame.gas;
    // The reference's EmulatedMs is its ticks over the ARM11 clock; the game clock counts the same ticks.
    const double ms = static_cast<double>(frame.tick) / (static_cast<double>(kTicksPerSecond) / 1000.0);
    std::string stem;
    if (!schedule_.Due(ms, frame.game_frame, stem)) {
        return;
    }
    ++shots_taken_;
    if (gas_since_last_shot_) {
        std::lock_guard lock(state_->mutex);
        state_->gas_shots.push_back(StemNumber(stem));
    }
    gas_since_last_shot_ = false;
    Request(stem + "_top.ppm", frame.top);
    Request(stem + "_bottom.ppm", frame.bottom);
}

void ShotSink::Request(const std::string& name, const std::optional<present::ScreenImage>& screen) {
    if (!screen) {
        std::lock_guard lock(state_->mutex);
        ++state_->empty;
        MH4N_LOG(Warning, kLog, "{}: the screen is off, no file", name);
        return;
    }
    {
        std::lock_guard lock(state_->mutex);
        ++state_->pending;
    }
    // The callback may run before ReadScreen returns, so no lock is held across the call. It owns the
    // state, not the sink: it stays valid however late it runs.
    display_.ReadScreen(*screen, [state = state_, name](present::RgbImage image) {
        bool wrote = false, empty = image.empty();
        if (empty) {
            MH4N_LOG(Warning, kLog, "{}: the screen could not be read, no file", name);
        } else if (!(wrote = WritePpm(state->dir / utf8::PathFromUtf8(name), image))) {
            MH4N_LOG(Error, kLog, "cannot write {}", (state->dir / utf8::PathFromUtf8(name)).string());
        }
        std::lock_guard lock(state->mutex);
        state->written += wrote ? 1 : 0;
        state->empty += empty ? 1 : 0;
        --state->pending;
        state->cv.notify_all();
    });
}

bool ShotSink::Finish(std::chrono::milliseconds timeout) {
    std::unique_lock lock(state_->mutex);
    const bool all = state_->cv.wait_for(lock, timeout, [this] { return state_->pending == 0; });
    if (!all) {
        MH4N_LOG(Error, kLog, "{} screens were never delivered by the GPU", state_->pending);
    }
    std::string text;
    for (const std::string& shot : state_->gas_shots) {
        text += shot + '\n';
    }
    const std::filesystem::path gas = state_->dir / "gas.txt";
    std::ofstream out(gas, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        MH4N_LOG(Error, kLog, "cannot write {}", gas.string());
    }
    return all;
}

u32 ShotSink::FilesWritten() const {
    std::lock_guard lock(state_->mutex);
    return state_->written;
}

u32 ShotSink::EmptyScreens() const {
    std::lock_guard lock(state_->mutex);
    return state_->empty;
}

// ---- the runner's options ---------------------------------------------------------------------

MH4N_OPTION("runner", "--shots", "DIR",
            "save the top and bottom screens as PPM images in DIR (needs a GPU that shows its screens: "
            "null-display, renderer); writes DIR/gas.txt too");
MH4N_OPTION("runner", "--shot-every", "MS|fN",
            "how often, in game time (default 1000); fN: every N game frames, the files named "
            "f<frame>_top.ppm and f<frame>_bottom.ppm");
MH4N_OPTION("runner", "--shots-from", "MS|fN",
            "first screenshot at this game time (default: one interval in; with --shot-every fN, frame N); "
            "fM: at game frame M");

namespace {

void StartShots(Runtime& rt) {
    const Options& options = rt.GetOptions();
    const auto fail = [&rt](const std::string& message) {
        MH4N_LOG(Error, kLog, "{}", message);
        rt.RequestExit(2, message);
    };
    // The reference parses both whether or not --shots is given.
    Schedule::Plan plan;
    std::string error;
    if (const auto from = options.Get("--shots-from"); from && !Schedule::ParseShotsFrom(*from, plan, error)) {
        return fail("bad --shots-from " + *from + " (" + error + ")");
    }
    if (const auto every = options.Get("--shot-every"); every && !Schedule::ParseShotEvery(*every, plan, error)) {
        return fail("bad --shot-every " + *every + " (" + error + ")");
    }
    const auto dir = options.Get("--shots");
    if (!dir) {
        return;
    }
    present::Display* display = present::FindDisplay(rt.Interfaces().gpu);
    if (display == nullptr) {
        return fail("--shots needs a GPU that shows its screens (--gpu null-display, or --gpu renderer)");
    }
    const std::filesystem::path path = utf8::PathFromUtf8(*dir);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec || !std::filesystem::is_directory(path)) {
        return fail("cannot create the screenshot folder " + *dir);
    }
    auto sink = std::make_shared<ShotSink>(Schedule::ShotSchedule(plan), path, *display);
    display->AddScreenSink(sink.get());
    // Before the GPU is destroyed: no more frames reach the sink, then what the GPU still owes it.
    rt.OnFinish([display, sink] {
        display->RemoveScreenSink(sink.get());
        sink->Finish(std::chrono::seconds(30));
        MH4N_LOG(Info, kLog, "{} screenshots taken, {} files written", sink->ShotsTaken(), sink->FilesWritten());
    });
}

MH4N_REGISTER(BootStep, "shots.start", BootStep{Phase::Tracing, StartShots});

}  // namespace

}  // namespace mh4n::shots

#else  // a copy of native/ without port/src/schedule.cpp

namespace mh4n::shots {
int ShotsUnavailable() { return 0; }
}  // namespace mh4n::shots

#endif
