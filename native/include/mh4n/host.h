// What the runtime gives the services from the host (N1-09): input, audio and the GPU, behind
// small interfaces so that headless runs, tests and the platform layer (N2b) plug in the same way.
//
// An implementation registers a factory by name from its own module, like gpu::GpuFactory:
//
//     MH4N_REGISTER(InputSourceFactory, "script", [](Runtime& rt) { ... });
//     MH4N_REGISTER(AudioSinkFactory, "null", [](Runtime&) { return std::make_unique<NullSink>(); });
//
// The runtime's `devices` boot step creates the ones named by `--gpu` (default `null`),
// `--input-source` (default `script`) and `--audio` (default `null`). A factory reads its options
// from `rt.GetOptions()` and subscribes to frames with `rt.OnFrame(...)`. A name nothing has
// registered leaves that interface empty (nullptr in HostInterfaces) with an Info line.
//
// The PAD bit values are those of the HID shared memory (3dbrew "HID Shared Memory").
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "mh4n/gpu.h"
#include "mh4n/types.h"

namespace mh4n {

class Runtime;

// The buttons of InputState::buttons, as the PAD word of HID's shared memory has them. ZL and ZR
// (Circle Pad Pro, New 3DS) are separate fields of InputState.
inline constexpr u32 kPadA = 1u << 0;
inline constexpr u32 kPadB = 1u << 1;
inline constexpr u32 kPadSelect = 1u << 2;
inline constexpr u32 kPadStart = 1u << 3;
inline constexpr u32 kPadRight = 1u << 4;
inline constexpr u32 kPadLeft = 1u << 5;
inline constexpr u32 kPadUp = 1u << 6;
inline constexpr u32 kPadDown = 1u << 7;
inline constexpr u32 kPadR = 1u << 8;
inline constexpr u32 kPadL = 1u << 9;
inline constexpr u32 kPadX = 1u << 10;
inline constexpr u32 kPadY = 1u << 11;

// One sample of the console's inputs.
struct InputState {
    u32 buttons = 0;          // kPad* bits
    s16 circle_x = 0;         // the circle pad, in the units of HID's shared memory (about -156..156)
    s16 circle_y = 0;
    bool touch_down = false;  // the touch screen, in pixels of the bottom screen (320 x 240)
    u16 touch_x = 0;
    u16 touch_y = 0;
    s16 cstick_x = 0;         // the Circle Pad Pro's stick (New 3DS C-stick), same units as the circle pad
    s16 cstick_y = 0;
    bool zl = false;          // ZL and ZR (Circle Pad Pro, New 3DS)
    bool zr = false;
    s16 accel[3] = {0, 0, 0}; // accelerometer, raw sensor units
    s16 gyro[3] = {0, 0, 0};  // gyroscope, raw sensor units
    friend bool operator==(const InputState&, const InputState&) = default;
};

class InputSource {
public:
    virtual ~InputSource() = default;
    // The inputs at game tick `tick` (268,111,856 per second). Called on the scheduler thread.
    virtual InputState Sample(u64 tick) = 0;
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Interleaved signed 16-bit stereo (left, right), `frames` sample frames, at the DSP's output
    // rate. Called on the scheduler thread; a real sink copies the samples and returns.
    virtual void Queue(const s16* stereo, std::size_t frames) = 0;
};

// Raw pointers: the Runtime owns what the factories made. Any of them can be nullptr.
struct HostInterfaces {
    gpu::Gpu* gpu = nullptr;
    InputSource* input = nullptr;
    AudioSink* audio = nullptr;
};

using InputSourceFactory = std::function<std::unique_ptr<InputSource>(Runtime&)>;
using AudioSinkFactory = std::function<std::unique_ptr<AudioSink>(Runtime&)>;

}  // namespace mh4n
