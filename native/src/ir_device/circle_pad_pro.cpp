// Derived from Azahar src/core/hle/service/ir/extra_hid.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): ExtraHID's calibration
// data, HandleConfigureHIDPollingRequest, HandleReadCalibrationDataRequest, OnReceive and SendHIDStatus
// (the artic controller's branch, which converts raw stick units).
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project
#include "ir_device/circle_pad_pro.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/log.h"

namespace mh4n::ir_device {
namespace {

constexpr const char* kLog = "Service.IR";

// Azahar's msToCycles(int ms): BASE_CLOCK_RATE_ARM11 * ms / 1000.
constexpr u64 MsToTicks(u8 ms) {
    return kTicksPerSecond * static_cast<u64>(ms) / 1000;
}

}  // namespace

// "The data below was retrieved from a New 3DS"
const std::array<u8, kCalibrationSize> kCalibrationData = {{
    0x00, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,  // 0x00
    0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0xF5,  // 0x08
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,  // 0x10
    0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,  // 0x18
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,  // 0x20
    0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,  // 0x28
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,  // 0x30
    0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,  // 0x38
}};

std::array<u8, kStatusSize> BuildStatus(const InputState& input) {
    constexpr u32 kZlBit = 1u << 5;
    constexpr u32 kZrBit = 1u << 6;
    constexpr u32 kRBit = 1u << 7;
    constexpr int kCenter = 0x800;
    // "this value is not accurately measured. We currently assume that the axis can take values in
    // the whole range of a 12-bit integer."
    constexpr int kRadius = 0x7FF;

    // Azahar's artic branch: (raw / MAX_CSTICK_RADIUS) * C_STICK_RADIUS + C_STICK_CENTER, in float,
    // truncated. It does not clamp (a raw value never exceeds the radius); a value that does is held
    // to the circle's square here, because the 12-bit field would wrap.
    const auto field = [&](s16 raw) {
        const int held = std::clamp<int>(raw, -kCStickRadiusRaw, kCStickRadiusRaw);
        const float f =
            (static_cast<float>(held) / static_cast<float>(kCStickRadiusRaw)) * static_cast<float>(kRadius) +
            static_cast<float>(kCenter);
        return static_cast<u32>(f) & 0xFFF;
    };

    const u32 head = static_cast<u32>(ResponseId::PollHid) | (field(input.cstick_x) << 8) |
                     (field(input.cstick_y) << 20);
    const u8 buttons = static_cast<u8>(kBatteryLevel | (input.zl ? 0 : kZlBit) | (input.zr ? 0 : kZrBit) |
                                       kRBit);
    std::array<u8, kStatusSize> packet{};
    packet[0] = static_cast<u8>(head);
    packet[1] = static_cast<u8>(head >> 8);
    packet[2] = static_cast<u8>(head >> 16);
    packet[3] = static_cast<u8>(head >> 24);
    packet[4] = buttons;
    packet[5] = 0;  // unknown
    return packet;
}

CirclePadPro::CirclePadPro(SendFunc send, Timer& timer, InputFunc input)
    : send_(std::move(send)), timer_(timer), input_(std::move(input)) {}

CirclePadPro::~CirclePadPro() {
    OnDisconnect();
}

void CirclePadPro::OnConnect() {}

void CirclePadPro::OnDisconnect() {
    if (polling_) {
        timer_.Cancel();
    }
    polling_ = false;
    ++generation_;
}

void CirclePadPro::OnReceive(std::span<const u8> data) {
    if (data.empty()) {  // Azahar reads data[0] of an empty request
        if (!logged_size_[0]) {
            logged_size_[0] = true;
            MH4N_LOG(Error, kLog, "Empty request");
        }
        return;
    }
    switch (static_cast<RequestId>(data[0])) {
    case RequestId::ConfigureHidPolling:
        HandleConfigureHidPolling(data);
        break;
    case RequestId::ReadCalibrationData:
        HandleReadCalibrationData(data);
        break;
    default:
        if (!logged_unknown_[data[0]]) {  // Azahar logs every one; once per request id here
            logged_unknown_[data[0]] = true;
            MH4N_LOG(Error, kLog, "Unknown request {:#04x} ({} bytes)", data[0], data.size());
        }
        break;
    }
}

void CirclePadPro::HandleConfigureHidPolling(std::span<const u8> request) {
    if (request.size() != 3) {
        if (!logged_size_[request[0]]) {
            logged_size_[request[0]] = true;
            MH4N_LOG(Error, kLog, "Wrong request size ({}) for request {}", request.size(), request[0]);
        }
        return;
    }
    // Change HID input polling interval
    if (polling_) {
        timer_.Cancel();
    }
    ++generation_;
    period_ms_ = request[1];
    if (period_ms_ == 0) {
        // Azahar schedules an event after 0 cycles that reschedules itself after 0 cycles: it never
        // lets time advance. Here a period of 0 stops the packets.
        polling_ = false;
        MH4N_LOG(Warning, kLog, "Polling period 0: no status packets");
        return;
    }
    polling_ = true;
    Schedule();
}

void CirclePadPro::Schedule() {
    const u64 generation = generation_;
    timer_.Schedule(MsToTicks(period_ms_), [this, generation] {
        if (generation != generation_ || !polling_) {
            return;
        }
        SendHidStatus();
        if (generation == generation_ && polling_) {  // the send callback may have disconnected
            Schedule();
        }
    });
}

void CirclePadPro::HandleReadCalibrationData(std::span<const u8> request) {
    // struct ReadCalibrationDataRequest { u8 request_id; u8 expected_response_time; u16_le offset;
    //                                     u16_le size; }
    constexpr std::size_t kRequestSize = 6;
    if (request.size() != kRequestSize) {
        if (!logged_size_[request[0]]) {
            logged_size_[request[0]] = true;
            MH4N_LOG(Error, kLog, "Wrong request size ({}) for request {}", request.size(), request[0]);
        }
        return;
    }
    const u16 raw_offset = static_cast<u16>(request[2] | (request[3] << 8));
    const u16 raw_size = static_cast<u16>(request[4] | (request[5] << 8));
    const u16 offset = raw_offset & ~u16{15};  // Common::AlignDown(x, 16)
    const u16 size = raw_size & ~u16{15};

    if (static_cast<std::size_t>(offset + size) > kCalibrationData.size()) {
        MH4N_LOG(Error, kLog, "Read beyond the end of calibration data! (offset={}, size={})", offset, size);
        return;
    }

    // The response echoes the offset and size as the game wrote them, not aligned.
    std::vector<u8> response(5 + size);
    response[0] = static_cast<u8>(ResponseId::ReadCalibrationData);
    response[1] = request[2];
    response[2] = request[3];
    response[3] = request[4];
    response[4] = request[5];
    std::memcpy(&response[5], kCalibrationData.data() + offset, size);
    send_(response);
}

void CirclePadPro::SendHidStatus() {
    const InputState input = input_ ? input_() : InputState{};
    const auto packet = BuildStatus(input);
    send_(packet);
}

}  // namespace mh4n::ir_device
