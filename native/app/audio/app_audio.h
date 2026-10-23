// The SDL3 audio device of mh4u-native (N3-06, D63): `--audio sdl` plays the stream of N3-05
// (native/src/audio_out) on the player's default audio device.
//
//   - The `sdl` AudioSinkFactory makes the `stream` sink through its registered factory, finds the
//     stream with audio_out::Find and opens SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
//     {S16, 2, 32728}, ...). A device SDL cannot open logs a Warning; the sink then plays nothing
//     and the game goes on (the stream's ring stays full and counts what it discards).
//   - Feeder is SDL's get callback: it pulls from the stream what SDL asks for, hands it to the SDL
//     stream and sets the frequency ratio. It runs on SDL's audio thread: no lock, no allocation, no
//     logging, counters only.
//   - Options (runner scope): --volume N, --audio-log FILE (the stream's own are --audio-latency and
//     --audio-file).
//   - SetVolume: thread-safe, for N3-14.
//
// docs/native/audio-out.md has the details.
#pragma once

#include <SDL3/SDL_audio.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "audio_out/audio_out.h"
#include "audio_out/drift_controller.h"
#include "mh4n/types.h"

namespace mh4n {
class Runtime;
}

namespace mh4n::app_audio {

// ---- the volume ------------------------------------------------------------------------------------

inline constexpr int kDefaultVolume = 100;

// The gain for a volume of `percent` (clamped to 0 to 100): (N / 100)^3, Azahar's curve, which
// approximates a linear change in perceived loudness. 0 is silence, 100 is 1.
float VolumeGain(int percent);

// Sets the volume of the runtime's SDL audio device (`percent` is clamped to 0 to 100): the gain
// (N / 100)^3 through SDL_SetAudioStreamGain. Thread-safe (any thread, at any time). False when the
// runtime has no open SDL audio device (another sink, a device that did not open, a finished run):
// nothing is remembered then; --volume is what the device starts with.
bool SetVolume(Runtime& runtime, int percent);

// ---- the device, from outside --------------------------------------------------------------------------

struct DeviceStatus {
    int volume = kDefaultVolume;  // percent
    float gain = 1.0f;            // what SDL has on the stream
    int device_rate = 0;          // Hz: the device's format, as the SDL stream sees it
    double ratio = 1.0;           // the frequency ratio last set on the SDL stream
    double factor = 1.0;          // the drift controller's factor in it
    double steered_fill = 0.0;    // stream frames: the fill the controller last steered on (the fill in the
                                  // middle of the block taken), 0 until the first block after priming
    u64 callbacks = 0;            // requests the feeder served
    u64 discarded_frames = 0;     // stream frames thrown away after an overflow (see Feeder)
};

// The runtime's open SDL audio device, or nothing. Any thread.
std::optional<DeviceStatus> Status(Runtime& runtime);

// ---- the feeder (SDL's get callback) -------------------------------------------------------------------

class Feeder {
public:
    // Seconds on a monotonic clock; the default is std::chrono::steady_clock.
    using Clock = std::function<double()>;

    // The source rate SDL's stream is opened with: audio_out::kStreamSampleRate.
    static constexpr int kSourceRate = static_cast<int>(audio_out::kStreamSampleRate);
    // Stream frames per Pull and per SDL_PutAudioStreamData (a larger request is served in chunks of this
    // size; one Pull of more than audio_out::kOverflowFrames could never be satisfied).
    static constexpr std::size_t kChunkFrames = 4096;
    static constexpr std::size_t kBytesPerFrame = 2 * sizeof(s16);
    // Stream frames given to SDL beyond what it asked for. SDL works out its request with the frequency
    // ratio of the last block, and the callback changes the ratio before the block is taken: the block
    // can need a frame more than was asked (about one block in a hundred, at 480 frames), and SDL would
    // then give its device a block with a missing frame. What SDL does not use it keeps and counts in its
    // next request, so the margin does not pile up.
    static constexpr std::size_t kMarginFrames = 2;

    explicit Feeder(std::shared_ptr<audio_out::StreamSink> stream, Clock now = {});

    // The SDL_AudioStreamCallback: `userdata` is the Feeder.
    static void SDLCALL Callback(void* userdata, SDL_AudioStream* sdl, int additional_amount, int total_amount);

    // What the callback does, for `additional_amount` bytes of the stream's input format (S16 stereo
    // at kSourceRate) that SDL needs now:
    //   1. After an overflow of the stream (the consumer was away long enough for the ring to reach
    //      its mark), the excess over the target is thrown away, so that a device that opens late or
    //      resumes plays from the target and not from 200 ms of old audio (audio-out.md).
    //   2. The drift controller gets the fill in the middle of the block it is about to take (the
    //      fill minus half the block) and the time since the last request, or is reset while the
    //      stream is priming; the frequency ratio becomes
    //      (kDspRate / kStreamSampleRate) * factor.
    //   3. The block, and kMarginFrames more, is pulled and given to SDL. The stream makes up for what
    //      it lacks (the fade, then silence), so SDL always gets all it asked for.
    void OnRequest(SDL_AudioStream* sdl, int additional_amount);

    // ---- any thread ----
    double Ratio() const { return ratio_.load(std::memory_order_relaxed); }
    double Factor() const { return factor_.load(std::memory_order_relaxed); }
    double SteeredFill() const { return steered_fill_.load(std::memory_order_relaxed); }
    u64 Callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
    u64 DiscardedFrames() const { return discarded_.load(std::memory_order_relaxed); }
    audio_out::StreamSink& Stream() { return *stream_; }

    // The frequency ratio for a factor: SDL plays the stream at kSourceRate, and the DSP makes
    // kDspRate, a little more.
    static double RatioFor(double factor);

private:
    // Throws away stream frames until the fill is `keep_frames`; returns how many.
    std::size_t Discard(std::size_t keep_frames);

    std::shared_ptr<audio_out::StreamSink> stream_;
    Clock now_;
    audio_out::DriftController controller_;
    std::vector<s16> scratch_;  // kChunkFrames stereo frames, allocated once
    bool have_last_ = false;
    double last_s_ = 0.0;
    u64 seen_overflows_ = 0;
    std::atomic<double> ratio_;
    std::atomic<double> factor_{1.0};
    std::atomic<double> steered_fill_{0.0};
    std::atomic<u64> callbacks_{0};
    std::atomic<u64> discarded_{0};
};

static_assert(std::atomic<double>::is_always_lock_free, "the callback publishes its numbers with plain atomics");

}  // namespace mh4n::app_audio
