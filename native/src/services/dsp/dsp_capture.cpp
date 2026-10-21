// --capture-dsp START:FRAMES:FILE (N1-18): from the first audio frame at or after game frame START
// (the game's frame counter, kFrameCounterAddress), records FRAMES audio frames to FILE as an MDSP
// capture (tools/native/capfmt.py reads it; its type-1 records are what `capfmt.py diff-dsp` compares
// with the reference's, N1-21). The syntax is mh4u_runner's: START and FRAMES are decimal numbers of
// at most nine digits, START may be written f<N>, FRAMES is above 0 and FILE is the rest (it may
// contain colons). The records are written by the DSP service (dsp_dsp.cpp), at the start of each
// frame; this step opens the file before the program runs. A capture that cannot be opened or an
// argument that does not parse ends the run with status 2.
#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "dsp_shared.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Audio.DSP";

MH4N_OPTION("runner", "--capture-dsp", "START:FRAMES:FILE",
            "from the first DSP audio frame at or after game frame START, record FRAMES audio frames "
            "to FILE (an MDSP capture, tools/native/capfmt.py)");

struct CaptureArgs {
    u32 start = 0;
    u32 frames = 0;
    std::string file;
};

bool Numeric(const std::string& text) {
    return !text.empty() && text.size() <= 9 &&
           std::all_of(text.begin(), text.end(),
                       [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
}

bool ParseCaptureDsp(const std::string& arg, CaptureArgs* out) {
    const auto first = arg.find(':');
    const auto second = first == std::string::npos ? first : arg.find(':', first + 1);
    if (second == std::string::npos || second + 1 >= arg.size()) {
        return false;
    }
    std::string start_text = arg.substr(0, first);
    if (!start_text.empty() && start_text[0] == 'f') {
        start_text.erase(0, 1);
    }
    const std::string frames_text = arg.substr(first + 1, second - first - 1);
    if (!Numeric(start_text) || !Numeric(frames_text)) {
        return false;
    }
    out->start = static_cast<u32>(std::stoul(start_text));
    out->frames = static_cast<u32>(std::stoul(frames_text));
    out->file = arg.substr(second + 1);
    return out->frames > 0;
}

void StartDspCapture(Runtime& rt) {
    const std::optional<std::string> arg = rt.GetOptions().Get("--capture-dsp");
    if (!arg) {
        return;
    }
    CaptureArgs args;
    if (!ParseCaptureDsp(*arg, &args)) {
        const std::string message = "bad --capture-dsp: " + *arg +
                                    " (want START:FRAMES:FILE, START a game frame, FRAMES above 0)";
        MH4N_LOG(Error, kLog, "{}", message);
        rt.RequestExit(2, message);
        return;
    }
    ServiceManager* const manager = FindServiceManager(rt);
    if (manager == nullptr) {
        MH4N_LOG(Error, kLog, "--capture-dsp needs the services phase");
        rt.RequestExit(2, "--capture-dsp needs the services phase");
        return;
    }
    const std::shared_ptr<DspCapture> capture = manager->Context().State<DspCapture>("dsp::capture");
    const u64 program_id = rt.Image() != nullptr ? rt.Image()->program_id : 0;
    if (!capture->writer.Open(args.file, program_id)) {
        rt.RequestExit(2, "cannot write " + args.file);
        return;
    }
    capture->start_frame = args.start;
    capture->frames = args.frames;
    capture->started = false;
    capture->origin_tick = rt.Clock().Now();
    rt.OnFinish([capture] { capture->writer.Close(); });
}

MH4N_REGISTER(BootStep, "tracing.dsp-capture", BootStep{Phase::Tracing, StartDspCapture});

}  // namespace

}  // namespace mh4n
