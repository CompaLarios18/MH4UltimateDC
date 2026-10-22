// Derived from Azahar src/audio_core/hle/filter.cpp and src/audio_core/hle/common.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or
// any later version"): SourceFilters, SimpleFilter, BiquadFilter and FilterFrame.
// Copyright 2016 Citra Emulator Project (filter.cpp, common.h)
//
// The per-source filters of the HLE DSP (N3-02); see filter.h.
#include "dsp_hle/filter.h"

#include <algorithm>

namespace mh4n::dsp_hle {

namespace {

// A sum of products in 32 bits that wraps where the compiled arithmetic of Azahar's does when the
// coefficients are extreme (signed overflow is undefined in C++, so it is done unsigned here).
constexpr u32 U(s32 value) {
    return static_cast<u32>(value);
}

}  // namespace

void SourceFilters::Reset() {
    Enable(false, false);
}

void SourceFilters::Enable(bool simple, bool biquad) {
    simple_filter_enabled = simple;
    biquad_filter_enabled = biquad;

    if (!simple) {
        simple_filter.Reset();
    }
    if (!biquad) {
        biquad_filter.Reset();
    }
}

void SourceFilters::ConfigureSimple(const s16 (&config)[2]) {
    simple_filter.Configure(config);
}

void SourceFilters::ConfigureBiquad(const s16 (&config)[5]) {
    biquad_filter.Configure(config);
}

void SourceFilters::ProcessFrame(StereoFrame16& frame) {
    if (!simple_filter_enabled && !biquad_filter_enabled) {
        return;
    }

    // FilterFrame: the filter's ProcessSample is called sequentially on the samples.
    if (simple_filter_enabled) {
        for (StereoSample& sample : frame) {
            sample = simple_filter.ProcessSample(sample);
        }
    }

    if (biquad_filter_enabled) {
        for (StereoSample& sample : frame) {
            sample = biquad_filter.ProcessSample(sample);
        }
    }
}

// SimpleFilter

void SourceFilters::SimpleFilter::Reset() {
    y1.fill(0);
    // Configure as passthrough.
    a1 = 0;
    b0 = 1 << 15;
}

void SourceFilters::SimpleFilter::Configure(const s16 (&config)[2]) {
    b0 = config[0];
    a1 = config[1];
}

StereoSample SourceFilters::SimpleFilter::ProcessSample(const StereoSample& x0) {
    StereoSample y0;
    for (std::size_t i = 0; i < 2; i++) {
        const s32 tmp = static_cast<s32>(U(b0) * U(x0[i]) + U(a1) * U(y1[i])) >> 15;
        y0[i] = static_cast<s16>(std::clamp(tmp, -32768, 32767));
    }

    y1 = y0;

    return y0;
}

// BiquadFilter

void SourceFilters::BiquadFilter::Reset() {
    x1.fill(0);
    x2.fill(0);
    y1.fill(0);
    y2.fill(0);
    // Configure as passthrough.
    a1 = a2 = b1 = b2 = 0;
    b0 = 1 << 14;
}

void SourceFilters::BiquadFilter::Configure(const s16 (&config)[5]) {
    a2 = config[0];
    a1 = config[1];
    b2 = config[2];
    b1 = config[3];
    b0 = config[4];
}

StereoSample SourceFilters::BiquadFilter::ProcessSample(const StereoSample& x0) {
    StereoSample y0;
    for (std::size_t i = 0; i < 2; i++) {
        const s32 tmp = static_cast<s32>(U(b0) * U(x0[i]) + U(b1) * U(x1[i]) + U(b2) * U(x2[i]) +
                                         U(a1) * U(y1[i]) + U(a2) * U(y2[i])) >>
                        14;
        y0[i] = static_cast<s16>(std::clamp(tmp, -32768, 32767));
    }

    x2 = x1;
    x1 = x0;
    y2 = y1;
    y1 = y0;

    return y0;
}

}  // namespace mh4n::dsp_hle
