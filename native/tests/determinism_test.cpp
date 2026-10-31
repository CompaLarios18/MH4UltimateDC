// Derived from Azahar src/core/hle/kernel/svc.cpp, src/core/hle/kernel/address_arbiter.cpp,
// src/core/hle/kernel/thread.cpp, src/core/core_timing.h and src/core/core.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp and core.cpp say "GPLv3 or any later
// version"; address_arbiter.cpp, thread.cpp and core_timing.h say "GPLv2 or any later version"):
// the cycles each system call costs (SVC_Table), the 1614 ticks of a Signal that wakes nobody, the
// length of Azahar's slice (BASE_CLOCK_RATE_ARM11 / 234), written out here as numbers, not computed from
// the code under test; and the rules of RunLoop (the 100-tick tolerance between the cores, the reschedule
// after a stretch, the catch-up of the core that is behind) and of SwitchContext (a displaced thread goes
// back to the front of its queue).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp, core.cpp, thread.cpp)
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
#include <set>
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
                if (after_budget) {
                    after_budget();  // the thread has been given a new stretch (its clock is synced, nothing used)
                }
            }
        }
    }
    // Called by Burn each time the budget ran out and the thread has its next stretch.
    std::function<void()> after_budget;
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
    // The two cores' clocks (the one a thread reads is its own core's), and the later of them.
    VirtualTimeSource& Time() { return static_cast<VirtualTimeSource&>(runtime.Clock().Source()); }
    u64 CoreNow(int core) { return Time().CoreTicks(core); }
    u64 Global() { return Time().GlobalTicks(); }

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

// Azahar's MAX_SLICE_LENGTH (core_timing.h): BASE_CLOCK_RATE_ARM11 / 234, an int, so 268,111,856 / 234 rounded
// down. Written out, not taken from the code under test (N3-32: the code had 1,145,774).
constexpr u64 kAzaharSlice = 1145777;

}  // namespace

// ---- the pieces ----------------------------------------------------------------------------------------------

TEST_CASE("the virtual clock is the tick count, and a GameClock over it reads exactly that") {
    VirtualTimeSource source;
    GameClock clock(source);
    CHECK(clock.Now() == 0);
    // Steps of any size, among them ones whose nanoseconds are not whole: no tick is ever lost.
    u64 total = 0;
    for (const u64 step : std::initializer_list<u64>{1, 2, 3, 7, 340, 946, 1614, 5825, 268111, kAzaharSlice,
                                                     kStart, 1, 99999999}) {
        source.AdvanceTicks(step);
        total += step;
        CHECK(source.Ticks() == total);
        CHECK(clock.Now() == total);
    }
    // The nanoseconds the scheduler hands to the preemption source map back to the tick they came
    // from: the first tick at which the source reads them.
    for (const u64 ahead : std::initializer_list<u64>{1, 2, 3, 100, kAzaharSlice, kStart + 5, total}) {
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

TEST_CASE("with the lockstep scheduler an idle wait takes both cores' clocks to the deadline") {
    // Every core is idle: Azahar's RunLoop idles each core for the slice, so both clocks are at the next event.
    VirtualTimeSource source(1000);
    source.Lockstep().enabled = true;
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_lock lock(mutex);
    const s64 deadline = source.NowNs() + 5000000;
    CHECK_FALSE(source.WaitUntil(cv, lock, deadline, [] { return false; }));
    const u64 tick = VirtualTimeSource::TicksAtOrAfterNs(deadline);
    CHECK(source.CoreTicks(0) == tick);
    CHECK(source.CoreTicks(1) == tick);
    CHECK(source.GlobalTicks() == tick);
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
    // Azahar's slice is 1,145,777 cycles; ours is the same, and the same to the microsecond in `--slice-us`.
    CHECK(kDeterministicSliceTicks == kAzaharSlice);
    CHECK(kMaxQuantumTicks > kAzaharSlice);
    CHECK(NsToTicks(static_cast<u64>(kDeterministicSliceUs) * 1000) >= kAzaharSlice);
    CHECK(NsToTicks(static_cast<u64>(kDeterministicSliceUs - 1) * 1000) < kAzaharSlice);
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
    u64 after_exit = 0;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        first = g.Tick();      // 340 cycles of GetSystemTick, charged before the call returns the time
        g.Burn(1000000);       // 4000 blocks of 250
        second = g.Tick();
        g.Burn(12345);         // the last block is short: 95 cycles
        third = g.Tick();
    });
    // A thread of a worse priority on the same core: it starts when the first one has gone, so what it reads is the
    // core's clock at that moment (after the exit's own cost, and nothing else: the clocks do not run on in between).
    sim.Spawn(0x31, 0, [&](Guest& g) { after_exit = g.Tick(); });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(first == kStart + 340);
    CHECK(second == first + 1000000 + 340);
    CHECK(third == second + 12345 + 340);
    CHECK(after_exit == third + 1000 + 340);  // ExitThread's 1000 (Azahar's guess for it), then GetSystemTick's 340
    CHECK(SvcCycles(0x09) == 1000);
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

// ---- the two cores of Azahar's RunLoop -----------------------------------------------------------------------

TEST_CASE("a thread on core 1 does not make the thread on core 0 wait for it: each core has its own clock") {
    Sim sim;
    u64 before = 0;
    u64 after = 0;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            before = g.Tick();
            g.Burn(1000000);
            after = g.Tick();
        },
        /*block=*/20);
    sim.Spawn(0x30, 1, [&](Guest& g) { g.Burn(1000000); }, /*block=*/20);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    // exactly its own cycles and the two calls, as if it were alone (one clock for both cores: twice that)
    CHECK(after - before == 1000000 + 340);
}

TEST_CASE("a call that costs more than what is left of its core's slice puts that core ahead of the other") {
    Sim sim;
    u64 traced = 0;
    u64 core0 = 0;
    u64 core1 = 0;
    u64 seen_by_thread = 0;
    sim.Spawn(0x30, 0, [&](Guest& g) { g.SleepNs(1000000); });  // 946 cycles, then it waits
    sim.Spawn(0x30, 1, [&](Guest& g) {
        // Core 0 ran for 946 ticks, so this stretch is 946 ticks long; SetThreadPriority's 1812 go over it (the
        // handle is no handle: the call fails before it asks for a reschedule, which is of no interest here: the
        // cost is charged before the call runs).
        g.Svc(0x0C, 0x1234, 0x30);
        core0 = sim.CoreNow(0);
        core1 = sim.CoreNow(1);
        seen_by_thread = sim.Now();
        traced = kernel_detail::TraceTicks(seen_by_thread);
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(core0 == kStart + 946);
    CHECK(core1 == kStart + 1812);
    CHECK(seen_by_thread == core1);  // the thread reads its own core's clock...
    CHECK(traced == core1);          // ...and the trace the later one: Azahar's GetGlobalTicks
}

TEST_CASE("the core that is behind runs alone until it has caught up, without a thread: the idle time") {
    Sim sim;
    u64 woke_at = 0;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        g.SleepNs(1000000);  // core 0: 946 cycles, then it waits for 268,111 ticks
        woke_at = g.Tick();
    });
    sim.Spawn(0x30, 1, [&](Guest& g) { g.Svc(0x0C, 0x1234, 0x30); });  // core 1 is 866 ticks ahead
    REQUIRE(sim.Run() == RunStatus::Stopped);
    // Core 0 sleeps from its own clock (946 ticks in), whatever core 1 did: the wake-up is 268,111 ticks after it
    // asked, and GetSystemTick's 340 are after that.
    CHECK(woke_at == kStart + 946 + 268111 + 340);
}

TEST_CASE("an event scheduled for the other core waits in that core's queue until its next Advance") {
    Sim sim;
    u64 due = 0;
    u64 fired_at = 0;
    int fired_core = -1;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            g.Tick();
            due = sim.CoreNow(1) + 5000;
            kernel_detail::ScheduleOnCore(1);
            sim.GetKernel().ScheduleAt(due, [&] {
                fired_at = sim.Now();
                fired_core = sim.Time().Core();
            });
            g.Burn(300000);
            g.Tick();
        },
        /*block=*/20);
    REQUIRE(sim.Sched().Run([&] { return fired_core >= 0; }) == RunStatus::Stopped);
    CHECK(fired_core == 1);
    // It did not run at its tick inside the stretch of core 0 (it is not in core 1's queue yet, and does not limit
    // the stretch): core 1 got to it when its own clock moved, after that stretch.
    CHECK(fired_at >= due);
    CHECK(fired_at - due > 100000);
}

TEST_CASE("the thread-safe event of ScheduleEvent runs on its core two slices later, and exactly then") {
    Sim sim;
    u64 asked = 0;
    u64 fired_at = 0;
    int fired_core = -1;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        g.Tick();
        asked = sim.CoreNow(1);
        REQUIRE(kernel_detail::ScheduleThreadSafe(1, [&] {
            fired_at = sim.Now();
            fired_core = sim.Time().Core();
        }));
        g.SleepNs(20000000);  // the clocks run on with nothing to do: 20 ms, far past the event
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(fired_core == 1);
    CHECK(fired_at == asked + 2 * kAzaharSlice);  // MAX_SLICE_LENGTH * 2 after core 1's clock, to the tick
}

TEST_CASE("a thread-safe event asked for from core 0 is due 2,291,554 ticks after core 1's clock, as the reference's (N3-32)") {
    // The `nwm` asynchronous signal of the reference runner: a thread of core 0 makes the request, and
    // ScheduleEvent(0, ..., core 1, thread_safe_mode) puts the event at core 1's GetTicks() + MAX_SLICE_LENGTH * 2.
    // With a slice 3 ticks short the event came 6 ticks early, and at 120 fps in G3 the stretch it ends then moved
    // core 0's timeline for good. The tick it is due at is looked up in core 1's queue once it is there.
    Sim sim;
    u64 asked = 0;
    u64 queued_at = 0;
    bool fired = false;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            g.Tick();
            asked = sim.CoreNow(1);
            REQUIRE(kernel_detail::ScheduleThreadSafe(1, [&] { fired = true; }));
            g.after_budget = [&] {
                for (const auto& entry : sim.Time().Lockstep().events[1]) {
                    queued_at = std::max(queued_at, std::get<0>(entry));
                }
            };
            g.Burn(3 * kAzaharSlice);  // core 1 takes the event into its queue at its next Advance, then runs it
        },
        /*block=*/20);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(queued_at == asked + 2291554);
    CHECK(fired);
}

TEST_CASE("an event due inside the stretch that is running ends the stretch at that tick (ForceExceptionCheck)") {
    Sim sim;
    u64 fired_at = 0;
    u64 due = 0;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            g.Tick();
            due = sim.Now() + 123456;
            sim.GetKernel().ScheduleAt(due, [&] { fired_at = sim.Now(); });
            g.Tick();         // a system call: its end gives the thread what is left of the (cut) stretch
            g.Burn(2000000);  // without the cut the stretch would run on to the end of its slice
        },
        /*block=*/20);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(fired_at >= due);
    CHECK(fired_at - due < 20);  // the block that crossed it, not a slice later
}

namespace {

// What the thread of core 0 sees when it is given its next stretch after core 1 has gone ahead of it.
struct Behind {
    bool seen = false;
    u64 c0 = 0;           // the two cores' clocks at that moment
    u64 c1 = 0;
    u64 stretch_end = 0;  // the end of the stretch it was given
    u64 traced = 0;       // `t` of the system call (svcGetSystemTick) it makes at once
};

// Core 0's stretch is cut short by an event, 980 ticks into it (320 ticks of calls, 620 to the event, and the 20 of
// the block that crosses it), so core 1's is 980 ticks long too. Core 1's thread makes the system call `imm` there,
// which costs more than that and puts core 1 ahead. Core 0's thread is then run again by the next RunLoop call
// (RunLoop's catch-up branch if core 1 is more than 100 ticks ahead), and reads what it was given. After the call,
// core 1's thread charges `extra` more ticks in one block (the block that yields), so core 1 is `20 + extra` ticks
// ahead for a call of 1000 ticks.
Behind CoreBehind(u32 imm, u32 r0, u32 r1, s64 extra = 1) {
    Sim sim;
    Behind result;
    u64 traced = 0;
    SvcHooks hooks;
    hooks.before = [&](const SvcCall& call) {
        if (call.number == 0x28) {
            traced = call.ticks;
        }
    };
    sim.runtime.AddSvcHooks(std::move(hooks));
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            g.after_budget = [&] {
                if (result.seen) {
                    return;
                }
                result.seen = true;
                result.c0 = sim.CoreNow(0);
                result.c1 = sim.CoreNow(1);
                result.stretch_end = sim.Time().Lockstep().stretch_end;
                g.Tick();
                result.traced = traced;
            };
            g.Tick();
            sim.GetKernel().ScheduleAt(sim.Now() + 620, [] {});
            g.Tick();  // (a system call: its end gives the thread what is left of the cut stretch)
            g.Burn(20000);
        },
        /*block=*/20);
    sim.Spawn(
        0x30, 1,
        [&](Guest& g) {
            g.Svc(imm, r0, r1);
            g.Burn(extra);  // the stretch is over (the call went past it): the thread yields at its next block
        },
        /*block=*/extra);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    REQUIRE(result.seen);
    return result;
}

}  // namespace

TEST_CASE("a core that is more than 100 ticks behind catches up first, for exactly the difference") {
    // SetThreadPriority's 1812 go well past core 1's stretch of 980: it is ahead by 833 ticks. RunLoop then runs
    // the core that is behind alone, for the difference (SetNextSlice(delay)), before the cores go on together.
    const Behind b = CoreBehind(0x0C, 0x1234, 0x30);
    CHECK(b.c1 == kStart + 1813);
    CHECK(b.c0 == kStart + 980);
    CHECK(b.stretch_end == b.c1);  // the stretch it was given ends where core 1 is: the other core is not waited for
}

TEST_CASE("a core that is no more than 100 ticks behind is not waited for: both go on at once, for a whole slice") {
    // ControlMemory's 1000 put core 1 21 ticks ahead, below RunLoop's tolerance (the jit overshoots by a few ticks):
    // the cores go on side by side and core 0 gets the longest stretch.
    const Behind b = CoreBehind(0x35, 0, 0x1234);
    CHECK(b.c0 == kStart + 980);
    CHECK(b.c1 == kStart + 1001);
    CHECK(b.stretch_end == b.c0 + kAzaharSlice);
}

TEST_CASE("RunLoop's tolerance is exact: 100 ticks behind is not waited for, 101 is") {
    // ControlMemory's 1000 ticks put core 1 at 1000 + extra, core 0 is at 980: the difference is 20 + extra. The
    // test of Azahar's core.cpp is `max_delay > min_delay` with min_delay = 100.
    const Behind at_100 = CoreBehind(0x35, 0, 0x1234, 80);
    REQUIRE(at_100.c1 - at_100.c0 == 100);
    CHECK(at_100.stretch_end == at_100.c0 + kAzaharSlice);  // both cores go on side by side, for a whole slice
    const Behind at_101 = CoreBehind(0x35, 0, 0x1234, 81);
    REQUIRE(at_101.c1 - at_101.c0 == 101);
    CHECK(at_101.stretch_end == at_101.c1);  // core 0 runs alone for the 101 ticks of the difference
    // (and the cases of 99 and 102 sit on the same sides)
    const Behind at_99 = CoreBehind(0x35, 0, 0x1234, 79);
    REQUIRE(at_99.c1 - at_99.c0 == 99);
    CHECK(at_99.stretch_end == at_99.c0 + kAzaharSlice);
    const Behind at_102 = CoreBehind(0x35, 0, 0x1234, 82);
    REQUIRE(at_102.c1 - at_102.c0 == 102);
    CHECK(at_102.stretch_end == at_102.c1);
}

TEST_CASE("the trace's t is the later of the two clocks, also for a thread on the core that is behind") {
    const Behind b = CoreBehind(0x0C, 0x1234, 0x30);
    REQUIRE(b.c0 + 100 < b.c1);
    CHECK(b.traced == b.c1);  // not core 0's own clock, the one the thread reads, which is behind
    CHECK(b.traced != b.c0);
}

TEST_CASE("thread-safe events enter their core's queue after what it holds, in the order they were made") {
    // Azahar's ts_queue is moved into the event queue at the core's next Advance, each taking the next fifo number then:
    // three events of the same tick on the running core, two thread-safe ones made first and a direct one after them,
    // run as the direct one, then the other two in order.
    Sim sim;
    std::vector<int> order;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        g.Tick();
        const u64 due = sim.CoreNow(0) + 2 * kAzaharSlice;
        REQUIRE(kernel_detail::ScheduleThreadSafe(0, [&] { order.push_back(1); }));
        REQUIRE(kernel_detail::ScheduleThreadSafe(0, [&] { order.push_back(2); }));
        sim.GetKernel().ScheduleAt(due, [&] { order.push_back(3); });
        g.SleepNs(20000000);  // far past all three
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(order == std::vector<int>{3, 1, 2});
}

TEST_CASE("an event that has been taken into the queue of the other core is cancelled there") {
    Sim sim;
    u64 id = 0;
    std::size_t queued = 0;
    std::size_t after = 99;
    bool cancelled = false;
    bool fired = false;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            g.Tick();
            kernel_detail::ScheduleOnCore(1);
            id = sim.GetKernel().ScheduleAt(sim.CoreNow(1) + 50000000, [&] { fired = true; });
            g.Burn(1500000);  // a slice has gone by: core 1 has taken its ts_queue into its own queue
            queued = sim.Time().Lockstep().events[1].size();
            cancelled = sim.GetKernel().Cancel(id);
            after = sim.Time().Lockstep().events[1].size();
        },
        /*block=*/250);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(queued == 1);
    CHECK(cancelled);
    CHECK(after == 0);  // gone from core 1's queue (the id says whose it is), not left there to end slices
    CHECK_FALSE(fired);
}

TEST_CASE("the next event of the other core ends the stretch of this one at its tick (max_slice is the shortest)") {
    // RunLoop gives both cores the shortest of their times to the next event (GetMaxSliceLength of each), so that
    // neither runs past the other's event. An event of core 1's, due half a slice after the first stretch, makes the
    // second stretch of core 0 end there, though core 0 has nothing of its own due.
    Sim sim;
    std::set<u64> ends;
    u64 due = 0;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            g.after_budget = [&] { ends.insert(sim.Time().Lockstep().stretch_end); };
            g.Tick();
            kernel_detail::ScheduleOnCore(1);
            due = sim.CoreNow(1) + kAzaharSlice + kAzaharSlice / 2;
            sim.GetKernel().ScheduleAt(due, [] {});
            g.Burn(3 * kAzaharSlice);
        },
        /*block=*/20);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(ends.count(due) == 1);
}

TEST_CASE("the longest stretch is Azahar's 1,145,777 ticks when both cores have an event far off") {
    // Timer::GetMaxSliceLength is the time to the core's next event, MAX_SLICE_LENGTH when it has none; the cap of the
    // stretch is the shortest of both cores' and the slice. With an event on each core, far off, it is the slice itself
    // (with none on one of them, that core's own MAX_SLICE_LENGTH is the cap, which hides the value of the slice).
    Sim sim;
    kernel_detail::ScheduleOnCore(1);
    sim.GetKernel().ScheduleAt(sim.CoreNow(1) + 1000000000, [] {});
    u64 first_end = 0;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        first_end = sim.Time().Lockstep().stretch_end;
        g.Tick();
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(first_end == kStart + kAzaharSlice);
}

TEST_CASE("a thread that a better one displaces goes back to the front of its queue, ahead of its peer") {
    // The lockstep select() puts the displaced thread at the front of its priority queue (Azahar's SwitchContext), not
    // at the back: A and B have the same priority, A runs, a better thread P wakes up in the middle of A's burn and
    // runs for a moment, and then A goes on before B gets its turn.
    Sim sim;
    std::vector<std::string> log;
    sim.Spawn(0x20, 0, [&](Guest& g) {
        g.SleepNs(1000000);  // 268,111 ticks, inside A's burn
        log.push_back("P");
    });
    sim.Spawn(0x30, 0, [&](Guest& g) {
        g.Burn(1000000);
        log.push_back("A");
    });
    sim.Spawn(0x30, 0, [&](Guest&) { log.push_back("B"); });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(log == std::vector<std::string>{"P", "A", "B"});
    CHECK(sim.Sched().Counters().preemptions >= 1);
}

TEST_CASE("the refill of a core's budget is exact to the tick: a stretch runs one block past its end, no more") {
    // Blocks of one cycle: the block that runs the budget out is charged, so a stretch ends one tick after its
    // stretch_end, and the next one begins there with a whole slice. A refill one tick too long or too short (after a
    // system call, or at the start of a stretch) moves every one of those clocks. (With blocks of 250 cycles, the
    // overshoot of a stretch hides such an error.)
    Sim sim;
    std::vector<u64> starts_after_end;  // the clock at the start of each stretch, less the previous one's stretch_end
    std::vector<u64> lengths;           // stretch_end - stretch_start of each stretch
    u64 previous_end = 0;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            g.Tick();  // a system call: the thread is given what is left of the stretch (the refill after the call)
            LockstepState& ls = sim.Time().Lockstep();
            previous_end = ls.stretch_end;
            lengths.push_back(ls.stretch_end - ls.stretch_start);
            g.after_budget = [&] {
                starts_after_end.push_back(sim.CoreNow(0) - previous_end);
                lengths.push_back(ls.stretch_end - ls.stretch_start);
                previous_end = ls.stretch_end;
            };
            g.Burn(4000000);
        },
        /*block=*/1);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    REQUIRE(starts_after_end.size() >= 3);
    for (std::size_t i = 0; i < starts_after_end.size(); ++i) {
        INFO("stretch " << i);
        CHECK(starts_after_end[i] == 1);  // the block that ran the budget out, charged once
    }
    for (const u64 length : lengths) {
        CHECK(length == kAzaharSlice);
    }
}

TEST_CASE("the stretch of a core is cut at its next event to the tick, also when the other core ran past it (slice_of)") {
    // Core 0's thread puts an event on core 1's queue (its own, far enough off); the event limits both cores' next
    // stretch (max_slice is the shortest time to an event), core 0 overshoots its stretch by the one tick of its
    // block, and core 1 is then given min(what core 0 ran, the time to its event) = the time to the event: its
    // stretch ends at the event's tick, not one tick after it (as it would if the limit alone decided).
    Sim sim;
    u64 due = 0;
    int callbacks = 0;
    std::set<u64> ends1;
    u64 fired_at = 0;
    int fired_core = -1;
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            g.Tick();
            g.after_budget = [&] {
                if (++callbacks != 1) {
                    return;
                }
                // Two stretches from now both cores are about 1,145,778 ticks on; the event is 5000 after that.
                kernel_detail::ScheduleOnCore(1);
                due = sim.CoreNow(1) + kAzaharSlice + 2 + 5000;
                sim.GetKernel().ScheduleAt(due, [&] {
                    fired_at = sim.Now();
                    fired_core = sim.Time().Core();
                });
            };
            g.Burn(6000000);
        },
        /*block=*/1);
    sim.Spawn(
        0x30, 1,
        [&](Guest& g) {
            g.after_budget = [&] { ends1.insert(sim.Time().Lockstep().stretch_end); };
            g.Burn(6000000);
        },
        /*block=*/1);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    REQUIRE(due != 0);
    CHECK(ends1.count(due) == 1);      // a stretch of core 1 ends exactly at the event
    CHECK(ends1.count(due - 1) == 0);
    CHECK(ends1.count(due + 1) == 0);
    CHECK(fired_core == 1);
    CHECK(fired_at == due + 1);        // core 1 runs it one tick late: the block that crossed the tick
}

TEST_CASE("a wake-up during a stretch is acted on at its end: the other core's stretch in between finds the better thread running") {
    // The system flag of RunLoop (PrepareReschedule, NeedReschedule): P (better) waits on an event on core 0; thread A
    // of the same core signals it and makes a system call. A's stretch ends there and, because something asked for a
    // reschedule, core 0 is rescheduled at once: P runs, A goes to the front of its queue. Core 1's stretch comes
    // before core 0's next select, and its thread gives P A's priority (SetThreadPriority): P is the running thread
    // then, so it stays one (nothing is *better* than it); without the reschedule at the end of A's stretch, P would
    // still be in the ready queue then, would be moved behind A, and A would go on first.
    Sim sim;
    std::vector<std::string> log;
    auto event = sim.GetKernel().NewEvent(ResetType::OneShot);
    bool signalled = false;
    bool changed = false;
    std::shared_ptr<Thread> p = sim.Spawn(0x20, 0, [&](Guest&) {
        sim.Sched().Block(*sim.Sched().CurrentThreadPtr(), {event}, false, -1);
        log.push_back("P");
    });
    sim.Spawn(0x30, 0, [&](Guest& g) {
        g.Tick();
        signalled = true;
        event->Signal();
        g.Tick();  // the system call that ends the stretch
        log.push_back("A");
    });
    sim.Spawn(
        0x30, 1,
        [&](Guest& g) {
            g.after_budget = [&] {
                if (signalled && !changed) {
                    changed = true;
                    p->SetPriority(0x30);
                }
            };
            g.Burn(3000000);
        },
        /*block=*/20);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(changed);
    CHECK(log == std::vector<std::string>{"P", "A"});
}

namespace {

// What a call that asks for a reschedule and readies nothing (PrepareReschedule, or Terminate of a thread that is
// not on a ready queue) does on the other core: P (0x20, core 1) is readied by core 0's stretch, after core 1's
// select of this RunLoop call, so core 1's Q (0x30) runs the next stretch with a better thread waiting. Q makes the call
// and a system call past core 0's clock by more than 100 ticks, so core 0 catches up alone before core 1's next
// select, and its thread gives P the priority of Q. With the flag, core 1 is rescheduled at the end of its stretch: P
// is running when its priority changes and stays. Without it, P is still on the queue, behind Q after the change.
enum class Ask { Nothing, PrepareReschedule, TerminateOther };

std::vector<std::string> RescheduleAsked(Ask ask, u64* gap) {
    Sim sim;
    std::vector<std::string> log;
    auto event = sim.GetKernel().NewEvent(ResetType::OneShot);
    auto other_event = sim.GetKernel().NewEvent(ResetType::OneShot);
    bool p_blocked = false;
    bool armed = false;
    bool asked = false;
    std::shared_ptr<Thread> other;
    std::shared_ptr<Thread> p = sim.Spawn(0x20, 1, [&](Guest&) {
        p_blocked = true;
        sim.Sched().Block(*sim.Sched().CurrentThreadPtr(), {event}, false, -1);
        log.push_back("P");
    });
    other = sim.Spawn(0x3F, 0, [&](Guest&) {
        sim.Sched().Block(*sim.Sched().CurrentThreadPtr(), {other_event}, false, -1);
    });
    sim.Spawn(
        0x30, 0,
        [&](Guest& g) {
            while (!p_blocked) {
                g.Burn(20);
            }
            armed = true;
            event->Signal();  // P is ready on core 1
            g.Tick();         // the stretch of core 0 ends here
            *gap = sim.CoreNow(1) - sim.CoreNow(0);
            p->SetPriority(0x30);
            g.Burn(3000000);
        },
        /*block=*/20);
    sim.Spawn(
        0x30, 1,
        [&](Guest& g) {
            g.after_budget = [&] {
                if (armed && !asked) {
                    asked = true;
                    if (ask == Ask::PrepareReschedule) {
                        sim.Sched().PrepareReschedule();
                    } else if (ask == Ask::TerminateOther) {
                        sim.Sched().Terminate(other);
                        ++sim.finished;
                    }
                    g.Svc(0x35, 0, 0x1234);
                }
            };
            g.Burn(6000000);
            log.push_back("Q");
            if (ask != Ask::TerminateOther) {
                other_event->Signal();
            }
        },
        /*block=*/20);
    REQUIRE(sim.Run() == RunStatus::Stopped);
    return log;
}

}  // namespace

TEST_CASE("a call that only asks for a reschedule brings the choice of core 1 forward to the end of its stretch") {
    // Control: with no call, Q goes on. Then PrepareReschedule, the whole of Azahar's SVC::ArbitrateAddress and
    // SendSyncRequest when they wake nobody (svc.cpp calls it unconditionally), and Terminate of a thread that is
    // waiting. (Azahar has no TerminateThread system call: that case pins the native design, which raises the flag
    // as KernelSystem::PrepareReschedule would; the PrepareReschedule one pins Azahar's rule.) The orders follow
    // Azahar's: one System::Reschedule per core guarded by curr_core_reschedule_pending, PopNextReadyThread
    // taking only a strictly better thread, Thread::SetPriority moving only a Ready thread.
    u64 gap = 0;
    CHECK(RescheduleAsked(Ask::Nothing, &gap) == std::vector<std::string>{"Q", "P"});
    CHECK(gap > 100);
    CHECK(RescheduleAsked(Ask::PrepareReschedule, &gap) == std::vector<std::string>{"P", "Q"});
    CHECK(gap > 100);
    CHECK(RescheduleAsked(Ask::TerminateOther, &gap) == std::vector<std::string>{"P", "Q"});
    CHECK(gap > 100);
}

TEST_CASE("the limiter's tick is an event of core 1, due on core 1's own clock, whichever core asks") {
    // Azahar: ScheduleEvent(next_timer, tick_event, 0, 1) from UpdateAppCpuLimit, which runs in a system call of a
    // thread of either core. Core 0's thread asks when core 0 is 340 ticks into the run and core 1 has not run: the
    // first phase is the system one (375,357 ticks of a 30 % share of 536,223), counted from core 1's clock.
    Sim sim;
    bool pending_on_core1 = false;
    bool queued_on_core0 = false;
    u64 core0_clock = 0;
    sim.Spawn(0x30, 0, [&](Guest& g) {
        g.Tick();
        core0_clock = sim.CoreNow(0);
        ThreadManager& threads = sim.runtime.Threads();
        threads.ApplyAppMaxCpuSetting(0x0004000000123400ull, 1, 80);  // Multi, limit 80, share 0
        REQUIRE(threads.SetAppCpuTimeLimit(30) == kResultSuccess);
        for (const LockstepState::Pending& pending : sim.Time().Lockstep().pending[1]) {
            pending_on_core1 = pending_on_core1 || pending.tick == kStart + 375357;
        }
        for (const auto& entry : sim.Time().Lockstep().events[0]) {
            queued_on_core0 = queued_on_core0 || std::get<0>(entry) == kStart + 375357 ||
                              std::get<0>(entry) == core0_clock + 375357;
        }
    });
    REQUIRE(sim.Run() == RunStatus::Stopped);
    CHECK(core0_clock == kStart + 340);
    CHECK(pending_on_core1);
    CHECK_FALSE(queued_on_core0);
}

TEST_CASE("the deterministic timeline has the periodic events of Azahar's, each ending a slice at its tick") {
    // A run with the services of the console (the HID, the GSP's VBlank, the APT module's button update, the DSP
    // and the rest, as booted) and one thread that burns cycles: every stretch of it ends at the next event or
    // after a slice, so the ends of the stretches are the ticks of the events (the numbers are the first of each
    // in the reference runner's --trace-timing of a recording: the first frame of the DSP at 1,310,720 after the
    // start, the second pad update at 2,291,554 (the first, at 1,145,777, falls three ticks after the end of the
    // first slice and so runs late), the button update at 4,468,352, the VBlank at 4,481,136 and the cheat engine's
    // run at 50,000,000). Each of those recurs one period after the tick it was due at, not after the tick it ran at:
    // the second ones are at the multiples (the DSP's frame 1,310,720, the button update 4,468,352 and the cheat
    // engine's 50,000,000), and the shared page's date update, which is at the start, is due again an hour later.
    SetLogFilter(LogFilter(LogLevel::Error));
    Runtime runtime;
    std::string error;
    REQUIRE_MESSAGE(runtime.Prepare(Parse({"--deterministic"}), {Phase::Devices, Phase::Services}, &error), error);
    VirtualTimeSource& time = static_cast<VirtualTimeSource&>(runtime.Clock().Source());
    std::set<u64> ends;
    bool hourly_queued = false;
    bool cheat_queued = false;
    std::atomic<int> finished{0};
    Kernel& kernel = runtime.GetKernel();
    auto thread = kernel.NewThread("burner", 0x30, 0, kernel.CurrentProcess());
    runtime.Scheduler().Attach(thread, [&](GuestThread& g) {
        RCtx& ctx = g.Context();
        ends.insert(time.Lockstep().stretch_end - kStart);  // the end of the first stretch
        for (const auto& entry : time.Lockstep().events[0]) {
            hourly_queued = hourly_queued || std::get<0>(entry) == kStart + 268111856ull * 3600;
            cheat_queued = cheat_queued || std::get<0>(entry) == kStart + 50000000;
        }
        for (s64 left = 101000000; left > 0; left -= 1000) {
            if ((ctx.budget -= 1000) < 0) {
                CodeHostCallbacks::Budget(&ctx);
                ends.insert(time.Lockstep().stretch_end - kStart);  // the end of the stretch that just began
            }
        }
        ++finished;
        CodeHostCallbacks::Svc(&ctx, 0x09, kText + 0x11);
    });
    runtime.Scheduler().Start(thread);
    REQUIRE(runtime.Scheduler().Run([&] { return finished.load() == 1; }) == RunStatus::Stopped);
    CHECK(kDeterministicSliceTicks == kAzaharSlice);
    CHECK(ends.count(kAzaharSlice) == 1);  // the first slice: Azahar's MAX_SLICE_LENGTH
    CHECK(hourly_queued);
    CHECK(cheat_queued);
    for (const u64 tick : {u64{1310720}, u64{2291554}, u64{4468352}, u64{4481136}, u64{50000000}}) {
        INFO("an event at " << tick);
        CHECK(ends.count(tick) == 1);
    }
    for (const u64 tick : {u64{2 * 1310720}, u64{2 * 4468352}, u64{2 * 50000000}}) {
        INFO("the next occurrence, at " << tick);
        CHECK(ends.count(tick) == 1);
    }
    SetLogFilter(LogFilter(LogLevel::Info));
}

namespace {

// Two threads of the same priority, one on each core, that never block: they run side by side, each in
// stretches of one slice. Each iteration costs 100,000 cycles and is logged when it is done. The blocks are
// 20 cycles, so that what a stretch overshoots its slice by stays far below the 100 ticks RunLoop puts up with
// between the cores (with blocks of 250 the cores would chase each other's overshoot, as the reference's would).
struct TakingTurns {
    std::vector<std::string> log;
    std::array<u64, 2> finish{};  // what each thread reads from svcGetSystemTick when it is done
};

TakingTurns RunTakingTurns(std::vector<std::string> args) {
    Sim sim(std::move(args));
    TakingTurns result;
    constexpr int kIterations = 60;
    for (const char* name : {"a", "b"}) {
        const int core = name[0] == 'a' ? 0 : 1;
        sim.Spawn(
            0x30, core,
            [&, name, core](Guest& g) {
                for (int i = 0; i < kIterations; ++i) {
                    g.Burn(100000);
                    result.log.push_back(std::string(name) + std::to_string(i));
                }
                result.finish[core] = g.Tick();
            },
            /*block=*/20);
    }
    REQUIRE(sim.Run() == RunStatus::Stopped);
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

TEST_CASE("two runs give the same log; the two cores run side by side, each for the time its own thread takes") {
    const TakingTurns one = RunTakingTurns({});
    const TakingTurns two = RunTakingTurns({});
    CHECK(one.log == two.log);
    CHECK(one.finish == two.finish);
    REQUIRE(one.log.size() == 120);
    // Each core's clock counts what its own thread charged: 60 iterations of 100,000 cycles, and GetSystemTick's
    // 340. Both finish there, not 6 million cycles apart: one clock for both cores would say 12 million.
    CHECK(one.finish[0] == kStart + 60 * 100000 + 340);
    CHECK(one.finish[1] == kStart + 60 * 100000 + 340);
    // A stretch is a slice, 1,145,777 cycles, so about 11 iterations in a row, not 60; core 0 first.
    const std::string runs = Runs(one.log);
    CAPTURE(runs);
    CHECK(runs.rfind("a:11 b:11 ", 0) == 0);
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
        // the results: every thread ran its 60 iterations in order, and each core's clock counted the same cycles
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
        CHECK(run.finish == plain.finish);
        shapes.push_back(Runs(run.log));
        // the same seed again: the same run, to the entry
        const TakingTurns again = RunTakingTurns({"--seed", std::to_string(seed)});
        CHECK(again.log == run.log);
        CHECK(again.finish == run.finish);
    }
    // ...but the points where the cores take turns are not the unseeded ones, and the seeds differ.
    std::sort(shapes.begin(), shapes.end());
    shapes.erase(std::unique(shapes.begin(), shapes.end()), shapes.end());
    CHECK(shapes.size() >= 5);  // the unseeded run and five seeds: at most one pair may coincide
}

TEST_CASE("a seeded stretch runs from one half to one times the default slice") {
    // The draw itself goes from half to one and a half times the default, but a core with an empty event queue is
    // capped at the default slice (Timer::GetMaxSliceLength is MAX_SLICE_LENGTH then), so here, where the only events
    // are far off, the stretches that run are from one half to one times. (With events on both cores far off, a drawn
    // stretch can run up to one and a half times, as in Azahar.)
    {
        Sim sim({"--seed", "9"});
        std::vector<u64> lengths;
        sim.Spawn(
            0x30, 0,
            [&](Guest& g) {
                g.Tick();
                g.after_budget = [&] {
                    lengths.push_back(sim.Time().Lockstep().stretch_end - sim.Time().Lockstep().stretch_start);
                };
                g.Burn(30000000);
            },
            /*block=*/20);
        REQUIRE(sim.Run() == RunStatus::Stopped);
        REQUIRE(lengths.size() > 20);
        const u64 shortest = *std::min_element(lengths.begin(), lengths.end());
        const u64 longest = *std::max_element(lengths.begin(), lengths.end());
        CHECK(longest == kAzaharSlice);  // the draws above the default are cut to it, and half of them are above
        CHECK(shortest >= kAzaharSlice / 2 - 140);
        CHECK(shortest < kAzaharSlice * 3 / 4);
    }
}

TEST_CASE("a seeded slice is drawn between half and one and a half times the default (the value drawn, not the stretch)") {
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
