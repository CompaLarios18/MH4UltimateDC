// The runtime's own `devices` boot step: the GPU, the input source and the audio sink named by
// --gpu, --input-source and --audio, made from their registries. A name nothing has registered
// leaves that interface empty, with an Info line (the null GPU is N1-17's, the script input N1-19's).
#include <string>

#include "mh4n/gpu.h"
#include "mh4n/host.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Runtime";

void CreateDevices(Runtime& rt) {
    const Options& options = rt.GetOptions();

    const std::string gpu_name = options.GetString("--gpu", "null");
    if (const auto* factory = Registry<gpu::GpuFactory>::Instance().Find(gpu_name)) {
        rt.SetGpu((*factory)(rt.GpuHost(), options));
    } else {
        MH4N_LOG(Info, kLog, "no GPU named '{}' is registered: running without one", gpu_name);
    }

    const std::string input_name = options.GetString("--input-source", "script");
    if (const auto* factory = Registry<InputSourceFactory>::Instance().Find(input_name)) {
        rt.SetInputSource((*factory)(rt));
    } else {
        MH4N_LOG(Info, kLog, "no input source named '{}' is registered: running without one", input_name);
    }

    const std::string audio_name = options.GetString("--audio", "null");
    if (const auto* factory = Registry<AudioSinkFactory>::Instance().Find(audio_name)) {
        rt.SetAudioSink((*factory)(rt));
    } else {
        MH4N_LOG(Info, kLog, "no audio sink named '{}' is registered: running without one", audio_name);
    }
}

MH4N_REGISTER(BootStep, "runtime.devices", BootStep{Phase::Devices, CreateDevices});

}  // namespace

}  // namespace mh4n
