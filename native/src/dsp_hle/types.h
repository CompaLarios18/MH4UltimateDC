// Derived from Azahar src/audio_core/audio_types.h and src/audio_core/hle/common.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or
// any later version"): the frame types (StereoFrame16, QuadFrame32), the samples per frame and the number
// of sources, and the sample rate (native_sample_rate).
// Copyright 2017 Citra Emulator Project (audio_types.h)
// Copyright 2016 Citra Emulator Project (common.h)
//
// The sample types of the HLE DSP (N3-02).
#pragma once

#include <array>
#include <cstddef>
#include <limits>

#include "audio/dsp_layout.h"
#include "mh4n/types.h"

namespace mh4n::dsp_hle {

inline constexpr std::size_t kSamplesPerFrame = audio::kSamplesPerFrame;  // 160
// Samples per second which the 3DS's audio hardware natively outputs at (native_sample_rate).
inline constexpr u32 kNativeSampleRate = 32728;
inline constexpr std::size_t kNumSources = audio::kNumSources;            // 24

// One stereo sample: left, right.
using StereoSample = std::array<s16, 2>;
// The output of a source for one frame, and of the final mix: stereo, signed 16 bits.
using StereoFrame16 = std::array<StereoSample, kSamplesPerFrame>;
// The DSP is quadraphonic internally: an intermediate mix has four channels of 32 bits.
using QuadFrame32 = std::array<std::array<s32, 4>, kSamplesPerFrame>;

// static_cast<s32>(float), defined where the C++ conversion is not (N3-03): a value outside the s32
// range, or a NaN, gives INT32_MIN, which is what the x86 conversion Azahar's reference platform
// compiles it to gives (cvttss2si). The conversion is undefined in Azahar's C++ and an ARM host's
// differs (it saturates); the mixers and the effects use this one so that the result does not depend
// on the host. In range it is the plain truncation.
inline s32 FloatToS32(float value) {
    if (!(value >= -2147483648.0f && value < 2147483648.0f)) {
        return std::numeric_limits<s32>::min();
    }
    return static_cast<s32>(value);
}

}  // namespace mh4n::dsp_hle
