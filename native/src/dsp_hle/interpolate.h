// Derived from Azahar src/audio_core/interpolate.h and src/audio_core/interpolate.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "Licensed under GPLv2 or any later
// version"): the interpolation state, AudioInterp::None, AudioInterp::Linear and the fixed-point
// stepping over the input (StepOverSamples).
// Copyright 2016 Citra Emulator Project (interpolate.h, interpolate.cpp)
//
// The interpolation of the HLE DSP (N3-02). Positions are in fixed point with 24 fractional bits and the
// step is `static_cast<u64>(rate * (1 << 24))` computed from the `float` rate as Azahar computes it (a
// float multiplication, then a truncation), so no sample drifts from Azahar's after any number of frames.
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "dsp_hle/types.h"

namespace mh4n::dsp_hle::interp {

// A variable-length buffer of stereo samples that is consumed from the front (Azahar's std::deque).
class StereoBuffer {
public:
    bool empty() const { return head_ == data_.size(); }
    std::size_t size() const { return data_.size() - head_; }
    const StereoSample& operator[](std::size_t i) const { return data_[head_ + i]; }
    void clear() {
        data_.clear();
        head_ = 0;
    }
    void assign(std::vector<StereoSample> samples) {
        data_ = std::move(samples);
        head_ = 0;
    }
    // Drops the first `count` samples (at most all of them).
    void EraseFront(std::size_t count) { head_ += count < size() ? count : size(); }

private:
    std::vector<StereoSample> data_;
    std::size_t head_ = 0;
};

struct State {
    // Two historical samples.
    StereoSample xn1{};  // x[n-1]
    StereoSample xn2{};  // x[n-2]
    // Current fractional position.
    u64 fposition = 0;
};

// The step in fixed point for a rate (0 < rate), saturating where the C++ conversion is undefined.
u64 StepSize(float rate);

// No interpolation: a zero-order hold with a two-sample predelay. `rate` > 1.0 decimates and < 1.0
// upsamples; one that is not positive (Azahar asserts) is taken as 1.0. Fills `output` from `outputi`
// and consumes what the steps passed from `input`.
void None(State& state, StereoBuffer& input, float rate, StereoFrame16& output, std::size_t& outputi);

// Linear interpolation: a first-order hold with a two-sample predelay. Polyphase is this too (Azahar's
// "TODO(merry): Implement polyphase interpolation").
void Linear(State& state, StereoBuffer& input, float rate, StereoFrame16& output, std::size_t& outputi);

}  // namespace mh4n::dsp_hle::interp
