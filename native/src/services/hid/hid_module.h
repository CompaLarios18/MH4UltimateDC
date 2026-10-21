// Derived from Azahar src/core/hle/service/hid/hid.h, src/core/hle/service/hid/hid.cpp and
// src/common/settings.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each says
// "GPLv2 or any later version"):
// the layout of HID's shared memory (SharedMem and the entries in it), the update periods, the
// update of the pad, touch, accelerometer and gyroscope blocks (the ring buffers, the deltas, the
// index-reset tick stamps, the averaging of the circle pad, the raw entries), GetStickDirectionState,
// the calibration values of the gyroscope, and the default of the 3D factor (settings.h).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.h, hid.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (settings.h)
//
// The pure part of the HID module (N1-16): what is written into the shared memory at each update.
// No kernel objects, no timers: the caller (hid_state.h) owns the memory, the events and the timer
// queue and calls these with the input sampled for the tick the update is due at.
//
// Departures from Azahar, all so that nothing the game can see depends on the host:
//  - the gyroscope is not stretched by the emulator's frame-time scale (Azahar multiplies by
//    perf_stats->GetLastFrameTimeScale(), which follows the host's speed): the source's raw values
//    are written as they are;
//  - the accelerometer and gyroscope values come from InputState in raw sensor units, where Azahar
//    scales its motion device's floats (accelerometer: * 512); a source that wants Azahar's scaling
//    does it itself;
//  - the tick stamps are the ticks the update was due at, not the clock's reading when the timer
//    queue got to it (they are equal in Azahar, whose events run exactly on time).
#pragma once

#include <array>
#include <cstddef>

#include "mh4n/clock.h"
#include "mh4n/host.h"
#include "mh4n/types.h"

namespace mh4n::hid {

// "Updating period for each HID device. These empirical values are measured from a 11.2 3DS."
inline constexpr u64 kPadUpdateTicks = kTicksPerSecond / 234;
inline constexpr u64 kAccelerometerUpdateTicks = kTicksPerSecond / 104;
inline constexpr u64 kGyroscopeUpdateTicks = kTicksPerSecond / 101;

// Azahar's SharedMemory is 0x1000 bytes; the game reads the first 0x2b0 (the structures below end
// at 0x238, the debug pad's block, which Azahar does not fill, is zeros).
inline constexpr u32 kSharedMemorySize = 0x1000;
inline constexpr u32 kReadableSize = 0x2B0;

// PadState bits beyond host.h's kPad* (HID has the debug button, a GPIO and the circle pad's
// directions).
inline constexpr u32 kPadDebug = 1u << 12;
inline constexpr u32 kPadGpio14 = 1u << 13;
inline constexpr u32 kPadCircleRight = 1u << 28;
inline constexpr u32 kPadCircleLeft = 1u << 29;
inline constexpr u32 kPadCircleUp = 1u << 30;
inline constexpr u32 kPadCircleDown = 1u << 31;
// The bits a button of the pad sets: A to Y, debug and GPIO14 (the circle pad's directions come
// from its position).
inline constexpr u32 kPadButtonMask = 0x3FFF;

// "0x9A seems to be the calibrated limit of the circle pad"
inline constexpr int kMaxCirclePadPosition = 0x9A;
// The number of raw circle pad readings averaged (the new one and the previous two).
inline constexpr int kCirclePadAveraging = 3;

// ---- the shared memory ---------------------------------------------------------------------------

struct PadDataEntry {
    u32 current_state;
    u32 delta_additions;
    u32 delta_removals;
    s16 circle_pad_x;
    s16 circle_pad_y;
};

struct TouchDataEntry {
    u16 x;
    u16 y;
    u32 valid;  // bits 0-6: 1 when the entry holds a touch
};

struct AccelerometerDataEntry {
    s16 x;
    s16 y;
    s16 z;
};

struct GyroscopeDataEntry {
    s16 x;
    s16 y;
    s16 z;
};

struct SharedMem {
    struct {
        s64 index_reset_ticks;
        s64 index_reset_ticks_previous;
        u32 index;
        u32 padding0;
        float sliderstate_3d;
        u32 current_state;
        u32 raw_circle_pad_data;  // not implemented in Azahar either
        u32 padding1;
        PadDataEntry entries[8];
    } pad;
    struct {
        s64 index_reset_ticks;
        s64 index_reset_ticks_previous;
        u32 index;
        u32 padding0;
        TouchDataEntry raw_entry;  // not implemented in Azahar either
        TouchDataEntry entries[8];
    } touch;
    struct {
        s64 index_reset_ticks;
        s64 index_reset_ticks_previous;
        u32 index;
        u32 padding0;
        AccelerometerDataEntry raw_entry;
        u8 padding1[2];
        AccelerometerDataEntry entries[8];
    } accelerometer;
    struct {
        s64 index_reset_ticks;
        s64 index_reset_ticks_previous;
        u32 index;
        u32 padding0;
        GyroscopeDataEntry raw_entry;
        u8 padding1[2];
        GyroscopeDataEntry entries[32];
    } gyroscope;
};

inline constexpr u32 kPadOffset = 0x00;
inline constexpr u32 kTouchOffset = 0xA8;
inline constexpr u32 kAccelerometerOffset = 0x108;
inline constexpr u32 kGyroscopeOffset = 0x158;
inline constexpr u32 kDebugPadOffset = 0x238;
static_assert(offsetof(SharedMem, pad) == kPadOffset);
static_assert(offsetof(SharedMem, touch) == kTouchOffset);
static_assert(offsetof(SharedMem, accelerometer) == kAccelerometerOffset);
static_assert(offsetof(SharedMem, gyroscope) == kGyroscopeOffset);
static_assert(sizeof(SharedMem) == kDebugPadOffset);
static_assert(offsetof(SharedMem, pad.entries) == 0x28);
static_assert(offsetof(SharedMem, touch.entries) == kTouchOffset + 0x20);
static_assert(offsetof(SharedMem, accelerometer.entries) == kAccelerometerOffset + 0x20);
static_assert(offsetof(SharedMem, gyroscope.entries) == kGyroscopeOffset + 0x20);
static_assert(sizeof(PadDataEntry) == 0x10);
static_assert(sizeof(TouchDataEntry) == 8);

// The 3 fields x, y, z of GetGyroscopeLowCalibrateParam, each {zero point, positive unit point,
// negative unit point}.
struct GyroscopeCalibrateParam {
    s16 values[3][3];
};
static_assert(sizeof(GyroscopeCalibrateParam) == 18);
GyroscopeCalibrateParam DefaultGyroscopeCalibrateParam();
// "got from hwtest GetGyroscopeLowRawToDpsCoefficient call"
inline constexpr float kGyroscopeCoefficient = 14.375f;

struct DirectionState {
    bool up;
    bool down;
    bool left;
    bool right;
};
// Translates the circle pad's position to directions: a radius above 40, split by the 30 and 60
// degree lines.
DirectionState GetStickDirectionState(s16 circle_pad_x, s16 circle_pad_y);

// The circle pad position a script's -1..1 stick makes: rounded, not truncated ("these are rounded
// rather than truncated on actual hardware").
s16 CirclePadRaw(float stick);

// ---- the updates ---------------------------------------------------------------------------------

// The state that lives between updates: where the next entry goes, the previous circle pad readings,
// how many times the sensors were enabled. `memory` below is the block of kSharedMemorySize bytes (a
// null pointer does nothing).
class Module {
public:
    // One pad update (Azahar's UpdatePadCallback without the event and the reschedule): the pad
    // entry, then the touch entry, with `tick` as the time stamp of index 0.
    void UpdatePad(u8* memory, const InputState& input, u64 tick);
    void UpdateAccelerometer(u8* memory, const InputState& input, u64 tick);
    void UpdateGyroscope(u8* memory, const InputState& input, u64 tick);

    // EnableAccelerometer and the others count; true when the update event has to be started (the
    // count became 1) or stopped (it became 0). The count can go below zero if the game disables more
    // than it enabled, as in Azahar.
    bool EnableAccelerometer() { return ++enable_accelerometer_ == 1; }
    bool DisableAccelerometer() { return --enable_accelerometer_ == 0; }
    bool EnableGyroscope() { return ++enable_gyroscope_ == 1; }
    bool DisableGyroscope() { return --enable_gyroscope_ == 0; }
    int AccelerometerCount() const { return enable_accelerometer_; }
    int GyroscopeCount() const { return enable_gyroscope_; }

    // The PadState written by the last pad update.
    u32 State() const { return state_; }

private:
    u32 state_ = 0;
    std::array<s16, kCirclePadAveraging - 1> circle_old_x_{};
    std::array<s16, kCirclePadAveraging - 1> circle_old_y_{};
    u32 next_pad_index_ = 0;
    u32 next_touch_index_ = 0;
    u32 next_accelerometer_index_ = 0;
    u32 next_gyroscope_index_ = 0;
    int enable_accelerometer_ = 0;
    int enable_gyroscope_ = 0;
};

}  // namespace mh4n::hid
