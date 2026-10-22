// Derived from Azahar src/core/hle/service/ir/extra_hid.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the expected status packets,
// calibration answers and polling behaviour, restated from its code.
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project
//
// mh4n_ir_device (N3-08): the Circle Pad Pro's IR protocol. No game data. The expected bytes are
// worked out by hand from extra_hid.cpp:
//   - the status packet is ExtraHIDResponse {u32 bitfield: header[0:8], c_stick_x[8:20],
//     c_stick_y[20:32]; u8: battery_level[0:5], zl_not_held[5], zr_not_held[6], r_not_held[7]; u8
//     unknown}, little-endian, 6 bytes, header 0x10 (PollHID); the battery level is 0x1F, R is never
//     pressed, the sticks are static_cast<u32>(0x800 + 0x7FF * x) for x in [-1, 1] (here x is
//     raw / 0x9C, the artic branch);
//   - the calibration answer is [0x11, offset u16, size u16, data...] with offset and size echoed as
//     the game wrote them and the data read at the offset and size aligned down to 16; a read past
//     the end of the 0x40 bytes sends nothing; the request is exactly 6 bytes;
//   - the polling configuration is exactly 3 bytes, [1, period ms, unknown]; it replaces the pending
//     event and the first packet comes msToCycles(period) later, the next one the same after it.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "ir_device/circle_pad_pro.h"
#include "mh4n/clock.h"
#include "mh4n/log.h"

namespace mh4n::ir_device {
namespace {

using Bytes = std::vector<u8>;

// Azahar's calibration data, restated.
const u8 kExpectedCalibration[0x40] = {
    0x00, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0xF5,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
};

// A timer queue of one event, driven by hand.
class FakeTimer : public Timer {
public:
    void Schedule(u64 delay_ticks, std::function<void()> callback) override {
        CHECK_MESSAGE(!callback_, "Schedule with an event pending");
        delay_ = delay_ticks;
        callback_ = std::move(callback);
        ++schedules;
    }
    void Cancel() override {
        callback_ = nullptr;
        ++cancels;
    }
    bool Pending() const { return static_cast<bool>(callback_); }
    u64 Delay() const { return delay_; }
    // Runs the pending event (as the queue does: it is gone before it runs).
    bool Fire() {
        auto callback = std::move(callback_);
        callback_ = nullptr;
        if (!callback) {
            return false;
        }
        callback();
        return true;
    }
    int schedules = 0;
    int cancels = 0;

private:
    u64 delay_ = 0;
    std::function<void()> callback_;
};

// Takes the log messages of the test (so the run stays quiet) and gives the sink back at the end.
struct LogCapture {
    std::vector<std::string> messages;
    LogCapture() {
        SetLogSink([this](const LogRecord& record) {
            messages.emplace_back(record.message);
        });
    }
    ~LogCapture() { SetLogSink({}); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
};

struct Rig {
    LogCapture log;
    FakeTimer timer;
    std::vector<Bytes> sent;
    InputState input;
    CirclePadPro device{[this](std::span<const u8> p) { sent.emplace_back(p.begin(), p.end()); }, timer,
                        [this] { return input; }};
    void Send(Bytes request) { device.OnReceive(request); }
};

InputState Stick(int x, int y, bool zl = false, bool zr = false) {
    InputState s;
    s.cstick_x = static_cast<s16>(x);
    s.cstick_y = static_cast<s16>(y);
    s.zl = zl;
    s.zr = zr;
    return s;
}

std::array<u8, 6> Status(const InputState& s) {
    return BuildStatus(s);
}

constexpr u64 MsTicks(int ms) {
    return 268111856ull * ms / 1000;
}

}  // namespace

TEST_CASE("the status packet with no input: the stick is centred (0x800), not zero") {
    CHECK(Status(InputState{}) == std::array<u8, 6>{0x10, 0x00, 0x08, 0x80, 0xFF, 0x00});
}

TEST_CASE("the status packet for given sticks and buttons") {
    // x = 0x800 + 0x7FF = 0xFFF, y = 0x800 - 0x7FF = 0x001: bytes 1-3 are x[0:8], x[8:12] | y[0:4] << 4,
    // y[4:12].
    CHECK(Status(Stick(156, -156)) == std::array<u8, 6>{0x10, 0xFF, 0x1F, 0x00, 0xFF, 0x00});
    CHECK(Status(Stick(-156, 156)) == std::array<u8, 6>{0x10, 0x01, 0xF0, 0xFF, 0xFF, 0x00});
    // raw 78 is exactly 0.5: 0x800 + 1023.5 -> 0xBFF; raw -78: 0x800 - 1023.5 = 1024.5 -> 0x400.
    CHECK(Status(Stick(78, -78)) == std::array<u8, 6>{0x10, 0xFF, 0x0B, 0x40, 0xFF, 0x00});
    // x only, y centred: x = 0xFFF, y = 0x800.
    CHECK(Status(Stick(156, 0)) == std::array<u8, 6>{0x10, 0xFF, 0x0F, 0x80, 0xFF, 0x00});
    // y only, x centred.
    CHECK(Status(Stick(0, 156)) == std::array<u8, 6>{0x10, 0x00, 0xF8, 0xFF, 0xFF, 0x00});
}

TEST_CASE("ZL, ZR and R: the bits are set when the button is NOT pressed") {
    CHECK(Status(Stick(0, 0, true, false))[4] == 0xDF);   // battery 0x1F | ZR not held | R not held
    CHECK(Status(Stick(0, 0, false, true))[4] == 0xBF);   // battery 0x1F | ZL not held | R not held
    CHECK(Status(Stick(0, 0, true, true))[4] == 0x9F);
    CHECK(Status(Stick(0, 0, false, false))[4] == 0xFF);
    // Other buttons of the InputState do not reach the device (R of the pad word included: the
    // Circle Pad Pro's R is not the console's).
    InputState s;
    s.buttons = 0xFFF;
    s.circle_x = 150;
    CHECK(Status(s) == Status(InputState{}));
}

TEST_CASE("the stick fields follow Azahar's float arithmetic for every raw value") {
    for (int raw = -156; raw <= 156; ++raw) {
        // static_cast<u32>((raw / 0x9C) * 0x7FF + 0x800), as written in extra_hid.cpp
        const float f = (static_cast<float>(raw) / 0x9C) * 0x7FF + 0x800;
        const u32 want = static_cast<u32>(f);
        REQUIRE(want <= 0xFFF);
        const auto p = Status(Stick(raw, -raw));
        const u32 x = p[1] | ((p[2] & 0x0Fu) << 8);
        const u32 y = (p[2] >> 4) | (static_cast<u32>(p[3]) << 4);
        CHECK(x == want);
        CHECK(y == static_cast<u32>((static_cast<float>(-raw) / 0x9C) * 0x7FF + 0x800));
    }
    // beyond the circle's radius the fields saturate instead of wrapping
    CHECK(Status(Stick(32767, -32768)) == Status(Stick(156, -156)));
}

TEST_CASE("the calibration answer") {
    Rig r;
    // all of it
    r.Send({2, 5, 0x00, 0x00, 0x40, 0x00});
    REQUIRE(r.sent.size() == 1);
    Bytes want = {0x11, 0x00, 0x00, 0x40, 0x00};
    want.insert(want.end(), kExpectedCalibration, kExpectedCalibration + 0x40);
    CHECK(r.sent[0] == want);
    CHECK(Bytes(kCalibrationData.begin(), kCalibrationData.end()) ==
          Bytes(kExpectedCalibration, kExpectedCalibration + 0x40));

    // a part: offset 0x10, size 0x20
    r.sent.clear();
    r.Send({2, 0, 0x10, 0x00, 0x20, 0x00});
    want = {0x11, 0x10, 0x00, 0x20, 0x00};
    want.insert(want.end(), kExpectedCalibration + 0x10, kExpectedCalibration + 0x30);
    CHECK(r.sent == std::vector<Bytes>{want});

    // not aligned: the data is read at 0x10 for 0x10, but the answer echoes 0x13 and 0x1F
    r.sent.clear();
    r.Send({2, 0, 0x13, 0x00, 0x1F, 0x00});
    want = {0x11, 0x13, 0x00, 0x1F, 0x00};
    want.insert(want.end(), kExpectedCalibration + 0x10, kExpectedCalibration + 0x20);
    CHECK(r.sent == std::vector<Bytes>{want});

    // size 0: five bytes
    r.sent.clear();
    r.Send({2, 0, 0x00, 0x00, 0x00, 0x00});
    CHECK(r.sent == std::vector<Bytes>{{0x11, 0x00, 0x00, 0x00, 0x00}});
    r.sent.clear();
    r.Send({2, 0, 0x00, 0x00, 0x0F, 0x00});   // 15 aligns down to 0
    CHECK(r.sent == std::vector<Bytes>{{0x11, 0x00, 0x00, 0x0F, 0x00}});

    // the last block, and an offset whose high byte is set (0x0130 is far past the end)
    r.sent.clear();
    r.Send({2, 0, 0x30, 0x00, 0x10, 0x00});
    REQUIRE(r.sent.size() == 1);
    CHECK(r.sent[0].size() == 5 + 0x10);
    CHECK(r.sent[0][5] == 0xFF);
}

TEST_CASE("the calibration request: past the end and the wrong size send nothing") {
    Rig r;
    r.Send({2, 0, 0x30, 0x00, 0x20, 0x00});   // 0x30 + 0x20 > 0x40
    r.Send({2, 0, 0x00, 0x00, 0x50, 0x00});
    r.Send({2, 0, 0x40, 0x00, 0x10, 0x00});
    r.Send({2, 0, 0x30, 0x01, 0x10, 0x00});   // offset 0x0130
    r.Send({2, 0, 0x00, 0x00, 0x10, 0x01});   // size 0x0110
    r.Send({2, 0, 0x00, 0x00, 0x10});         // 5 bytes
    r.Send({2, 0, 0x00, 0x00, 0x10, 0x00, 0}); // 7 bytes
    r.Send({2});
    CHECK(r.sent.empty());
    CHECK(r.timer.schedules == 0);
}

TEST_CASE("unknown and empty requests are ignored") {
    Rig r;
    r.Send({});
    for (u8 id : {0, 3, 4, 5, 0x10, 0x11, 0xFF}) {
        r.Send({id, 1, 2, 3});
        r.Send({id, 1, 2, 3});
    }
    CHECK(r.sent.empty());
    CHECK(r.timer.schedules == 0);
    CHECK_FALSE(r.device.Polling());
    // logged once per request id, not once per request: 7 ids sent twice, and the empty request
    CHECK(r.log.messages.size() == 8);
}

TEST_CASE("no packet comes before the game configures the polling") {
    Rig r;
    r.device.OnConnect();
    CHECK_FALSE(r.timer.Pending());
    CHECK(r.sent.empty());
    CHECK_FALSE(r.timer.Fire());
}

TEST_CASE("packets at the period the game configured, from the input at that moment") {
    Rig r;
    r.device.OnConnect();
    r.Send({1, 16, 0});
    REQUIRE(r.timer.Pending());
    CHECK(r.timer.Delay() == MsTicks(16));
    CHECK(r.timer.Delay() == 4289789);    // 268111856 * 16 / 1000, rounded down
    CHECK(r.device.PollPeriodMs() == 16);
    CHECK(r.sent.empty());                // the first packet comes after one period

    r.input = Stick(78, -78, true, false);
    REQUIRE(r.timer.Fire());
    REQUIRE(r.sent.size() == 1);
    CHECK(r.sent[0] == Bytes{0x10, 0xFF, 0x0B, 0x40, 0xDF, 0x00});
    REQUIRE(r.timer.Pending());           // the next one, after the same period
    CHECK(r.timer.Delay() == MsTicks(16));

    r.input = InputState{};
    for (int i = 0; i < 49; ++i) {
        REQUIRE(r.timer.Fire());
        CHECK(r.timer.Delay() == MsTicks(16));
    }
    REQUIRE(r.sent.size() == 50);
    for (int i = 1; i < 50; ++i) {
        CHECK(r.sent[i] == Bytes{0x10, 0x00, 0x08, 0x80, 0xFF, 0x00});
    }
    CHECK(r.timer.schedules == 51);   // the configuration and one per packet
}

TEST_CASE("a period change replaces the pending event") {
    Rig r;
    r.device.OnConnect();
    r.Send({1, 16, 0});
    REQUIRE(r.timer.Fire());
    REQUIRE(r.sent.size() == 1);
    r.Send({1, 8, 0});
    CHECK(r.timer.Pending());
    CHECK(r.timer.Delay() == MsTicks(8));
    CHECK(r.device.PollPeriodMs() == 8);
    CHECK(r.sent.size() == 1);            // the change itself sends nothing
    REQUIRE(r.timer.Fire());
    REQUIRE(r.timer.Fire());
    CHECK(r.sent.size() == 3);
    CHECK(r.timer.Delay() == MsTicks(8));
    // configuring again with the same period restarts the wait
    r.Send({1, 8, 0});
    CHECK(r.timer.Delay() == MsTicks(8));
    CHECK(r.sent.size() == 3);
    // a request between two packets does not disturb the polling
    r.Send({2, 0, 0, 0, 0x10, 0});
    CHECK(r.sent.size() == 4);
    CHECK(r.timer.Pending());
    CHECK(r.timer.Delay() == MsTicks(8));
}

TEST_CASE("a period of 255 ms and the shortest ones") {
    Rig r;
    r.Send({1, 255, 9});                  // the third byte is not used
    CHECK(r.timer.Delay() == 268111856ull * 255 / 1000);
    r.Send({1, 1, 0});
    CHECK(r.timer.Delay() == 268111);
}

TEST_CASE("a configuration of the wrong size changes nothing") {
    Rig r;
    r.Send({1, 16, 0});
    r.Send({1, 8});
    r.Send({1, 8, 0, 0});
    r.Send({1});
    CHECK(r.device.PollPeriodMs() == 16);
    CHECK(r.timer.Delay() == MsTicks(16));
    CHECK(r.timer.schedules == 1);
}

TEST_CASE("a period of 0 stops the packets instead of spinning") {
    Rig r;
    r.Send({1, 16, 0});
    REQUIRE(r.timer.Fire());
    r.Send({1, 0, 0});
    CHECK_FALSE(r.timer.Pending());
    CHECK_FALSE(r.device.Polling());
    CHECK(r.sent.size() == 1);
    r.Send({1, 4, 0});                    // the game can start it again
    CHECK(r.timer.Pending());
}

TEST_CASE("disconnect stops the packets, and they do not come back by themselves") {
    Rig r;
    r.device.OnConnect();
    r.Send({1, 16, 0});
    REQUIRE(r.timer.Fire());
    REQUIRE(r.sent.size() == 1);
    r.device.OnDisconnect();
    CHECK_FALSE(r.timer.Pending());
    CHECK_FALSE(r.device.Polling());
    CHECK_FALSE(r.timer.Fire());
    CHECK(r.sent.size() == 1);

    r.device.OnConnect();                 // connecting again does not restart them (as in Azahar)
    CHECK_FALSE(r.timer.Pending());
    r.Send({1, 16, 0});
    REQUIRE(r.timer.Fire());
    CHECK(r.sent.size() == 2);

    r.device.OnDisconnect();
    r.device.OnDisconnect();              // twice is harmless
    CHECK_FALSE(r.timer.Pending());
}

TEST_CASE("a callback that was queued before a disconnect does nothing") {
    // A timer whose Cancel cannot take back an event already due.
    struct StaleTimer : Timer {
        std::function<void()> stale;
        std::function<void()> live;
        void Schedule(u64, std::function<void()> cb) override {
            live = std::move(cb);
            stale = live;
        }
        void Cancel() override { live = nullptr; }
    } timer;
    LogCapture log;
    std::vector<Bytes> sent;
    CirclePadPro device([&](std::span<const u8> p) { sent.emplace_back(p.begin(), p.end()); }, timer,
                        [] { return InputState{}; });
    device.OnReceive(Bytes{1, 16, 0});
    device.OnDisconnect();
    timer.stale();
    CHECK(sent.empty());
    CHECK(timer.live == nullptr);
}

TEST_CASE("destroying the device cancels the event") {
    LogCapture log;
    FakeTimer timer;
    {
        CirclePadPro device([](std::span<const u8>) {}, timer, [] { return InputState{}; });
        device.OnReceive(Bytes{1, 16, 0});
        CHECK(timer.Pending());
    }
    CHECK_FALSE(timer.Pending());
}

TEST_CASE("the send callback may disconnect from inside a packet") {
    LogCapture log;
    FakeTimer timer;
    CirclePadPro* self = nullptr;
    int packets = 0;
    CirclePadPro device([&](std::span<const u8>) {
        ++packets;
        if (packets == 3) {
            self->OnDisconnect();
        }
    }, timer, [] { return InputState{}; });
    self = &device;
    device.OnReceive(Bytes{1, 16, 0});
    while (timer.Fire()) {
    }
    CHECK(packets == 3);
    CHECK_FALSE(timer.Pending());
}

TEST_CASE("a device without an input function sends a centred stick") {
    LogCapture log;
    FakeTimer timer;
    std::vector<Bytes> sent;
    CirclePadPro device([&](std::span<const u8> p) { sent.emplace_back(p.begin(), p.end()); }, timer, nullptr);
    device.OnReceive(Bytes{1, 16, 0});
    REQUIRE(timer.Fire());
    CHECK(sent == std::vector<Bytes>{{0x10, 0x00, 0x08, 0x80, 0xFF, 0x00}});
}

}  // namespace mh4n::ir_device
