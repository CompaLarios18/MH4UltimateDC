// Derived from Azahar src/core/hle/kernel/thread.cpp, src/core/hle/kernel/svc.cpp,
// src/core/core.cpp and src/common/thread_queue_list.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: svc.cpp and core.cpp say so; thread.cpp and thread_queue_list.h say
// GPL-2.0-or-later): the expected orders and results are worked out from their algorithms
// (PopNextReadyThread, SwitchContext and ThreadQueueList for who runs next and where a displaced or
// woken thread goes; SleepThread for the zero-nanosecond yield; WaitSynchronization1/N for what is
// acquired at once, what a timeout of 0 refuses and the index of a wait on any), never by running
// this code.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.cpp, svc.cpp,
//   core.cpp, thread_queue_list.h)
// Copyright 2014 PPSSPP Project (thread_queue_list.h, second notice: Licensed under GPLv2 or any
//   later version)
//
// mh4n_kernel (N1-06): the scheduler, the game clock and the timer queue, with fibers that run
// small C++ functions against the scheduler's API (no code library). Time is driven by hand
// (ManualTimeSource and a PreemptionSource that raises the flag when the manual time passes the
// armed deadline), so nothing here depends on how fast the machine is; the two cases that use the
// real clock and the timer thread (the Post from another host thread, the timer thread's flag) wait
// on conditions, not on sleeps.
//
// Clock arithmetic: 268,111,856 ticks per second. 5 ms = 1,340,559.28 ticks, floor 1,340,559; 1 ms
// = 268,111.856, floor 268,111; 2 ms = 536,223.712, floor 536,223.
//
// Assertions stay out of the fibers: a fiber records what it saw in `log` or in plain variables and
// the test checks it afterwards.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/cpu.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/scheduler.h"

using namespace mh4n;

namespace {

constexpr s64 kUs = 1000;
constexpr s64 kMs = 1000 * kUs;

// A PreemptionSource for manual time: raises when the manual clock reaches the armed deadline, which
// is what the timer thread does with real time.
class ManualPreemption final : public PreemptionSource {
public:
    explicit ManualPreemption(ManualTimeSource& source) : source_(source) {
        source_.SetAdvanceHook([this] { Check(); });
    }
    void Start(std::function<void()> raise) override { raise_ = std::move(raise); }
    void Stop() override { raise_ = nullptr; }
    void Arm(std::optional<s64> deadline_ns) override {
        deadline_ = deadline_ns;
        ++arms;
        Check();
    }
    std::optional<s64> Deadline() const { return deadline_; }
    int arms = 0;

private:
    void Check() {
        if (raise_ && deadline_ && source_.NowNs() >= *deadline_) {
            deadline_.reset();  // one shot, as the timer thread
            raise_();
        }
    }
    ManualTimeSource& source_;
    std::function<void()> raise_;
    std::optional<s64> deadline_;
};

struct Sim {
    std::unique_ptr<GuestMemory> memory;
    ManualTimeSource manual;
    ManualPreemption manual_preemption{manual};
    SteadyTimeSource steady;
    std::unique_ptr<PreemptionSource> timer_thread;
    TimeSource* time = nullptr;
    PreemptionSource* preemption = nullptr;
    std::unique_ptr<GameClock> clock;
    std::unique_ptr<CodeHost> host;
    std::unique_ptr<Kernel> kernel;
    std::unique_ptr<GuestScheduler> sched;
    std::shared_ptr<Process> process;
    std::vector<std::string> log;

    explicit Sim(bool real_time = false, u32 slice_us = 1000) {
        memory = GuestMemory::Create();
        REQUIRE(memory != nullptr);
        if (real_time) {
            timer_thread = MakeTimerThreadPreemption(steady);
            time = &steady;
            preemption = timer_thread.get();
        } else {
            time = &manual;
            preemption = &manual_preemption;
        }
        clock = std::make_unique<GameClock>(*time);
        host = std::make_unique<CodeHost>(*memory);
        kernel = std::make_unique<Kernel>(*memory, 0);
        GuestScheduler::Options options;
        options.slice_us = slice_us;
        sched = std::make_unique<GuestScheduler>(*kernel, *host, *clock, *time, preemption, options);
        process = kernel->NewProcess("app");
    }
    // The order of the lifetime rules in scheduler.h: the threads, then the kernel, then the scheduler.
    ~Sim() {
        sched->Shutdown();
        process.reset();
        kernel.reset();
        sched.reset();
        host.reset();
        clock.reset();
    }

    void Log(std::string line) { log.push_back(std::move(line)); }

    std::shared_ptr<Thread> Spawn(u32 priority, int core, std::function<void()> body,
                                  bool start = true, const std::string& name = "t") {
        auto thread = kernel->NewThread(name, priority, core, process);
        sched->AttachFunction(thread, std::move(body));
        if (start) {
            sched->Start(thread);
        }
        return thread;
    }
    Thread& Self() { return *sched->CurrentThread(); }
    void Advance(s64 ns) { manual.Advance(ns); }
};

}  // namespace

// ---- the clock -------------------------------------------------------------------------------------

TEST_CASE("clock conversions are exact at one year of ticks") {
    // 365 days = 31,536,000 s. Written out, not computed from the rate under test.
    constexpr u64 kYearNs = 31536000000000000ull;
    constexpr u64 kYearTicks = 8455175490816000ull;  // 31,536,000 * 268,111,856
    CHECK(NsToTicks(kYearNs) == kYearTicks);
    CHECK(TicksToNs(kYearTicks) == kYearNs);
    CHECK(TicksToNsCeil(kYearTicks) == kYearNs);
    // The same a second and a year later: no rounding creeps in.
    CHECK(NsToTicks(kYearNs + 1000000000ull) == kYearTicks + 268111856ull);
    CHECK(TicksToNs(kYearTicks + 268111856ull) == kYearNs + 1000000000ull);
    // 100 s: ns * 268111856 is 2.68e19, past 64 bits (1.8e19); the 128-bit product keeps it.
    CHECK(NsToTicks(100ull * 1000000000ull) == 26811185600ull);
    CHECK(TicksToNs(26811185600ull) == 100ull * 1000000000ull);
    // The values the kernel tests use.
    CHECK(NsToTicks(1000000) == 268111);
    CHECK(NsToTicks(2000000) == 536223);
    CHECK(NsToTicks(5000000) == 1340559);
    // The ceiling covers the ticks, and nothing less does.
    for (u64 ticks : std::initializer_list<u64>{1, 2, 3, 268111, 268111856, 1340559, kYearTicks - 1}) {
        const u64 ns = TicksToNsCeil(ticks);
        CHECK(NsToTicks(ns) == ticks);
        CHECK(NsToTicks(ns - 1) < ticks);
    }
    // Saturation instead of wrap-around at the end of the range.
    CHECK(TicksToNs(~u64{0}) == ~u64{0});
}

TEST_CASE("the game clock follows the host, minus pauses and holds") {
    ManualTimeSource source(1000);  // an origin that is not 0
    GameClock clock(source);
    int changes = 0;
    clock.SetChangeCallback([&] { ++changes; });
    CHECK(clock.Now() == 0);
    source.Advance(1000000000);  // 1 s
    CHECK(clock.Now() == 268111856);
    CHECK(clock.NowNs() == 1000000000);

    clock.Pause();
    CHECK(clock.Paused());
    source.Advance(5000000000);  // not counted
    CHECK(clock.Now() == 268111856);
    clock.Pause();  // does not nest
    clock.Resume();
    CHECK_FALSE(clock.Paused());
    source.Advance(1000000000);
    CHECK(clock.Now() == 2 * 268111856ull);
    CHECK(changes == 2);

    // Hold: the clock stops at the limit, and the time standing there is not made up.
    clock.Hold(3 * 268111856ull);
    source.Advance(5000000000);  // 2 s would reach the limit, 5 s passed
    CHECK(clock.Now() == 3 * 268111856ull);
    CHECK(clock.HeldAt() == 3 * 268111856ull);
    clock.ReleaseHold();
    CHECK_FALSE(clock.HeldAt().has_value());
    source.Advance(500000000);
    CHECK(clock.Now() == 3 * 268111856ull + 134055928ull);
    // A hold below the reading holds the clock where it is: it never goes backwards.
    const u64 before = clock.Now();
    clock.Hold(1);
    source.Advance(1000000000);
    CHECK(clock.Now() == before);
    clock.ReleaseHold();
}

TEST_CASE("the game clock tells the host time of a tick") {
    ManualTimeSource source(0);
    GameClock clock(source);
    // 5 ms of game time is 1,340,559 ticks (floor); the host time that covers them is 4,999,999 ns,
    // when the clock reads exactly that.
    const auto host = clock.HostNsFor(1340559);
    REQUIRE(host.has_value());
    CHECK(*host == 4999999);
    source.Advance(*host);
    CHECK(clock.Now() == 1340559);
    // A tick in the past is "now".
    CHECK(clock.HostNsFor(10) == source.NowNs());
    // Never while paused or held below it.
    clock.Pause();
    CHECK_FALSE(clock.HostNsFor(2000000).has_value());
    clock.Resume();
    CHECK(clock.HostNsFor(2000000).has_value());
    clock.Hold(1500000);
    CHECK_FALSE(clock.HostNsFor(2000000).has_value());
    CHECK(clock.HostNsFor(1400000).has_value());
}

// ---- priorities, order, yielding ---------------------------------------------------------------------

TEST_CASE("a thread made ready at a higher priority runs first") {
    Sim sim;
    // Started in the order low, high: the better priority runs first whatever the order.
    sim.Spawn(40, 0, [&] { sim.Log("low"); });
    sim.Spawn(10, 0, [&] { sim.Log("high"); });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"high", "low"});
}

TEST_CASE("a thread that becomes ready at a better priority displaces the running one at once") {
    Sim sim;
    sim.Spawn(50, 0, [&] {
        sim.Log("A1");
        sim.Spawn(10, 0, [&] { sim.Log("B"); });
        sim.sched->PreemptPoint();  // the flag was raised: B runs here
        sim.Log("A2");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"A1", "B", "A2"});
    CHECK(sim.sched->Counters().preemptions == 1);
}

TEST_CASE("an SVC that readies a better thread switches before it returns") {
    Sim sim;
    sim.Spawn(50, 0, [&] {
        sim.Log("A1");
        sim.sched->BeginSvc(0x08);
        sim.Spawn(10, 0, [&] { sim.Log("B"); });  // CreateThread, say
        sim.Log("A in svc");
        sim.sched->EndSvc();                      // B runs inside this call
        sim.Log("A2");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"A1", "A in svc", "B", "A2"});
}

TEST_CASE("equal priorities run first in, first out, and SleepThread(0) yields to them") {
    Sim sim;
    for (const char* name : {"A", "B", "C"}) {
        sim.Spawn(40, 0, [&sim, name] {
            for (int i = 0; i < 3; i++) {
                sim.Log(std::string(name) + std::to_string(i));
                sim.sched->Sleep(0);
            }
        });
    }
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"A0", "B0", "C0", "A1", "B1", "C1", "A2", "B2", "C2"});
    // 9 yields, each a fiber switch (the first entry and the last exits are switches too).
    CHECK(sim.sched->Counters().thread_switches >= 9);
}

TEST_CASE("equal priorities are not time-sliced: a thread runs until it blocks") {
    Sim sim;
    sim.Spawn(40, 0, [&] {
        for (int i = 0; i < 5; i++) {
            sim.Advance(300 * kUs);  // 1.5 ms: past the slice, but a core with one priority has no rival
            sim.sched->PreemptPoint();
            sim.Log("A" + std::to_string(i));
        }
    });
    sim.Spawn(40, 0, [&] { sim.Log("B"); });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"A0", "A1", "A2", "A3", "A4", "B"});
}

TEST_CASE("a displaced thread goes back to the front of its queue, a woken one to the back") {
    Sim sim;
    sim.Spawn(30, 0, [&] {
        sim.Log("A1");
        sim.Spawn(30, 0, [&] { sim.Log("E"); });  // ready behind A
        sim.Spawn(10, 0, [&] { sim.Log("H"); });  // displaces A, which keeps its place before E
        sim.sched->PreemptPoint();
        sim.Log("A2");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"A1", "H", "A2", "E"});
}

TEST_CASE("SleepThread(0) with nothing ready on the core returns at once; lower priorities get a turn") {
    Sim sim;
    // Alone: a no-op.
    sim.Spawn(10, 0, [&] {
        sim.Log("H0");
        sim.sched->Sleep(0);
        sim.Log("H1");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"H0", "H1"});

    // With only a lower priority ready, Azahar makes the thread wait for a zero delay and the lower
    // one runs until that event is processed: here, until its next preemption check.
    Sim sim2;
    sim2.Spawn(10, 0, [&] {
        sim2.Log("H0");
        sim2.sched->Sleep(0);
        sim2.Log("H1");
    });
    sim2.Spawn(40, 0, [&] {
        sim2.Log("L0");
        sim2.sched->PreemptPoint();  // processes the due wake-up: H displaces L
        sim2.Log("L1");
    });
    CHECK(sim2.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim2.log == std::vector<std::string>{"H0", "L0", "H1", "L1"});
}

TEST_CASE("a priority inherited from a mutex moves a ready thread ahead of the others") {
    Sim sim;
    std::shared_ptr<Mutex> mutex;
    sim.Spawn(40, 0, [&] {
        mutex = sim.kernel->NewMutex(/*initial_locked=*/true);  // L holds it
        sim.Log("L locked");
        sim.Spawn(20, 0, [&] { sim.Log("M"); });
        sim.Spawn(5, 0, [&] {
            sim.Log("H waits");
            const WaitOutcome outcome = sim.sched->Block(sim.Self(), {mutex}, false, -1);
            sim.Log(outcome.result == kResultSuccess ? "H acquired" : "H failed");
            mutex->Release(&sim.Self());
        });
        sim.sched->PreemptPoint();  // H runs and blocks; L inherits priority 5, ahead of M
        sim.Log("L boosted");
        const Result released = mutex->Release(&sim.Self());
        sim.Log(released == kResultSuccess ? "L released" : "L release failed");
        sim.sched->PreemptPoint();  // H wakes
        sim.Log("L done");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"L locked", "H waits", "L boosted", "L released",
                                              "H acquired", "M", "L done"});
}

// ---- waiting ---------------------------------------------------------------------------------------

TEST_CASE("a wait on any wakes with the index of the object that was signalled") {
    Sim sim;
    auto e1 = sim.kernel->NewEvent(ResetType::OneShot);
    auto e2 = sim.kernel->NewEvent(ResetType::OneShot);
    WaitOutcome outcome;
    sim.Spawn(20, 0, [&] { outcome = sim.sched->Block(sim.Self(), {e1, e2}, false, -1); sim.Log("W"); });
    sim.Spawn(30, 0, [&] {
        sim.Log("S");
        e2->Signal();
        sim.sched->PreemptPoint();
        sim.Log("S2");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"S", "W", "S2"});
    CHECK(outcome.blocked);
    CHECK(outcome.result == kResultSuccess);
    CHECK(outcome.index == 1);
    CHECK_FALSE(e2->IsSignaled());  // a one-shot event is taken by the wait
}

TEST_CASE("a wait on all wakes when the last object is signalled") {
    Sim sim;
    auto e1 = sim.kernel->NewEvent(ResetType::Sticky);
    auto e2 = sim.kernel->NewEvent(ResetType::Sticky);
    WaitOutcome outcome;
    sim.Spawn(10, 0, [&] {
        outcome = sim.sched->Block(sim.Self(), {e1, e2}, true, -1);
        sim.Log("W woke");
    });
    sim.Spawn(30, 0, [&] {
        sim.Log("S e1");
        e1->Signal();
        sim.sched->PreemptPoint();
        sim.Log("S after e1");
        e2->Signal();
        sim.sched->PreemptPoint();
        sim.Log("S after e2");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"S e1", "S after e1", "W woke", "S after e2"});
    CHECK(outcome.result == kResultSuccess);
    CHECK(outcome.index == -1);  // no output for a wait on all
}

TEST_CASE("waits that can be satisfied at once, or refused with a timeout of 0, do not block") {
    Sim sim;
    auto signalled = sim.kernel->NewEvent(ResetType::OneShot);
    auto idle = sim.kernel->NewEvent(ResetType::OneShot);
    signalled->Signal();
    WaitOutcome any_first, any_second, all_ready, all_refused;
    sim.Spawn(20, 0, [&] {
        any_first = sim.sched->Block(sim.Self(), {idle, signalled}, false, 0);  // index 1
        signalled->Signal();
        any_second = sim.sched->Block(sim.Self(), {idle}, false, 0);            // refused
        signalled->Signal();
        all_ready = sim.sched->Block(sim.Self(), {signalled}, true, 0);
        all_refused = sim.sched->Block(sim.Self(), {idle, signalled}, true, 0);
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK_FALSE(any_first.blocked);
    CHECK(any_first.result == kResultSuccess);
    CHECK(any_first.index == 1);
    CHECK_FALSE(any_second.blocked);
    CHECK(any_second.result == kResultTimeout);
    CHECK(any_second.index == -1);
    CHECK_FALSE(all_ready.blocked);
    CHECK(all_ready.result == kResultSuccess);
    CHECK(all_ready.index == -1);
    CHECK_FALSE(all_refused.blocked);
    CHECK(all_refused.result == kResultTimeout);
}

TEST_CASE("a wait with a timeout ends with the timeout result when the game clock gets there") {
    Sim sim;
    auto never = sim.kernel->NewEvent(ResetType::OneShot);
    auto other = sim.kernel->NewEvent(ResetType::OneShot);
    WaitOutcome any, all;
    u64 any_at = 0, all_at = 0;
    sim.Spawn(20, 0, [&] {
        any = sim.sched->Block(sim.Self(), {never, other}, false, 5 * kMs);
        any_at = sim.clock->Now();
        all = sim.sched->Block(sim.Self(), {never, other}, true, 2 * kMs);
        all_at = sim.clock->Now();
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(any.blocked);
    CHECK(any.result == kResultTimeout);
    CHECK(any.index == -1);
    CHECK(any_at == 1340559);                  // 5 ms
    CHECK(all.result == kResultTimeout);
    CHECK(all_at == 1340559 + 536223);         // 2 ms later
    CHECK(sim.sched->Counters().idle_ns > 0);  // the host thread had nothing to run in between
    CHECK(never->GetWaitingThreads().empty());
    CHECK(other->GetWaitingThreads().empty());
}

TEST_CASE("a signal before the timeout wins, and the timeout does not fire later") {
    Sim sim;
    auto ev = sim.kernel->NewEvent(ResetType::OneShot);
    WaitOutcome first, second;
    sim.Spawn(20, 0, [&] {
        first = sim.sched->Block(sim.Self(), {ev}, false, 10 * kMs);
        // A later, unrelated wait must not be woken by the first wait's timeout (a departure from
        // Azahar, which leaves the event queued).
        second = sim.sched->Block(sim.Self(), {ev}, false, 30 * kMs);
    });
    sim.Spawn(30, 0, [&] {
        ev->Signal();
        sim.sched->PreemptPoint();
        sim.Log("S");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(first.result == kResultSuccess);
    CHECK(first.index == 0);
    CHECK(second.result == kResultTimeout);
    CHECK(sim.clock->Now() == NsToTicks(30 * kMs));  // the second wait ran its whole 30 ms
}

TEST_CASE("SleepThread waits on the game clock and leaves r0 alone") {
    Sim sim;
    u64 woke_at = 0;
    sim.Spawn(20, 0, [&] {
        sim.Self().status = ThreadStatus::WaitSleep;
        sim.Self().ArmTimeout(1 * kMs);
        const WaitOutcome outcome = sim.sched->Park(sim.Self());
        sim.Log(outcome.result ? "has result" : "no result");
        woke_at = sim.clock->Now();
        sim.sched->Sleep(2 * kMs);
        sim.Log("slept");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"no result", "slept"});
    CHECK(woke_at == 268111);
    CHECK(sim.clock->Now() == 268111 + 536223);
}

TEST_CASE("arbiter waiters are woken in priority order") {
    Sim sim;
    REQUIRE(sim.memory->Map(0x08000000, 0x1000, MemPerm::ReadWrite, MemState::Private) ==
            kResultSuccess);
    REQUIRE(sim.memory->Write<s32>(0x08000100, 0));
    auto arbiter = sim.kernel->NewAddressArbiter();
    std::vector<Result> results(3, 0xFFFFFFFF);
    int index = 0;
    // Started in the order 30, 10, 20; each waits until the value at the address is not less than 1.
    for (u32 priority : {30u, 10u, 20u}) {
        const int slot = index++;
        sim.Spawn(priority, 0, [&, priority, slot] {
            Thread& self = sim.Self();
            const Result r = arbiter->ArbitrateAddress(sim.sched->CurrentThreadPtr(),
                                                       ArbitrationType::WaitIfLessThan, 0x08000100,
                                                       1, -1);
            if (r == kResultSuccess && self.status == ThreadStatus::WaitArb) {
                results[slot] = sim.sched->Park(self).result.value_or(0xDEAD);
            }
            sim.Log("woke " + std::to_string(priority));
        });
    }
    sim.Spawn(60, 0, [&] {
        // All three have blocked by now (they are better than this thread).
        sim.Log("waiters: " + std::to_string(arbiter->WaitingThreads().size()));
        for (int i = 0; i < 3; i++) {
            arbiter->ArbitrateAddress(sim.sched->CurrentThreadPtr(), ArbitrationType::Signal,
                                      0x08000100, 1, 0);
            sim.sched->PreemptPoint();  // the woken waiter runs here, being better than this one
        }
        sim.Log("done");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"waiters: 3", "woke 10", "woke 20", "woke 30", "done"});
    for (Result r : results) {
        CHECK(r == kResultSuccess);
    }
}

TEST_CASE("a thread that ends wakes the threads waiting for it") {
    Sim sim;
    WaitOutcome outcome;
    auto target = sim.Spawn(30, 0, [&] { sim.Log("T runs"); }, /*start=*/false);
    sim.Spawn(10, 0, [&] {
        sim.Log("J waits");
        outcome = sim.sched->Block(sim.Self(), {target}, false, -1);
        sim.Log("J woke");
    });
    sim.Spawn(50, 0, [&] {
        sim.Log("S starts T");
        sim.sched->Start(target);
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    // J (the best) blocks on a thread that has not started; S starts it; T (better than S) runs and
    // ends; J wakes.
    CHECK(sim.log == std::vector<std::string>{"J waits", "S starts T", "T runs", "J woke"});
    CHECK(outcome.result == kResultSuccess);
    CHECK(target->status == ThreadStatus::Dead);
}

// ---- timers ------------------------------------------------------------------------------------------

TEST_CASE("a periodic timer fires at its interval") {
    Sim sim;
    auto timer = sim.kernel->NewTimer(ResetType::OneShot);
    std::vector<u64> ticks;
    sim.Spawn(20, 0, [&] {
        timer->Set(1 * kMs, 2 * kMs);
        for (int i = 0; i < 3; i++) {
            sim.sched->Block(sim.Self(), {timer}, false, -1);
            ticks.push_back(sim.clock->Now());
        }
        timer->Cancel();
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    // First shot at 1 ms = 268,111 ticks, then one interval (2 ms = 536,223 ticks) after each
    // deadline, so the period does not drift: 804,334 and 1,340,557.
    CHECK(ticks == std::vector<u64>{268111, 804334, 1340557});
    CHECK_FALSE(timer->IsArmed());
}

TEST_CASE("timer queue events run in tick order at a safe point and can be cancelled") {
    Sim sim;
    std::vector<int> ran;
    const u64 id_a = sim.sched->ScheduleAt(300, [&] { ran.push_back(3); });
    sim.sched->ScheduleAt(100, [&] { ran.push_back(1); });
    sim.sched->ScheduleAt(200, [&] { ran.push_back(2); });
    sim.sched->ScheduleAt(200, [&] { ran.push_back(22); });  // same tick: in the order scheduled
    const u64 id_c = sim.sched->ScheduleAt(250, [&] { ran.push_back(99); });
    CHECK(id_a != 0);
    CHECK(sim.sched->Cancel(id_c));
    CHECK_FALSE(sim.sched->Cancel(id_c));  // already gone
    CHECK(ran.empty());                    // nothing runs inside ScheduleAt
    // Not due yet.
    CHECK_FALSE(sim.sched->RunPending());
    sim.Advance(1000);  // 1000 ns = 268 ticks: past 100 and 200, not 300
    CHECK(sim.sched->RunPending());
    CHECK(ran == std::vector<int>{1, 2, 22});
    sim.Advance(1000);
    CHECK(sim.sched->RunPending());
    CHECK(ran == std::vector<int>{1, 2, 22, 3});
    // An event in the past is run at the next safe point, not at once.
    sim.sched->ScheduleAt(5, [&] { ran.push_back(5); });
    CHECK(ran.size() == 4);
    CHECK(sim.sched->RunPending());
    CHECK(ran.back() == 5);
}

TEST_CASE("RunPending runs posted work and due timers without any fiber") {
    Sim sim;
    int posted = 0, timer = 0;
    sim.kernel->Post([&] { posted++; });
    sim.kernel->ScheduleAt(sim.kernel->NowTicks() + NsToTicks(1 * kMs), [&] { timer++; });
    CHECK(sim.sched->RunPending());
    CHECK(posted == 1);
    CHECK(timer == 0);
    sim.Advance(1 * kMs);
    CHECK(sim.sched->RunPending());
    CHECK(timer == 1);
    CHECK_FALSE(sim.sched->RunPending());
}

TEST_CASE("a paused game clock stops timers; resuming starts them again from where they were") {
    Sim sim;
    int fired = 0;
    sim.kernel->ScheduleAt(NsToTicks(1 * kMs), [&] { fired++; });
    sim.clock->Pause();
    sim.Advance(10 * kMs);
    CHECK_FALSE(sim.sched->RunPending());
    CHECK(fired == 0);
    sim.clock->Resume();
    sim.Advance(1 * kMs);
    CHECK(sim.sched->RunPending());
    CHECK(fired == 1);
}

// ---- cores and slices --------------------------------------------------------------------------------

TEST_CASE("two cores, each with a busy thread, both make progress in slices") {
    Sim sim;  // slice 1000 us
    std::string order;
    for (int core = 0; core < 2; core++) {
        sim.Spawn(30, core, [&, core] {
            for (int i = 0; i < 30; i++) {
                sim.Advance(100 * kUs);  // guest code takes time...
                order.push_back(core == 0 ? 'A' : 'B');
                sim.sched->PreemptPoint();  // ...and RC_TICK looks at the flag
            }
        });
    }
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    // 10 iterations of 100 us fill a slice; the flag is raised when the clock reaches its end.
    const std::string slice_a(10, 'A'), slice_b(10, 'B');
    CHECK(order == slice_a + slice_b + slice_a + slice_b + slice_a + slice_b);
    const SchedulerCounters& counters = sim.sched->Counters();
    // A to B and back at each of the five slice ends in the sequence above, then the last slice of
    // B ends while A still holds its core (6), and A, which has nothing left to do, ends (7).
    CHECK(counters.core_switches == 7);
    CHECK(counters.preemptions >= 5);
    CHECK(counters.guest_ns.at(1) == 3000 * kUs);
    CHECK(counters.guest_ns.at(2) == 3000 * kUs);
}

TEST_CASE("the slice length is configurable") {
    Sim sim(false, /*slice_us=*/500);
    std::string order;
    for (int core = 0; core < 2; core++) {
        sim.Spawn(30, core, [&, core] {
            for (int i = 0; i < 10; i++) {
                sim.Advance(100 * kUs);
                order.push_back(core == 0 ? 'A' : 'B');
                sim.sched->PreemptPoint();
            }
        });
    }
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(order == "AAAAABBBBBAAAAABBBBB");
}

TEST_CASE("a thread that becomes ready on an idle core runs at once, without waiting for the slice") {
    Sim sim;
    sim.Spawn(30, 0, [&] {
        sim.Log("A1");
        sim.Spawn(10, 1, [&] { sim.Log("B"); });
        sim.sched->PreemptPoint();  // the flag says core 1 has something to run
        sim.Log("A2");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"A1", "B", "A2"});
}

TEST_CASE("a core with a thread that waits does not hold the other back") {
    Sim sim;
    // Core 0 sleeps; core 1 runs meanwhile, then the sleeper wakes and (equal slice rules) goes on.
    sim.Spawn(30, 0, [&] {
        sim.Log("A sleeps");
        sim.sched->Sleep(5 * kMs);
        sim.Log("A wakes");
    });
    sim.Spawn(30, 1, [&] {
        sim.Log("B1");
        sim.Advance(1 * kMs);
        sim.sched->PreemptPoint();
        sim.Log("B2");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"A sleeps", "B1", "B2", "A wakes"});
}

// ---- host work -----------------------------------------------------------------------------------------

TEST_CASE("Post from another host thread wakes a waiting fiber") {
    Sim sim(/*real_time=*/true);
    auto ev = sim.kernel->NewEvent(ResetType::OneShot);
    std::atomic<bool> waiting{false}, woke{false};
    WaitOutcome outcome;
    std::thread::id ran_on;
    sim.Spawn(20, 0, [&] {
        waiting = true;  // nothing can run between this and the block: the post is applied after it
        outcome = sim.sched->Block(sim.Self(), {ev}, false, -1);
        woke = true;
    });
    std::thread poster([&] {
        while (!waiting.load()) {
            std::this_thread::yield();
        }
        sim.kernel->Post([&] {
            ran_on = std::this_thread::get_id();
            ev->Signal();
        });
    });
    const RunStatus status = sim.sched->Run([&] { return woke.load(); });
    poster.join();
    CHECK(status == RunStatus::Stopped);
    CHECK(woke.load());
    CHECK(outcome.blocked);
    CHECK(outcome.result == kResultSuccess);
    CHECK(outcome.index == 0);
    CHECK(ran_on == std::this_thread::get_id());  // the function ran on the scheduler's host thread
}

TEST_CASE("Post while a fiber runs raises the flag and the work runs at its next check") {
    Sim sim;
    int posted = 0;
    bool flag_after_post = false, flag_after_check = true;
    sim.Spawn(20, 0, [&] {
        sim.kernel->Post([&] { posted++; });
        flag_after_post = *sim.sched->PreemptFlag() != 0;
        sim.sched->PreemptPoint();
        flag_after_check = *sim.sched->PreemptFlag() != 0;
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(flag_after_post);
    CHECK_FALSE(flag_after_check);
    CHECK(posted == 1);
}

TEST_CASE("the timer thread raises the flag at the deadline and guest code sees it") {
    Sim sim(/*real_time=*/true);
    std::atomic<bool> fired{false};
    bool timed_out = false;
    sim.Spawn(20, 0, [&] {
        // A kernel event 3 ms of game time away, while the guest spins on its preemption checks. Only
        // the timer thread can make them call Preempt().
        sim.sched->ScheduleAt(sim.clock->Now() + NsToTicks(3 * kMs), [&] { fired = true; });
        const s64 give_up = sim.steady.NowNs() + 20000 * kMs;
        while (!fired.load()) {
            sim.sched->PreemptPoint();
            if (sim.steady.NowNs() > give_up) {
                timed_out = true;
                break;
            }
        }
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK_FALSE(timed_out);
    CHECK(fired.load());
}

TEST_CASE("the timer thread source raises once per request and stops cleanly") {
    SteadyTimeSource source;
    auto timer = MakeTimerThreadPreemption(source);
    std::mutex mutex;
    std::condition_variable cv;
    int raised = 0;
    timer->Start([&] {
        {
            std::lock_guard lock(mutex);
            raised++;
        }
        cv.notify_all();
    });
    timer->Arm(source.NowNs() + 1000000);  // 1 ms
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return raised >= 1; }));
        CHECK(raised == 1);
    }
    // A request that is cancelled before its time never fires; one in the past fires at once.
    timer->Arm(source.NowNs() + 3600ll * 1000000000);
    timer->Arm(std::nullopt);
    timer->Arm(source.NowNs() - 1);
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return raised >= 2; }));
        CHECK(raised == 2);
    }
    timer->Stop();
    timer->Stop();  // idempotent
    timer->Arm(source.NowNs());
    CHECK(raised == 2);
}

// ---- counters, hooks, running ------------------------------------------------------------------------

TEST_CASE("counters: guest time per thread, SVCs by number, services by name, idle time") {
    Sim sim;
    sim.Spawn(20, 0, [&] {
        sim.Advance(1 * kMs);  // guest
        sim.sched->BeginSvc(0x0A);
        sim.Advance(2 * kMs);  // in the SVC
        sim.sched->BeginService("srv:");
        sim.Advance(3 * kMs);  // in the service
        sim.sched->EndService();
        sim.Advance(4 * kMs);  // in the SVC again
        sim.sched->EndSvc();
        sim.Advance(5 * kMs);  // guest
        sim.sched->BeginSvc(0x0A);
        sim.sched->EndSvc();
        sim.sched->Sleep(7 * kMs);  // idle: no other thread
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    const SchedulerCounters& counters = sim.sched->Counters();
    CHECK(counters.guest_ns.at(1) == 6 * kMs);
    CHECK(counters.svc[0x0A].ns == 6 * kMs);
    CHECK(counters.svc[0x0A].calls == 2);
    CHECK(counters.svc[0x0B].calls == 0);
    REQUIRE(counters.services.count("srv:") == 1);
    CHECK(counters.services.at("srv:").ns == 3 * kMs);
    CHECK(counters.services.at("srv:").calls == 1);
    // The idle wait skipped to the wake-up deadline, 7 ms of game time rounded up to a host ns.
    CHECK(counters.idle_ns >= 6999990);
    CHECK(counters.idle_ns <= 7000010);
}

TEST_CASE("safe-point hooks run at every SVC and preemption check, and can stop the run") {
    Sim sim;
    int hooks = 0, in_svc = -1;
    sim.sched->OnSafePoint([&] { hooks++; });
    int iterations = 0;
    sim.Spawn(20, 0, [&] {
        const int before = hooks;
        sim.sched->BeginSvc(0x01);
        in_svc = hooks - before;
        sim.sched->EndSvc();
        for (; iterations < 100; iterations++) {
            sim.sched->Sleep(1 * kMs);
        }
    });
    sim.sched->OnSafePoint([&] {
        if (hooks >= 10) {
            sim.sched->RequestStop();
        }
    });
    CHECK(sim.sched->Run() == RunStatus::Stopped);
    CHECK(in_svc == 1);
    CHECK(hooks >= 10);
    CHECK(iterations < 100);
    // Continue where it stopped.
    sim.sched->ClearSafePointHooks();
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(iterations == 100);
}

TEST_CASE("Run stops when the predicate holds and can be called again") {
    Sim sim;
    int count = 0;
    sim.Spawn(20, 0, [&] {
        for (int i = 0; i < 5; i++) {
            count++;
            sim.sched->Sleep(1 * kMs);
        }
    });
    CHECK(sim.sched->Run([&] { return count >= 2; }) == RunStatus::Stopped);
    CHECK(count == 2);
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(count == 5);
    // A stop request made before Run is honoured by the next Run, once.
    sim.sched->RequestStop();
    CHECK(sim.sched->Run({}, true) == RunStatus::Stopped);
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
}

TEST_CASE("RequestStop from a fiber returns control to the host stack") {
    Sim sim;
    int step = 0;
    sim.Spawn(20, 0, [&] {
        step = 1;
        sim.sched->RequestStop();
        sim.sched->PreemptPoint();  // yields to Run
        step = 2;
    });
    CHECK(sim.sched->Run() == RunStatus::Stopped);
    CHECK(step == 1);
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(step == 2);
}

TEST_CASE("a failure in a fiber ends Run with Failed") {
    Sim sim;
    auto thread = sim.kernel->NewThread("bad", 20, 0, sim.process);
    sim.sched->Attach(thread, [](GuestThread& guest) { guest.Fail("boom"); });
    sim.sched->Start(thread);
    CHECK(sim.sched->Run() == RunStatus::Failed);
    REQUIRE(sim.host->Failure() != nullptr);
    CHECK(sim.host->Failure()->message == "boom");
}

TEST_CASE("Run returns Idle only when nothing is runnable or pending") {
    Sim sim;
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);  // no threads at all
    bool fired = false;
    sim.kernel->ScheduleAt(sim.kernel->NowTicks() + NsToTicks(2 * kMs), [&] { fired = true; });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);  // waits for the timer first
    CHECK(fired);
    CHECK(sim.clock->Now() >= NsToTicks(2 * kMs));
}

// ---- ending threads ----------------------------------------------------------------------------------

TEST_CASE("Terminate takes a ready or waiting thread out of the scheduler") {
    Sim sim;
    auto never = sim.kernel->NewEvent(ResetType::OneShot);
    std::shared_ptr<Thread> waiter, ready;
    waiter = sim.Spawn(30, 0, [&] {
        sim.Log("waiter blocks");
        sim.sched->Block(sim.Self(), {never}, false, 100 * kMs);
        sim.Log("waiter woke (must not happen)");
    });
    sim.Spawn(10, 0, [&] {
        // `waiter` has not run yet: terminate it while it is ready, and a second one while it waits.
        ready = sim.Spawn(30, 0, [&] { sim.Log("ready ran (must not happen)"); });
        sim.sched->Terminate(ready);
        sim.sched->Sleep(1 * kMs);  // lets the waiter run and block
        sim.Log("terminating the waiter");
        sim.sched->Terminate(waiter);
        sim.Log("done");
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"waiter blocks", "terminating the waiter", "done"});
    CHECK(waiter->status == ThreadStatus::Dead);
    CHECK(ready->status == ThreadStatus::Dead);
    CHECK(never->GetWaitingThreads().empty());
    // The 100 ms timeout of the dead waiter never fires: nothing is left to wait for.
    CHECK(sim.clock->Now() < NsToTicks(50 * kMs));
}

TEST_CASE("a selection filter keeps a thread back until it is woken") {
    Sim sim;
    bool released = false;
    std::shared_ptr<Thread> held;
    int filter_calls = 0;
    sim.sched->SetSelectionFilter(1, [&](Thread& candidate) {
        filter_calls++;
        if (&candidate == held.get() && !released) {
            candidate.status = ThreadStatus::WaitSleep;  // as Azahar's CpuLimiter
            return true;
        }
        return false;
    });
    held = sim.Spawn(20, 1, [&] { sim.Log("X"); });
    sim.Spawn(30, 1, [&] { sim.Log("Y"); });
    sim.Spawn(50, 0, [&] {
        sim.Log("Z1");
        sim.sched->Sleep(1 * kMs);  // core 0 waits, so core 1 gets the host thread
        sim.Log("Z2");
        released = true;
        held->ResumeFromWait(std::nullopt);
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"Z1", "Y", "Z2", "X"});
    CHECK(filter_calls >= 3);
}

// ---- more rules -------------------------------------------------------------------------------------

TEST_CASE("setting a ready thread's priority moves it to the back of its queue, even if unchanged") {
    Sim sim;
    std::shared_ptr<Thread> b;
    sim.Spawn(10, 0, [&] {
        b = sim.Spawn(40, 0, [&] { sim.Log("B"); });
        sim.Spawn(40, 0, [&] { sim.Log("C"); });
        b->SetPriority(40);  // Azahar's ready_queue.move: out and in again, behind C
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim.log == std::vector<std::string>{"C", "B"});

    Sim sim2;
    std::shared_ptr<Thread> d;
    sim2.Spawn(10, 0, [&] {
        d = sim2.Spawn(40, 0, [&] { sim2.Log("D"); });
        sim2.Spawn(30, 0, [&] { sim2.Log("E"); });
        d->SetPriority(20);  // now the best of the ready threads
    });
    CHECK(sim2.sched->Run({}, true) == RunStatus::Idle);
    CHECK(sim2.log == std::vector<std::string>{"D", "E"});
}

TEST_CASE("CurrentThread is the running guest thread, and nothing outside guest code") {
    Sim sim;
    CHECK(sim.kernel->CurrentThread() == nullptr);
    Thread* seen = nullptr;
    std::shared_ptr<Thread> shared;
    auto thread = sim.Spawn(20, 1, [&] {
        seen = sim.kernel->CurrentThread();
        shared = sim.sched->CurrentThreadPtr();
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(seen == thread.get());
    CHECK(shared == thread);
    CHECK(sim.kernel->CurrentThread() == nullptr);
}

TEST_CASE("a better thread readied on the other core's busy thread's core displaces it at once") {
    Sim sim;
    std::string order;
    for (int core = 0; core < 2; core++) {
        sim.Spawn(core == 0 ? 30 : 40, core, [&, core] {
            for (int i = 1; i <= 30; i++) {
                sim.Advance(100 * kUs);
                order.push_back(core == 0 ? 'A' : 'B');
                if (core == 0 && i == 13) {
                    // In A's second slice: F, better than B, is ready on core 1, where B was
                    // running when its slice ended.
                    sim.Spawn(10, 1, [&] { order.push_back('F'); });
                }
                sim.sched->PreemptPoint();
            }
        });
    }
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    // A fills its first slice, B its own; A runs 3 iterations of the next one, then F runs on core 1
    // at once (and ends), B goes on to the end of the slice that began then.
    const std::string expected_start = std::string(10, 'A') + std::string(10, 'B') + "AAAF" +
                                       std::string(10, 'B');
    CHECK(order.substr(0, expected_start.size()) == expected_start);
    CHECK(std::count(order.begin(), order.end(), 'A') == 30);
    CHECK(std::count(order.begin(), order.end(), 'B') == 30);
    CHECK(std::count(order.begin(), order.end(), 'F') == 1);
}

TEST_CASE("while hooks are registered, a running guest gets a check at least once per slice") {
    Sim sim;  // 1 ms slices, a single busy thread: no other core to hand the slice to
    int hooks = 0, delta = -1;
    sim.sched->OnSafePoint([&] { hooks++; });
    sim.Spawn(30, 0, [&] {
        const int baseline = hooks;
        for (int i = 0; i < 50; i++) {
            sim.Advance(100 * kUs);
            sim.sched->PreemptPoint();
        }
        delta = hooks - baseline;
    });
    CHECK(sim.sched->Run({}, true) == RunStatus::Idle);
    CHECK(delta == 5);  // at 1, 2, 3, 4 and 5 ms
}

TEST_CASE("Shutdown lets go of threads that still wait, with their timeouts") {
    Sim sim;
    auto never = sim.kernel->NewEvent(ResetType::OneShot);
    bool blocking = false;
    sim.Spawn(20, 0, [&] {
        blocking = true;
        sim.sched->Block(sim.Self(), {never}, false, 50 * kMs);
    });
    CHECK(sim.sched->Run([&] { return blocking; }) == RunStatus::Stopped);
    CHECK(never->GetWaitingThreads().size() == 1);
    sim.sched->Shutdown();  // the pending 50 ms timeout is cancelled; the fiber is freed unwound
    CHECK(sim.sched->RunPending() == false);
    // The event still lists the thread, which now lives only there; it goes away with the event.
    CHECK(never->GetWaitingThreads().size() == 1);
}
