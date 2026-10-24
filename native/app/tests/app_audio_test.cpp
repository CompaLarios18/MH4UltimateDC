// The SDL3 audio device of mh4u-native (N3-06): `--audio sdl`, the feeder SDL calls for data, the
// volume and the audio log. No game data: the audio is a synthetic tone.
//
// Three kinds of test:
//   - the feeder against an SDL audio stream that no device owns, on a manual clock: SDL's own
//     converter and resampler, the callback and the drift controller, 10 simulated minutes in a
//     moment (the way audio_out_test drives the stream);
//   - the whole sink on SDL's `disk` driver (the device's output goes to a file, in the device's own
//     format) and its `dummy` driver (a device that consumes in real time and plays nowhere), with a
//     producer that queues in real time as the DSP does;
//   - devices that cannot open, the options and the volume.
// Every device here is opened with the driver forced by an override hint, which wins over a
// SDL_AUDIO_DRIVER in the environment.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio/app_audio.h"
#include "audio_out/audio_out.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

using namespace mh4n;
using namespace mh4n::app_audio;
using audio_out::kDspRate;
using audio_out::kStreamSampleRate;
using audio_out::StreamSink;

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- helpers ------------------------------------------------------------------------------------------------

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

struct LogCapture {
    std::vector<std::pair<LogLevel, std::string>> lines;
    LogCapture() {
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { lines.emplace_back(r.level, std::string(r.message)); });
    }
    ~LogCapture() { SetLogSink({}); }
    bool Has(LogLevel level, const std::string& needle) const {
        return std::any_of(lines.begin(), lines.end(), [&](const auto& l) {
            return l.first == level && l.second.find(needle) != std::string::npos;
        });
    }
    bool HasAtLeast(LogLevel level) const {
        return std::any_of(lines.begin(), lines.end(), [&](const auto& l) { return l.first >= level; });
    }
};

std::filesystem::path TempFile(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("mh4n_app_audio_" + name);
}

// The SDL audio driver of a test, forced by override hints for as long as this lives, with the
// device's block size (frames; 0 leaves SDL's) and the disk driver's output file.
struct SdlDriver {
    explicit SdlDriver(const char* driver, int block_frames = 0, const std::string& disk_file = {}) {
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, driver, SDL_HINT_OVERRIDE);
        if (block_frames > 0) {
            SDL_SetHintWithPriority(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(block_frames).c_str(),
                                    SDL_HINT_OVERRIDE);
        }
        if (!disk_file.empty()) {
            SDL_SetHintWithPriority(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, disk_file.c_str(), SDL_HINT_OVERRIDE);
        }
    }
    ~SdlDriver() {
        SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);
    }
};

// A 1 kHz tone in the DSP's own time (kDspRate samples per second), `amplitude` of full scale.
constexpr double kToneHz = 1000.0;
constexpr double kToneAmplitude = 0.5;

void ToneFrames(u64 first, std::size_t frames, std::vector<s16>& out) {
    out.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * kPi * kToneHz * static_cast<double>(first + i) / kDspRate.Value();
        const s16 v = static_cast<s16>(std::lround(kToneAmplitude * 32768.0 * std::sin(phase)));
        out[i * 2] = v;
        out[i * 2 + 1] = v;
    }
}
constexpr double kToneRms = kToneAmplitude / 1.41421356237309504880;  // of full scale

// The DSP as a real-time producer on the calling thread (the "scheduler thread"): for `seconds` of
// the steady clock it queues 160-frame blocks paced at kDspRate, catching up when it wakes late, and
// calls `tick(elapsed_seconds)` about every millisecond.
template <class Tick>
void RunProducer(AudioSink& sink, double seconds, Tick&& tick) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    std::vector<s16> block;
    u64 produced = 0;
    for (;;) {
        const double elapsed = std::chrono::duration<double>(clock::now() - start).count();
        if (elapsed >= seconds) {
            break;
        }
        const u64 due = static_cast<u64>(elapsed * kDspRate.Value());
        while (produced + audio_out::kDspFrameSamples <= due) {
            ToneFrames(produced, audio_out::kDspFrameSamples, block);
            sink.Queue(block.data(), audio_out::kDspFrameSamples);
            produced += audio_out::kDspFrameSamples;
        }
        tick(elapsed);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// The DSP paced by the device's own clock instead of the host's: the device's time is the blocks it
// has asked for (the feeder's callbacks, each one block of `block_s` seconds of the device's time,
// interpolated with the host clock between two callbacks), and the DSP makes audio at kDspRate
// times (1 + offset) per second of it. This is the relation the drift controller has to hold, a
// device whose crystal is `offset` slow against the DSP's clock, without the dummy driver's own
// sleep error (a whole-millisecond sleep per block, plus the host's wake-up latency: 0.1 to 0.3 %
// of a 40 ms block on this machine, and more on some hosts) moving the drift out of the controller's
// +-0.5 % on another machine. Returns the seconds of the host clock it took.
template <class Tick>
double RunDeviceClockProducer(Runtime& rt, AudioSink& sink, double seconds, double block_s, double offset, Tick&& tick) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto last_change = start;
    u64 last_callbacks = 0;
    std::vector<s16> block;
    u64 produced = 0;
    for (;;) {
        const auto now = clock::now();
        const auto status = Status(rt);
        if (status && status->callbacks != last_callbacks) {
            last_callbacks = status->callbacks;
            last_change = now;
        }
        const double since = std::min(std::chrono::duration<double>(now - last_change).count(), block_s);
        const double device_time = static_cast<double>(last_callbacks) * block_s + since;
        if (device_time >= seconds) {
            return std::chrono::duration<double>(now - start).count();
        }
        if (now - start > std::chrono::duration<double>(seconds * 2.0 + 5.0)) {
            FAIL_CHECK("the device stopped asking for audio: " << last_callbacks << " blocks in "
                                                                << std::chrono::duration<double>(now - start).count() << " s");
            return std::chrono::duration<double>(now - start).count();
        }
        const u64 due = static_cast<u64>(device_time * (1.0 + offset) * kDspRate.Value());
        while (produced + audio_out::kDspFrameSamples <= due) {
            ToneFrames(produced, audio_out::kDspFrameSamples, block);
            sink.Queue(block.data(), audio_out::kDspFrameSamples);
            produced += audio_out::kDspFrameSamples;
        }
        tick(device_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ---- reading what the disk driver wrote ---------------------------------------------------------------------------

// Channel 0 of the file, as floats in [-1, 1], in the device's format (never assumed).
std::vector<float> ReadDeviceFile(const std::filesystem::path& path, const SDL_AudioSpec& spec) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "the disk driver wrote no file: " << path);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t sample_bytes = SDL_AUDIO_BITSIZE(spec.format) / 8;
    const std::size_t frame_bytes = sample_bytes * static_cast<std::size_t>(spec.channels);
    REQUIRE(frame_bytes > 0);
    std::vector<float> out(bytes.size() / frame_bytes);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char* p = bytes.data() + i * frame_bytes;
        if (spec.format == SDL_AUDIO_F32) {
            float v;
            std::memcpy(&v, p, 4);
            out[i] = v;
        } else if (spec.format == SDL_AUDIO_S16) {
            s16 v;
            std::memcpy(&v, p, 2);
            out[i] = static_cast<float>(v) / 32768.0f;
        } else if (spec.format == SDL_AUDIO_S32) {
            s32 v;
            std::memcpy(&v, p, 4);
            out[i] = static_cast<float>(static_cast<double>(v) / 2147483648.0);
        } else {
            FAIL("an audio format this test cannot read: " << SDL_GetAudioFormatName(spec.format));
        }
    }
    return out;
}

struct ToneStats {
    double seconds = 0;     // the stretch measured: from the first to the last sample of the tone, less 10 % at each end
    double rms = 0;         // of full scale
    double hz_median = 0;   // from the median interval between rising zero crossings
    double hz_count = 0;    // from the number of rising zero crossings over the whole stretch
};

// Measures the tone in `x` (a device-rate signal): the stretch where it is present, less 10 % at
// both ends (the priming silence, the fade and the end of the run).
ToneStats MeasureTone(const std::vector<float>& x, double rate, double expected_amplitude) {
    ToneStats t;
    const float threshold = static_cast<float>(0.25 * expected_amplitude);
    std::size_t first = 0, last = 0;
    bool found = false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (std::fabs(x[i]) > threshold) {
            if (!found) {
                first = i;
                found = true;
            }
            last = i;
        }
    }
    if (!found || last <= first) {
        return t;
    }
    const std::size_t trim = (last - first) / 10;
    const std::size_t a = first + trim, b = last - trim;
    t.seconds = static_cast<double>(b - a) / rate;
    double sum = 0;
    for (std::size_t i = a; i < b; ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    t.rms = std::sqrt(sum / static_cast<double>(b - a));

    std::vector<double> crossings;  // times, in samples, of the rising zero crossings (interpolated)
    for (std::size_t i = a + 1; i < b; ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            crossings.push_back(static_cast<double>(i - 1) + static_cast<double>(-x[i - 1]) /
                                                                 static_cast<double>(x[i] - x[i - 1]));
        }
    }
    if (crossings.size() >= 3) {
        std::vector<double> periods;
        for (std::size_t i = 1; i < crossings.size(); ++i) {
            periods.push_back(crossings[i] - crossings[i - 1]);
        }
        std::sort(periods.begin(), periods.end());
        t.hz_median = rate / periods[periods.size() / 2];
        t.hz_count = static_cast<double>(crossings.size() - 1) / ((crossings.back() - crossings.front()) / rate);
    }
    return t;
}

double Db(double ratio) { return 20.0 * std::log10(ratio); }

// ---- the feeder on a manual clock, against an SDL stream no device owns --------------------------------------------

constexpr int kSimDeviceRate = 48000;
constexpr int kSimBlock = 480;  // device frames per block

struct SimStream {
    SDL_AudioStream* sdl = nullptr;
    ~SimStream() {
        if (sdl != nullptr) {
            SDL_DestroyAudioStream(sdl);
        }
    }
};

// The DSP on the game clock and a device with a crystal `ppm` parts per million fast, on one manual
// clock, around SDL's own audio stream: converter, resampler and the feeder as its get callback.
// Each device block is SDL_GetAudioStreamData(kSimBlock frames of F32 stereo), the call SDL's audio
// thread makes.
class FeederSim {
public:
    explicit FeederSim(double ppm, std::size_t target_frames = audio_out::MsToFrames(audio_out::kDefaultLatencyMs))
        : stream_(std::make_shared<StreamSink>(target_frames)),
          feeder_(stream_, [this] { return now_s_; }),
          ppm_(ppm) {
        const SDL_AudioSpec src{SDL_AUDIO_S16, 2, Feeder::kSourceRate};
        const SDL_AudioSpec dst{SDL_AUDIO_F32, 2, kSimDeviceRate};
        sim_.sdl = SDL_CreateAudioStream(&src, &dst);
        REQUIRE_MESSAGE(sim_.sdl != nullptr, SDL_GetError());
        REQUIRE(SDL_SetAudioStreamGetCallback(sim_.sdl, &Feeder::Callback, &feeder_));
        out_.resize(static_cast<std::size_t>(kSimBlock) * 2);
        next_block_s_ = BlockTime(1);
    }

    StreamSink& Stream() { return *stream_; }
    Feeder& GetFeeder() { return feeder_; }
    SDL_AudioStream* Sdl() { return sim_.sdl; }
    double Now() const { return now_s_; }
    void SetTime(double seconds) { now_s_ = seconds; }

    // Statistics, from `sample_after_s` of the clock on.
    double sample_after_s = 5.0;
    std::size_t min_fill = ~std::size_t{0}, max_fill = 0;
    double min_ratio = 10.0, max_ratio = 0.0;
    u64 short_blocks = 0;     // blocks after the first that SDL gave fewer bytes than it was asked for
    int first_block_short_bytes = 0;
    u64 blocks = 0;
    bool producing = true;
    bool consuming = true;
    float last_block_peak = 0;  // the largest |sample| of the last block

    // Runs the next event: the producer's audio frame or the device's block, whichever is first.
    void Step() {
        const double next_produce = NextProduceTime();
        const bool produce = producing && next_produce <= next_block_s_;
        const double event = produce ? next_produce : next_block_s_;
        now_s_ = event;
        if (produce) {
            ToneFrames(offered_, audio_out::kDspFrameSamples, scratch_);
            stream_->Queue(scratch_.data(), audio_out::kDspFrameSamples);
            offered_ += audio_out::kDspFrameSamples;
            Sample();
        } else {
            Block();
        }
    }
    void RunUntil(double end_s) {
        while ((producing ? std::min(NextProduceTime(), next_block_s_) : next_block_s_) <= end_s) {
            Step();
        }
        now_s_ = end_s;
    }
    void ResetWindow() {
        min_fill = ~std::size_t{0};
        max_fill = 0;
        min_ratio = 10.0;
        max_ratio = 0.0;
    }
    // The producer stands still (the game clock held), or goes on; the consumer skips its blocks (the device
    // is away) or returns. When the consumer is away its next block is the first one after the pause: the
    // device's clock does not stand still, so the blocks it missed are not made up.
    void StopProducer() { producing = false; }
    void ResumeProducer() {
        producing = true;
        produce_base_s_ = now_s_ - static_cast<double>(offered_) / kDspRate.Value();
    }
    void PauseDevice() { consuming = false; }
    void ResumeDevice() {
        consuming = true;
        while (next_block_s_ <= now_s_) {
            ++block_index_;
            next_block_s_ = BlockTime(block_index_ + 1);
        }
    }

private:
    double NextProduceTime() const {
        return produce_base_s_ + static_cast<double>(offered_ + audio_out::kDspFrameSamples) / kDspRate.Value();
    }
    double BlockTime(u64 k) const {
        return static_cast<double>(k) * kSimBlock / (kSimDeviceRate * (1.0 + ppm_ * 1e-6));
    }
    void Sample() {
        if (now_s_ < sample_after_s) {
            return;
        }
        const std::size_t fill = stream_->Fill();
        min_fill = std::min(min_fill, fill);
        max_fill = std::max(max_fill, fill);
    }
    void Block() {
        if (!consuming) {
            ++block_index_;
            next_block_s_ = BlockTime(block_index_ + 1);
            return;
        }
        Sample();
        const int want = kSimBlock * 2 * static_cast<int>(sizeof(float));
        const int got = SDL_GetAudioStreamData(sim_.sdl, out_.data(), want);
        if (got != want) {
            if (blocks == 0) {
                first_block_short_bytes = want - got;  // SDL's resampler has no history yet: see audio-out.md
            } else {
                ++short_blocks;
            }
        }
        float peak = 0;
        for (float v : out_) {
            peak = std::max(peak, std::fabs(v));
        }
        last_block_peak = peak;
        if (now_s_ >= sample_after_s) {
            min_ratio = std::min(min_ratio, feeder_.Ratio());
            max_ratio = std::max(max_ratio, feeder_.Ratio());
        }
        Sample();
        ++blocks;
        ++block_index_;
        next_block_s_ = BlockTime(block_index_ + 1);
    }

    std::shared_ptr<StreamSink> stream_;
    SimStream sim_;
    double now_s_ = 0;
    Feeder feeder_;
    double ppm_;
    std::vector<s16> scratch_;
    std::vector<float> out_;
    u64 offered_ = 0;
    double produce_base_s_ = 0;
    u64 block_index_ = 0;
    double next_block_s_ = 0;
};

}  // namespace

// ---- the volume ------------------------------------------------------------------------------------------------------

// The curve is Azahar's (src/audio_core/dsp_interface.cpp, OutputCallback: the cube of the linear volume, GPL-2.0-or-later);
// the numbers below are cubes written out, not copied code.
TEST_CASE("the gain is (N / 100) cubed, Azahar's curve: 0 is silence, 100 is 1") {
    CHECK(VolumeGain(0) == 0.0f);
    CHECK(VolumeGain(100) == 1.0f);
    CHECK(VolumeGain(50) == doctest::Approx(0.125f));
    CHECK(VolumeGain(25) == doctest::Approx(0.015625f));
    CHECK(VolumeGain(10) == doctest::Approx(0.001f));
    CHECK(VolumeGain(-7) == 0.0f);   // clamped
    CHECK(VolumeGain(250) == 1.0f);
    CHECK(Db(VolumeGain(50)) == doctest::Approx(-18.06).epsilon(0.001));  // the 18.1 dB of the Done-when
    float last = -1.0f;
    for (int n = 0; n <= 100; ++n) {  // never decreasing
        CHECK(VolumeGain(n) >= last);
        last = VolumeGain(n);
    }
}

TEST_CASE("`sdl` is registered, with --volume and --audio-log, and the stream's options are still there") {
    const auto& sinks = Registry<AudioSinkFactory>::Instance();
    CHECK(sinks.Find("sdl") != nullptr);
    CHECK(sinks.Find("stream") != nullptr);
    CHECK(sinks.Find("null") != nullptr);
    const std::string usage = Usage("runner");
    CHECK(usage.find("--volume N") != std::string::npos);
    CHECK(usage.find("--audio-log FILE") != std::string::npos);
    CHECK(usage.find("--audio-latency MS") != std::string::npos);
    CHECK(Feeder::RatioFor(1.0) == doctest::Approx(32728.498046875 / 32728.0).epsilon(1e-12));
}

// ---- the feeder, on a manual clock ----------------------------------------------------------------------------------

TEST_CASE("the feeder pulls what SDL asks for, and sets the frequency ratio from the controller") {
    FeederSim sim(0);
    Feeder& feeder = sim.GetFeeder();
    std::vector<s16> in;
    ToneFrames(0, 6000, in);
    sim.Stream().Queue(in.data(), 6000);  // more than the target: not priming after the first pull

    // SDL has nothing queued: a request is served in full, and the margin, from the stream
    constexpr int kMargin = static_cast<int>(Feeder::kMarginFrames);
    feeder.OnRequest(sim.Sdl(), 1000 * 4);
    CHECK(SDL_GetAudioStreamQueued(sim.Sdl()) == (1000 + kMargin) * 4);
    CHECK(sim.Stream().Stats().pulled == 1000 + kMargin);  // the stream was already past its target (6000 > 1309)
    // no controller input yet: the first request has no time step, so the factor is 1
    CHECK(feeder.Factor() == 1.0);
    CHECK(feeder.Ratio() == doctest::Approx(Feeder::RatioFor(1.0)));
    CHECK(SDL_GetAudioStreamFrequencyRatio(sim.Sdl()) == doctest::Approx(static_cast<float>(Feeder::RatioFor(1.0))));
    CHECK(feeder.Callbacks() == 1);

    // less than a frame: nothing to do
    feeder.OnRequest(sim.Sdl(), 3);
    feeder.OnRequest(sim.Sdl(), 0);
    CHECK(SDL_GetAudioStreamQueued(sim.Sdl()) == (1000 + kMargin) * 4);
    CHECK(feeder.Callbacks() == 3);

    // a fill far above the target, 50 ms after 50 ms: the controller asks for a faster pull (factor above 1)
    sim.producing = false;
    sim.consuming = false;
    for (int i = 1; i <= 3; ++i) {
        sim.SetTime(0.050 * i);
        feeder.OnRequest(sim.Sdl(), 500 * 4);
    }
    CHECK(feeder.Factor() > 1.0);
    CHECK(feeder.Factor() <= audio_out::DriftController::kMaxFactor);
    CHECK(feeder.SteeredFill() > 3000.0);  // what the controller saw: the fill less half the block
    CHECK(SDL_GetAudioStreamFrequencyRatio(sim.Sdl()) > static_cast<float>(Feeder::RatioFor(1.0)));
}

TEST_CASE("a request larger than the stream's mark is served in full, in chunks") {
    FeederSim sim(0);
    std::vector<s16> in;
    ToneFrames(0, 6000, in);
    sim.Stream().Queue(in.data(), 6000);
    // 10,000 stream frames: one Pull of that size could never be satisfied (the mark is 6546)
    sim.GetFeeder().OnRequest(sim.Sdl(), 10000 * 4);
    CHECK(SDL_GetAudioStreamQueued(sim.Sdl()) == (10000 + static_cast<int>(Feeder::kMarginFrames)) * 4);
    CHECK(sim.Stream().Stats().underruns == 1);  // it ran dry in the second chunk, and was counted, once
}

TEST_CASE("10 simulated minutes with the device's clock 0.01 % fast: the fill holds, SDL always gets its block") {
    FeederSim sim(100.0);
    sim.RunUntil(600.0);
    const double target = static_cast<double>(sim.Stream().Target());
    CHECK(static_cast<double>(sim.min_fill) >= 0.75 * target);
    CHECK(static_cast<double>(sim.max_fill) <= 1.25 * target);
    const auto stats = sim.Stream().Stats();
    CHECK(stats.underruns == 0);
    CHECK(stats.overflows == 0);
    CHECK(stats.dropped == 0);
    CHECK(sim.short_blocks == 0);
    CHECK(sim.min_ratio >= Feeder::RatioFor(audio_out::DriftController::kMinFactor));
    CHECK(sim.max_ratio <= Feeder::RatioFor(audio_out::DriftController::kMaxFactor));
    CHECK(sim.GetFeeder().DiscardedFrames() == 0);
    CHECK(sim.blocks > 59000);
}

TEST_CASE("10 simulated minutes with the device's clock 0.01 % slow") {
    FeederSim sim(-100.0);
    sim.RunUntil(600.0);
    const double target = static_cast<double>(sim.Stream().Target());
    CHECK(static_cast<double>(sim.min_fill) >= 0.75 * target);
    CHECK(static_cast<double>(sim.max_fill) <= 1.25 * target);
    const auto stats = sim.Stream().Stats();
    CHECK(stats.underruns == 0);
    CHECK(stats.overflows == 0);
    CHECK(stats.dropped == 0);
    CHECK(sim.short_blocks == 0);
}

TEST_CASE("10 simulated minutes with a device exactly on the DSP's clock") {
    FeederSim sim(0.0);
    sim.RunUntil(600.0);
    const double target = static_cast<double>(sim.Stream().Target());
    CHECK(static_cast<double>(sim.min_fill) >= 0.75 * target);
    CHECK(static_cast<double>(sim.max_fill) <= 1.25 * target);
    const auto stats = sim.Stream().Stats();
    CHECK(stats.underruns == 0);
    CHECK(stats.overflows == 0);
    CHECK(sim.short_blocks == 0);
}

TEST_CASE("a device 0.3 % fast and one 0.3 % slow: the controller still holds, inside its bounds") {
    for (const double ppm : {3000.0, -3000.0}) {
        CAPTURE(ppm);
        FeederSim sim(ppm);
        sim.RunUntil(600.0);
        const double target = static_cast<double>(sim.Stream().Target());
        CHECK(static_cast<double>(sim.min_fill) >= 0.75 * target);
        CHECK(static_cast<double>(sim.max_fill) <= 1.25 * target);
        const auto stats = sim.Stream().Stats();
        CHECK(stats.underruns == 0);
        CHECK(stats.overflows == 0);
        CHECK(stats.dropped == 0);
        CHECK(sim.short_blocks == 0);
        CHECK(sim.min_ratio >= Feeder::RatioFor(audio_out::DriftController::kMinFactor));
        CHECK(sim.max_ratio <= Feeder::RatioFor(audio_out::DriftController::kMaxFactor));
        // the controller really works at this drift: its factor is away from 1, the right way
        // (a fast device uses the stream faster by itself: the controller slows the pull; a slow one, the opposite)
        CHECK((ppm > 0 ? sim.min_ratio < Feeder::RatioFor(0.999) : sim.max_ratio > Feeder::RatioFor(1.001)));
    }
}

TEST_CASE("a 2 s stop of the producer: one underrun, SDL still gets every block, the fade and silence, recovery") {
    FeederSim sim(0.0);
    sim.RunUntil(10.0);
    REQUIRE(sim.Stream().Stats().underruns == 0);
    sim.StopProducer();
    sim.RunUntil(10.5);
    CHECK(sim.Stream().Stats().underruns == 1);
    sim.RunUntil(11.9);
    CHECK(sim.last_block_peak == 0.0f);  // silence after the fade
    sim.RunUntil(12.0);
    sim.ResumeProducer();
    sim.ResetWindow();
    sim.sample_after_s = 14.0;  // the stream refills to its target, then plays and steers
    sim.RunUntil(30.0);
    const double target = static_cast<double>(sim.Stream().Target());
    CHECK(static_cast<double>(sim.min_fill) >= 0.75 * target);
    CHECK(static_cast<double>(sim.max_fill) <= 1.25 * target);
    CHECK(sim.Stream().Stats().underruns == 1);
    CHECK(sim.short_blocks == 0);
    CHECK(sim.last_block_peak > 0.1f);  // the tone is back
}

TEST_CASE("a device that resumes after the ring reached its mark plays from the target, not from 200 ms of old audio") {
    FeederSim sim(0.0);
    sim.RunUntil(10.0);
    REQUIRE(sim.Stream().Stats().overflows == 0);
    sim.PauseDevice();  // the device is away (a switch of the default device, a pause), the DSP goes on
    sim.RunUntil(11.0);
    const auto before = sim.Stream().Stats();
    CHECK(before.overflows > 0);
    CHECK(sim.Stream().Fill() == audio_out::kOverflowFrames);  // the ring is at its mark
    sim.ResumeDevice();
    sim.RunUntil(11.011);  // the first block after the pause (it finds the ring at its mark and discards the excess)
    sim.ResetWindow();
    sim.sample_after_s = 11.0;
    sim.RunUntil(11.2);  // 20 blocks: audio-out.md says the controller alone needs about 33 s
    const double target = static_cast<double>(sim.Stream().Target());
    CHECK(static_cast<double>(sim.min_fill) >= 0.75 * target);
    CHECK(static_cast<double>(sim.max_fill) <= 1.25 * target);
    CHECK(sim.GetFeeder().DiscardedFrames() > 3000);
    CHECK(sim.Stream().Stats().underruns == 0);
    sim.RunUntil(60.0);
    CHECK(static_cast<double>(sim.min_fill) >= 0.75 * target);
    CHECK(static_cast<double>(sim.max_fill) <= 1.25 * target);
    CHECK(sim.Stream().Stats().underruns == 0);
    CHECK(sim.short_blocks == 0);
}

// ---- the whole sink on SDL's drivers -----------------------------------------------------------------------------------

namespace {

struct DiskTone {
    ToneStats tone;
    SDL_AudioSpec spec{};
    int block_frames = 0;
};

// 2 s of real time of the tone through `--audio sdl` on the disk driver, at the given volume.
DiskTone PlayOnDisk(int volume) {
    const auto path = TempFile("tone_" + std::to_string(volume) + ".raw");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    DiskTone result;
    {
        SdlDriver driver("disk", 882, path.string());
        LogCapture log;
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--volume", std::to_string(volume)}), {Phase::Devices}));
        REQUIRE_FALSE(rt.ExitStatus().has_value());
        REQUIRE(rt.Interfaces().audio != nullptr);
        const auto status = Status(rt);
        REQUIRE(status.has_value());
        CHECK(status->volume == volume);
        CHECK(status->gain == doctest::Approx(VolumeGain(volume)));
        REQUIRE(SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &result.spec, &result.block_frames));
        CHECK(status->device_rate == result.spec.freq);
        CHECK_FALSE(log.HasAtLeast(LogLevel::Warning));
        // paced by the device's own clock (see RunDeviceClockProducer): the host's sleep error in the disk driver's
        // clock would otherwise move the tone's pitch, measured in the file's own timeline, by that much
        RunDeviceClockProducer(rt, *rt.Interfaces().audio, 2.0,
                               static_cast<double>(result.block_frames) / static_cast<double>(result.spec.freq), 0.0,
                               [](double) {});
        const auto after = Status(rt);
        REQUIRE(after.has_value());
        CHECK(after->callbacks > 50);
        CHECK(after->ratio >= Feeder::RatioFor(audio_out::DriftController::kMinFactor));
        CHECK(after->ratio <= Feeder::RatioFor(audio_out::DriftController::kMaxFactor));
    }  // the run is over: the device closes and the file with it
    const std::vector<float> samples = ReadDeviceFile(path, result.spec);
    result.tone = MeasureTone(samples, result.spec.freq, kToneAmplitude * VolumeGain(volume));
    std::filesystem::remove(path, ignored);
    return result;
}

}  // namespace

TEST_CASE("disk driver: a 1 kHz tone queued at 32,728 Hz for 2 s comes out at the device's rate, 1 kHz, at its level") {
    const DiskTone full = PlayOnDisk(100);
    MESSAGE("disk driver device: " << std::string(SDL_GetAudioFormatName(full.spec.format)) << ", " << full.spec.channels << " ch, "
                                   << full.spec.freq << " Hz, " << full.block_frames << " frames per block; tone "
                                   << full.tone.hz_median << " Hz (median), " << full.tone.hz_count << " Hz (count), "
                                   << Db(full.tone.rms / kToneRms) << " dB against the input, over " << full.tone.seconds
                                   << " s");
    CHECK(full.spec.freq > 0);
    CHECK(full.spec.freq != static_cast<int>(kStreamSampleRate));  // SDL converted: the file is at the device's rate
    CHECK(full.tone.seconds > 1.2);
    CHECK(full.tone.hz_median == doctest::Approx(kToneHz).epsilon(0.01));
    CHECK(full.tone.hz_count == doctest::Approx(kToneHz).epsilon(0.01));
    CHECK(std::fabs(Db(full.tone.rms / kToneRms)) <= 1.0);
}

TEST_CASE("disk driver: --volume 50 is 18.1 dB lower, 0 is silence, 25 is 36.1 dB lower") {
    const DiskTone full = PlayOnDisk(100);
    REQUIRE(full.tone.rms > 0);
    const DiskTone half = PlayOnDisk(50);
    REQUIRE(half.tone.seconds > 1.2);
    CHECK(half.tone.hz_median == doctest::Approx(kToneHz).epsilon(0.01));
    CHECK(std::fabs(Db(half.tone.rms / full.tone.rms) + 18.1) <= 0.5);
    CHECK(std::fabs(Db(half.tone.rms / kToneRms) + 18.06) <= 0.5);
    MESSAGE("volume 50: " << Db(half.tone.rms / full.tone.rms) << " dB against volume 100");
    const DiskTone quarter = PlayOnDisk(25);
    CHECK(std::fabs(Db(quarter.tone.rms / full.tone.rms) + 36.12) <= 0.5);
    const DiskTone mute = PlayOnDisk(0);
    CHECK(mute.tone.seconds == 0.0);  // nothing above the tone's threshold in the file
}

TEST_CASE("dummy driver: the controller keeps the fill within 25 % of the target after 5 s, and the audio log is written") {
    const auto log_path = TempFile("audio.csv");
    std::error_code ignored;
    std::filesystem::remove(log_path, ignored);
    // The dummy driver sleeps io_delay = block * 1000 / rate whole milliseconds for each block of the
    // device: 1764 frames at 44,100 Hz is 40 ms exactly. Its clock is the host's sleep, with an error of
    // its own, so the producer is paced by the device's clock (RunDeviceClockProducer) and made 0.3 %
    // fast against it: the drift the controller has to follow, on any host. A target of 100 ms keeps the
    // first block after the priming inside the band (the fill in the middle of a 40 ms block is the
    // target less 20 ms).
    constexpr double kBlockSeconds = 1764.0 / 44100.0;
    constexpr double kProducerOffset = 0.003;
    SdlDriver driver("dummy", 1764);
    LogCapture log;
    audio_out::PublishFrame(1234);
    std::vector<double> steered;
    DeviceStatus last;
    {
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--audio-latency", "100", "--audio-log", log_path.string()}),
                           {Phase::Devices}));
        REQUIRE_FALSE(rt.ExitStatus().has_value());
        const auto stream = audio_out::Find(rt);
        REQUIRE(stream != nullptr);
        CHECK(stream->Target() == audio_out::MsToFrames(100));
        CHECK_FALSE(log.HasAtLeast(LogLevel::Warning));

        // a second thread sets the volume all the time: SetVolume is thread-safe
        std::atomic<bool> stop{false};
        std::atomic<u64> volume_calls{0};
        std::thread volume_thread([&] {
            int v = 0;
            while (!stop.load()) {
                if (SetVolume(rt, v)) {
                    volume_calls.fetch_add(1);
                }
                v = (v + 7) % 101;
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        });
        const double wall_s = RunDeviceClockProducer(rt, *rt.Interfaces().audio, 12.0, kBlockSeconds, kProducerOffset,
                                                     [&](double device_time) {
            if (device_time >= 5.0) {
                if (const auto s = Status(rt)) {
                    steered.push_back(s->steered_fill);
                }
            }
        });
        stop = true;
        volume_thread.join();
        CHECK(volume_calls.load() > 100);
        REQUIRE(Status(rt).has_value());
        last = *Status(rt);
        const auto stats = stream->Stats();
        CHECK(stats.underruns == 0);
        CHECK(stats.overflows == 0);
        CHECK(stats.dropped == 0);
        CHECK(last.discarded_frames == 0);
        CHECK(last.ratio >= Feeder::RatioFor(audio_out::DriftController::kMinFactor));
        CHECK(last.ratio <= Feeder::RatioFor(audio_out::DriftController::kMaxFactor));
        CHECK(last.callbacks >= 12.0 / kBlockSeconds - 1);  // one a block, 40 ms
        // the controller really steered: the DSP is 0.3 % fast against the device, so the stream is used 0.3 % faster
        CHECK(last.factor > 1.0015);
        CHECK(last.factor <= audio_out::DriftController::kMaxFactor);
        MESSAGE("dummy driver: " << last.callbacks << " blocks of 40 ms took " << wall_s << " s of the host's clock ("
                                 << (last.callbacks * kBlockSeconds / wall_s - 1.0) * 100.0 << " % off)");
    }
    // The producer samples the fill once per iteration, and an iteration sleeps 1 ms plus the host's timer
    // slack (about 9 ms on a loaded macOS runner: 746 samples in these 7 s of device time, where an idle
    // host gives about 7,000). So the guard that the window was sampled is counted in device time, at
    // least one sample for every second block of the 7 s, not in iterations of the host's loop.
    REQUIRE(static_cast<double>(steered.size()) > 0.5 * (12.0 - 5.0) / kBlockSeconds);
    const double target = static_cast<double>(audio_out::MsToFrames(100));
    const double lo = *std::min_element(steered.begin(), steered.end());
    const double hi = *std::max_element(steered.begin(), steered.end());
    MESSAGE("dummy driver: the fill the controller steered on after 5 s was " << lo << " to " << hi << " frames, target "
                                                                                << target << "; factor "
                                                                                << last.factor << ", callbacks " << last.callbacks);
    CHECK(lo >= 0.75 * target);
    CHECK(hi <= 1.25 * target);

    // the audio log
    std::ifstream in(log_path);
    REQUIRE(in.good());
    std::string line;
    REQUIRE(std::getline(in, line));
    CHECK(std::regex_match(line, std::regex("# audiolog v1 [0-9]{4}-[0-9]{2}-[0-9]{2} device_rate=[0-9]+")));
    CHECK(line.find("device_rate=44100") != std::string::npos);
    REQUIRE(std::getline(in, line));
    CHECK(line == "t_ms,frame,fill_frames,ratio,underruns,overflows,dropped");
    struct Row {
        long long t_ms, frame, fill, underruns, overflows, dropped;
        double ratio;
    };
    std::vector<Row> rows;
    while (std::getline(in, line)) {
        std::istringstream cells(line);
        Row r{};
        char comma;
        REQUIRE_MESSAGE((cells >> r.t_ms >> comma >> r.frame >> comma >> r.fill >> comma >> r.ratio >> comma >>
                         r.underruns >> comma >> r.overflows >> comma >> r.dropped),
                        "row: " << line);
        rows.push_back(r);
    }
    REQUIRE(rows.size() >= 12);  // t = 0, one a second for 12 s, and the row written when the run ended
    CHECK(rows.front().t_ms < 100);
    for (std::size_t i = 1; i + 1 < rows.size(); ++i) {  // a row per second of host time (+-1 s)
        const long long gap = rows[i].t_ms - rows[i - 1].t_ms;
        CHECK(gap >= 500);
        CHECK(gap <= 1500);
    }
    CHECK(rows.size() <= 14);
    for (const Row& r : rows) {
        CHECK(r.frame == 1234);  // audio_out::LastFrame(), the game's frame counter
        CHECK(r.ratio >= Feeder::RatioFor(audio_out::DriftController::kMinFactor));
        CHECK(r.ratio <= Feeder::RatioFor(audio_out::DriftController::kMaxFactor));
        CHECK(r.underruns == 0);
        CHECK(r.overflows == 0);
        CHECK(r.dropped == 0);
    }
    CHECK(rows.back().t_ms >= 11000);
    // after the 100 ms of priming and the first block the fill is the target's neighbourhood
    CHECK(rows[6].fill > 0.5 * target);
    CHECK(rows[6].fill < 1.5 * target);
    std::filesystem::remove(log_path, ignored);
}

TEST_CASE("the audio log's frame column follows the game's frame, and the log ends with the run") {
    const auto log_path = TempFile("audio_frames.csv");
    std::error_code ignored;
    std::filesystem::remove(log_path, ignored);
    SdlDriver driver("dummy");
    audio_out::PublishFrame(10);
    {
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--audio-log", log_path.string()}), {Phase::Devices}));
        REQUIRE_FALSE(rt.ExitStatus().has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        audio_out::PublishFrame(777);
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        audio_out::PublishFrame(900);
    }  // OnFinish: the last row
    std::ifstream in(log_path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string l; std::getline(stream, l);) {
        lines.push_back(l);
    }
    REQUIRE(lines.size() >= 5);  // header, columns, t = 0, t = 1 s, the end
    CHECK(lines[0].rfind("# audiolog v1 ", 0) == 0);
    // t = 0 saw frame 10; the row of the first second saw 777; the last row (written when the run
    // finished, after PublishFrame(900) and before the stream is forgotten) saw 900
    CHECK(lines[2].find(",10,") != std::string::npos);
    CHECK(lines[3].find(",777,") != std::string::npos);
    CHECK(lines.back().find(",900,") != std::string::npos);
    std::filesystem::remove(log_path, ignored);
}

// ---- devices that do not open, the options, and device events ------------------------------------------------------------

TEST_CASE("a device SDL cannot open: a Warning, the sink plays nothing, the game goes on") {
    const auto bad_file = std::filesystem::temp_directory_path() / "mh4n_no_such_folder" / "audio.raw";
    struct Case {
        const char* what;
        const char* driver;
        std::string disk_file;
    };
    // an audio driver that does not exist (SDL's audio cannot start), and a disk driver that cannot create its file
    for (const Case& c : {Case{"no such driver", "mh4n-no-such-driver", {}}, Case{"disk file", "disk", bad_file.string()}}) {
        CAPTURE(c.what);
        SdlDriver driver(c.driver, 0, c.disk_file);
        LogCapture log;
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--volume", "30"}), {Phase::Devices}));
        CHECK_FALSE(rt.ExitStatus().has_value());     // the game goes on
        REQUIRE(rt.Interfaces().audio != nullptr);    // with a sink that takes the DSP's frames
        CHECK(log.Has(LogLevel::Warning, "game plays without sound"));
        CHECK_FALSE(Status(rt).has_value());
        CHECK_FALSE(SetVolume(rt, 80));               // nothing to set it on, and nothing remembered
        const auto stream = audio_out::Find(rt);
        REQUIRE(stream != nullptr);

        // with no consumer the ring stays full, counts what it discards and never blocks: 10 s of audio
        std::vector<s16> block;
        const auto start = std::chrono::steady_clock::now();
        for (u64 i = 0; i < 2048; ++i) {  // 2048 * 160 frames = 10 s
            ToneFrames(i * 160, 160, block);
            rt.Interfaces().audio->Queue(block.data(), 160);
        }
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        CHECK(stream->Fill() == audio_out::kOverflowFrames);
        CHECK(stream->Stats().dropped > 2048 * 160 - audio_out::kRingFrames - 1);
    }
}

TEST_CASE("--volume and --audio-latency out of range end the run with status 2, without a device") {
    SdlDriver driver("dummy");
    for (const char* bad : {"101", "-1", "loud", "1e2"}) {
        CAPTURE(bad);
        LogCapture log;
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--volume", bad}), {Phase::Devices}));
        CHECK(rt.ExitStatus() == std::optional<int>(2));
        CHECK(rt.Interfaces().audio == nullptr);
        CHECK_FALSE(Status(rt).has_value());
        CHECK(log.HasAtLeast(LogLevel::Error));
    }
    {
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--audio-latency", "9"}), {Phase::Devices}));
        CHECK(rt.ExitStatus() == std::optional<int>(2));  // the stream factory's own check
        CHECK(rt.Interfaces().audio == nullptr);
        CHECK_FALSE(Status(rt).has_value());
    }
    {
        LogCapture log;
        Runtime rt;  // an --audio-log that cannot be created
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--audio-log",
                                  (std::filesystem::temp_directory_path() / "mh4n_no_such_folder" / "a.csv").string()}),
                           {Phase::Devices}));
        CHECK(rt.ExitStatus() == std::optional<int>(2));
        CHECK(rt.Interfaces().audio == nullptr);
        CHECK(log.Has(LogLevel::Error, "cannot write --audio-log"));
    }
    for (const char* ok : {"0", "100", "37"}) {
        CAPTURE(ok);
        Runtime rt;
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--volume", ok}), {Phase::Devices}));
        CHECK_FALSE(rt.ExitStatus().has_value());
        const auto status = Status(rt);
        REQUIRE(status.has_value());
        CHECK(status->volume == std::atoi(ok));
        CHECK(status->gain == doctest::Approx(VolumeGain(std::atoi(ok))));
    }
    {
        Runtime rt;  // the default volume is 100
        REQUIRE(rt.Prepare(Parse({"--audio", "sdl"}), {Phase::Devices}));
        REQUIRE(Status(rt).has_value());
        CHECK(Status(rt)->volume == 100);
        CHECK(Status(rt)->gain == 1.0f);
    }
}

TEST_CASE("SetVolume sets the gain (N / 100)^3 on the SDL stream, clamps, and stops when the run is over") {
    SdlDriver driver("dummy");
    Runtime rt;
    REQUIRE(rt.Prepare(Parse({"--audio", "sdl", "--volume", "20"}), {Phase::Devices}));
    REQUIRE(Status(rt).has_value());
    CHECK(Status(rt)->volume == 20);
    for (const int percent : {0, 25, 50, 75, 100}) {
        CAPTURE(percent);
        CHECK(SetVolume(rt, percent));
        const auto s = Status(rt);
        REQUIRE(s.has_value());
        CHECK(s->volume == percent);
        CHECK(s->gain == doctest::Approx(static_cast<float>(percent * percent * percent) / 1e6f));
    }
    CHECK(SetVolume(rt, 150));
    CHECK(Status(rt)->volume == 100);
    CHECK(Status(rt)->gain == 1.0f);
    CHECK(SetVolume(rt, -5));
    CHECK(Status(rt)->volume == 0);
    CHECK(Status(rt)->gain == 0.0f);

    // another runtime has its own device state; with another sink there is no device
    Runtime other;
    REQUIRE(other.Prepare(Parse({"--audio", "stream"}), {Phase::Devices}));
    CHECK_FALSE(SetVolume(other, 10));
    CHECK_FALSE(Status(other).has_value());
    CHECK(Status(rt)->volume == 0);
}

TEST_CASE("a finished run closes its device and forgets it") {
    SdlDriver driver("dummy");
    for (int round = 0; round < 4; ++round) {
        auto rt = std::make_unique<Runtime>();
        REQUIRE(rt->Prepare(Parse({"--audio", "sdl"}), {Phase::Devices}));
        REQUIRE(Status(*rt).has_value());
        rt.reset();  // Runtime::OnFinish closes the device
        auto next = std::make_unique<Runtime>();
        REQUIRE(next->Prepare(Parse({"--audio", "null"}), {Phase::Devices}));
        CHECK_FALSE(Status(*next).has_value());  // whatever address it got
        CHECK_FALSE(SetVolume(*next, 50));
        // SDL's audio is down again: the device balanced its SDL_InitSubSystem
        const bool audio_up = (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0;
        CHECK_FALSE(audio_up);
    }
}

TEST_CASE("SDL's device events are logged: a removed output device is a Warning") {
    SdlDriver driver("dummy");
    LogCapture log;
    Runtime rt;
    REQUIRE(rt.Prepare(Parse({"--audio", "sdl"}), {Phase::Devices}));
    REQUIRE(Status(rt).has_value());
    auto push = [](SDL_EventType type, bool recording) {
        SDL_Event e{};
        e.type = type;
        e.adevice.which = 7;
        e.adevice.recording = recording;
        SDL_PushEvent(&e);
    };
    push(SDL_EVENT_AUDIO_DEVICE_REMOVED, true);   // a microphone: nothing to say
    CHECK_FALSE(log.Has(LogLevel::Warning, "removed"));
    push(SDL_EVENT_AUDIO_DEVICE_REMOVED, false);
    CHECK(log.Has(LogLevel::Warning, "audio output device was removed"));
    push(SDL_EVENT_AUDIO_DEVICE_ADDED, false);
    CHECK(log.Has(LogLevel::Info, "audio output device was added"));
    push(SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED, false);
    CHECK(log.Has(LogLevel::Info, "changed its format"));
    SDL_FlushEvents(SDL_EVENT_AUDIO_DEVICE_ADDED, SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED);
    // the device keeps going
    CHECK(Status(rt).has_value());
    CHECK_FALSE(rt.ExitStatus().has_value());
}

// ---- the listening test (opt-in, for a person at a real device) ------------------------------------------------------------
//
// Skipped unless MH4N_AUDIO_LISTEN is set (to anything but 0), so ctest never runs it. It plays the
// 1 kHz tone through the real path (--audio sdl, the player's default device, SDL_AUDIO_DRIVER from
// the environment if the player set one) and lists what to listen for on the console; the procedure
// is "Listening test on a real device (pc)" in docs/native/audio-out.md. Environment:
//   MH4N_AUDIO_LISTEN_STEP_S   seconds per volume step (default 10)
//   MH4N_AUDIO_LISTEN_MINUTES  minutes of the free run at volume 100 that follow (default 1)
//   MH4N_AUDIO_LISTEN_LOG      a file for --audio-log
namespace {

bool ListenEnabled() {
    const char* v = std::getenv("MH4N_AUDIO_LISTEN");
    return v != nullptr && *v != '\0' && std::string(v) != "0";
}

double EnvNumber(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const double x = std::strtod(v, &end);
    return (end != v && x > 0.0) ? x : fallback;
}

}  // namespace

TEST_CASE("listen: the tone on the default audio device at volume 100, 50, 25, 0, then a free run (MH4N_AUDIO_LISTEN=1)" *
          doctest::skip(!ListenEnabled())) {
    const double step_s = EnvNumber("MH4N_AUDIO_LISTEN_STEP_S", 10.0);
    const double free_s = EnvNumber("MH4N_AUDIO_LISTEN_MINUTES", 1.0) * 60.0;
    std::vector<std::string> args{"--audio", "sdl"};
    if (const char* log = std::getenv("MH4N_AUDIO_LISTEN_LOG"); log != nullptr && *log != '\0') {
        args.push_back("--audio-log");
        args.push_back(log);
    }
    SetLogFilter(LogFilter(LogLevel::Info));  // the Warning and Info lines of the device go to the console
    Runtime rt;
    REQUIRE(rt.Prepare(Parse(args), {Phase::Devices}));
    REQUIRE_MESSAGE(Status(rt).has_value(), "the default audio device could not be opened (see the Warning above)");
    std::printf("listen: device at %d Hz. Steps of %.0f s: volume 100, 50, 25, 0, 100; then %.0f s at 100.\n",
                Status(rt)->device_rate, step_s, free_s);
    std::fflush(stdout);

    const int volumes[] = {100, 50, 25, 0, 100};
    int step = -1;
    bool free_run_announced = false;
    double next_report = 0.0;
    const double total = step_s * 5.0 + free_s;
    RunProducer(*rt.Interfaces().audio, total, [&](double elapsed) {
        SDL_PumpEvents();  // device events (a removed or switched device) reach the log through here
        const int volume_step = std::min(static_cast<int>(elapsed / step_s), 4);
        if (volume_step != step) {
            step = volume_step;
            SetVolume(rt, volumes[step]);
            std::printf("listen: t=%.0f s: volume %d\n", elapsed, volumes[step]);
            std::fflush(stdout);
        }
        if (!free_run_announced && elapsed >= step_s * 5.0) {
            free_run_announced = true;
            std::printf("listen: t=%.0f s: free run at volume 100 (switch or unplug the output device now)\n", elapsed);
            std::fflush(stdout);
        }
        if (elapsed >= next_report) {
            next_report = elapsed + 5.0;
            const auto stats = audio_out::Find(rt)->Stats();
            std::printf("listen: t=%.0f s fill=%zu underruns=%llu overflows=%llu dropped=%llu\n", elapsed,
                        audio_out::Find(rt)->Fill(), static_cast<unsigned long long>(stats.underruns),
                        static_cast<unsigned long long>(stats.overflows), static_cast<unsigned long long>(stats.dropped));
            std::fflush(stdout);
        }
    });
    const auto stats = audio_out::Find(rt)->Stats();
    std::printf("listen: done. underruns=%llu overflows=%llu dropped=%llu\n", static_cast<unsigned long long>(stats.underruns),
                static_cast<unsigned long long>(stats.overflows), static_cast<unsigned long long>(stats.dropped));
    std::fflush(stdout);
}
