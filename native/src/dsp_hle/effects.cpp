// Derived from Azahar src/audio_core/hle/effects.cpp, src/audio_core/hle/effects.h and
// src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each
// of them says "Licensed under GPLv2 or any later version"): DelayEffect::Reset, Resize, ParseConfig and
// ProcessFrame, ReverbEffect::ParseConfig.
// Copyright 2026 Citra Emulator Project / Azahar Emulator Project (effects.cpp, effects.h)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
//
// The effects of the HLE DSP's mixers (N3-03); see effects.h.
#include "dsp_hle/effects.h"

#include <algorithm>

#include "mh4n/log.h"

namespace mh4n::dsp_hle {

namespace {

constexpr const char* kLog = "Audio.DSP.HLE";

s32 ClampToS32(float value) {
    // Clamp to valid S32 range.
    constexpr float min = -2147483648.0f;
    constexpr float max = 2147483520.0f;
    return FloatToS32(std::clamp(value, min, max));
}

}  // namespace

void DelayEffect::Reset() {
    enabled_ = false;
    delay_samples_ = 0;
    g_ = a_ = b_ = 0.0f;
    for (auto& channel : delay_output_) {
        channel.clear();
    }
    last_output_.fill(0.0f);
    delay_pos_ = 0;
}

void DelayEffect::Resize(u32 samples) {
    if (samples == delay_samples_ && !delay_output_[0].empty()) {
        return;
    }
    delay_samples_ = samples;
    delay_pos_ = 0;
    last_output_.fill(0.0f);
    for (auto& channel : delay_output_) {
        channel.assign(samples, 0.0f);
    }
}

void DelayEffect::ParseConfig(DelayEffectConfig& config, std::size_t index) {
    if (!config.dirty_raw) {
        return;
    }

    const bool was_enabled = enabled_;
    enabled_ = config.enable != 0;

    if (!enabled_) {
        config.dirty_raw = 0;
        if (was_enabled) {
            MH4N_LOG(Debug, kLog, "delay_effect[{}] disabled", index);
            Reset();
        }
        return;
    }

    u32 samples = static_cast<u32>(config.frame_count) * static_cast<u32>(kSamplesPerFrame);
    if (samples == 0) {
        // Delay of zero samples is invalid as it would make the DSP read the sample it is
        // about to write. Figure out what real HW does in this case.
        MH4N_LOG(Debug, kLog, "delay_effect[{}] enabled with frame_count=0, bypassing", index);
        enabled_ = false;
        config.dirty_raw = 0;
        Reset();
        return;
    }
    if (samples > kMaxDelaySamples) {
        MH4N_LOG(Warning, kLog, "delay_effect[{}] frame_count={} exceeds sane range, clamping", index,
                 static_cast<u32>(config.frame_count));
        samples = kMaxDelaySamples;
    }

    // Convert s16 with 7 fractional bits to float.
    constexpr float q7_scale = 1.0f / 128.0f;

    g_ = static_cast<s16>(config.g) * q7_scale;
    a_ = static_cast<s16>(config.a) * q7_scale;
    b_ = static_cast<s16>(config.b) * q7_scale;

    Resize(samples);

    MH4N_LOG(Debug, kLog, "delay_effect[{}] enabled frames={} samples={} g={} a={} b={}", index,
             static_cast<u32>(config.frame_count), samples, g_, a_, b_);

    // We do not need the work buffer provided by the application as it is kept track separately.

    config.dirty_raw = 0;
}

void DelayEffect::ProcessFrame(QuadFrame32& frame) {
    if (!enabled_) {
        return;
    }

    for (std::size_t sample = 0; sample < kSamplesPerFrame; sample++) {
        const std::size_t pos = (delay_pos_ + sample) % delay_samples_;
        for (std::size_t channel = 0; channel < 4; channel++) {
            // y[n] = b y[n-1] + a ( x[n-N] - g y[n-N] ), with v[n-N] = x[n-N] - g y[n-N]
            // (Azahar's derivation of the transfer function H(z) = a z^-N / (1 - b z^-1 + a g z^-N)).
            const float x = static_cast<float>(frame[sample][channel]);
            const float delayed = delay_output_[channel][pos];

            // y[n] = b y[n-1] + a v[n-N]
            const float y = b_ * last_output_[channel] + a_ * delayed;
            // v[n] = x[n] - g y[n]
            delay_output_[channel][pos] = x - g_ * y;

            last_output_[channel] = y;
            frame[sample][channel] = ClampToS32(y);
        }
    }

    delay_pos_ = (delay_pos_ + kSamplesPerFrame) % delay_samples_;
}

void ReverbEffect::ParseConfig(const ReverbEffectConfig&, std::size_t index) {
    // Azahar logs the configuration for now ("TODO: Implement").
    MH4N_LOG(Debug, kLog, "(stubbed) reverb_effect[{}]", index);
}

}  // namespace mh4n::dsp_hle
