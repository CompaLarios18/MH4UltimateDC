// Derived from Azahar src/core/hle/service/hid/hid.cpp and src/core/3ds.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later version"):
// the circle pad's limit (0x9A) and its rounding, and the bottom screen's size that the touch
// coordinates are divided and multiplied by.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.cpp)
// Copyright 2017 Citra Emulator Project (3ds.h)
//
// The scripted input source (N1-16), registered as `script`, the runtime's default input source.
// See the header next to this file.
#include "script_input.h"

#include <cmath>
#include <memory>

#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::input_script {

namespace {
constexpr const char* kLog = "Input.Script";
}  // namespace

MH4N_OPTION("runner", "--input", "SCRIPT",
            "scripted input: comma-separated presses WHEN:BUTTON[:HOLD], WHEN:TOUCH:x:y[:HOLD], "
            "WHEN:CIRCLE:x:y[:HOLD], e.g. 3000:A,5000:TOUCH:160:120 (WHEN and HOLD are ms of game "
            "time or fN frames; repeatable)");
MH4N_OPTION("runner", "--input-file", "PATH",
            "the same presses, one per line; '#' starts a comment (repeatable)");

#if MH4N_HAVE_SCHEDULE

namespace {

// The bounds and units of the HID shared memory (docs/native/wp/N1.md, N1-16).
constexpr int kMaxCirclePadPosition = 0x9A;  // Azahar's calibrated limit of the circle pad
constexpr int kBottomScreenWidth = 320;      // Core::kScreenBottomWidth / Height
constexpr int kBottomScreenHeight = 240;

// The button names of port/src/schedule.cpp (Schedule::ButtonName) and the pad bit each sets, in
// the same order: A B X Y UP DOWN LEFT RIGHT L R START SELECT ZL ZR HOME. 0: not a bit of the pad
// word (ZL and ZR have their own fields, HOME is not a button of HID).
constexpr u32 kButtonBits[Schedule::kButtonCount] = {
    kPadA,  kPadB,    kPadX,     kPadY, kPadUp, kPadDown, kPadLeft, kPadRight,
    kPadL,  kPadR,    kPadStart, kPadSelect, 0, 0, 0};
constexpr int kZl = 12;
constexpr int kZr = 13;

}  // namespace

ScriptInput::ScriptInput(Schedule::Plan plan, Runtime* runtime, u64 start_tick)
    : plan_(std::move(plan)), runtime_(runtime), start_tick_(start_tick) {}

InputState ScriptInput::Sample(u64 tick) {
    // The frame counter as it is now: a look at it here, not the value the last safe point saw.
    if (runtime_ != nullptr) {
        runtime_->PollFrame();
    }
    // The reference's EmulatedMs(): ticks since the program started over the ticks of a ms.
    const double now_ms = static_cast<double>(tick - start_tick_) /
                          (static_cast<double>(kTicksPerSecond) / 1000.0);
    const u32 frame = *frame_;

    InputState state;
    // A button is held while the first of its presses that is active says so; the reference asks
    // each button of the pad for its presses in turn and stops at the first one held, so a later
    // press of the same button does not start (remember its moment) before that.
    bool found[Schedule::kButtonCount] = {};
    bool circle_found = false;
    bool touch_found = false;
    for (Schedule::Press& press : plan_.presses) {
        switch (press.kind) {
        case Schedule::PressKind::Button: {
            // HOME is not read by anything.
            if (press.button == Schedule::kButtonCount - 1 || found[press.button]) {
                break;
            }
            if (press.Active(now_ms, frame)) {
                found[press.button] = true;
                state.buttons |= kButtonBits[press.button];
                state.zl = state.zl || press.button == kZl;
                state.zr = state.zr || press.button == kZr;
            }
            break;
        }
        case Schedule::PressKind::Circle:
            if (!circle_found && press.Active(now_ms, frame)) {
                circle_found = true;
                // Rounded, as the hardware does; the HID module averages the readings.
                state.circle_x = static_cast<s16>(std::roundf(press.x * kMaxCirclePadPosition));
                state.circle_y = static_cast<s16>(std::roundf(press.y * kMaxCirclePadPosition));
            }
            break;
        case Schedule::PressKind::Touch:
            if (!touch_found && press.Active(now_ms, frame)) {
                touch_found = true;
                state.touch_down = true;
                // The reference's touch device gives x / 320 and y / 240 as floats and the HID
                // module multiplies them back and truncates: keep the float round trip, which
                // loses a pixel for some values.
                const float x = press.x / static_cast<float>(kBottomScreenWidth);
                const float y = press.y / static_cast<float>(kBottomScreenHeight);
                state.touch_x = static_cast<u16>(x * static_cast<float>(kBottomScreenWidth));
                state.touch_y = static_cast<u16>(y * static_cast<float>(kBottomScreenHeight));
            }
            break;
        }
    }
    return state;
}

bool ParseScripts(const std::vector<std::string>& inputs, const std::vector<std::string>& files,
                  Schedule::Plan* plan, std::string* error) {
    for (const std::string& script : inputs) {
        if (!Schedule::ParseInput(script, *plan, *error)) {
            *error = "--input: " + *error;
            return false;
        }
    }
    for (const std::string& path : files) {
        if (!Schedule::ParseInputFile(path, *plan, *error)) {
            *error = "--input-file " + path + ": " + *error;
            return false;
        }
    }
    return true;
}

namespace {

std::unique_ptr<InputSource> MakeScriptInput(Runtime& rt) {
    Schedule::Plan plan;
    std::string error;
    if (!ParseScripts(rt.GetOptions().All("--input"), rt.GetOptions().All("--input-file"), &plan,
                      &error)) {
        MH4N_LOG(Error, kLog, "{}", error);
        rt.RequestExit(2, error);
        plan = Schedule::Plan{};
    }
    // Script time 0 is where the game clock stands when the devices are made: the clock is paused
    // from the end of the setup to the start of the run.
    auto source = std::make_unique<ScriptInput>(std::move(plan), &rt, rt.Clock().Now());
    // The frame keys of the script follow the game's counter (Runtime::OnFrame); Sample also looks
    // at the counter itself, so a press keyed on a frame starts at the first update after it.
    rt.OnFrame([cell = source->FrameCell()](u32 frame, u64) { *cell = frame; });
    return source;
}

MH4N_REGISTER(InputSourceFactory, "script", InputSourceFactory(MakeScriptInput));

}  // namespace

#else  // no port/src/schedule.cpp around native/

namespace {

std::unique_ptr<InputSource> MakeScriptInput(Runtime& rt) {
    if (rt.GetOptions().Has("--input") || rt.GetOptions().Has("--input-file")) {
        const std::string message =
            "--input and --input-file need port/src/schedule.cpp, which is not around native/";
        MH4N_LOG(Error, kLog, "{}", message);
        rt.RequestExit(2, message);
    }
    struct Neutral final : InputSource {
        InputState Sample(u64) override { return {}; }
    };
    return std::make_unique<Neutral>();
}

MH4N_REGISTER(InputSourceFactory, "script", InputSourceFactory(MakeScriptInput));

}  // namespace

#endif

}  // namespace mh4n::input_script
