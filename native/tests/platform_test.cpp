// platform_test.cpp - N1-20.
//
// mh4n_platform: the hooks' platform (port/src/platform.h) on the native runtime. The refresh rate
// is checked against the real GSP state (the VBlank period of N1-17) on a manual clock, not against
// a mock; the hooks are the port's real hooks.cpp, installed on a fake code-library table and run
// through their own `sMhMain::frame` hook. Time is a ManualTimeSource: nothing here sleeps as
// synchronisation, and nothing reads the host's clock.
#include <doctest/doctest.h>

#if !__has_include("recomp.h") || !__has_include("hooks.h")
TEST_CASE("platform_test needs recomp.h and the port's hooks.h (the repository around native/)" * doctest::skip()) {}
#else

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "hooks.h"
#include "../src/common/utf8.h"
#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "platform/platform_native.h"
#include "../src/services/gsp/gsp.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr u32 kText = 0x00100000;
constexpr u32 kFramePage = 0x01057000;  // holds kFrameCounterAddress
// The pages the hooks read when sMhMain::frame runs (the game's statics, from SMAIN_INSTANCE up to
// the panel manager); mapped as zeros, so every pointer the hooks follow is null.
constexpr u32 kGamePagesBase = 0x01057000;
constexpr u32 kGamePagesSize = 0x00049000;
// hooks.cpp's table entry for sMhMain::frame (config/hooks.txt): the hook that sets the refresh rate.
constexpr u32 kFrameHookAddress = 0x00af5250;

void Put16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v), b[at + 1] = static_cast<u8>(v >> 8);
}
void Put32(std::vector<u8>& b, std::size_t at, u32 v) {
    Put16(b, at, v & 0xFFFF), Put16(b, at + 2, v >> 16);
}

// An ARM ELF with one read-execute segment at kText.
std::vector<u8> MakeElf() {
    std::vector<u8> b(52 + 32, 0);
    b[0] = 0x7F, b[1] = 'E', b[2] = 'L', b[3] = 'F', b[4] = 1, b[5] = 1, b[6] = 1;
    Put16(b, 16, 2), Put16(b, 18, 40), Put32(b, 20, 1), Put32(b, 24, kText);
    Put32(b, 28, 52), Put16(b, 40, 52), Put16(b, 42, 32), Put16(b, 44, 1);
    Put32(b, 52, 1);  // PT_LOAD
    Put32(b, 52 + 4, 84), Put32(b, 52 + 8, kText), Put32(b, 52 + 12, kText);
    Put32(b, 52 + 16, 0x40), Put32(b, 52 + 20, 0x40), Put32(b, 52 + 24, 5), Put32(b, 52 + 28, 0x1000);
    for (u32 i = 0; i < 0x40; ++i) {
        b.push_back(static_cast<u8>(i));
    }
    return b;
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_platform_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string Path() const { return path_.string(); }
    std::string File(const std::string& name) const { return (path_ / name).string(); }
    std::string WriteElf() const {
        const std::string path = File("program.elf");
        const std::vector<u8> bytes = MakeElf();
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                    static_cast<std::streamsize>(bytes.size()));
        return path;
    }
    void WriteText(const std::string& name, const std::string& text) const {
        std::ofstream(File(name)) << text;
    }

private:
    std::filesystem::path path_;
};

struct Fixture {
    ManualTimeSource* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    std::vector<std::string> logged;
    Fixture() {
        // The hooks read MH4U_* variables after mh4u.ini; a test must not depend on the host's.
#if !defined(_WIN32)
        unsetenv("MH4U_FPS");
        unsetenv("MH4U_REFRESH_RATE");
        unsetenv("MH4U_NET_SYNC");
        unsetenv("MH4U_NET_SYNC_SLACK");
#endif
        auto source = std::make_unique<ManualTimeSource>();
        time = source.get();
        RuntimeConfig config;
        config.time = std::move(source);
        runtime = std::make_unique<Runtime>(std::move(config));
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { logged.push_back(std::string(r.message)); });
    }
    ~Fixture() {
        runtime.reset();
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    int LoggedCount(const std::string& needle) const {
        int n = 0;
        for (const std::string& line : logged) {
            n += line.find(needle) != std::string::npos ? 1 : 0;
        }
        return n;
    }
    bool Logged(const std::string& needle) const { return LoggedCount(needle) > 0; }

    // Moves the game clock to `tick` (never back) and runs what became due.
    void AdvanceTo(u64 tick) {
        while (runtime->Clock().Now() < tick) {
            time->Advance(static_cast<s64>(TicksToNsCeil(tick - runtime->Clock().Now())));
        }
        runtime->Scheduler().RunPending();
    }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

std::shared_ptr<gsp::GspState> StateOf(Fixture& f) {
    ServiceManager* manager = FindServiceManager(*f.runtime);
    REQUIRE(manager != nullptr);
    return manager->Context().State<gsp::GspState>("gsp::Gpu");
}

// Runs `body` on a guest thread of the runtime (its fiber, on this host thread, as the hooks run) and
// returns once it has finished and what became due when it ended has run.
void RunOnGuestThread(Fixture& f, std::function<void()> body) {
    Kernel& kernel = f.runtime->GetKernel();
    GuestScheduler& scheduler = f.runtime->Scheduler();
    bool finished = false;
    auto thread = kernel.NewThread("guest", 0x30, 0, kernel.CurrentProcess());
    scheduler.AttachFunction(thread, [&] {
        body();
        finished = true;
    });
    scheduler.Start(thread);
    REQUIRE(scheduler.Run([&] { return finished; }) == RunStatus::Stopped);
    REQUIRE(finished);
}

// Changes the current directory for its lifetime and puts it back, also when a check throws.
class CurrentDirGuard {
public:
    explicit CurrentDirGuard(const std::string& to) : saved_(std::filesystem::current_path()) {
        std::filesystem::current_path(to);
    }
    ~CurrentDirGuard() {
        std::error_code ignored;
        std::filesystem::current_path(saved_, ignored);
    }
    CurrentDirGuard(const CurrentDirGuard&) = delete;
    CurrentDirGuard& operator=(const CurrentDirGuard&) = delete;

private:
    std::filesystem::path saved_;
};

// A fake code-library table: records the hooks the port installs on it.
std::map<u32, RHookFn>* g_hooks = nullptr;
int FakeSetHook(u32 addr, RHookFn hook) {
    (*g_hooks)[addr] = hook;
    return 1;
}
void OriginalStub(RCtx*) {}

// The runtime prepared with the devices and services (the real GSP, with its VBlank timer), the
// platform of the runner's options installed, and the hooks installed on a fake table.
struct HookRun {
    Fixture f;
    std::map<u32, RHookFn> hooks;
    RCodeInfo info{};
    std::shared_ptr<platform::NativePlatform> platform;
    RCtx ctx{};

    explicit HookRun(const std::vector<std::string>& args) {
        g_hooks = &hooks;
        REQUIRE(f.runtime->Prepare(Parse(args), {Phase::Devices, Phase::Services}));
        REQUIRE(IsSuccess(f.runtime->Memory().Map(kGamePagesBase, kGamePagesSize, MemPerm::ReadWrite,
                                                  MemState::Private)));
        info.hook_count = 1;
        info.set_hook = FakeSetHook;
        ctx.mem = f.runtime->Memory().Base();
        platform = platform::InstallPlatform(*f.runtime);
        REQUIRE(platform != nullptr);
        platform::InstallHooks(info);
        REQUIRE(platform::InstallOptionChanges(*f.runtime));
    }
    ~HookRun() { g_hooks = nullptr; }
    // One run of the game's frame function through the port's hook.
    void Frame() {
        REQUIRE(hooks.count(kFrameHookAddress) == 1);
        hooks[kFrameHookAddress](&ctx, OriginalStub);
        f.runtime->Scheduler().RunPending();
    }
};

constexpr u64 kDefaultPeriod = 4481136;  // gsp::kFrameTicks
constexpr u64 kPeriod120 = 2234265;      // 268111856 / 120, truncated

}  // namespace

// ---- the host's settings file --------------------------------------------------------------------------

TEST_CASE("platform: ParseAppOptions reads mh4u-app.ini's layout") {
    std::map<std::string, std::string> options;
    std::string error;
    const std::string text =
        "# the app's settings\r\n"
        "[Game]\r\n"
        "new_3ds = off\r\n"
        "\r\n"
        "[Display]\n"
        "; comment\n"
        "renderer=vulkan\n"
        "  aspect_ratio =  16:9  \n"
        "perf_log = D:\\perf\\run 1.csv\n"
        "aspect_ratio = 4:3\n";  // a later value wins
    REQUIRE(platform::ParseAppOptions(text, &options, &error));
    CHECK(options.size() == 4);
    CHECK(options["new_3ds"] == "off");
    CHECK(options["renderer"] == "vulkan");
    CHECK(options["aspect_ratio"] == "4:3");
    CHECK(options["perf_log"] == "D:\\perf\\run 1.csv");

    CHECK(platform::ParseAppOptions("", &options, &error));
    CHECK(options.empty());

    CHECK_FALSE(platform::ParseAppOptions("[Game]\nnonsense\n", &options, &error));
    CHECK(error == "line 2: expected key = value");
    CHECK_FALSE(platform::ParseAppOptions(" = 3\n", &options, &error));
    CHECK(error == "line 1: expected key = value");
}

TEST_CASE("platform: --set and --options are registered runner options") {
    const Options ok = ParseOptions("runner", std::vector<std::string>{"program", "--set", "0:fps=120", "--set",
                                                                     "f600:fps=60", "--options", "app.ini"});
    REQUIRE_MESSAGE(ok.ok(), ok.error());
    CHECK(ok.All("--set") == std::vector<std::string>{"0:fps=120", "f600:fps=60"});
    CHECK(ok.GetString("--options") == "app.ini");
}

// ---- the platform ---------------------------------------------------------------------------------------

TEST_CASE("platform: GameSeconds is the game clock, not the host's") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    platform::NativePlatform p(*f.runtime, "", {});
    const double start = p.GameSeconds();
    CHECK(start == doctest::Approx(static_cast<double>(f.runtime->Clock().Now()) / 268111856.0));

    // The host's clock moves (this thread really sleeps); the game's does not.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(p.GameSeconds() == start);

    f.time->Advance(1500000000);  // 1.5 s of game time
    CHECK(p.GameSeconds() - start == doctest::Approx(1.5).epsilon(1e-12));

    // It stops with the clock, as Azahar's global tick count stops with the emulation.
    f.runtime->Clock().Pause();
    const double paused = p.GameSeconds();
    f.time->Advance(1000000000);
    CHECK(p.GameSeconds() == paused);
    f.runtime->Clock().Resume();
    f.time->Advance(250000000);
    CHECK(p.GameSeconds() - paused == doctest::Approx(0.25).epsilon(1e-12));
}

TEST_CASE("platform: the refresh rate reaches the GSP's VBlank period") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
    auto state = StateOf(f);
    platform::NativePlatform p(*f.runtime, "", {});
    const u64 start = f.runtime->Clock().Now();
    const u64 first = start + kDefaultPeriod;
    CHECK(state->VBlankPeriodTicks() == kDefaultPeriod);

    // The hooks call it on the thread that runs the guest; the GSP's sink runs on the scheduler's.
    p.SetRefreshRate(120.0);
    f.runtime->Scheduler().RunPending();
    CHECK(p.RequestedRefreshRate() == 120.0);
    CHECK(state->VBlankPeriodTicks() == kPeriod120);
    CHECK(f.LoggedCount("vblank period 2234265 ticks") == 1);
    CHECK(state->NextVBlankTick() == first);  // the first VBlank keeps its deadline

    // From the first VBlank on, the cadence is exact in ticks.
    f.AdvanceTo(first + 1);
    CHECK(state->VBlankCount() == 1);
    f.AdvanceTo(first + 500 * kPeriod120 + 1);
    CHECK(state->VBlankCount() == 501);
    CHECK(state->NextVBlankTick() == first + 501 * kPeriod120);

    // 0 is the 3DS's own rate.
    p.SetRefreshRate(0.0);
    f.runtime->Scheduler().RunPending();
    CHECK(state->VBlankPeriodTicks() == kDefaultPeriod);
    CHECK(p.RequestedRefreshRate() == 0.0);
}

TEST_CASE("platform: a refresh rate set from guest code changes the period for the very next VBlank") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
    auto state = StateOf(f);
    platform::NativePlatform p(*f.runtime, "", {});
    const u64 first = state->NextVBlankTick();
    REQUIRE(first == f.runtime->Clock().Now() + kDefaultPeriod);

    u64 period_inside = 0;
    u64 count_inside = ~0ull;
    RunOnGuestThread(f, [&] {
        // The game clock reaches the first VBlank's deadline while the guest runs: it is due, but it
        // runs at the next safe point, after this call (the hooks run in the same place).
        f.time->Advance(static_cast<s64>(TicksToNsCeil(first - f.runtime->Clock().Now())));
        p.SetRefreshRate(120.0);
        // At once, as the reference's GPU::SetRefreshRate: not at a later safe point.
        period_inside = state->VBlankPeriodTicks();
        count_inside = state->VBlankCount();
    });
    CHECK(period_inside == kPeriod120);
    CHECK(count_inside == 0);
    // That VBlank, the very next one, was rescheduled with the new period.
    CHECK(state->VBlankCount() == 1);
    CHECK(state->NextVBlankTick() == first + kPeriod120);
    CHECK(f.LoggedCount("vblank period 2234265 ticks") == 1);
    f.AdvanceTo(first + 100 * kPeriod120 + 1);
    CHECK(state->VBlankCount() == 101);
    CHECK(state->NextVBlankTick() == first + 101 * kPeriod120);
}

TEST_CASE("platform: from another host thread the refresh rate still goes through the kernel's queue") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
    auto state = StateOf(f);
    platform::NativePlatform p(*f.runtime, "", {});
    std::thread([&] { p.SetRefreshRate(120.0); }).join();
    // Not on the scheduler thread: the guest state is untouched until the scheduler's next safe point.
    CHECK(state->VBlankPeriodTicks() == kDefaultPeriod);
    CHECK(p.RequestedRefreshRate() == 120.0);
    f.runtime->Scheduler().RunPending();
    CHECK(state->VBlankPeriodTicks() == kPeriod120);
    CHECK(f.LoggedCount("vblank period 2234265 ticks") == 1);
}

TEST_CASE("platform: a call from another thread that is still queued never overrides a newer one from guest code") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
    auto state = StateOf(f);
    platform::NativePlatform p(*f.runtime, "", {});
    const u64 period180 = 268111856 / 180;
    u64 period_queued = 0;
    RunOnGuestThread(f, [&] {
        // While the guest runs, another thread's call is queued for the next safe point...
        std::thread([&] { p.SetRefreshRate(120.0); }).join();
        period_queued = state->VBlankPeriodTicks();
        // ... and the guest's own, newer call is applied at once.
        p.SetRefreshRate(180.0);
    });
    CHECK(period_queued == kDefaultPeriod);
    // The queued call ran when the guest ended: it was out of date and did not override.
    CHECK(state->VBlankPeriodTicks() == period180);  // the queued 120 Hz was out of date
    CHECK(p.RequestedRefreshRate() == 180.0);
    CHECK(f.runtime->RefreshRate() == 180.0);
    CHECK(f.LoggedCount("vblank period 2234265 ticks") == 0);
    // A call from another thread after that one works as before.
    std::thread([&] { p.SetRefreshRate(120.0); }).join();
    f.runtime->Scheduler().RunPending();
    CHECK(state->VBlankPeriodTicks() == kPeriod120);
}

TEST_CASE("platform: the hooks' own refresh rate change, made on a guest thread, applies at once") {
    TempDir dir;
    dir.WriteText("mh4u.ini", "fps = 120\n");
    HookRun run({"--user", dir.Path()});
    auto state = StateOf(run.f);
    CHECK(state->VBlankPeriodTicks() == kDefaultPeriod);
    u64 period_inside = 0;
    RunOnGuestThread(run.f, [&] {
        run.hooks[kFrameHookAddress](&run.ctx, OriginalStub);  // sMhMain::frame, the hook that sets it
        period_inside = state->VBlankPeriodTicks();
    });
    CHECK(period_inside == kPeriod120);
    CHECK(run.platform->RequestedRefreshRate() == 120.0);
}

TEST_CASE("platform: the screen layout is stored for the renderer") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    platform::NativePlatform p(*f.runtime, "", {});
    CHECK(p.TopScreenRows() == 0);
    CHECK(p.TopScreenWidth() == 0);
    CHECK(p.LayoutRefreshes() == 0);
    CHECK(p.Overlay().height == 0.0f);

    p.SetTopScreenRows(480);
    p.SetTopScreenWidth(427);
    p.RefreshScreenLayout();
    p.RefreshScreenLayout();
    Port::ScreenOverlay overlay;
    overlay.height = 0.3f;
    overlay.opacity = 0.85f;
    overlay.corner = Port::ScreenOverlay::Corner::TopRight;
    overlay.views[1].height = 0.12f;
    overlay.views[1].x = 0.5f;
    p.SetScreenOverlay(overlay);
    CHECK(p.TopScreenRows() == 480);
    CHECK(p.TopScreenWidth() == 427);
    CHECK(p.LayoutRefreshes() == 2);
    const Port::ScreenOverlay stored = p.Overlay();
    CHECK(stored.height == 0.3f);
    CHECK(stored.opacity == 0.85f);
    CHECK(stored.corner == Port::ScreenOverlay::Corner::TopRight);
    CHECK(stored.views[1].height == 0.12f);
    CHECK(stored.views[1].x == 0.5f);
}

TEST_CASE("platform: aspect_ratio is the host option the hooks read and set") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    platform::NativePlatform p(*f.runtime, "", {{"aspect_ratio", "16:10"}, {"renderer", "vulkan"}});
    CHECK(p.GetOption("aspect_ratio") == "16:10");  // from --options
    CHECK(p.GetOption("renderer").empty());         // the hooks only know aspect_ratio
    CHECK(p.GetOption("nothing").empty());
    for (const char* ratio : {"stretch", "4:3", "16:9", "21:9", "default", "16:10"}) {
        CHECK(p.SetOption("aspect_ratio", ratio));
        CHECK(p.GetOption("aspect_ratio") == ratio);
    }
    CHECK_FALSE(p.SetOption("aspect_ratio", "5:4"));
    CHECK(p.GetOption("aspect_ratio") == "16:10");  // unchanged
    CHECK_FALSE(p.SetOption("renderer", "software"));
    CHECK(p.AppOptions().at("renderer") == "vulkan");

    platform::NativePlatform defaults(*f.runtime, "", {});
    CHECK(defaults.GetOption("aspect_ratio") == "default");
}

TEST_CASE("platform: network sync is local only and the end-skip vote is always agreed") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    platform::NativePlatform p(*f.runtime, "", {});
    std::vector<std::string> log;
    p.InstallNetSync(Port::NetSyncMode::Session, 0.0, log);  // the default: no line
    CHECK(log.empty());
    p.InstallNetSync(Port::NetSyncMode::Session, 200.0, log);  // the default slack: no line
    CHECK(log.empty());
    // The lines the Azahar build writes for the same options (the port's netsync.cpp).
    p.InstallNetSync(Port::NetSyncMode::Off, 0.0, log);
    p.InstallNetSync(Port::NetSyncMode::All, 350.0, log);
    CHECK(log == std::vector<std::string>{"net sync off", "net sync with every port console in the room",
                                          "net sync slack 350 ms"});

    CHECK(p.EndSkipAgreed());  // before any vote: no room, no peers
    p.VoteEndSkip(true);
    CHECK(p.EndSkipVote());
    CHECK(p.EndSkipAgreed());
    p.VoteEndSkip(false);
    CHECK_FALSE(p.EndSkipVote());
    CHECK(p.EndSkipAgreed());
}

TEST_CASE("platform: Log goes to the runtime's log, with the Azahar build's category") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    platform::NativePlatform p(*f.runtime, "", {});
    SetLogFilter(LogFilter(LogLevel::Debug));
    std::vector<std::pair<LogLevel, std::string>> seen;
    SetLogSink([&](const LogRecord& r) {
        if (r.category == "Frontend") {
            seen.emplace_back(r.level, std::string(r.message));
        }
    });
    p.Log(Port::LogLevel::Debug, "d");
    p.Log(Port::LogLevel::Info, "i");
    p.Log(Port::LogLevel::Warning, "w");
    p.Log(Port::LogLevel::Error, "e");
    REQUIRE(seen.size() == 4);
    CHECK(seen[0] == std::make_pair(LogLevel::Debug, std::string("d")));
    CHECK(seen[1] == std::make_pair(LogLevel::Info, std::string("i")));
    CHECK(seen[2] == std::make_pair(LogLevel::Warning, std::string("w")));
    CHECK(seen[3] == std::make_pair(LogLevel::Error, std::string("e")));
}

// ---- the boot step --------------------------------------------------------------------------------------

TEST_CASE("platform: the started step makes the platform from --user and --options") {
    Fixture f;
    TempDir dir;
    dir.WriteText("app.ini", "[Display]\naspect_ratio = 21:9\nrenderer = vulkan\n");
    std::shared_ptr<platform::NativePlatform> seen;
    bool running = false;
    f.runtime->SetMainThreadEntry([&](GuestThread&) {
        seen = platform::NativePlatform::Find(*f.runtime);
        running = seen && seen->IsRunning();
        f.runtime->Svc();  // the SVC layer exists
        RCtx ctx{};
        ctx.r[14] = kText + 0x20;
        f.runtime->Svc().CallSvc(ctx, 0x03, kText + 0x11);  // svcExitProcess
    });
    const int status = f.runtime->Boot(
        Parse({dir.WriteElf(), "--user", dir.Path(), "--options", dir.File("app.ini")}));
    CHECK(status == 0);
    REQUIRE(seen != nullptr);
    CHECK(running);
    // The hooks open <UserDir()>mh4u.ini: the platform hands them the folder with a separator.
    CHECK(seen->UserDir() == dir.Path() + "/");
    CHECK(seen->GetOption("aspect_ratio") == "21:9");
    CHECK(seen->AppOptions().at("renderer") == "vulkan");
}

TEST_CASE("platform: IsRunning follows the runtime") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    platform::NativePlatform p(*f.runtime, "", {});
    CHECK_FALSE(p.IsRunning());  // no main thread yet
}

TEST_CASE("platform: a bad --options or --set ends the boot with status 2") {
    TempDir dir;
    {
        Fixture f;
        f.runtime->SetMainThreadEntry([](GuestThread&) {});
        CHECK(f.runtime->Boot(Parse({dir.WriteElf(), "--options", dir.File("missing.ini")})) == 2);
        CHECK(f.Logged("cannot read"));
    }
    {
        Fixture f;
        dir.WriteText("bad.ini", "[Display]\nnonsense\n");
        f.runtime->SetMainThreadEntry([](GuestThread&) {});
        CHECK(f.runtime->Boot(Parse({dir.WriteElf(), "--options", dir.File("bad.ini")})) == 2);
        CHECK(f.Logged("line 2: expected key = value"));
    }
    {
        Fixture f;
        dir.WriteText("ratio.ini", "aspect_ratio = 5:4\n");
        f.runtime->SetMainThreadEntry([](GuestThread&) {});
        CHECK(f.runtime->Boot(Parse({dir.WriteElf(), "--options", dir.File("ratio.ini")})) == 2);
        CHECK(f.Logged("aspect_ratio 5:4 is not one of"));
    }
    {
        Fixture f;
        f.runtime->SetMainThreadEntry([](GuestThread&) {});
        CHECK(f.runtime->Boot(Parse({dir.WriteElf(), "--set", "soon:fps=60"})) == 2);
        CHECK(f.Logged("--set soon:fps=60"));
    }
    {
        // A key that cannot change while the game runs ends the run when its time comes.
        Fixture f;
        f.runtime->SetMainThreadEntry([&](GuestThread&) {
            f.time->Advance(5000000);
            RCtx ctx{};
            ctx.r[14] = kText + 0x20;
            f.runtime->Svc().CallSvc(ctx, 0x23, kText + 0x11);  // a safe point
            f.runtime->Svc().CallSvc(ctx, 0x03, kText + 0x11);
        });
        CHECK(f.runtime->Boot(Parse({dir.WriteElf(), "--set", "2:widescreen=1.7"})) == 2);
        CHECK(f.Logged("--set: widescreen is not an option that can change while the game runs"));
    }
}

// ---- --set on the game clock and the frame counter --------------------------------------------------

TEST_CASE("platform: --set changes apply at their tick and at their frame, in order") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    REQUIRE(IsSuccess(f.runtime->Memory().Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
    Schedule::Plan plan;
    std::string error;
    for (const char* arg : {"0:a=zero", "1500:b=ms", "f10:c=frame10", "1500.5:d=ms-half", "f3:e=frame3", "0:f=zero2"}) {
        REQUIRE_MESSAGE(Schedule::ParseOptionChange(arg, plan, error), error);
    }
    const u64 start = f.runtime->Clock().Now();
    std::vector<std::pair<std::string, u64>> applied;
    platform::ScheduleOptionChanges(*f.runtime, plan.option_changes, [&](const std::string& key, const std::string&) {
        applied.emplace_back(key, f.runtime->Clock().Now() - start);
        return true;
    });
    // Time 0: before anything runs, in the order given.
    REQUIRE(applied.size() == 2);
    CHECK(applied[0].first == "a");
    CHECK(applied[1].first == "f");

    const u64 ticks_1500 = static_cast<u64>(std::ceil(1500.0 * (268111856.0 / 1000.0)));
    f.AdvanceTo(start + ticks_1500 - 1);
    CHECK(applied.size() == 2);  // one tick early
    f.AdvanceTo(start + ticks_1500);
    REQUIRE(applied.size() == 3);
    CHECK(applied[2].first == "b");
    CHECK(applied[2].second == ticks_1500);  // exact in ticks

    const u64 ticks_1500_5 = static_cast<u64>(std::ceil(1500.5 * (268111856.0 / 1000.0)));
    f.AdvanceTo(start + ticks_1500_5 + 1000);
    REQUIRE(applied.size() == 4);
    CHECK(applied[3].first == "d");

    // Frames follow the game's counter, not the time: nothing yet at frame 2.
    REQUIRE(f.runtime->Memory().Write<u32>(kFrameCounterAddress, 2));
    f.runtime->PollFrame();
    CHECK(applied.size() == 4);
    REQUIRE(f.runtime->Memory().Write<u32>(kFrameCounterAddress, 3));
    f.runtime->PollFrame();
    REQUIRE(applied.size() == 5);
    CHECK(applied[4].first == "e");
    // A counter that jumps past a frame applies it too, once.
    REQUIRE(f.runtime->Memory().Write<u32>(kFrameCounterAddress, 40));
    f.runtime->PollFrame();
    REQUIRE(applied.size() == 6);
    CHECK(applied[5].first == "c");
    REQUIRE(f.runtime->Memory().Write<u32>(kFrameCounterAddress, 41));
    f.runtime->PollFrame();
    f.AdvanceTo(start + 3 * ticks_1500);
    CHECK(applied.size() == 6);  // each only once
}

// ---- the hooks on the native platform -------------------------------------------------------------------

TEST_CASE("platform: without options the hooks leave the refresh rate alone") {
    TempDir dir;
    HookRun run({"--user", dir.Path()});
    auto state = StateOf(run.f);
    run.Frame();
    run.Frame();
    CHECK(state->VBlankPeriodTicks() == kDefaultPeriod);
    CHECK(run.platform->RequestedRefreshRate() == 0.0);
    CHECK(run.f.LoggedCount("vblank period") == 0);
    // The overlay is the hooks' default (touch_overlay "off" height 0) and nothing asks for a layout yet.
    CHECK(run.platform->Overlay().height == 0.0f);
}

TEST_CASE("platform: fps=120 from mh4u.ini in --user reaches the VBlank period at the first frame") {
    TempDir dir;
    dir.WriteText("mh4u.ini", "# the port's options\nfps = 120\n");
    HookRun run({"--user", dir.Path()});
    auto state = StateOf(run.f);
    // Reported as the Azahar build reports it (ARM_Recomp::LoadCode logs "hooks: <line>").
    CHECK(run.f.Logged("hooks: options from " + dir.Path() + "/mh4u.ini"));
    CHECK(run.f.Logged("hooks: frame-rate cap 120"));
    CHECK(run.f.Logged("hooks: emulated screen refresh rate 120 Hz, as fps (experimental)"));
    CHECK(state->VBlankPeriodTicks() == kDefaultPeriod);  // not before the game's first frame
    const u64 first = state->NextVBlankTick();
    run.Frame();
    CHECK(run.platform->RequestedRefreshRate() == 120.0);
    CHECK(state->VBlankPeriodTicks() == kPeriod120);
    CHECK(run.f.Logged("vblank period 2234265 ticks"));
    // Exact in ticks from the first VBlank on.
    run.f.AdvanceTo(first + 100 * kPeriod120 + 1);
    CHECK(state->VBlankCount() == 101);
    CHECK(state->NextVBlankTick() == first + 101 * kPeriod120);
}

TEST_CASE("platform: --set 0:fps=120 is the same as fps = 120 in mh4u.ini") {
    TempDir dir;
    HookRun run({"--user", dir.Path(), "--set", "0:fps=120"});
    auto state = StateOf(run.f);
    CHECK(run.f.Logged("set fps = 120 at 0 ms"));
    run.Frame();
    CHECK(state->VBlankPeriodTicks() == kPeriod120);
    CHECK(run.f.LoggedCount("vblank period 2234265 ticks") == 1);
}

TEST_CASE("platform: --set fN:fps=60 puts the refresh rate back once the game reaches frame N") {
    TempDir dir;
    dir.WriteText("mh4u.ini", "fps = 120\n");
    HookRun run({"--user", dir.Path(), "--set", "f2:fps=60"});
    auto state = StateOf(run.f);
    run.Frame();  // the hooks' first frame: 120 Hz
    CHECK(state->VBlankPeriodTicks() == kPeriod120);
    REQUIRE(run.f.runtime->Memory().Write<u32>(kFrameCounterAddress, 1));
    run.f.runtime->PollFrame();
    CHECK(state->VBlankPeriodTicks() == kPeriod120);  // frame 1: not yet
    REQUIRE(run.f.runtime->Memory().Write<u32>(kFrameCounterAddress, 2));
    run.f.runtime->PollFrame();  // fps = 60 asks for the 3DS's own rate (60 is not above 60)
    run.f.runtime->Scheduler().RunPending();
    CHECK(run.f.Logged("set fps = 60 at"));
    CHECK(state->VBlankPeriodTicks() == kDefaultPeriod);
    CHECK(run.platform->RequestedRefreshRate() == 0.0);
}

TEST_CASE("platform: without --user a stray ./mh4u.ini in the current directory is ignored") {
    TempDir cwd;
    cwd.WriteText("mh4u.ini", "fps = 120\nwidescreen = 1\nskip = ZR\n");
    {
        CurrentDirGuard guard(cwd.Path());  // put back when the scope ends, also on a failed check
        HookRun run({});                    // no --user
        // A path that holds no file, the same on every platform, and logged once.
        CHECK(run.platform->UserDir() == platform::kNoUserDir);
        CHECK(run.f.LoggedCount("no --user folder") == 1);
        // The hooks never read the file in the current directory.
        for (const std::string& line : run.f.logged) {
            CHECK(line.find("options from") == std::string::npos);
            CHECK(line.find("frame-rate cap") == std::string::npos);
        }
        CHECK(std::filesystem::exists("mh4u.ini"));  // it is there, in the directory the process is in
        auto state = StateOf(run.f);
        run.Frame();
        run.Frame();
        CHECK(state->VBlankPeriodTicks() == kDefaultPeriod);
        CHECK(run.platform->RequestedRefreshRate() == 0.0);
        CHECK(run.f.LoggedCount("vblank period") == 0);
    }
    // The same file is read when it is in the --user folder: the check above is not vacuous.
    {
        CurrentDirGuard guard(cwd.Path());
        HookRun run({"--user", cwd.Path()});
        CHECK(run.platform->UserDir() == cwd.Path() + "/");
        CHECK(run.f.LoggedCount("no --user folder") == 0);
        CHECK(run.f.Logged("hooks: options from " + cwd.Path() + "/mh4u.ini"));
        CHECK(run.f.Logged("hooks: frame-rate cap 120"));
    }
}

TEST_CASE("platform: an empty user folder given to the platform directly is the placeholder too") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    platform::NativePlatform none(*f.runtime, "", {});
    CHECK(none.UserDir() == platform::kNoUserDir);
    // The placeholder is not a path the hooks can open a file in: it has characters no Windows file
    // name may hold, and it is not absolute.
    const std::string placeholder = platform::kNoUserDir;
    CHECK(placeholder.find_first_of("<>") != std::string::npos);
    CHECK(placeholder.back() == '/');
    CHECK_FALSE(std::filesystem::exists(placeholder + "mh4u.ini"));
    platform::NativePlatform given(*f.runtime, "/some/folder/", {});
    CHECK(given.UserDir() == "/some/folder/");  // --user is unchanged
}

// ---- D45: UTF-8 paths ---------------------------------------------------------------------------------------

namespace {

// "José-ñ-日本" as UTF-8 bytes: accents and characters outside every ANSI code page.
const std::string kUnicodeName = std::string("Jos") + "\xC3\xA9" + "-" + "\xC3\xB1" + "-" + "\xE6\x97\xA5" + "\xE6\x9C\xAC";

// The UTF-8 text of a folder or file named `name` under the temporary folder `dir`.
std::string Under(const TempDir& dir, const std::string& name) {
    return utf8::PathToUtf8(std::filesystem::path(dir.Path()) / utf8::PathFromUtf8(name));
}

// A short-name call that answers from a table and counts how often it was asked.
struct FakeShortNames {
    std::map<std::string, std::string> table;  // folder (no trailing separator) -> short name
    std::vector<std::string> asked;
    platform::ShortPathFunction Function() {
        return [this](const std::string& folder) -> std::optional<std::string> {
            asked.push_back(folder);
            const auto it = table.find(folder);
            if (it == table.end()) {
                return std::nullopt;
            }
            return it->second;
        };
    }
};

}  // namespace

TEST_CASE("D45: UserDir keeps an ASCII folder as it is and never asks for a short name") {
    FakeShortNames fake;
    fake.table["C:\\Users\\Hunter\\mh4u"] = "SHOULD-NOT-BE-USED";
    CHECK(platform::ResolveUserDir("C:\\Users\\Hunter\\mh4u\\", fake.Function()) == "C:\\Users\\Hunter\\mh4u\\");
    CHECK(platform::ResolveUserDir("/home/hunter/mh4u/", fake.Function()) == "/home/hunter/mh4u/");
    CHECK(platform::ResolveUserDir("relative", fake.Function()) == "relative");
    CHECK(platform::ResolveUserDir("", fake.Function()) == "");
    CHECK(fake.asked.empty());
    // an ASCII folder on a system without a short-name call is the same
    CHECK(platform::ResolveUserDir("/home/hunter/mh4u/", {}) == "/home/hunter/mh4u/");
}

TEST_CASE("D45: UserDir of a folder with non-ASCII characters is its short name when there is one") {
    FakeShortNames fake;
    const std::string folder = "C:\\Users\\" + kUnicodeName;
    fake.table[folder] = "C:\\Users\\JOS~1";
    std::string warning;
    // the call gets the folder without its trailing separator, and the answer gets the separator back
    CHECK(platform::ResolveUserDir(folder + "\\", fake.Function(), &warning) == "C:\\Users\\JOS~1\\");
    CHECK(fake.asked == std::vector<std::string>{folder});
    CHECK(warning.empty());
    // the separator of the folder is the one kept
    fake.table["/home/" + kUnicodeName] = "/home/JOS~1";
    CHECK(platform::ResolveUserDir("/home/" + kUnicodeName + "/", fake.Function(), &warning) == "/home/JOS~1/");
    // no trailing separator at all (a caller that passes the bare folder): the hooks still get one
    CHECK(platform::ResolveUserDir("/home/" + kUnicodeName, fake.Function(), &warning) == "/home/JOS~1/");
    // a short name that ends with a separator does not double it
    fake.table["/home/" + kUnicodeName] = "/home/JOS~1/";
    CHECK(platform::ResolveUserDir("/home/" + kUnicodeName + "/", fake.Function(), &warning) == "/home/JOS~1/");
    CHECK(warning.empty());
    // the result is ASCII by construction: every byte is below 0x80
    for (const char c : platform::ResolveUserDir(folder + "\\", fake.Function())) {
        CHECK(static_cast<unsigned char>(c) < 0x80);
    }
    // a system whose ANSI paths are UTF-8 (no short-name call) gets the folder unchanged
    CHECK(platform::ResolveUserDir("/home/" + kUnicodeName + "/", {}) == "/home/" + kUnicodeName + "/");
}

TEST_CASE("D45: UserDir of a folder with non-ASCII characters and no short name is the placeholder, with a reason") {
    const std::string folder = "D:\\Juegos\\" + kUnicodeName;
    // nothing for it (8.3 names are off for the volume, or the folder is missing)
    {
        FakeShortNames fake;
        std::string warning;
        CHECK(platform::ResolveUserDir(folder + "\\", fake.Function(), &warning) == platform::kNoUserDir);
        CHECK(fake.asked == std::vector<std::string>{folder});
        CHECK(warning.find(folder) != std::string::npos);  // names the folder
        CHECK(warning.find("mh4u.ini") != std::string::npos);
        CHECK(warning.find("8.3") != std::string::npos);
        CHECK(warning.find("fsutil 8dot3name") != std::string::npos);
    }
    // an empty answer is no answer
    {
        FakeShortNames fake;
        fake.table[folder] = "";
        std::string warning;
        CHECK(platform::ResolveUserDir(folder + "\\", fake.Function(), &warning) == platform::kNoUserDir);
        CHECK_FALSE(warning.empty());
    }
    // a "short name" that still has a non-ASCII character (Windows gave the long name back for a part that has no
    // short one) is no use to the hooks either
    {
        FakeShortNames fake;
        fake.table[folder] = "D:\\JUEGOS~1\\" + kUnicodeName;
        std::string warning;
        CHECK(platform::ResolveUserDir(folder + "\\", fake.Function(), &warning) == platform::kNoUserDir);
        CHECK_FALSE(warning.empty());
    }
    // the warning is optional
    {
        FakeShortNames fake;
        CHECK(platform::ResolveUserDir(folder + "\\", fake.Function()) == platform::kNoUserDir);
    }
}

TEST_CASE("D45: the platform logs one Warning when a folder with non-ASCII characters has no short name") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {}));
    std::vector<std::pair<LogLevel, std::string>> records;
    SetLogSink([&](const LogRecord& r) { records.emplace_back(r.level, std::string(r.message)); });
    const std::string folder = "D:\\Juegos\\" + kUnicodeName;
    {
        FakeShortNames none;
        platform::NativePlatform p(*f.runtime, folder + "\\", {}, none.Function());
        CHECK(p.UserDir() == platform::kNoUserDir);
    }
    int warnings = 0;
    for (const auto& [level, text] : records) {
        if (level == LogLevel::Warning && text.find("mh4u.ini") != std::string::npos &&
            text.find(folder) != std::string::npos) {
            ++warnings;
        }
        CHECK(level != LogLevel::Error);
    }
    CHECK(warnings == 1);
    // with a short name there is no warning, and UserDir is the short name
    records.clear();
    {
        FakeShortNames fake;
        fake.table[folder] = "D:\\JUEGOS~1\\JOS~1";
        platform::NativePlatform p(*f.runtime, folder + "\\", {}, fake.Function());
        CHECK(p.UserDir() == "D:\\JUEGOS~1\\JOS~1\\");
    }
    for (const auto& [level, text] : records) {
        CHECK(level != LogLevel::Warning);
    }
    // an ASCII folder: not asked, no warning
    records.clear();
    {
        FakeShortNames fake;
        platform::NativePlatform p(*f.runtime, "D:\\Games\\mh4u\\", {}, fake.Function());
        CHECK(p.UserDir() == "D:\\Games\\mh4u\\");
        CHECK(fake.asked.empty());
    }
    CHECK(records.empty());
}

TEST_CASE("D45: the hooks read mh4u.ini from the short name that stands for a folder with non-ASCII characters") {
    TempDir dir;
    // the folder the player named, and the ASCII folder its short name stands for (the file is in the latter:
    // the hooks open it through an ANSI std::string path and could not reach the former on Windows)
    const std::string accented = Under(dir, kUnicodeName);
    const std::string short_name = Under(dir, "SHORT83");
    std::filesystem::create_directories(utf8::PathFromUtf8(accented));
    std::filesystem::create_directories(utf8::PathFromUtf8(short_name));
    std::ofstream(utf8::PathFromUtf8(short_name + "/mh4u.ini")) << "fps = 120\n";
    FakeShortNames fake;
    fake.table[accented] = short_name;

    RCodeInfo info{};
    info.hook_count = 1;
    info.set_hook = FakeSetHook;
    {
        Fixture f;
        std::map<u32, RHookFn> hooks;
        g_hooks = &hooks;
        REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
        {
            platform::NativePlatform p(*f.runtime, accented + "/", {}, fake.Function());
            CHECK(p.UserDir() == short_name + "/");
            Port::Hooks::SetPlatform(&p);
            platform::InstallHooks(info);
            CHECK(f.Logged("hooks: options from " + short_name + "/mh4u.ini"));
            CHECK(f.Logged("hooks: frame-rate cap 120"));
        }
        g_hooks = nullptr;
    }
    // and without a short name the hooks find nothing, and read no file from anywhere else
    {
        Fixture f;
        std::map<u32, RHookFn> hooks;
        g_hooks = &hooks;
        REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
        {
            FakeShortNames none;
            platform::NativePlatform p(*f.runtime, accented + "/", {}, none.Function());
            CHECK(p.UserDir() == platform::kNoUserDir);
            Port::Hooks::SetPlatform(&p);
            platform::InstallHooks(info);
            for (const std::string& line : f.logged) {
                CHECK(line.find("options from") == std::string::npos);
            }
        }
        g_hooks = nullptr;
    }
}

TEST_CASE("D45: --options is read from a folder whose name has non-ASCII characters") {
    TempDir dir;
    const std::string folder = Under(dir, kUnicodeName);
    std::filesystem::create_directories(utf8::PathFromUtf8(folder));
    const std::string ini = folder + std::string(1, static_cast<char>(std::filesystem::path::preferred_separator)) + "app.ini";
    {
        std::ofstream out(utf8::PathFromUtf8(ini), std::ios::binary);
        out << "[Display]\naspect_ratio = 21:9\nrenderer = vulkan\n";
    }
    {
        Fixture f;
        std::shared_ptr<platform::NativePlatform> seen;
        f.runtime->SetMainThreadEntry([&](GuestThread&) {
            seen = platform::NativePlatform::Find(*f.runtime);
            RCtx ctx{};
            ctx.r[14] = kText + 0x20;
            f.runtime->Svc().CallSvc(ctx, 0x03, kText + 0x11);  // svcExitProcess
        });
        CHECK(f.runtime->Boot(Parse({dir.WriteElf(), "--options", ini})) == 0);
        REQUIRE(seen != nullptr);
        CHECK(seen->GetOption("aspect_ratio") == "21:9");
        CHECK(seen->AppOptions().at("renderer") == "vulkan");
    }
    // a file that is missing there is named as the exact text the player gave
    {
        Fixture f;
        f.runtime->SetMainThreadEntry([](GuestThread&) {});
        const std::string missing = folder + std::string(1, static_cast<char>(std::filesystem::path::preferred_separator)) + "nada.ini";
        CHECK(f.runtime->Boot(Parse({dir.WriteElf(), "--options", missing})) == 2);
        CHECK(f.Logged("--options: cannot read " + missing));
    }
    // a bad one names it too, in the error of the line that is wrong
    {
        Fixture f;
        {
            std::ofstream out(utf8::PathFromUtf8(ini), std::ios::binary | std::ios::trunc);
            out << "[Display]\nnonsense\n";
        }
        f.runtime->SetMainThreadEntry([](GuestThread&) {});
        CHECK(f.runtime->Boot(Parse({dir.WriteElf(), "--options", ini})) == 2);
        CHECK(f.Logged("--options " + ini + ": line 2: expected key = value"));
    }
}

TEST_CASE("platform: the hooks take the touch overlay and the top screen through the platform") {
    TempDir dir;
    dir.WriteText("mh4u.ini", "touch_overlay = bottom-left 25 70\n");
    HookRun run({"--user", dir.Path()});
    const Port::ScreenOverlay overlay = run.platform->Overlay();
    CHECK(overlay.height == doctest::Approx(0.25f));
    CHECK(overlay.opacity == doctest::Approx(0.70f));
    CHECK(overlay.corner == Port::ScreenOverlay::Corner::BottomLeft);
}

#endif  // recomp.h and hooks.h around native/
