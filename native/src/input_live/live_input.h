// The live input source (N2b-08, D59): `--input-source live`. The app feeds it from any thread
// (Set, SetTouch); the game's HID module reads it on the scheduler thread (Sample) and sees a change
// only between two of its frames, so the session can be written down as a frame-keyed script
// (--record-input) that both runners replay.
//
// Options of the runner registered by this module (scope `runner`):
//   --input-latch frame|none   `frame` (the default): the state set last becomes the one Sample
//                              returns at each change of the game's frame counter
//                              (Runtime::OnFrame), and stays until the next one. `none`: Sample
//                              returns the state set last at once; such a session cannot be replayed
//                              exactly (the recording keys it on the frame it was seen in);
//   --record-input FILE        writes every change of the state Sample returns as script lines
//                              (script_recorder.h), when the run ends; milliseconds of game time
//                              before the frame counter has been seen.
//
// Threads: Set and SetTouch (any thread, the main thread in the app) and Sample, OnFrame (the
// scheduler thread) share one state under a mutex, which is held for the copy of a few bytes and
// never across a callback. Everything else is the scheduler thread's alone.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "input_live/script_recorder.h"
#include "mh4n/host.h"
#include "mh4n/types.h"

namespace mh4n {
class Runtime;
}

namespace mh4n::input_live {

// A point of the bottom screen, in pixels (320 x 240).
struct TouchPoint {
    u16 x = 0;
    u16 y = 0;
};

enum class Latch { Frame, None };

class LiveInput final : public InputSource {
public:
    // `runtime` supplies the frame counter (Sample looks at it, as the script source does) and may
    // be null, a test's; `start_tick` is the game clock's tick at which recorded time 0 is.
    LiveInput(Runtime* runtime, u64 start_tick, Latch latch = Latch::Frame);
    ~LiveInput() override;

    // ---- any thread -------------------------------------------------------------------------------

    // Everything but the touch fields (the shell's touch tracker owns them): a key pressed while the
    // stylus is down never lifts it. The touch fields of `state` are ignored.
    void Set(const InputState& state);
    // Only the touch fields: a point, or nullopt for the stylus up.
    void SetTouch(std::optional<TouchPoint> touch);

    // ---- the scheduler thread ----------------------------------------------------------------------

    InputState Sample(u64 tick) override;
    // The frame counter changed (Runtime::OnFrame calls it; tests call it by hand).
    void OnFrame(u32 frame, u64 tick);

    // Starts the recording (--record-input). FinishRecording closes the presses still held at
    // `tick` and returns the script.
    void StartRecording();
    std::string FinishRecording(u64 tick);
    bool Recording() const { return recording_; }

    Latch GetLatch() const { return latch_; }

private:
    InputState Snapshot() const;
    Stamp StampAt(u64 tick) const;

    Runtime* runtime_;
    u64 start_tick_;
    Latch latch_;

    mutable std::mutex mutex_;
    InputState pending_;  // what Set and SetTouch made; the only thing two threads share

    // The scheduler thread's
    InputState latched_;
    bool frame_seen_ = false;
    u32 frame_ = 0;
    bool recording_ = false;
    ScriptRecorder recorder_;
};

// The live source of `runtime` (made by the `devices` step for --input-source live), or null. The
// runtime owns the source; the handle keeps the part Set and SetTouch use alive, so a call after
// the runtime is gone changes nothing and is safe.
// Call it once and keep the shared_ptr: it takes a mutex and does a map lookup.
std::shared_ptr<LiveInput> Find(Runtime& runtime);

}  // namespace mh4n::input_live
