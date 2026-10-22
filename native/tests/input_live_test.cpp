// The live input source (N2b-08, D59): what Set and SetTouch put in, what Sample gives the game and
// when, the script --record-input writes (read back by the script source, frame by frame), and the
// two threads that share the state.
//
// Synthetic only: states made here, a program of this file's making, a fake frame counter.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "input_live/live_input.h"
#include "input_live/script_recorder.h"
#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

#if MH4N_HAVE_SCHEDULE
#include "input_script/script_input.h"
#include "schedule.h"
#endif

using namespace mh4n;
using namespace mh4n::input_live;

namespace {

constexpr u64 kStart = 1'000'000;  // the tick at which recorded time 0 is
constexpr u32 kFramePage = 0x01057000;

// A frame every 4,468,531 ticks (60 fps), three samples inside each.
constexpr u64 kFrameTicks = 4'468'531;
u64 FrameTick(u32 frame) {
    return kStart + 1000 + static_cast<u64>(frame) * kFrameTicks;
}
u64 TickAfter(u64 tick, double ms) {
    return tick + static_cast<u64>(ms * (static_cast<double>(kTicksPerSecond) / 1000.0));
}

InputState Buttons(u32 buttons, s16 circle_x = 0, s16 circle_y = 0) {
    InputState state;
    state.buttons = buttons;
    state.circle_x = circle_x;
    state.circle_y = circle_y;
    return state;
}

// ---- the states of the stress test ----------------------------------------------------------------------

// Every field of a state derives from one number, so a state made of two numbers' fields (a torn
// copy) is recognisable; the touch fields have a number of their own (SetTouch's).
InputState StateFor(u32 n) {
    InputState s;
    s.buttons = n;
    s.circle_x = static_cast<s16>(n);
    s.circle_y = static_cast<s16>(~n);
    s.cstick_x = static_cast<s16>(n * 3);
    s.cstick_y = static_cast<s16>(n * 5);
    s.zl = (n & 1) != 0;
    s.zr = (n & 2) != 0;
    for (int i = 0; i < 3; ++i) {
        s.accel[i] = static_cast<s16>(n + static_cast<u32>(i));
        s.gyro[i] = static_cast<s16>(n ^ static_cast<u32>(i));
    }
    // Set must ignore these: if it kept them, the touch invariant below breaks.
    s.touch_down = true;
    s.touch_x = 1;
    s.touch_y = 1;
    return s;
}

bool SameNonTouchFields(const InputState& a, const InputState& b) {
    InputState x = a, y = b;
    x.touch_down = y.touch_down = false;
    x.touch_x = y.touch_x = x.touch_y = y.touch_y = 0;
    return x == y;
}

// The touch is either up (and zero) or a point whose y is the complement of its x.
bool TouchConsistent(const InputState& s) {
    if (!s.touch_down) {
        return s.touch_x == 0 && s.touch_y == 0;
    }
    return s.touch_y == static_cast<u16>(~s.touch_x);
}

// A state as Set stores it: the fields of StateFor(n), the touch the touch tracker made.
InputState Whole(u32 n) {
    InputState s = StateFor(n);
    s.touch_down = false;
    s.touch_x = s.touch_y = 0;
    return s;
}

bool Consistent(const InputState& s) {
    return SameNonTouchFields(s, StateFor(s.buttons)) && TouchConsistent(s);
}

std::optional<TouchPoint> TouchFor(u32 j) {
    if ((j & 1) == 0) {
        return std::nullopt;
    }
    const u16 x = static_cast<u16>(j);
    return TouchPoint{x, static_cast<u16>(~x)};
}

}  // namespace

// ---- latching ---------------------------------------------------------------------------------------------

TEST_CASE("input_live: states set between two frame changes are seen only from the second") {
    LiveInput source(nullptr, kStart);
    // Before the game's frame counter has been seen, the state set last.
    CHECK(source.Sample(kStart).buttons == 0);
    source.Set(Buttons(kPadA));
    CHECK(source.Sample(kStart + 10).buttons == kPadA);
    source.Set(Buttons(kPadB));
    CHECK(source.Sample(kStart + 20).buttons == kPadB);

    // The first reading of the counter counts as a change: B becomes the latched state.
    source.OnFrame(100, kStart + 30);
    CHECK(source.Sample(kStart + 31).buttons == kPadB);
    source.Set(Buttons(kPadX, 10, -20));
    source.SetTouch(TouchPoint{5, 6});
    CHECK(source.Sample(kStart + 40) == Buttons(kPadB));  // the same, as many times as asked
    CHECK(source.Sample(kStart + 50) == Buttons(kPadB));
    source.Set(Buttons(kPadY));  // replaces X: X is never seen
    source.OnFrame(101, kStart + 60);
    const InputState second = source.Sample(kStart + 61);
    CHECK(second.buttons == kPadY);
    CHECK(second.circle_x == 0);
    CHECK(second.touch_down);  // set after the first frame change, latched with the second
    CHECK(second.touch_x == 5);
    CHECK(second.touch_y == 6);

    // A state set and replaced inside one frame is never seen at all.
    source.Set(Buttons(kPadStart));
    source.Set(Buttons(kPadY));
    source.OnFrame(102, kStart + 70);
    CHECK(source.Sample(kStart + 71).buttons == kPadY);

    // A frame change with nothing set changes nothing.
    source.OnFrame(103, kStart + 80);
    CHECK(source.Sample(kStart + 81).buttons == kPadY);
}

TEST_CASE("input_live: --input-latch none shows a change at once") {
    LiveInput source(nullptr, kStart, Latch::None);
    source.Set(Buttons(kPadA));
    CHECK(source.Sample(kStart + 1).buttons == kPadA);
    source.OnFrame(5, kStart + 2);
    source.Set(Buttons(kPadB));
    CHECK(source.Sample(kStart + 3).buttons == kPadB);
    source.SetTouch(TouchPoint{1, 2});
    CHECK(source.Sample(kStart + 4).touch_down);
    source.SetTouch(std::nullopt);
    CHECK_FALSE(source.Sample(kStart + 5).touch_down);
}

TEST_CASE("input_live: Set and SetTouch own different fields") {
    LiveInput source(nullptr, kStart, Latch::None);
    source.SetTouch(TouchPoint{160, 120});
    // A key pressed while the stylus is down never lifts it, and whatever touch the caller's state
    // carries is ignored.
    InputState keys = Buttons(kPadA, 30, 40);
    keys.cstick_x = 7;
    keys.zl = true;
    keys.accel[1] = -9;
    keys.gyro[2] = 11;
    source.Set(keys);
    InputState state = source.Sample(kStart + 1);
    CHECK(state.touch_down);
    CHECK(state.touch_x == 160);
    CHECK(state.touch_y == 120);
    CHECK(state.buttons == kPadA);
    CHECK(state.circle_x == 30);
    CHECK(state.circle_y == 40);
    CHECK(state.cstick_x == 7);
    CHECK(state.zl);
    CHECK(state.accel[1] == -9);
    CHECK(state.gyro[2] == 11);

    InputState lifted = Buttons(kPadB);
    lifted.touch_down = false;
    source.Set(lifted);
    CHECK(source.Sample(kStart + 2).touch_down);
    InputState pressed = Buttons(0);
    pressed.touch_down = true;
    pressed.touch_x = 9;
    pressed.touch_y = 9;
    source.Set(pressed);
    state = source.Sample(kStart + 3);
    CHECK(state.touch_x == 160);

    // The other way round: a touch leaves the keys alone, and lifting it zeroes its coordinates.
    source.SetTouch(TouchPoint{7, 8});
    state = source.Sample(kStart + 4);
    CHECK(state.touch_x == 7);
    CHECK(state.touch_y == 8);
    CHECK(state.buttons == 0);
    source.Set(Buttons(kPadL));
    source.SetTouch(std::nullopt);
    state = source.Sample(kStart + 5);
    CHECK(state.buttons == kPadL);
    CHECK_FALSE(state.touch_down);
    CHECK(state.touch_x == 0);
    CHECK(state.touch_y == 0);
}

// ---- the two threads ----------------------------------------------------------------------------------------

TEST_CASE("input_live: the stress test's check recognises a torn state") {
    // The check is what makes the stress test able to fail: it must refuse every way a copy can mix
    // two states, and take every state that is whole.
    CHECK(Consistent(Whole(77)));
    CHECK(Consistent([] { InputState s = StateFor(79); s.touch_down = true; s.touch_x = 12; s.touch_y = static_cast<u16>(~12); return s; }()));
    const InputState a = Whole(1000), b = Whole(2001);
    InputState torn;
    torn = a; torn.circle_y = b.circle_y; CHECK_FALSE(Consistent(torn));
    torn = a; torn.cstick_x = b.cstick_x; CHECK_FALSE(Consistent(torn));
    torn = a; torn.cstick_y = b.cstick_y; CHECK_FALSE(Consistent(torn));
    torn = a; torn.zl = !a.zl; CHECK_FALSE(Consistent(torn));
    torn = a; torn.zr = !a.zr; CHECK_FALSE(Consistent(torn));
    torn = a; torn.accel[2] = b.accel[2]; CHECK_FALSE(Consistent(torn));
    torn = a; torn.gyro[0] = b.gyro[0]; CHECK_FALSE(Consistent(torn));
    torn = a; torn.buttons = b.buttons; CHECK_FALSE(Consistent(torn));
    torn = a; torn.touch_down = true; torn.touch_x = 3; torn.touch_y = 3; CHECK_FALSE(Consistent(torn));
    torn = a; torn.touch_down = false; torn.touch_x = 3; CHECK_FALSE(Consistent(torn));
}

namespace {

// The main thread plays the scheduler thread (Sample, and OnFrame in the latched mode), a second one
// plays the app's (Set, SetTouch), 100,000 times. The asserts run on the main thread only.
void RunStress(Latch latch) {
    constexpr u32 kIterations = 100'000;
    constexpr u32 kCheckpoint = 1000;
    LiveInput source(nullptr, kStart, latch);
    std::atomic<bool> reader_ready{false};
    std::atomic<bool> writer_done{false};
    std::atomic<u32> reader_saw{0};  // the newest state number the reader has taken

    std::thread writer([&] {
        while (!reader_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (u32 i = 1; i <= kIterations; ++i) {
            source.Set(StateFor(i));
            if (i % 3 == 0) {
                source.SetTouch(TouchFor(i));
            }
            // Every 1000th state waits until the reader has taken it, so the two threads overlap
            // however loaded the machine is; the 999 writes in between run against its samples.
            if (i % kCheckpoint == 0) {
                while (reader_saw.load(std::memory_order_acquire) < i) {
                    std::this_thread::yield();
                }
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    u64 samples = 0, bad = 0, went_back = 0, distinct = 0;
    u32 last_n = 0, frame = 0;
    u64 tick = kStart;
    bool done = false;
    reader_ready.store(true, std::memory_order_release);
    while (!done) {
        done = writer_done.load(std::memory_order_acquire);  // one more round after it
        if (latch == Latch::Frame) {
            source.OnFrame(frame++, tick);
        }
        const InputState state = source.Sample(++tick);
        ++samples;
        if (state.buttons == 0) {
            // nothing set yet: the neutral state, whole
            bad += state == InputState{} ? 0 : 1;
            continue;
        }
        bad += Consistent(state) ? 0 : 1;
        went_back += state.buttons < last_n ? 1 : 0;
        distinct += state.buttons != last_n ? 1 : 0;
        last_n = state.buttons;
        reader_saw.store(state.buttons, std::memory_order_release);
    }
    writer.join();

    CHECK(bad == 0);
    CHECK(went_back == 0);  // the states of one writer arrive in order
    CHECK(last_n == kIterations);
    // The two threads really overlapped: the reader took the state of every checkpoint, and more.
    CHECK(distinct >= kIterations / kCheckpoint);
    MESSAGE("samples " << samples << ", distinct states seen " << distinct);
}

}  // namespace

TEST_CASE("input_live: Set from one thread while Sample runs on another never yields a mixed state") {
    SUBCASE("latch none") { RunStress(Latch::None); }
    SUBCASE("latch frame, with a frame change before every sample") { RunStress(Latch::Frame); }
}

// ---- the recording ---------------------------------------------------------------------------------------------

TEST_CASE("input_live: a recording is lines in the grammar of the script") {
    LiveInput source(nullptr, kStart);
    source.StartRecording();
    source.Set(Buttons(kPadA | kPadStart, 77, -154));
    source.SetTouch(TouchPoint{160, 120});
    source.OnFrame(10, FrameTick(10));
    source.Sample(FrameTick(10) + 1);
    source.OnFrame(11, FrameTick(11));
    source.Set(Buttons(kPadStart, 77, -154));  // A is released at the next frame change
    source.OnFrame(12, FrameTick(12));
    source.Set(Buttons(kPadStart));            // the circle pad too
    source.SetTouch(std::nullopt);
    source.OnFrame(14, FrameTick(14));         // the counter skipped 13
    const std::string text = source.FinishRecording(FrameTick(20));

    // Header lines are comments; one line per press, in order of start.
    std::istringstream in(text);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] != '#') {
            lines.push_back(line);
        }
    }
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "f10:A:f2");
    CHECK(lines[1] == "f10:START:f5");  // still held when the run ends: through the last frame seen, 14
    CHECK(lines[2] == "f10:CIRCLE:0.5:-1:f4");
    CHECK(lines[3] == "f10:TOUCH:160:120:f4");
}

TEST_CASE("input_live: a frame counter that goes backwards writes a hold of one frame, not billions") {
    ScriptRecorder recorder;
    Stamp at;
    at.have_frame = true;
    at.frame = 100;
    recorder.Update(Buttons(kPadA), at);
    at.frame = 50;  // e.g. the game restarted its counter
    recorder.Update(Buttons(0), at);
    const std::string text = recorder.Finish(at);
    CHECK(text.find("f100:A:f1\n") != std::string::npos);
}

TEST_CASE("input_live: a program without a frame counter is recorded in milliseconds") {
    LiveInput source(nullptr, kStart);
    source.StartRecording();
    // Samples at 100 ms and every 10 ms after: A is pressed from 120 ms to 150 ms.
    auto at = [](double ms) { return TickAfter(kStart, ms); };
    source.Sample(at(100));
    source.Set(Buttons(kPadA));
    source.Sample(at(120));
    source.Sample(at(130));
    source.Set(Buttons(0));
    source.Sample(at(150));
    source.SetTouch(TouchPoint{10, 20});
    source.Sample(at(160));
    const std::string text = source.FinishRecording(at(200));
    // (a tick is a whole number, so the times are the ones asked for to a few nanoseconds)
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] != '#') {
            lines.push_back(line);
        }
    }
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find(":A:") != std::string::npos);
    CHECK(std::strtod(lines[0].c_str(), nullptr) == doctest::Approx(120.0).epsilon(1e-6));
    CHECK(std::strtod(lines[0].c_str() + lines[0].rfind(':') + 1, nullptr) == doctest::Approx(30.0).epsilon(1e-6));
    CHECK(lines[1].find(":TOUCH:10:20:") != std::string::npos);
    CHECK(std::strtod(lines[1].c_str(), nullptr) == doctest::Approx(160.0).epsilon(1e-6));
    CHECK(std::strtod(lines[1].c_str() + lines[1].rfind(':') + 1, nullptr) == doctest::Approx(40.0).epsilon(1e-6));
    CHECK(text.find("f1") == std::string::npos);  // no frame keys
    CHECK(text.find(":f") == std::string::npos);  // no frame holds
}

TEST_CASE("input_live: the numbers of the script read back exactly") {
    for (const double ms : {0.0, 1.0, 100.25, 3600000.5, 1.0 / 3.0, 0.001, 2.0e6 + 1.0e-5}) {
        CHECK(std::strtod(FormatMs(ms).c_str(), nullptr) == ms);
    }
    CHECK(FormatMs(1500.0) == "1500");
    CHECK(FormatMs(0.5) == "0.5");
}

#if MH4N_HAVE_SCHEDULE

namespace {

Schedule::Plan ParsePlan(const std::string& text) {
    Schedule::Plan plan;
    std::string error;
    REQUIRE_MESSAGE(Schedule::ParseInputText(text, plan, error), error << "\n" << text);
    return plan;
}

// One sample of the live source: when, the frame counter then, what it gave.
struct Taken {
    u64 tick;
    u32 frame;
    InputState state;
};

std::string Describe(const InputState& s) {
    std::ostringstream out;
    out << "buttons=" << s.buttons << " circle=" << s.circle_x << "," << s.circle_y << " touch=" << s.touch_down
        << ":" << s.touch_x << "," << s.touch_y;
    return out.str();
}

// Feeds the script the recording is, with the frame counter of each sample, and checks every sample.
void ExpectReplay(const std::string& text, const std::vector<Taken>& taken) {
    input_script::ScriptInput replay(ParsePlan(text), nullptr, kStart);
    for (const Taken& t : taken) {
        *replay.FrameCell() = t.frame;
        const InputState got = replay.Sample(t.tick);
        CHECK_MESSAGE(got == t.state, "frame " << t.frame << " tick " << t.tick << ": live gave [" << Describe(t.state)
                                               << "], the replay [" << Describe(got) << "]\n" << text);
    }
}

}  // namespace

TEST_CASE("input_live: a recorded session replays the same Sample at every frame") {
    LiveInput source(nullptr, kStart);
    source.StartRecording();
    std::vector<Taken> taken;

    InputState everything = Buttons(kPadA | kPadB | kPadX | kPadY | kPadUp | kPadDown | kPadLeft | kPadRight | kPadL |
                                    kPadR | kPadStart | kPadSelect);
    u32 frame = 0;
    for (u32 step = 0; step < 90; ++step) {
        // what the app does between two frames
        switch (step) {
        case 2: source.Set(Buttons(kPadA)); break;
        case 4: source.Set(Buttons(kPadA | kPadB)); break;   // overlapping holds
        case 6: source.Set(Buttons(kPadB)); break;
        case 7: source.Set(Buttons(0)); break;
        case 9: source.SetTouch(TouchPoint{100, 50}); break;
        case 11: source.SetTouch(TouchPoint{101, 50}); break;  // the stylus moves
        case 12: source.Set(Buttons(kPadX)); break;            // a key while the stylus is down
        case 14: source.SetTouch(std::nullopt); break;
        case 16: source.Set(Buttons(kPadX, 100, -77)); break;
        case 17: source.Set(Buttons(0, 100, -77)); break;
        case 19: source.Set(Buttons(0, -154, 154)); break;
        case 21: source.Set(Buttons(0)); break;
        case 23: source.Set(Buttons(kPadA)); source.Set(Buttons(0)); break;  // never seen
        case 25: source.Set(everything); break;
        case 28: source.Set(Buttons(kPadUp, 1, 1)); source.SetTouch(TouchPoint{319, 239}); break;
        case 30: source.SetTouch(TouchPoint{0, 0}); break;
        case 32: source.SetTouch(std::nullopt); source.Set(Buttons(0)); break;
        case 34: source.Set(Buttons(kPadL)); break;  // held across the frames the counter skips
        case 44: source.Set(Buttons(kPadL | kPadR)); break;
        case 46: source.Set(Buttons(0)); break;
        default: break;
        }
        // three samples before the frame change: they see the previous frame's state
        for (int k = 1; k <= 3; ++k) {
            const u64 tick = FrameTick(frame) + static_cast<u64>(k) * 1000 + kFrameTicks / 2;
            taken.push_back({tick, frame, source.Sample(tick)});
        }
        frame += (step == 36 || step == 37) ? 3 : 1;  // the counter skips frames
        source.OnFrame(frame, FrameTick(frame));
        for (int k = 1; k <= 3; ++k) {
            const u64 tick = FrameTick(frame) + static_cast<u64>(k) * 1000;
            taken.push_back({tick, frame, source.Sample(tick)});
        }
    }
    const std::string text = source.FinishRecording(FrameTick(frame) + 5000);
    ExpectReplay(text, taken);

    // The sequence did what it was written to do: the samples are not all the same.
    bool saw_overlap = false, saw_circle = false, saw_touch_and_key = false, saw_all = false;
    for (const Taken& t : taken) {
        saw_overlap = saw_overlap || t.state.buttons == (kPadA | kPadB);
        saw_circle = saw_circle || (t.state.circle_x == -154 && t.state.circle_y == 154);
        saw_touch_and_key = saw_touch_and_key || (t.state.touch_down && t.state.buttons == kPadX);
        saw_all = saw_all || t.state.buttons == everything.buttons;
    }
    CHECK(saw_overlap);
    CHECK(saw_circle);
    CHECK(saw_touch_and_key);
    CHECK(saw_all);
}

TEST_CASE("input_live: a session in milliseconds, and one that finds the counter half way, replay too") {
    LiveInput source(nullptr, kStart);
    source.StartRecording();
    std::vector<Taken> taken;
    auto sample = [&](double ms, u32 frame) { taken.push_back({TickAfter(kStart, ms), frame, source.Sample(TickAfter(kStart, ms))}); };

    // no counter yet
    sample(10, 0);
    source.Set(Buttons(kPadA, 50, 0));
    sample(10.123456789, 0);
    sample(40.5, 0);
    source.SetTouch(TouchPoint{33, 44});
    sample(60, 0);
    // the counter appears (frame 7) while A, the circle pad and the stylus are held
    source.OnFrame(7, TickAfter(kStart, 70));
    sample(70, 7);
    source.Set(Buttons(kPadB));  // latched at the next frame
    sample(75, 7);
    source.OnFrame(8, TickAfter(kStart, 80));
    sample(80, 8);
    sample(90, 8);
    source.SetTouch(std::nullopt);
    source.OnFrame(9, TickAfter(kStart, 100));
    sample(100, 9);
    sample(110, 9);
    const std::string text = source.FinishRecording(TickAfter(kStart, 120));
    CHECK(text.find("f8:B:") != std::string::npos);                // started after the counter: frames
    const std::size_t a_at = text.find(":A:");
    REQUIRE(a_at != std::string::npos);
    const std::size_t a_line = text.rfind('\n', a_at) + 1;
    CHECK(text.compare(a_line, 6, "10.123") == 0);  // started before it: milliseconds (10.123456789 ms of game time, to a tick)
    ExpectReplay(text, taken);
}

TEST_CASE("input_live: --input-latch none is recorded on the frame it was seen in") {
    LiveInput source(nullptr, kStart, Latch::None);
    source.StartRecording();
    std::vector<Taken> taken;
    source.OnFrame(3, FrameTick(3));
    source.Set(Buttons(kPadA));
    taken.push_back({FrameTick(3) + 10, 3, source.Sample(FrameTick(3) + 10)});
    source.OnFrame(5, FrameTick(5));
    source.Set(Buttons(0));
    taken.push_back({FrameTick(5) + 10, 5, source.Sample(FrameTick(5) + 10)});
    taken.push_back({FrameTick(6) + 10, 6, source.Sample(FrameTick(6) + 10)});
    const std::string text = source.FinishRecording(FrameTick(7));
    CHECK(text.find("f3:A:f2\n") != std::string::npos);
    ExpectReplay(text, taken);
}

TEST_CASE("input_live: every button name is one the script reads, and means the same bit") {
    struct Named { u32 bit; const char* name; };
    const Named names[] = {{kPadA, "A"}, {kPadB, "B"}, {kPadX, "X"}, {kPadY, "Y"}, {kPadUp, "UP"}, {kPadDown, "DOWN"},
                           {kPadLeft, "LEFT"}, {kPadRight, "RIGHT"}, {kPadL, "L"}, {kPadR, "R"},
                           {kPadStart, "START"}, {kPadSelect, "SELECT"}};
    for (const Named& named : names) {
        LiveInput source(nullptr, kStart);
        source.StartRecording();
        source.Set(Buttons(named.bit));
        source.OnFrame(1, FrameTick(1));
        source.Set(Buttons(0));
        source.OnFrame(3, FrameTick(3));
        const std::string text = source.FinishRecording(FrameTick(4));
        CHECK_MESSAGE(text.find(std::string("f1:") + named.name + ":f2\n") != std::string::npos, text);
        input_script::ScriptInput replay(ParsePlan(text), nullptr, kStart);
        *replay.FrameCell() = 1;
        CHECK_MESSAGE(replay.Sample(FrameTick(1)).buttons == named.bit, named.name);
        *replay.FrameCell() = 3;
        CHECK(replay.Sample(FrameTick(3)).buttons == 0);
    }
}

TEST_CASE("input_live: every circle pad reading and every touch pixel reads back through the script") {
    // The circle pad: the whole range of the HID readings.
    for (int raw = -32768; raw <= 32767; ++raw) {
        const int other = static_cast<s16>(raw * 3);
        const std::string line = "f0:CIRCLE:" + FormatCircleAxis(raw) + ":" + FormatCircleAxis(other) + ":f1";
        input_script::ScriptInput replay(ParsePlan(line), nullptr, kStart);
        *replay.FrameCell() = 0;
        const InputState got = replay.Sample(kStart);
        if (got.circle_x != raw || got.circle_y != other) {
            FAIL("circle " << raw << "," << other << " came back as " << got.circle_x << "," << got.circle_y);
        }
    }
    // The touch screen: the script's float round trip loses a pixel for some values; the recorder
    // writes those as a fraction. Count them, so the test shows the fraction was needed.
    int fractions = 0;
    for (int i = 0; i < 320; ++i) {
        const int x = i, y = (i * 7) % 240;
        const std::string fx = FormatTouchAxis(x, 320), fy = FormatTouchAxis(y, 240);
        fractions += fx.find('.') != std::string::npos ? 1 : 0;
        input_script::ScriptInput replay(ParsePlan("f0:TOUCH:" + fx + ":" + fy + ":f1"), nullptr, kStart);
        *replay.FrameCell() = 0;
        const InputState got = replay.Sample(kStart);
        if (!got.touch_down || got.touch_x != x || got.touch_y != y) {
            FAIL("touch " << x << "," << y << " came back as " << got.touch_x << "," << got.touch_y);
        }
    }
    for (int j = 0; j < 240; ++j) {
        const int x = (j * 3) % 320, y = j;
        const std::string fx = FormatTouchAxis(x, 320), fy = FormatTouchAxis(y, 240);
        fractions += fy.find('.') != std::string::npos ? 1 : 0;
        input_script::ScriptInput replay(ParsePlan("f0:TOUCH:" + fx + ":" + fy + ":f1"), nullptr, kStart);
        *replay.FrameCell() = 0;
        const InputState got = replay.Sample(kStart);
        if (!got.touch_down || got.touch_x != x || got.touch_y != y) {
            FAIL("touch " << x << "," << y << " came back as " << got.touch_x << "," << got.touch_y);
        }
    }
    MESSAGE("touch values written as a fraction: " << fractions);
}

#endif  // MH4N_HAVE_SCHEDULE

// ---- in a runtime -----------------------------------------------------------------------------------------------

namespace {

struct TempFile {
    std::filesystem::path path;
    TempFile() {
        static std::atomic<int> counter{0};
        path = std::filesystem::temp_directory_path() /
               ("mh4n_input_live_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(counter++) + ".txt");
    }
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    std::string Read() const {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
};

struct RuntimeFixture {
    ManualTimeSource* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    std::vector<std::pair<LogLevel, std::string>> logged;
    RuntimeFixture() {
        auto source = std::make_unique<ManualTimeSource>();
        time = source.get();
        RuntimeConfig config;
        config.time = std::move(source);
        runtime = std::make_unique<Runtime>(std::move(config));
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { logged.emplace_back(r.level, std::string(r.message)); });
    }
    ~RuntimeFixture() {
        runtime.reset();
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    bool Prepare(const std::vector<std::string>& args) {
        const Options options = ParseOptions("runner", args);
        REQUIRE_MESSAGE(options.ok(), options.error());
        return runtime->Prepare(options, {Phase::Devices, Phase::Services});
    }
    bool Logged(LogLevel level, const std::string& needle) const {
        for (const auto& [l, message] : logged) {
            if (l == level && message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    void MapFrameCounter() {
        REQUIRE(IsSuccess(runtime->Memory().Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
    }
    void ShowFrame(u32 frame) {
        REQUIRE(runtime->Memory().Write<u32>(kFrameCounterAddress, frame));
        runtime->PollFrame();
    }
    // Moves the game clock `ms` forward.
    u64 Advance(double ms) {
        time->Advance(static_cast<s64>(ms * 1e6));
        return runtime->Clock().Now();
    }
};

}  // namespace

TEST_CASE("input_live: --input-source live is found through the runtime and follows its frame counter") {
    RuntimeFixture f;
    REQUIRE(f.Prepare({"--input-source", "live"}));
    const std::shared_ptr<LiveInput> live = Find(*f.runtime);
    REQUIRE(live != nullptr);
    CHECK(live->GetLatch() == Latch::Frame);
    CHECK_FALSE(live->Recording());
    InputSource* input = f.runtime->Interfaces().input;
    REQUIRE(input != nullptr);

    // The frame counter is not mapped (a program that never maps it): the state set last.
    live->Set(Buttons(kPadA));
    CHECK(input->Sample(f.runtime->Clock().Now()).buttons == kPadA);
    live->Set(Buttons(kPadB));
    CHECK(input->Sample(f.Advance(1)).buttons == kPadB);

    // Once it is mapped, Sample looks at the counter itself, and its first reading counts as a change.
    f.MapFrameCounter();
    live->Set(Buttons(kPadX));
    CHECK(input->Sample(f.Advance(1)).buttons == kPadX);
    live->Set(Buttons(kPadY));
    CHECK(input->Sample(f.Advance(1)).buttons == kPadX);  // the counter has not moved
    f.ShowFrame(1);
    CHECK(input->Sample(f.Advance(1)).buttons == kPadY);
    live->Set(Buttons(kPadR));
    live->SetTouch(TouchPoint{3, 4});
    CHECK(input->Sample(f.Advance(1)) == Buttons(kPadY));
    // Writing the counter is enough: Sample polls it before it answers.
    REQUIRE(f.runtime->Memory().Write<u32>(kFrameCounterAddress, 2));
    const InputState seen = input->Sample(f.Advance(1));
    CHECK(seen.buttons == kPadR);
    CHECK(seen.touch_down);
    CHECK(seen.touch_x == 3);
}

TEST_CASE("input_live: the options choose the latch and name the recording") {
    SUBCASE("--input-latch none") {
        RuntimeFixture f;
        REQUIRE(f.Prepare({"--input-source", "live", "--input-latch", "none"}));
        REQUIRE(Find(*f.runtime) != nullptr);
        CHECK(Find(*f.runtime)->GetLatch() == Latch::None);
        CHECK_FALSE(f.runtime->ExitStatus().has_value());
    }
    SUBCASE("a latch mode that is neither ends the run with status 2") {
        RuntimeFixture f;
        REQUIRE(f.Prepare({"--input-source", "live", "--input-latch", "sometimes"}));
        CHECK(f.runtime->ExitStatus() == 2);
        CHECK(f.Logged(LogLevel::Error, "--input-latch"));
    }
    SUBCASE("a recording that cannot be written ends the run with status 2 before it starts") {
        RuntimeFixture f;
        const auto missing = std::filesystem::temp_directory_path() / "mh4n_input_live_no_such_dir" / "x.txt";
        REQUIRE(f.Prepare({"--input-source", "live", "--record-input", missing.string()}));
        CHECK(f.runtime->ExitStatus() == 2);
        CHECK(f.Logged(LogLevel::Error, "--record-input"));
    }
    SUBCASE("the options of another input source are said not to apply") {
        RuntimeFixture f;
        TempFile file;
        REQUIRE(f.Prepare({"--record-input", file.path.string()}));
        CHECK(f.Logged(LogLevel::Warning, "only apply to --input-source live"));
        CHECK_FALSE(f.runtime->ExitStatus().has_value());
    }
    SUBCASE("none, without the options, says nothing") {
        RuntimeFixture f;
        REQUIRE(f.Prepare({"--input-source", "live"}));
        CHECK_FALSE(f.Logged(LogLevel::Warning, "only apply"));
    }
}

TEST_CASE("input_live: --record-input writes the script when the run ends") {
    TempFile file;
    {
        RuntimeFixture f;
        REQUIRE(f.Prepare({"--input-source", "live", "--record-input", file.path.string()}));
        REQUIRE_FALSE(f.runtime->ExitStatus().has_value());
        const std::shared_ptr<LiveInput> live = Find(*f.runtime);
        REQUIRE(live != nullptr);
        CHECK(live->Recording());
        InputSource* input = f.runtime->Interfaces().input;
        f.MapFrameCounter();
        f.ShowFrame(10);
        live->Set(Buttons(kPadA));
        live->SetTouch(TouchPoint{200, 100});
        input->Sample(f.Advance(5));
        f.ShowFrame(11);
        input->Sample(f.Advance(5));
        live->Set(Buttons(0));
        f.ShowFrame(13);
        input->Sample(f.Advance(5));
        f.ShowFrame(14);
        f.ShowFrame(15);
        // the file is written when the run is over, not before
        CHECK(file.Read().empty());
    }  // the runtime ends here
    const std::string text = file.Read();
    CHECK(text.find("f11:A:f2\n") != std::string::npos);
    CHECK(text.find("f11:TOUCH:200:100:f5\n") != std::string::npos);  // held through the last frame seen, 15
    CHECK(text[0] == '#');
}
