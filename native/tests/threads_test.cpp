// Derived from Azahar src/core/hle/kernel/thread.cpp, process.cpp, svc.cpp, resource_limit.cpp,
// kernel.cpp and thread.h, src/core/core_timing.h and src/core/hle/service/pm/pm_app.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: thread.h, svc.cpp and kernel.cpp say
// so; the others say GPL-2.0-or-later): the expected values, worked out from their algorithms
// (Process::AllocateThreadLocalStorage for the TLS addresses and their reuse, SVC::CreateThread and
// KernelSystem::CreateThread for the checks, their order and their result codes, SetupMainThread
// and ResetThreadContext for the initial registers, ApplyAppMaxCPUSetting for the CPU settings and
// CpuLimiterMulti for the alternation on core 1), never by running this code.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.cpp, thread.h,
//   svc.cpp, kernel.cpp, pm_app.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp,
//   resource_limit.cpp)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
// PPSSPP Project (thread.h, second notice: Licensed under GPLv2 or any later version)
//
// mh4n_kernel (N1-27): thread creation, thread-local storage and the time limit of application
// threads on core 1, with fibers that run small C++ functions (no code library). Time is driven by
// hand (a ManualTimeSource and a PreemptionSource that raises the flag when the manual time passes
// the armed deadline), so nothing here depends on how fast the machine is, and nothing waits on a
// sleep.
//
// The limiter's sequence, worked out from CpuLimiterMulti (thread.cpp):
//  - base_tick_interval (thread.h:125) = nsToCycles(2'000'000), an int argument, so
//    268111856 * 2000000 / 1000000000 = 536223.712 -> 536223 ticks.
//  - UpdateAppCpuLimit (thread.cpp, near line 636) calls ChangeState(0) when the share is not 0;
//    ChangeState (near line 686) flips curr_state first, and the limiter started in APP ("So that
//    ChangeState starts with SYS"), so the first phase is SYS. In it
//      next_timer = base * (app_cpu_time / 100.f)        (float, converted to s64 by truncation)
//    and in SYS next_timer = base - next_timer; then `if (next_timer > cycles_late) next_timer -=
//    cycles_late`. The event is scheduled that far ahead. OnTick wakes the sleepers and calls
//    ChangeState(cycles_late).
//  - 30 %: 30 / 100.f = 0.3f; 536223 * 0.3f = 160866.9 -> 160866 (APP), SYS = 536223 - 160866 =
//    375357.   25 %: 0.25f exactly; 134055.75 -> 134055 (APP), SYS = 402168.   10 %: 0.1f;
//    53622.3 -> 53622 (APP), SYS = 482601.   50 %: 0.5f; 268111.5 -> 268111, SYS = 268112.
//  - Activation at tick 0 with 30 %: events at 375357 (SYS -> APP), +160866 = 536223 (APP -> SYS),
//    +375357 = 911580, +160866 = 1072446, +375357 = 1447803, +160866 = 1608669.
//    With 25 %: 402168, 536223, 938391, 1072446, 1474614, 1608669.
//  - A manual time of N ns is floor(N * 268111856 / 1e9) ticks; the least N that reaches a tick T is
//    ceil(T * 1e9 / 268111856). When nothing runs the scheduler jumps the manual clock to that N, so
//    a thread that was held wakes at exactly the event's tick.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("threads_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/cpu.h"
#include "mh4n/kernel.h"
#include "mh4n/loader.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/scheduler.h"
#include "mh4n/threads.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr s64 kUs = 1000;
constexpr s64 kMs = 1000 * kUs;
constexpr u32 kCode = 0x00100000;  // one mapped page of "code"

// A PreemptionSource for manual time (see scheduler_test.cpp): raises when the manual clock reaches
// the armed deadline, which is what the timer thread does with real time.
class ManualPreemption final : public PreemptionSource {
public:
    explicit ManualPreemption(ManualTimeSource& source) : source_(source) {
        source_.SetAdvanceHook([this] { Check(); });
    }
    void Start(std::function<void()> raise) override { raise_ = std::move(raise); }
    void Stop() override { raise_ = nullptr; }
    void Arm(std::optional<s64> deadline_ns) override {
        deadline_ = deadline_ns;
        Check();
    }

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

struct World {
    std::unique_ptr<GuestMemory> memory;
    ManualTimeSource manual;
    ManualPreemption preemption{manual};
    std::unique_ptr<GameClock> clock;
    std::unique_ptr<CodeHost> host;
    std::unique_ptr<Kernel> kernel;
    std::unique_ptr<GuestScheduler> sched;
    std::shared_ptr<Process> process;
    std::unique_ptr<ThreadManager> mgr;

    explicit World(u32 memory_mode = 0) {
        SetLogFilter(LogFilter(LogLevel::Critical));  // the refusals under test log at Error
        memory = GuestMemory::Create();
        REQUIRE(memory != nullptr);
        REQUIRE(IsSuccess(memory->Map(kCode, kPageSize, MemPerm::ReadExecute, MemState::Code)));
        clock = std::make_unique<GameClock>(manual);
        host = std::make_unique<CodeHost>(*memory);
        kernel = std::make_unique<Kernel>(*memory, memory_mode);
        sched = std::make_unique<GuestScheduler>(*kernel, *host, *clock, manual, &preemption);
        process = kernel->NewProcess("app");
        mgr = std::make_unique<ThreadManager>(*kernel, *sched);
    }
    // The lifetime order of threads.h: Shutdown(), the manager, the kernel, the scheduler.
    ~World() {
        sched->Shutdown();
        mgr.reset();
        process.reset();
        kernel.reset();
        sched.reset();
        host.reset();
        clock.reset();
    }

    // svcCreateThread of a thread that runs `body` on its fiber and then ends.
    std::shared_ptr<Thread> SpawnG(u32 priority, int core, std::function<void(GuestThread&)> body,
                                   u32 entry = kCode, u32 arg = 0, u32 stack_top = kStackTop) {
        ThreadSpec spec;
        spec.entry_point = entry;
        spec.arg = arg;
        spec.stack_top = stack_top;
        spec.priority = priority;
        spec.processor_id = core;
        spec.host_entry = [this, body = std::move(body)](GuestThread& guest) {
            body(guest);
            sched->ExitCurrent();
        };
        std::shared_ptr<Thread> thread;
        Handle handle = 0;
        REQUIRE(mgr->CreateThread(spec, &handle, &thread) == kResultSuccess);
        return thread;
    }
    std::shared_ptr<Thread> Spawn(u32 priority, int core, std::function<void()> body,
                                  u32 entry = kCode) {
        return SpawnG(priority, core, [body = std::move(body)](GuestThread&) { body(); }, entry);
    }
    Thread& Self() { return *sched->CurrentThread(); }
    void Advance(s64 ns) { manual.Advance(ns); }
    u64 Now() { return clock->Now(); }
    s32 ThreadCount() {
        return process->resource_limit->GetCurrentValue(ResourceLimitType::Thread);
    }
};

// An image like the ones the loaders make, with the CPU setting of the exheader.
ProcessImage MakeImage(u32 schedule_mode, u32 max_cpu, u64 program_id = 0x0004000000123400ull) {
    ProcessImage image;
    image.name = "game";
    image.program_id = program_id;
    image.entry = kCode;
    image.stack_top = kStackTop;
    image.main_thread_priority = 48;
    image.ideal_processor = 0;
    image.kernel_version = 0x23A;
    image.apply_app_max_cpu = true;
    image.core1_schedule_mode = schedule_mode;
    image.core1_max_cpu = max_cpu;
    return image;
}

}  // namespace

// ---- thread-local storage ---------------------------------------------------------------------------

TEST_CASE("TLS slots are 0x200 bytes at 0x1FF82000, eight to a page, in creation order") {
    World w;
    std::vector<std::shared_ptr<Thread>> threads;
    for (int i = 0; i < 10; i++) {
        threads.push_back(w.Spawn(30, 0, [] {}));
    }
    // Thread k (ids from 1) takes slot (k-1) % 8 of page (k-1) / 8:
    //   0x1FF82000 + 0x1000 * page + 0x200 * slot.
    const u32 expected[10] = {0x1FF82000, 0x1FF82200, 0x1FF82400, 0x1FF82600, 0x1FF82800,
                              0x1FF82A00, 0x1FF82C00, 0x1FF82E00, 0x1FF83000, 0x1FF83200};
    for (int i = 0; i < 10; i++) {
        CHECK(threads[i]->thread_id == static_cast<u32>(i + 1));
        CHECK(threads[i]->tls_address == expected[i]);
        CHECK(CommandBufferAddress(*threads[i]) == expected[i] + 0x80);
    }
    CHECK(w.mgr->TlsPageCount() == 2);
    CHECK(w.mgr->TlsSlotInUse(0, 7));
    CHECK(w.mgr->TlsSlotInUse(1, 1));
    CHECK_FALSE(w.mgr->TlsSlotInUse(1, 2));
    CHECK_FALSE(w.mgr->TlsSlotInUse(2, 0));
}

TEST_CASE("TLS pages come from the BASE region, first fit from the bottom, mapped locked") {
    World w;
    FcramRegions& regions = w.kernel->Regions();
    const u32 base = regions.Base(FcramRegion::Base);
    CHECK(regions.Used(FcramRegion::Base) == 0);
    for (int i = 0; i < 9; i++) {
        w.Spawn(30, 0, [] {});
    }
    // Two pages: the first free page of the BASE region and the one after it (nothing is freed in
    // between), 0x1000 bytes each. The free list starts after them.
    CHECK(regions.Used(FcramRegion::Base) == 0x2000);
    const std::vector<FcramRange> free_blocks = regions.FreeBlocks(FcramRegion::Base);
    REQUIRE_FALSE(free_blocks.empty());
    CHECK(free_blocks.front().offset == base + 0x2000);
    CHECK(w.process->memory_used == 0x2000);
    // Both pages are mapped read/write, state Locked, and as their FCRAM is contiguous they are one
    // block for svcQueryMemory (Azahar's VMAs merge when their backing memory is contiguous).
    const auto info = w.memory->Query(0x1FF82000);
    REQUIRE(info.has_value());
    CHECK(info->base == 0x1FF82000);
    CHECK(info->size == 0x2000);
    CHECK(info->perm == MemPerm::ReadWrite);
    CHECK(info->state == MemState::Locked);
    CHECK_FALSE(w.memory->IsMapped(0x1FF84000, 1));
}

TEST_CASE("the first free slot is reused when a thread ends, and its memory is cleared") {
    World w;
    std::vector<std::shared_ptr<Thread>> threads;
    for (int i = 0; i < 9; i++) {
        threads.push_back(w.Spawn(30, 0, [] {}));
    }
    // Page 0 is full, page 1 has slot 0. Thread 3 (slot 2) and thread 6 (slot 5) end.
    CHECK(threads[8]->tls_address == 0x1FF83000);
    REQUIRE(w.memory->Write<u32>(0x1FF82400, 0xAABBCCDD));
    REQUIRE(w.memory->Write<u32>(0x1FF82400 + 0x1FC, 0x11223344));  // the slot's last word
    REQUIRE(w.memory->Write<u32>(0x1FF82600, 0x55555555));          // the next slot: not touched
    w.sched->Terminate(threads[2]);
    w.sched->Terminate(threads[5]);
    CHECK_FALSE(w.mgr->TlsSlotInUse(0, 2));
    CHECK_FALSE(w.mgr->TlsSlotInUse(0, 5));
    CHECK(w.mgr->TlsSlotInUse(0, 3));

    // The next threads take the free slots of page 0, lowest first, then slot 1 of page 1.
    const auto a = w.Spawn(30, 0, [] {});
    const auto b = w.Spawn(30, 0, [] {});
    const auto c = w.Spawn(30, 0, [] {});
    CHECK(a->tls_address == 0x1FF82400);
    CHECK(b->tls_address == 0x1FF82A00);
    CHECK(c->tls_address == 0x1FF83200);
    CHECK(w.mgr->TlsPageCount() == 2);
    // The slot is zeroed when it is given out again; its neighbour keeps what it had.
    CHECK(w.memory->Read<u32>(0x1FF82400) == 0);
    CHECK(w.memory->Read<u32>(0x1FF82400 + 0x1FC) == 0);
    CHECK(w.memory->Read<u32>(0x1FF82600) == 0x55555555);
    // Thread ids are never reused.
    CHECK(a->thread_id == 10);
    CHECK(b->thread_id == 11);
    CHECK(c->thread_id == 12);
}


// ---- the main thread and the initial context ----------------------------------------------------------

TEST_CASE("the main thread: id 1, the image's priority, processor and FPSCR, ready at the entry") {
    World w;
    std::shared_ptr<Thread> main_thread;
    RCtx regs{};
    bool ran = false;
    ProcessImage image = MakeImage(1, 89);
    image.ideal_processor = 1;
    image.main_thread_fpscr = kMainThreadFpscr;
    REQUIRE(w.mgr->CreateMainThread(image, &main_thread, [&](GuestThread& guest) {
        regs = guest.Context();
        ran = true;
        w.sched->ExitCurrent();
    }) == kResultSuccess);

    // Process::Run reserves one thread; SetupMainThread uses the process's ideal processor.
    CHECK(w.ThreadCount() == 1);
    CHECK(w.process->status == ProcessStatus::Running);
    CHECK(w.process->kernel_version == 0x23A);
    CHECK(w.process->ideal_processor == 1);
    CHECK(main_thread->thread_id == 1);
    CHECK(main_thread->Name() == "app-main");
    CHECK(main_thread->nominal_priority == 48);
    CHECK(main_thread->current_priority == 48);
    CHECK(main_thread->processor_id == 1);
    CHECK(main_thread->status == ThreadStatus::Ready);
    CHECK(main_thread->entry_point == kCode);
    CHECK(main_thread->stack_top == 0x10000000);
    CHECK(main_thread->tls_address == 0x1FF82000);
    CHECK(w.sched->ReadyCount(1) == 1);

    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    REQUIRE(ran);
    // ResetThreadContext: r0 = the argument (0 for the main thread), sp = the stack top, pc = the
    // entry, user mode, ARM state; the main thread's FPSCR is 0x03C00010.
    CHECK(regs.r[0] == 0);
    CHECK(regs.r[13] == 0x10000000);
    CHECK(regs.r[14] == 0);
    CHECK(regs.r[15] == kCode);
    CHECK(regs.cpsr_misc == 0x10);
    CHECK(regs.fpscr == 0x03C00010);
    CHECK(regs.tpidruro == 0x1FF82000);
    CHECK(regs.tpidrurw == 0);
    for (int i = 1; i < 13; i++) {
        CHECK(regs.r[i] == 0);
    }
    // The thread ended: its resource limit slot and its TLS slot are free.
    CHECK(w.ThreadCount() == 0);
    CHECK_FALSE(w.mgr->TlsSlotInUse(0, 0));
}

TEST_CASE("the main thread is refused with a priority above 63, a processor that does not exist, "
          "or an entry that is not mapped") {
    {
        World w;
        ProcessImage image = MakeImage(1, 89);
        image.main_thread_priority = -1;  // Azahar passes it on as a u32
        CHECK(w.mgr->CreateMainThread(image) == 0xE0E01BFD);
    }
    {
        World w;
        ProcessImage image = MakeImage(1, 89);
        image.ideal_processor = 2;
        CHECK(w.mgr->CreateMainThread(image) == 0xE0E01BFD);
    }
    {
        World w;
        ProcessImage image = MakeImage(1, 89);
        image.entry = 0x00300000;
        CHECK(w.mgr->CreateMainThread(image) == 0xD8E007F5);
    }
}

TEST_CASE("svcCreateThread: r0 the argument, sp the stack top, pc the entry, FPSCR 0x03C00000, "
          "Thumb state from bit 0 of the entry") {
    World w;
    RCtx arm{}, thumb{};
    const auto a = w.SpawnG(30, 0, [&](GuestThread& g) { arm = g.Context(); }, kCode + 0x10,
                            0x12345678, 0x0FFF0000);
    const auto b = w.SpawnG(31, 0, [&](GuestThread& g) { thumb = g.Context(); }, kCode + 0x21,
                            0xCAFEBABE, 0x0FFE0000);
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);

    CHECK(arm.r[0] == 0x12345678);
    CHECK(arm.r[13] == 0x0FFF0000);
    CHECK(arm.r[15] == kCode + 0x10);
    CHECK(arm.cpsr_misc == 0x10);  // USER32MODE, ARM
    CHECK(arm.fpscr == 0x03C00000);
    CHECK(arm.tpidruro == a->tls_address);
    CHECK(thumb.r[0] == 0xCAFEBABE);
    CHECK(thumb.r[13] == 0x0FFE0000);
    CHECK(thumb.r[15] == kCode + 0x21);  // the Thumb bit stays in the entry
    CHECK(thumb.cpsr_misc == 0x30);      // USER32MODE | T
    CHECK(thumb.fpscr == 0x03C00000);
    CHECK(thumb.tpidruro == b->tls_address);
    CHECK(a->tls_address != b->tls_address);
    CHECK(a->entry_point == kCode + 0x10);
    CHECK(a->stack_top == 0x0FFF0000);
}

TEST_CASE("thread ids: from 1 in creation order with the main thread first, never reused") {
    World w;
    std::shared_ptr<Thread> main_thread;
    REQUIRE(w.mgr->CreateMainThread(MakeImage(1, 89), &main_thread) == kResultSuccess);
    const auto t2 = w.Spawn(30, 0, [] {});
    const auto t3 = w.Spawn(30, 0, [] {});
    CHECK(main_thread->thread_id == 1);
    CHECK(t2->thread_id == 2);
    CHECK(t3->thread_id == 3);
    w.sched->Terminate(t2);
    w.sched->Terminate(t3);
    const auto t4 = w.Spawn(30, 0, [] {});
    CHECK(t4->thread_id == 4);
    CHECK(t4->tls_address == t2->tls_address);  // the slot was reused, the id was not
}

TEST_CASE("svcCreateThread returns a handle in the process's table to the thread") {
    World w;
    ThreadSpec spec;
    spec.entry_point = kCode;
    spec.stack_top = kStackTop;
    spec.priority = 30;
    spec.processor_id = 0;
    Handle first = 0, second = 0;
    std::shared_ptr<Thread> t1, t2;
    CHECK(w.mgr->CreateThread(spec, &first, &t1) == kResultSuccess);
    CHECK(w.mgr->CreateThread(spec, &second, &t2) == kResultSuccess);
    CHECK(first == 0x00000001);   // generation 1, slot 0
    CHECK(second == 0x00008002);  // generation 2, slot 1
    CHECK(w.kernel->GetObject<Thread>(first) == t1);
    CHECK(w.kernel->GetObject<Thread>(second) == t2);
    CHECK(t1->status == ThreadStatus::Ready);
    CHECK(t1->owner_process.lock() == w.process);
}

TEST_CASE("a full handle table fails svcCreateThread after the thread exists, as in Azahar") {
    World w;
    for (int i = 0; i < 4096; i++) {
        Handle h = 0;
        REQUIRE(w.kernel->AddHandle(&h, w.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    }
    ThreadSpec spec;
    spec.entry_point = kCode;
    spec.stack_top = kStackTop;
    spec.priority = 30;
    spec.processor_id = 0;
    Handle handle = 0xDEAD;
    std::shared_ptr<Thread> thread;
    CHECK(w.mgr->CreateThread(spec, &handle, &thread) == 0xD8600413);
    CHECK(handle == 0xDEAD);
    REQUIRE(thread != nullptr);  // SVC::CreateThread made it ready before it asked for the handle
    CHECK(thread->status == ThreadStatus::Ready);
    CHECK(thread->thread_id == 1);
}

// ---- the checks of svcCreateThread --------------------------------------------------------------------

namespace {

Result Create(World& w, u32 priority, s32 processor_id, u32 entry = kCode, Handle* out = nullptr) {
    ThreadSpec spec;
    spec.entry_point = entry;
    spec.stack_top = kStackTop;
    spec.priority = priority;
    spec.processor_id = processor_id;
    Handle handle = 0xDEAD;
    const Result result = w.mgr->CreateThread(spec, &handle);
    if (IsError(result)) {
        CHECK(handle == 0xDEAD);  // nothing is written on an error
    }
    if (out != nullptr) {
        *out = handle;
    }
    return result;
}

}  // namespace

TEST_CASE("CreateThread: a priority above 63 is out of range (0xE0E01BFD)") {
    World w;
    CHECK(Create(w, 64, 0) == 0xE0E01BFD);
    CHECK(Create(w, 0xFFFFFFFF, 0) == 0xE0E01BFD);
    CHECK(Create(w, 63, 0) == kResultSuccess);
    CHECK(w.ThreadCount() == 1);  // the refused calls reserved nothing
}

TEST_CASE("CreateThread: a better priority than the resource limit's is not authorised "
          "(0xD9001BEA), unless the process has no thread restrictions") {
    World w;
    // The Application limit's Priority value is 0x18: priorities 0..23 are better than a process may
    // have.
    REQUIRE(w.process->resource_limit->GetLimitValue(ResourceLimitType::Priority) == 0x18);
    CHECK(Create(w, 23, 0) == 0xD9001BEA);
    CHECK(Create(w, 0, 0) == 0xD9001BEA);
    CHECK(Create(w, 24, 0) == kResultSuccess);  // equal is allowed
    CHECK(w.ThreadCount() == 1);

    w.mgr->SetNoThreadRestrictions(true);  // PROCESSOP_DISABLE_CREATE_THREAD_RESTRICTIONS
    CHECK(Create(w, 0, 0) == kResultSuccess);
    CHECK(Create(w, 23, 1) == kResultSuccess);
    CHECK(Create(w, 64, 0) == 0xE0E01BFD);  // the range check comes first and stays
    w.mgr->SetNoThreadRestrictions(false);
    CHECK(Create(w, 5, 0) == 0xD9001BEA);
}

TEST_CASE("CreateThread: processor -2 is the ideal processor, -1 is core 0, 0 and 1 are valid") {
    World w;
    Handle h = 0;
    std::shared_ptr<Thread> thread;
    ThreadSpec spec;
    spec.entry_point = kCode;
    spec.stack_top = kStackTop;
    spec.priority = 30;

    spec.processor_id = kThreadProcessorIdDefault;
    w.process->ideal_processor = 0;
    REQUIRE(w.mgr->CreateThread(spec, &h, &thread) == kResultSuccess);
    CHECK(thread->processor_id == 0);
    w.process->ideal_processor = 1;
    REQUIRE(w.mgr->CreateThread(spec, &h, &thread) == kResultSuccess);
    CHECK(thread->processor_id == 1);

    spec.processor_id = kThreadProcessorIdAll;
    REQUIRE(w.mgr->CreateThread(spec, &h, &thread) == kResultSuccess);
    CHECK(thread->processor_id == 0);  // "for now run in core 0"
    spec.processor_id = 0;
    REQUIRE(w.mgr->CreateThread(spec, &h, &thread) == kResultSuccess);
    CHECK(thread->processor_id == 0);
    spec.processor_id = 1;
    REQUIRE(w.mgr->CreateThread(spec, &h, &thread) == kResultSuccess);
    CHECK(thread->processor_id == 1);
}

TEST_CASE("CreateThread: processors 2 and 3, 4 and above, and below -2 are out of range") {
    World w;
    // 2 and 3 are New 3DS cores: Azahar lets them through and then indexes a core that an Old 3DS
    // does not have; here they are out of range. From 4 up and below -2 is Azahar's default case.
    for (const s32 processor : {2, 3, 4, 5, 255, INT_MAX, -3, -100, INT_MIN}) {
        CAPTURE(processor);
        CHECK(Create(w, 30, processor) == 0xE0E01BFD);
    }
    CHECK(w.ThreadCount() == 0);
    CHECK(w.sched->ReadyCount(0) == 0);
}

TEST_CASE("CreateThread: an entry address that is not mapped is refused (0xD8E007F5)") {
    World w;
    CHECK(Create(w, 30, 0, 0x00200000) == 0xD8E007F5);
    CHECK(Create(w, 30, 0, 0) == 0xD8E007F5);
    CHECK(Create(w, 30, 0, 0xFFFFFFFF) == 0xD8E007F5);
    CHECK(Create(w, 30, 0, 0x50000001) == 0xD8E007F5);  // outside the managed range, Thumb bit set
    // Any byte of a mapped page will do, the Thumb bit included.
    std::shared_ptr<Thread> a, b;
    ThreadSpec spec;
    spec.stack_top = kStackTop;
    spec.priority = 30;
    spec.processor_id = 0;
    Handle h = 0;
    spec.entry_point = kCode + 0xFFF;
    CHECK(w.mgr->CreateThread(spec, &h, &a) == kResultSuccess);
    spec.entry_point = kCode + 0x1;
    CHECK(w.mgr->CreateThread(spec, &h, &b) == kResultSuccess);

    // SVC::CreateThread reserves the thread before KernelSystem::CreateThread looks at the entry
    // and does not give it back: the four refused calls cost four slots of the limit, the two that
    // worked two more. The refused ones did not take an id (the id comes after the entry check).
    CHECK(w.ThreadCount() == 6);
    CHECK(a->thread_id == 1);
    CHECK(b->thread_id == 2);
}

TEST_CASE("CreateThread: the checks come in Azahar's order") {
    World w;
    // Everything is wrong: the priority comes first.
    CHECK(Create(w, 64, 9, 0) == 0xE0E01BFD);
    // A bad processor and an unmapped entry with a priority that is not authorised: authorisation.
    CHECK(Create(w, 10, 9, 0) == 0xD9001BEA);
    // A bad processor and an unmapped entry: the processor.
    CHECK(Create(w, 30, 9, 0) == 0xE0E01BFD);
    // Only the entry is wrong.
    CHECK(Create(w, 30, 1, 0) == 0xD8E007F5);
}

TEST_CASE("the resource limit counts threads: reserved on creation, given back when they end; "
          "the limit itself never refuses (Azahar's Reserve always succeeds)") {
    std::vector<std::string> errors;
    SetLogSink([&](const LogRecord& r) {
        if (r.level >= LogLevel::Error) {
            errors.emplace_back(r.message);
        }
    });
    {
        World w;
        SetLogFilter(LogFilter(LogLevel::Error));
        const auto& limit = *w.process->resource_limit;
        CHECK(limit.GetLimitValue(ResourceLimitType::Thread) == 0x20);
        std::vector<std::shared_ptr<Thread>> threads;
        for (int i = 0; i < 0x22; i++) {
            threads.push_back(w.Spawn(30, 0, [] {}));
        }
        // Past the limit of 32: Azahar logs and goes on.
        CHECK(w.ThreadCount() == 0x22);
        CHECK(errors.size() == 2);
        w.sched->Terminate(threads[0]);
        w.sched->Terminate(threads[1]);
        CHECK(w.ThreadCount() == 0x20);
        // A thread that is terminated twice gives back once.
        w.sched->Terminate(threads[1]);
        CHECK(w.ThreadCount() == 0x20);
    }
    SetLogSink({});
}

// ---- the CPU setting of the application ----------------------------------------------------------------

TEST_CASE("ApplyAppMaxCPUSetting: the exheader's mode and limit, or the 1.0 defaults and overrides") {
    using M = Core1ScheduleMode;
    // The title is (program id >> 8) & 0xFFFFF: program id 0x0004000000123400 is title 0x01234.
    constexpr u64 kUnknown = 0x0004000000123400ull;
    // The exheader's fields: mode 0 is Single, anything else Multi; the share is the limit value and
    // the current value stays 0 until PM/APT set it.
    CHECK(ComputeAppMaxCpuSetting(kUnknown, 1, 89) == Core1CpuSetting{M::Multi, 89, 0});
    CHECK(ComputeAppMaxCpuSetting(kUnknown, 0, 30) == Core1CpuSetting{M::Single, 30, 0});
    CHECK(ComputeAppMaxCpuSetting(kUnknown, 1, 0) == Core1CpuSetting{M::Multi, 0, 0});
    CHECK(ComputeAppMaxCpuSetting(kUnknown, 2, 50) == Core1CpuSetting{M::Multi, 50, 0});
    // A 1.0 title with nothing set: Single, 80 % as the limit, 25 % as the share.
    CHECK(ComputeAppMaxCpuSetting(kUnknown, 0, 0) == Core1CpuSetting{M::Single, 80, 25});
    // ... unless the real 1.0 kernel has another value for it: exempt (10000) or 30 %.
    CHECK(ComputeAppMaxCpuSetting(0x0004000000020500ull, 0, 0) ==
          Core1CpuSetting{M::Single, 10000, 10000});  // title 0x205, 3DS sound
    CHECK(ComputeAppMaxCpuSetting(0x0004000000033400ull, 0, 0) ==
          Core1CpuSetting{M::Single, 30, 30});  // title 0x334, Ocarina of Time 3D
    CHECK(ComputeAppMaxCpuSetting(0x0004000000049100ull, 0, 0) ==
          Core1CpuSetting{M::Single, 10000, 10000});  // title 0x491, Star Fox 64 3D
    CHECK(ComputeAppMaxCpuSetting(0x0004000000056200ull, 0, 0) ==
          Core1CpuSetting{M::Single, 10000, 10000});  // title 0x562
    // The table is for 1.0 titles with no value only.
    CHECK(ComputeAppMaxCpuSetting(0x0004000000033400ull, 1, 40) == Core1CpuSetting{M::Multi, 40, 0});
}

TEST_CASE("CreateMainThread applies the image's CPU setting; the share stays off until APT sets it") {
    SUBCASE("an ELF program: (1, 89)") {
        World w;
        REQUIRE(w.mgr->CreateMainThread(MakeImage(1, 89)) == kResultSuccess);
        const auto& limit = *w.process->resource_limit;
        CHECK(limit.GetLimitValue(ResourceLimitType::CpuTime) == 89);
        CHECK(limit.GetCurrentValue(ResourceLimitType::CpuTime) == 0);
        CHECK(w.mgr->ScheduleMode() == Core1ScheduleMode::Multi);
        CHECK(w.mgr->Core1LimiterReady());
        CHECK_FALSE(w.mgr->Core1LimiterActive());
        CHECK(w.mgr->AppCpuTimeLimit() == 0);
    }
    SUBCASE("a 1.0 title with nothing set starts limited at 25 %") {
        World w;
        REQUIRE(w.mgr->CreateMainThread(MakeImage(0, 0)) == kResultSuccess);
        const auto& limit = *w.process->resource_limit;
        CHECK(limit.GetLimitValue(ResourceLimitType::CpuTime) == 80);
        CHECK(limit.GetCurrentValue(ResourceLimitType::CpuTime) == 25);
        CHECK(w.mgr->ScheduleMode() == Core1ScheduleMode::Single);
        CHECK(w.mgr->Core1LimiterActive());
        CHECK(w.mgr->Core1CurrentPhase() == Core1Phase::Sys);
        CHECK(w.mgr->AppCpuTimeLimit() == 25);
    }
    SUBCASE("an image that does not ask for it (not the application category)") {
        World w;
        ProcessImage image = MakeImage(1, 89);
        image.apply_app_max_cpu = false;
        REQUIRE(w.mgr->CreateMainThread(image) == kResultSuccess);
        CHECK(w.process->resource_limit->GetLimitValue(ResourceLimitType::CpuTime) == 0);
        CHECK_FALSE(w.mgr->Core1LimiterReady());
        CHECK_FALSE(w.mgr->Core1LimiterActive());
    }
}

TEST_CASE("SetAppCpuTimeLimit: a share up to the limit value is taken and the limiter follows") {
    World w;
    // Before any setting the limit value is 0 (PREEMPTION_DISABLED): nothing above it is taken.
    CHECK(w.mgr->SetAppCpuTimeLimit(30) == kResultSuccess);
    CHECK(w.mgr->AppCpuTimeLimit() == 0);

    REQUIRE(w.mgr->CreateMainThread(MakeImage(1, 89), nullptr, [&](GuestThread&) {
        w.sched->ExitCurrent();
    }) == kResultSuccess);
    CHECK(w.mgr->SetAppCpuTimeLimit(90) == kResultSuccess);  // above 89: ignored, still a success
    CHECK(w.mgr->AppCpuTimeLimit() == 0);
    CHECK_FALSE(w.mgr->Core1LimiterActive());

    CHECK(w.mgr->SetAppCpuTimeLimit(89) == kResultSuccess);
    CHECK(w.mgr->AppCpuTimeLimit() == 89);
    CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::CpuTime) == 89);
    CHECK(w.mgr->Core1LimiterActive());
    CHECK(w.mgr->Core1CurrentPhase() == Core1Phase::Sys);  // an activation starts in the system phase

    // Setting a share again while active goes through ChangeState again, which flips the phase.
    CHECK(w.mgr->SetAppCpuTimeLimit(50) == kResultSuccess);
    CHECK(w.mgr->Core1CurrentPhase() == Core1Phase::App);
    CHECK(w.mgr->SetAppCpuTimeLimit(50) == kResultSuccess);
    CHECK(w.mgr->Core1CurrentPhase() == Core1Phase::Sys);

    CHECK(w.mgr->SetAppCpuTimeLimit(0) == kResultSuccess);  // off
    CHECK(w.mgr->AppCpuTimeLimit() == 0);
    CHECK_FALSE(w.mgr->Core1LimiterActive());
    CHECK(w.mgr->Core1LimiterReady());
    // Off, with the event cancelled: nothing is pending.
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
}

// ---- the alternation on core 1 --------------------------------------------------------------------------

TEST_CASE("core 1 alternates as CpuLimiterMulti does: 30 % of every 2 ms, system phase first") {
    World w;
    w.mgr->ApplyAppMaxCpuSetting(0x0004000000123400ull, 1, 80);  // Multi, limit 80, share 0
    REQUIRE(w.mgr->SetAppCpuTimeLimit(30) == kResultSuccess);
    CHECK(w.mgr->Core1LimiterActive());
    CHECK(w.mgr->Core1CurrentPhase() == Core1Phase::Sys);

    // A busy thread on core 1: 8 ns of guest time, then the preemption check (RC_TICK).
    //   resumed: the game time at which the thread got the core after being held (its first run
    //            included); held_at: the time of the check that held it back.
    std::vector<u64> resumed, held_at;
    u64 last = 0;
    bool done = false;
    w.Spawn(30, 1, [&] {
        resumed.push_back(w.Now());
        for (;;) {
            w.Advance(8);
            const u64 before = w.Now();
            if (before >= 1608669) {
                last = before;
                break;
            }
            w.sched->PreemptPoint();
            const u64 after = w.Now();
            if (after != before) {
                held_at.push_back(before);
                resumed.push_back(after);
            }
        }
        done = true;
    });
    CHECK(w.sched->Run([&] { return done; }) == RunStatus::Stopped);

    // The events are at 375357, 536223, 911580, 1072446, 1447803, 1608669 (see the top of the
    // file). The thread is held from tick 0 to the first event, runs until the second, is held
    // until the third, and so on: it gets the core exactly at the odd events (the idle scheduler
    // jumps the clock there), and is held at the first check after the even ones. A check comes
    // every 8 ns = 2.14 ticks.
    REQUIRE(resumed.size() == 3);
    CHECK(resumed[0] == 375357);
    CHECK(resumed[1] == 911580);
    CHECK(resumed[2] == 1447803);
    REQUIRE(held_at.size() == 2);
    CHECK(held_at[0] >= 536223);
    CHECK(held_at[0] <= 536223 + 3);
    CHECK(held_at[1] >= 1072446);
    CHECK(held_at[1] <= 1072446 + 3);
    CHECK(last >= 1608669);
    CHECK(last <= 1608669 + 3);
}

TEST_CASE("a limiter event that runs late is made up for in the next interval") {
    World w;
    w.mgr->ApplyAppMaxCpuSetting(0x0004000000123400ull, 1, 80);
    REQUIRE(w.mgr->SetAppCpuTimeLimit(25) == kResultSuccess);  // APP 134055, SYS 402168

    // The thread checks every 130 us = 34854.5 ticks, so the events at 536223 and 1072446 are seen
    // late. ChangeState(cycles_late) shortens the next interval by the delay, so the events stay
    // where the sums put them: 402168, 536223, 938391, 1072446, 1474614, 1608669, and the thread gets
    // the core back exactly at the odd ones.
    std::vector<u64> resumed, held_at;
    u64 last = 0;
    bool done = false;
    w.Spawn(30, 1, [&] {
        resumed.push_back(w.Now());
        for (;;) {
            w.Advance(130 * kUs);
            const u64 before = w.Now();
            if (before >= 1608669) {
                last = before;
                break;
            }
            w.sched->PreemptPoint();
            const u64 after = w.Now();
            if (after != before) {
                held_at.push_back(before);
                resumed.push_back(after);
            }
        }
        done = true;
    });
    CHECK(w.sched->Run([&] { return done; }) == RunStatus::Stopped);

    // The thread gets the core at 1500001 ns = 402168 ticks, 3499999 ns = 938391 and 5499996 ns =
    // 1474614, the ceilings of the event times. From the first of them its checks are at +130 us
    // steps; the first at or past the event at 536223 is at 2020001 ns = 541586 ticks (5363 ticks
    // late), and the one past 1072446 is at 4019999 ns = 1077809 (5363 late). In the last window the
    // checks run 1474614, 1509468, 1544323, 1579177, 1614032 ticks (the thread stops at the first
    // one at or past 1608669).
    REQUIRE(resumed.size() == 3);
    CHECK(resumed[0] == 402168);
    CHECK(resumed[1] == 938391);
    CHECK(resumed[2] == 1474614);
    REQUIRE(held_at.size() == 2);
    CHECK(held_at[0] == 541586);
    CHECK(held_at[1] == 1077809);
    CHECK(last == 1614032);
}

TEST_CASE("a delay longer than the next interval is not made up for") {
    World w;
    w.mgr->ApplyAppMaxCpuSetting(0x0004000000123400ull, 1, 80);
    REQUIRE(w.mgr->SetAppCpuTimeLimit(10) == kResultSuccess);  // APP 53622, SYS 482601

    // Events: 482601 (SYS -> APP), then 482601 + 53622 = 536223 (APP -> SYS). The thread starts at
    // 482601 (1799999 ns) and does not check again until 5 ms later: 6799999 ns = 1823160 ticks,
    // 1286937 ticks after the event was due. ChangeState(1286937) to SYS: next_timer = 482601, which
    // is not greater than the delay, so the delay is not subtracted (`if (next_timer > cycles_late)
    // next_timer -= cycles_late`) and the next event is 482601 ticks after the check: 2305761.
    std::vector<u64> resumed, held_at;
    bool done = false;
    w.Spawn(30, 1, [&] {
        resumed.push_back(w.Now());
        while (resumed.size() < 2) {
            w.Advance(5 * kMs);
            const u64 before = w.Now();
            w.sched->PreemptPoint();
            const u64 after = w.Now();
            if (after != before) {
                held_at.push_back(before);
                resumed.push_back(after);
            }
        }
        done = true;
    });
    CHECK(w.sched->Run([&] { return done; }) == RunStatus::Stopped);
    REQUIRE(resumed.size() == 2);
    REQUIRE(held_at.size() == 1);
    CHECK(resumed[0] == 482601);
    CHECK(held_at[0] == 1823160);
    CHECK(resumed[1] == 2305761);
}

TEST_CASE("only threads on core 1 are held; core 0 runs while core 1 waits for its phase") {
    World w;
    w.mgr->ApplyAppMaxCpuSetting(0x0004000000123400ull, 1, 80);
    REQUIRE(w.mgr->SetAppCpuTimeLimit(50) == kResultSuccess);  // SYS 268112 ticks first
    std::vector<std::string> log;
    u64 core0_at = ~0ull, core1_at = 0;
    w.Spawn(30, 1, [&] {
        core1_at = w.Now();
        log.push_back("core 1");
    });
    w.Spawn(30, 0, [&] {
        core0_at = w.Now();
        log.push_back("core 0");
    });
    CHECK(w.sched->Run([&] { return log.size() == 2; }) == RunStatus::Stopped);
    CHECK(log == std::vector<std::string>{"core 0", "core 1"});
    CHECK(core0_at == 0);
    CHECK(core1_at == 268112);  // the end of the first SYS phase: 536223 - trunc(536223 * 0.5f)
}

TEST_CASE("threads exempt from preemption (the limit value is 10000) are never held") {
    World w;
    // A 1.0 title in the table with PREEMPTION_EXCEMPTED: limit 10000, share 10000, so the
    // limiter is active, but DoTimeLimit lets every thread through.
    w.mgr->ApplyAppMaxCpuSetting(0x0004000000020500ull, 0, 0);
    CHECK(w.mgr->Core1LimiterActive());
    u64 ran_at = ~0ull;
    bool done = false;
    w.Spawn(30, 1, [&] {
        ran_at = w.Now();
        done = true;
    });
    CHECK(w.sched->Run([&] { return done; }) == RunStatus::Stopped);
    CHECK(ran_at == 0);
}

TEST_CASE("held threads are released by turning the limit off, by a change of mode, and a held "
          "thread that ended is skipped") {
    World w;
    w.mgr->ApplyAppMaxCpuSetting(0x0004000000123400ull, 1, 80);
    REQUIRE(w.mgr->SetAppCpuTimeLimit(50) == kResultSuccess);  // first event at 268112 ticks
    bool core1_ran = false;
    std::vector<std::string> log;
    std::shared_ptr<Thread> held;
    held = w.Spawn(30, 1, [&] { core1_ran = true; });

    SUBCASE("SetAppCpuTimeLimit(0)") {
        w.Spawn(30, 0, [&] {
            w.sched->Sleep(100 * kUs);  // core 1 gets the host thread and holds the thread
            log.push_back(w.mgr->Core1HeldThreads() == 1 ? "held" : "not held");
            log.push_back(core1_ran ? "ran" : "not run");
            w.mgr->SetAppCpuTimeLimit(0);
            log.push_back(w.mgr->Core1HeldThreads() == 0 ? "released" : "still held");
            w.sched->Sleep(10 * kUs);  // the released thread has the core
            log.push_back(core1_ran ? "ran" : "not run");
        });
        // The limiter is off and its event cancelled: when both threads end, nothing is pending.
        CHECK(w.sched->Run({}, true) == RunStatus::Idle);
        CHECK(log == std::vector<std::string>{"held", "not run", "released", "ran"});
    }
    SUBCASE("a new CPU setting ends the limiter and starts it from scratch") {
        w.Spawn(30, 0, [&] {
            w.sched->Sleep(100 * kUs);
            log.push_back(w.mgr->Core1HeldThreads() == 1 ? "held" : "not held");
            w.mgr->ApplyAppMaxCpuSetting(0x0004000000123400ull, 1, 80);  // End(), Start()
            log.push_back(w.mgr->Core1HeldThreads() == 0 ? "released" : "still held");
            log.push_back(w.mgr->Core1LimiterActive() ? "active" : "inactive");  // share 0 again
            w.sched->Sleep(10 * kUs);
            log.push_back(core1_ran ? "ran" : "not run");
        });
        CHECK(w.sched->Run({}, true) == RunStatus::Idle);
        CHECK(log == std::vector<std::string>{"held", "released", "inactive", "ran"});
    }
    SUBCASE("a held thread that is terminated does not come back") {
        w.Spawn(30, 0, [&] {
            w.sched->Sleep(100 * kUs);
            log.push_back(w.mgr->Core1HeldThreads() == 1 ? "held" : "not held");
            w.sched->Terminate(held);
            log.push_back(w.mgr->Core1HeldThreads() == 0 ? "gone" : "still held");
            w.mgr->SetAppCpuTimeLimit(0);  // wakes the sleepers: this one is dead
            w.sched->Sleep(10 * kUs);
            log.push_back(core1_ran ? "ran" : "not run");
        });
        CHECK(w.sched->Run({}, true) == RunStatus::Idle);
        CHECK(log == std::vector<std::string>{"held", "gone", "not run"});
        CHECK(held->status == ThreadStatus::Dead);
        CHECK(w.ThreadCount() == 0);
    }
}

TEST_CASE("a wait that was satisfied while its thread was held back still returns its result") {
    World w;
    w.mgr->ApplyAppMaxCpuSetting(0x0004000000123400ull, 1, 80);
    const auto first = w.kernel->NewEvent(ResetType::OneShot);
    const auto second = w.kernel->NewEvent(ResetType::OneShot);
    WaitOutcome outcome;
    bool finished = false;
    std::vector<std::string> log;
    std::shared_ptr<Thread> waiter;
    waiter = w.Spawn(30, 1, [&] {
        // WaitSynchronizationN on any, for ever: the limiter is off now, so the thread blocks.
        outcome = w.sched->Block(w.Self(), {first, second}, false, -1);
        finished = true;
    });
    w.Spawn(30, 0, [&] {
        w.sched->Sleep(1 * kMs);  // core 1 runs the waiter, which blocks
        w.mgr->SetAppCpuTimeLimit(50);  // a SYS phase of 268112 ticks (1 ms) starts now
        second->Signal();               // the waiter is woken: Success, index 1 ...
        w.sched->Sleep(100 * kUs);      // ... and core 1 picks it: the limiter holds it back
        log.push_back(waiter->status == ThreadStatus::WaitSleep ? "held" : "not held");
        log.push_back(finished ? "finished" : "waiting");
        w.sched->Sleep(2 * kMs);  // the phase ends and the thread goes on
        log.push_back(finished ? "finished" : "waiting");
        w.mgr->SetAppCpuTimeLimit(0);
    });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(log == std::vector<std::string>{"held", "waiting", "finished"});
    // The signal's outcome survives the second wake-up that the limiter gives the thread.
    CHECK(outcome.result == std::optional<Result>(kResultSuccess));
    CHECK(outcome.index == 1);
    CHECK(outcome.blocked);
}

#endif  // __has_include("recomp.h")
