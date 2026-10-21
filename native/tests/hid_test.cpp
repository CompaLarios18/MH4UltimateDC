// Derived from Azahar src/core/hle/service/hid/hid.h, src/core/hle/service/hid/hid.cpp,
// src/core/hle/service/ir/ir_user.cpp, src/core/hle/result.h and src/common/settings.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the layout of HID's shared memory and the values its updates write (the ring buffers,
// the deltas, the index-reset stamps, the circle pad averaging, the raw entries), the update
// periods, the commands of hid:USER and ir:USER with their replies and result codes, the header
// and receive buffer of ir:USER's shared memory, and the default sound volume.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.h, hid.cpp)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (ir_user.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (result.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (settings.h)
//
// HID and ir:USER (N1-16). Three parts:
//  - the pure update logic (hid_module.h) on synthetic input: layout, rings, deltas, stamps,
//    averaging, directions, touch, sensors;
//  - the scripted input source (input_script) on a script and a frame cell: time and frame keys,
//    holds, rounding, the reference's float round trip for touch;
//  - the services, through the SVC layer as a program uses them (an ELF whose main thread runs a
//    C++ function, as srv_test), on a ManualTimeSource: the cadence in exact ticks, the replies, the
//    events, the guest's view of the shared memory, a script played end to end, replay, the
//    capture that tools/native/capfmt.py reads, and ir:USER's buffer.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "input_script/script_input.h"
#include "mh4n/clock.h"
#include "mh4n/host.h"
#include "services/hid/hid_module.h"

using namespace mh4n;

namespace {

hid::SharedMem Parsed(const std::vector<u8>& memory) {
    hid::SharedMem mem;
    std::memcpy(&mem, memory.data(), sizeof(mem));
    return mem;
}

u32 Word(const std::vector<u8>& memory, u32 offset) {
    u32 value;
    std::memcpy(&value, memory.data() + offset, 4);
    return value;
}

InputState Buttons(u32 buttons, s16 circle_x = 0, s16 circle_y = 0) {
    InputState input;
    input.buttons = buttons;
    input.circle_x = circle_x;
    input.circle_y = circle_y;
    return input;
}

}  // namespace

// ---- the update periods and the layout ---------------------------------------------------------------

TEST_CASE("the update periods are Azahar's, in ticks of the 268,111,856 Hz clock") {
    // BASE_CLOCK_RATE_ARM11 / 234, / 104 and / 101, rounded down
    CHECK(hid::kPadUpdateTicks == 1145777);
    CHECK(hid::kAccelerometerUpdateTicks == 2577998);
    CHECK(hid::kGyroscopeUpdateTicks == 2654572);
    CHECK(kTicksPerSecond == 268111856);
}

TEST_CASE("the shared memory has the layout of 3dbrew and Azahar") {
    CHECK(sizeof(hid::SharedMem) == 0x238);
    CHECK(offsetof(hid::SharedMem, pad) == 0x00);
    CHECK(offsetof(hid::SharedMem, pad.index) == 0x10);
    CHECK(offsetof(hid::SharedMem, pad.sliderstate_3d) == 0x18);
    CHECK(offsetof(hid::SharedMem, pad.current_state) == 0x1C);
    CHECK(offsetof(hid::SharedMem, pad.entries) == 0x28);
    CHECK(offsetof(hid::SharedMem, touch) == 0xA8);
    CHECK(offsetof(hid::SharedMem, touch.index) == 0xA8 + 0x10);
    CHECK(offsetof(hid::SharedMem, touch.entries) == 0xA8 + 0x20);
    CHECK(offsetof(hid::SharedMem, accelerometer) == 0x108);
    CHECK(offsetof(hid::SharedMem, accelerometer.entries) == 0x108 + 0x20);
    CHECK(offsetof(hid::SharedMem, gyroscope) == 0x158);
    CHECK(offsetof(hid::SharedMem, gyroscope.entries) == 0x158 + 0x20);
    CHECK(hid::kReadableSize == 0x2B0);
}

// ---- the pad ------------------------------------------------------------------------------------------

TEST_CASE("a pad update fills the ring buffer, the index and the stamp where the game reads them") {
    std::vector<u8> memory(hid::kSharedMemorySize, 0);
    hid::Module module;

    module.UpdatePad(memory.data(), Buttons(kPadA | kPadStart), 5000);
    // index at +0x10, current state at +0x1C, entry 0 at +0x28: state, additions, removals, x, y
    CHECK(Word(memory, 0x10) == 0);
    CHECK(Word(memory, 0x1C) == (kPadA | kPadStart));
    CHECK(Word(memory, 0x28) == (kPadA | kPadStart));
    CHECK(Word(memory, 0x2C) == (kPadA | kPadStart));  // additions: everything is new
    CHECK(Word(memory, 0x30) == 0);
    // the stamp of index 0 is the tick the update was due at; the previous one was 0
    const hid::SharedMem first = Parsed(memory);
    CHECK(first.pad.index_reset_ticks == 5000);
    CHECK(first.pad.index_reset_ticks_previous == 0);
    CHECK(first.pad.sliderstate_3d == 0.0f);

    // A is released and B pressed: additions and removals are against the previous entry
    module.UpdatePad(memory.data(), Buttons(kPadB | kPadStart), 6000);
    const hid::SharedMem second = Parsed(memory);
    CHECK(second.pad.index == 1);
    CHECK(second.pad.entries[1].current_state == (kPadB | kPadStart));
    CHECK(second.pad.entries[1].delta_additions == kPadB);
    CHECK(second.pad.entries[1].delta_removals == kPadA);
    CHECK(second.pad.current_state == (kPadB | kPadStart));
    // the stamp moves only when the ring wraps to index 0
    CHECK(second.pad.index_reset_ticks == 5000);
}

TEST_CASE("the pad ring wraps after eight entries and stamps index 0 again") {
    std::vector<u8> memory(hid::kSharedMemorySize, 0);
    hid::Module module;
    for (u32 k = 0; k < 8; ++k) {
        module.UpdatePad(memory.data(), Buttons(k % 2 ? kPadX : 0), 1000 + k * 100);
        CHECK(Parsed(memory).pad.index == k);
    }
    CHECK(Parsed(memory).pad.index_reset_ticks == 1000);
    module.UpdatePad(memory.data(), Buttons(0), 2000);  // the ninth: index 0 again
    const hid::SharedMem wrapped = Parsed(memory);
    CHECK(wrapped.pad.index == 0);
    CHECK(wrapped.pad.index_reset_ticks == 2000);
    CHECK(wrapped.pad.index_reset_ticks_previous == 1000);
    // entry 0 was replaced; its delta is against entry 7 (X held) so X is a removal
    CHECK(wrapped.pad.entries[0].current_state == 0);
    CHECK(wrapped.pad.entries[0].delta_removals == kPadX);
    CHECK(wrapped.pad.entries[7].current_state == kPadX);
}

TEST_CASE("the circle pad is the average of the new reading and the previous two, truncated") {
    std::vector<u8> memory(hid::kSharedMemorySize, 0);
    hid::Module module;
    std::vector<s16> seen;
    for (int k = 0; k < 4; ++k) {
        module.UpdatePad(memory.data(), Buttons(0, 100, -100), 100 + k);
        const hid::SharedMem mem = Parsed(memory);
        seen.push_back(mem.pad.entries[mem.pad.index].circle_pad_x);
        seen.push_back(mem.pad.entries[mem.pad.index].circle_pad_y);
    }
    // (100 + 0 + 0) / 3 = 33, (100 + 100 + 0) / 3 = 66, then 100 for good; negative values truncate
    // toward zero
    CHECK(seen == std::vector<s16>{33, -33, 66, -66, 100, -100, 100, -100});
    // and the circle pad's range: a stick pushed to the end (0x9A) reads 0x9A once the average fills
    CHECK(hid::kMaxCirclePadPosition == 154);
    CHECK(hid::CirclePadRaw(1.0f) == 154);
    CHECK(hid::CirclePadRaw(-1.0f) == -154);
    CHECK(hid::CirclePadRaw(0.5f) == 77);
    CHECK(hid::CirclePadRaw(0.7f) == 108);    // 107.8
    CHECK(hid::CirclePadRaw(0.003f) == 0);    // 0.46
    CHECK(hid::CirclePadRaw(0.0035f) == 1);   // 0.539: rounded, not truncated
    CHECK(hid::CirclePadRaw(-0.0035f) == -1);
}

TEST_CASE("the circle pad's directions are bits of the pad state, from the averaged position") {
    using hid::GetStickDirectionState;
    auto bits = [](s16 x, s16 y) {
        const auto d = GetStickDirectionState(x, y);
        return std::array<bool, 4>{d.up, d.down, d.left, d.right};
    };
    using B = std::array<bool, 4>;
    CHECK(bits(0, 0) == B{false, false, false, false});
    CHECK(bits(40, 0) == B{false, false, false, false});  // the radius has to be greater than 40
    CHECK(bits(41, 0) == B{false, false, false, true});
    CHECK(bits(-41, 0) == B{false, false, true, false});
    CHECK(bits(0, 41) == B{true, false, false, false});
    CHECK(bits(0, -41) == B{false, true, false, false});
    CHECK(bits(30, 30) == B{true, false, false, true});    // between 30 and 60 degrees: both
    CHECK(bits(41, 10) == B{false, false, false, true});
    CHECK(bits(10, -41) == B{false, true, false, false});

    std::vector<u8> memory(hid::kSharedMemorySize, 0);
    hid::Module module;
    module.UpdatePad(memory.data(), Buttons(kPadA, 154, 0), 1);  // averaged: 51, right
    const u32 state = Parsed(memory).pad.entries[0].current_state;
    CHECK(state == (kPadA | hid::kPadCircleRight));
    module.UpdatePad(memory.data(), Buttons(0, 0, -154), 2);  // x: (0 + 154 + 0) / 3 = 51 still, y: -51
    CHECK(Parsed(memory).pad.entries[1].current_state ==
          (hid::kPadCircleRight | hid::kPadCircleDown));
}

// ---- touch ----------------------------------------------------------------------------------------------

TEST_CASE("the touch entry holds the pixel and the valid bits; a release is zeros") {
    std::vector<u8> memory(hid::kSharedMemorySize, 0);
    // a bit above the 7-bit field in the entry is not touched
    std::memset(memory.data() + 0xA8 + 0x20 + 8 + 4, 0x80, 1);
    hid::Module module;
    InputState down;
    down.touch_down = true;
    down.touch_x = 160;
    down.touch_y = 120;
    module.UpdatePad(memory.data(), down, 9000);
    module.UpdatePad(memory.data(), InputState{}, 9100);
    const hid::SharedMem mem = Parsed(memory);
    CHECK(mem.touch.index == 1);
    CHECK(mem.touch.entries[0].x == 160);
    CHECK(mem.touch.entries[0].y == 120);
    CHECK(mem.touch.entries[0].valid == 1);
    CHECK(mem.touch.entries[1].x == 0);
    CHECK(mem.touch.entries[1].y == 0);
    CHECK(mem.touch.entries[1].valid == 0x80);  // only bits 0-6 are the field
    // the stamp of the touch ring, like the pad's, is set when index 0 is written
    CHECK(mem.touch.index_reset_ticks == 9000);
    CHECK(mem.touch.index_reset_ticks_previous == 0);
    // seen as capfmt.py reads it: index at +0x10 of the block, entries at +0x20, 8 bytes each
    CHECK(Word(memory, 0xA8 + 0x10) == 1);
    CHECK(Word(memory, 0xA8 + 0x20) == (160u | 120u << 16));
}

// ---- the sensors ----------------------------------------------------------------------------------------

TEST_CASE("the accelerometer and gyroscope entries, their raw entries and their stamps") {
    std::vector<u8> memory(hid::kSharedMemorySize, 0);
    hid::Module module;
    InputState input;
    input.accel[0] = 10;
    input.accel[1] = -20;
    input.accel[2] = 30;
    input.gyro[0] = 5;
    input.gyro[1] = 6;
    input.gyro[2] = -7;
    module.UpdateAccelerometer(memory.data(), input, 7000);
    module.UpdateGyroscope(memory.data(), input, 7500);
    hid::SharedMem mem = Parsed(memory);
    CHECK(mem.accelerometer.index == 0);
    CHECK(mem.accelerometer.entries[0].x == 10);
    CHECK(mem.accelerometer.entries[0].y == -20);
    CHECK(mem.accelerometer.entries[0].z == 30);
    CHECK(mem.accelerometer.raw_entry.x == -20);  // -2 x
    CHECK(mem.accelerometer.raw_entry.z == -40);  // 2 y
    CHECK(mem.accelerometer.raw_entry.y == -60);  // -2 z
    CHECK(mem.accelerometer.index_reset_ticks == 7000);
    CHECK(mem.gyroscope.entries[0].x == 5);
    CHECK(mem.gyroscope.raw_entry.x == 5);
    CHECK(mem.gyroscope.raw_entry.z == -6);
    CHECK(mem.gyroscope.raw_entry.y == -7);
    CHECK(mem.gyroscope.index_reset_ticks == 7500);

    // the accelerometer's ring has 8 entries, the gyroscope's 32
    for (int k = 1; k < 8; ++k) {
        module.UpdateAccelerometer(memory.data(), input, 7000 + 100 * k);
    }
    CHECK(Parsed(memory).accelerometer.index == 7);
    module.UpdateAccelerometer(memory.data(), input, 9000);
    mem = Parsed(memory);
    CHECK(mem.accelerometer.index == 0);
    CHECK(mem.accelerometer.index_reset_ticks == 9000);
    CHECK(mem.accelerometer.index_reset_ticks_previous == 7000);
    for (int k = 1; k < 32; ++k) {
        module.UpdateGyroscope(memory.data(), input, 8000 + k);
    }
    CHECK(Parsed(memory).gyroscope.index == 31);
    module.UpdateGyroscope(memory.data(), input, 9500);
    mem = Parsed(memory);
    CHECK(mem.gyroscope.index == 0);
    CHECK(mem.gyroscope.index_reset_ticks == 9500);
    CHECK(mem.gyroscope.index_reset_ticks_previous == 7500);
}

TEST_CASE("enabling and disabling a sensor counts, and starts or stops its updates as Azahar's") {
    hid::Module module;
    CHECK(module.EnableAccelerometer());    // 1: start
    CHECK_FALSE(module.EnableAccelerometer());
    CHECK_FALSE(module.DisableAccelerometer());  // 1: still enabled
    CHECK(module.DisableAccelerometer());   // 0: stop
    CHECK_FALSE(module.DisableAccelerometer());  // -1
    CHECK_FALSE(module.EnableAccelerometer());   // 0: not started, as in Azahar
    CHECK(module.EnableAccelerometer());    // 1
    CHECK(module.AccelerometerCount() == 1);
    CHECK(module.EnableGyroscope());
    CHECK(module.DisableGyroscope());
    CHECK(module.GyroscopeCount() == 0);
    // the values the gyroscope commands answer with
    const hid::GyroscopeCalibrateParam param = hid::DefaultGyroscopeCalibrateParam();
    CHECK(param.values[0][0] == 0);
    CHECK(param.values[2][1] == 6700);
    CHECK(param.values[1][2] == -6700);
    CHECK(hid::kGyroscopeCoefficient == 14.375f);
}

// ---- the scripted input source ------------------------------------------------------------------------------

#if MH4N_HAVE_SCHEDULE

namespace {

constexpr u64 kStart = 1000000;  // the tick script time 0 is at (not 0)
constexpr double kTicksPerMs = 268111.856;

u64 TickAt(double ms) {
    return kStart + static_cast<u64>(ms * kTicksPerMs);
}

Schedule::Plan Parse(const std::string& script) {
    Schedule::Plan plan;
    std::string error;
    REQUIRE_MESSAGE(Schedule::ParseInput(script, plan, error), error);
    return plan;
}

}  // namespace

TEST_CASE("a scripted button is held from its start for its hold, in game milliseconds") {
    input_script::ScriptInput source(Parse("100:A:50,200:START,300:L:20,300:R:20"), nullptr, kStart);
    CHECK(source.Sample(TickAt(99)).buttons == 0);
    CHECK(source.Sample(TickAt(100.5)).buttons == kPadA);
    CHECK(source.Sample(TickAt(149)).buttons == kPadA);
    CHECK(source.Sample(TickAt(151)).buttons == 0);
    // the default hold is 100 ms
    CHECK(source.Sample(TickAt(250)).buttons == kPadStart);
    CHECK(source.Sample(TickAt(299)).buttons == kPadStart);  // 200 + 100
    CHECK(source.Sample(TickAt(310)).buttons == (kPadL | kPadR));
    CHECK(source.Sample(TickAt(321)).buttons == 0);
}

TEST_CASE("every button of the script maps to the pad bit of the HID shared memory") {
    const char* names[] = {"A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "L", "R", "START", "SELECT"};
    const u32 bits[] = {kPadA, kPadB, kPadX, kPadY, kPadUp, kPadDown, kPadLeft, kPadRight, kPadL, kPadR, kPadStart, kPadSelect};
    for (int i = 0; i < 12; ++i) {
        input_script::ScriptInput source(Parse(std::string("10:") + names[i] + ":100"), nullptr, kStart);
        CHECK_MESSAGE(source.Sample(TickAt(20)).buttons == bits[i], names[i]);
    }
    // ZL and ZR have their own fields; HOME is not a button of HID's pad word
    input_script::ScriptInput extra(Parse("10:ZL:100,10:ZR:100,10:HOME:100"), nullptr, kStart);
    const InputState state = extra.Sample(TickAt(20));
    CHECK(state.zl);
    CHECK(state.zr);
    CHECK(state.buttons == 0);
}

TEST_CASE("a press keyed on a frame starts at the game's frame and a hold in frames ends by frames") {
    input_script::ScriptInput source(Parse("f600:B:f3,f700:TOUCH:10:20:f2"), nullptr, kStart);
    *source.FrameCell() = 599;
    CHECK(source.Sample(TickAt(1)).buttons == 0);
    *source.FrameCell() = 600;
    CHECK(source.Sample(TickAt(2)).buttons == kPadB);
    *source.FrameCell() = 602;
    CHECK(source.Sample(TickAt(3)).buttons == kPadB);
    *source.FrameCell() = 603;  // 600 + 3: over
    CHECK(source.Sample(TickAt(4)).buttons == 0);
    *source.FrameCell() = 700;
    InputState touch = source.Sample(TickAt(5));
    CHECK(touch.touch_down);
    CHECK(touch.touch_x == 10);
    CHECK(touch.touch_y == 20);
    *source.FrameCell() = 702;
    CHECK_FALSE(source.Sample(TickAt(6)).touch_down);
}

TEST_CASE("a press keyed on time with a hold in frames counts the frames from the first sample after it") {
    input_script::ScriptInput source(Parse("100:A:f2"), nullptr, kStart);
    *source.FrameCell() = 40;
    CHECK(source.Sample(TickAt(90)).buttons == 0);
    *source.FrameCell() = 50;
    CHECK(source.Sample(TickAt(101)).buttons == kPadA);  // remembers frame 50
    *source.FrameCell() = 51;
    CHECK(source.Sample(TickAt(102)).buttons == kPadA);
    *source.FrameCell() = 52;
    CHECK(source.Sample(TickAt(103)).buttons == 0);
}

TEST_CASE("the circle pad is the stick times 0x9A, rounded") {
    input_script::ScriptInput source(Parse("100:CIRCLE:1:-0.5,300:CIRCLE:0.7:0.0035:50"), nullptr, kStart);
    InputState a = source.Sample(TickAt(110));
    CHECK(a.circle_x == 154);
    CHECK(a.circle_y == -77);
    InputState idle = source.Sample(TickAt(250));
    CHECK(idle.circle_x == 0);
    CHECK(idle.circle_y == 0);
    InputState b = source.Sample(TickAt(310));
    CHECK(b.circle_x == 108);
    CHECK(b.circle_y == 1);
}

TEST_CASE("the touch pixel goes through the reference's float round trip") {
    // x / 320 and y / 240 as floats, multiplied back and truncated: y = 63 comes out as 62, as in
    // the runner this reproduces (and 125, 126, 127 as one less)
    input_script::ScriptInput source(
        Parse("10:TOUCH:160:120:20,40:TOUCH:319:63:20,70:TOUCH:0:127:20,100:TOUCH:1:239:20"), nullptr, kStart);
    InputState s = source.Sample(TickAt(15));
    CHECK(s.touch_down);
    CHECK(s.touch_x == 160);
    CHECK(s.touch_y == 120);
    s = source.Sample(TickAt(45));
    CHECK(s.touch_x == 319);
    CHECK(s.touch_y == 62);
    s = source.Sample(TickAt(75));
    CHECK(s.touch_x == 0);
    CHECK(s.touch_y == 126);
    s = source.Sample(TickAt(105));
    CHECK(s.touch_x == 1);
    CHECK(s.touch_y == 239);
    CHECK_FALSE(source.Sample(TickAt(200)).touch_down);
}

TEST_CASE("when two presses of one kind overlap, the first one that is held wins") {
    input_script::ScriptInput source(Parse("100:CIRCLE:1:0:100,150:CIRCLE:0:1:100,100:A:50,120:A:100"), nullptr,
                                     kStart);
    InputState s = source.Sample(TickAt(160));
    CHECK(s.circle_x == 154);
    CHECK(s.circle_y == 0);
    CHECK(s.buttons == kPadA);
    s = source.Sample(TickAt(210));  // the first circle press is over; the second is held until 250
    CHECK(s.circle_x == 0);
    CHECK(s.circle_y == 154);
    CHECK(s.buttons == kPadA);  // the second A (120..220)
    CHECK(source.Sample(TickAt(230)).buttons == 0);
}

#endif  // MH4N_HAVE_SCHEDULE

// ---- the services ---------------------------------------------------------------------------------------------

#if !__has_include("recomp.h")
TEST_CASE("hid_test's service cases need recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>

#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"
#include "services/hid/hid_state.h"
#include "services/ir/ir_state.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

namespace {

namespace fs = std::filesystem;

constexpr u32 kText = 0x00100000;
constexpr u32 kFrameCounter = kFrameCounterAddress;  // 0x010571d4, in the page below

// ---- harness: an ELF whose main thread runs a C++ body (as srv_test) ------------------------------------

void Put16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v), b[at + 1] = static_cast<u8>(v >> 8);
}
void Put32(std::vector<u8>& b, std::size_t at, u32 v) {
    Put16(b, at, v & 0xFFFF), Put16(b, at + 2, v >> 16);
}

// Two loadable segments: the code page, and the page that holds the game's frame counter, so that
// the runtime has frames to watch and the test can advance them.
std::vector<u8> MakeElf() {
    constexpr u32 kData = kFrameCounter & ~0xFFFu;
    std::vector<u8> b(52 + 2 * 32, 0);
    b[0] = 0x7F, b[1] = 'E', b[2] = 'L', b[3] = 'F', b[4] = 1, b[5] = 1, b[6] = 1;
    Put16(b, 16, 2), Put16(b, 18, 40), Put32(b, 20, 1), Put32(b, 24, kText);
    Put32(b, 28, 52), Put16(b, 40, 52), Put16(b, 42, 32), Put16(b, 44, 2);
    const std::size_t code = 52, data = 52 + 32;
    Put32(b, code, 1);  // PT_LOAD
    Put32(b, code + 4, 116), Put32(b, code + 8, kText), Put32(b, code + 12, kText);
    Put32(b, code + 16, 0x40), Put32(b, code + 20, 0x40), Put32(b, code + 24, 5);
    Put32(b, code + 28, 0x1000);
    Put32(b, data, 1);
    Put32(b, data + 4, 116 + 0x40), Put32(b, data + 8, kData), Put32(b, data + 12, kData);
    Put32(b, data + 16, 0x10), Put32(b, data + 20, 0x10), Put32(b, data + 24, 6);
    Put32(b, data + 28, 0x1000);
    for (u32 i = 0; i < 0x40; ++i) {
        b.push_back(static_cast<u8>(i));
    }
    for (u32 i = 0; i < 0x10; ++i) {
        b.push_back(0);
    }
    return b;
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("mh4n_hid_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    std::string File(const std::string& name) const { return (path_ / name).string(); }
    std::string WriteElf() const {
        const std::string path = File("program.elf");
        const std::vector<u8> bytes = MakeElf();
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return path;
    }

private:
    fs::path path_;
};

struct Fixture {
    ManualTimeSource* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    std::vector<std::string> logged;
    // `real_time`: the runtime's own time source, the host's steady clock (and the timer thread that
    // preempts guest code), as a real run has.
    explicit Fixture(bool real_time = false) {
        if (real_time) {
            runtime = std::make_unique<Runtime>();
        } else {
            auto source = std::make_unique<ManualTimeSource>();
            time = source.get();
            RuntimeConfig config;
            config.time = std::move(source);
            runtime = std::make_unique<Runtime>(std::move(config));
        }
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { logged.push_back(std::string(r.message)); });
    }
    ~Fixture() {
        runtime.reset();
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    bool Logged(const std::string& needle) const {
        for (const std::string& line : logged) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra,
             std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf()};
    args.insert(args.end(), extra.begin(), extra.end());
    f.runtime->SetMainThreadEntry([body = std::move(body)](GuestThread&) { body(); });
    return f.runtime->Boot(Parse(args));
}

struct Regs {
    std::array<u32, 16> r{};
};

Regs CallSvc(Runtime& rt, u32 number, std::initializer_list<u32> in) {
    RCtx ctx{};
    int i = 0;
    for (const u32 value : in) {
        ctx.r[i++] = value;
    }
    ctx.r[14] = kText + 0x20;
    rt.Svc().CallSvc(ctx, number, kText + 0x11);
    Regs out;
    for (int k = 0; k < 16; ++k) {
        out.r[static_cast<std::size_t>(k)] = ctx.r[k];
    }
    return out;
}

void ExitProcess(Runtime& rt) {
    CallSvc(rt, 0x03, {});
}

constexpr Result kTimeout = 0x09401BFE;

// svcSleepThread: the game clock advances by `ns` (the timers due before it run, in order).
void SleepNs(Runtime& rt, u64 ns) {
    CallSvc(rt, 0x0A, {static_cast<u32>(ns), static_cast<u32>(ns >> 32)});
}
void SleepMs(Runtime& rt, u64 ms) {
    SleepNs(rt, ms * 1000000);
}

struct Guest {
    Runtime& rt;
    explicit Guest(Runtime& runtime) : rt(runtime) {}

    u32 Cmd() const { return CommandBufferAddress(*rt.MainThread()); }
    u32 Scratch(u32 offset = 0) const { return rt.MainThread()->stack_top - 0x3000 + offset; }

    void Put(const std::vector<u32>& words) {
        REQUIRE(rt.Memory().WriteBlock(Cmd(), words.data(), static_cast<u32>(words.size() * 4)));
    }
    std::vector<u32> Get(std::size_t count) {
        std::vector<u32> words(count);
        REQUIRE(rt.Memory().ReadBlock(words.data(), Cmd(), static_cast<u32>(count * 4)));
        return words;
    }
    Result ConnectToPort(const std::string& name, Handle* handle) {
        std::vector<u8> text(name.begin(), name.end());
        text.push_back(0);
        REQUIRE(rt.Memory().WriteBlock(Scratch(0x2000), text.data(), static_cast<u32>(text.size())));
        const Regs out = CallSvc(rt, 0x2D, {0, Scratch(0x2000)});
        *handle = out.r[1];
        return out.r[0];
    }
    Result Send(Handle session) { return CallSvc(rt, 0x32, {session}).r[0]; }
    std::vector<u32> Request(Handle session, const std::vector<u32>& words, std::size_t reply_words,
                             Result* result = nullptr) {
        Put(words);
        const Result r = Send(session);
        if (result != nullptr) {
            *result = r;
        }
        return Get(reply_words);
    }
    Result Wait(Handle handle, s64 ns) {
        return CallSvc(rt, 0x24,
                       {handle, static_cast<u32>(ns), static_cast<u32>(static_cast<u64>(ns) >> 32)})
            .r[0];
    }
    // svcMapMemoryBlock at `address`, read-only for the application.
    Result MapReadOnly(Handle block, u32 address) {
        return CallSvc(rt, 0x1F, {block, address, 1, 0x10000000}).r[0];
    }
    std::vector<u8> Read(u32 address, u32 size) {
        std::vector<u8> bytes(size);
        REQUIRE(rt.Memory().ReadBlock(bytes.data(), address, size));
        return bytes;
    }

    // srv: GetServiceHandle for `name`: the handle (0 if none).
    Handle Open(Handle srv, const std::string& name) {
        std::array<u32, 2> words{};
        std::memcpy(words.data(), name.data(), std::min<std::size_t>(name.size(), 8));
        const std::vector<u32> reply = Request(
            srv, {ipc::MakeHeader(0x0005, 4, 0), words[0], words[1], static_cast<u32>(name.size()), 0}, 4);
        return reply[1] == kResultSuccess ? reply[3] : 0;
    }
    Handle Srv() {
        Handle h = 0;
        REQUIRE(ConnectToPort("srv:", &h) == kResultSuccess);
        return h;
    }
};

std::shared_ptr<hid::Shared> HidOf(Runtime& rt) {
    ServiceManager* manager = FindServiceManager(rt);
    REQUIRE(manager != nullptr);
    return manager->Context().State<hid::Shared>("hid");
}

std::shared_ptr<ir::Shared> IrOf(Runtime& rt) {
    ServiceManager* manager = FindServiceManager(rt);
    REQUIRE(manager != nullptr);
    return manager->Context().State<ir::Shared>("ir:USER");
}

// An input source that records the ticks it is asked about, and answers `answer(tick)`.
class Recorder final : public InputSource {
public:
    explicit Recorder(std::function<InputState(u64)> answer = {}) : answer_(std::move(answer)) {}
    InputState Sample(u64 tick) override {
        ticks.push_back(tick);
        return answer_ ? answer_(tick) : InputState{};
    }
    std::vector<u64> ticks;

private:
    std::function<InputState(u64)> answer_;
};

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

// ---- the cadence ----------------------------------------------------------------------------------------------

TEST_CASE("the pad update runs every 1,145,777 ticks of the game clock from the module's start") {
    Fixture f;
    TempDir dir;
    Recorder* recorder = nullptr;
    u64 origin = 0;
    u64 end = 0;
    std::shared_ptr<hid::Shared> hid;
    const int status = BootWith(f, dir, {}, [&] {
        hid = HidOf(*f.runtime);
        origin = hid->Origin();
        auto source = std::make_unique<Recorder>();
        recorder = source.get();
        f.runtime->SetInputSource(std::move(source));
        SleepMs(*f.runtime, 1000);
        end = f.runtime->Clock().Now();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(recorder != nullptr);
    // 1000 ms at 234 updates a second
    REQUIRE(recorder->ticks.size() >= 233);
    for (std::size_t k = 0; k < recorder->ticks.size(); ++k) {
        CHECK(recorder->ticks[k] == origin + (k + 1) * hid::kPadUpdateTicks);
    }
    // none is missing, none is early: the last one is the last due before the end of the sleep
    CHECK(recorder->ticks.back() <= end);
    CHECK(recorder->ticks.back() + hid::kPadUpdateTicks > end);
    // the ring index and the stamp of index 0 follow the count
    const u64 count = recorder->ticks.size();
    const hid::SharedMem mem = [&] {
        hid::SharedMem m;
        std::memcpy(&m, hid->MemoryPointer(), sizeof m);
        return m;
    }();
    CHECK(mem.pad.index == (count - 1) % 8);
    const u64 last_wrap = (count - 1) / 8 * 8;  // the update that wrote index 0 last
    CHECK(static_cast<u64>(mem.pad.index_reset_ticks) == recorder->ticks[last_wrap]);
    CHECK(static_cast<u64>(mem.pad.index_reset_ticks_previous) == recorder->ticks[last_wrap - 8]);
}

TEST_CASE("the accelerometer and gyroscope update only while enabled, at their own periods") {
    Fixture f;
    TempDir dir;
    struct Snapshot {
        hid::SharedMem mem;
        u64 now = 0;
    };
    Snapshot idle, accel_on, both_on, at_disable, accel_off;
    u64 accel_enabled_at = 0, gyro_enabled_at = 0;
    Result accel_wait_before = 0, accel_wait_after = 0, gyro_wait_after = 0, accel_enable_result = 1;
    std::vector<u32> coefficient, calibrate, volume;
    const int status = BootWith(f, dir, {}, [&] {
        Guest g(*f.runtime);
        auto hid = HidOf(*f.runtime);
        auto take = [&](Snapshot* out) {
            std::memcpy(&out->mem, hid->MemoryPointer(), sizeof(hid::SharedMem));
            out->now = f.runtime->Clock().Now();
        };
        f.runtime->SetInputSource(std::make_unique<Recorder>([](u64) {
            InputState s;
            s.accel[0] = 7;
            s.gyro[1] = -9;
            return s;
        }));

        const Handle hid_user = g.Open(g.Srv(), "hid:USER");
        REQUIRE(hid_user != 0);
        const std::vector<u32> handles = g.Request(hid_user, {ipc::MakeHeader(0x000A, 0, 0)}, 9);
        const Handle accel_event = handles[6], gyro_event = handles[7];

        SleepMs(*f.runtime, 100);
        take(&idle);
        accel_wait_before = g.Wait(accel_event, 0);  // nothing enabled yet: nothing signalled

        accel_enabled_at = f.runtime->Clock().Now();
        accel_enable_result = g.Request(hid_user, {ipc::MakeHeader(0x0011, 0, 0)}, 2)[1];
        SleepMs(*f.runtime, 100);
        take(&accel_on);
        accel_wait_after = g.Wait(accel_event, 0);

        gyro_enabled_at = f.runtime->Clock().Now();
        g.Request(hid_user, {ipc::MakeHeader(0x0013, 0, 0)}, 2);
        SleepMs(*f.runtime, 100);
        take(&both_on);
        gyro_wait_after = g.Wait(gyro_event, 0);

        take(&at_disable);
        g.Request(hid_user, {ipc::MakeHeader(0x0012, 0, 0)}, 2);  // disable the accelerometer
        SleepMs(*f.runtime, 100);
        take(&accel_off);

        coefficient = g.Request(hid_user, {ipc::MakeHeader(0x0015, 0, 0)}, 3);
        calibrate = g.Request(hid_user, {ipc::MakeHeader(0x0016, 0, 0)}, 7);
        volume = g.Request(hid_user, {ipc::MakeHeader(0x0017, 0, 0)}, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(accel_enable_result == kResultSuccess);

    // before the enable: no accelerometer or gyroscope update, no event
    CHECK(idle.mem.accelerometer.index_reset_ticks == 0);
    CHECK(idle.mem.gyroscope.index_reset_ticks == 0);
    CHECK(accel_wait_before == kTimeout);
    CHECK(accel_wait_after == kResultSuccess);
    CHECK(gyro_wait_after == kResultSuccess);

    // the accelerometer's updates are due at enable + k * 2,577,998 ticks: after n of them the ring
    // index is (n - 1) % 8 and index 0 was stamped by update 1 + 8 * floor((n - 1) / 8)
    {
        const u64 n = (accel_on.now - accel_enabled_at) / hid::kAccelerometerUpdateTicks;
        REQUIRE(n >= 9);
        const u64 stamped = 1 + 8 * ((n - 1) / 8);
        CHECK(accel_on.mem.accelerometer.index == (n - 1) % 8);
        CHECK(static_cast<u64>(accel_on.mem.accelerometer.index_reset_ticks) ==
              accel_enabled_at + stamped * hid::kAccelerometerUpdateTicks);
        CHECK(static_cast<u64>(accel_on.mem.accelerometer.index_reset_ticks_previous) ==
              accel_enabled_at + (stamped - 8) * hid::kAccelerometerUpdateTicks);
        CHECK(accel_on.mem.accelerometer.entries[accel_on.mem.accelerometer.index].x == 7);
        CHECK(accel_on.mem.gyroscope.index_reset_ticks == 0);  // the gyroscope is still off
    }
    // the gyroscope's, at 2,654,572 ticks, in a ring of 32
    {
        const u64 n = (both_on.now - gyro_enabled_at) / hid::kGyroscopeUpdateTicks;
        REQUIRE(n >= 9);
        CHECK(both_on.mem.gyroscope.index == (n - 1) % 32);
        CHECK(static_cast<u64>(both_on.mem.gyroscope.index_reset_ticks) ==
              gyro_enabled_at + hid::kGyroscopeUpdateTicks);
        CHECK(both_on.mem.gyroscope.entries[both_on.mem.gyroscope.index].y == -9);
        CHECK(both_on.mem.gyroscope.raw_entry.z == 9);
    }
    // disabled: the accelerometer's block stands still from then on, the gyroscope's goes on
    CHECK(std::memcmp(&accel_off.mem.accelerometer, &at_disable.mem.accelerometer,
                      sizeof(accel_off.mem.accelerometer)) == 0);
    CHECK(accel_off.mem.accelerometer.index_reset_ticks != 0);
    CHECK(accel_off.mem.gyroscope.index != at_disable.mem.gyroscope.index);

    // GetGyroscopeLowRawToDpsCoefficient: 0x00150080, result, 14.375f
    CHECK(coefficient[0] == 0x00150080);
    CHECK(coefficient[1] == kResultSuccess);
    float coef;
    std::memcpy(&coef, &coefficient[2], 4);
    CHECK(coef == 14.375f);
    // GetGyroscopeLowCalibrateParam: 0x00160180, result, 18 bytes: {0, 6700, -6700} three times
    CHECK(calibrate[0] == 0x00160180);
    CHECK(calibrate[1] == kResultSuccess);
    CHECK(calibrate[2] == (0u | 6700u << 16));
    CHECK(calibrate[3] == static_cast<u32>(static_cast<u16>(-6700)));
    // GetSoundVolume: 0x00170080, result, 0x3F
    CHECK(volume[0] == 0x00170080);
    CHECK(volume[1] == kResultSuccess);
    CHECK(volume[2] == 0x3F);
}

// ---- the game's view ------------------------------------------------------------------------------------------

TEST_CASE("GetIPCHandles gives the shared memory and five events; the game maps and reads the block") {
    Fixture f;
    TempDir dir;
    std::vector<u32> handles;
    Result map_result = 1, wait_early = 1, wait_result = 1, wait_again = 1, spvr_wait = 1;
    std::vector<u8> after_map;
    std::vector<u8> mapped_block;
    std::vector<u32> spvr_handles;
    const int status = BootWith(f, dir, {}, [&] {
        Guest g(*f.runtime);
        auto hid = HidOf(*f.runtime);
        f.runtime->SetInputSource(std::make_unique<Recorder>([](u64) {
            InputState s;  // A held and a touch at (160, 120)
            s.buttons = kPadA;
            s.touch_down = true;
            s.touch_x = 160;
            s.touch_y = 120;
            return s;
        }));
        const Handle srv = g.Srv();
        const Handle hid_user = g.Open(srv, "hid:USER");
        REQUIRE(hid_user != 0);
        handles = g.Request(hid_user, {ipc::MakeHeader(0x000A, 0, 0)}, 9);
        REQUIRE(handles.size() == 9);
        const Handle memory = handles[3];
        wait_early = g.Wait(handles[4], 0);  // pad or touch 1: the first update is a period away
        SleepMs(*f.runtime, 60);
        wait_result = g.Wait(handles[4], 0);
        wait_again = g.Wait(handles[4], 0);  // a OneShot event clears when a thread takes it
        map_result = g.MapReadOnly(memory, 0x10000000);
        mapped_block = g.Read(0x10000000, 0x2B0);
        after_map.assign(hid->MemoryPointer(), hid->MemoryPointer() + 0x2B0);
        const Handle spvr = g.Open(srv, "hid:SPVR");
        REQUIRE(spvr != 0);
        spvr_handles = g.Request(spvr, {ipc::MakeHeader(0x000A, 0, 0)}, 9);
        spvr_wait = g.Wait(spvr_handles[5], 0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // 0x000A0047: one normal word (the result), seven translate words (the descriptor, six handles)
    CHECK(handles[0] == 0x000A0047);
    CHECK(handles[1] == kResultSuccess);
    CHECK(handles[2] == ipc::CopyHandleDesc(6));
    CHECK(std::all_of(handles.begin() + 3, handles.end(), [](u32 h) { return h != 0; }));
    // six distinct handles
    std::vector<u32> sorted(handles.begin() + 3, handles.end());
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK(wait_early == kTimeout);
    CHECK(wait_result == kResultSuccess);
    CHECK(wait_again == kTimeout);
    CHECK(map_result == kResultSuccess);
    // the block the game maps is the module's block, with what the updates wrote
    CHECK(mapped_block == after_map);
    // what the source answered
    const hid::SharedMem mem = [&] {
        hid::SharedMem m;
        std::memcpy(&m, mapped_block.data(), sizeof m);
        return m;
    }();
    const hid::PadDataEntry& pad = mem.pad.entries[mem.pad.index];
    CHECK(pad.current_state == kPadA);
    const hid::TouchDataEntry& touch = mem.touch.entries[mem.touch.index];
    CHECK(touch.x == 160);
    CHECK(touch.y == 120);
    CHECK(touch.valid == 1);
    // hid:SPVR is the same module: the same shared memory and events
    CHECK(spvr_handles[0] == 0x000A0047);
    CHECK(spvr_handles[1] == kResultSuccess);
    CHECK(spvr_wait == kResultSuccess);
}

TEST_CASE("hid commands that Azahar lists without a handler get the answer to an unknown command") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {}, [&] {
        Guest g(*f.runtime);
        const Handle hid_user = g.Open(g.Srv(), "hid:USER");
        REQUIRE(hid_user != 0);
        // CalibrateTouchScreen
        reply = g.Request(hid_user, {ipc::MakeHeader(0x0001, 0, 0)}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply[0] == ipc::MakeHeader(0x0001, 1, 0));
    CHECK(reply[1] == kResultSuccess);
    CHECK(f.Logged("unimplemented hid:USER 0x00010000"));
}

// ---- a script, end to end, and replay -------------------------------------------------------------------------

#if MH4N_HAVE_SCHEDULE

namespace {

struct Run {
    std::vector<u64> ticks;
    std::vector<InputState> states;
    std::vector<u8> memory;  // the first 0x2b0 bytes at the end
    int status = -1;
};

// Plays `script` (the `script` source made from --input, wrapped to log what it answered) for
// `ms` of game time, advancing the frame counter by one every 16 ms.
Run PlayScript(const std::string& script, u32 ms, u32 first_frame = 0) {
    Fixture f;
    TempDir dir;
    Run run;
    class Tee final : public InputSource {
    public:
        Tee(std::unique_ptr<InputSource> inner, Run* run) : inner_(std::move(inner)), run_(run) {}
        InputState Sample(u64 tick) override {
            const InputState state = inner_->Sample(tick);
            run_->ticks.push_back(tick);
            run_->states.push_back(state);
            return state;
        }

    private:
        std::unique_ptr<InputSource> inner_;
        Run* run_;
    };
    run.status = BootWith(f, dir, {"--input", script}, [&] {
        // wrap the source the devices step made: make a second one from the same options
        const auto* factory = Registry<InputSourceFactory>::Instance().Find("script");
        REQUIRE(factory != nullptr);
        f.runtime->SetInputSource(std::make_unique<Tee>((*factory)(*f.runtime), &run));
        auto hid = HidOf(*f.runtime);
        for (u32 t = 0; t < ms; t += 16) {
            f.runtime->Memory().Write<u32>(kFrameCounter, first_frame + t / 16);
            SleepMs(*f.runtime, 16);
        }
        run.memory.assign(hid->MemoryPointer(), hid->MemoryPointer() + hid::kReadableSize);
        ExitProcess(*f.runtime);
    });
    return run;
}

}  // namespace

TEST_CASE("a script plays the same way every run: ticks, answers and the shared memory are equal") {
    const std::string script = "50:A:80,60:TOUCH:300:63:70,100:CIRCLE:1:-1:60,f20:START:f4,150:L:30";
    const Run first = PlayScript(script, 400);
    const Run second = PlayScript(script, 400);
    REQUIRE(first.status == 0);
    REQUIRE(second.status == 0);
    REQUIRE(!first.ticks.empty());
    CHECK(first.ticks == second.ticks);
    CHECK(first.states == second.states);
    CHECK(first.memory == second.memory);
    // and every answer is the script's at the tick it was asked for: A is held from 50 ms to 130 ms
    bool saw_a = false, saw_touch = false, saw_circle = false, saw_start = false, saw_l = false;
    for (std::size_t k = 0; k < first.ticks.size(); ++k) {
        const double ms = static_cast<double>(first.ticks[k] - first.ticks[0]) / 268111.856 +
                          1145777.0 / 268111.856;  // from the module's start (script time 0)
        const InputState& s = first.states[k];
        if (ms > 51 && ms < 129) {
            CHECK_MESSAGE((s.buttons & kPadA) != 0, "A at " << ms << " ms");
        }
        if (ms < 49 || ms > 131) {
            CHECK_MESSAGE((s.buttons & kPadA) == 0, "A at " << ms << " ms");
        }
        saw_a = saw_a || (s.buttons & kPadA);
        saw_touch = saw_touch || (s.touch_down && s.touch_x == 300 && s.touch_y == 62);
        saw_circle = saw_circle || (s.circle_x == 154 && s.circle_y == -154);
        saw_start = saw_start || (s.buttons & kPadStart);
        saw_l = saw_l || (s.buttons & kPadL);
    }
    CHECK(saw_a);
    CHECK(saw_touch);
    CHECK(saw_circle);
    CHECK(saw_start);  // keyed on frame 20, held 4 frames
    CHECK(saw_l);
}

TEST_CASE("on the host's real clock the updates stay on exact ticks and answer by game time only") {
    Fixture f(/*real_time=*/true);
    TempDir dir;
    Run run;
    u64 start = 0;
    class Tee final : public InputSource {
    public:
        Tee(std::unique_ptr<InputSource> inner, Run* run) : inner_(std::move(inner)), run_(run) {}
        InputState Sample(u64 tick) override {
            const InputState state = inner_->Sample(tick);
            run_->ticks.push_back(tick);
            run_->states.push_back(state);
            return state;
        }

    private:
        std::unique_ptr<InputSource> inner_;
        Run* run_;
    };
    const int status = BootWith(f, dir, {"--input", "50:A:80,120:TOUCH:10:20:60"}, [&] {
        const auto* factory = Registry<InputSourceFactory>::Instance().Find("script");
        REQUIRE(factory != nullptr);
        start = HidOf(*f.runtime)->Origin();
        f.runtime->SetInputSource(std::make_unique<Tee>((*factory)(*f.runtime), &run));
        SleepMs(*f.runtime, 300);
        ExitProcess(*f.runtime);
    });
    REQUIRE(status == 0);
    REQUIRE(run.ticks.size() >= 60);  // 300 ms at 234 updates a second is 70
    for (std::size_t k = 0; k < run.ticks.size(); ++k) {
        // whatever the host did meanwhile, the k-th update is due exactly k + 1 periods after the start
        CHECK(run.ticks[k] == start + (k + 1) * hid::kPadUpdateTicks);
        const double ms = static_cast<double>(run.ticks[k] - start) / (static_cast<double>(kTicksPerSecond) / 1000.0);
        CHECK_MESSAGE(((run.states[k].buttons & kPadA) != 0) == (ms >= 50 && ms < 130), "A at " << ms << " ms");
        CHECK_MESSAGE(run.states[k].touch_down == (ms >= 120 && ms < 180), "touch at " << ms << " ms");
    }
}

TEST_CASE("a script that does not parse ends the run with status 2 before the program runs") {
    Fixture f;
    TempDir dir;
    bool ran = false;
    const int status = BootWith(f, dir, {"--input", "10:NOTABUTTON"}, [&] {
        ran = true;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 2);
    CHECK_FALSE(ran);
    CHECK(f.Logged("NOTABUTTON"));
}

#endif  // MH4N_HAVE_SCHEDULE

// ---- --capture-hid, read by tools/native/capfmt.py --------------------------------------------------------------

TEST_CASE("--capture-hid writes the MHID capture that capfmt.py summary and check read") {
    Fixture f;
    TempDir dir;
    const std::string capture = dir.File("hid.mhid");
    const int status = BootWith(f, dir, {"--capture-hid", capture}, [&] {
        Guest g(*f.runtime);
        f.runtime->SetInputSource(std::make_unique<Recorder>([](u64 tick) {
            InputState s;  // A held, and a touch in the second half of the run
            s.buttons = kPadA;
            if (tick > 2 * kTicksPerSecond / 10) {
                s.touch_down = true;
                s.touch_x = 100;
                s.touch_y = 80;
            }
            return s;
        }));
        const Handle srv = g.Srv();
        const Handle hid_user = g.Open(srv, "hid:USER");
        REQUIRE(hid_user != 0);
        const std::vector<u32> handles = g.Request(hid_user, {ipc::MakeHeader(0x000A, 0, 0)}, 9);
        REQUIRE(g.MapReadOnly(handles[3], 0x10000000) == kResultSuccess);
        // ir:USER, set up with a shared memory block of the game's, so that the capture has IR records
        std::shared_ptr<SharedMemory> block;
        REQUIRE(f.runtime->GetKernel().NewSharedMemory(&block, nullptr, 0x1000, MemoryPermission::ReadWrite,
                                                       MemoryPermission::ReadWrite, 0, FcramRegion::Base,
                                                       "test") == kResultSuccess);
        Handle block_handle = 0;
        REQUIRE(f.runtime->GetKernel().AddHandle(&block_handle, block) == kResultSuccess);
        const Handle ir_user = g.Open(srv, "ir:USER");
        REQUIRE(ir_user != 0);
        g.Request(ir_user, {ipc::MakeHeader(0x0018, 6, 2), 0x1000, 0x400, 8, 0x400, 8, 3, ipc::CopyHandleDesc(1), block_handle}, 2);
        for (u32 frame = 1; frame <= 20; ++frame) {
            f.runtime->Memory().Write<u32>(kFrameCounter, frame);
            SleepMs(*f.runtime, 16);
        }
        ExitProcess(*f.runtime);
    });
    REQUIRE(status == 0);

    // The file: magic, version, then {type, frame, ticks, size, payload}
    const std::string bytes = ReadFile(capture);
    REQUIRE(bytes.size() > 8);
    CHECK(bytes.substr(0, 4) == "MHID");
    u32 version;
    std::memcpy(&version, bytes.data() + 4, 4);
    CHECK(version == 1);
    std::size_t at = 8;
    std::size_t hid_records = 0, ir_records = 0;
    u32 last_frame = 0;
    u64 last_ticks = 0;
    bool mapped_seen = false;
    while (at + 20 <= bytes.size()) {
        u32 type, frame, size;
        u64 ticks;
        std::memcpy(&type, bytes.data() + at, 4);
        std::memcpy(&frame, bytes.data() + at + 4, 4);
        std::memcpy(&ticks, bytes.data() + at + 8, 8);
        std::memcpy(&size, bytes.data() + at + 16, 4);
        at += 20;
        REQUIRE(at + size <= bytes.size());
        if (type == 1) {
            CHECK(size == 0x2B0);
            ++hid_records;
            CHECK(frame >= last_frame);
            CHECK(ticks >= last_ticks);
            last_frame = frame;
            last_ticks = ticks;
            if (std::any_of(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                            bytes.begin() + static_cast<std::ptrdiff_t>(at + size),
                            [](char c) { return c != 0; })) {
                mapped_seen = true;
            }
        } else if (type == 2) {
            CHECK(size == 0x200);
            ++ir_records;
        }
        at += size;
    }
    CHECK(at == bytes.size());
    CHECK(hid_records >= 18);  // one per frame of the counter
    CHECK(ir_records >= 1);
    CHECK(ir_records <= hid_records);
    CHECK(mapped_seen);  // zeros before the game maps the memory, the block's bytes after
    const std::string manifest = ReadFile(capture + ".json");
    CHECK(manifest.find("\"format\": \"MHID\"") != std::string::npos);

#if !defined(_WIN32)
    const fs::path tool = fs::path(__FILE__).parent_path() / ".." / ".." / "tools" / "native" / "capfmt.py";
    if (fs::exists(tool) && std::system("python3 --version > /dev/null 2>&1") == 0) {
        const std::string summary = dir.File("summary.json");
        const std::string check = "python3 \"" + tool.string() + "\" check \"" + capture + "\" > /dev/null 2>&1";
        CHECK_MESSAGE(std::system(check.c_str()) == 0, "capfmt.py check rejected the capture");
        const std::string command = "python3 \"" + tool.string() + "\" summary \"" + capture +
                                    "\" --json > \"" + summary + "\" 2>&1";
        CHECK_MESSAGE(std::system(command.c_str()) == 0, "capfmt.py summary could not read the capture");
        const std::string text = ReadFile(summary);
        CHECK(text.find("\"kind\": \"hid\"") != std::string::npos);
        CHECK(text.find("\"hid_records\": " + std::to_string(hid_records)) != std::string::npos);
        CHECK(text.find("\"ir_records\": " + std::to_string(ir_records)) != std::string::npos);
        CHECK(text.find("\"A\"") != std::string::npos);  // buttons_seen
        CHECK(text.find("\"frames_with_touch\": 0") == std::string::npos);
        // the same capture against itself: no difference
        const std::string diff =
            "python3 \"" + tool.string() + "\" diff-hid \"" + capture + "\" \"" + capture + "\" > /dev/null 2>&1";
        CHECK(std::system(diff.c_str()) == 0);
    } else {
        const char* require = std::getenv("MH4N_REQUIRE_TEST_DATA");
        if (require != nullptr && std::string(require) == "1") {
            FAIL("python3 or tools/native/capfmt.py not found, and MH4N_REQUIRE_TEST_DATA=1");
        }
        WARN_MESSAGE(false, "python3 or tools/native/capfmt.py not found: capfmt.py was not run");
    }
#endif
}

TEST_CASE("--capture-hid to a path that cannot be written ends the run with status 2") {
    Fixture f;
    TempDir dir;
    bool ran = false;
    const int status = BootWith(f, dir, {"--capture-hid", dir.File("no/such/dir/hid.mhid")}, [&] {
        ran = true;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 2);
    CHECK_FALSE(ran);
}

// ---- ir:USER ---------------------------------------------------------------------------------------------------

namespace {

struct IrSession {
    Guest& g;
    Handle session;
    std::vector<u32> Call(const std::vector<u32>& words, std::size_t reply_words) {
        return g.Request(session, words, reply_words);
    }
};

}  // namespace

TEST_CASE("ir:USER with no Circle Pad Pro: shared memory header, events, connection and buffer") {
    Fixture f;
    TempDir dir;
    std::vector<u32> init, events[3], require, require_other, send, release0, release1, disconnect, finalize, release_after;
    std::vector<u8> header_after_init, header_after_require, header_after_disconnect;
    std::vector<u8> info_after_init;
    std::vector<u32> unknown;
    Handle second_session = 1;
    std::shared_ptr<ir::Shared> ir;
    bool memory_dropped = false;
    const int status = BootWith(f, dir, {}, [&] {
        Guest g(*f.runtime);
        ir = IrOf(*f.runtime);
        std::shared_ptr<SharedMemory> block;
        REQUIRE(f.runtime->GetKernel().NewSharedMemory(&block, nullptr, 0x1000, MemoryPermission::ReadWrite,
                                                       MemoryPermission::ReadWrite, 0, FcramRegion::Base,
                                                       "test") == kResultSuccess);
        std::memset(block->GetPointer(), 0xEE, 0x40);
        Handle block_handle = 0;
        REQUIRE(f.runtime->GetKernel().AddHandle(&block_handle, block) == kResultSuccess);
        const Handle srv = g.Srv();
        const Handle handle = g.Open(srv, "ir:USER");
        REQUIRE(handle != 0);
        IrSession s{g, handle};

        // a second session is refused: ir:USER takes one at a time
        second_session = g.Open(srv, "ir:USER");

        // InitializeIrNopShared: 0x00180182
        init = s.Call({ipc::MakeHeader(0x0018, 6, 2), 0x1000, 0x400, 8, 0x400, 8, 3, ipc::CopyHandleDesc(1), block_handle}, 2);
        header_after_init.assign(block->GetPointer(), block->GetPointer() + 0x30);
        info_after_init.assign(block->GetPointer() + 0x10, block->GetPointer() + 0x20);
        // the events: 0x000A0042, 0x000B0042, 0x000C0042
        events[0] = s.Call({ipc::MakeHeader(0x000A, 0, 0)}, 4);
        events[1] = s.Call({ipc::MakeHeader(0x000B, 0, 0)}, 4);
        events[2] = s.Call({ipc::MakeHeader(0x000C, 0, 0)}, 4);
        // RequireConnection(1): the game asks for the Circle Pad Pro; with none, the unknown-device branch
        require = s.Call({ipc::MakeHeader(0x0006, 1, 0), 1}, 2);
        header_after_require.assign(block->GetPointer(), block->GetPointer() + 0x10);
        require_other = s.Call({ipc::MakeHeader(0x0006, 1, 0), 7}, 2);
        // SendIrNop: 0x000D0042, size, the static buffer
        const u32 data = g.Scratch(0x100);
        const std::vector<u8> payload = {1, 2, 3, 4};
        REQUIRE(f.runtime->Memory().WriteBlock(data, payload.data(), 4));
        send = s.Call({ipc::MakeHeader(0x000D, 1, 2), 4, ipc::StaticBufferDesc(4, 2), data}, 2);
        // ReleaseReceivedData: 0x00190040
        release0 = s.Call({ipc::MakeHeader(0x0019, 1, 0), 0}, 2);
        release1 = s.Call({ipc::MakeHeader(0x0019, 1, 0), 1}, 2);
        unknown = s.Call({ipc::MakeHeader(0x0003, 0, 0)}, 2);  // ClearReceiveBuffer: nullptr in Azahar
        disconnect = s.Call({ipc::MakeHeader(0x0009, 0, 0)}, 2);
        header_after_disconnect.assign(block->GetPointer(), block->GetPointer() + 0x10);
        finalize = s.Call({ipc::MakeHeader(0x0002, 0, 0)}, 2);
        memory_dropped = ir->shared_memory == nullptr && ir->receive_buffer == nullptr;
        release_after = s.Call({ipc::MakeHeader(0x0019, 1, 0), 1}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(second_session == 0);

    CHECK(init[0] == 0x00180040);
    CHECK(init[1] == kResultSuccess);
    // the 16-byte header: all zero but initialized (offset 14); the buffer info after it is zeros
    CHECK(header_after_init.size() == 0x30);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK_MESSAGE(header_after_init[i] == (i == 14 ? 1 : 0), "header byte " << i);
    }
    CHECK(info_after_init == std::vector<u8>(16, 0));
    CHECK(header_after_init[0x20] == 0xEE);  // nothing else is written

    for (int i = 0; i < 3; ++i) {
        CHECK(events[i][0] == ipc::MakeHeader(static_cast<u16>(0x000A + i), 1, 2));
        CHECK(events[i][1] == kResultSuccess);
        CHECK(events[i][2] == ipc::CopyHandleDesc(1));
        CHECK(events[i][3] != 0);
    }
    CHECK(events[0][3] != events[1][3]);

    // RequireConnection answers success; no device connects: connection_status 1, trying to connect 2
    CHECK(require[0] == 0x00060040);
    CHECK(require[1] == kResultSuccess);
    CHECK(header_after_require[8] == 1);
    CHECK(header_after_require[9] == 2);
    CHECK(header_after_require[10] == 0);   // connection role
    CHECK(header_after_require[12] == 0);   // connected
    CHECK(require_other[1] == kResultSuccess);

    // SendIrNop: not connected: description 13, module IR (67), summary InvalidState (5), level Status
    CHECK(send[0] == 0x000D0040);
    CHECK(send[1] == 0xC8A10C0D);
    // ReleaseReceivedData(0) works, (1) finds no packet: description NoData (1007), summary NotFound
    CHECK(release0[0] == 0x00190040);
    CHECK(release0[1] == kResultSuccess);
    CHECK(release1[1] == 0xC8810FEF);
    // a command Azahar has no handler for: its id, one word, success
    CHECK(unknown[0] == ipc::MakeHeader(0x0003, 1, 0));
    CHECK(unknown[1] == kResultSuccess);
    CHECK(f.Logged("unimplemented ir:USER 0x00030000"));

    CHECK(disconnect[0] == 0x00090040);
    CHECK(disconnect[1] == kResultSuccess);
    CHECK(header_after_disconnect[8] == 0);
    CHECK(header_after_disconnect[12] == 0);
    CHECK(finalize[0] == 0x00020040);
    CHECK(finalize[1] == kResultSuccess);
    CHECK(memory_dropped);
    // Azahar would dereference a null buffer here; this answers as for an empty one
    CHECK(release_after[1] == 0xC8810FEF);
}

TEST_CASE("the receive buffer of ir:USER is Azahar's circular queue in the shared memory") {
    Fixture f;
    TempDir dir;
    std::vector<u8> bytes;
    ir::BufferManager::BufferInfo info{};
    bool all_ok = true;
    const int status = BootWith(f, dir, {}, [&] {
        std::shared_ptr<SharedMemory> block;
        REQUIRE(f.runtime->GetKernel().NewSharedMemory(&block, nullptr, 0x1000, MemoryPermission::ReadWrite,
                                                       MemoryPermission::ReadWrite, 0, FcramRegion::Base,
                                                       "test") == kResultSuccess);
        // 4 packets at most, 0x100 bytes of buffer: 4 * 8 for the packet infos, 0xE0 for data
        ir::BufferManager buffer(block, 0x10, 0x20, 4, 0x100);
        const std::vector<u8> a(0x50, 0xA1), b(0x50, 0xB2), c(0x50, 0xC3), d(0x50, 0xD4);
        all_ok = all_ok && buffer.Put(a);
        all_ok = all_ok && buffer.Put(b);
        all_ok = all_ok && !buffer.Put(c);  // 0xE0 - 0xA0 = 0x40 free: too big
        all_ok = all_ok && buffer.Release(1);
        all_ok = all_ok && buffer.Put(c);   // wraps around the end of the data buffer
        all_ok = all_ok && !buffer.Release(3);
        all_ok = all_ok && buffer.Release(2);
        all_ok = all_ok && buffer.Put(d);
        info = buffer.Info();
        bytes.assign(block->GetPointer(), block->GetPointer() + 0x120);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(all_ok);
    // BufferInfo at 0x10: begin, end, count, unknown
    CHECK(info.begin_index == 3);
    CHECK(info.end_index == 0);
    CHECK(info.packet_count == 1);
    u32 in_memory[4];
    std::memcpy(in_memory, bytes.data() + 0x10, 16);
    CHECK(in_memory[0] == 3);
    CHECK(in_memory[1] == 0);
    CHECK(in_memory[2] == 1);
    // the packet infos at 0x20: {offset, size}; d went in the fourth slot (index 3) at offset 0,
    // because the queue was empty
    u32 packet3[2];
    std::memcpy(packet3, bytes.data() + 0x20 + 8 * 3, 8);
    CHECK(packet3[0] == 0);
    CHECK(packet3[1] == 0x50);
    // c's second slot (index 2) was {0xA0, 0x50}: it wrapped around the end of the 0xE0-byte data buffer
    u32 packet2[2];
    std::memcpy(packet2, bytes.data() + 0x20 + 8 * 2, 8);
    CHECK(packet2[0] == 0xA0);
    CHECK(packet2[1] == 0x50);
    // the data buffer starts after the packet infos, at 0x20 + 4 * 8 = 0x40
    CHECK(bytes[0x40] == 0xD4);                // d overwrote the start of a and of the wrapped c
    CHECK(bytes[0x40 + 0x4F] == 0xD4);
    CHECK(bytes[0x40 + 0x50] == 0xB2);         // b, untouched
    CHECK(bytes[0x40 + 0xA0] == 0xC3);         // the first part of c, up to the end of the buffer
    CHECK(bytes[0x40 + 0xDF] == 0xC3);
}

TEST_CASE("a handle that is not a shared memory block is refused by InitializeIrNopShared") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {}, [&] {
        Guest g(*f.runtime);
        const Handle srv = g.Srv();
        const Handle ir_user = g.Open(srv, "ir:USER");
        REQUIRE(ir_user != 0);
        const Handle not_memory = CallSvc(*f.runtime, 0x17, {0}).r[1];  // an event
        reply = g.Request(ir_user, {ipc::MakeHeader(0x0018, 6, 2), 0x1000, 0x400, 8, 0x400, 8, 3,
                                    ipc::CopyHandleDesc(1), not_memory}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply[1] == kResultInvalidHandle);
}

#endif  // recomp.h
