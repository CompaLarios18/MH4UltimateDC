// Derived from Azahar src/audio_core/hle/filter.h, src/audio_core/hle/filter.cpp and
// src/audio_core/hle/common.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them
// says "Licensed under GPLv2 or any later version"): SourceFilters (the simple and the biquad filter,
// Enable, Configure, ProcessFrame) and FilterFrame.
// Copyright 2016 Citra Emulator Project (filter.h, filter.cpp, common.h)
//
// The per-source filters of the HLE DSP (N3-02). There is an independent set for each source.
#pragma once

#include <array>

#include "dsp_hle/types.h"

namespace mh4n::dsp_hle {

class SourceFilters final {
public:
    SourceFilters() { Reset(); }

    // Reset internal state (both filters off and configured as passthrough).
    void Reset();

    // Enable or disable the filters. Disabling one resets its coefficients and history (Azahar).
    void Enable(bool simple, bool biquad);

    // The simple filter: H(z) = b0 / (1 - a1 z^-1), 15 fractional bits. The array is the
    // configuration's {b0, a1}.
    void ConfigureSimple(const s16 (&config)[2]);
    // The biquad filter: H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 - a1 z^-1 - a2 z^-2), 14 fractional
    // bits. The array is the configuration's {a2, a1, b2, b1, b0}.
    void ConfigureBiquad(const s16 (&config)[5]);

    // Processes a frame in place.
    void ProcessFrame(StereoFrame16& frame);

private:
    bool simple_filter_enabled = false;
    bool biquad_filter_enabled = false;

    struct SimpleFilter {
        SimpleFilter() { Reset(); }
        void Reset();
        void Configure(const s16 (&config)[2]);
        StereoSample ProcessSample(const StereoSample& x0);

    private:
        // Configuration
        s32 a1, b0;
        // Internal state
        StereoSample y1;
    } simple_filter;

    struct BiquadFilter {
        BiquadFilter() { Reset(); }
        void Reset();
        void Configure(const s16 (&config)[5]);
        StereoSample ProcessSample(const StereoSample& x0);

    private:
        // Configuration
        s32 a1, a2, b0, b1, b2;
        // Internal state
        StereoSample x1;
        StereoSample x2;
        StereoSample y1;
        StereoSample y2;
    } biquad_filter;
};

}  // namespace mh4n::dsp_hle
