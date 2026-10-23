#include "dsp_replay/tool.h"

#include <fstream>
#include <ostream>

#include "dsp_replay/capture.h"
#include "dsp_replay/replay.h"
#include "mh4n/log.h"

namespace mh4n::dsp_replay {

namespace {

constexpr const char* kUsage =
    "usage: mh4n_dsp_replay CAPTURE.mdsp [--report FILE]\n"
    "  CAPTURE.mdsp  an MDSP capture of the reference runner (--capture-dsp)\n"
    "  --report FILE also write the settling, every window and every judged frame that is not bit-exact\n"
    "exit status: 0 PASS, 1 FAIL, 2 a usage error or a capture that cannot be read\n";

// The DSP says "Application has requested initialization" at Info; the tool's output is its result lines.
struct QuietLog {
    LogFilter saved = GetLogFilter();
    QuietLog() { SetLogFilter(LogFilter(LogLevel::Warning)); }
    ~QuietLog() { SetLogFilter(saved); }
};

}  // namespace

int RunDspReplay(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const QuietLog quiet;
    std::string path;
    std::string report;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help") {
            out << kUsage;
            return 0;
        }
        if (args[i] == "--report") {
            if (i + 1 >= args.size()) {
                err << "error: --report needs a file\n" << kUsage;
                return 2;
            }
            report = args[++i];
        } else if (!args[i].empty() && args[i][0] == '-') {
            err << "error: unknown option " << args[i] << "\n" << kUsage;
            return 2;
        } else if (path.empty()) {
            path = args[i];
        } else {
            err << "error: more than one capture\n" << kUsage;
            return 2;
        }
    }
    if (path.empty()) {
        err << "error: no capture given\n" << kUsage;
        return 2;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        err << "dsp-replay: cannot open " << path << "\n";
        return 2;
    }
    Verdict result;
    try {
        result = Replay(file);
    } catch (const CaptureError& error) {
        err << "dsp-replay: " << error.what() << "\n";
        return 2;
    }
    WriteDetails(result, out);
    out << SummaryLine(result) << "\n";
    if (!report.empty()) {
        std::ofstream rep(report);
        if (!rep) {
            err << "dsp-replay: cannot write " << report << "\n";
            return 2;
        }
        WriteDetails(result, rep);
        rep << SummaryLine(result) << "\n";
    }
    return result.pass ? 0 : 1;
}

}  // namespace mh4n::dsp_replay
