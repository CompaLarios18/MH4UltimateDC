// Derived from Azahar src/audio_core/hle/effects.h, src/audio_core/hle/effects.cpp and
// src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each
// of them says "Licensed under GPLv2 or any later version"): DelayEffect and ReverbEffect (their state,
// ParseConfig and ProcessFrame) and the layout of DspConfiguration::DelayEffect and ::ReverbEffect.
// Copyright 2026 Citra Emulator Project / Azahar Emulator Project (effects.h, effects.cpp)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
//
// The effects of the HLE DSP's mixers (N3-03): the delay with feedback, and the reverb, which Azahar
// does not implement (it does nothing, and so does this). Each effect applies to one of the two
// auxiliary intermediate mixes.
//
// Departure from Azahar (D78: its result depends on the host): the delay's output is clamped to the s32
// range and then converted; a NaN (an unstable filter: |b| above 1 grows without bound, and infinity
// minus infinity is NaN) passes std::clamp and its conversion is undefined in Azahar's C++. Here the
// conversion is FloatToS32 (types.h): a NaN gives INT32_MIN, as on x86.
#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "dsp_hle/types.h"

namespace mh4n::dsp_hle {

// DspConfiguration::DelayEffect (20 bytes, in the DSP configuration at audio::dspconfig::kDelayEffect).
struct DelayEffectConfig {
    // Set by the application when it updates the fields; the DSP clears it.
    u16 dirty_raw;
    u16 enable;
    u16 padding;
    u16 outputs;
    u32 work_buffer_address;  // u32_dsp: the 16-bit halves swapped
    u16 frame_count;          // frames to delay by
    s16 g;                    // fixed point with 7 fractional bits
    s16 a;
    s16 b;
};
static_assert(sizeof(DelayEffectConfig) == 20);

// DspConfiguration::ReverbEffect: 26 words nobody has figured out.
struct ReverbEffectConfig {
    u16 words[26];
};
static_assert(sizeof(ReverbEffectConfig) == 52);

class DelayEffect final {
public:
    void Reset();

    // Reads the block (clearing its dirty word, as the DSP does) when its dirty word is not 0.
    void ParseConfig(DelayEffectConfig& config, std::size_t index);

    void ProcessFrame(QuadFrame32& frame);

private:
    // Set a max delay to prevent garbage data allocating too much memory: 4 seconds (Azahar's).
    static constexpr u32 kMaxDelaySamples = 4 * kNativeSampleRate;

    void Resize(u32 samples);

    bool enabled_ = false;
    u32 delay_samples_ = 0;

    // Coefficients, converted from the s16 Q7 values in shared memory.
    float g_ = 0.0f;
    float a_ = 0.0f;
    float b_ = 0.0f;

    std::array<std::vector<float>, 4> delay_output_{};
    std::array<float, 4> last_output_{};
    std::size_t delay_pos_ = 0;
};

// Azahar: "TODO: Figure out the reverb effect." It does nothing.
class ReverbEffect final {
public:
    void ParseConfig(const ReverbEffectConfig& config, std::size_t index);
    void ProcessFrame(QuadFrame32&) {}
};

}  // namespace mh4n::dsp_hle
