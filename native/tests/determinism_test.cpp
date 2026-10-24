// Derived from Azahar src/core/hle/kernel/svc.cpp, src/core/hle/kernel/address_arbiter.cpp and
// src/core/core_timing.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp says
// "GPLv3 or any later version"; address_arbiter.cpp and core_timing.h say "GPLv2 or any later version"):
// the cycles each system call costs (SVC_Table), the 1614 ticks of a Signal that wakes nobody and the
// length of Azahar's slice (BASE_CLOCK_RATE_ARM11 / 234), written out here as numbers, not computed from
// the code under test.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (address_arbiter.cpp)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
//
// The deterministic mode (N1-25): `--deterministic` and `--seed`. Three levels:
//   - the pieces (the virtual clock, the preemption source, the SVC cost table);
//   - the driver, on small C++ programs that run as guest threads and charge cycles the way the code
//     library's RC_TICK does (the budget is charged at the start of each block, and the code host's
//     budget callback runs when it goes below 0): the time is exactly the cycles charged, timers fire at
//     their tick, an idle clock skips to the next timer, two runs give the same log, a seed moves the
//     interleaving and nothing else;
//   - a real 3DS program (port/tests/svc_test.c), recompiled with --native --deterministic, through
//     Runtime::Boot: Azahar's output, the same traces twice, other seeds the same output.
// Nothing here depends on how fast the machine is: the clock is the cycle count.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("determinism_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/cpu/callbacks.h"
#include "../src/kernel/deterministic.h"
#include "../src/services/fs/fs_state.h"
#include "mh4n/clock.h"
#include "mh4n/cpu.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr u32 kText = 0x00100000;

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

// ---- a guest written in C++ ------------------------------------------------------------------------------

// What the code library does around a guest function: RC_TICK(n) at the start of every block
// (`budget -= n`, the code host's budget callback when it goes below 0) and an SVC instruction.
class Guest {
public:
    Guest(GuestThread& thread, s64 block) : thread_(thread), block_(block) {}

    // `cycles` of straight-line code in blocks of `block_` cycles.
    void Burn(s64 cycles) {
        RCtx& ctx = thread_.Context();
        while (cycles > 0) {
            const s64 n = std::min(cycles, block_);
            cycles -= n;
            if ((ctx.budget -= n) < 0) {
                CodeHostCallbacks::Budget(&ctx);
            }
        }
    }
    // One system call through the code host, as the recompiled code makes it: r0 and r1 in, r0 out.
    u32 Svc(u32 imm, u32 r0 = 0, u32 r1 = 0) {
        RCtx& ctx = thread_.Context();
        ctx.r[0] = r0;
        ctx.r[1] = r1;
        ctx.r[14] = kText + 0x20;
        CodeHostCallbacks::Svc(&ctx, imm, kText + 0x11);
        return ctx.r[0];
    }
    // svcGetSystemTick.
    u64 Tick() {
        Svc(0x28);
        const RCtx& ctx = thread_.Context();
        return static_cast<u64>(ctx.r[0]) | (static_cast<u64>(ctx.r[1]) << 32);
    }
    void SleepNs(u64 ns) { Svc(0x0A, static_cast<u32>(ns), static_cast<u32>(ns >> 32)); }

private:
    GuestThread& thread_;
    s64 block_;
};

// A runtime in the deterministic mode with no program: threads are made by hand.
struct Sim {
    Runtime runtime;
    std::atomic<int> finished{0};
    int started = 0;
    u32 next_id = 0;

    explicit Sim(std::vector<std::string> args = {}) {
        SetLogFilter(LogFilter(LogLevel::Error));
        args.insert(args.begin(), "--deterministic");
        std::string error;
        REQUIRE_MESSAGE(runtime.Prepare(Parse(args), {}, &error), error);
    }
    ~Sim() { SetLogFilter(LogFilter(LogLevel::Info)); }

    GuestScheduler& Sched() { return runtime.Scheduler(); }
    Kernel& GetKernel() { return runtime.GetKernel(); }
    u64 Now() { return runtime.Clock().Now(); }

    // A thread that runs `body` and then exits through svcExitThread (so its last cycles are counted).
    std::shared_ptr<Thread> Spawn(u32 priority, int core, std::function<void(Guest&)> body,
                                  s64 block = 250) {
        Kernel& kernel = GetKernel();
        auto thread = kernel.NewThread("t" + std::to_string(next_id++), priority, core,
                                       kernel.CurrentProcess());
        Sched().Attach(thread, [this, body = std::move(body), block](GuestThread& g) {
            Guest guest(g, block);
            body(guest);
            ++finished;
            guest.Svc(0x09);  // ExitThread
        });
        Sched().Start(thread);
        ++started;
        return thread;
    }
    RunStatus Run() {
        return Sched().Run([this] { return finished.load() == started; });
    }
};

constexpr u64 kStart = kDeterministicStartTicks;

}  // namespace

// ---- the pieces ----------------------------------------------------------------------------------------------

TEST_CASE("the virtual clock is the tick count, and a GameClock over it reads exactly that") {
    VirtualTimeSource source;
    GameClock clock(source);
    CHECK(clock.Now() == 0);
    // Steps of any size, among them ones whose nanoseconds are not whole: no tick is ever lost.
    u64 total = 0;
    for (const u64 step : std::initializer_list<u64>{1, 2, 3, 7, 340, 946, 1614, 5825, 268111, 1145774,
                                                     kStart, 1, 99999999}) {
        source.AdvanceTicks(step);
        total += step;
        CHECK(source.Ticks() == total);
        CHECK(clock.Now() == total);
    }
    // The nanoseconds the scheduler hands to the preemption source map back to the tick they came
    // from: the first tick at which the source reads them.
    for (const u64 ahead : std::initializer_list<u64>{1, 2, 3, 100, 1145774, kStart + 5, total}) {
        const std::optional<s64> ns = clock.HostNsFor(total + ahead);
        REQUIRE(ns.has_value());
        CHECK(VirtualTimeSource::TicksAtOrAfterNs(*ns) == total + ahead);
    }
    CHECK(VirtualTimeSource::TicksAtOrAfterNs(0) == 0);
    CHECK(VirtualTimeSource::TicksAtOrAfterNs(-5) == 0);
    for (const s64 ns : std::initializer_list<s64>{1, 3, 4, 5, 1000, 999999, 1000000000, 123456789012}) {
        const u64 tick = VirtualTimeSource::TicksAtOrAfterNs(ns);
        VirtualTimeSource probe(tick);
        CHECK(probe.NowNs() >= ns);
        VirtualTimeSource before(tick - 1);
        CHECK(before.NowNs() < ns);  // and no earlier tick does
    }
    // The clock never goes back.
    source.AdvanceToTick(5);
    CHECK(source.Ticks() == total);
    source.AdvanceToTick(total + 10);
    CHECK(source.Ticks() == total + 10);
}

TEST_CASE("waiting for a deadline skips the idle time; waiting for nothing waits for the predicate") {
    VirtualTimeSource source(1000);
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_lock lock(mutex);
    const s64 deadline = source.NowNs() + 5000000;  // 5 ms
    CHECK_FALSE(source.WaitUntil(cv, lock, deadline, [] { return false; }));
    CHECK(source.NowNs() >= deadline);
    CHECK(source.Ticks() == VirtualTimeSource::TicksAtOrAfterNs(deadline));
    const u64 there = source.Ticks();
    // A predicate that already holds returns at once, with no time passing.
    CHECK(source.WaitUntil(cv, lock, deadline + 5000000, [] { return true; }));
    CHECK(source.Ticks() == there);
    // A deadline in the past does not move it back.
    CHECK_FALSE(source.WaitUntil(cv, lock, 10, [] { return false; }));
    CHECK(source.Ticks() == there);
}

TEST_CASE("the preemption source keeps the deadline until a check has run, and raises for a past one") {
    VirtualTimeSource source;
    DeterministicPreemption preemption(source);
    u64 checks = 0;
    preemption.SetEpochSource([&] { return checks; });
    int raised = 0;
    preemption.Start([&] { ++raised; });
    CHECK_FALSE(preemption.Armed().has_value());

    preemption.Arm(1000000);
    CHECK(preemption.Armed() == std::optional<s64>(1000000));
    CHECK(raised == 0);
    preemption.Arm(std::nullopt);
    CHECK_FALSE(preemption.Armed().has_value());

    // A deadline that has passed is raised at once.
    source.AdvanceTicks(1000);
    preemption.Arm(source.NowNs());
    CHECK(raised == 1);
    // A Preempt() since the arming spends it: the scheduler forgets the request then too.
    preemption.Arm(source.NowNs() + 4000000);
    CHECK(preemption.Armed().has_value());
    ++checks;
    CHECK_FALSE(preemption.Armed().has_value());
    // Armed again after the check, it holds.
    preemption.Arm(source.NowNs() + 4000000);
    CHECK(preemption.Armed().has_value());
    preemption.Stop();
    preemption.Arm(source.NowNs());
    CHECK(raised == 1);  // nothing is raised after Stop
}

TEST_CASE("Azahar's cycle costs of the system calls the game makes, and 0 for what Azahar leaves out") {
    CHECK(SvcCycles(0x01) == 1000);   // ControlMemory
    CHECK(SvcCycles(0x08) == 5214);   // CreateThread
    CHECK(SvcCycles(0x0A) == 946);    // SleepThread
    CHECK(SvcCycles(0x0B) == 616);    // GetThreadPriority
    CHECK(SvcCycles(0x0C) == 1812);   // SetThreadPriority
    CHECK(SvcCycles(0x14) == 1324);   // ReleaseMutex
    CHECK(SvcCycles(0x16) == 2713);   // ReleaseSemaphore
    CHECK(SvcCycles(0x17) == 4329);   // CreateEvent
    CHECK(SvcCycles(0x18) == 3285);   // SignalEvent
    CHECK(SvcCycles(0x19) == 1389);   // ClearEvent
    CHECK(SvcCycles(0x1B) == 5163);   // SetTimer
    CHECK(SvcCycles(0x22) == 5664);   // ArbitrateAddress
    CHECK(SvcCycles(0x23) == 2937);   // CloseHandle
    CHECK(SvcCycles(0x24) == 4005);   // WaitSynchronization1
    CHECK(SvcCycles(0x25) == 6918);   // WaitSynchronizationN
    CHECK(SvcCycles(0x28) == 340);    // GetSystemTick
    CHECK(SvcCycles(0x2B) == 1510);   // GetProcessInfo
    CHECK(SvcCycles(0x32) == 5825);   // SendSyncRequest
    CHECK(SvcCycles(0x37) == 677);    // GetThreadId
    CHECK(SvcCycles(0x3D) == 1000);   // OutputDebugString
    CHECK(SvcCycles(0xB3) == 1000);   // ControlProcess
    CHECK(SvcCycles(0x2E) == 0);      // SendSyncRequest1: nullptr in Azahar's table
    CHECK(SvcCycles(0x26) == 0);      // SignalAndWait: nullptr
    CHECK(SvcCycles(0x00) == 0);
    CHECK(SvcCycles(0xFF) == 0);
    // Azahar's slice is 1,145,774 cycles; ours is the same to the microsecond.
    CHECK(kMaxQuantumTicks > 1145774);
    CHECK(NsToTicks(static_cast<u64>(kDeterministicSliceUs) * 1000) >= 1145774);
    CHECK(NsToTicks(static_cast<u64>(kDeterministicSliceUs - 1) * 1000) < 1145774);
}

// ---- the options ----------------------------------------------------------------------------------------------

TEST_CASE("--seed needs --deterministic, and --deterministic needs the runtime's own clock") {
    SetLogFilter(LogFilter(LogLevel::Error));
    {
        Runtime runtime;
        std::string error;
        CHECK_FALSE(runtime.Prepare(Parse({"--seed", "3"}), {}, &error));
        CHECK(error == "--seed needs --deterministic");
    }
    {
        // a bad number is refused like any other option value
        Runtime runtime;
        std::string error;
        CHECK_FALSE(runtime.Prepare(Parse({"--deterministic", "--seed", "three"}), {}, &error));
        CHECK(error.find("seed") != std::string::npos);
    }
    {
        RuntimeConfig config;
        config.time = std::make_unique<ManualTimeSource>();
        Runtime runtime(std::move(config));
        std::string error;
        CHECK_FALSE(runtime.Prepare(Parse({"--deterministic"}), {}, &error));
        CHECK(error.find("--deterministic needs the runtime's own clock") != std::string::npos);
    }
    {
        // without the option nothing changes: the host's clock, no driver
        Runtime runtime;
        std::string error;
        REQUIRE(runtime.Prepare(Parse({}), {}, &error));
        CHECK(runtime.Clock().Now() < kStart);
    }
    SetLogFilter(LogFilter(LogLevel::Info));
}

TEST_CASE("the clock starts at the reference's starting tick") {
    Sim sim;
    CHECK(sim.Now() == kStart);
    CHECK(kStart == 2000000000);  // port/README.md, the determinism table
}

// ---- the driver ------------------------------------------------------------------------------------------------

TEST_CASE("the time is the cycles charged, the SVC costs and nothing else") {
    Sim sim;
    u64 first = 0;
    u64 second = 0;
    u64 third = 0;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        first = g.Tick();      // 340 cycles of GetSystemTick, charged before the call returns the time
        g.Burn(1000000);       // 4000 blocks of 250
        second = g.Tick();
        g.Burn(12345);         // the last block is short: 95 cycles
        third = g.Tick();
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(first == kStart + 340);
    CHECK(second == first + 1000000 + 340);
    CHECK(third == second + 12345 + 340);
    // ExitThread costs its 1000 cycles too; the clock is the sum of everything the thread charged.
    CHECK(sim.Now() == third + 1000);
}

TEST_CASE("a timer fires at its tick, at the block that crosses it") {
    Sim sim;
    std::vector<u64> fired;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            Kernel& kernel = sim.GetKernel();
            const u64 base = kernel.NowTicks();
            for (const u64 offset : {50000ull, 50001ull, 123456ull, 3000000ull}) {
                kernel.ScheduleAt(base + offset, [&] { fired.push_back(kernel.NowTicks() - base); });
            }
            g.Tick();  // an SVC: the thread is given a budget that ends at the first timer
            g.Burn(4000000);
        },
        /*block=*/1000);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    REQUIRE(fired.size() == 4);
    const std::array<u64, 4> due = {50000, 50001, 123456, 3000000};
    for (std::size_t i = 0; i < fired.size(); ++i) {
        INFO("timer " << i);
        // never early; late by less than one block (1000 cycles), the cost of the block that crossed it
        // included: the check runs when the block that runs the budget out is about to execute
        CHECK(fired[i] >= due[i]);
        CHECK(fired[i] <= due[i] + 1000 + 340);
    }
}

TEST_CASE("an idle clock skips to the next timer: a sleep takes exactly its ticks") {
    Sim sim;
    u64 before = 0;
    u64 after = 0;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        before = g.Tick();
        g.SleepNs(1000000);  // 1 ms = 268,111 ticks
        after = g.Tick();
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    // SleepThread's 946 cycles, then the wait (the timer is set at the time the call sees), then
    // GetSystemTick's 340.
    CHECK(after - before == 946 + 268111 + 340);
}

TEST_CASE("an arbiter Signal that wakes nobody costs 1614 ticks from priority 49 down, and no less") {
    Sim sim;
    u64 low = 0;
    u64 high = 0;
    auto arbiter = sim.GetKernel().NewAddressArbiter();
    // Priority 0x31 = 49: charged. 0x30 = 48: not.
    sim.Spawn(0x31, 0, [&](Guest& g) {
        g.Tick();
        const std::shared_ptr<Thread> self = sim.Sched().CurrentThreadPtr();
        const u64 t0 = sim.Now();
        arbiter->ArbitrateAddress(self, ArbitrationType::Signal, 0x08000000, 1, 0);
        low = sim.Now() - t0;
    });
    sim.Spawn(0x30, 1, [&](Guest& g) {
        g.Tick();
        const std::shared_ptr<Thread> self = sim.Sched().CurrentThreadPtr();
        const u64 t0 = sim.Now();
        arbiter->ArbitrateAddress(self, ArbitrationType::Signal, 0x08000000, 1, 0);
        high = sim.Now() - t0;
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(low == 1614);
    CHECK(high == 0);
}

namespace {

// Two threads of the same priority, one on each core, that never block: they take turns in slices.
// Each iteration costs 100,000 cycles and is logged when it is done.
struct TakingTurns {
    std::vector<std::string> log;
    u64 end_tick = 0;
    std::vector<u32> slice_lengths_us;
};

TakingTurns RunTakingTurns(std::vector<std::string> args) {
    Sim sim(std::move(args));
    TakingTurns result;
    constexpr int kIterations = 60;
    for (const char* name : {"a", "b"}) {
        sim.Spawn(0x30, name[0] == 'a' ? 0 : 1, [&, name](Guest& g) {
            for (int i = 0; i < kIterations; ++i) {
                g.Burn(100000);
                result.log.push_back(std::string(name) + std::to_string(i));
            }
        });
    }
    REQUIRE(sim.Run() == RunStatus::Stopped);
    result.end_tick = sim.Now();
    return result;
}

// Who ran how long in a row: "a:11 b:11 a:11 ..." (the order of the log, in runs).
std::string Runs(const std::vector<std::string>& log) {
    std::ostringstream out;
    char current = 0;
    int length = 0;
    for (const std::string& entry : log) {
        if (entry[0] != current) {
            if (length > 0) {
                out << current << ':' << length << ' ';
            }
            current = entry[0];
            length = 0;
        }
        ++length;
    }
    out << current << ':' << length;
    return out.str();
}

}  // namespace

TEST_CASE("two runs give the same log and the same end tick; the virtual cores take turns in slices") {
    const TakingTurns one = RunTakingTurns({});
    const TakingTurns two = RunTakingTurns({});
    CHECK(one.log == two.log);
    CHECK(one.end_tick == two.end_tick);
    REQUIRE(one.log.size() == 120);
    // 120 iterations of 100,000 cycles and the two ExitThread calls: the clock counted all of them.
    CHECK(one.end_tick == kStart + 120 * 100000 + 2 * 1000);
    // The slice is 4274 us = about 1.146 million cycles, so about 11 iterations in a row, not 60.
    const std::string runs = Runs(one.log);
    CAPTURE(runs);
    CHECK(runs.find("a:11") != std::string::npos);
    CHECK(runs.find("b:11") != std::string::npos);
    CHECK(std::count(runs.begin(), runs.end(), ' ') >= 8);
}

TEST_CASE("a seed moves the interleaving, repeatably, and changes nothing else") {
    const TakingTurns plain = RunTakingTurns({});
    std::map<u64, TakingTurns> seeded;
    for (const u64 seed : {1ull, 2ull, 3ull, 4ull, 5ull}) {
        seeded[seed] = RunTakingTurns({"--seed", std::to_string(seed)});
    }
    std::vector<std::string> shapes;
    shapes.push_back(Runs(plain.log));
    for (const auto& [seed, run] : seeded) {
        INFO("seed " << seed);
        // the results: every thread ran its 60 iterations in order, and the clock counted the same cycles
        REQUIRE(run.log.size() == plain.log.size());
        for (const char* name : {"a", "b"}) {
            std::vector<std::string> own;
            for (const std::string& entry : run.log) {
                if (entry[0] == name[0]) {
                    own.push_back(entry);
                }
            }
            REQUIRE(own.size() == 60);
            for (std::size_t i = 0; i < own.size(); ++i) {
                CHECK(own[i] == std::string(name) + std::to_string(i));
            }
        }
        CHECK(run.end_tick == plain.end_tick);
        shapes.push_back(Runs(run.log));
        // the same seed again: the same run, to the entry
        const TakingTurns again = RunTakingTurns({"--seed", std::to_string(seed)});
        CHECK(again.log == run.log);
        CHECK(again.end_tick == run.end_tick);
    }
    // ...but the points where the cores take turns are not the unseeded ones, and the seeds differ.
    std::sort(shapes.begin(), shapes.end());
    shapes.erase(std::unique(shapes.begin(), shapes.end()), shapes.end());
    CHECK(shapes.size() >= 5);  // the unseeded run and five seeds: at most one pair may coincide
}

TEST_CASE("a seeded slice is drawn between half and one and a half times the default") {
    // CycleDriver draws from splitmix64; the bounds hold for every draw and both ends are reachable.
    Sim sim({"--seed", "7", "--slice-us", "1000"});
    std::vector<u32> lengths;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        for (int i = 0; i < 400; ++i) {
            g.Tick();  // each system call draws a slice
            lengths.push_back(sim.Sched().SliceUs());
        }
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    REQUIRE(lengths.size() == 400);
    CHECK(*std::min_element(lengths.begin(), lengths.end()) >= 500);
    CHECK(*std::max_element(lengths.begin(), lengths.end()) <= 1500);
    std::sort(lengths.begin(), lengths.end());
    CHECK(lengths.front() < 600);   // 400 draws from a uniform 501 values reach the low end
    CHECK(lengths.back() > 1400);   // and the high end
    CHECK(std::unique(lengths.begin(), lengths.end()) - lengths.begin() > 200);
}

TEST_CASE("work posted by the scheduler's own services lands at the same tick in every run") {
    // Post from the guest side (a service handler's deferred reply) is applied at the next safe point,
    // which in this mode is an SVC end or the end of a budget: the same cycle every time.
    std::vector<u64> ticks;
    for (int run = 0; run < 2; ++run) {
        Sim sim;
        u64 landed = 0;
        sim.Spawn(0x30, 0, [&](Guest& g) {
            sim.GetKernel().Post([&] { landed = sim.Now(); });
            g.Tick();
        });
        REQUIRE(sim.Run() == RunStatus::Stopped);
        CHECK(landed != 0);
        ticks.push_back(landed);
    }
    CHECK(ticks[0] == ticks[1]);
}

// ---- the file system answers on the scheduler thread -------------------------------------------------------

namespace {

struct FsHarness {
    explicit FsHarness(std::vector<std::string> args) {
        SetLogFilter(LogFilter(LogLevel::Error));
        std::string error;
        REQUIRE_MESSAGE(runtime.Prepare(Parse(args), {}, &error), error);
        ServiceEnvironment env;
        env.options = &runtime.GetOptions();
        env.kernel = &runtime.GetKernel();
        env.scheduler = &runtime.Scheduler();
        env.threads = &runtime.Threads();
        env.interfaces = &runtime.Interfaces();
        context = std::make_unique<ServiceContext>(env);
        state = fs::GetFsState(*context);
    }
    ~FsHarness() {
        context.reset();
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    Runtime runtime;
    std::unique_ptr<ServiceContext> context;
    std::shared_ptr<fs::FsState> state;
};

}  // namespace

TEST_CASE("with --deterministic the work of a file operation runs inside StartIo, the reply at its tick") {
    FsHarness h({"--deterministic"});
    REQUIRE(h.state->synchronous);
    std::thread::id worked_on;
    bool worked = false;
    u64 finished_at = 0;
    bool finished = false;
    const u64 started = h.runtime.Clock().Now();
    constexpr u64 kDelayNs = 663124;  // Azahar's smallest RomFS read delay
    fs::StartIo(
        *h.state, *h.context, kDelayNs,
        [&] {
            worked = true;
            worked_on = std::this_thread::get_id();
        },
        [&] {
            finished = true;
            finished_at = h.runtime.Clock().Now();
        });
    CHECK(worked);  // done before StartIo returned...
    CHECK(worked_on == std::this_thread::get_id());  // ...on this thread
    CHECK_FALSE(finished);  // the reply waits for its tick
    REQUIRE(h.runtime.Scheduler().Run([&] { return finished; }) == RunStatus::Stopped);
    CHECK(finished_at == started + NsToTicks(kDelayNs));  // exactly: the clock skipped the idle time
    CHECK(h.state->io_in_flight.load() == 0);
}

TEST_CASE("without --deterministic the work runs on an I/O thread, as before") {
    FsHarness h({});
    CHECK_FALSE(h.state->synchronous);
    std::thread::id worked_on;
    bool finished = false;
    fs::StartIo(
        *h.state, *h.context, /*delay_ns=*/0, [&] { worked_on = std::this_thread::get_id(); },
        [&] { finished = true; });
    REQUIRE(h.runtime.Scheduler().Run([&] { return finished; }) == RunStatus::Stopped);
    CHECK(worked_on != std::this_thread::get_id());
}

TEST_CASE("--io-latency none in the deterministic mode answers at once, in the same call") {
    FsHarness h({"--deterministic", "--io-latency", "none"});
    bool finished = false;
    fs::StartIo(*h.state, *h.context, 663124, [] {}, [&] { finished = true; });
    CHECK(finished);
}

// ---- a real program ----------------------------------------------------------------------------------------------

namespace {

std::string DataDir() {
#ifdef MH4N_DETERMINISM_TEST_DATA_DIR
    const std::string dir = MH4N_DETERMINISM_TEST_DATA_DIR;
    if (std::filesystem::exists(dir + "/svc_test_det.so")) {
        return dir;
    }
#endif
    return {};
}

bool HaveData() {
    if (!DataDir().empty()) {
        return true;
    }
    const char* require = std::getenv("MH4N_REQUIRE_TEST_DATA");
    if (require != nullptr && std::string(require) == "1") {
        FAIL("the deterministic mode's test data was not built, and MH4N_REQUIRE_TEST_DATA=1");
    }
    WARN_MESSAGE(false, "skipped: the deterministic mode's test data was not built");
    return false;
}

std::vector<std::string> ReadLines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

struct ProgramRun {
    int status = -1;
    std::vector<std::string> output;  // the program's OutputDebugString lines
    std::vector<std::string> svc;     // the SVC trace without its header line
    std::vector<std::string> log;     // what the runtime logged
};

ProgramRun RunSvcTest(const std::vector<std::string>& extra, const std::string& tag) {
    const std::string dir = DataDir();
    const auto scratch = std::filesystem::temp_directory_path() / ("mh4n-determinism-test-" + tag);
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    const std::string trace = (scratch / "svc.txt").string();
    ProgramRun run;
    SetLogFilter(LogFilter(LogLevel::Info));
    SetLogSink([&](const LogRecord& r) { run.log.push_back(std::string(r.message)); });
    {
        Runtime runtime;
        SvcHooks hooks;
        hooks.debug_string = [&](std::string_view text) { run.output.emplace_back(text); };
        runtime.AddSvcHooks(std::move(hooks));
        std::vector<std::string> args = {dir + "/svc_test.elf", "--code", dir + "/svc_test_det.so", "--ms",
                                         "5000", "--user", (scratch / "user").string(), "--trace-svc", trace};
        args.insert(args.end(), extra.begin(), extra.end());
        run.status = runtime.Boot(Parse(args));
    }
    SetLogSink({});
    run.svc = ReadLines(trace);
    if (!run.svc.empty()) {
        run.svc.erase(run.svc.begin());  // the header carries the date
    }
    std::filesystem::remove_all(scratch);
    return run;
}

std::vector<std::string> ExpectedOutput() {
#ifdef MH4N_DETERMINISM_EXPECTED_DIR
    return ReadLines(std::string(MH4N_DETERMINISM_EXPECTED_DIR) + "/svc_test.txt");
#else
    return {};
#endif
}

}  // namespace

TEST_CASE("svc_test, recompiled with --native --deterministic, gives Azahar's output and the same trace twice") {
    if (!HaveData()) {
        return;
    }
    const std::vector<std::string> expected = ExpectedOutput();
    REQUIRE(expected.size() > 10);
    const ProgramRun one = RunSvcTest({"--deterministic"}, "one");
    const ProgramRun two = RunSvcTest({"--deterministic"}, "two");
    CHECK(one.status == 0);
    CHECK(two.status == 0);
    CHECK(one.output == expected);
    CHECK(two.output == expected);
    REQUIRE(one.svc.size() > 50);
    // The whole SVC trace, with its ticks, is the same: thread by thread, call by call.
    CHECK(one.svc == two.svc);
}

TEST_CASE("svc_test with seeds: the same output, repeatable, and the interleaving may move") {
    if (!HaveData()) {
        return;
    }
    const std::vector<std::string> expected = ExpectedOutput();
    for (const std::string seed : {"1", "2", "3"}) {
        INFO("seed " << seed);
        const ProgramRun one = RunSvcTest({"--deterministic", "--seed", seed}, "seed-one");
        const ProgramRun two = RunSvcTest({"--deterministic", "--seed", seed}, "seed-two");
        CHECK(one.status == 0);
        CHECK(one.output == expected);
        CHECK(one.svc == two.svc);
    }
}

TEST_CASE("--deterministic needs the library built for it, and the reverse") {
#if defined(MH4N_CPU_TEST_DATA_DIR)
    const std::string dir = MH4N_CPU_TEST_DATA_DIR;
    if (!std::filesystem::exists(dir + "/codehost_native_det.so")) {
        WARN_MESSAGE(false, "skipped: the code host's test data was not built");
        return;
    }
    struct Case {
        const char* library;
        bool deterministic;
        int status;
        const char* message;
    };
    const Case cases[] = {
        {"codehost_native_det.so", true, 0, ""},
        {"codehost_native.so", true, 2, "not a deterministic build"},
        {"codehost_native_det.so", false, 2, "deterministic build"},
        {"codehost_native.so", false, 0, ""},
    };
    for (const Case& c : cases) {
        INFO(c.library << (c.deterministic ? " with --deterministic" : " without"));
        std::vector<std::string> log;
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([&](const LogRecord& r) { log.push_back(std::string(r.message)); });
        int status = -1;
        std::vector<std::string> output;
        {
            Runtime runtime;
            SvcHooks hooks;
            hooks.debug_string = [&](std::string_view text) { output.emplace_back(text); };
            runtime.AddSvcHooks(std::move(hooks));
            std::vector<std::string> args = {dir + "/codehost_test.elf", "--code", dir + "/" + c.library,
                                             "--ms", "2000"};
            if (c.deterministic) {
                args.push_back("--deterministic");
            }
            status = runtime.Boot(Parse(args));
        }
        SetLogSink({});
        CHECK(status == c.status);
        bool found = std::string(c.message).empty();
        for (const std::string& line : log) {
            found = found || line.find(c.message) != std::string::npos;
        }
        CHECK(found);
        if (c.status == 0) {
            // the program computes the same values whichever way its time is kept
            CHECK(output.size() >= 5);
        }
    }
#else
    WARN_MESSAGE(false, "skipped: the code host's test data is not configured");
#endif
}

TEST_CASE("the deterministic mode gives a user folder without one the reference's MAC address, and keeps another") {
#if defined(MH4N_CPU_TEST_DATA_DIR)
    const std::string dir = MH4N_CPU_TEST_DATA_DIR;
    if (!std::filesystem::exists(dir + "/codehost_native_det.so")) {
        WARN_MESSAGE(false, "skipped: the code host's test data was not built");
        return;
    }
    SetLogFilter(LogFilter(LogLevel::Error));
    const auto scratch = std::filesystem::temp_directory_path() / "mh4n-determinism-test-mac";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "fresh");
    std::filesystem::create_directories(scratch / "kept" / "sysdata");
    std::ofstream(scratch / "kept" / "sysdata" / "mac.txt") << "18:2A:7B:00:00:01";
    for (const char* name : {"fresh", "kept"}) {
        Runtime runtime;
        const int status = runtime.Boot(Parse({dir + "/codehost_test.elf", "--code", dir + "/codehost_native_det.so",
                                               "--ms", "2000", "--deterministic", "--user",
                                               (scratch / name).string()}));
        CHECK(status == 0);
    }
    CHECK(ReadLines((scratch / "fresh" / "sysdata" / "mac.txt").string()) ==
          std::vector<std::string>{"02:00:00:00:00:01"});
    CHECK(ReadLines((scratch / "kept" / "sysdata" / "mac.txt").string()) ==
          std::vector<std::string>{"18:2A:7B:00:00:01"});
    std::filesystem::remove_all(scratch);
    SetLogFilter(LogFilter(LogLevel::Info));
#else
    WARN_MESSAGE(false, "skipped: the code host's test data is not configured");
#endif
}

#endif  // recomp.h
