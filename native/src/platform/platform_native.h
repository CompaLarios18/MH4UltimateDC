// The hooks' platform on the native runtime (N1-20): what port/src/hooks.cpp needs of the program
// that runs the game (port/src/platform.h), answered from the runtime instead of from Azahar.
//
//   - GameSeconds is the game clock (never the host's): ticks over 268111856, paused when the clock is;
//   - SetRefreshRate goes to Runtime::SetRefreshRate, and from there to the GSP's VBlank period;
//   - UserDir is --user (the folder that holds mh4u.ini), Log is MH4N_LOG;
//   - the screen layout (top-screen rows and width, the overlay) is stored for the renderer (N2b);
//   - network sync does nothing and the end-skip vote is always agreed: local only until N4.
//
// Options of the runner registered by this module (scope `runner`):
//   --set MS|fN:KEY=VALUE   at that game time (or game frame), change one of the hooks' options
//                           (the keys of mh4u.ini that can change while the game runs); repeatable;
//   --options FILE          the host's own settings, in the layout of mh4u-app.ini (`[Section]`
//                           lines, `key = value` lines, `#` and `;` comment lines): stored, with
//                           `aspect_ratio` also answering Platform::GetOption.
//
// Threads (principle 6): the hooks call the platform on the scheduler thread, from guest code; so
// does the timer queue and the frame watcher that apply --set. The stored layout is guarded by a
// mutex all the same, since the renderer reads it from its own thread.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "platform.h"  // port/src/platform.h
#include "mh4n/runtime.h"
#include "mh4n/types.h"

#if MH4N_HAVE_SCHEDULE
#include "schedule.h"
#endif

struct RCodeInfo;

namespace mh4n::platform {

// The host settings of an mh4u-app.ini-style text, key to value (the section names are dropped; a
// later value of a key wins). False and a message in *error for a line that is neither blank, a
// comment, a section header nor `key = value`.
bool ParseAppOptions(std::string_view text, std::map<std::string, std::string>* options,
                     std::string* error);

class NativePlatform final : public Port::Platform, public std::enable_shared_from_this<NativePlatform> {
public:
    // `user_dir` is the folder mh4u.ini lives in, as the hooks get it from UserDir(): with a
    // trailing separator when it is not empty. `app_options` are the host settings of --options.
    NativePlatform(Runtime& runtime, std::string user_dir, std::map<std::string, std::string> app_options);
    ~NativePlatform() override;

    // ---- Port::Platform ------------------------------------------------------------------------
    void Log(Port::LogLevel level, const std::string& message) override;
    std::string UserDir() override { return user_dir_; }
    void SetRefreshRate(double hz) override;
    void SetTopScreenRows(std::uint32_t rows) override;
    double GameSeconds() override;
    bool IsRunning() override;
    void RefreshScreenLayout() override;
    void SetScreenOverlay(const Port::ScreenOverlay& overlay) override;
    void SetTopScreenWidth(std::uint32_t width) override;
    bool SetOption(const std::string& key, const std::string& value) override;
    std::string GetOption(const std::string& key) override;
    void InstallNetSync(Port::NetSyncMode mode, double slack_ms, std::vector<std::string>& log) override;
    void VoteEndSkip(bool vote) override;
    bool EndSkipAgreed() override;

    // ---- what the renderer (N2b) reads -----------------------------------------------------------
    std::uint32_t TopScreenRows() const;      // 0 until the hooks set it
    std::uint32_t TopScreenWidth() const;     // 0: the 3DS's 400
    Port::ScreenOverlay Overlay() const;
    // How many times the hooks asked for the layout to be worked out again.
    std::uint64_t LayoutRefreshes() const;
    // The last refresh rate the hooks asked for (0: the 3DS's own).
    double RequestedRefreshRate() const;
    // This console's vote to end the wait after a quest, as the hooks last cast it.
    bool EndSkipVote() const;
    // The settings of --options (all of them, including aspect_ratio).
    std::map<std::string, std::string> AppOptions() const;

    // The platform made for `runtime` by InstallPlatform, while it is alive.
    static std::shared_ptr<NativePlatform> Find(Runtime& runtime);

private:
    Runtime& runtime_;
    std::string user_dir_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> app_options_;
    std::string aspect_ratio_ = "default";
    std::uint32_t top_rows_ = 0;
    std::uint32_t top_width_ = 0;
    Port::ScreenOverlay overlay_;
    std::uint64_t layout_refreshes_ = 0;
    double refresh_hz_ = 0.0;
    bool end_skip_vote_ = false;
};

// The `started` step, in three parts (the tests call them one by one):
// 1. makes the platform from the runner's options (--user, --options), gives it to the hooks
//    (Hooks::SetPlatform) and keeps it alive with the runtime. On a bad --options file it logs,
//    calls Runtime::RequestExit(2, ...) and returns null.
std::shared_ptr<NativePlatform> InstallPlatform(Runtime& runtime);

// 2. Hooks::Install on the code library `info` (after InstallPlatform), every line it returns logged
//    as the Azahar build does ("hooks: <line>"). The step does it only when a code library is loaded.
void InstallHooks(const RCodeInfo& info);
// 3. Parses the --set options and schedules them (ScheduleOptionChanges, with Hooks::SetOption). After
//    InstallHooks, which reads the hooks' options afresh. False, after RequestExit(2, ...), for a
//    bad --set.
bool InstallOptionChanges(Runtime& runtime);

#if MH4N_HAVE_SCHEDULE
// Applies the changes of --set: each at its game time (the clock's tick when the platform was made
// plus the milliseconds, on the timer queue, so exact in ticks), or at the first frame of the game's
// counter that has reached its frame, in the order given. Changes due now (0 ms) are applied before
// this returns. `apply` takes the key and the value and returns false for a key that cannot change
// while the game runs: the run then ends with status 2 (docs/native/runner.md). Everything runs on
// the scheduler thread.
using ApplyOption = std::function<bool(const std::string& key, const std::string& value)>;
void ScheduleOptionChanges(Runtime& runtime, const std::vector<Schedule::OptionChange>& changes,
                           ApplyOption apply);
#endif

}  // namespace mh4n::platform
