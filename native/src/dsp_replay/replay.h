// The replay (N3-04): an MDSP capture through our dsp_hle::Dsp, frame by frame, with the captured
// regions as its input and the captured sample bytes (by (address, size, CRC) key) as its memory; the
// result is D75's verdict. docs/native/dsp-replay.md has the method.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "dsp_replay/judge.h"
#include "mh4n/types.h"

namespace mh4n::dsp_replay {

struct Listed {
    u32 index = 0;     // the position in the capture
    u32 counter = 0;   // the DSP's frame counter
    u32 max_diff = 0;  // the largest |reference - ours| in the frame
};

struct Verdict {
    u32 total_frames = 0;
    // Settling
    bool settled = false;                  // every source enabled in the first frame settled
    u32 first_judged_index = 0;            // meaningful when settled
    u32 first_judged_counter = 0;
    std::vector<u32> unsettled_sources;    // when not settled
    // What was judged
    u32 judged = 0;
    u32 bit_exact = 0;
    Span whole;
    std::vector<Window> windows;           // positions in the judged list
    std::vector<Span> window_spans;
    bool has_worst = false;
    double worst_window_db = 0;
    u32 worst_window_counter = 0;
    std::vector<Listed> not_exact;         // every judged frame that is not bit-exact
    // The status blocks
    u32 status_frames_compared = 0;
    bool status_equal = true;
    u32 first_status_difference_counter = 0;
    u32 first_status_difference_index = 0;
    // The verdict
    bool pass = false;
    std::string fail_reason;               // "not settled", "snr", "window", "status", "snr and status"...
};

// Replays the capture in `capture`; throws CaptureError for one that cannot be read, has no frames, or a
// frame without its output record, or a type-3 record whose key was never given bytes.
Verdict Replay(std::istream& capture);

// The line of the spec: `dsp-replay: <n> frames judged of <total>, <m> bit-exact, snr <dB>, worst window
// <dB> at <frame>, status <equal|first difference at frame k>, PASS|FAIL`.
std::string SummaryLine(const Verdict& result);
// The lines before it: settling, and every judged frame that is not bit-exact.
void WriteDetails(const Verdict& result, std::ostream& out);

}  // namespace mh4n::dsp_replay
