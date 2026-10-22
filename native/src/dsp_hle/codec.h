// Derived from Azahar src/audio_core/codec.h and src/audio_core/codec.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "Licensed under GPLv2 or any later
// version"): DecodeADPCM, DecodePCM8 and DecodePCM16 and the ADPCM state.
// Copyright 2016 Citra Emulator Project (codec.h, codec.cpp)
//
// The sample decoders of the HLE DSP (N3-02). Each takes the guest bytes of a buffer and returns its
// samples as stereo PCM16 (a mono buffer is copied into both channels). A span shorter than the buffer
// needs reads as zeros after its end (the memory behind a buffer that the caller cannot supply).
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "dsp_hle/types.h"

namespace mh4n::dsp_hle::codec {

// y[n-1] and y[n-2] of the previous buffer, which ADPCM decoding needs.
struct AdpcmState {
    s16 yn1 = 0;
    s16 yn2 = 0;
};

// The bytes a buffer of `sample_count` samples occupies.
std::size_t AdpcmBytes(std::size_t sample_count);  // 8-byte frames of 14 samples, the last one whole
std::size_t Pcm8Bytes(unsigned num_channels, std::size_t sample_count);
std::size_t Pcm16Bytes(unsigned num_channels, std::size_t sample_count);

// GC-ADPCM with a scale factor and 8 pairs of coefficients (11 fractional bits). The result has
// `sample_count` rounded up to an even number of samples, as Azahar's. `state` is updated.
std::vector<StereoSample> DecodeAdpcm(std::span<const u8> data, std::size_t sample_count,
                                      const std::array<s16, 16>& coefficients, AdpcmState& state);

// `num_channels` is 1 or 2 (anything else is treated as 1; Azahar asserts).
std::vector<StereoSample> DecodePcm8(unsigned num_channels, std::span<const u8> data,
                                     std::size_t sample_count);
std::vector<StereoSample> DecodePcm16(unsigned num_channels, std::span<const u8> data,
                                      std::size_t sample_count);

}  // namespace mh4n::dsp_hle::codec
