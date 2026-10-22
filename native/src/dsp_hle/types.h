// Derived from Azahar src/audio_core/audio_types.h and src/audio_core/hle/common.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or
// any later version"): the frame types (StereoFrame16, QuadFrame32), the samples per frame and the number
// of sources.
// Copyright 2017 Citra Emulator Project (audio_types.h)
// Copyright 2016 Citra Emulator Project (common.h)
//
// The sample types of the HLE DSP (N3-02).
#pragma once

#include <array>
#include <cstddef>

#include "audio/dsp_layout.h"
#include "mh4n/types.h"

namespace mh4n::dsp_hle {

inline constexpr std::size_t kSamplesPerFrame = audio::kSamplesPerFrame;  // 160
inline constexpr std::size_t kNumSources = audio::kNumSources;            // 24

// One stereo sample: left, right.
using StereoSample = std::array<s16, 2>;
// The output of a source for one frame, and of the final mix: stereo, signed 16 bits.
using StereoFrame16 = std::array<StereoSample, kSamplesPerFrame>;
// The DSP is quadraphonic internally: an intermediate mix has four channels of 32 bits.
using QuadFrame32 = std::array<std::array<s32, 4>, kSamplesPerFrame>;

}  // namespace mh4n::dsp_hle
