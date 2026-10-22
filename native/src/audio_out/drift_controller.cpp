// Derived from Azahar src/audio_core/time_stretch.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: the file says "Licensed under GPLv2 or any later version"); src/audio_core/dsp_interface.cpp (also GPL-2.0-or-later) was read for how the FIFO is drained and how the last frame is held, and nothing of it is copied
// Copyright 2016 Citra Emulator Project (time_stretch.cpp)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (dsp_interface.cpp)
//
// The drift controller of N3-05: the formula of TimeStretcher::Process that steers the tempo (the
// tweak over 50 ms of the distance to the target, and the 0.712 s low-pass), without SoundTouch,
// with the factor bounded.
#include "audio_out/drift_controller.h"

#include <algorithm>
#include <cmath>

namespace mh4n::audio_out {

double DriftController::Update(double fill_frames, double target_frames, double dt_seconds) {
    if (!(target_frames > 0.0) || !(dt_seconds > 0.0) || !std::isfinite(fill_frames) ||
        !std::isfinite(dt_seconds)) {
        return factor_;
    }

    // Azahar: backlog_fullness = samples / max_backlog, with 0.5 as the target.
    const double backlog_fullness = fill_frames / (2.0 * target_frames);

    // We ideally want the backlog to be about 50% full. We tweak the ratio (1 here, Azahar's
    // in/out) to encourage this.
    const double tweak_correction = (backlog_fullness - 0.5) * (dt_seconds / kTweakTimeScale);
    const double base = 1.0 + 2.0 * tweak_correction;
    // Azahar raises the base to 3 for a fill below the target; a time step so long that the base
    // is not positive is as far below the target as it gets.
    double current_ratio = base <= 0.0 ? kMinFactor : std::pow(base, tweak_correction < 0 ? 3.0 : 1.0);
    current_ratio = std::clamp(current_ratio, kMinFactor, kMaxFactor);

    // This low-pass filter smoothes out variance in the calculated ratio.
    const double lpf_gain = 1.0 - std::exp(-dt_seconds / kLowPassTimeScale);
    factor_ += lpf_gain * (current_ratio - factor_);
    factor_ = std::clamp(factor_, kMinFactor, kMaxFactor);
    return factor_;
}

}  // namespace mh4n::audio_out
