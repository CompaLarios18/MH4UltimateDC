#include "dsp_replay/judge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mh4n::dsp_replay {

std::vector<Window> MakeWindows(u32 judged) {
    std::vector<Window> windows;
    if (judged == 0) {
        return windows;
    }
    u32 full = judged / kWindowFrames;
    const u32 rest = judged % kWindowFrames;
    if (full == 0) {
        windows.push_back({0, judged});
        return windows;
    }
    for (u32 i = 0; i < full; ++i) {
        windows.push_back({i * kWindowFrames, kWindowFrames});
    }
    if (rest >= kShortWindowFrames) {
        windows.push_back({full * kWindowFrames, rest});
    } else {
        windows.back().count += rest;
    }
    return windows;
}

double SnrDb(u64 ref_energy, u64 err_energy) {
    if (err_energy == 0) {
        return std::numeric_limits<double>::infinity();
    }
    if (ref_energy == 0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10(static_cast<double>(ref_energy) / static_cast<double>(err_energy));
}

std::string FormatDb(double db) {
    if (std::isinf(db)) {
        return db > 0 ? "inf" : "-inf";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", db);
    return text;
}

Span JudgeSpan(const std::vector<FrameStat>& stats, u32 first, u32 count) {
    Span span;
    span.frames = count;
    for (u32 i = first; i < first + count; ++i) {
        span.ref_energy += stats[i].ref_energy;
        span.err_energy += stats[i].err_energy;
        span.max_diff = std::max(span.max_diff, stats[i].max_diff);
    }
    span.db = SnrDb(span.ref_energy, span.err_energy);
    // RMS < 1  <=>  sum ref^2 < number of samples
    span.low_rms = span.ref_energy < static_cast<u64>(count) * kSamplesPerFrame;
    span.pass = span.low_rms ? span.max_diff <= 1 : span.db >= kMinSnrDb;
    return span;
}

}  // namespace mh4n::dsp_replay
