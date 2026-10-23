// Derived from Azahar src/audio_core/hle/mixers.h, src/audio_core/hle/mixers.cpp and
// src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each
// of them says "Licensed under GPLv2 or any later version"): Mixers (its state, Tick and the steps of a
// tick) and the layouts of DspConfiguration, DspStatus and IntermediateMixSamples.
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (mixers.h, mixers.cpp)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
//
// The three intermediate mixers of the HLE DSP and the final mix (N3-03): reads the DSP configuration
// the application wrote (clearing its dirty bits), exchanges the two auxiliary buses with the shared
// memory (the samples the application processes between frames), applies the effects and mixes the
// three intermediate mixes, with the master volume and the aux return volumes, down to 160 stereo
// samples of 16 bits, by integer arithmetic on s32 with saturation where Azahar saturates.
//
// Departures from Azahar, all deliberate (D78; the DSP memory belongs to a game that must keep running):
//  - An output format above 2 (Azahar's UNREACHABLE_MSG ends the process) is logged and mixed as stereo,
//    the format Surround also falls back to.
//  - The float-to-s32 conversions of the downmix are FloatToS32 (types.h): where Azahar's is undefined
//    (a value of 2^31 or more in magnitude, or a NaN, from a huge gain or aux samples) the result is
//    INT32_MIN as on x86, whatever the host. In range they are the plain truncation.
//  - The 28 padding bytes of DspStatus are indeterminate in Azahar (an uninitialised local that is
//    copied to the shared memory); here DspStatus has the two words it has and Tick leaves the padding
//    in the memory as it is.
#pragma once

#include <array>
#include <cstddef>

#include "dsp_hle/effects.h"
#include "dsp_hle/types.h"

namespace mh4n::dsp_hle {

// DspConfiguration (196 bytes, at audio::shm::kDspConfiguration): the dirty bits and what the mixers
// read of it.
struct DspConfiguration {
    u32 dirty_raw;
    float master_volume;           // the DSP has three intermediate mixers: this is the volume of the first
    float aux_return_volume[2];    // and of the two auxiliary ones, at the final mixer
    u16 output_buffer_count;
    u16 padding0[2];
    u16 output_format;             // OutputFormat
    u16 clipping_mode;
    u16 headphones_connected;
    u16 surround_depth;
    u16 surround_speaker_position;
    u16 padding1;
    u16 rear_ratio;
    u16 aux_front_bypass[2];
    u16 aux_bus_enable[2];
    DelayEffectConfig delay_effect[2];
    ReverbEffectConfig reverb_effect[2];
    u16 sync_mode;
    u16 padding2;
    u32 dirty2_raw;
};
static_assert(sizeof(DspConfiguration) == 196);
static_assert(offsetof(DspConfiguration, output_format) == 22);
static_assert(offsetof(DspConfiguration, aux_bus_enable) == 40);
static_assert(offsetof(DspConfiguration, delay_effect) == 44);
static_assert(offsetof(DspConfiguration, reverb_effect) == 84);
static_assert(offsetof(DspConfiguration, sync_mode) == 0xBC);
static_assert(offsetof(DspConfiguration, dirty2_raw) == 0xC0);

// The bits of DspConfiguration::dirty_raw the mixers read.
namespace config_dirty {
inline constexpr u32 kAuxBusEnable0 = 1u << 8;
inline constexpr u32 kAuxBusEnable1 = 1u << 9;
inline constexpr u32 kDelayEffect0 = 1u << 10;
inline constexpr u32 kDelayEffect1 = 1u << 11;
inline constexpr u32 kReverbEffect0 = 1u << 12;
inline constexpr u32 kReverbEffect1 = 1u << 13;
inline constexpr u32 kMasterVolume = 1u << 16;
inline constexpr u32 kAuxReturnVolume0 = 1u << 24;
inline constexpr u32 kAuxReturnVolume1 = 1u << 25;
inline constexpr u32 kOutputFormat = 1u << 26;
inline constexpr u32 kHeadphonesConnected = 1u << 28;
}  // namespace config_dirty

// DspConfiguration::OutputFormat
inline constexpr u16 kOutputMono = 0;
inline constexpr u16 kOutputStereo = 1;
inline constexpr u16 kOutputSurround = 2;

// What the DSP reports about itself (at audio::shm::kDspStatus, 32 bytes of which two words mean
// anything).
struct DspStatus {
    u16 unknown;
    u16 dropped_frames;
};

// The samples of the two auxiliary buses in the shared memory (5120 bytes): little-endian, four channels
// of 160 samples each (the dimensions are in the reverse order to QuadFrame32).
struct IntermediateMixSamples {
    struct Samples {
        s32 pcm32[4][kSamplesPerFrame];
    };
    Samples mix1;
    Samples mix2;
};
static_assert(sizeof(IntermediateMixSamples) == 5120);

class Mixers final {
public:
    Mixers() { Reset(); }

    void Reset();

    void Sleep();
    void Wakeup();

    // One frame: `config` is the application's configuration (its dirty bits are cleared), `read_samples`
    // the aux buses in the region the DSP reads, `write_samples` those in the region it writes (a bus
    // that is enabled is overwritten with `input`'s), `input` the three intermediate mixes of the
    // sources.
    DspStatus Tick(DspConfiguration& config, const IntermediateMixSamples& read_samples,
                   IntermediateMixSamples& write_samples, const std::array<QuadFrame32, 3>& input);

    StereoFrame16 GetOutput() const { return current_frame_; }

private:
    struct MixerState {
        std::array<float, 3> intermediate_mixer_volume = {};

        std::array<bool, 2> aux_bus_enable = {};
        std::array<QuadFrame32, 3> intermediate_mix_buffer = {};

        // Effect index i applies to intermediate_mix_buffer[i + 1].
        std::array<DelayEffect, 2> delay_effect = {};
        std::array<ReverbEffect, 2> reverb_effect = {};

        u16 output_format = kOutputStereo;
    };

    // INTERNAL: Update our internal state based on the current config.
    void ParseConfig(DspConfiguration& config);
    // INTERNAL: Read samples from shared memory that have been modified by the ARM11.
    void AuxReturn(const IntermediateMixSamples& read_samples);
    // INTERNAL: Write samples to shared memory for the ARM11 to modify.
    void AuxSend(IntermediateMixSamples& write_samples, const std::array<QuadFrame32, 3>& input);
    // INTERNAL: Apply the delay and reverb effects.
    void ApplyEffects();
    // INTERNAL: Mix current_frame.
    void MixCurrentFrame();
    // INTERNAL: Downmix from quadraphonic to stereo based on the output format and accumulate into
    // current_frame.
    void DownmixAndMixIntoCurrentFrame(float gain, const QuadFrame32& samples);

    StereoFrame16 current_frame_ = {};
    StereoFrame16 backup_frame_ = {};  // Azahar: "TODO(PabloMK7): Check if we actually need this"

    MixerState state_;
    MixerState backup_state_;
};

}  // namespace mh4n::dsp_hle
