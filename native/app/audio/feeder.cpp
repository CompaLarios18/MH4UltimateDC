// The feeder of N3-06: SDL's get callback for the audio stream of `--audio sdl`. It runs on SDL's
// audio thread, with the stream's lock held, and only calls the stream's Pull, the drift controller
// and SDL's stream functions: no allocation, no logging, no wait.
#include "audio/app_audio.h"

#include <algorithm>
#include <chrono>

namespace mh4n::app_audio {

namespace {

double SteadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

double Feeder::RatioFor(double factor) {
    return audio_out::kDspRate.Value() / static_cast<double>(audio_out::kStreamSampleRate) * factor;
}

Feeder::Feeder(std::shared_ptr<audio_out::StreamSink> stream, Clock now)
    : stream_(std::move(stream)),
      now_(now ? std::move(now) : Clock(SteadySeconds)),
      scratch_(kChunkFrames * 2, 0),
      ratio_(RatioFor(1.0)) {
    seen_overflows_ = stream_->Stats().overflows;
}

void SDLCALL Feeder::Callback(void* userdata, SDL_AudioStream* sdl, int additional_amount, int /*total_amount*/) {
    static_cast<Feeder*>(userdata)->OnRequest(sdl, additional_amount);
}

std::size_t Feeder::Discard(std::size_t keep_frames) {
    std::size_t excess = stream_->Fill() > keep_frames ? stream_->Fill() - keep_frames : 0;
    std::size_t thrown = 0;
    while (excess > 0) {
        const std::size_t n = std::min(excess, kChunkFrames);
        const std::size_t real = stream_->Pull(scratch_.data(), n);
        if (real == 0) {
            break;
        }
        thrown += real;
        discarded_.fetch_add(real, std::memory_order_relaxed);
        excess -= std::min(excess, real);
    }
    return thrown;
}

void Feeder::OnRequest(SDL_AudioStream* sdl, int additional_amount) {
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    if (additional_amount < static_cast<int>(kBytesPerFrame)) {
        return;  // SDL has all it needs queued
    }
    const std::size_t frames = static_cast<std::size_t>(additional_amount) / kBytesPerFrame;
    const double now = now_();
    const double target = static_cast<double>(stream_->Target());
    const double block = static_cast<double>(frames);

    // 1. The ring reached its mark while nobody pulled (the device opened late, or resumed after a
    //    pause): play from the target, not from 200 ms of old audio, which the controller would need
    //    about 33 s to work off. The block itself must still find its frames.
    const u64 overflows = stream_->Stats().overflows;
    if (overflows != seen_overflows_) {
        seen_overflows_ = overflows;
        if (Discard(std::max(static_cast<std::size_t>(target + block / 2.0), frames)) > 0) {
            controller_.Reset();  // the fill is where the controller wants it: start from the neutral factor
        }
    }

    // 2. Steer: the fill in the middle of the block about to be taken (docs/native/audio-out.md,
    //    "How a consumer steers"), or the factor 1 while the stream primes.
    double factor = 1.0;
    if (stream_->Priming()) {
        controller_.Reset();
    } else {
        const double dt = have_last_ ? now - last_s_ : 0.0;
        const double steered = static_cast<double>(stream_->Fill()) - block / 2.0;
        factor = controller_.Update(steered, target, dt);
        steered_fill_.store(steered, std::memory_order_relaxed);
    }
    have_last_ = true;
    last_s_ = now;
    const double ratio = RatioFor(factor);
    SDL_SetAudioStreamFrequencyRatio(sdl, static_cast<float>(ratio));
    ratio_.store(ratio, std::memory_order_relaxed);
    factor_.store(factor, std::memory_order_relaxed);

    // 3. Pull what SDL asked for, and the margin, and give it to SDL, in chunks that one Pull can satisfy.
    std::size_t remaining = frames + kMarginFrames;
    while (remaining > 0) {
        const std::size_t n = std::min(remaining, kChunkFrames);
        stream_->Pull(scratch_.data(), n);
        SDL_PutAudioStreamData(sdl, scratch_.data(), static_cast<int>(n * kBytesPerFrame));
        remaining -= n;
    }
}

}  // namespace mh4n::app_audio
