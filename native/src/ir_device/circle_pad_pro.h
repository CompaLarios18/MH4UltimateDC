// Derived from Azahar src/core/hle/service/ir/extra_hid.cpp and src/core/hle/service/ir/extra_hid.h
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "Licensed under GPLv2 or
// any later version"): the request and response ids, the layout of the status packet, the
// calibration data and the handling of the two requests.
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project
//
// The Circle Pad Pro (N3-08), the device Azahar connects as device 1 of ir:USER whatever the input
// settings: the IR protocol only. It answers the requests the game sends with SendIrNop, and sends a
// status packet (the C-stick, ZL, ZR, R, the battery) every period the game configured. Packets
// leave through the `send` callback as bare payloads; the service (N3-12) wraps them into the
// receive buffer's packets (Azahar's IR_USER::PutToReceive). Time is the service's: a `Timer` bound
// to its timer queue, so this class runs on the scheduler's thread and holds no clock of its own.
#pragma once

#include <array>
#include <functional>
#include <span>

#include "mh4n/host.h"
#include "mh4n/types.h"

namespace mh4n::ir_device {

// The service's timer queue, for one event at a time: Schedule replaces nothing (the device always
// calls Cancel first); Cancel drops the pending callback, if any. The callback runs once, `delay_ticks`
// ticks of the game clock after the call. The device schedules the next status packet from inside the
// callback with the whole period, so a timer queue whose clock is already past the event's due time
// should take its lateness off the delay (Azahar's `msToCycles(period) - cycles_late`).
struct Timer {
    virtual ~Timer() = default;
    virtual void Schedule(u64 delay_ticks, std::function<void()> callback) = 0;
    virtual void Cancel() = 0;
};

enum class RequestId : u8 {
    ConfigureHidPolling = 1,  // [1, period in ms, unknown]
    ReadCalibrationData = 2,  // [2, expected response time, offset u16, size u16]
};

enum class ResponseId : u8 {
    PollHid = 0x10,              // [0x10, stick x/y as two 12-bit fields, battery and buttons, 0]
    ReadCalibrationData = 0x11,  // [0x11, offset u16, size u16, data]
};

inline constexpr std::size_t kStatusSize = 6;
inline constexpr std::size_t kCalibrationSize = 0x40;
inline constexpr int kCStickRadiusRaw = 0x9C;  // the units of InputState::cstick_* (ir_rst.cpp, extra_hid.cpp)
inline constexpr u8 kBatteryLevel = 0x1F;

// The calibration data "retrieved from a New 3DS".
extern const std::array<u8, kCalibrationSize> kCalibrationData;

// The status packet for an input: the C-stick as two little-endian 12-bit fields (centre 0x800,
// radius 0x7FF), the battery level 0x1F, and for ZL, ZR and R the bit set when the button is NOT
// pressed (R is never pressed). The stick is `cstick_*` / 0x9C, clamped to the pad's circle square.
std::array<u8, kStatusSize> BuildStatus(const InputState& input);

class CirclePadPro {
public:
    using SendFunc = std::function<void(std::span<const u8>)>;
    using InputFunc = std::function<InputState()>;

    // `input` is called for every status packet, on the thread that runs the timer's callbacks.
    CirclePadPro(SendFunc send, Timer& timer, InputFunc input);
    ~CirclePadPro();
    CirclePadPro(const CirclePadPro&) = delete;
    CirclePadPro& operator=(const CirclePadPro&) = delete;

    void OnConnect();
    // Stops the status packets. They do not come back until the game configures the polling again.
    void OnDisconnect();
    // One request of the game (the payload of SendIrNop).
    void OnReceive(std::span<const u8> data);

    // The period the game configured, in ms (0: none yet), and whether packets are being sent.
    u8 PollPeriodMs() const { return period_ms_; }
    bool Polling() const { return polling_; }

private:
    void HandleConfigureHidPolling(std::span<const u8> request);
    void HandleReadCalibrationData(std::span<const u8> request);
    void SendHidStatus();
    void Schedule();

    SendFunc send_;
    Timer& timer_;
    InputFunc input_;
    u8 period_ms_ = 0;
    bool polling_ = false;
    u64 generation_ = 0;  // a callback of an earlier Schedule does nothing
    std::array<bool, 256> logged_unknown_{};
    std::array<bool, 256> logged_size_{};
};

}  // namespace mh4n::ir_device
