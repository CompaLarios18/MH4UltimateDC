// Derived from Azahar src/core/memory.h, src/core/memory.cpp (the physical memory areas and
// IsValidPhysicalAddress), src/core/hle/kernel/svc.cpp and src/core/hle/kernel/svc_wrapper.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp says "GPLv3 or any later
// version"; memory.h, memory.cpp and svc_wrapper.h say "GPLv2 or any later version"): the expected
// answers at the edges of the physical areas (copied from the GX engine test's "bounds: Window"
// case, which takes them from IsValidPhysicalAddress), and the numbers and register layouts of the calls the
// tests make (ExitProcess 0x03, SleepThread 0x0A with its 64-bit argument in r0 and r1, CloseHandle
// 0x23, Break 0x3C, OutputDebugString 0x3D with the address and length in r0 and r1).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h, memory.cpp, svc.cpp,
//   svc_wrapper.h)
//
// mh4n_runtime (N1-09): the boot order, the frame watcher, the host interfaces, the GPU host and
// the runner's own traces. Programs here are ELF files written by the test, whose main thread runs
// a C++ function (Runtime::SetMainThreadEntry), so no code library is needed. Time is a
// ManualTimeSource, so nothing waits on a clock and nothing sleeps as synchronisation.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("runtime_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "gpu/gx/blitter.h"
#include "gpu_capture/capture_host.h"
#include "mh4n/clock.h"
#include "mh4n/gpu.h"
#include "mh4n/host.h"
#include "mh4n/loader.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "../src/runtime/timer_resolution.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr u32 kText = 0x00100000;
constexpr u32 kFramePage = 0x01057000;  // holds kFrameCounterAddress

// ---- boot steps registered from the test ---------------------------------------------------------

// What a recorded step saw. Only recorded while a test has installed a recorder, so the steps do
// nothing for the other tests of this binary.
struct StepRecord {
    std::string name;
    Phase phase;
    bool has_memory;
    bool has_main_thread;
};
std::vector<StepRecord>* g_recorder = nullptr;

void Record(const char* name, Phase phase, Runtime& rt) {
    if (g_recorder != nullptr) {
        g_recorder->push_back({name, phase, rt.GetKernel().CurrentProcess() != nullptr,
                               rt.MainThread() != nullptr});
    }
}

#define TEST_STEP(NAME, PHASE)                                                                     \
    MH4N_REGISTER(BootStep, NAME, (BootStep{PHASE, [](Runtime& rt) { Record(NAME, PHASE, rt); }}))

TEST_STEP("test.d2", Phase::Devices);
TEST_STEP("test.d1", Phase::Devices);
TEST_STEP("test.s1", Phase::Services);
TEST_STEP("test.t2", Phase::Tracing);
TEST_STEP("test.t1", Phase::Tracing);
TEST_STEP("test.x1", Phase::Started);

// A step that asks for the run to end, when a test says so.
std::optional<int> g_request_exit;
MH4N_REGISTER(BootStep, "test.y-exit", (BootStep{Phase::Services, [](Runtime& rt) {
                                           if (g_request_exit) {
                                               rt.RequestExit(*g_request_exit, "asked by the test");
                                           }
                                       }}));

// Devices made by factories the test registers.
struct TestGpu final : gpu::Gpu {
    void Execute(const gpu::GxCommand&) override {}
    void WriteReg(u32, u32) override {}
    u32 ReadReg(u32) override { return 0; }
    void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}
};
struct TestInput final : InputSource {
    InputState Sample(u64 tick) override {
        InputState state;
        state.buttons = kPadA;
        state.circle_x = static_cast<s16>(tick);
        return state;
    }
};
struct TestAudio final : AudioSink {
    void Queue(const s16*, std::size_t) override {}
};
gpu::Host* g_gpu_host_seen = nullptr;
Runtime* g_input_runtime_seen = nullptr;
MH4N_REGISTER(gpu::GpuFactory, "test-gpu", [](gpu::Host& host, const Options&) -> std::unique_ptr<gpu::Gpu> {
    g_gpu_host_seen = &host;
    return std::make_unique<TestGpu>();
});
MH4N_REGISTER(InputSourceFactory, "test-input", [](Runtime& rt) -> std::unique_ptr<InputSource> {
    g_input_runtime_seen = &rt;
    return std::make_unique<TestInput>();
});
MH4N_REGISTER(AudioSinkFactory, "test-audio",
              [](Runtime&) -> std::unique_ptr<AudioSink> { return std::make_unique<TestAudio>(); });

// ---- helpers ------------------------------------------------------------------------------------

void Put16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v), b[at + 1] = static_cast<u8>(v >> 8);
}
void Put32(std::vector<u8>& b, std::size_t at, u32 v) {
    Put16(b, at, v & 0xFFFF), Put16(b, at + 2, v >> 16);
}

// An ARM ELF with one read-execute segment at kText (Azahar's ELF defaults apply).
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
                ("mh4n_runtime_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
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

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}

// A runtime on manual time (`time` moves it), with the log captured.
RuntimeConfig ManualConfig(ManualTimeSource** time) {
    auto source = std::make_unique<ManualTimeSource>();
    *time = source.get();
    RuntimeConfig config;
    config.time = std::move(source);
    return config;
}

struct Fixture {
    ManualTimeSource* time = nullptr;
    Runtime runtime;
    std::vector<std::string> logged;
    Fixture() : runtime(ManualConfig(&time)) {
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { logged.push_back(std::string(r.message)); });
    }
    ~Fixture() {
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    bool Logged(const std::string& needle) const {
        for (const std::string& line : logged) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

// One SVC from the running guest thread, with a fake register file (as svc_test does).
struct Regs {
    std::array<u32, 16> r{};
};
Regs CallSvc(Runtime& rt, u32 number, std::initializer_list<u32> in) {
    RCtx ctx{};
    int i = 0;
    for (const u32 value : in) {
        ctx.r[i++] = value;
    }
    ctx.r[14] = kText + 0x20;
    rt.Svc().CallSvc(ctx, number, kText + 0x11);
    Regs out;
    for (int k = 0; k < 16; ++k) {
        out.r[static_cast<std::size_t>(k)] = ctx.r[k];
    }
    return out;
}
void ExitProcess(Runtime& rt) {
    CallSvc(rt, 0x03, {});
}
void SleepNs(Runtime& rt, u64 ns) {
    CallSvc(rt, 0x0A, {static_cast<u32>(ns), static_cast<u32>(ns >> 32)});
}

// Boots the ELF whose main thread runs `body` (which must end the process or the thread).
int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra,
             std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf()};
    args.insert(args.end(), extra.begin(), extra.end());
    f.runtime.SetMainThreadEntry([body = std::move(body)](GuestThread&) { body(); });
    return f.runtime.Boot(Parse(args));
}

}  // namespace

// ---- boot steps ----------------------------------------------------------------------------------

TEST_CASE("Boot runs the steps of each phase in phase order, then by name, around the main thread") {
    Fixture f;
    TempDir dir;
    std::vector<StepRecord> records;
    g_recorder = &records;
    std::vector<std::string> events;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        // the main thread runs after every step, the `started` ones included
        for (const StepRecord& r : records) {
            events.push_back(r.name);
        }
        events.push_back("main");
        ExitProcess(f.runtime);
    });
    g_recorder = nullptr;
    CHECK(status == 0);
    CHECK(events == std::vector<std::string>{"test.d1", "test.d2", "test.s1", "test.t1", "test.t2",
                                             "test.x1", "main"});
    REQUIRE(records.size() == 6);
    for (const StepRecord& r : records) {
        INFO(r.name);
        CHECK(r.has_memory);  // the process exists for every phase
        CHECK(r.has_main_thread == (r.phase == Phase::Started));  // only `started` sees the thread
    }
    CHECK(records[0].phase == Phase::Devices);
    CHECK(records[2].phase == Phase::Services);
    CHECK(records[3].phase == Phase::Tracing);
    CHECK(records[5].phase == Phase::Started);
}

TEST_CASE("Prepare with two phases and no program runs only those steps") {
    Fixture f;
    std::vector<StepRecord> records;
    g_recorder = &records;
    std::string error;
    // given out of order, with a phase repeated: phase order and name order rule
    REQUIRE(f.runtime.Prepare(Parse({}), {Phase::Tracing, Phase::Devices, Phase::Tracing}, &error));
    g_recorder = nullptr;
    CHECK(error.empty());
    REQUIRE(records.size() == 4);
    CHECK(records[0].name == "test.d1");
    CHECK(records[1].name == "test.d2");
    CHECK(records[2].name == "test.t1");
    CHECK(records[3].name == "test.t2");
    // memory, the kernel and a process; no program, no code library, no main thread
    CHECK(f.runtime.Memory().Base() != nullptr);
    CHECK(f.runtime.GetKernel().CurrentProcess() != nullptr);
    CHECK(f.runtime.GetLoader() == nullptr);
    CHECK(f.runtime.Image() == nullptr);
    CHECK(f.runtime.Pages() == nullptr);
    CHECK(f.runtime.MainThread() == nullptr);
    CHECK(f.runtime.GetCodeHost().Library() == nullptr);
    CHECK(f.runtime.GetKernel().Regions().MemoryMode() == kPrepareMemoryMode);
    // the runtime's own devices step ran: the default devices exist, each registered by its own module
    CHECK(f.runtime.Interfaces().gpu != nullptr);    // N1-17 registers the `null` GPU
    CHECK(f.runtime.Interfaces().input != nullptr);  // N1-16 registers `script`
    CHECK(f.runtime.Interfaces().audio != nullptr);  // N1-18 registers the `null` sink
}

TEST_CASE("Prepare refuses a memory mode that does not exist and bad option values") {
    Fixture f;
    std::string error;
    CHECK_FALSE(f.runtime.Prepare(Parse({}), {}, &error, /*memory_mode=*/1));
    CHECK(error.find("memory mode") != std::string::npos);
    Fixture g;
    CHECK_FALSE(g.runtime.Prepare(Parse({"--slice-us", "abc"}), {}, &error));
    CHECK_FALSE(error.empty());
}

// ---- the command line golden.py gives a candidate runner (N1-09b) --------------------------------------

// tools/native/golden.py check --candidate-runner starts the runner with only the options both runners
// share (build_args with full=False). Every module that serves one of them registers it in the scope
// "runner" (this binary links them all), with the reference's syntax: a value after the name, except
// --trace-ipc-full, which is a flag. (--log is the program's own, native/runner/main.cpp.)
#if MH4N_HAVE_SCHEDULE  // --set and --input-file need port/src/schedule.cpp (a copy of native/ alone has neither)
TEST_CASE("the options golden.py passes a candidate runner are all registered, with the reference's syntax") {
    const Options options = Parse({"build/native-game", "--code", "lib.so", "--user", "work", "--frames", "9000",
                                   "--input-file", "session.txt", "--set", "0:fps=60", "--set", "f600:fps=30",
                                   "--trace-svc", "o/svc.txt", "--trace-ipc", "o/ipc.txt", "--trace-ipc-full",
                                   "--state-trace", "o/state.txt", "--capture-hid", "o/hid.mhid",
                                   "--capture-dsp", "300:2:o/dsp-300.mdsp", "--observable", "o.txt",
                                   "--ms", "f100", "--init-time", "1767268800"});
    REQUIRE(options.positional().size() == 1);
    CHECK(options.positional()[0] == "build/native-game");
    CHECK(options.GetString("--code") == "lib.so");
    CHECK(options.GetString("--user") == "work");
    CHECK(options.GetString("--frames") == "9000");
    CHECK(options.GetString("--input-file") == "session.txt");
    CHECK(options.All("--set") == std::vector<std::string>{"0:fps=60", "f600:fps=30"});
    CHECK(options.GetString("--trace-svc") == "o/svc.txt");
    CHECK(options.GetString("--trace-ipc") == "o/ipc.txt");
    CHECK(options.Has("--trace-ipc-full"));
    CHECK(options.GetString("--state-trace") == "o/state.txt");
    CHECK(options.GetString("--capture-hid") == "o/hid.mhid");
    CHECK(options.GetString("--capture-dsp") == "300:2:o/dsp-300.mdsp");
    CHECK(options.GetString("--observable") == "o.txt");
    // the flag takes no value: "--trace-ipc-full=1" is refused, as is a value option without its value
    CHECK_FALSE(ParseOptions("runner", std::vector<std::string>{"--trace-ipc-full=1"}).ok());
    CHECK_FALSE(ParseOptions("runner", std::vector<std::string>{"--frames"}).ok());
}
#endif  // MH4N_HAVE_SCHEDULE

// ---- the devices step ---------------------------------------------------------------------------

TEST_CASE("the devices step makes the GPU, the input source and the audio sink the options name") {
    Fixture f;
    g_gpu_host_seen = nullptr;
    g_input_runtime_seen = nullptr;
    REQUIRE(f.runtime.Prepare(
        Parse({"--gpu", "test-gpu", "--input-source", "test-input", "--audio", "test-audio"}),
        {Phase::Devices}));
    const HostInterfaces& hi = f.runtime.Interfaces();
    REQUIRE(hi.gpu != nullptr);
    REQUIRE(hi.input != nullptr);
    REQUIRE(hi.audio != nullptr);
    CHECK(g_gpu_host_seen == &f.runtime.GpuHost());  // the GPU's Host is the runtime's
    CHECK(g_input_runtime_seen == &f.runtime);
    InputState state = hi.input->Sample(77);
    CHECK(state.buttons == kPadA);
    CHECK(state.circle_x == 77);
}

TEST_CASE("a device name nothing has registered leaves that interface empty, the others stay") {
    Fixture f;
    REQUIRE(f.runtime.Prepare(Parse({"--gpu", "nope", "--input-source", "nope", "--audio", "test-audio"}),
                              {Phase::Devices}));
    CHECK(f.runtime.Interfaces().gpu == nullptr);
    CHECK(f.runtime.Interfaces().input == nullptr);
    CHECK(f.runtime.Interfaces().audio != nullptr);
    CHECK(f.Logged("no GPU named 'nope'"));
}

// ---- the frame watcher ---------------------------------------------------------------------------

TEST_CASE("PollFrame calls the OnFrame subscribers as the frame counter changes") {
    Fixture f;
    REQUIRE(f.runtime.Prepare(Parse({}), {}));
    GuestMemory& memory = f.runtime.Memory();
    std::vector<std::pair<u32, u64>> seen, seen2;
    f.runtime.OnFrame([&](u32 frame, u64 tick) { seen.emplace_back(frame, tick); });
    f.runtime.OnFrame([&](u32 frame, u64) { seen2.emplace_back(frame, 0); });

    f.runtime.PollFrame();  // the counter's page is not mapped: nothing to read
    CHECK(seen.empty());

    REQUIRE(IsSuccess(memory.Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
    f.runtime.PollFrame();  // the first reading counts as a change, even at 0
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].first == 0);
    f.runtime.PollFrame();  // unchanged
    CHECK(seen.size() == 1);

    REQUIRE(memory.Write<u32>(kFrameCounterAddress, 1));
    f.time->Advance(1000000);  // 1 ms
    f.runtime.PollFrame();
    REQUIRE(seen.size() == 2);
    CHECK(seen[1].first == 1);
    CHECK(seen[1].second == NsToTicks(1000000));  // the game clock's tick of the reading
    f.runtime.PollFrame();
    CHECK(seen.size() == 2);

    // a jump of several frames is one call; a counter that goes back is a change too
    REQUIRE(memory.Write<u32>(kFrameCounterAddress, 9));
    f.runtime.PollFrame();
    REQUIRE(memory.Write<u32>(kFrameCounterAddress, 3));
    f.runtime.PollFrame();
    CHECK(seen.size() == 4);
    CHECK(seen[2].first == 9);
    CHECK(seen[3].first == 3);
    CHECK(seen2.size() == 4);  // every subscriber, in order
}

TEST_CASE("the frame watcher runs at the scheduler's safe points and --frames stops the run") {
    Fixture f;
    TempDir dir;
    std::vector<u32> frames;
    f.runtime.OnFrame([&](u32 frame, u64) { frames.push_back(frame); });
    u32 reached = 0;
    const int status = BootWith(f, dir, {"--frames", "5"}, [&] {
        CHECK(IsSuccess(f.runtime.Memory().Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        for (u32 frame = 1; frame < 100; ++frame) {
            f.runtime.Memory().Write<u32>(kFrameCounterAddress, frame);
            // every SVC is a safe point: the watcher reads the counter there
            CallSvc(f.runtime, 0x23, {0});
            if (frame == 2) {
                f.runtime.Memory().Write<u32>(kFrameCounterAddress, 2);  // unchanged: no call
                CallSvc(f.runtime, 0x23, {0});
            }
            reached = frame;
        }
        ExitProcess(f.runtime);
    });
    CHECK(status == 0);
    CHECK(f.Logged("frame limit reached: frame 5"));
    CHECK(frames == std::vector<u32>{1, 2, 3, 4, 5});  // the first reading counts, whatever its value
    // The run stopped at frame 5, in the middle of the body.
    CHECK(reached < 100);
}

// ---- the end of the run ---------------------------------------------------------------------------

TEST_CASE("exit statuses: the process exits, svcBreak, a failing thread, a time limit, a request") {
    TempDir dir;
    {
        Fixture f;
        CHECK(BootWith(f, dir, {}, [&] { ExitProcess(f.runtime); }) == 0);
        CHECK(f.Logged("process exited"));
    }
    {
        Fixture f;
        // svcBreak (reason 0, panic): the report is logged and the status is 3
        CHECK(BootWith(f, dir, {}, [&] { CallSvc(f.runtime, 0x3C, {0, 0, 0}); }) == 3);
    }
    {
        Fixture f;
        // the entry function returns: the code host records a failure, status 4
        CHECK(BootWith(f, dir, {"--ms", "50"}, [] {}) == 4);
        CHECK(f.Logged("recompiled code stopped"));
    }
    {
        Fixture f;
        // sleeping for ever: the time limit ends the run at 25 ms of game time (nothing waits: the
        // manual clock jumps to the deadline)
        CHECK(BootWith(f, dir, {"--ms", "25"}, [&] { SleepNs(f.runtime, ~0ull); }) == 0);
        CHECK(f.Logged("time limit reached"));
        CHECK(f.runtime.Clock().Now() >= NsToTicks(24999999));
        CHECK(f.time->NowNs() < 26000000);
    }
    {
        Fixture f;
        // a step asks for the run to end: the status is the request's and the program never runs
        g_request_exit = 5;
        bool ran = false;
        const int status = BootWith(f, dir, {}, [&] { ran = true; });
        g_request_exit.reset();
        CHECK(status == 5);
        CHECK_FALSE(ran);
        CHECK(f.runtime.ExitStatus() == std::optional<int>(5));
    }
}

TEST_CASE("the segments and the stack are charged to the process and to its Commit limit (N1-21)") {
    // Process::HeapAllocate does this for every segment in Azahar; the game sizes its heaps from the
    // limit's current value, so without the charge it asked for more linear memory than the region has.
    Fixture f;
    TempDir dir;
    std::string error;
    const auto loader = Loader::OpenElf(dir.WriteElf(), &error);
    REQUIRE_MESSAGE(loader, error);
    s64 expected = 0;
    for (const SegmentMapping& segment : loader->Image().segments) {
        expected += segment.size;
    }
    REQUIRE(expected > 0);
    s32 commit = -1;
    u32 used = 0;
    CHECK(BootWith(f, dir, {}, [&] {
              const auto process = f.runtime.GetKernel().CurrentProcess();
              commit = process->resource_limit->GetCurrentValue(ResourceLimitType::Commit);
              used = process->memory_used;
              ExitProcess(f.runtime);
          }) == 0);
    CHECK(commit == expected);
    // The segments, plus the one TLS page the main thread gets (Azahar's Process::AllocateThreadLocalStorage
    // adds a page to memory_used the same way).
    CHECK(used == static_cast<u32>(expected) + kPageSize);
}

TEST_CASE("Run holds a TimerResolutionScope while the program runs (N1-21)") {
    // A unit test cannot see the host's tick of the whole process from here, but it can see that the run
    // holds a scope: removing the scope from Runtime::Run makes the count inside the program 0.
    Fixture f;
    TempDir dir;
    const int before = detail::TimerResolutionScope::live_count();
    int during = -1;
    CHECK(BootWith(f, dir, {}, [&] {
              during = detail::TimerResolutionScope::live_count();
              ExitProcess(f.runtime);
          }) == 0);
    CHECK(during == before + 1);
    CHECK(detail::TimerResolutionScope::live_count() == before);  // released when the run ends
}

#if defined(_WIN32)
// What the test asks Windows directly (not through the code under test). The declarations are made here
// and not by windows.h, whose macros break the headers above; kernel32 and ntdll are always loaded.
extern "C" {
void* GetModuleHandleW(const wchar_t* name);
void* GetProcAddress(void* module, const char* name);
void* GetCurrentProcess(void);
}

namespace win_probe {
struct PowerThrottling {
    unsigned long version;
    unsigned long control_mask;
    unsigned long state_mask;
};
constexpr unsigned long kExecutionSpeed = 0x1;
constexpr unsigned long kIgnoreTimerResolution = 0x4;

// GetProcessInformation(ProcessPowerThrottling = 4), resolved from kernel32 like SetProcessInformation
// is by the code; nullopt when this Windows has no such call or it fails.
std::optional<PowerThrottling> ReadPowerThrottling() {
    using Fn = int (*)(void*, int, void*, unsigned long);
    void* kernel = GetModuleHandleW(L"kernel32.dll");
    void* address = kernel != nullptr ? GetProcAddress(kernel, "GetProcessInformation") : nullptr;
    if (address == nullptr) {
        return std::nullopt;
    }
    PowerThrottling state{1, 0, 0};
    if (reinterpret_cast<Fn>(address)(GetCurrentProcess(), 4, &state, sizeof(state)) == 0) {
        return std::nullopt;
    }
    return state;
}

// The system timer's current resolution in 100 ns units (NtQueryTimerResolution), nullopt when it
// cannot be read.
std::optional<unsigned long> CurrentTimerResolution() {
    using Fn = long (*)(unsigned long*, unsigned long*, unsigned long*);
    void* ntdll = GetModuleHandleW(L"ntdll.dll");
    void* address = ntdll != nullptr ? GetProcAddress(ntdll, "NtQueryTimerResolution") : nullptr;
    if (address == nullptr) {
        return std::nullopt;
    }
    unsigned long minimum = 0, maximum = 0, current = 0;
    if (reinterpret_cast<Fn>(address)(&minimum, &maximum, &current) < 0) {
        return std::nullopt;
    }
    return current;
}
}  // namespace win_probe
#endif

TEST_CASE("TimerResolutionScope: the 1 ms tick, the power-throttling opt-out, nesting (N1-21)") {
    // Without it, on Windows, every host wait with a deadline (the scheduler asleep, the preemption
    // timer) ends at the next 15.6 ms tick, and the game's frames drift off the VBlank.
    using detail::TimerResolutionScope;
    REQUIRE(TimerResolutionScope::live_count() == 0);
    CHECK(TimerResolutionScope::throttling_opt_out_flags() == 0);
#if defined(_WIN32)
    const auto throttling_before = win_probe::ReadPowerThrottling();
#endif
    {
        TimerResolutionScope scope;
        CHECK(TimerResolutionScope::live_count() == 1);
#if defined(_WIN32)
        CHECK(scope.active());

        // The tick, read back from the system: at most 1 ms plus a tolerance (another process may hold
        // a finer one, which only makes it smaller).
        if (const auto tick = win_probe::CurrentTimerResolution()) {
            INFO("the system timer's resolution: " << *tick << " x 100 ns");
            CHECK(*tick <= 11000);
        }

        // The opt-out, read back: the flags are controlled (in the control mask) and not set (in the
        // state mask). EXECUTION_SPEED is required on every Windows that has the call; a build that
        // refused IGNORE_TIMER_RESOLUTION says so through throttling_opt_out_flags().
        const unsigned flags = TimerResolutionScope::throttling_opt_out_flags();
        CHECK((flags & win_probe::kExecutionSpeed) != 0);
        if (const auto state = win_probe::ReadPowerThrottling()) {
            INFO("opt-out flags " << flags << ", control mask " << state->control_mask << ", state mask "
                                  << state->state_mask);
            CHECK((state->control_mask & flags) == flags);
            CHECK((state->state_mask & flags) == 0);
            CHECK((state->control_mask & win_probe::kExecutionSpeed) != 0);
            if (flags != (win_probe::kExecutionSpeed | win_probe::kIgnoreTimerResolution)) {
                MESSAGE("this Windows refused IGNORE_TIMER_RESOLUTION: only EXECUTION_SPEED is checked");
            } else {
                CHECK((state->control_mask & win_probe::kIgnoreTimerResolution) != 0);
            }
        }

        // The elapsed time proves the request only on a host whose tick is otherwise coarse: another
        // process that keeps a fine tick makes the same loop fast without the code (40 waits of 1 ms
        // took 210 ms here with no request and 41 ms with it), so the tick read back above is the
        // check that holds everywhere; this one separates the two cases when the tick is coarse.
        SteadyTimeSource source;
        std::mutex mutex;
        std::condition_variable cv;
        const auto start = std::chrono::steady_clock::now();
        constexpr int kWaits = 40;  // 40 ticks of 15.6 ms would take 624 ms
        for (int i = 0; i < kWaits; ++i) {
            std::unique_lock lock(mutex);
            source.WaitUntil(cv, lock, source.NowNs() + 1000000, [] { return false; });
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        INFO("40 waits of 1 ms took " << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
                                      << " us");
        CHECK(elapsed < std::chrono::milliseconds(150));
#else
        CHECK_FALSE(scope.active());  // nothing to ask on this host
        CHECK(TimerResolutionScope::throttling_opt_out_flags() == 0);
#endif
        {
            // a second scope nests: the host counts the requests, and the opt-out stays until the last
            TimerResolutionScope second;
            CHECK(second.active() == scope.active());
            CHECK(TimerResolutionScope::live_count() == 2);
        }
        CHECK(TimerResolutionScope::live_count() == 1);
#if defined(_WIN32)
        const unsigned still = TimerResolutionScope::throttling_opt_out_flags();
        CHECK((still & win_probe::kExecutionSpeed) != 0);
        if (const auto state = win_probe::ReadPowerThrottling()) {
            CHECK((state->control_mask & still) == still);  // the first scope still has it
        }
#endif
    }
    CHECK(TimerResolutionScope::live_count() == 0);
    CHECK(TimerResolutionScope::throttling_opt_out_flags() == 0);
#if defined(_WIN32)
    // the last scope gave control back to the system, as it was before the first
    const auto throttling_after = win_probe::ReadPowerThrottling();
    CHECK(throttling_after.has_value() == throttling_before.has_value());
    if (throttling_before && throttling_after) {
        CHECK(throttling_after->control_mask == throttling_before->control_mask);
        CHECK(throttling_after->state_mask == throttling_before->state_mask);
    }
#endif
}

TEST_CASE("Boot refuses what it cannot load, with status 2") {
    TempDir dir;
    {
        Fixture f;
        CHECK(f.runtime.Boot(Parse({})) == 2);  // no PROGRAM
        CHECK(f.Logged("expected one PROGRAM"));
    }
    {
        Fixture f;
        CHECK(f.runtime.Boot(Parse({dir.File("missing.elf")})) == 2);
        CHECK(f.Logged("cannot load"));
    }
    {
        Fixture f;
        CHECK(f.runtime.Boot(Parse({dir.WriteElf()})) == 2);  // guest code and no --code
        CHECK(f.Logged("--code"));
    }
    {
        Fixture f;
        CHECK(f.runtime.Boot(Parse({dir.WriteElf(), "--code", dir.File("missing.so")})) == 2);
    }
    {
        Fixture f;
        f.runtime.SetMainThreadEntry([](GuestThread&) {});
        CHECK(f.runtime.Boot(Parse({dir.WriteElf(), "--ms", "soon"})) == 2);
        CHECK(f.Logged("bad --ms"));
    }
    {
        Fixture f;
        f.runtime.SetMainThreadEntry([](GuestThread&) {});
        CHECK(f.runtime.Boot(Parse({dir.WriteElf(), "--init-time", "yesterday"})) == 2);
    }
}

// ---- the system pages ------------------------------------------------------------------------------

TEST_CASE("the shared page starts at --init-time and advances with game time") {
    Fixture f;
    TempDir dir;
    u32 counter_at_start = 0, counter_after = 0;
    u64 seconds_at_start = 0, seconds_after = 0;
    auto date = [&](GuestMemory& memory) -> std::pair<u32, u64> {
        const u32 counter = *memory.Read<u32>(kSharedPageVaddr + shared_page::kDateTimeCounter);
        // the update writes the block the old counter's parity picks and then bumps it, so after the
        // bump the block written last is DateTime1 for an odd counter and DateTime0 for an even one
        const u32 block = kSharedPageVaddr + (counter % 2 ? shared_page::kDateTime1 : shared_page::kDateTime0);
        const u64 ms_since_1900 = *memory.Read<u64>(block + shared_page::kDtDateTime);
        return {counter, (ms_since_1900 - kMillisecondsFrom1900To2000) / 1000 + kUnixTimeOf2000};
    };
    const int status = BootWith(f, dir, {"--init-time", "1700000000", "--frames", "99999"}, [&] {
        std::tie(counter_at_start, seconds_at_start) = date(f.runtime.Memory());
        SleepNs(f.runtime, 3601ull * kNanosecondsPerSecond);  // past the first hourly update
        std::tie(counter_after, seconds_after) = date(f.runtime.Memory());
        ExitProcess(f.runtime);
    });
    CHECK(status == 0);
    CHECK(f.runtime.InitTime() == 1700000000);
    CHECK(counter_at_start == 1);
    CHECK(seconds_at_start == 1700000000);
    CHECK(counter_after == 2);
    CHECK(seconds_after == 1700000000 + 3600);  // the update ran at one hour of game time
}

// ---- the traces ----------------------------------------------------------------------------------

TEST_CASE("--trace-svc writes N0-04's format: header, thread lines, one line per call with out=") {
    Fixture f;
    TempDir dir;
    const std::string trace = dir.File("svc.txt");
    const int status = BootWith(f, dir, {"--trace-svc", trace}, [&] {
        CallSvc(f.runtime, 0x23, {0xD7});  // CloseHandle of nothing: 0xD8E007F7
        SleepNs(f.runtime, 5000000);
        ExitProcess(f.runtime);
    });
    CHECK(status == 0);
    const std::vector<std::string> lines = Lines(ReadFile(trace));
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].rfind("# svctrace v1 ", 0) == 0);
    CHECK(lines[0].find(" game=0x0000000000000000 cpu=native new3ds=off") != std::string::npos);
    CHECK(lines[1] == "# thread 1 parent=0 entry=00100000 arg=00000000 stack=10000000 prio=48 core=0/0");
    const std::string closed = lines[2];
    CHECK(closed.substr(0, closed.find(" t=")) == "1 23 pc=00100010 lr=00100020 000000D7 00000000 00000000 00000000");
    CHECK(closed.substr(closed.find(" in4=")) ==
          " in4=00000000,00000000,00000000 out=D8E007F7,00000000,00000000,00000000");
    // SleepThread: written when the thread ran again, with the time it was called at
    CHECK(lines[3].rfind("1 0A pc=00100010 lr=00100020 004C4B40 00000000 00000000 00000000 t=", 0) == 0);
    // (SleepThread leaves the registers alone, so out= is what the call was made with)
    CHECK(lines[3].find(" in4=00000000,00000000,00000000 out=004C4B40,00000000,00000000,00000000") !=
          std::string::npos);
    // ExitProcess never completes
    CHECK(lines[4].rfind("1 03 pc=00100010 ", 0) == 0);
    CHECK(lines[4].substr(lines[4].size() - 6) == " out=-");
}

TEST_CASE("--state-trace writes a line when the frame counter changes") {
    Fixture f;
    TempDir dir;
    dir.WriteText("observable.txt",
                  "# values\n"
                  "0x010571d4  4 frame_counter eq\n"
                  "[[0x01057100]+0x4]  1 chain eq   # a pointer chain\n"
                  "[[0x01057200]+0x4]  2 null_chain ignore\n");
    const std::string trace = dir.File("state.txt");
    const int status = BootWith(f, dir, {"--state-trace", trace, "--observable", dir.File("observable.txt")}, [&] {
        GuestMemory& memory = f.runtime.Memory();
        CHECK(IsSuccess(memory.Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        memory.Write<u32>(0x01057100, kFramePage + 0x300);  // the pointer chain's target
        memory.Write<u8>(kFramePage + 0x304, 0x5A);
        memory.Write<u32>(kFrameCounterAddress, 1);
        CallSvc(f.runtime, 0x23, {0});
        memory.Write<u32>(kFrameCounterAddress, 1);  // unchanged
        CallSvc(f.runtime, 0x23, {0});
        memory.Write<u32>(kFrameCounterAddress, 2);
        CallSvc(f.runtime, 0x23, {0});
        ExitProcess(f.runtime);
    });
    CHECK(status == 0);
    const std::vector<std::string> lines = Lines(ReadFile(trace));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].rfind("# statetrace v1 ", 0) == 0);
    auto tail = [](const std::string& line) { return line.substr(line.find(' ', line.find("t=")) + 1); };
    CHECK(lines[1].rfind("f=1 t=", 0) == 0);
    CHECK(tail(lines[1]) == "frame_counter=0x00000001 chain=0x5a null_chain=-");
    CHECK(lines[2].rfind("f=2 t=", 0) == 0);
    CHECK(tail(lines[2]) == "frame_counter=0x00000002 chain=0x5a null_chain=-");
}

TEST_CASE("a trace that cannot be written ends Boot with status 2 before the program runs") {
    Fixture f;
    TempDir dir;
    bool ran = false;
    CHECK(BootWith(f, dir, {"--trace-svc", dir.File("no/such/dir/svc.txt")}, [&] { ran = true; }) == 2);
    CHECK_FALSE(ran);
    Fixture g;
    CHECK(BootWith(g, dir, {"--state-trace", dir.File("s.txt"), "--observable", dir.File("none.txt")},
                   [&] { ran = true; }) == 2);
    CHECK_FALSE(ran);
    CHECK(g.Logged("cannot read the observable file"));
}

TEST_CASE("AddSvcHooks fans out to every subscriber, and debug strings reach them") {
    Fixture f;
    TempDir dir;
    std::vector<u32> numbers_a, numbers_b;
    std::vector<std::string> strings;
    SvcHooks a;
    a.complete = [&](const SvcCall& call, const std::array<u32, 4>&) { numbers_a.push_back(call.number); };
    a.debug_string = [&](std::string_view text) { strings.emplace_back(text); };
    SvcHooks b;
    b.complete = [&](const SvcCall& call, const std::array<u32, 4>&) { numbers_b.push_back(call.number); };
    f.runtime.AddSvcHooks(a);  // before Boot
    const int status = BootWith(f, dir, {}, [&] {
        CallSvc(f.runtime, 0x23, {0});
        f.runtime.AddSvcHooks(b);  // and while the program runs
        const char text[] = "hello";
        f.runtime.Memory().WriteBlock(kText + 0x10, text, 5);
        CallSvc(f.runtime, 0x3D, {kText + 0x10, 5});
        CallSvc(f.runtime, 0x23, {0});
        ExitProcess(f.runtime);
    });
    CHECK(status == 0);
    CHECK(strings == std::vector<std::string>{"hello"});
    CHECK(numbers_a == std::vector<u32>{0x23, 0x3D, 0x23});
    CHECK(numbers_b == std::vector<u32>{0x3D, 0x23});  // subscribed after the first call
}

// ---- Own and OnFinish -----------------------------------------------------------------------------

TEST_CASE("OnFinish runs once the run is over, in reverse order; Own objects go in reverse order") {
    TempDir dir;
    std::vector<std::string> events;
    struct Marker {
        Marker(std::vector<std::string>* e, std::string n) : events(e), name(std::move(n)) {}
        ~Marker() { events->push_back("destroy " + name); }
        std::vector<std::string>* events;
        std::string name;
    };
    {
        Fixture f;
        f.runtime.OnFinish([&] { events.push_back("finish 1"); });
        f.runtime.OnFinish([&] { events.push_back("finish 2"); });
        f.runtime.Own(std::make_shared<Marker>(&events, "a"));
        f.runtime.Own(std::make_shared<Marker>(&events, "b"));
        CHECK(BootWith(f, dir, {}, [&] {
                  events.push_back("main");
                  ExitProcess(f.runtime);
              }) == 0);
        CHECK(events == std::vector<std::string>{"main", "finish 2", "finish 1"});
    }
    CHECK(events == std::vector<std::string>{"main", "finish 2", "finish 1", "destroy b", "destroy a"});
}

// ---- the GPU host ----------------------------------------------------------------------------------

TEST_CASE("an interrupt signalled from another thread reaches the sink on the scheduler thread, its delay ignored") {
    Fixture f;
    REQUIRE(f.runtime.Prepare(Parse({}), {}));
    ManualTimeSource& time = *f.time;
    std::vector<gpu::Interrupt> got;
    std::vector<std::thread::id> threads;
    f.runtime.SetGpuInterruptSink([&](gpu::Interrupt id) {
        got.push_back(id);
        threads.push_back(std::this_thread::get_id());
    });
    gpu::Host& host = f.runtime.GpuHost();
    std::thread([&] {
        host.SignalInterrupt(gpu::Interrupt::PSC0, 0);
        host.SignalInterrupt(gpu::Interrupt::P3D, 2000000);  // the GPU's 2 ms: the host does not wait (D25, D41)
    }).join();
    CHECK(got.empty());  // nothing is called from the other thread
    CHECK(f.runtime.Scheduler().RunPending());
    REQUIRE(got.size() == 2);  // both at the first safe point, in the order signalled, with no game time passed
    CHECK(got[0] == gpu::Interrupt::PSC0);
    CHECK(got[1] == gpu::Interrupt::P3D);
    CHECK(threads[0] == std::this_thread::get_id());
    CHECK(threads[1] == std::this_thread::get_id());
    time.Advance(1900000);
    f.runtime.Scheduler().RunPending();
    time.Advance(200000);
    f.runtime.Scheduler().RunPending();
    CHECK(got.size() == 2);  // nothing arrives later: no second delivery of the delayed one
}

TEST_CASE("gpu::Host::Post runs the work on the scheduler thread at a safe point") {
    Fixture f;
    REQUIRE(f.runtime.Prepare(Parse({}), {}));
    std::vector<int> order;
    std::thread([&] {
        f.runtime.GpuHost().Post([&] { order.push_back(1); });
        f.runtime.GpuHost().Post([&] { order.push_back(2); });
    }).join();
    CHECK(order.empty());
    f.runtime.Scheduler().RunPending();
    CHECK(order == std::vector<int>{1, 2});
}

TEST_CASE("a refresh rate reaches the sink, before or after the sink is set") {
    Fixture f;
    REQUIRE(f.runtime.Prepare(Parse({}), {}));
    CHECK(f.runtime.RefreshRate() == 0.0);
    std::vector<double> got;
    f.runtime.SetRefreshRate(59.83);  // no sink yet
    f.runtime.SetRefreshRateSink([&](double hz) { got.push_back(hz); });
    REQUIRE(got.size() == 1);  // a known rate is delivered when the sink arrives
    CHECK(got[0] == 59.83);
    std::thread([&] { f.runtime.SetRefreshRate(60.0); }).join();  // the platform's thread
    CHECK(got.size() == 1);
    f.runtime.Scheduler().RunPending();
    REQUIRE(got.size() == 3);  // the first call's post, then the second
    CHECK(got[2] == 60.0);
    CHECK(f.runtime.RefreshRate() == 60.0);
}

TEST_CASE("SetRefreshRate from guest code calls the sink before it returns; elsewhere it is queued") {
    Fixture f;
    REQUIRE(f.runtime.Prepare(Parse({}), {}));
    std::vector<double> got;
    f.runtime.SetRefreshRateSink([&](double hz) { got.push_back(hz); });
    bool finished = false;
    std::size_t inside = 0;
    Kernel& kernel = f.runtime.GetKernel();
    auto thread = kernel.NewThread("guest", 0x30, 0, kernel.CurrentProcess());
    f.runtime.Scheduler().AttachFunction(thread, [&] {
        std::thread([&] { f.runtime.SetRefreshRate(90.0); }).join();  // queued: not on this thread
        CHECK(got.empty());
        f.runtime.SetRefreshRate(120.0);  // guest code: applied before it returns
        inside = got.size();
        finished = true;
    });
    f.runtime.Scheduler().Start(thread);
    REQUIRE(f.runtime.Scheduler().Run([&] { return finished; }) == RunStatus::Stopped);
    REQUIRE(inside == 1);
    CHECK(got[0] == 120.0);
    f.runtime.Scheduler().RunPending();
    CHECK(got.size() == 1);  // the queued 90 Hz was older than the call that was applied at once
    CHECK(f.runtime.RefreshRate() == 120.0);
    // The scheduler's own host stack (a posted function, a timer event) is not guest code: queued.
    f.runtime.GetKernel().Post([&] { f.runtime.SetRefreshRate(75.0); });
    f.runtime.Scheduler().RunPending();
    REQUIRE(got.size() == 2);
    CHECK(got[1] == 75.0);
}

// ---- Host::PhysicalPointer at the edges of the areas ----------------------------------------------------

namespace {

constexpr u32 kVramEnd = 0x18600000;
constexpr u32 kDspEnd = 0x1FF80000;
constexpr u32 kFcramEnd = 0x28000000;

// A runtime whose linear heap covers the whole 128 MiB of FCRAM, so that all three areas are
// completely mapped, as CaptureHost's are.
struct Edges {
    Fixture f;
    Edges() {
        REQUIRE(f.runtime.Prepare(Parse({}), {}));
        GuestMemory& memory = f.runtime.Memory();
        memory.SetLinearHeapArea(kLinearHeapVaddr);
        REQUIRE(IsSuccess(memory.Map(kLinearHeapVaddr, kFcramSize, MemPerm::ReadWrite, MemState::Continuous)));
    }
    gpu::Host& host() { return f.runtime.GpuHost(); }
};

}  // namespace

TEST_CASE("bounds: Window on the runtime's gpu::Host (the cases of gpu_gx_test.cpp for VRAM, DSP RAM and FCRAM)") {
    Edges e;
    gpu::Host& host = e.host();
    // A size of 0 one past the end of an area is valid: an empty span that points at the end.
    for (const u32 end : {kVramEnd, kDspEnd, kFcramEnd}) {
        INFO("end " << end);
        const std::span<u8> w = gpu::gx::Window(host, end, 0);
        CHECK(w.empty());
        CHECK(w.data() == host.PhysicalPointer(end, 0));
        CHECK(w.data() != nullptr);
        CHECK(gpu::gx::IsValidPhysicalAddress(host, end));
        CHECK_FALSE(gpu::gx::IsValidPhysicalAddress(host, end + 8));
        // A range that starts at the end has an empty prefix (still non-null).
        const std::span<u8> after = gpu::gx::Window(host, end, 16);
        CHECK(after.empty());
        CHECK(after.data() != nullptr);
    }
    // A start in no area (below, between, above, and at the top of the address space).
    for (const u32 paddr : {0x08000000u, 0x18600008u, 0x1FF80008u, 0x28000008u, 0x30000008u, 0xFFFFFFF0u}) {
        const std::span<u8> w = gpu::gx::Window(host, paddr, 16);
        INFO("paddr " << paddr);
        CHECK(w.empty());
        CHECK(w.data() == nullptr);
        CHECK_FALSE(gpu::gx::IsValidPhysicalAddress(host, paddr));
    }
    // Every prefix length from 1 to 40 bytes before the end of an area, for a request that is longer:
    // the binary search finds exactly the bytes that are left.
    for (const u32 end : {kVramEnd, kDspEnd, kFcramEnd}) {
        for (u32 left = 1; left <= 40; ++left) {
            const std::span<u8> w = gpu::gx::Window(host, end - left, 64);
            INFO("end " << end << " left " << left);
            REQUIRE(w.size() == left);
            CHECK(w.data() == host.PhysicalPointer(end - left, 0));
        }
    }
    // A request that fits is returned whole, and the exact size of the area too.
    CHECK(gpu::gx::Window(host, 0x185FFFF0, 16).size() == 16);
    CHECK(gpu::gx::Window(host, 0x18000000, 0x600000).size() == 0x600000);
    CHECK(gpu::gx::Window(host, 0x18000000, 0x600001).size() == 0x600000);
    CHECK(gpu::gx::Window(host, 0x18000000, 0x10000000).size() == 0x600000);
    CHECK(gpu::gx::Window(host, 0x27FFFFF0, 16).size() == 16);
    CHECK(gpu::gx::Window(host, 0x20000000, 0x08000000).size() == 0x08000000);
    CHECK(gpu::gx::Window(host, 0x20000000, 0x08000001).size() == 0x08000000);
    // The areas do not join: a range from DSP RAM into FCRAM is cut at the end of DSP RAM.
    CHECK(gpu::gx::Window(host, 0x1FF7FFF8, 0x80010).size() == 8);
    // A size that would wrap the 32-bit address space is cut to what is left of the area.
    CHECK(gpu::gx::Window(host, 0x27FFFFF0, 0xFFFFFFFFull).size() == 16);
}

TEST_CASE("bounds: PhysicalPointer answers as CaptureHost does inside and at the ends of VRAM, DSP RAM and FCRAM") {
    Edges e;
    gpu::Host& runtime_host = e.host();
    gpu_capture::CaptureHost capture_host;
    struct Probe {
        u32 paddr, size;
    };
    std::vector<Probe> probes;
    for (const u32 base : {0x18000000u, kVramEnd, 0x1FF00000u, kDspEnd, 0x20000000u, kFcramEnd - 0x10000u}) {
        for (const u32 delta : {0u, 1u, 0x10u, 0xFFFFu}) {
            for (const u32 size : {0u, 1u, 16u, 0x1000u, 0x10000u}) {
                probes.push_back({base + delta, size});
                if (base >= delta) {
                    probes.push_back({base - delta, size});
                }
            }
        }
    }
    probes.push_back({0x1FF7FFFF, 1});
    probes.push_back({0x1FF7FFFF, 2});
    probes.push_back({0x185FFFFF, 1});
    probes.push_back({0x185FFFFF, 2});
    probes.push_back({0x27FFFFFF, 1});
    probes.push_back({0x27FFFFFF, 2});
    for (const Probe& p : probes) {
        // CaptureHost's FCRAM runs to 0x30000000 for the New 3DS; the runtime's ends at 0x28000000.
        if (p.paddr >= kFcramEnd && p.paddr < 0x30000000) {
            continue;
        }
        if (p.paddr < kFcramEnd && p.paddr >= 0x20000000 && p.paddr + static_cast<u64>(p.size) > kFcramEnd) {
            continue;  // ranges that leave the Old 3DS FCRAM
        }
        INFO("paddr " << p.paddr << " size " << p.size);
        CHECK((runtime_host.PhysicalPointer(p.paddr, p.size) != nullptr) ==
              (capture_host.PhysicalPointer(p.paddr, p.size) != nullptr));
    }
}

TEST_CASE("PhysicalPointer: FCRAM is the linear heap, so only where it is mapped; the heap base moves it") {
    Fixture f;
    REQUIRE(f.runtime.Prepare(Parse({}), {}));
    GuestMemory& memory = f.runtime.Memory();
    gpu::Host& host = f.runtime.GpuHost();
    // before the loader has set the heap base FCRAM has no view, not even its end
    CHECK(host.PhysicalPointer(0x20000000, 0) == nullptr);
    CHECK(host.PhysicalPointer(kFcramEnd, 0) == nullptr);
    memory.SetLinearHeapArea(kNewLinearHeapVaddr);
    CHECK(host.PhysicalPointer(0x20000000, 0) == nullptr);  // nothing mapped yet
    REQUIRE(IsSuccess(memory.Map(kNewLinearHeapVaddr + 0x1000, 0x2000, MemPerm::ReadWrite, MemState::Continuous)));
    CHECK(host.PhysicalPointer(0x20001000, 0x2000) == memory.Base() + kNewLinearHeapVaddr + 0x1000);
    CHECK(host.PhysicalPointer(0x20001000, 0x2001) == nullptr);
    CHECK(host.PhysicalPointer(0x20000FFF, 2) == nullptr);
    CHECK(host.PhysicalPointer(0x20003000, 0) == nullptr);
    // the end of the area is an address even though the heap is not mapped there
    CHECK(host.PhysicalPointer(kFcramEnd, 0) == memory.Base() + kNewLinearHeapVaddr + kFcramSize);
    CHECK(host.PhysicalPointer(kFcramEnd, 1) == nullptr);
    // VRAM and DSP RAM are always there
    CHECK(host.PhysicalPointer(0x18000000, 0x600000) == memory.Base() + kVramVaddr);
    CHECK(host.PhysicalPointer(0x1FF00000, 0x80000) == memory.Base() + kDspRamVaddr);
    CHECK(host.VirtualToPhysical(kVramVaddr + 0x10) == std::optional<u32>(0x18000010));
    CHECK(host.VirtualToPhysical(kNewLinearHeapVaddr + 0x1000) == std::optional<u32>(0x20001000));
    CHECK_FALSE(host.VirtualToPhysical(0).has_value());
}

#endif  // recomp.h
