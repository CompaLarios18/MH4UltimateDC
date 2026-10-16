// The capture player (N2-02): replays an MGPU capture into a gpu::Gpu and checks it against the
// capture. The program is native/tools/capture_player; the manual is docs/native/capture-player.md.
#pragma once

#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gpu_capture/image.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"

namespace mh4n::gpu_capture {

// What `--judge` picks. Commands of other kinds are still sent to the Gpu, then their GPU_PAGES
// are applied in place of its output and they are reported as skipped.
enum JudgeKind : u32 {
    kJudgeFill = 1u << 0,
    kJudgeTransfer = 1u << 1,
    kJudgeCopy = 1u << 2,
    kJudgeDma = 1u << 3,
    kJudgeCmdList = 1u << 4,
    kJudgeFrames = 1u << 5,
    kJudgeCacheFlush = 1u << 6,
};
inline constexpr u32 kDefaultJudge =
    kJudgeFill | kJudgeTransfer | kJudgeCopy | kJudgeDma | kJudgeCmdList | kJudgeFrames;

// The kind names, in the order they are listed: fill, transfer, copy, dma, cmdlist, frames,
// cacheflush.
std::vector<std::string> JudgeKindNames();
// "fill,dma", "none", "all". False with *error for an unknown name.
bool ParseJudge(const std::string& text, u32& mask, std::string* error);

struct PlayerConfig {
    std::string capture_path;
    std::string gpu_name;
    bool isolated = false;          // --isolated; the default is chained
    u32 judge = kDefaultJudge;
    std::vector<std::string> checks;
    bool skip_gas_frames = false;
    bool fail_fast = false;
    std::string frames_dir;         // the reference PPMs; empty: <capture>.frames, then <capture without extension>.frames
    std::string dump_dir;           // --dump-frames
};

// Reads the "capture-player" options (positional 0 is the capture). False with *error when
// something is missing or malformed.
bool ConfigFromOptions(const Options& options, PlayerConfig& config, std::string* error);

// A command or event that did not match the capture.
struct Failure {
    size_t record = 0;       // 0-based index in the file
    u32 frame = 0;           // the record's frame stamp
    std::string record_name; // "GX SUBMIT_CMDLIST", "FRAME", ...
    std::string message;
};

struct ScreenResult {
    bool compared = false;   // both images existed and could be compared
    ImageMetrics metrics;
    std::string note;        // why it was not compared, or how it differs
};

struct FrameRow {
    u32 frame = 0;
    size_t record = 0;
    bool gas = false;        // the command lists since the last FRAME used fog mode 7
    bool judged = false;     // the images were judged (frames in --judge, and not skipped as gas)
    bool passed = true;      // images judged OK (or not judged) and no command failed in the frame
    u32 command_failures = 0;
    ScreenResult top, bottom;
    u64 cpu_ns = 0;          // time spent inside the Gpu since the last FRAME
    std::optional<u64> gpu_ns;
    std::vector<gpu::Counter> counters;
    std::vector<std::pair<std::string, std::string>> columns; // added by checks
};

struct PlayerResult {
    std::string error;                        // set: the run could not complete (status 2)
    std::vector<Failure> failures;
    std::vector<FrameRow> frames;
    size_t records = 0;
    std::map<std::string, u32> judged;        // commands judged, by kind
    std::map<std::string, u32> skipped;       // commands sent to the Gpu but not judged, by kind
    u32 unjudged_pages = 0;                   // reference pages of command lists outside any colour buffer
    std::vector<std::string> check_columns;   // the columns checks declared, in order

    int ExitStatus() const {
        return !error.empty() ? 2 : (failures.empty() ? 0 : 1);
    }
};

// Replays `config.capture_path`. `options` are the whole command line (the Gpus, wrappers and
// checks read their own options from it).
PlayerResult RunPlayer(const PlayerConfig& config, const Options& options);

// The frame table, the failures and the summary, as text.
std::string FormatReport(const PlayerResult& result);

// The registered Gpus, wrappers and checks, and the options of the "capture-player" scope.
std::string ListRegistered();

// The program: parses `args` (without the program name), runs, prints the report to `out` and
// errors to `err`. Returns the exit status (0 all passed, 1 something failed, 2 usage or an
// unreadable capture).
int RunCapturePlayer(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace mh4n::gpu_capture
