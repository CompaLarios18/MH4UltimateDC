// Derived from Azahar src/audio_core/codec.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): DecodeADPCM, DecodePCM8 and
// DecodePCM16.
// Copyright 2016 Citra Emulator Project (codec.cpp)
//
// The sample decoders of the HLE DSP (N3-02); see codec.h.
#include "dsp_hle/codec.h"

#include <algorithm>
#include <cstring>

namespace mh4n::dsp_hle::codec {

namespace {

constexpr std::size_t kAdpcmFrameLen = 8;
constexpr std::size_t kAdpcmSamplesPerFrame = 14;

// `data` itself, or a copy of it with zeros after its end when it is shorter than `needed`.
std::span<const u8> Padded(std::span<const u8> data, std::size_t needed, std::vector<u8>& storage) {
    if (data.size() >= needed) {
        return data;
    }
    storage.assign(needed, 0);
    std::copy(data.begin(), data.end(), storage.begin());
    return storage;
}

}  // namespace

std::size_t AdpcmBytes(std::size_t sample_count) {
    return (sample_count + (kAdpcmSamplesPerFrame - 1)) / kAdpcmSamplesPerFrame * kAdpcmFrameLen;
}

std::size_t Pcm8Bytes(unsigned num_channels, std::size_t sample_count) {
    return sample_count * (num_channels == 2 ? 2 : 1);
}

std::size_t Pcm16Bytes(unsigned num_channels, std::size_t sample_count) {
    return sample_count * (num_channels == 2 ? 2 : 1) * sizeof(s16);
}

std::vector<StereoSample> DecodeAdpcm(std::span<const u8> input, std::size_t sample_count,
                                      const std::array<s16, 16>& coefficients, AdpcmState& state) {
    // GC-ADPCM with scale factor and variable coefficients. Frames are 8 bytes long containing 14
    // samples each. Samples are 4 bits (one nibble) long.
    static constexpr std::array<int, 16> kSignedNibbles{
        0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1,
    };

    std::vector<u8> storage;
    const std::span<const u8> data = Padded(input, AdpcmBytes(sample_count), storage);

    const std::size_t ret_size = sample_count % 2 == 0 ? sample_count : sample_count + 1;  // even
    std::vector<StereoSample> ret(ret_size);

    int yn1 = state.yn1;
    int yn2 = state.yn2;

    const std::size_t num_frames = (sample_count + (kAdpcmSamplesPerFrame - 1)) / kAdpcmSamplesPerFrame;
    for (std::size_t framei = 0; framei < num_frames; framei++) {
        const int frame_header = data[framei * kAdpcmFrameLen];
        const int scale = 1 << (frame_header & 0xF);
        const int idx = (frame_header >> 4) & 0x7;

        // Coefficients are fixed point with 11 bits fractional part.
        const int coef1 = coefficients[idx * 2 + 0];
        const int coef2 = coefficients[idx * 2 + 1];

        // Decodes an audio sample. One nibble produces one sample. The sum wraps as the 32-bit
        // arithmetic of Azahar's compiled code does when the coefficients are extreme (signed overflow
        // is undefined in C++, so it is done unsigned here).
        const auto decode_sample = [&](const int nibble) -> s16 {
            const int xn = nibble * scale;
            // We first transform everything into 11 bit fixed point, perform the second order
            // digital filter, then transform back. 0x400 == 0.5 in 11 bit fixed point.
            // Filter: y[n] = x[n] + 0.5 + c1 * y[n-1] + c2 * y[n-2]
            const u32 sum = (static_cast<u32>(xn) << 11) + 0x400u + static_cast<u32>(coef1) * static_cast<u32>(yn1) +
                            static_cast<u32>(coef2) * static_cast<u32>(yn2);
            int val = static_cast<s32>(sum) >> 11;
            // Clamp to output range.
            val = std::clamp(val, -32768, 32767);
            // Advance output feedback.
            yn2 = yn1;
            yn1 = val;
            return static_cast<s16>(val);
        };

        std::size_t outputi = framei * kAdpcmSamplesPerFrame;
        std::size_t datai = framei * kAdpcmFrameLen + 1;
        for (std::size_t i = 0; i < kAdpcmSamplesPerFrame && outputi < sample_count; i += 2) {
            const s16 sample1 = decode_sample(kSignedNibbles[data[datai] >> 4]);
            ret[outputi].fill(sample1);
            outputi++;

            const s16 sample2 = decode_sample(kSignedNibbles[data[datai] & 0xF]);
            ret[outputi].fill(sample2);
            outputi++;

            datai++;
        }
    }

    state.yn1 = static_cast<s16>(yn1);
    state.yn2 = static_cast<s16>(yn2);

    return ret;
}

std::vector<StereoSample> DecodePcm8(unsigned num_channels, std::span<const u8> input,
                                     std::size_t sample_count) {
    std::vector<u8> storage;
    const std::span<const u8> data = Padded(input, Pcm8Bytes(num_channels, sample_count), storage);

    const auto decode_sample = [](u8 sample) {
        return static_cast<s16>(static_cast<u16>(static_cast<u16>(sample) << 8));
    };

    std::vector<StereoSample> ret(sample_count);

    if (num_channels != 2) {
        for (std::size_t i = 0; i < sample_count; i++) {
            ret[i].fill(decode_sample(data[i]));
        }
    } else {
        for (std::size_t i = 0; i < sample_count; i++) {
            ret[i][0] = decode_sample(data[i * 2 + 0]);
            ret[i][1] = decode_sample(data[i * 2 + 1]);
        }
    }

    return ret;
}

std::vector<StereoSample> DecodePcm16(unsigned num_channels, std::span<const u8> input,
                                      std::size_t sample_count) {
    std::vector<u8> storage;
    const std::span<const u8> data = Padded(input, Pcm16Bytes(num_channels, sample_count), storage);

    std::vector<StereoSample> ret(sample_count);

    // The guest is little-endian, and so is the host (ipc.h asserts it).
    if (num_channels != 2) {
        for (std::size_t i = 0; i < sample_count; i++) {
            s16 sample;
            std::memcpy(&sample, data.data() + i * sizeof(s16), sizeof(s16));
            ret[i].fill(sample);
        }
    } else {
        for (std::size_t i = 0; i < sample_count; ++i) {
            std::memcpy(&ret[i], data.data() + i * sizeof(s16) * 2, 2 * sizeof(s16));
        }
    }

    return ret;
}

}  // namespace mh4n::dsp_hle::codec
