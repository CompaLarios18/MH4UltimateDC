// Parsing of the runner's scripts: times, input events and scheduled options (N0-08).
//
// This file and schedule.cpp include only the C++ standard library (no Azahar headers): the
// native runner (N1-09) reuses them, so a script means the same thing on every runtime.
//
// A time is either emulated milliseconds ("1500", "1500.5") or a game frame ("f600"): the first
// moment the game's frame counter (the u32 at 0x010571d4) reads 600 or more. Hold durations of
// input events take the same two forms: "200" is 200 ms, "f5" is 5 frames.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Schedule {

/// A moment: emulated milliseconds, or a frame number of the game's frame counter.
struct When {
    bool by_frame = false;
    double ms = 0.0;       // when !by_frame
    std::uint32_t frame = 0; // when by_frame

    /// "1500", "1500.5" or "f600" (a whole number of frames); false on anything else, a negative
    /// or non-finite number included.
    static bool Parse(const std::string& text, When& out);

    /// Whether the moment has come, given the emulated time and the frame counter now.
    bool Due(double now_ms, std::uint32_t now_frame) const {
        return by_frame ? now_frame >= frame : now_ms >= ms;
    }

    /// "1500ms" or "f600".
    std::string ToString() const;
};

/// How long an input event is held: milliseconds, or frames.
struct Hold {
    bool by_frame = false;
    double ms = 100.0;
    std::uint32_t frames = 0;

    static bool Parse(const std::string& text, Hold& out);
    std::string ToString() const;
};

/// The button names an input script accepts, in this order (Press::button indexes them).
constexpr int kButtonCount = 15;
const char* ButtonName(int index);

enum class PressKind { Button, Touch, Circle };

/// One input event. Touch: x, y in pixels of the bottom screen; circle pad: -1..1.
struct Press {
    When start;
    Hold hold;
    PressKind kind = PressKind::Button;
    int button = 0; // index into the button names, for kind == Button
    float x = 0.0f, y = 0.0f;

    /// Whether the event is being held at this time and frame. The first call after the start
    /// has come remembers the moment, so a hold of the other unit than the start still has an end.
    bool Active(double now_ms, std::uint32_t now_frame);

private:
    bool started_ = false;
    double started_ms_ = 0.0;
    std::uint32_t started_frame_ = 0;
};

struct Dump {
    When when;
    std::uint32_t addr = 0, size = 0;
    bool done = false;
};

struct Replace {
    When when;
    std::vector<std::uint8_t> from, to;
    bool done = false;
};

/// --set WHEN:KEY=VALUE
struct OptionChange {
    When when;
    std::string key, value;
    bool done = false;
};

/// --freeze WHEN:HOST_MS
struct Freeze {
    When when;
    double host_ms = 0.0;
    bool done = false;
};

/// --call-counts WHEN
struct CallCounts {
    When when;
    bool done = false;
};

/// Everything a run schedules.
struct Plan {
    std::vector<Press> presses;
    std::vector<Dump> dumps;
    std::vector<Replace> replaces;
    std::vector<OptionChange> option_changes;
    std::vector<CallCounts> call_counts;
    std::vector<Freeze> freezes;
    bool has_shots_from = false;
    When shots_from;
    double stop_ms = 10000.0;        // --ms (without --ms but with --frames: no time limit)
    bool has_stop_ms = false;
    std::uint32_t stop_frame = 0;    // --frames; 0: none
    bool has_stop_frame = false;
};

/// Each parser returns false and sets `error` (a short phrase naming what is wrong) on failure,
/// and then leaves the plan unchanged.

/// --input "3000:A,3500:START:200,5000:TOUCH:160:120,6000:CIRCLE:0:1:f30": comma-separated
/// events WHEN:BUTTON[:HOLD], WHEN:TOUCH:x:y[:HOLD], WHEN:CIRCLE:x:y[:HOLD]. The hold
/// defaults to 100 ms.
bool ParseInput(const std::string& script, Plan& plan, std::string& error);

/// --input-file: the same events, one per line (commas may also separate several on a line);
/// `#` starts a comment, blank lines are skipped. `error` names the line.
bool ParseInputText(const std::string& text, Plan& plan, std::string& error);
bool ParseInputFile(const std::string& path, Plan& plan, std::string& error);

bool ParseDump(const std::string& arg, Plan& plan, std::string& error);     // WHEN:ADDR:SIZE (hex)
bool ParseReplace(const std::string& arg, Plan& plan, std::string& error);  // WHEN:FROM:TO (hex bytes)
bool ParseOptionChange(const std::string& arg, Plan& plan, std::string& error); // WHEN:KEY=VALUE
bool ParseFreeze(const std::string& arg, Plan& plan, std::string& error);   // WHEN:HOST_MS
bool ParseCallCounts(const std::string& arg, Plan& plan, std::string& error); // WHEN
bool ParseShotsFrom(const std::string& arg, Plan& plan, std::string& error);  // WHEN
/// --ms: a number of milliseconds, or f<N> (the same as --frames N).
bool ParseStop(const std::string& arg, Plan& plan, std::string& error);
/// --frames N
bool ParseFrames(const std::string& arg, Plan& plan, std::string& error);

/// The text --print-schedule prints: a header `# schedule v1`, then one line per scheduled
/// item as `<kind> at=<when> ...`, in this order: stop, press, dump, replace, set, freeze,
/// call-counts, shots-from.
std::string Describe(const Plan& plan);

} // namespace Schedule
