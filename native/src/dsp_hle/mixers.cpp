// Derived from Azahar src/audio_core/hle/mixers.cpp, src/audio_core/hle/mixers.h and
// src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each
// of them says "Licensed under GPLv2 or any later version"): Mixers::Reset, Sleep, Wakeup, Tick,
// ParseConfig, AuxReturn, AuxSend, ApplyEffects, MixCurrentFrame and DownmixAndMixIntoCurrentFrame.
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (mixers.cpp, mixers.h)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
//
// The mixers of the HLE DSP (N3-03); see mixers.h.
#include "dsp_hle/mixers.h"

#include <algorithm>

#include "mh4n/log.h"

namespace mh4n::dsp_hle {

namespace {

constexpr const char* kLog = "Audio.DSP.HLE";

s16 ClampToS16(s32 value) {
    return static_cast<s16>(std::clamp(value, -32768, 32767));
}

std::array<s16, 2> AddAndClampToS16(const std::array<s16, 2>& a, const std::array<s16, 2>& b) {
    return {ClampToS16(static_cast<s32>(a[0]) + static_cast<s32>(b[0])),
            ClampToS16(static_cast<s32>(a[1]) + static_cast<s32>(b[1]))};
}

}  // namespace

void Mixers::Reset() {
    current_frame_.fill({});
    state_ = {};
}

void Mixers::Sleep() {
    backup_state_ = state_;
    backup_frame_ = current_frame_;
}

void Mixers::Wakeup() {
    state_ = backup_state_;
    current_frame_ = backup_frame_;
    backup_state_ = {};
    backup_frame_.fill({});
}

DspStatus Mixers::Tick(DspConfiguration& config, const IntermediateMixSamples& read_samples,
                       IntermediateMixSamples& write_samples, const std::array<QuadFrame32, 3>& input) {
    ParseConfig(config);

    AuxReturn(read_samples);
    AuxSend(write_samples, input);

    ApplyEffects();

    MixCurrentFrame();

    return DspStatus{0, 0};  // GetCurrentStatus: unknown = 0, dropped_frames = 0
}

void Mixers::ParseConfig(DspConfiguration& config) {
    if (!config.dirty_raw) {
        return;
    }

    const auto take = [&config](u32 bit) {
        const bool dirty = (config.dirty_raw & bit) != 0;
        config.dirty_raw &= ~bit;
        return dirty;
    };

    if (take(config_dirty::kAuxBusEnable0)) {
        state_.aux_bus_enable[0] = config.aux_bus_enable[0] != 0;
        MH4N_LOG(Trace, kLog, "mixers aux_bus_enable[0] = {}", config.aux_bus_enable[0]);
    }

    if (take(config_dirty::kAuxBusEnable1)) {
        state_.aux_bus_enable[1] = config.aux_bus_enable[1] != 0;
        MH4N_LOG(Trace, kLog, "mixers aux_bus_enable[1] = {}", config.aux_bus_enable[1]);
    }

    if (take(config_dirty::kMasterVolume)) {
        state_.intermediate_mixer_volume[0] = config.master_volume;
        MH4N_LOG(Trace, kLog, "mixers master_volume = {}", config.master_volume);
    }

    if (take(config_dirty::kAuxReturnVolume0)) {
        state_.intermediate_mixer_volume[1] = config.aux_return_volume[0];
        MH4N_LOG(Trace, kLog, "mixers aux_return_volume[0] = {}", config.aux_return_volume[0]);
    }

    if (take(config_dirty::kAuxReturnVolume1)) {
        state_.intermediate_mixer_volume[2] = config.aux_return_volume[1];
        MH4N_LOG(Trace, kLog, "mixers aux_return_volume[1] = {}", config.aux_return_volume[1]);
    }

    if (take(config_dirty::kOutputFormat)) {
        state_.output_format = config.output_format;
        if (config.output_format > kOutputSurround) {
            // Azahar: UNREACHABLE_MSG when it mixes (a departure, see mixers.h).
            MH4N_LOG(Error, kLog, "mixers output_format = {} is not a format, mixed as stereo",
                     config.output_format);
        }
        MH4N_LOG(Trace, kLog, "mixers output_format = {}", config.output_format);
    }

    if (take(config_dirty::kDelayEffect0)) {
        state_.delay_effect[0].ParseConfig(config.delay_effect[0], 0);
    }

    if (take(config_dirty::kDelayEffect1)) {
        state_.delay_effect[1].ParseConfig(config.delay_effect[1], 1);
    }

    if (take(config_dirty::kReverbEffect0)) {
        state_.reverb_effect[0].ParseConfig(config.reverb_effect[0], 0);
    }

    if (take(config_dirty::kReverbEffect1)) {
        state_.reverb_effect[1].ParseConfig(config.reverb_effect[1], 1);
    }

    if (take(config_dirty::kHeadphonesConnected)) {
        // Do nothing. (Note: Whether headphones are connected does affect coefficients used for
        // surround sound.)
        MH4N_LOG(Trace, kLog, "mixers headphones_connected={}", config.headphones_connected);
    }

    if (config.dirty_raw) {
        MH4N_LOG(Debug, kLog, "mixers remaining_dirty={:x}", config.dirty_raw);
    }

    config.dirty_raw = 0;
}

void Mixers::DownmixAndMixIntoCurrentFrame(float gain, const QuadFrame32& samples) {
    // TODO(merry): Limiter. (Currently we're performing final mixing assuming a disabled limiter.)

    switch (state_.output_format) {
    case kOutputMono:
        std::transform(
            current_frame_.begin(), current_frame_.end(), samples.begin(), current_frame_.begin(),
            [gain](const std::array<s16, 2>& accumulator,
                   const std::array<s32, 4>& sample) -> std::array<s16, 2> {
                // Downmix to mono
                s16 mono = ClampToS16(FloatToS32(
                    (gain * sample[0] + gain * sample[1] + gain * sample[2] + gain * sample[3]) / 2));
                // Mix into current frame
                return AddAndClampToS16(accumulator, {mono, mono});
            });
        return;

    case kOutputSurround:
        // TODO(merry): Implement surround sound.
        [[fallthrough]];

    case kOutputStereo:
    default:  // not a format: Azahar's UNREACHABLE_MSG; mixed as stereo (see mixers.h)
        std::transform(
            current_frame_.begin(), current_frame_.end(), samples.begin(), current_frame_.begin(),
            [gain](const std::array<s16, 2>& accumulator,
                   const std::array<s32, 4>& sample) -> std::array<s16, 2> {
                // Downmix to stereo
                s16 left = ClampToS16(FloatToS32(gain * sample[0] + gain * sample[2]));
                s16 right = ClampToS16(FloatToS32(gain * sample[1] + gain * sample[3]));
                // Mix into current frame
                return AddAndClampToS16(accumulator, {left, right});
            });
        return;
    }
}

void Mixers::AuxReturn(const IntermediateMixSamples& read_samples) {
    // NOTE: read_samples.mix{1,2}.pcm32 annoyingly have their dimensions in reverse order to
    // QuadFrame32.

    if (state_.aux_bus_enable[0]) {
        for (std::size_t sample = 0; sample < kSamplesPerFrame; sample++) {
            for (std::size_t channel = 0; channel < 4; channel++) {
                state_.intermediate_mix_buffer[1][sample][channel] = read_samples.mix1.pcm32[channel][sample];
            }
        }
    }

    if (state_.aux_bus_enable[1]) {
        for (std::size_t sample = 0; sample < kSamplesPerFrame; sample++) {
            for (std::size_t channel = 0; channel < 4; channel++) {
                state_.intermediate_mix_buffer[2][sample][channel] = read_samples.mix2.pcm32[channel][sample];
            }
        }
    }
}

void Mixers::AuxSend(IntermediateMixSamples& write_samples, const std::array<QuadFrame32, 3>& input) {
    // NOTE: read_samples.mix{1,2}.pcm32 annoyingly have their dimensions in reverse order to
    // QuadFrame32.

    state_.intermediate_mix_buffer[0] = input[0];

    if (state_.aux_bus_enable[0]) {
        for (std::size_t sample = 0; sample < kSamplesPerFrame; sample++) {
            for (std::size_t channel = 0; channel < 4; channel++) {
                write_samples.mix1.pcm32[channel][sample] = input[1][sample][channel];
            }
        }
    } else {
        state_.intermediate_mix_buffer[1] = input[1];
    }

    if (state_.aux_bus_enable[1]) {
        for (std::size_t sample = 0; sample < kSamplesPerFrame; sample++) {
            for (std::size_t channel = 0; channel < 4; channel++) {
                write_samples.mix2.pcm32[channel][sample] = input[2][sample][channel];
            }
        }
    } else {
        state_.intermediate_mix_buffer[2] = input[2];
    }
}

void Mixers::ApplyEffects() {
    for (u32 i = 0; i < 2; i++) {
        state_.delay_effect[i].ProcessFrame(state_.intermediate_mix_buffer[i + 1]);
        state_.reverb_effect[i].ProcessFrame(state_.intermediate_mix_buffer[i + 1]);
    }
}

void Mixers::MixCurrentFrame() {
    current_frame_.fill({});

    // TODO(SachinV): This is probably not accurate, based on symbols from FE:Fates,
    // state.intermediate_mixer_volume[0] represents the master volume
    for (std::size_t mix = 0; mix < 3; mix++) {
        DownmixAndMixIntoCurrentFrame(state_.intermediate_mixer_volume[mix], state_.intermediate_mix_buffer[mix]);
    }

    // TODO(merry): Compressor. (We currently assume a disabled compressor.)
}

}  // namespace mh4n::dsp_hle
