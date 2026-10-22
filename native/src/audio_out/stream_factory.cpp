// The `stream` AudioSink's registration (N3-05): the factory, --audio-latency, Find and the frame
// subscription. The ring itself is stream_sink.cpp.
#include <iterator>
#include <map>
#include <mutex>

#include "audio_out/audio_out.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::audio_out {

namespace {
constexpr const char* kLog = "AudioOut";
}

// ---- the sink the runtime owns, and finding the stream -------------------------------------------------

namespace {

// The runtime owns this; the stream itself is shared with whoever found it (the app's device).
class StreamFront final : public AudioSink {
public:
    explicit StreamFront(std::shared_ptr<StreamSink> stream) : stream_(std::move(stream)) {}
    void Queue(const s16* stereo, std::size_t frames) override { stream_->Queue(stereo, frames); }

private:
    std::shared_ptr<StreamSink> stream_;
};

std::mutex g_registry_mutex;
std::map<const Runtime*, std::weak_ptr<StreamSink>>& Streams() {
    static std::map<const Runtime*, std::weak_ptr<StreamSink>> streams;
    return streams;
}

std::unique_ptr<AudioSink> MakeStream(Runtime& rt) {
    const Options& options = rt.GetOptions();
    s64 latency_ms = kDefaultLatencyMs;
    try {
        latency_ms = options.GetInt("--audio-latency", kDefaultLatencyMs);
    } catch (const OptionError& e) {
        MH4N_LOG(Error, kLog, "bad --audio-latency: {}", e.what());
        rt.RequestExit(2, "bad --audio-latency");
        return nullptr;
    }
    if (latency_ms < kMinLatencyMs || latency_ms > kMaxLatencyMs) {
        MH4N_LOG(Error, kLog, "--audio-latency {} is outside {} to {} ms", latency_ms, kMinLatencyMs,
                 kMaxLatencyMs);
        rt.RequestExit(2, "bad --audio-latency");
        return nullptr;
    }

    auto stream = std::make_shared<StreamSink>(MsToFrames(static_cast<double>(latency_ms)));
    {
        std::lock_guard lock(g_registry_mutex);
        auto& streams = Streams();
        for (auto it = streams.begin(); it != streams.end();) {
            it = it->second.expired() ? streams.erase(it) : std::next(it);
        }
        streams[&rt] = stream;
    }
    rt.OnFrame([](u32 frame, u64) { PublishFrame(frame); });
    // The registry is keyed by the runtime's address: forget the entry when the run ends, so that a
    // later runtime at the same address (with another sink) cannot find this stream. Whoever holds
    // the pointer keeps the stream.
    rt.OnFinish([key = static_cast<const Runtime*>(&rt)] {
        std::lock_guard lock(g_registry_mutex);
        Streams().erase(key);
    });
    MH4N_LOG(Info, kLog, "stream sink: target {} ms ({} frames), ring {} frames", latency_ms,
             stream->Target(), kRingFrames);
    return std::make_unique<StreamFront>(std::move(stream));
}

MH4N_OPTION("runner", "--audio-latency", "MS",
            "with --audio stream: the audio stream's target fill, 10 to 200 ms (default 40)");
MH4N_REGISTER(AudioSinkFactory, "stream", [](Runtime& rt) { return MakeStream(rt); });

}  // namespace

std::shared_ptr<StreamSink> Find(Runtime& runtime) {
    std::lock_guard lock(g_registry_mutex);
    const auto it = Streams().find(&runtime);
    return it == Streams().end() ? nullptr : it->second.lock();
}

}  // namespace mh4n::audio_out
