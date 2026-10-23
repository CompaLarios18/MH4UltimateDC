// The verdict of D75 (N3-04), as pure functions over per-frame numbers.
//   SNR = 10 log10(sum ref^2 / sum (ref - ours)^2) over both channels, at least 60 dB over the judged
//   stream and in every window: tumbling windows of 205 DSP frames from the first judged frame, a last
//   window shorter than 103 frames joining the one before; a window (or stream) whose reference RMS is
//   below 1 is judged by its largest absolute difference, at most 1. (First guesses, N3.md rule 7.)
#pragma once

#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::dsp_replay {

inline constexpr double kMinSnrDb = 60.0;
inline constexpr u32 kWindowFrames = 205;
inline constexpr u32 kShortWindowFrames = 103;
inline constexpr u32 kJudgedSharePercent = 80;
inline constexpr u32 kSamplesPerFrame = 320;  // 160 stereo samples, both channels

// What one judged frame contributes.
struct FrameStat {
    u32 index = 0;        // position in the capture, from 0
    u32 counter = 0;      // the DSP's own frame counter
    u64 ref_energy = 0;   // sum of ref^2 over its 320 samples
    u64 err_energy = 0;   // sum of (ref - ours)^2
    u32 max_diff = 0;     // the largest |ref - ours|
};

struct Window {
    u32 first = 0;  // position in the judged list
    u32 count = 0;
};

// The windows of `judged` frames: floor(n / 205) of them, the remainder (when under 103 frames) joining
// the last; n < 205 is one window of n; a remainder of 103 or more is a window of its own.
std::vector<Window> MakeWindows(u32 judged);

// 10 log10(ref / err): +infinity when err is 0, -infinity when ref is 0 and err is not.
double SnrDb(u64 ref_energy, u64 err_energy);
// The text of a number of dB: "inf", "-inf", or two decimals.
std::string FormatDb(double db);

struct Span {
    u64 ref_energy = 0;
    u64 err_energy = 0;
    u32 max_diff = 0;
    u32 frames = 0;
    double db = 0;
    bool low_rms = false;  // the reference RMS is below 1
    bool pass = false;
};
// Judges `count` frames of `stats` from `first`.
Span JudgeSpan(const std::vector<FrameStat>& stats, u32 first, u32 count);

}  // namespace mh4n::dsp_replay
