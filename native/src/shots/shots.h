// The native runner's screenshots (N2b-14).
//
// `--shots DIR` saves both screens as binary PPM images, with the reference runner's options, names
// and timing (port/README.md): `--shot-every MS|fN` (default 1000 ms) and `--shots-from MS|fN`, the
// files named `f%07u_top.ppm` / `f%07u_bottom.ppm` for frames and `%07u_top.ppm` for milliseconds. The
// schedule is Schedule::ShotSchedule itself, asked once per published frame with the game clock's
// milliseconds and the game's frame counter: a shot is taken at the first published frame that
// reads the scheduled frame or more (or that is at the scheduled time or after it), and a frame the
// counter skipped has no shot.
//
// ShotSink is a present::ScreenSink. Publish runs on whatever thread publishes (the scheduler
// thread under `null-display`, the renderer thread behind `queue`), never waits for another thread,
// and asks the Display for both screens; the `done` callbacks (inside ReadScreen or later, on any
// thread) write the files. Finish, called at the end of the run, waits for the callbacks still
// pending and writes `gas.txt`.
//
// `DIR/gas.txt` has one line per shot that follows a published frame with `gas` set since the
// previous shot (the frame the shot is taken from included): the shot's number, without padding and
// without the `f` (`300` for `f0000300_top.ppm`, `1500` for `0001500_top.ppm`). The file is written
// at the end of every run with `--shots`, empty when there was no gas, so its absence means a runner
// that does not report gas (the reference).
#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mh4n/present.h"
#include "mh4n/types.h"

#if MH4N_HAVE_SCHEDULE

#include "schedule.h"

namespace mh4n::shots {

class ShotSink final : public present::ScreenSink {
public:
    // `dir` must exist. `display` reads the screens; it must outlive the sink's last Publish (the
    // `done` callbacks it makes later never touch the sink or `display`).
    ShotSink(const Schedule::ShotSchedule& schedule, std::filesystem::path dir, present::Display& display);

    void Publish(const present::DisplayFrame& frame) override;

    // Waits (up to `timeout`) until every requested screen has been written or given up, then writes
    // gas.txt. False when callbacks were still pending at the timeout. Call it after the last Publish.
    bool Finish(std::chrono::milliseconds timeout);

    // Counters, for tests and the end-of-run line: shots taken (a shot is two screens), files written,
    // screens that came back empty (no file for them).
    u32 ShotsTaken() const { return shots_taken_; }
    u32 FilesWritten() const;
    u32 EmptyScreens() const;

private:
    struct State {
        std::filesystem::path dir;
        std::mutex mutex;
        std::condition_variable cv;
        u32 pending = 0;
        u32 written = 0;
        u32 empty = 0;
        std::vector<std::string> gas_shots;
    };

    void Request(const std::string& name, const std::optional<present::ScreenImage>& screen);

    Schedule::ShotSchedule schedule_;
    present::Display& display_;
    std::shared_ptr<State> state_;
    bool gas_since_last_shot_ = false;
    u32 shots_taken_ = 0;
};

// The PPM file of an image (P6, maxval 255). False when it cannot be written.
bool WritePpm(const std::filesystem::path& path, const present::RgbImage& image);

}  // namespace mh4n::shots

#endif  // MH4N_HAVE_SCHEDULE
