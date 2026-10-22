#include "input_live/live_input.h"

#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::input_live {

namespace {

constexpr const char* kLog = "Input.Live";

// Live sources by runtime, for Find. An entry goes with its source.
struct Entry {
    LiveInput* source;
    std::weak_ptr<LiveInput> handle;
};
std::mutex g_registry_mutex;
std::map<const Runtime*, Entry>& Registry() {
    static std::map<const Runtime*, Entry> registry;
    return registry;
}

}  // namespace

MH4N_OPTION("runner", "--input-latch", "MODE",
            "live input (--input-source live): frame (default) = the game sees a change only at "
            "its next frame, so the session replays; none = at once");
MH4N_OPTION("runner", "--record-input", "FILE",
            "live input: write what the game saw as a script that --input-file replays (frame "
            "keys; ms of game time before the frame counter is seen), when the run ends");

LiveInput::LiveInput(Runtime* runtime, u64 start_tick, Latch latch)
    : runtime_(runtime), start_tick_(start_tick), latch_(latch) {}

LiveInput::~LiveInput() {
    std::lock_guard lock(g_registry_mutex);
    auto& registry = Registry();
    for (auto it = registry.begin(); it != registry.end();) {
        it = it->second.source == this ? registry.erase(it) : std::next(it);
    }
}

void LiveInput::Set(const InputState& state) {
    std::lock_guard lock(mutex_);
    InputState next = state;
    next.touch_down = pending_.touch_down;
    next.touch_x = pending_.touch_x;
    next.touch_y = pending_.touch_y;
    pending_ = next;
}

void LiveInput::SetTouch(std::optional<TouchPoint> touch) {
    std::lock_guard lock(mutex_);
    pending_.touch_down = touch.has_value();
    pending_.touch_x = touch ? touch->x : 0;
    pending_.touch_y = touch ? touch->y : 0;
}

InputState LiveInput::Snapshot() const {
    std::lock_guard lock(mutex_);
    return pending_;
}

Stamp LiveInput::StampAt(u64 tick) const {
    Stamp at;
    at.have_frame = frame_seen_;
    at.frame = frame_;
    // The script source's own formula (ScriptInput::Sample), so a recorded time reads back exactly.
    at.ms = static_cast<double>(tick - start_tick_) / (static_cast<double>(kTicksPerSecond) / 1000.0);
    return at;
}

void LiveInput::OnFrame(u32 frame, u64 tick) {
    frame_seen_ = true;
    frame_ = frame;
    if (latch_ == Latch::Frame) {
        latched_ = Snapshot();
        if (recording_) {
            recorder_.Update(latched_, StampAt(tick));
        }
    }
}

InputState LiveInput::Sample(u64 tick) {
    // The frame counter as it is now, as the script source looks at it: a change is latched
    // before this sample is answered.
    if (runtime_ != nullptr) {
        runtime_->PollFrame();
    }
    const InputState state = (latch_ == Latch::Frame && frame_seen_) ? latched_ : Snapshot();
    if (recording_) {
        recorder_.Update(state, StampAt(tick));
    }
    return state;
}

void LiveInput::StartRecording() {
    recording_ = true;
}

std::string LiveInput::FinishRecording(u64 tick) {
    recording_ = false;
    return recorder_.Finish(StampAt(tick));
}

std::shared_ptr<LiveInput> Find(Runtime& runtime) {
    std::lock_guard lock(g_registry_mutex);
    const auto it = Registry().find(&runtime);
    return it == Registry().end() ? nullptr : it->second.handle.lock();
}

namespace {

// What the runtime owns: it forwards to the shared source, which Find hands out.
class Forwarder final : public InputSource {
public:
    explicit Forwarder(std::shared_ptr<LiveInput> source) : source_(std::move(source)) {}
    InputState Sample(u64 tick) override { return source_->Sample(tick); }

private:
    std::shared_ptr<LiveInput> source_;
};

std::unique_ptr<InputSource> MakeLiveInput(Runtime& rt) {
    const Options& options = rt.GetOptions();

    Latch latch = Latch::Frame;
    const std::string mode = options.GetString("--input-latch", "frame");
    if (mode == "none") {
        latch = Latch::None;
    } else if (mode != "frame") {
        const std::string message = "--input-latch: '" + mode + "' is not frame or none";
        MH4N_LOG(Error, kLog, "{}", message);
        rt.RequestExit(2, message);
    }

    // Recorded time 0 is where the game clock stands when the devices are made (as the script source's).
    auto source = std::make_shared<LiveInput>(&rt, rt.Clock().Now(), latch);
    rt.OnFrame([source](u32 frame, u64 tick) { source->OnFrame(frame, tick); });

    if (const std::optional<std::string> path = options.Get("--record-input")) {
        // Fail now, not when the run is over, on a file that cannot be written.
        if (!std::ofstream(*path, std::ios::binary | std::ios::trunc)) {
            const std::string message = "--record-input: cannot write " + *path;
            MH4N_LOG(Error, kLog, "{}", message);
            rt.RequestExit(2, message);
        } else {
            source->StartRecording();
            rt.OnFinish([source, &rt, file = *path] {
                const std::string text = source->FinishRecording(rt.Clock().Now());
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << text;
                out.flush();
                if (!out) {
                    MH4N_LOG(Error, kLog, "--record-input: cannot write {}", file);
                } else {
                    MH4N_LOG(Info, kLog, "recorded input written to {}", file);
                }
            });
        }
    }

    {
        std::lock_guard lock(g_registry_mutex);
        Registry()[&rt] = Entry{source.get(), source};
    }
    return std::make_unique<Forwarder>(std::move(source));
}

MH4N_REGISTER(InputSourceFactory, "live", InputSourceFactory(MakeLiveInput));

// Options that only the live source reads, given to another one, would do nothing without a word.
void WarnUnusedOptions(Runtime& rt) {
    const Options& options = rt.GetOptions();
    if ((options.Has("--record-input") || options.Has("--input-latch")) && Find(rt) == nullptr) {
        MH4N_LOG(Warning, kLog, "--record-input and --input-latch only apply to --input-source live");
    }
}

MH4N_REGISTER(BootStep, "input_live.warn", BootStep{Phase::Services, WarnUnusedOptions});

}  // namespace

}  // namespace mh4n::input_live
