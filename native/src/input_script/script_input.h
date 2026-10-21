// The scripted input source (N1-16): the presses of --input and --input-file, played back on the
// game clock and the game's frame counter. A script means the same thing as in the reference runner
// (port/src/runner.cpp, ScriptButton / ScriptCirclePad / ScriptTouch): a press is held from its
// start for its hold, in milliseconds of game time since the program started or in frames; the first
// press of a kind that is held wins.
//
// Nothing here reads the host's clock: the tick comes from the caller (the HID module's timer
// event) and the frame from the game's counter at that moment (Runtime::PollFrame), so a script
// replays the same way every run.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mh4n/host.h"

#if MH4N_HAVE_SCHEDULE
#include "schedule.h"
#endif

namespace mh4n {

class Runtime;

namespace input_script {

#if MH4N_HAVE_SCHEDULE

class ScriptInput final : public InputSource {
public:
    // `runtime` supplies the frame counter and may be null (frames then stay 0); `start_tick` is the
    // game clock's tick at which script time 0 is.
    ScriptInput(Schedule::Plan plan, Runtime* runtime, u64 start_tick);

    // The inputs held at game tick `tick`, at the frame the counter reads now (a look at it through
    // the runtime, if there is one). Must be called with non-decreasing ticks (a press remembers
    // the moment it started).
    InputState Sample(u64 tick) override;

    // The game's frame counter as the source knows it: the runtime's OnFrame callbacks (and a test)
    // set it through this; the source keeps the cell alive for as long as the callbacks do.
    std::shared_ptr<u32> FrameCell() const { return frame_; }
    u32 Frame() const { return *frame_; }

private:
    Schedule::Plan plan_;
    Runtime* runtime_;
    u64 start_tick_;
    std::shared_ptr<u32> frame_ = std::make_shared<u32>(0);
};

// The two shapes of a script on the command line (both repeatable): false and a message on a
// script that does not parse.
bool ParseScripts(const std::vector<std::string>& inputs, const std::vector<std::string>& files,
                  Schedule::Plan* plan, std::string* error);

#endif

}  // namespace input_script
}  // namespace mh4n
