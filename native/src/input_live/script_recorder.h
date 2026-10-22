// Writes the states the game saw as a script (N2b-08, --record-input): the grammar of
// port/src/schedule.h, the one --input and --input-file read.
//
//     f<N>:<BUTTON>:f<held>
//     f<N>:TOUCH:<x>:<y>:f<held>
//     f<N>:CIRCLE:<x>:<y>:f<held>
//
// A press is keyed on the game's frame counter once the counter has been seen; before that (and in
// programs that never map it) on milliseconds of game time, held for milliseconds. A press keeps the
// unit it started in. The C-stick, ZL/ZR and the motion sensors are not written: the buttons of the
// script are those of the pad word, and N5-08 widens the grammar.
//
// This class knows nothing about threads: the scheduler thread feeds it (LiveInput).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "mh4n/host.h"
#include "mh4n/types.h"

namespace mh4n::input_live {

// A moment of the run: the frame counter as last seen (if it has been) and the game time since the
// source was made.
struct Stamp {
    bool have_frame = false;
    u32 frame = 0;
    double ms = 0.0;
};

class ScriptRecorder {
public:
    // The state the game sees at `at`. Calls with the same state are free; a change closes the
    // presses that ended and opens the ones that started.
    void Update(const InputState& state, const Stamp& at);

    // Closes what is still held (through the last frame seen, or until `at` in milliseconds) and
    // returns the whole script: a header comment, then one line per press in the order they
    // started. Call once.
    std::string Finish(const Stamp& at);

private:
    struct Open {
        bool active = false;
        Stamp start;
        u32 seq = 0;       // the order the presses started in
        int x = 0, y = 0;  // circle pad: raw units; touch: pixels
    };
    struct Line {
        Stamp start;
        u32 seq = 0;
        std::string text;  // without the hold
        std::string hold;
    };
    static constexpr int kButtons = 12;

    void Close(Open& press, const std::string& head, const Stamp& at);

    std::array<Open, kButtons> buttons_;
    Open circle_;
    Open touch_;
    std::vector<Line> lines_;
    u32 next_seq_ = 0;
};

// The numbers as the script reads them back (exposed for the tests).
// Milliseconds: the shortest decimal that parses back to the same double.
std::string FormatMs(double ms);
// The value after TOUCH: that the script source turns back into pixel `pixel` of an axis of `size`
// pixels (320 or 240): its float round trip loses a pixel for some values, so some are written as
// a fraction.
std::string FormatTouchAxis(int pixel, int size);
// The value after CIRCLE: that turns back into raw circle pad reading `raw`.
std::string FormatCircleAxis(int raw);

}  // namespace mh4n::input_live
