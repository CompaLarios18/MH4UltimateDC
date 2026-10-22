// The drift controller (N3-05, D63): keeps the audio device's clock and the game's apart without
// ever pacing the game. Pure: it holds one number and takes the fill, the target and the time
// since the last call. The consumer (the device's thread, N3-06) calls Update before each block
// and multiplies its resampling ratio (frames of the stream used per frame played) by the factor:
// above 1 it uses the stream faster, so the fill goes down.
//
// The formula is that of Azahar's time stretcher (src/audio_core/time_stretch.cpp, Process):
//   - the tweak: the distance of the fill from the target, over 50 ms of the call's time step
//     (kTweakTimeScale); a fill below the target is corrected harder (the cube);
//   - the 0.712 s low-pass (kLowPassTimeScale) over the result;
// bounded to [kMinFactor, kMaxFactor] (+-0.5 %, first guess).
// Azahar's backlog_fullness is the fill over a maximum backlog that is twice its target
// (0.5 is the target), and so it is here: fullness = fill / (2 * target).
#pragma once

namespace mh4n::audio_out {

class DriftController {
public:
    static constexpr double kMinFactor = 0.995;         // first guess
    static constexpr double kMaxFactor = 1.005;         // first guess
    static constexpr double kTweakTimeScale = 0.050;    // seconds (Azahar's tweak_time_scale)
    static constexpr double kLowPassTimeScale = 0.712;  // seconds (Azahar's lpf_time_scale)

    // The factor for the next block. `fill_frames` is what is waiting in the stream, `target_frames`
    // the fill to hold, `dt_seconds` the time since the previous call (a call with a time step or a
    // target that is not positive, or a fill that is not a number, changes nothing).
    double Update(double fill_frames, double target_frames, double dt_seconds);

    double Factor() const { return factor_; }
    // Back to 1: after an underrun, once the stream has refilled to its target.
    void Reset() { factor_ = 1.0; }

private:
    double factor_ = 1.0;
};

}  // namespace mh4n::audio_out
