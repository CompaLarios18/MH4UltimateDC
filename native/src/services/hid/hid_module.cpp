// Derived from Azahar src/core/hle/service/hid/hid.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): the updates of the pad, touch,
// accelerometer and gyroscope blocks and GetStickDirectionState (see the header next to this file).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
#include "hid_module.h"

#include <cmath>
#include <cstring>

namespace mh4n::hid {

namespace {

// Reads the structures out of the memory and writes them back, so that nothing here depends on the
// alignment of the pointer (the memory is the guest's or a host block).
SharedMem Load(const u8* memory) {
    SharedMem mem;
    std::memcpy(&mem, memory, sizeof(mem));
    return mem;
}
void Store(u8* memory, const SharedMem& mem) {
    std::memcpy(memory, &mem, sizeof(mem));
}

}  // namespace

GyroscopeCalibrateParam DefaultGyroscopeCalibrateParam() {
    const s16 param_unit = 6700;  // "an approximate value taken from hw"
    GyroscopeCalibrateParam param{};
    for (auto& axis : param.values) {
        axis[0] = 0;
        axis[1] = param_unit;
        axis[2] = static_cast<s16>(-param_unit);
    }
    return param;
}

DirectionState GetStickDirectionState(s16 circle_pad_x, s16 circle_pad_y) {
    // 30 degree and 60 degree are angular thresholds for directions
    constexpr float TAN30 = 0.577350269f;
    constexpr float TAN60 = 1 / TAN30;
    // a circle pad radius greater than 40 will trigger circle pad direction
    constexpr int CIRCLE_PAD_THRESHOLD_SQUARE = 40 * 40;
    DirectionState state{false, false, false, false};

    if (circle_pad_x * circle_pad_x + circle_pad_y * circle_pad_y > CIRCLE_PAD_THRESHOLD_SQUARE) {
        float t = std::abs(static_cast<float>(circle_pad_y) / circle_pad_x);

        if (circle_pad_x != 0 && t < TAN60) {
            if (circle_pad_x > 0)
                state.right = true;
            else
                state.left = true;
        }

        if (circle_pad_x == 0 || t > TAN30) {
            if (circle_pad_y > 0)
                state.up = true;
            else
                state.down = true;
        }
    }

    return state;
}

s16 CirclePadRaw(float stick) {
    return static_cast<s16>(std::roundf(stick * kMaxCirclePadPosition));
}

void Module::UpdatePad(u8* memory, const InputState& input, u64 tick) {
    if (memory == nullptr) {
        return;
    }
    SharedMem mem = Load(memory);

    // Azahar assigns each button from its device; a bit nobody drives stays clear.
    state_ = input.buttons & kPadButtonMask;

    // These are rounded rather than truncated on actual hardware; the source gives the raw reading.
    // The pad reports the average of the new reading and the previous two, to simulate the
    // sluggishness of a real circle pad (the Theatrhythm games need at least 3).
    const s16 circle_pad_new_x = input.circle_x;
    const s16 circle_pad_new_y = input.circle_y;
    const s16 circle_pad_x = static_cast<s16>(
        (circle_pad_new_x + circle_old_x_[0] + circle_old_x_[1]) / kCirclePadAveraging);
    const s16 circle_pad_y = static_cast<s16>(
        (circle_pad_new_y + circle_old_y_[0] + circle_old_y_[1]) / kCirclePadAveraging);
    circle_old_x_ = {circle_old_x_[1], circle_pad_new_x};
    circle_old_y_ = {circle_old_y_[1], circle_pad_new_y};

    const DirectionState direction = GetStickDirectionState(circle_pad_x, circle_pad_y);
    state_ |= direction.up ? kPadCircleUp : 0;
    state_ |= direction.down ? kPadCircleDown : 0;
    state_ |= direction.left ? kPadCircleLeft : 0;
    state_ |= direction.right ? kPadCircleRight : 0;

    constexpr u32 pad_entries = 8;
    mem.pad.current_state = state_;
    mem.pad.index = next_pad_index_;
    next_pad_index_ = (next_pad_index_ + 1) % pad_entries;

    // Get the previous Pad state
    const u32 last_entry_index = (mem.pad.index - 1) % pad_entries;
    const u32 old_state = mem.pad.entries[last_entry_index].current_state;

    // Compute bitmask with 1s for bits different from the old state
    const u32 changed = state_ ^ old_state;

    // Update the current Pad entry
    PadDataEntry& pad_entry = mem.pad.entries[mem.pad.index];
    pad_entry.current_state = state_;
    pad_entry.delta_additions = changed & state_;
    pad_entry.delta_removals = changed & old_state;
    pad_entry.circle_pad_x = circle_pad_x;
    pad_entry.circle_pad_y = circle_pad_y;

    // If we just updated index 0, provide a new timestamp
    if (mem.pad.index == 0) {
        mem.pad.index_reset_ticks_previous = mem.pad.index_reset_ticks;
        mem.pad.index_reset_ticks = static_cast<s64>(tick);
    }

    constexpr u32 touch_entries = 8;
    mem.touch.index = next_touch_index_;
    next_touch_index_ = (next_touch_index_ + 1) % touch_entries;

    // Azahar's touch device reports 0, 0 when nothing touches the screen; `valid` is a 7-bit field.
    TouchDataEntry& touch_entry = mem.touch.entries[mem.touch.index];
    touch_entry.x = input.touch_down ? input.touch_x : 0;
    touch_entry.y = input.touch_down ? input.touch_y : 0;
    touch_entry.valid = (touch_entry.valid & ~0x7Fu) | (input.touch_down ? 1u : 0u);

    // If we just updated index 0, provide a new timestamp
    if (mem.touch.index == 0) {
        mem.touch.index_reset_ticks_previous = mem.touch.index_reset_ticks;
        mem.touch.index_reset_ticks = static_cast<s64>(tick);
    }

    // The 3D slider: Azahar writes factor_3d / 100 (its default factor is 0).
    mem.pad.sliderstate_3d = 0.0f;

    Store(memory, mem);
}

void Module::UpdateAccelerometer(u8* memory, const InputState& input, u64 tick) {
    if (memory == nullptr) {
        return;
    }
    SharedMem mem = Load(memory);

    mem.accelerometer.index = next_accelerometer_index_;
    next_accelerometer_index_ = (next_accelerometer_index_ + 1) % 8;

    AccelerometerDataEntry& entry = mem.accelerometer.entries[mem.accelerometer.index];
    entry.x = input.accel[0];
    entry.y = input.accel[1];
    entry.z = input.accel[2];

    // Make up "raw" entry: "approximately, but not exactly, as twice as corresponding entries (or
    // with a minus sign)".
    mem.accelerometer.raw_entry.x = static_cast<s16>(-2 * entry.x);
    mem.accelerometer.raw_entry.z = static_cast<s16>(2 * entry.y);
    mem.accelerometer.raw_entry.y = static_cast<s16>(-2 * entry.z);

    // If we just updated index 0, provide a new timestamp
    if (mem.accelerometer.index == 0) {
        mem.accelerometer.index_reset_ticks_previous = mem.accelerometer.index_reset_ticks;
        mem.accelerometer.index_reset_ticks = static_cast<s64>(tick);
    }

    Store(memory, mem);
}

void Module::UpdateGyroscope(u8* memory, const InputState& input, u64 tick) {
    if (memory == nullptr) {
        return;
    }
    SharedMem mem = Load(memory);

    mem.gyroscope.index = next_gyroscope_index_;
    next_gyroscope_index_ = (next_gyroscope_index_ + 1) % 32;

    GyroscopeDataEntry& entry = mem.gyroscope.entries[mem.gyroscope.index];
    entry.x = input.gyro[0];
    entry.y = input.gyro[1];
    entry.z = input.gyro[2];

    // Make up "raw" entry
    mem.gyroscope.raw_entry.x = entry.x;
    mem.gyroscope.raw_entry.z = static_cast<s16>(-entry.y);
    mem.gyroscope.raw_entry.y = entry.z;

    // If we just updated index 0, provide a new timestamp
    if (mem.gyroscope.index == 0) {
        mem.gyroscope.index_reset_ticks_previous = mem.gyroscope.index_reset_ticks;
        mem.gyroscope.index_reset_ticks = static_cast<s64>(tick);
    }

    Store(memory, mem);
}

}  // namespace mh4n::hid
