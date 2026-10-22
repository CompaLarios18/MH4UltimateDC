// Derived from Azahar src/audio_core/interpolate.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): StepOverSamples,
// AudioInterp::None and AudioInterp::Linear.
// Copyright 2016 Citra Emulator Project (interpolate.cpp)
//
// The interpolation of the HLE DSP (N3-02); see interpolate.h.
#include "dsp_hle/interpolate.h"

#include <algorithm>
#include <limits>

#include "mh4n/log.h"

namespace mh4n::dsp_hle::interp {

namespace {

// Calculations are done in fixed point with 24 fractional bits.
// (This is not verified. This was chosen for minimal error: Azahar's comment.)
constexpr u64 kScaleFactor = 1 << 24;
constexpr u64 kScaleMask = kScaleFactor - 1;

// Here we step over the input in steps of rate, until we consume all of the input. Three adjacent
// samples are passed to fn each step. Azahar puts the two historical samples in front of the input,
// steps over that and erases what the steps passed; here the two are read from the state where the
// index says so and the real input loses `inputi` samples: the same thing without the copy.
template <typename Function>
void StepOverSamples(State& state, StereoBuffer& input, float rate, StereoFrame16& output,
                     std::size_t& outputi, Function fn) {
    if (!(rate > 0)) {  // Azahar: ASSERT(rate > 0)
        MH4N_LOG(Error, "Audio.DSP.HLE", "Was given an invalid rate multiplier {}; taken as 1.0", rate);
        rate = 1.0f;
    }

    if (input.empty()) {
        return;
    }

    const std::size_t extended_size = input.size() + 2;
    const auto at = [&](std::size_t i) -> const StereoSample& {
        return i == 0 ? state.xn2 : i == 1 ? state.xn1 : input[i - 2];
    };

    const u64 step_size = StepSize(rate);
    u64 fposition = state.fposition;
    std::size_t inputi = 0;

    while (outputi < output.size()) {
        inputi = static_cast<std::size_t>(fposition / kScaleFactor);

        if (inputi + 2 >= extended_size) {
            inputi = extended_size - 2;
            break;
        }

        const u64 fraction = fposition & kScaleMask;
        output[outputi++] = fn(fraction, at(inputi), at(inputi + 1), at(inputi + 2));

        fposition += step_size;
    }

    const StereoSample xn2 = at(inputi);
    const StereoSample xn1 = at(inputi + 1);
    state.xn2 = xn2;
    state.xn1 = xn1;
    state.fposition = fposition - inputi * kScaleFactor;

    input.EraseFront(inputi);  // Azahar erases inputi + 2 of the input with the two samples in front
}

}  // namespace

u64 StepSize(float rate) {
    // static_cast<u64>(rate * scale_factor): the u64 factor becomes a float, the product is a float.
    const float scaled = rate * static_cast<float>(kScaleFactor);
    if (!(scaled < 18446744073709551616.0f)) {
        return std::numeric_limits<u64>::max();
    }
    return static_cast<u64>(scaled);
}

void None(State& state, StereoBuffer& input, float rate, StereoFrame16& output, std::size_t& outputi) {
    StepOverSamples(state, input, rate, output, outputi,
                    [](u64, const StereoSample& x0, const StereoSample&, const StereoSample&) { return x0; });
}

void Linear(State& state, StereoBuffer& input, float rate, StereoFrame16& output, std::size_t& outputi) {
    // Note on accuracy: "Some values that this produces are +/- 1 from the actual firmware" (Azahar).
    StepOverSamples(
        state, input, rate, output, outputi,
        [](u64 fraction, const StereoSample& x0, const StereoSample& x1, const StereoSample&) {
            // This is a saturated subtraction. (Verified by black-box fuzzing: Azahar.)
            const s64 delta0 = std::clamp<s64>(x1[0] - x0[0], -32768, 32767);
            const s64 delta1 = std::clamp<s64>(x1[1] - x0[1], -32768, 32767);

            // The arithmetic is Azahar's, type for type: `fraction` is unsigned, so a negative delta
            // makes the product, the division and the sum unsigned too, and the cast to s16 keeps the low
            // 16 bits. (That rounds a negative step towards the previous sample; it is what the oracle
            // table has.)
            return StereoSample{
                static_cast<s16>(x0[0] + fraction * delta0 / kScaleFactor),
                static_cast<s16>(x0[1] + fraction * delta1 / kScaleFactor),
            };
        });
}

}  // namespace mh4n::dsp_hle::interp
