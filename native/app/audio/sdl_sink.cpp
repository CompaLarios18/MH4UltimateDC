// Derived from Azahar src/audio_core/dsp_interface.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: the file says "Licensed under GPLv2 or any later version"); only the volume curve of DspInterface::OutputCallback (the cube of the linear volume) is taken
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project
//
// The `sdl` audio sink of mh4u-native (N3-06): the device, the volume, the audio log and the
// registrations. The callback itself is feeder.cpp.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "audio/app_audio.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::app_audio {

namespace {

constexpr const char* kLog = "AudioSdl";

// ---- SDL's device events -----------------------------------------------------------------------------------

// SDL posts these when the player plugs, unplugs or switches an output device (the events reach the
// watch while the app pumps SDL's events). An SDL stream opened on SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK
// follows the system's default device by itself: it moves to the new default (the stream converts to
// the new format), and when no device is left it keeps consuming into nothing until one appears. The
// game never notices; the log does.
bool SDLCALL OnSdlEvent(void* /*userdata*/, SDL_Event* event) {
    switch (event->type) {
    case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        if (!event->adevice.recording) {
            MH4N_LOG(Warning, kLog,
                     "an audio output device was removed (SDL id {}): the sound follows the system's default "
                     "device, and plays to nowhere until there is one",
                     static_cast<u32>(event->adevice.which));
        }
        break;
    case SDL_EVENT_AUDIO_DEVICE_ADDED:
        if (!event->adevice.recording) {
            MH4N_LOG(Info, kLog, "an audio output device was added (SDL id {})", static_cast<u32>(event->adevice.which));
        }
        break;
    case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
        if (!event->adevice.recording) {
            MH4N_LOG(Info, kLog, "an audio output device changed its format (SDL id {})",
                     static_cast<u32>(event->adevice.which));
        }
        break;
    default:
        break;
    }
    return true;
}

// ---- the audio log (--audio-log) -------------------------------------------------------------------------------

// A CSV, one row per second of host time, written by a thread of its own and never from the audio
// callback (docs/native/audio-out.md). The last row is written when the log stops.
class AudioLog {
public:
    AudioLog(const std::string& path, Feeder& feeder) : file_(path, std::ios::out | std::ios::trunc), feeder_(feeder) {}
    ~AudioLog() { Stop(); }

    bool ok() const { return file_.is_open() && file_.good(); }

    void Start(int device_rate) {
        const std::time_t now = std::time(nullptr);
        char date[16] = "0000-00-00";
        if (const std::tm* utc = std::gmtime(&now)) {
            std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
        }
        file_ << "# audiolog v1 " << date << " device_rate=" << device_rate << "\n"
              << "t_ms,frame,fill_frames,ratio,underruns,overflows,dropped\n";
        file_.flush();
        start_ = std::chrono::steady_clock::now();
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() {
        {
            std::lock_guard lock(mutex_);
            if (!thread_.joinable()) {
                return;
            }
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

private:
    void Run() {
        for (u64 k = 1;; ++k) {
            WriteRow();
            std::unique_lock lock(mutex_);
            if (cv_.wait_until(lock, start_ + std::chrono::seconds(k), [this] { return stop_; })) {
                break;
            }
        }
        WriteRow();  // the end of the run, for whoever reads the last row
    }

    void WriteRow() {
        audio_out::StreamStats stats = feeder_.Stream().Stats();
        const auto t_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
        file_ << t_ms << ',' << audio_out::LastFrame() << ',' << feeder_.Stream().Fill() << ','
              << fmt::format("{:.6f}", feeder_.Ratio()) << ',' << stats.underruns << ',' << stats.overflows << ','
              << stats.dropped << '\n';
        file_.flush();
    }

    std::ofstream file_;
    Feeder& feeder_;
    std::chrono::steady_clock::time_point start_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

// ---- the device ---------------------------------------------------------------------------------------------------

class Device {
public:
    Device(std::shared_ptr<audio_out::StreamSink> stream, int volume)
        : feeder_(std::move(stream)), volume_(volume) {}
    ~Device() { Close(); }

    // Logs a Warning and returns false when the device cannot be opened; the sink then plays nothing.
    bool Open() {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            MH4N_LOG(Warning, kLog, "SDL's audio could not start: {}; the game plays without sound", SDL_GetError());
            return false;
        }
        init_ = true;
        watching_ = SDL_AddEventWatch(OnSdlEvent, nullptr);

        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, Feeder::kSourceRate};
        SDL_AudioStream* sdl = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Feeder::Callback, &feeder_);
        if (sdl == nullptr) {
            MH4N_LOG(Warning, kLog, "the default audio device could not be opened: {}; the game plays without sound",
                     SDL_GetError());
            Close();
            return false;
        }
        {
            std::lock_guard lock(mutex_);
            sdl_ = sdl;
            SDL_SetAudioStreamGain(sdl_, VolumeGain(volume_));
        }
        // The device starts paused; the callback fires from here on.
        SDL_ResumeAudioStreamDevice(sdl);

        SDL_AudioSpec device{};
        int frames = 0;
        SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, &frames);
        const char* driver = SDL_GetCurrentAudioDriver();
        MH4N_LOG(Info, kLog, "SDL audio ({}): the stream at {} Hz on a device at {} Hz, {} channels, {} frames per block",
                 driver != nullptr ? driver : "?", Feeder::kSourceRate, device.freq, device.channels, frames);
        return true;
    }

    bool IsOpen() const {
        std::lock_guard lock(mutex_);
        return sdl_ != nullptr;
    }

    void AttachLog(std::unique_ptr<AudioLog> log) { log_ = std::move(log); }
    void StartLog() {
        if (log_) {
            log_->Start(DeviceRate());
        }
    }

    // Stops the callback, closes the device and the log. Safe to call twice.
    void Close() {
        {
            std::lock_guard lock(mutex_);
            if (sdl_ != nullptr) {
                SDL_DestroyAudioStream(sdl_);  // closes the device; the callback has stopped when it returns
                sdl_ = nullptr;
            }
        }
        if (log_) {
            log_->Stop();
        }
        if (watching_) {
            SDL_RemoveEventWatch(OnSdlEvent, nullptr);
            watching_ = false;
        }
        if (init_) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            init_ = false;
        }
    }

    bool SetVolume(int percent) {
        std::lock_guard lock(mutex_);
        if (sdl_ == nullptr) {
            return false;
        }
        volume_ = percent;
        return SDL_SetAudioStreamGain(sdl_, VolumeGain(percent));
    }

    std::optional<DeviceStatus> Describe() {
        std::lock_guard lock(mutex_);
        if (sdl_ == nullptr) {
            return std::nullopt;
        }
        DeviceStatus s;
        s.volume = volume_;
        s.gain = SDL_GetAudioStreamGain(sdl_);
        SDL_AudioSpec dst{};
        if (SDL_GetAudioStreamFormat(sdl_, nullptr, &dst)) {
            s.device_rate = dst.freq;
        }
        s.ratio = feeder_.Ratio();
        s.factor = feeder_.Factor();
        s.steered_fill = feeder_.SteeredFill();
        s.callbacks = feeder_.Callbacks();
        s.discarded_frames = feeder_.DiscardedFrames();
        return s;
    }

    Feeder& GetFeeder() { return feeder_; }

private:
    int DeviceRate() {
        std::lock_guard lock(mutex_);
        SDL_AudioSpec dst{};
        if (sdl_ != nullptr && SDL_GetAudioStreamFormat(sdl_, nullptr, &dst)) {
            return dst.freq;
        }
        return 0;
    }

    Feeder feeder_;
    mutable std::mutex mutex_;  // sdl_ and volume_
    SDL_AudioStream* sdl_ = nullptr;
    int volume_;
    bool init_ = false;
    bool watching_ = false;
    std::unique_ptr<AudioLog> log_;
};

// ---- finding the device of a runtime ---------------------------------------------------------------------------

std::mutex g_registry_mutex;
std::map<const Runtime*, std::weak_ptr<Device>>& Devices() {
    static std::map<const Runtime*, std::weak_ptr<Device>> devices;
    return devices;
}

std::shared_ptr<Device> FindDevice(Runtime& runtime) {
    std::lock_guard lock(g_registry_mutex);
    const auto it = Devices().find(&runtime);
    return it == Devices().end() ? nullptr : it->second.lock();
}

// ---- the sink ------------------------------------------------------------------------------------------------------

std::unique_ptr<AudioSink> MakeSdl(Runtime& rt) {
    const Options& options = rt.GetOptions();
    s64 volume = kDefaultVolume;
    try {
        volume = options.GetInt("--volume", kDefaultVolume);
    } catch (const OptionError& e) {
        MH4N_LOG(Error, kLog, "bad --volume: {}", e.what());
        rt.RequestExit(2, "bad --volume");
        return nullptr;
    }
    if (volume < 0 || volume > 100) {
        MH4N_LOG(Error, kLog, "--volume {} is outside 0 to 100", volume);
        rt.RequestExit(2, "bad --volume");
        return nullptr;
    }

    // The stream: made by its own factory (it reads --audio-latency and may end the run itself).
    const AudioSinkFactory* stream_factory = Registry<AudioSinkFactory>::Instance().Find("stream");
    if (stream_factory == nullptr) {
        MH4N_LOG(Error, kLog, "the `stream` audio sink is not registered");
        rt.RequestExit(2, "no stream sink");
        return nullptr;
    }
    std::unique_ptr<AudioSink> sink = (*stream_factory)(rt);
    std::shared_ptr<audio_out::StreamSink> stream = sink ? audio_out::Find(rt) : nullptr;
    if (!sink || !stream) {
        return nullptr;
    }

    auto device = std::make_shared<Device>(stream, static_cast<int>(volume));
    if (const auto path = options.Get("--audio-log")) {
        auto log = std::make_unique<AudioLog>(*path, device->GetFeeder());
        if (!log->ok()) {
            MH4N_LOG(Error, kLog, "cannot write --audio-log {}", *path);
            rt.RequestExit(2, "cannot write --audio-log");
            return nullptr;
        }
        device->AttachLog(std::move(log));
    }
    // Open early, before the game makes any sound (a late device finds old audio). A device that does
    // not open leaves the sink playing nothing: the stream's ring stays full and counts what it drops.
    device->Open();
    device->StartLog();
    {
        std::lock_guard lock(g_registry_mutex);
        auto& devices = Devices();
        for (auto it = devices.begin(); it != devices.end();) {
            it = it->second.expired() ? devices.erase(it) : std::next(it);
        }
        devices[&rt] = device;
    }
    rt.OnFinish([device, key = static_cast<const Runtime*>(&rt)] {
        device->Close();
        std::lock_guard lock(g_registry_mutex);
        Devices().erase(key);
    });
    return sink;
}

MH4N_OPTION("runner", "--volume", "N", "with --audio sdl: the volume, 0 to 100 (default 100), gain (N/100)^3");
MH4N_OPTION("runner", "--audio-log", "FILE",
            "with --audio sdl: a CSV of the audio stream, one row per second (t_ms, frame, fill_frames, ratio, "
            "underruns, overflows, dropped)");
MH4N_REGISTER(AudioSinkFactory, "sdl", [](Runtime& rt) { return MakeSdl(rt); });

}  // namespace

float VolumeGain(int percent) {
    // A cubic curve approximates a linear change in perceived loudness (Azahar's slider).
    const float linear = static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f;
    return linear * linear * linear;
}

bool SetVolume(Runtime& runtime, int percent) {
    const std::shared_ptr<Device> device = FindDevice(runtime);
    return device != nullptr && device->SetVolume(std::clamp(percent, 0, 100));
}

std::optional<DeviceStatus> Status(Runtime& runtime) {
    const std::shared_ptr<Device> device = FindDevice(runtime);
    return device ? device->Describe() : std::nullopt;
}

}  // namespace mh4n::app_audio
