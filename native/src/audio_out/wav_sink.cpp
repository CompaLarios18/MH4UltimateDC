// The `wav` AudioSink (N3-05): every queued frame into a 16-bit stereo WAV file at kStreamSampleRate,
// for tests and for listening to a headless run. --audio wav --audio-file FILE. The header's sizes are
// written when the run ends (Runtime::OnFinish). It is a test sink: Queue writes to the file on the
// scheduler thread, and nothing paces on it.
#include <memory>

#include "audio_out/audio_out.h"
#include "audio_out/wav_writer.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::audio_out {

namespace {

constexpr const char* kLog = "AudioOut";

class WavSink final : public AudioSink {
public:
    bool Open(const std::string& path) { return writer_.Open(path, kStreamSampleRate, 2); }

    void Queue(const s16* stereo, std::size_t frames) override {
        if (!writer_.Write(stereo, frames) && !failed_logged_) {
            failed_logged_ = true;
            MH4N_LOG(Error, kLog, "wav sink: a write failed; the file is incomplete");
        }
    }

    void Finish() {
        if (!writer_.Close() && !failed_logged_) {
            failed_logged_ = true;
            MH4N_LOG(Error, kLog, "wav sink: the file could not be finished");
        }
    }

private:
    WavWriter writer_;
    bool failed_logged_ = false;
};

// The runtime owns this; Runtime::OnFinish holds the sink as well, so it is closed in either order.
class WavFront final : public AudioSink {
public:
    explicit WavFront(std::shared_ptr<WavSink> sink) : sink_(std::move(sink)) {}
    void Queue(const s16* stereo, std::size_t frames) override { sink_->Queue(stereo, frames); }

private:
    std::shared_ptr<WavSink> sink_;
};

std::unique_ptr<AudioSink> MakeWav(Runtime& rt) {
    const auto path = rt.GetOptions().Get("--audio-file");
    if (!path || path->empty()) {
        MH4N_LOG(Error, kLog, "--audio wav needs --audio-file FILE");
        rt.RequestExit(2, "--audio wav needs --audio-file");
        return nullptr;
    }
    auto sink = std::make_shared<WavSink>();
    if (!sink->Open(*path)) {
        MH4N_LOG(Error, kLog, "cannot write {}", *path);
        rt.RequestExit(2, "cannot write " + *path);
        return nullptr;
    }
    rt.OnFinish([sink] { sink->Finish(); });
    return std::make_unique<WavFront>(std::move(sink));
}

MH4N_OPTION("runner", "--audio-file", "FILE",
            "with --audio wav: the 16-bit stereo WAV file (32,728 Hz) that gets every frame the DSP makes");
MH4N_REGISTER(AudioSinkFactory, "wav", [](Runtime& rt) { return MakeWav(rt); });

}  // namespace

}  // namespace mh4n::audio_out
