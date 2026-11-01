// mh4n_perf (N1-23): --perf-log writes one row per game frame from the scheduler's counters. Synthetic
// programs only: an ELF the test writes whose main thread runs a C++ function (Runtime::SetMainThreadEntry),
// on a ManualTimeSource, so every number below is a time the test moved the clock by.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("perf_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"
#include "perf/perf_log.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr u32 kText = 0x00100000;
constexpr u32 kFramePage = 0x01057000;  // holds kFrameCounterAddress

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
                ("mh4n_perf_test_" +
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

private:
    std::filesystem::path path_;
};

std::vector<std::string> Lines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string field; std::getline(in, field, ',');) {
        out.push_back(field);
    }
    return out;
}

// the first 7 fields of a row: frame and the six times
std::vector<std::string> Head(const std::string& line) {
    std::vector<std::string> fields = Split(line);
    fields.resize(7);
    return fields;
}

struct Fixture {
    ManualTimeSource* time = nullptr;
    Runtime runtime;
    static RuntimeConfig Config(ManualTimeSource** time) {
        auto source = std::make_unique<ManualTimeSource>();
        *time = source.get();
        RuntimeConfig config;
        config.time = std::move(source);
        return config;
    }
    Fixture() : runtime(Config(&time)) { SetLogFilter(LogFilter(LogLevel::Info)); }
};

void CallSvc(Runtime& rt, u32 number, std::initializer_list<u32> in) {
    RCtx ctx{};
    int i = 0;
    for (const u32 value : in) {
        ctx.r[i++] = value;
    }
    ctx.r[14] = kText + 0x20;
    rt.Svc().CallSvc(ctx, number, kText + 0x11);
}
void ExitProcess(Runtime& rt) {
    CallSvc(rt, 0x03, {});
}

int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra, std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf()};
    args.insert(args.end(), extra.begin(), extra.end());
    f.runtime.SetMainThreadEntry([body = std::move(body)](GuestThread&) { body(); });
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return f.runtime.Boot(options);
}

constexpr s64 kMs = 1'000'000;
constexpr s64 kUs = 1'000;

}  // namespace

TEST_CASE("Snapshot adds the threads, the system calls and the services; the row is the difference") {
    SchedulerCounters before;
    before.guest_ns[1] = 1'000;
    before.guest_ns[7] = 4'000;
    before.svc[0x23].ns = 500;
    before.services["fs:USER"].ns = 200;
    before.thread_svc_ns[1] = 300;        // the main thread's part of svc: 300 of 500
    before.thread_svc_ns[7] = 200;
    before.thread_service_ns[7] = 200;    // the main thread made no service request yet
    before.idle_ns = 10'000;
    before.thread_switches = 5;
    before.preemptions = 1;
    perf::Totals a = perf::Snapshot(before, 1, 100'000);

    SchedulerCounters after = before;
    after.guest_ns[1] += 2'210'004;       // the main thread
    after.guest_ns[7] += 680'109;         // another thread
    after.guest_ns[9] = 5'000;            // a thread that did not exist before
    after.svc[0x23].ns += 600'000;
    after.svc[0x0A].ns += 40'220;
    after.thread_svc_ns[1] += 402'118;    // of the 640,220 ns above
    after.thread_svc_ns[7] += 238'102;
    after.services["fs:USER"].ns += 90'000;
    after.services["srv:"].ns += 5'300;   // a service seen for the first time
    after.thread_service_ns[1] = 61'250;  // the main thread's first service request
    after.thread_service_ns[7] += 34'050;
    after.idle_ns += 4'210'000;
    after.thread_switches += 31;
    after.preemptions += 2;
    perf::Totals b = perf::Snapshot(after, 1, 100'000 + 8'333'511);

    CHECK(a.main_ns == 1'000);
    CHECK(a.guest_ns == 5'000);
    CHECK(a.main_svc_ns == 300);
    CHECK(a.main_service_ns == 0);
    CHECK(b.main_ns == 2'211'004);
    CHECK(b.main_svc_ns == 402'418);
    CHECK(b.main_service_ns == 61'250);
    // 8,333.511 us in all; main 2,210.004; all threads 2,210.004 + 680.109 + 5.000 = 2,895.113; of the system
    // calls' 640.220 the main thread's 402.118, of the services' 95.300 its 61.250
    CHECK(perf::FormatRow(12, a, b) == "12,8333.511,2210.004,2895.113,640.220,95.300,4210.000,31,2,402.118,61.250");
    // the main thread is told apart by its id; 0 means none, and an id with no entry gives 0
    const perf::Totals none = perf::Snapshot(after, 0, 0);
    CHECK(none.main_ns == 0);
    CHECK(none.main_svc_ns == 0);
    CHECK(none.main_service_ns == 0);
    const perf::Totals unknown = perf::Snapshot(after, 42, 0);
    CHECK(unknown.main_ns == 0);
    CHECK(unknown.main_svc_ns == 0);
    CHECK(unknown.main_service_ns == 0);
    CHECK(perf::Snapshot(after, 9, 0).main_ns == 5'000);
    CHECK(perf::Snapshot(after, 9, 0).main_svc_ns == 0);  // a guest entry but no system call entry
    CHECK(perf::Snapshot(after, 7, 0).main_service_ns == 34'250);
    // a counter that went back (it cannot) gives 0, not a huge number
    CHECK(perf::FormatRow(1, b, a) == "1,0.000,0.000,0.000,0.000,0.000,0.000,0,0,0.000,0.000");
}

TEST_CASE("the header names the title, the host clock and the mode; a deterministic log says virtual") {
    const std::string host = perf::HeaderLine(0x0004000000126300ull, false);
    CHECK(host.rfind("# perf v2 ", 0) == 0);
    CHECK(host.find(" game=0x0004000000126300 cpu=native new3ds=off clock=host mode=realtime") != std::string::npos);
    const std::string det = perf::HeaderLine(0, true);
    CHECK(det.rfind("# perf v2 ", 0) == 0);
    CHECK(det.find(" clock=virtual mode=deterministic") != std::string::npos);
    // v1's columns in their places, then the main thread's part of svc_us and service_us
    CHECK(std::string(perf::ColumnLine()) ==
          "frame,present_us,main_us,guest_us,svc_us,service_us,idle_us,switches,preempts,main_svc_us,main_service_us");
}

TEST_CASE("--perf-log writes a row per game frame with the time of each category") {
    Fixture f;
    TempDir dir;
    const std::string csv = dir.File("perf.csv");
    const int status = BootWith(f, dir, {"--perf-log", csv}, [&] {
        Runtime& rt = f.runtime;
        GuestMemory& memory = rt.Memory();
        CHECK(IsSuccess(memory.Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        GuestScheduler& scheduler = rt.Scheduler();

        // frame 1: 2 ms in guest code
        f.time->Advance(2 * kMs);
        memory.Write<u32>(kFrameCounterAddress, 1);
        CallSvc(rt, 0x23, {0});

        // frame 2: 3 ms in guest code, 0.7 ms in a system call, 0.5 ms in a service
        f.time->Advance(3 * kMs);
        scheduler.BeginSvc(0x30);
        f.time->Advance(700 * kUs);
        scheduler.EndSvc();
        scheduler.BeginService("fs:USER");
        f.time->Advance(500 * kUs);
        scheduler.EndService();
        memory.Write<u32>(kFrameCounterAddress, 2);
        CallSvc(rt, 0x23, {0});

        // the same counter again is no frame; frame 3: asleep for 1 ms (idle)
        CallSvc(rt, 0x23, {0});
        CallSvc(rt, 0x0A, {static_cast<u32>(kMs), 0});
        memory.Write<u32>(kFrameCounterAddress, 3);
        CallSvc(rt, 0x23, {0});
        ExitProcess(rt);
    });
    CHECK(status == 0);

    const std::vector<std::string> lines = Lines(csv);
    REQUIRE(lines.size() == 6);  // header, columns, three rows, footer
    CHECK(lines[0].rfind("# perf v2 ", 0) == 0);
    CHECK(lines[0].find("clock=host mode=realtime") != std::string::npos);
    CHECK(lines[1] == perf::ColumnLine());
    // frame, present, main, guest, svc, service, idle, switches, preempts, main_svc, main_service
    // the first row is from the start of the log
    const std::vector<std::string> first = Split(lines[2]);
    REQUIRE(first.size() == 11);
    CHECK(Head(lines[2]) ==
          std::vector<std::string>{"1", "2000.000", "2000.000", "2000.000", "0.000", "0.000", "0.000"});
    // the only thread is the main thread: its parts are the whole of svc_us and service_us
    CHECK(Split(lines[3]) == std::vector<std::string>{"2", "4200.000", "3000.000", "3000.000", "700.000", "500.000",
                                                      "0.000", "0", "0", "700.000", "500.000"});
    const std::vector<std::string> third = Split(lines[4]);
    REQUIRE(third.size() == 11);
    CHECK(third[0] == "3");
    // idle_us and present_us: the thread slept 1 ms and nothing else could run. The sleep goes through the game
    // clock's ticks (3.7 ns each), so the deadline is reached to within a few ns, not exactly.
    CHECK(std::stod(third[6]) == doctest::Approx(1000.0).epsilon(0.0001));
    CHECK(std::stod(third[1]) == doctest::Approx(1000.0).epsilon(0.0001));
    CHECK(third[2] == "0.000");
    // only idle time went by: the time blocked inside svcSleepThread belongs to no system call and no thread
    CHECK(third[3] == "0.000");   // guest_us
    CHECK(third[4] == "0.000");   // svc_us
    CHECK(third[5] == "0.000");   // service_us
    CHECK(third[9] == "0.000");   // main_svc_us: nor to the thread blocked in it
    CHECK(third[10] == "0.000");  // main_service_us
    CHECK(third[1] == third[6]);  // present_us == idle_us: the same move of the manual clock
    CHECK(std::stoi(third[7]) >= 1);  // it was switched away and back
    CHECK(lines[5] == "# frames=3 dropped=0");
}

TEST_CASE("a frame seen at a preemption check or a posted task is a row of its own time, not the next one's") {
    Fixture f;
    TempDir dir;
    const std::string csv = dir.File("perf.csv");
    const int status = BootWith(f, dir, {"--perf-log", csv}, [&] {
        Runtime& rt = f.runtime;
        GuestMemory& memory = rt.Memory();
        CHECK(IsSuccess(memory.Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        GuestScheduler& scheduler = rt.Scheduler();

        f.time->Advance(2 * kMs);  // frame 1: seen at a system call
        memory.Write<u32>(kFrameCounterAddress, 1);
        CallSvc(rt, 0x23, {0});

        f.time->Advance(3 * kMs);  // frame 2: seen at a preemption check; the row is written at the next call
        memory.Write<u32>(kFrameCounterAddress, 2);
        scheduler.RaiseFlag();
        scheduler.PreemptPoint();
        CallSvc(rt, 0x23, {0});

        f.time->Advance(1 * kMs);  // frame 3: seen in a posted task (the input sources poll the frame there)
        memory.Write<u32>(kFrameCounterAddress, 3);
        scheduler.Post([&] { rt.PollFrame(); });
        scheduler.RaiseFlag();
        scheduler.PreemptPoint();
        CallSvc(rt, 0x23, {0});

        f.time->Advance(1 * kMs);  // frame 4: seen at a system call
        memory.Write<u32>(kFrameCounterAddress, 4);
        CallSvc(rt, 0x23, {0});
        ExitProcess(rt);
    });
    CHECK(status == 0);

    const std::vector<std::string> lines = Lines(csv);
    REQUIRE(lines.size() == 7);  // header, columns, four rows, footer
    for (std::size_t i = 2; i < 6; ++i) {
        const std::vector<std::string> row = Split(lines[i]);
        REQUIRE(row.size() == 11);
        const double present = std::stod(row[1]), main = std::stod(row[2]), guest = std::stod(row[3]);
        CHECK(main <= guest);
        CHECK(guest <= present);
    }
    CHECK(Head(lines[3]) ==
          std::vector<std::string>{"2", "3000.000", "3000.000", "3000.000", "0.000", "0.000", "0.000"});
    CHECK(Head(lines[4]) ==
          std::vector<std::string>{"3", "1000.000", "1000.000", "1000.000", "0.000", "0.000", "0.000"});
    CHECK(Head(lines[5]) ==
          std::vector<std::string>{"4", "1000.000", "1000.000", "1000.000", "0.000", "0.000", "0.000"});
    CHECK(lines[6] == "# frames=4 dropped=0");
}

TEST_CASE("main_us is the main thread's time even when another thread's system call sees the frame") {
    Fixture f;
    TempDir dir;
    const std::string csv = dir.File("perf.csv");
    const int status = BootWith(f, dir, {"--perf-log", csv}, [&] {
        Runtime& rt = f.runtime;
        GuestMemory& memory = rt.Memory();
        CHECK(IsSuccess(memory.Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        Kernel& kernel = rt.GetKernel();
        auto other = kernel.NewThread("other", 0x30, 0, kernel.CurrentProcess());
        rt.Scheduler().AttachFunction(other, [&] {
            f.time->Advance(680 * kUs);
            memory.Write<u32>(kFrameCounterAddress, 1);
            CallSvc(rt, 0x23, {0});  // the frame is seen here, on "other"
        });
        CallSvc(rt, 0x23, {0});  // the first poll gives the counter's starting value as a frame (row 0)
        f.time->Advance(250 * kUs);
        rt.Scheduler().Start(other);
        CallSvc(rt, 0x0A, {static_cast<u32>(kMs), 0});  // the main thread sleeps; "other" runs
        ExitProcess(rt);
    });
    CHECK(status == 0);

    const std::vector<std::string> lines = Lines(csv);
    REQUIRE(lines.size() == 5);  // header, columns, the starting value, frame 1, footer
    const std::vector<std::string> row = Split(lines[3]);
    REQUIRE(row.size() == 11);
    CHECK(row[0] == "1");
    CHECK(row[2] == "250.000");  // main_us: the main thread's own guest time
    CHECK(row[3] == "930.000");  // guest_us: 250 + 680 of the other thread
}

TEST_CASE("main_svc_us and main_service_us hold the main thread's calls and services only (N1-32)") {
    Fixture f;
    TempDir dir;
    const std::string csv = dir.File("perf.csv");
    const int status = BootWith(f, dir, {"--perf-log", csv}, [&] {
        Runtime& rt = f.runtime;
        GuestMemory& memory = rt.Memory();
        CHECK(IsSuccess(memory.Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        GuestScheduler& scheduler = rt.Scheduler();
        Kernel& kernel = rt.GetKernel();
        auto other = kernel.NewThread("other", 0x30, 0, kernel.CurrentProcess());
        scheduler.AttachFunction(other, [&] {
            f.time->Advance(50 * kUs);   // guest
            scheduler.BeginSvc(0x31);
            f.time->Advance(300 * kUs);  // the other thread's system call
            scheduler.BeginService("srv:");
            f.time->Advance(400 * kUs);  // the other thread's service
            scheduler.EndService();
            scheduler.EndSvc();
            memory.Write<u32>(kFrameCounterAddress, 1);
            CallSvc(rt, 0x23, {0});  // the frame is seen here, on "other"
        });
        CallSvc(rt, 0x23, {0});  // the first poll gives the counter's starting value as a frame (row 0)
        f.time->Advance(250 * kUs);   // guest
        scheduler.BeginSvc(0x30);
        f.time->Advance(300 * kUs);   // the main thread's system call
        scheduler.BeginService("fs:USER");
        f.time->Advance(200 * kUs);   // the main thread's service
        scheduler.EndService();
        f.time->Advance(100 * kUs);   // its system call again
        scheduler.EndSvc();
        scheduler.Start(other);
        CallSvc(rt, 0x0A, {static_cast<u32>(kMs), 0});  // the main thread sleeps; "other" runs
        ExitProcess(rt);
    });
    CHECK(status == 0);

    const std::vector<std::string> lines = Lines(csv);
    REQUIRE(lines.size() == 5);  // header, columns, the starting value, frame 1, footer
    CHECK(lines[0].rfind("# perf v2 ", 0) == 0);
    CHECK(lines[1] == perf::ColumnLine());
    const std::vector<std::string> row = Split(lines[3]);
    REQUIRE(row.size() == 11);
    CHECK(row[0] == "1");
    CHECK(row[2] == "250.000");   // main_us
    CHECK(row[3] == "300.000");   // guest_us: 250 + 50
    CHECK(row[4] == "700.000");   // svc_us: the main thread's 400 and the other's 300
    CHECK(row[5] == "600.000");   // service_us: 200 and 400
    CHECK(row[9] == "400.000");   // main_svc_us: the main thread's only
    CHECK(row[10] == "200.000");  // main_service_us: the main thread's only
}

TEST_CASE("with --deterministic the log's header says clock=virtual (perf_report counts no such log)") {
    TempDir dir;
    Runtime runtime;  // its own clock: --deterministic replaces it with the virtual one
    const std::string csv = dir.File("perf.csv");
    runtime.SetMainThreadEntry([&](GuestThread&) { ExitProcess(runtime); });
    const Options options =
        ParseOptions("runner", {dir.WriteElf(), "--perf-log", csv, "--deterministic", "--ms", "500"});
    REQUIRE_MESSAGE(options.ok(), options.error());
    CHECK(runtime.Boot(options) == 0);
    const std::vector<std::string> lines = Lines(csv);
    REQUIRE(lines.size() >= 2);
    CHECK(lines[0].rfind("# perf v2 ", 0) == 0);
    CHECK(lines[0].find(" clock=virtual mode=deterministic") != std::string::npos);
    CHECK(lines[1] == perf::ColumnLine());
}

TEST_CASE("without --perf-log the step does nothing: no file, and the run is the same") {
    Fixture f;
    TempDir dir;
    const std::string csv = dir.File("perf.csv");
    u32 frames_seen = 0;
    f.runtime.OnFrame([&](u32, u64) { ++frames_seen; });
    const int status = BootWith(f, dir, {}, [&] {
        GuestMemory& memory = f.runtime.Memory();
        CHECK(IsSuccess(memory.Map(kFramePage, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        memory.Write<u32>(kFrameCounterAddress, 1);
        CallSvc(f.runtime, 0x23, {0});
        ExitProcess(f.runtime);
    });
    CHECK(status == 0);
    CHECK(frames_seen == 1);
    CHECK_FALSE(std::filesystem::exists(csv));
}

TEST_CASE("a log that cannot be written ends Boot with status 2 before the program runs") {
    Fixture f;
    TempDir dir;
    bool ran = false;
    CHECK(BootWith(f, dir, {"--perf-log", dir.File("no/such/dir/perf.csv")}, [&] { ran = true; }) == 2);
    CHECK_FALSE(ran);
}

#endif
