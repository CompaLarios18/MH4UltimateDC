// Derived from Azahar src/core/hle/kernel/svc.cpp, svc_wrapper.h, errors.h, process.cpp,
// resource_limit.cpp, memory.cpp, handle_table.cpp, mutex.cpp and src/core/memory.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp says so; svc_wrapper.h,
// errors.h, process.cpp, resource_limit.cpp, memory.cpp, handle_table.cpp, mutex.cpp and memory.h
// say GPL-2.0-or-later): the expected register encodings (worked out from
// the argument lists of the SVC:: functions with the ABI rules of SVCWrapper::GetSVCABI), the result
// codes (errors.h, and the checks of each SVC:: function in their order), the initial resource
// limit values, the address ranges and the order in which MemoryRegionInfo hands out FCRAM, never
// by running this code.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp, svc_wrapper.h,
//   memory.h)
// Copyright 2017 Citra Emulator Project (errors.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp,
//   resource_limit.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (memory.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (handle_table.cpp, mutex.cpp)
//
// mh4n_kernel (N1-07): the SVC table. Every call is made through `SvcTable::CallSvc` with a fake
// guest context (an RCtx whose registers the test fills), on a guest thread whose body is a C++
// function; the calls that block are made from two or more threads on the manual clock of N1-06, so
// nothing here waits on real time or sleeps as synchronisation.
//
// Register positions, from SVCWrapper (svc_wrapper.h): the result is r0, the pointer parameters'
// values follow in r1 upward; the other parameters keep their ARM-ABI positions (a 64-bit one on an
// even pair, the slots of pointer parameters skipped) and the ones that do not fit take the free
// slots from r0 up, then r4 up:
//   ControlMemory(u32* out, addr0, addr1, size, operation, permissions)  in r0 op, r1 addr0, r2 addr1,
//                                                                         r3 size, r4 perm; out r1
//   QueryMemory(MemoryInfo*, PageInfo*, addr)                            in r2 addr; out r1 base,
//                                                                         r2 size, r3 perm, r4 state,
//                                                                         r5 page flags
//   CreateThread(Handle*, entry, arg, stack_top, priority, processor)    in r0 priority, r1 entry,
//                                                                         r2 arg, r3 stack, r4 cpu
//   SetTimer(Handle, s64 initial, s64 interval)  r0 handle, r2/r3 initial, r1 interval low, r4 high
//   WaitSynchronization1(Handle, s64)            r0 handle, r2/r3 timeout
//   WaitSynchronizationN(s32*, handles, count, wait_all, s64)  r1 handles, r2 count, r3 all, r0/r4
//                                                              timeout; out r1 the index
//   ArbitrateAddress(Handle, address, type, value, s64)        r0..r3, r4/r5 timeout
//   CreateMemoryBlock(Handle*, addr, size, my, other)          r0 other, r1 addr, r2 size, r3 my
//   GetHandleInfo/GetSystemInfo/GetProcessInfo(s64*, a, b)     in r1, r2; out r1/r2
//   GetResourceLimit*Values(values, limit, names, count)       r0..r3
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("svc_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <array>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/cpu.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/scheduler.h"
#include "mh4n/svc.h"
#include "mh4n/threads.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr s64 kUs = 1000;
constexpr s64 kMs = 1000 * kUs;
constexpr u32 kCode = 0x00100000;  // one mapped page of "code"
constexpr u32 kData = 0x00200000;  // one page the tests read and write
constexpr u32 kNever = 0xFFFFFFFF; // the low and high words of a timeout of -1

// Result codes of Azahar's errors.h and svc.cpp, as the numbers its comments give (the value of
// ResultOutOfHeapMemory is the one its arguments compute: see svc_internal.h).
constexpr u32 kOk = 0;
constexpr u32 kTimeout = 0x09401BFE;
constexpr u32 kInvalidHandle = 0xD8E007F7;
constexpr u32 kInvalidEnum = 0xD8E007ED;
constexpr u32 kInvalidPointer = 0xD8E007F6;
constexpr u32 kNotFound = 0xD88007FA;
constexpr u32 kOutOfRange = 0xE0E01BFD;
constexpr u32 kOutOfRangeKernel = 0xD8E007FD;
constexpr u32 kNotAuthorized = 0xD9001BEA;
constexpr u32 kInvalidCombination = 0xE0E01BEE;
constexpr u32 kMisalignedAddress = 0xE0E01BF1;
constexpr u32 kMisalignedSize = 0xE0E01BF2;
constexpr u32 kNotImplemented = 0xE0E01BF4;
constexpr u32 kInvalidAddress = 0xE0E01BF5;
constexpr u32 kInvalidAddressState = 0xE0A01BF5;
constexpr u32 kOutOfHeapMemory = 0xC8601BF3;
constexpr u32 kPortNameTooLong = 0xE0E0181E;

// A PreemptionSource for manual time (see threads_test.cpp): raises when the manual clock reaches
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
            deadline_.reset();
            raise_();
        }
    }
    ManualTimeSource& source_;
    std::function<void()> raise_;
    std::optional<s64> deadline_;
};

// The registers after a call.
struct Out {
    std::array<u32, 16> r{};
    u32 operator[](int i) const { return r[static_cast<std::size_t>(i)]; }
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
    std::unique_ptr<SvcTable> svc;

    explicit World(u32 memory_mode = 0, u16 kernel_version = 0) {
        SetLogFilter(LogFilter(LogLevel::Critical));  // the failures under test log at Error
        memory = GuestMemory::Create();
        REQUIRE(memory != nullptr);
        REQUIRE(IsSuccess(memory->Map(kCode, kPageSize, MemPerm::ReadExecute, MemState::Code)));
        REQUIRE(IsSuccess(memory->Map(kData, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        clock = std::make_unique<GameClock>(manual);
        host = std::make_unique<CodeHost>(*memory);
        // There is no code library: a thread made by svcCreateThread that gets to run just ends.
        host->SetMissingFunctionHandler([this](RCtx&, u32) { sched->ExitCurrent(); });
        kernel = std::make_unique<Kernel>(*memory, memory_mode);
        sched = std::make_unique<GuestScheduler>(*kernel, *host, *clock, manual, &preemption);
        process = kernel->NewProcess("app");
        process->kernel_version = kernel_version;
        mgr = std::make_unique<ThreadManager>(*kernel, *sched);
        svc = std::make_unique<SvcTable>(*kernel, *sched, *mgr);
    }
    // The lifetime order: Shutdown(), the table and the manager, the kernel, the scheduler.
    ~World() {
        sched->Shutdown();
        svc.reset();
        mgr.reset();
        process.reset();
        kernel.reset();
        sched.reset();
        host.reset();
        clock.reset();
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }

    // A guest thread (made as svcCreateThread does) that runs `body` and ends.
    std::shared_ptr<Thread> Spawn(u32 priority, std::function<void()> body, int core = 0) {
        ThreadSpec spec;
        spec.entry_point = kCode;
        spec.stack_top = kStackTop;
        spec.priority = priority;
        spec.processor_id = core;
        spec.host_entry = [this, body = std::move(body)](GuestThread&) {
            body();
            sched->ExitCurrent();
        };
        std::shared_ptr<Thread> thread;
        Handle handle = 0;
        REQUIRE(mgr->CreateThread(spec, &handle, &thread) == kResultSuccess);
        return thread;
    }
    // One thread that runs `body`, and the scheduler until nothing is left to run.
    RunStatus Run(std::function<void()> body, u32 priority = 30) {
        Spawn(priority, std::move(body));
        return sched->Run({}, true);
    }
    // One SVC from the running guest thread: `in` fills r0 upward, the other registers hold
    // markers, so that a call that does not write one is seen.
    Out Svc(u32 number, std::initializer_list<u32> in, u32 lr = 0x00100020,
            u32 next_pc = 0x00100009) {
        RCtx ctx{};
        for (int i = 0; i < 13; i++) {
            ctx.r[i] = 0xA0A00000u + static_cast<u32>(i);
        }
        int i = 0;
        for (const u32 value : in) {
            ctx.r[i++] = value;
        }
        ctx.r[14] = lr;
        svc->CallSvc(ctx, number, next_pc);
        Out out;
        for (int k = 0; k < 16; k++) {
            out.r[static_cast<std::size_t>(k)] = ctx.r[k];
        }
        return out;
    }
    void Advance(s64 ns) { manual.Advance(ns); }
    u64 Now() { return clock->Now(); }
    FcramRegions& Regions() { return kernel->Regions(); }
    void Poke32(u32 address, u32 value) { REQUIRE(memory->Write<u32>(address, value)); }
    u32 Peek32(u32 address) {
        const auto value = memory->Read<u32>(address);
        REQUIRE(value.has_value());
        return *value;
    }
    void PokeString(u32 address, const std::string& text) {
        REQUIRE(memory->WriteBlock(address, text.c_str(), static_cast<u32>(text.size() + 1)));
    }
    // A handle made by an SVC.
    Handle Create(u32 number, std::initializer_list<u32> in) {
        const Out out = Svc(number, in);
        REQUIRE(out[0] == kOk);
        return out[1];
    }
    Handle NewEvent(u32 reset_type = 0) { return Create(0x17, {0, reset_type}); }
};

}  // namespace

// ---- dispatch, unknown SVCs and the trace hooks ------------------------------------------------------

TEST_CASE("CallSvc sets r15 to the address after the instruction, Thumb bit cleared") {
    World w;
    RCtx arm{}, thumb{};
    CHECK(w.Run([&] {
        arm.r[0] = 0xD7;  // CloseHandle of nothing: only the register r15 matters here
        w.svc->CallSvc(arm, 0x23, 0x00100104);
        thumb.r[0] = 0xD7;
        w.svc->CallSvc(thumb, 0x23, 0x00100105);
    }) == RunStatus::Idle);
    CHECK(arm.r[15] == 0x00100104);
    CHECK(thumb.r[15] == 0x00100104);
}

TEST_CASE("an unknown SVC leaves the registers alone and is logged once per number, with lr") {
    World w;
    std::vector<std::string> logged;
    SetLogFilter(LogFilter(LogLevel::Error));
    SetLogSink([&](const LogRecord& r) { logged.push_back(std::string(r.message)); });
    Out first, second, other, far;
    CHECK(w.Run([&] {
        // 0x26 is SignalAndWait, which Azahar knows and does not implement; 0x7F is "Unused" in its
        // table and 0x1234 is past it.
        first = w.Svc(0x26, {1, 2, 3, 4, 5, 6, 7}, 0x00100ABC);
        second = w.Svc(0x26, {1, 2, 3, 4, 5, 6, 7}, 0x00100DEF);
        other = w.Svc(0x7F, {9, 8, 7}, 0x00100111);
        far = w.Svc(0x1234, {5}, 0x00100222);
    }) == RunStatus::Idle);
    for (const Out& out : {first, second}) {
        for (u32 i = 0; i < 7; i++) {
            CHECK(out[static_cast<int>(i)] == i + 1);
        }
        CHECK(out[7] == 0xA0A00007);  // untouched markers
    }
    CHECK(other[0] == 9);
    CHECK(other[3] == 0xA0A00003);
    CHECK(far[0] == 5);
    REQUIRE(logged.size() == 3);
    CHECK(logged[0] == "unimplemented svc 0x26 at 0x00100ABC");
    CHECK(logged[1] == "unimplemented svc 0x7F at 0x00100111");
    CHECK(logged[2] == "unimplemented svc 0x1234 at 0x00100222");
}

TEST_CASE("the hooks see the call before it runs and its registers when it completes") {
    World w;
    std::vector<std::string> events;
    std::vector<SvcCall> before_calls;
    std::vector<std::array<u32, 4>> outs;
    SvcHooks hooks;
    hooks.before = [&](const SvcCall& call) {
        before_calls.push_back(call);
        events.push_back("before");
    };
    hooks.complete = [&](const SvcCall& call, const std::array<u32, 4>& out) {
        events.push_back("complete " + std::to_string(call.number));
        outs.push_back(out);
    };
    w.svc->SetHooks(hooks);
    w.manual.Advance(1 * kMs);
    const u64 start = w.Now();
    CHECK(w.Run([&] {
        // CloseHandle(0xDEAD) is refused with the invalid-handle result.
        w.Svc(0x23, {0xDEAD, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, 0x00100777, 0x00100009);
        CHECK(w.svc->PendingCalls().empty());
    }) == RunStatus::Idle);
    REQUIRE(before_calls.size() == 1);
    const SvcCall& call = before_calls[0];
    CHECK(call.thread_id == 1);
    CHECK(call.number == 0x23);
    CHECK(call.pc == 0x00100008);  // r15
    CHECK(call.lr == 0x00100777);
    CHECK(call.regs == std::array<u32, 7>{0xDEAD, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
    CHECK(call.ticks == start);
    REQUIRE(outs.size() == 1);
    CHECK(outs[0] == std::array<u32, 4>{kInvalidHandle, 0x11, 0x22, 0x33});
    CHECK(events == std::vector<std::string>{"before", "complete 35"});
}

// ---- ExitProcess, ExitThread and Break ---------------------------------------------------------------

TEST_CASE("svcExitProcess ends the run with status 0 and the thread never returns") {
    World w;
    bool after = false;
    std::vector<SvcCall> pending;
    CHECK(w.Run([&] {
        w.Svc(0x03, {});
        after = true;
    }) == RunStatus::Stopped);
    CHECK_FALSE(after);
    REQUIRE(w.svc->Exit().has_value());
    CHECK(w.svc->Exit()->status == 0);
    CHECK(w.svc->Exit()->report.empty());
    pending = w.svc->PendingCalls();
    REQUIRE(pending.size() == 1);  // it never completed: the trace writes out=-
    CHECK(pending[0].number == 0x03);
}

TEST_CASE("svcBreak ends the run with status 3 and a report naming the reason") {
    struct Case {
        u32 r0;
        const char* reason;
    };
    for (const Case& c : {Case{0, "PANIC"}, Case{1, "ASSERT"}, Case{2, "USER"}, Case{3, "UNKNOWN"},
                          Case{0x101, "ASSERT"} /* the reason is a u8 */}) {
        World w;
        SetLogSink([](const LogRecord&) {});  // the report goes to the log at Critical level
        bool after = false;
        CHECK(w.Run([&] {
            w.Svc(0x3C, {c.r0, 0x12345678});
            after = true;
        }) == RunStatus::Stopped);
        CHECK_FALSE(after);
        REQUIRE(w.svc->Exit().has_value());
        CHECK(w.svc->Exit()->status == 3);
        CHECK(w.svc->Exit()->report.find(c.reason) != std::string::npos);
        // The registers of the thread are in the report.
        CHECK(w.svc->Exit()->report.find("r1 =12345678") != std::string::npos);
    }
}

TEST_CASE("svcExitThread ends the calling thread only") {
    World w;
    bool after = false;
    bool other_ran = false;
    std::shared_ptr<Thread> first = w.Spawn(30, [&] {
        w.Svc(0x09, {});
        after = true;
    });
    w.Spawn(31, [&] { other_ran = true; });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK_FALSE(after);
    CHECK(other_ran);
    CHECK(first->status == ThreadStatus::Dead);
    CHECK_FALSE(w.svc->Exit().has_value());
    CHECK(w.svc->PendingCalls().size() == 1);  // the exit never completes
    CHECK(w.svc->PendingCalls()[0].number == 0x09);
}

// ---- memory: ControlMemory and QueryMemory -----------------------------------------------------------

TEST_CASE("ControlMemory commits heap from the top of the APPLICATION region, as Process::HeapAllocate") {
    World w;  // memory mode 0: APPLICATION 0x04000000 at FCRAM offset 0
    const u32 top = 0x04000000;
    CHECK(w.Run([&] {
        // COMMIT (3) of three pages at the start of the heap, read/write.
        Out out = w.Svc(0x01, {3, 0x08000000, 0, 0x3000, 3});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x08000000);
        // The pieces come from the highest free block, so the block is the end of the region.
        CHECK(w.Regions().Used(FcramRegion::Application) == 0x3000);
        const std::vector<FcramRange> free_blocks = w.Regions().FreeBlocks(FcramRegion::Application);
        REQUIRE(free_blocks.size() == 1);
        CHECK(free_blocks[0] == FcramRange{0, top - 0x3000});
        // QueryMemory: base, size, permission (RW), state Private (5).
        out = w.Svc(0x02, {0, 0, 0x08001234});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x08000000);
        CHECK(out[2] == 0x3000);
        CHECK(out[3] == 3);
        CHECK(out[4] == 5);
        CHECK(out[5] == 0);
        // The memory is there and zero.
        CHECK(w.memory->Read<u32>(0x08002FFC) == 0);
        CHECK(w.memory->Write<u32>(0x08002FFC, 0xCAFEBABE));
        // The process's counters (GetProcessInfo type 0 and the resource limit's Commit value) follow.
        CHECK(w.process->memory_used == 0x3000 + 0x1000);  // and the TLS page of this thread
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::Commit) == 0x3000);
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory refuses what Azahar refuses, in its order, with its result codes") {
    World w;
    CHECK(w.Run([&] {
        CHECK(w.Svc(0x01, {3, 0x08000800, 0, 0x1000, 3})[0] == kMisalignedAddress);
        CHECK(w.Svc(0x01, {3, 0x08000000, 0x100, 0x1000, 3})[0] == kMisalignedAddress);  // addr1 too
        CHECK(w.Svc(0x01, {3, 0x08000000, 0, 0x800, 3})[0] == kMisalignedSize);
        CHECK(w.Svc(0x01, {3, 0x08000000, 0, 0x1000, 4})[0] == kInvalidCombination);   // execute
        CHECK(w.Svc(0x01, {3, 0x08000000, 0, 0x1000, 8})[0] == kInvalidCombination);
        CHECK(w.Svc(0x01, {2, 0x08000000, 0, 0x1000, 3})[0] == kInvalidCombination);   // RESERVE
        CHECK(w.Svc(0x01, {99, 0x08000000, 0, 0x1000, 3})[0] == kInvalidCombination);
        // The heap is [0x08000000, 0x10000000).
        CHECK(w.Svc(0x01, {3, 0x07FFF000, 0, 0x1000, 3})[0] == kInvalidAddress);
        CHECK(w.Svc(0x01, {3, 0x0FFFF000, 0, 0x2000, 3})[0] == kInvalidAddress);
        CHECK(w.Svc(0x01, {3, 0xFFFFF000, 0, 0x1000, 3})[0] == kInvalidAddress);  // the sum wraps
        // The same page twice: the second finds it allocated.
        CHECK(w.Svc(0x01, {3, 0x08000000, 0, 0x1000, 3})[0] == kOk);
        CHECK(w.Svc(0x01, {3, 0x08000000, 0, 0x1000, 3})[0] == kInvalidAddressState);
        CHECK(w.Svc(0x01, {3, 0x08000000, 0, 0x2000, 3})[0] == kInvalidAddressState);
        // A failed call writes 0 to the output register.
        CHECK(w.Svc(0x01, {3, 0x08000000, 0, 0x1000, 3})[1] == 0);
        // A size of 0 gives Process::HeapAllocate an empty set of FCRAM pieces.
        CHECK(w.Svc(0x01, {3, 0x08100000, 0, 0, 3})[0] == kOutOfHeapMemory);
        // The region bits are accepted and ignored (Azahar only warns).
        CHECK(w.Svc(0x01, {0x303, 0x08200000, 0, 0x1000, 3})[0] == kOk);
        // FREE: outside the heap and the linear heap, and of what is not there.
        CHECK(w.Svc(0x01, {1, 0x00300000, 0, 0x1000, 0})[0] == kInvalidAddress);
        CHECK(w.Svc(0x01, {1, 0x08300000, 0, 0x1000, 0})[0] == kInvalidAddressState);
        CHECK(w.Svc(0x01, {1, 0x08000000, 0, 0, 0})[0] == kOk);  // size 0 does nothing
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory FREE gives the FCRAM of the freed pages back, and the next COMMIT reuses it") {
    World w;
    const u32 top = 0x04000000;
    CHECK(w.Run([&] {
        REQUIRE(w.Svc(0x01, {3, 0x08000000, 0, 0x3000, 3})[0] == kOk);
        // Free the middle page: the heap block splits in two for QueryMemory.
        Out out = w.Svc(0x01, {1, 0x08001000, 0, 0x1000, 0});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x08001000);
        CHECK(w.Regions().Used(FcramRegion::Application) == 0x2000);
        const std::vector<FcramRange> free_blocks = w.Regions().FreeBlocks(FcramRegion::Application);
        REQUIRE(free_blocks.size() == 2);
        CHECK(free_blocks[0] == FcramRange{0, top - 0x3000});
        CHECK(free_blocks[1] == FcramRange{top - 0x2000, 0x1000});
        out = w.Svc(0x02, {0, 0, 0x08000000});
        CHECK(out[1] == 0x08000000);
        CHECK(out[2] == 0x1000);
        out = w.Svc(0x02, {0, 0, 0x08001000});  // the hole: free, no permission
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x08001000);
        CHECK(out[2] == 0x1000);
        CHECK(out[3] == 0);
        CHECK(out[4] == 0);
        out = w.Svc(0x02, {0, 0, 0x08002000});
        CHECK(out[1] == 0x08002000);
        CHECK(out[2] == 0x1000);
        // The freed page is the highest free block, so the next page comes from the same FCRAM.
        CHECK(w.Svc(0x01, {3, 0x08010000, 0, 0x1000, 3})[0] == kOk);
        CHECK(w.Regions().Used(FcramRegion::Application) == 0x3000);
        const std::vector<FcramRange> after = w.Regions().FreeBlocks(FcramRegion::Application);
        REQUIRE(after.size() == 1);
        CHECK(after[0] == FcramRange{0, top - 0x3000});
        // Freeing the whole of what is left gives everything back.
        CHECK(w.Svc(0x01, {1, 0x08000000, 0, 0x1000, 0})[0] == kOk);
        CHECK(w.Svc(0x01, {1, 0x08002000, 0, 0x1000, 0})[0] == kOk);
        CHECK(w.Svc(0x01, {1, 0x08010000, 0, 0x1000, 0})[0] == kOk);
        CHECK(w.Regions().Used(FcramRegion::Application) == 0);
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::Commit) == 0);
        CHECK_FALSE(w.memory->IsMapped(0x08000000, 1));
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory FREE of part of a block frees only that part") {
    World w;
    const u32 top = 0x04000000;
    CHECK(w.Run([&] {
        REQUIRE(w.Svc(0x01, {3, 0x08000000, 0, 0x4000, 3})[0] == kOk);
        // The last two pages and the first one: three FREEs of pieces of one run.
        CHECK(w.Svc(0x01, {1, 0x08002000, 0, 0x2000, 0})[0] == kOk);
        CHECK(w.Svc(0x01, {1, 0x08000000, 0, 0x1000, 0})[0] == kOk);
        CHECK(w.Regions().Used(FcramRegion::Application) == 0x1000);
        // What is left is page 1 of the four, at FCRAM [top - 0x3000, top - 0x2000).
        const std::vector<FcramRange> free_blocks = w.Regions().FreeBlocks(FcramRegion::Application);
        REQUIRE(free_blocks.size() == 2);
        CHECK(free_blocks[0] == FcramRange{0, top - 0x3000});
        CHECK(free_blocks[1] == FcramRange{top - 0x2000, 0x2000});
        CHECK(w.memory->IsMapped(0x08001000, 0x1000));
        CHECK_FALSE(w.memory->IsMapped(0x08000000, 1));
        CHECK_FALSE(w.memory->IsMapped(0x08002000, 1));
    }) == RunStatus::Idle);
}

TEST_CASE("QueryMemory merges neighbours with the same permission and state, whatever their backing") {
    World w;
    CHECK(w.Run([&] {
        // Two pages in ascending addresses take descending FCRAM (the highest free block first), so
        // their backing is not contiguous and GuestMemory keeps two blocks; Azahar's
        // QueryProcessMemory merges them.
        REQUIRE(w.Svc(0x01, {3, 0x08000000, 0, 0x1000, 3})[0] == kOk);
        REQUIRE(w.Svc(0x01, {3, 0x08001000, 0, 0x1000, 3})[0] == kOk);
        CHECK(w.memory->Query(0x08000000)->size == 0x1000);
        const Out out = w.Svc(0x02, {0, 0, 0x08001000});
        CHECK(out[1] == 0x08000000);
        CHECK(out[2] == 0x2000);
        // Another permission is another block.
        REQUIRE(w.Svc(0x01, {3, 0x08002000, 0, 0x1000, 1})[0] == kOk);
        const Out other = w.Svc(0x02, {0, 0, 0x08002000});
        CHECK(other[1] == 0x08002000);
        CHECK(other[2] == 0x1000);
        CHECK(other[3] == 1);
    }) == RunStatus::Idle);
}

TEST_CASE("QueryMemory: free space, the code, and addresses outside the managed range") {
    World w;
    CHECK(w.Run([&] {
        Out out = w.Svc(0x02, {0, 0, 0x00000000});  // free from 0 up to the code page
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0);
        CHECK(out[2] == 0x00100000);
        CHECK(out[3] == 0);
        CHECK(out[4] == 0);
        out = w.Svc(0x02, {0, 0, kCode + 0x20});  // the code: read/execute (5), state Code (4)
        CHECK(out[1] == kCode);
        CHECK(out[2] == 0x1000);
        CHECK(out[3] == 5);
        CHECK(out[4] == 4);
        out = w.Svc(0x02, {0, 0, 0x40000000});  // Azahar's FindVMA gives the end of its map
        CHECK(out[0] == kInvalidAddress);
        for (int i = 1; i <= 5; i++) {
            CHECK(out[i] == 0);
        }
        out = w.Svc(0x02, {0, 0, 0xFFFFFFFF});
        CHECK(out[0] == kInvalidAddress);
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory PROTECT changes the permission, and fails on memory that is not there") {
    World w;
    CHECK(w.Run([&] {
        REQUIRE(w.Svc(0x01, {3, 0x08000000, 0, 0x2000, 3})[0] == kOk);
        CHECK(w.Svc(0x01, {6, 0x08000000, 0, 0x1000, 1})[0] == kOk);
        Out out = w.Svc(0x02, {0, 0, 0x08000000});
        CHECK(out[2] == 0x1000);
        CHECK(out[3] == 1);
        out = w.Svc(0x02, {0, 0, 0x08001000});
        CHECK(out[3] == 3);
        CHECK(w.Svc(0x01, {6, 0x08100000, 0, 0x1000, 1})[0] == kInvalidAddressState);
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory linear heap: first fit from the bottom of the region, state Continuous") {
    // A kernel version from 0x22C puts the linear heap at 0x30000000; the APPLICATION region of mode
    // 0 starts at FCRAM offset 0.
    World w(0, 0x23A);
    CHECK(w.Run([&] {
        Out out = w.Svc(0x01, {0x10003, 0, 0, 0x2000, 3});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x30000000);
        out = w.Svc(0x01, {0x10003, 0, 0, 0x1000, 3});
        CHECK(out[1] == 0x30002000);
        CHECK(w.Regions().Used(FcramRegion::Application) == 0x3000);
        // Contiguous backing: one block of three pages, permission RW, state Continuous (7).
        out = w.Svc(0x02, {0, 0, 0x30001000});
        CHECK(out[1] == 0x30000000);
        CHECK(out[2] == 0x3000);
        CHECK(out[3] == 3);
        CHECK(out[4] == 7);
        // FREE of the first block; the next allocation of that size finds the hole again.
        out = w.Svc(0x01, {1, 0x30000000, 0, 0x2000, 0});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x30000000);
        CHECK(w.Regions().Used(FcramRegion::Application) == 0x1000);
        out = w.Svc(0x01, {0x10003, 0, 0, 0x1000, 3});
        CHECK(out[1] == 0x30000000);
        // GetProcessInfo type 20: FCRAM_PADDR minus the linear heap's area, in 32-bit arithmetic.
        out = w.Svc(0x2B, {0, 0xFFFF8001, 20});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0xF0000000);
        CHECK(out[2] == 0);
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory linear heap with an old kernel version starts at 0x14000000") {
    World w(0, 0x1F6);
    CHECK(w.Run([&] {
        CHECK(w.Svc(0x01, {0x10003, 0, 0, 0x1000, 3})[1] == 0x14000000);
        CHECK(w.Svc(0x2B, {0, 0xFFFF8001, 20})[1] == 0x0C000000);
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory linear heap with a given address, and its refusals") {
    World w(0, 0x23A);
    CHECK(w.Run([&] {
        // A given target inside the region.
        Out out = w.Svc(0x01, {0x10003, 0x30004000, 0, 0x1000, 3});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x30004000);
        // Taken: the same range, or part of it.
        CHECK(w.Svc(0x01, {0x10003, 0x30004000, 0, 0x1000, 3})[0] == kInvalidAddressState);
        CHECK(w.Svc(0x01, {0x10003, 0x30003000, 0, 0x2000, 3})[0] == kInvalidAddressState);
        // Outside the APPLICATION region [0x30000000, 0x34000000) of mode 0, or wrapping.
        CHECK(w.Svc(0x01, {0x10003, 0x2FFFF000, 0, 0x1000, 3})[0] == kInvalidAddress);
        CHECK(w.Svc(0x01, {0x10003, 0x33FFF000, 0, 0x2000, 3})[0] == kInvalidAddress);
        CHECK(w.Svc(0x01, {0x10003, 0xFFFFF000, 0, 0x1000, 3})[0] == kInvalidAddress);
        // The execute bit is refused before anything is looked at.
        CHECK(w.Svc(0x01, {0x10003, 0, 0, 0x1000, 5})[0] == kInvalidCombination);
        // FREE: inside the region but not the heap range of the process, and not allocated.
        CHECK(w.Svc(0x01, {1, 0x30008000, 0, 0x1000, 0})[0] == kInvalidAddressState);
        CHECK(w.Svc(0x01, {1, 0x34000000, 0, 0x1000, 0})[0] == kInvalidAddress);
        CHECK(w.Svc(0x01, {1, 0x30004000, 0, 0, 0})[0] == kOk);  // size 0
        CHECK(w.Svc(0x01, {1, 0x30004000, 0, 0x1000, 0})[0] == kOk);
        CHECK(w.Regions().Used(FcramRegion::Application) == 0);
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory linear allocation gives the physical view to the GPU") {
    World w(0, 0x23A);
    w.memory->SetLinearHeapArea(0x30000000);
    CHECK(w.Run([&] {
        const Out out = w.Svc(0x01, {0x10003, 0, 0, 0x1000, 3});
        REQUIRE(out[0] == kOk);
        // FCRAM offset 0 is physical 0x20000000.
        REQUIRE(w.memory->PhysicalPointer(0x20000000, 0x1000) != nullptr);
        CHECK(w.memory->PhysicalPointer(0x20000000, 0x1000) == w.memory->Base() + 0x30000000);
    }) == RunStatus::Idle);
}

TEST_CASE("ControlMemory MAP and UNMAP check their arguments, then are refused (flat memory cannot alias)") {
    World w;
    CHECK(w.Run([&] {
        REQUIRE(w.Svc(0x01, {3, 0x08000000, 0, 0x2000, 3})[0] == kOk);
        // MAP (4): r1 target, r2 source. The source must be in the heap.
        CHECK(w.Svc(0x01, {4, 0x08100000, 0x00300000, 0x1000, 3})[0] == kInvalidAddress);
        CHECK(w.Svc(0x01, {4, 0x08100000, 0x0FFFF000, 0x2000, 3})[0] == kInvalidAddress);
        // The ranges must not overlap.
        CHECK(w.Svc(0x01, {4, 0x08001000, 0x08000000, 0x2000, 3})[0] == kInvalidAddressState);
        CHECK(w.Svc(0x01, {4, 0x08000000, 0x08000000, 0x1000, 3})[0] == kInvalidAddressState);
        // A target that is not free.
        CHECK(w.Svc(0x01, {4, 0x08000000, 0x08002000, 0x1000, 3})[0] == kInvalidAddressState);
        // A valid request: logged and refused.
        CHECK(w.Svc(0x01, {4, 0x08100000, 0x08000000, 0x1000, 3})[0] == kInvalidAddressState);
        CHECK(w.Svc(0x01, {5, 0x08100000, 0x08000000, 0x1000, 3})[0] == kInvalidAddressState);
        CHECK(w.Svc(0x01, {5, 0x08100000, 0x00300000, 0x1000, 3})[0] == kInvalidAddress);
    }) == RunStatus::Idle);
}

// ---- memory blocks -----------------------------------------------------------------------------------

TEST_CASE("CreateMemoryBlock at address 0 comes from the BASE region and maps in the linear heap area") {
    World w;
    const u32 base = 0x04000000 + 0x02C00000;  // BASE starts after APPLICATION and SYSTEM (mode 0)
    CHECK(w.Run([&] {
        const u32 used_before = w.Regions().Used(FcramRegion::Base);
        // r0 other permissions, r1 address, r2 size, r3 my permissions; out r1 the handle.
        const Out out = w.Svc(0x1E, {0x10000000, 0, 0x2000, 3, 0});
        REQUIRE(out[0] == kOk);
        const Handle block = out[1];
        CHECK(w.Regions().Used(FcramRegion::Base) == used_before + 0x2000);
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::SharedMemory) == 1);
        // Map it anywhere (address 0) with other permissions DontCare (1 << 28): at the old linear
        // heap area plus the block's FCRAM offset.
        CHECK(w.Svc(0x1F, {block, 0, 3, 0x10000000})[0] == kOk);
        const auto info = w.memory->Query(0x14000000 + base + used_before);
        REQUIRE(info.has_value());
        CHECK(info->state == MemState::Shared);
        CHECK(info->size == 0x2000);
        // Unmap at the same address; the pages go.
        CHECK(w.Svc(0x20, {block, 0x14000000 + base + used_before})[0] == kOk);
        CHECK_FALSE(w.memory->IsMapped(0x14000000 + base + used_before, 1));
        // Closing the last handle destroys the block: its FCRAM and its resource go back.
        CHECK(w.Svc(0x23, {block})[0] == kOk);
        CHECK(w.Regions().Used(FcramRegion::Base) == used_before);
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::SharedMemory) == 0);
    }) == RunStatus::Idle);
}

TEST_CASE("CreateMemoryBlock over the process's own memory locks it") {
    World w;
    CHECK(w.Run([&] {
        REQUIRE(w.Svc(0x01, {3, 0x08000000, 0, 0x2000, 3})[0] == kOk);
        const Out out = w.Svc(0x1E, {3, 0x08000000, 0x2000, 3});
        REQUIRE(out[0] == kOk);
        const Out query = w.Svc(0x02, {0, 0, 0x08000000});
        CHECK(query[4] == 11);  // MemoryState::Locked
        CHECK(query[2] == 0x2000);
    }) == RunStatus::Idle);
}

TEST_CASE("CreateMemoryBlock, MapMemoryBlock and UnmapMemoryBlock: arguments and result codes") {
    World w;
    CHECK(w.Run([&] {
        // The size must be a multiple of a page.
        CHECK(w.Svc(0x1E, {3, 0x08000000, 0x800, 3})[0] == kMisalignedSize);
        // Neither side may have execute permission; None, Read, Write, ReadWrite and DontCare do.
        CHECK(w.Svc(0x1E, {4, 0x08000000, 0x1000, 3})[0] == kInvalidCombination);
        CHECK(w.Svc(0x1E, {3, 0x08000000, 0x1000, 5})[0] == kInvalidCombination);
        CHECK(w.Svc(0x1E, {0x10000000, 0, 0x1000, 0})[0] == kOk);
        // The address must be 0 or inside [0x00100000, 0x14000000) with the whole block.
        CHECK(w.Svc(0x1E, {3, 0x00000800, 0x1000, 3})[0] == kInvalidAddress);
        CHECK(w.Svc(0x1E, {3, 0x13FFF000, 0x2000, 3})[0] == kInvalidAddress);
        // A failed call writes 0 to the handle register.
        CHECK(w.Svc(0x1E, {3, 0x00000800, 0x1000, 3})[1] == 0);
        // Map and Unmap of a handle that is not a memory block, and of a bad permission.
        const Handle event = w.NewEvent();
        CHECK(w.Svc(0x1F, {event, 0, 3, 3})[0] == kInvalidHandle);
        CHECK(w.Svc(0x20, {event, 0})[0] == kInvalidHandle);
        CHECK(w.Svc(0x1F, {0xDEAD, 0, 3, 3})[0] == kInvalidHandle);
        const Handle block = w.Create(0x1E, {0x10000000, 0, 0x1000, 3});
        CHECK(w.Svc(0x1F, {block, 0, 0, 0x10000000})[0] == kInvalidCombination);  // None
        CHECK(w.Svc(0x1F, {block, 0, 8, 0x10000000})[0] == kInvalidCombination);
        // An automatically allocated block maps only with other permissions DontCare.
        CHECK(w.Svc(0x1F, {block, 0, 3, 3})[0] == kInvalidCombination);
        // Unmap of a block that is not mapped.
        CHECK(w.Svc(0x20, {block, 0x14000000})[0] == kInvalidAddressState);
    }) == RunStatus::Idle);
}

// ---- threads -----------------------------------------------------------------------------------------

TEST_CASE("CreateThread: r0 priority, r1 entry, r2 argument, r3 stack top, r4 processor; out r1 handle") {
    World w;
    std::vector<SvcThreadCreated> created;
    std::vector<std::string> events;
    SvcHooks hooks;
    hooks.thread_created = [&](const SvcThreadCreated& info) {
        created.push_back(info);
        events.push_back("thread " + std::to_string(info.thread_id));
    };
    hooks.complete = [&](const SvcCall& call, const std::array<u32, 4>&) {
        events.push_back("complete " + std::to_string(call.number));
    };
    w.svc->SetHooks(hooks);
    Handle handle = 0;
    CHECK(w.Run([&] {
        const Out out = w.Svc(0x08, {40, kCode + 0x10, 0x12345678, 0x0FFF0000, 0xFFFFFFFE /* -2 */});
        CHECK(out[0] == kOk);
        handle = out[1];
        // The first handle of the table was the test thread's own; this is the second: generation 2,
        // slot 1.
        CHECK(handle == 0x00008002);
        // r1 held the entry and now holds the handle; r2 (the argument) is not an output.
        CHECK(out[2] == 0x12345678);
        const auto thread = w.kernel->GetObject<Thread>(handle);
        REQUIRE(thread != nullptr);
        CHECK(thread->thread_id == 2);
        CHECK(thread->nominal_priority == 40);
        CHECK(thread->entry_point == kCode + 0x10);
        CHECK(thread->stack_top == 0x0FFF0000);
        CHECK(thread->processor_id == 0);  // -2: the process's ideal processor, 0
        w.sched->Terminate(thread);       // before it runs (it has no code to run)
    }) == RunStatus::Idle);
    // The thread's line comes before the line of the call that made it.
    CHECK(events == std::vector<std::string>{"thread 2", "complete 8"});
    REQUIRE(created.size() == 1);
    CHECK(created[0].parent_id == 1);
    CHECK(created[0].thread_id == 2);
    CHECK(created[0].entry == kCode + 0x10);
    CHECK(created[0].arg == 0x12345678);
    CHECK(created[0].stack_top == 0x0FFF0000);
    CHECK(created[0].priority == 40);
    CHECK(created[0].requested_processor == -2);
    CHECK(created[0].resolved_processor == 0);
}

TEST_CASE("CreateThread: processors -1, 0, 1 and the refusals, with Azahar's result codes") {
    World w;
    std::vector<SvcThreadCreated> created;
    SvcHooks hooks;
    hooks.thread_created = [&](const SvcThreadCreated& info) { created.push_back(info); };
    w.svc->SetHooks(hooks);
    CHECK(w.Run([&] {
        const auto end = [&](Handle h) { w.sched->Terminate(w.kernel->GetObject<Thread>(h)); };
        Out out = w.Svc(0x08, {40, kCode, 0, 0x0FFF0000, 0xFFFFFFFF /* -1: any, core 0 */});
        CHECK(out[0] == kOk);
        end(out[1]);
        out = w.Svc(0x08, {40, kCode, 0, 0x0FFF0000, 1});
        CHECK(out[0] == kOk);
        end(out[1]);
        CHECK(w.Svc(0x08, {64, kCode, 0, 0, 0})[0] == kOutOfRange);
        CHECK(w.Svc(0x08, {0xFFFFFFFF, kCode, 0, 0, 0})[0] == kOutOfRange);
        CHECK(w.Svc(0x08, {10, kCode, 0, 0, 0})[0] == kNotAuthorized);   // better than the limit's 0x18
        CHECK(w.Svc(0x08, {40, kCode, 0, 0, 4})[0] == kOutOfRange);
        CHECK(w.Svc(0x08, {40, kCode, 0, 0, 0xFFFFFFFD /* -3 */})[0] == kOutOfRange);
        CHECK(w.Svc(0x08, {40, kCode, 0, 0, 2})[0] == kOutOfRange);      // New 3DS cores
        CHECK(w.Svc(0x08, {40, kCode, 0, 0, 3})[0] == kOutOfRange);
        CHECK(w.Svc(0x08, {40, 0x00300000, 0, 0, 0})[0] == 0xD8E007F5);  // the entry is not mapped
        // The handle register of a failed call is 0.
        CHECK(w.Svc(0x08, {64, kCode, 0, 0, 0})[1] == 0);
    }) == RunStatus::Idle);
    REQUIRE(created.size() == 2);  // only the successes
    CHECK(created[0].requested_processor == -1);
    CHECK(created[0].resolved_processor == 0);
    CHECK(created[1].requested_processor == 1);
    CHECK(created[1].resolved_processor == 1);
}

TEST_CASE("svcControlProcess: operation 7 lifts the creation restrictions; others are not implemented") {
    World w;
    CHECK(w.Run([&] {
        CHECK(w.Svc(0x08, {10, kCode, 0, 0, 0})[0] == kNotAuthorized);
        // r0 process (the current one), r1 operation 7, r2 = 1.
        CHECK(w.Svc(0xB3, {0xFFFF8001, 7, 1})[0] == kOk);
        CHECK(w.mgr->NoThreadRestrictions());
        const Out out = w.Svc(0x08, {10, kCode, 0, 0x0FFF0000, 0});
        CHECK(out[0] == kOk);
        w.sched->Terminate(w.kernel->GetObject<Thread>(out[1]));
        CHECK(w.Svc(0xB3, {0xFFFF8001, 7, 0})[0] == kOk);  // only 1 enables it
        CHECK_FALSE(w.mgr->NoThreadRestrictions());
        CHECK(w.Svc(0xB3, {0xFFFF8001, 1})[0] == kNotImplemented);
        CHECK(w.Svc(0xB3, {0xDEAD, 7, 1})[0] == kInvalidHandle);
    }) == RunStatus::Idle);
}

TEST_CASE("Get/SetThreadPriority and Get/ProcessId/ThreadId") {
    World w;
    CHECK(w.Run([&] {
        // GetThreadPriority: r1 handle; out r1.
        Out out = w.Svc(0x0B, {0, 0xFFFF8000});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 30);
        CHECK(w.Svc(0x0B, {0, 0xDEAD})[0] == kInvalidHandle);
        CHECK(w.Svc(0x0B, {0, 0xDEAD})[1] == 0);
        // SetThreadPriority: r0 handle, r1 priority. 64 is out of range, 10 is better than the limit.
        CHECK(w.Svc(0x0C, {0xFFFF8000, 64})[0] == kOutOfRange);
        CHECK(w.Svc(0x0C, {0xDEAD, 64})[0] == kOutOfRange);   // the priority is checked first
        CHECK(w.Svc(0x0C, {0xDEAD, 30})[0] == kInvalidHandle);
        CHECK(w.Svc(0x0C, {0xFFFF8000, 10})[0] == kNotAuthorized);
        CHECK(w.Svc(0x0C, {0xFFFF8000, 24})[0] == kOk);
        CHECK(w.Svc(0x0B, {0, 0xFFFF8000})[1] == 24);
        // The ids: GetThreadId r1 handle -> r1; GetProcessId r1 handle -> r1.
        out = w.Svc(0x37, {0, 0xFFFF8000});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 1);
        out = w.Svc(0x35, {0, 0xFFFF8001});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 11);  // the application's process id
        CHECK(w.Svc(0x35, {0, 0xFFFF8000})[0] == kInvalidHandle);  // a thread is not a process
        CHECK(w.Svc(0x37, {0, 0xFFFF8001})[0] == kInvalidHandle);
    }) == RunStatus::Idle);
}

TEST_CASE("SetThreadPriority reschedules: a thread made better than the others runs first") {
    World w;
    std::string order;
    Handle b_handle = 0;
    w.Spawn(30, [&] {
        order += "A1 ";
        // Raise thread B above this one (B has the next id and handle).
        CHECK(w.Svc(0x0C, {b_handle, 25})[0] == kOk);
        order += "A2 ";
    });
    const auto b = w.Spawn(35, [&] { order += "B "; });
    b_handle = 0x00008002;  // the second thread's handle
    REQUIRE(w.kernel->GetObject<Thread>(b_handle) == b);
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(order == "A1 B A2 ");
}

TEST_CASE("SleepThread: r0/r1 hold the nanoseconds, the registers stay, and time passes") {
    World w;
    CHECK(w.Run([&] {
        const u64 before = w.Now();
        // 2.5 ms as an s64 in r0/r1 (high word 0).
        const Out out = w.Svc(0x0A, {2500000, 0, 0x77});
        CHECK(out[0] == 2500000);  // untouched: SleepThread has no outputs
        CHECK(out[1] == 0);
        CHECK(out[2] == 0x77);
        const u64 elapsed = w.Now() - before;
        CHECK(elapsed >= NsToTicks(2500000));
        CHECK(elapsed <= NsToTicks(2500000) + 4);
    }) == RunStatus::Idle);
}

TEST_CASE("SleepThread(0) yields to threads of the same priority that are ready") {
    World w;
    std::string order;
    w.Spawn(30, [&] {
        order += "A1 ";
        w.Svc(0x0A, {0, 0});
        order += "A2 ";
    });
    w.Spawn(30, [&] { order += "B "; });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(order == "A1 B A2 ");
}

// ---- synchronisation objects --------------------------------------------------------------------------

TEST_CASE("handles are numbered as Azahar's table does: generation | slot << 15") {
    World w;
    CHECK(w.Run([&] {
        // The test thread's own handle is slot 0 (generation 1).
        CHECK(w.NewEvent() == 0x00008002);
        CHECK(w.NewEvent() == 0x00010003);
        CHECK(w.Create(0x13, {0, 0}) == 0x00018004);  // CreateMutex
        // Closing frees the slot, which is the first one reused (a new generation).
        CHECK(w.Svc(0x23, {0x00010003})[0] == kOk);
        CHECK(w.NewEvent() == 0x00010005);
    }) == RunStatus::Idle);
}

TEST_CASE("CloseHandle and DuplicateHandle") {
    World w;
    CHECK(w.Run([&] {
        const Handle event = w.NewEvent();
        // DuplicateHandle: r1 handle; out r1 the new one, a different slot, the same object.
        Out out = w.Svc(0x27, {0, event});
        CHECK(out[0] == kOk);
        const Handle copy = out[1];
        CHECK(copy != event);
        CHECK(w.kernel->GetObject<Event>(copy) == w.kernel->GetObject<Event>(event));
        // The current thread and process pseudo-handles duplicate into real handles.
        out = w.Svc(0x27, {0, 0xFFFF8000});
        CHECK(out[0] == kOk);
        CHECK(w.kernel->GetObject<Thread>(out[1]).get() == w.sched->CurrentThread());
        CHECK(w.Svc(0x27, {0, 0xDEAD})[0] == kInvalidHandle);
        CHECK(w.Svc(0x27, {0, 0xDEAD})[1] == 0);
        // CloseHandle: once, then again; the pseudo-handles cannot be closed.
        CHECK(w.Svc(0x23, {event})[0] == kOk);
        CHECK(w.Svc(0x23, {event})[0] == kInvalidHandle);
        CHECK(w.Svc(0x23, {copy})[0] == kOk);
        CHECK(w.Svc(0x23, {0xFFFF8000})[0] == kInvalidHandle);
        CHECK(w.Svc(0x23, {0xFFFF8001})[0] == kInvalidHandle);
        CHECK(w.Svc(0x23, {0})[0] == kInvalidHandle);
    }) == RunStatus::Idle);
}

TEST_CASE("events: reset types, Signal and Clear, and the resource limit's count") {
    World w;
    CHECK(w.Run([&] {
        const Handle once = w.NewEvent(0);
        const Handle sticky = w.NewEvent(1);
        const Handle pulse = w.NewEvent(2);
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::Event) == 3);
        // r0 handle, r2/r3 timeout. A timeout of 0 on an unsignalled event.
        CHECK(w.Svc(0x24, {once, 0, 0, 0})[0] == kTimeout);
        CHECK(w.Svc(0x18, {once})[0] == kOk);  // SignalEvent
        CHECK(w.Svc(0x24, {once, 0, 0, 0})[0] == kOk);       // consumed by the wait
        CHECK(w.Svc(0x24, {once, 0, 0, 0})[0] == kTimeout);
        CHECK(w.Svc(0x18, {sticky})[0] == kOk);
        CHECK(w.Svc(0x24, {sticky, 0, 0, 0})[0] == kOk);
        CHECK(w.Svc(0x24, {sticky, 0, 0, 0})[0] == kOk);     // sticky stays signalled
        CHECK(w.Svc(0x19, {sticky})[0] == kOk);              // ClearEvent
        CHECK(w.Svc(0x24, {sticky, 0, 0, 0})[0] == kTimeout);
        CHECK(w.Svc(0x18, {pulse})[0] == kOk);               // nobody waits: the pulse is lost
        CHECK(w.Svc(0x24, {pulse, 0, 0, 0})[0] == kTimeout);
        CHECK(w.Svc(0x18, {0xDEAD})[0] == kInvalidHandle);
        CHECK(w.Svc(0x19, {0xDEAD})[0] == kInvalidHandle);
        CHECK(w.Svc(0x18, {w.Create(0x13, {0, 0})})[0] == kInvalidHandle);  // a mutex is not an event
        // Closing the last handle releases the slot.
        CHECK(w.Svc(0x23, {once})[0] == kOk);
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::Event) == 2);
    }) == RunStatus::Idle);
}

TEST_CASE("mutexes: initially locked, recursion, and the release rules") {
    World w;
    Handle mutex = 0;
    Handle done = 0;
    std::shared_ptr<Thread> other;
    CHECK(w.Run([&] {
        done = w.NewEvent();
        // r1 initial_locked: the creating thread holds it.
        mutex = w.Create(0x13, {0, 1});
        CHECK(w.kernel->GetObject<Mutex>(mutex)->HoldingThread() == w.sched->CurrentThread());
        CHECK(w.Svc(0x24, {mutex, 0, 0, 0})[0] == kOk);   // recursion: a second lock
        CHECK(w.kernel->GetObject<Mutex>(mutex)->LockCount() == 2);
        CHECK(w.Svc(0x14, {mutex})[0] == kOk);            // ReleaseMutex
        CHECK(w.Svc(0x14, {mutex})[0] == kOk);
        // Not held by anyone: Mutex::Release answers WrongLockingThread for any thread that is not
        // the holder (its zero-count check cannot be reached).
        CHECK(w.Svc(0x14, {mutex})[0] == 0xD8E0041F);
        CHECK(w.Svc(0x14, {0xDEAD})[0] == kInvalidHandle);
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::Mutex) == 1);
        // Held by this thread, released by another: the wrong locking thread.
        CHECK(w.Svc(0x24, {mutex, 0, 0, 0})[0] == kOk);
        other = w.Spawn(31, [&] {
            CHECK(w.Svc(0x14, {mutex})[0] == 0xD8E0041F);
            CHECK(w.kernel->GetObject<Mutex>(mutex)->LockCount() == 1);
            w.Svc(0x18, {done});
        });
        // Wait for it (it runs when this thread blocks).
        CHECK(w.Svc(0x24, {done, kNever, kNever, kNever})[0] == kOk);
    }) == RunStatus::Idle);
}

TEST_CASE("semaphores: CreateSemaphore r1 initial, r2 maximum; ReleaseSemaphore r1 handle, r2 count") {
    World w;
    CHECK(w.Run([&] {
        Out out = w.Svc(0x15, {0, 1, 3});
        CHECK(out[0] == kOk);
        const Handle sem = out[1];
        // Release 2: the count before is 1 (out r1), and 3 is the maximum.
        out = w.Svc(0x16, {0, sem, 2});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 1);
        // Past the maximum: nothing changes.
        out = w.Svc(0x16, {0, sem, 1});
        CHECK(out[0] == kOutOfRangeKernel);
        CHECK(out[1] == 0);
        CHECK(w.kernel->GetObject<Semaphore>(sem)->AvailableCount() == 3);
        CHECK(w.Svc(0x16, {0, 0xDEAD, 1})[0] == kInvalidHandle);
        // Three waits take the three counts, the fourth times out.
        for (int i = 0; i < 3; i++) {
            CHECK(w.Svc(0x24, {sem, 0, 0, 0})[0] == kOk);
        }
        CHECK(w.Svc(0x24, {sem, 0, 0, 0})[0] == kTimeout);
        // initial > maximum is refused; the reserved slot is not given back (as in Azahar).
        out = w.Svc(0x15, {0, 3, 2});
        CHECK(out[0] == 0xD90007EE);
        CHECK(out[1] == 0);
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::Semaphore) == 2);
    }) == RunStatus::Idle);
}

TEST_CASE("timers: SetTimer has the low word of the interval in r1 and the high word in r4") {
    World w;
    CHECK(w.Run([&] {
        const Handle timer = w.Create(0x1A, {0, 0});  // one shot
        CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::Timer) == 1);
        // initial 1 ms (r2/r3), interval 2 ms (r1 low, r4 high 0).
        const u64 start = w.Now();
        CHECK(w.Svc(0x1B, {timer, 2000000, 1000000, 0, 0})[0] == kOk);
        CHECK(w.Svc(0x24, {timer, kNever, kNever, kNever})[0] == kOk);
        u64 elapsed = w.Now() - start;
        CHECK(elapsed >= NsToTicks(1000000));
        CHECK(elapsed <= NsToTicks(1000000) + 4);
        // The next shot is one interval after the first one's deadline.
        CHECK(w.Svc(0x24, {timer, kNever, kNever, kNever})[0] == kOk);
        elapsed = w.Now() - start;
        // The interval is converted to ticks on its own, so the second shot is the sum of the two
        // conversions after the start (804334), not the conversion of the sum (804335).
        CHECK(elapsed >= NsToTicks(1000000) + NsToTicks(2000000));
        CHECK(elapsed <= NsToTicks(1000000) + NsToTicks(2000000) + 4);
        // CancelTimer stops it; ClearTimer unsignals.
        CHECK(w.Svc(0x1C, {timer})[0] == kOk);
        CHECK(w.Svc(0x1D, {timer})[0] == kOk);
        CHECK(w.Svc(0x24, {timer, 0, 0, 0})[0] == kTimeout);
        // A negative initial value (r3 is the high word of r2/r3) or interval (r4) is refused,
        // before the handle is looked at.
        CHECK(w.Svc(0x1B, {timer, 0, 5, 0xFFFFFFFF, 0})[0] == kOutOfRangeKernel);
        CHECK(w.Svc(0x1B, {timer, 0, 5, 0, 0x80000000})[0] == kOutOfRangeKernel);
        CHECK(w.Svc(0x1B, {0xDEAD, 0, 5, 0xFFFFFFFF, 0})[0] == kOutOfRangeKernel);
        CHECK(w.Svc(0x1B, {0xDEAD, 0, 5, 0, 0})[0] == kInvalidHandle);
        CHECK(w.Svc(0x1C, {0xDEAD})[0] == kInvalidHandle);
        CHECK(w.Svc(0x1D, {0xDEAD})[0] == kInvalidHandle);
    }) == RunStatus::Idle);
}

TEST_CASE("timers: a 64-bit interval uses r4 as its high word") {
    World w;
    CHECK(w.Run([&] {
        const Handle timer = w.Create(0x1A, {0, 0});
        // interval = 0x1_0000_0000 ns (r1 = 0, r4 = 1) = 4.29 s; initial 1 ms.
        const u64 start = w.Now();
        CHECK(w.Svc(0x1B, {timer, 0, 1000000, 0, 1})[0] == kOk);
        CHECK(w.Svc(0x24, {timer, kNever, kNever, kNever})[0] == kOk);
        CHECK(w.Now() - start >= NsToTicks(1000000));
        CHECK(w.kernel->GetObject<Timer>(timer)->IntervalDelay() == 0x100000000ll);
        CHECK(w.kernel->GetObject<Timer>(timer)->InitialDelay() == 1000000);
        CHECK(w.Svc(0x1C, {timer})[0] == kOk);  // a periodic timer would keep the scheduler busy
    }) == RunStatus::Idle);
}

// ---- WaitSynchronization ------------------------------------------------------------------------------

TEST_CASE("WaitSynchronization1: invalid handle, a timeout that expires, and a signal that wakes") {
    World w;
    CHECK(w.Run([&] {
        CHECK(w.Svc(0x24, {0xDEAD, 0, 0, 0})[0] == kInvalidHandle);
        const Handle event = w.NewEvent();
        // 10 ms (r2 = low word) on an event nobody signals.
        const u64 start = w.Now();
        const Out out = w.Svc(0x24, {event, 0x77, 10000000, 0});
        CHECK(out[0] == kTimeout);
        CHECK(out[1] == 0x77);  // r1 is not an output of this call
        CHECK(w.Now() - start >= NsToTicks(10000000));
        // An object that is not waitable.
        const Handle arbiter = w.Create(0x21, {});
        CHECK(w.Svc(0x24, {arbiter, 0, 0, 0})[0] == kInvalidHandle);
    }) == RunStatus::Idle);
}

TEST_CASE("a blocked WaitSynchronization1 completes when the thread runs again, after the signal") {
    World w;
    std::vector<std::string> events;
    SvcHooks hooks;
    hooks.before = [&](const SvcCall& call) {
        events.push_back("before " + std::to_string(call.thread_id) + ":" + std::to_string(call.number));
    };
    hooks.complete = [&](const SvcCall& call, const std::array<u32, 4>& out) {
        events.push_back("complete " + std::to_string(call.thread_id) + ":" +
                         std::to_string(call.number) + "=" + std::to_string(out[0]));
    };
    w.svc->SetHooks(hooks);
    Handle event = 0;
    Out wait;
    std::vector<SvcCall> pending_while_blocked;
    w.Spawn(30, [&] {  // thread 1: makes the event and waits for it for ever
        event = w.NewEvent();
        wait = w.Svc(0x24, {event, kNever, kNever, kNever});
    });
    w.Spawn(31, [&] {  // thread 2: runs once thread 1 blocks
        pending_while_blocked = w.svc->PendingCalls();
        w.Svc(0x18, {event});  // SignalEvent wakes thread 1, which preempts this one
        events.push_back("after signal");
    });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(wait[0] == kOk);
    // The pending call of thread 1 was visible while it was blocked (thread 2's own call has begun).
    REQUIRE(pending_while_blocked.size() == 1);
    CHECK(pending_while_blocked[0].thread_id == 1);
    CHECK(pending_while_blocked[0].number == 0x24);
    // Thread 1's wait completes only when it runs again: after thread 2's SignalEvent completed
    // (at once), and before thread 2 goes on (thread 1 is the better priority).
    // The numbers are in decimal: 0x17 CreateEvent is 23, 0x24 WaitSynchronization1 is 36 and 0x18
    // SignalEvent is 24.
    CHECK(events == std::vector<std::string>{
                        "before 1:23", "complete 1:23=0", "before 1:36", "before 2:24",
                        "complete 2:24=0", "complete 1:36=0", "after signal"});
    CHECK(w.svc->PendingCalls().empty());
}

TEST_CASE("WaitSynchronizationN: any object, the index in r1, wait all, and the timeouts") {
    World w;
    constexpr u32 kHandles = kData;
    Handle a = 0, b = 0, c = 0;
    Out any_signalled, any_blocked, all_blocked, any_timeout, all_timeout;
    w.Spawn(30, [&] {
        a = w.NewEvent();
        b = w.NewEvent();
        c = w.NewEvent();
        w.Poke32(kHandles, a);
        w.Poke32(kHandles + 4, b);
        w.Poke32(kHandles + 8, c);
        // A wait on any where only the second is signalled: it is taken at once, index 1.
        w.Svc(0x18, {b});
        // r0/r4 timeout (low/high), r1 the handles, r2 the count, r3 wait for all.
        any_signalled = w.Svc(0x25, {0, kHandles, 3, 0, 0});
        // Blocks for ever on any: thread 2 signals the third.
        any_blocked = w.Svc(0x25, {kNever, kHandles, 3, 0, kNever});
        // Blocks on all: thread 2 signals all three, one after the other.
        all_blocked = w.Svc(0x25, {kNever, kHandles, 3, 1, kNever});
        // Timeouts: a wait on any and a wait on all, 1 ms, with nothing signalled.
        any_timeout = w.Svc(0x25, {1000000, kHandles, 3, 0, 0});
        all_timeout = w.Svc(0x25, {1000000, kHandles, 3, 1, 0});
    });
    w.Spawn(31, [&] {
        w.Svc(0x18, {c});  // wakes the wait on any: index 2
        w.Svc(0x0A, {0, 0});
        w.Svc(0x18, {a});  // a wait on all needs the three at once
        w.Svc(0x18, {b});
        w.Svc(0x18, {c});
    });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(any_signalled[0] == kOk);
    CHECK(any_signalled[1] == 1);
    CHECK(any_blocked[0] == kOk);
    CHECK(any_blocked[1] == 2);
    CHECK(all_blocked[0] == kOk);
    CHECK(all_blocked[1] == 0xFFFFFFFF);  // the index is -1 for a wait on all
    CHECK(any_timeout[0] == kTimeout);
    CHECK(any_timeout[1] == 0xFFFFFFFF);
    CHECK(all_timeout[0] == kTimeout);
    CHECK(all_timeout[1] == 0xFFFFFFFF);
}

TEST_CASE("WaitSynchronizationN: arguments, and the outputs that are left alone") {
    World w;
    CHECK(w.Run([&] {
        const Handle a = w.NewEvent();
        const Handle b = w.NewEvent(1);  // sticky
        w.Poke32(kData, a);
        w.Poke32(kData + 4, b);
        w.Poke32(kData + 8, 0xDEAD);
        // The handles' address must be mapped.
        Out out = w.Svc(0x25, {0, 0x00500000, 1, 0, 0});
        CHECK(out[0] == kInvalidPointer);
        out = w.Svc(0x25, {0, 0, 0, 0, 0});
        CHECK(out[0] == kInvalidPointer);
        // A negative count; a handle that is not valid (the third).
        CHECK(w.Svc(0x25, {0, kData, 0xFFFFFFFF, 0, 0})[0] == kOutOfRange);
        CHECK(w.Svc(0x25, {0, kData, 3, 0, 0})[0] == kInvalidHandle);
        // A refusal with a timeout of 0 sets no index: r1 is 0 (Azahar's wrapper writes an
        // uninitialised variable, and the reference reads 0).
        out = w.Svc(0x25, {0, kData, 2, 0, 0});
        CHECK(out[0] == kTimeout);
        CHECK(out[1] == 0);
        // A wait on all that is satisfied at once does not set the index either (conf_wait.txt:
        // wn.all_full_index is 0 although the program preloaded 0x77).
        w.Svc(0x18, {a});
        w.Svc(0x18, {b});
        out = w.Svc(0x25, {0, kData, 2, 1, 0});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0);
        // No handles: a wait on all is satisfied (vacuously); a wait on any with timeout 0 is not.
        out = w.Svc(0x25, {0, kData, 0, 1, 0});
        CHECK(out[0] == kOk);
        out = w.Svc(0x25, {0, kData, 0, 0, 0});
        CHECK(out[0] == kTimeout);
        // wait_all is a bool: any non-zero value.
        // Any non-zero value is true: a (consumed above) is not signalled, so the wait on all
        // cannot be satisfied; a wait on any would have found the sticky b.
        out = w.Svc(0x25, {0, kData, 2, 0x100, 0});
        CHECK(out[0] == kTimeout);
    }) == RunStatus::Idle);
}

TEST_CASE("WaitSynchronizationN: a wait on any takes the first object that is available") {
    World w;
    CHECK(w.Run([&] {
        const Handle a = w.NewEvent(1);
        const Handle b = w.NewEvent(1);
        w.Poke32(kData, a);
        w.Poke32(kData + 4, b);
        w.Svc(0x18, {a});
        w.Svc(0x18, {b});
        const Out out = w.Svc(0x25, {0, kData, 2, 0, 0});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0);
    }) == RunStatus::Idle);
}

// ---- address arbiters -------------------------------------------------------------------------------

TEST_CASE("address arbiter: Signal wakes a waiter, WaitIfLessThan waits, and the results") {
    World w;
    Handle arbiter = 0;
    Out wait, signal;
    w.Spawn(30, [&] {
        arbiter = w.Create(0x21, {});
        w.Poke32(kData, 0);
        // r0 arbiter, r1 address, r2 type, r3 value, r4/r5 timeout. Type 1: wait if the word (0) is
        // less than the value (1): it is, so the thread waits.
        wait = w.Svc(0x22, {arbiter, kData, 1, 1, 0, 0});
    });
    w.Spawn(31, [&] {
        // Type 0, Signal: wake one thread waiting on the address.
        signal = w.Svc(0x22, {arbiter, kData, 0, 1, 0, 0});
    });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(wait[0] == kOk);
    CHECK(signal[0] == kOk);
    CHECK(w.process->resource_limit->GetCurrentValue(ResourceLimitType::AddressArbiter) == 1);
}

TEST_CASE("address arbiter: the types with a timeout return Timeout even when they do not wait") {
    World w;
    CHECK(w.Run([&] {
        const Handle arbiter = w.Create(0x21, {});
        w.Poke32(kData, 5);
        // Type 3: wait if less than, with a timeout (r4/r5). 5 < 3 is false: no wait, and Timeout.
        CHECK(w.Svc(0x22, {arbiter, kData, 3, 3, 1000000, 0})[0] == kTimeout);
        // 5 < 9: waits 1 ms, and the timeout is the result.
        const u64 start = w.Now();
        CHECK(w.Svc(0x22, {arbiter, kData, 3, 9, 1000000, 0})[0] == kTimeout);
        CHECK(w.Now() - start >= NsToTicks(1000000));
        // Type 4: decrement the word and wait if it was less than the value (with a timeout).
        CHECK(w.Svc(0x22, {arbiter, kData, 4, 9, 1000000, 0})[0] == kTimeout);
        CHECK(w.Peek32(kData) == 4);
        // Type 2: decrement and wait; with the word not less than the value, nothing changes.
        CHECK(w.Svc(0x22, {arbiter, kData, 2, 4, 0, 0})[0] == kOk);
        CHECK(w.Peek32(kData) == 4);
        // An unknown type, and a handle that is not an arbiter.
        CHECK(w.Svc(0x22, {arbiter, kData, 5, 0, 0, 0})[0] == 0xD8E093ED);
        CHECK(w.Svc(0x22, {0xDEAD, kData, 0, 0, 0, 0})[0] == kInvalidHandle);
        CHECK(w.Svc(0x22, {w.NewEvent(), kData, 0, 0, 0, 0})[0] == kInvalidHandle);
    }) == RunStatus::Idle);
}

// ---- information ------------------------------------------------------------------------------------

TEST_CASE("GetSystemTick returns the game clock's ticks in r0/r1") {
    World w;
    CHECK(w.Run([&] {
        w.Advance(5 * kMs);
        const u64 now = w.Now();
        const Out out = w.Svc(0x28, {0xFFFF, 0xFFFF});
        CHECK(out[0] == static_cast<u32>(now));
        CHECK(out[1] == static_cast<u32>(now >> 32));
        // A second later the high word is still 0 and the low one has grown.
        w.Advance(1000 * kMs);
        const Out later = w.Svc(0x28, {});
        CHECK(later[0] > out[0]);
        CHECK(later[0] - out[0] == static_cast<u32>(NsToTicks(1000 * kMs)));
        // More than 2^32 ticks (16 s): the high word counts.
        w.Advance(20000 * kMs);
        const u64 far = w.Now();
        CHECK(far > 0xFFFFFFFFull);
        const Out high = w.Svc(0x28, {});
        CHECK(high[0] == static_cast<u32>(far));
        CHECK(high[1] == static_cast<u32>(far >> 32));
        CHECK(high[1] == 1);
    }) == RunStatus::Idle);
}

TEST_CASE("GetHandleInfo: elapsed ticks, the reference count and the stubbed types") {
    World w;
    w.Advance(3 * kMs);
    CHECK(w.Run([&] {
        const Handle event = w.NewEvent();
        // r1 handle, r2 type; out r1/r2 an s64. Type 0 on the process: the tick it was created at.
        Out out = w.Svc(0x29, {0, 0xFFFF8001, 0});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0);  // the process was made at tick 0
        CHECK(out[2] == 0);
        // Type 0 on something else is 0. Type 1: the object's reference count, here the table's
        // handle: Azahar's use_count() - 1 with its own local copy.
        out = w.Svc(0x29, {0, event, 0});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0);
        out = w.Svc(0x29, {0, event, 1});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 1);
        CHECK(out[2] == 0);
        CHECK(w.Svc(0x29, {0, event, 2})[0] == kOk);
        CHECK(w.Svc(0x29, {0, event, 0x32107})[0] == kOk);
        CHECK(w.Svc(0x29, {0, event, 2})[1] == 0);
        CHECK(w.Svc(0x29, {0, event, 3})[0] == kInvalidEnum);
        CHECK(w.Svc(0x29, {0, 0xDEAD, 0})[0] == kInvalidHandle);
        // A duplicate raises the count.
        const Handle copy = w.Svc(0x27, {0, event})[1];
        CHECK(copy != 0);
        CHECK(w.Svc(0x29, {0, event, 1})[1] == 2);
    }) == RunStatus::Idle);
}

TEST_CASE("GetSystemInfo: memory usage by region, kernel counters, the homebrew types") {
    World w;
    CHECK(w.Run([&] {
        REQUIRE(w.Svc(0x01, {3, 0x08000000, 0, 0x3000, 3})[0] == kOk);
        // Type 0, param: 0 all, 1 APPLICATION, 2 SYSTEM, 3 BASE. BASE has this thread's TLS page.
        Out out = w.Svc(0x2A, {0, 0, 1});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x3000);
        CHECK(out[2] == 0);
        CHECK(w.Svc(0x2A, {0, 0, 0})[1] == 0x4000);
        CHECK(w.Svc(0x2A, {0, 0, 2})[1] == 0);
        CHECK(w.Svc(0x2A, {0, 0, 3})[1] == 0x1000);
        out = w.Svc(0x2A, {0, 0, 4});  // an unknown region: success, 0
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0);
        // Type 2 is not implemented in Azahar (0); type 26 is the number of processes the kernel
        // started.
        CHECK(w.Svc(0x2A, {0, 2, 0})[0] == kOk);
        CHECK(w.Svc(0x2A, {0, 2, 0})[1] == 0);
        CHECK(w.Svc(0x2A, {0, 26, 0})[1] == 5);
        // The Luma types: the real size of the APPLICATION region, and is it a New 3DS.
        CHECK(w.Svc(0x2A, {0, 0x10000, 0x80})[1] == 0x04000000);
        CHECK(w.Svc(0x2A, {0, 0x10000, 0x201})[1] == 0);
        CHECK(w.Svc(0x2A, {0, 0x10000, 5})[0] == kOk);
        // 0x10001 only works on a New 3DS.
        CHECK(w.Svc(0x2A, {0, 0x10001, 0})[0] == kInvalidEnum);
        // Anything else is success with 0, whatever the arguments.
        out = w.Svc(0x2A, {0, 0x123456, 7});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0);
        CHECK(out[2] == 0);
    }) == RunStatus::Idle);
}

TEST_CASE("GetSystemInfo on a New 3DS memory mode") {
    World w(6);  // memory mode 6: a New 3DS layout, APPLICATION 0x07C00000
    CHECK(w.Run([&] {
        CHECK(w.Svc(0x2A, {0, 0x10001, 0})[0] == kOk);
        CHECK(w.Svc(0x2A, {0, 0x10000, 0x201})[1] == 1);
        CHECK(w.Svc(0x2A, {0, 0x10000, 0x80})[1] == 0x07C00000);
    }) == RunStatus::Idle);
}

TEST_CASE("GetProcessInfo: memory used, types that are valid but not implemented, and the errors") {
    World w;
    CHECK(w.Run([&] {
        REQUIRE(w.Svc(0x01, {3, 0x08000000, 0, 0x3000, 3})[0] == kOk);
        // r1 handle, r2 type; out r1/r2. Types 0 and 2: the memory used (heap and this thread's TLS).
        Out out = w.Svc(0x2B, {0, 0xFFFF8001, 0});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x4000);
        CHECK(out[2] == 0);
        CHECK(w.Svc(0x2B, {0, 0xFFFF8001, 2})[1] == 0x4000);
        // Types 1, 3 to 8: valid, not implemented in Azahar, success.
        for (const u32 type : {1u, 3u, 4u, 5u, 6u, 7u, 8u}) {
            CHECK(w.Svc(0x2B, {0, 0xFFFF8001, type})[0] == kOk);
        }
        // 21 to 23 return a different error from the unknown types.
        for (const u32 type : {21u, 22u, 23u}) {
            CHECK(w.Svc(0x2B, {0, 0xFFFF8001, type})[0] == kNotImplemented);
        }
        CHECK(w.Svc(0x2B, {0, 0xFFFF8001, 19})[0] == kInvalidEnum);
        CHECK(w.Svc(0x2B, {0, 0xFFFF8001, 9})[0] == kInvalidEnum);
        CHECK(w.Svc(0x2B, {0, 0xDEAD, 0})[0] == kInvalidHandle);
        // A thread handle is not a process.
        CHECK(w.Svc(0x2B, {0, 0xFFFF8000, 0})[0] == kInvalidHandle);
        // The memory used must be a multiple of a page.
        w.process->memory_used = 0x4123;
        CHECK(w.Svc(0x2B, {0, 0xFFFF8001, 0})[0] == kMisalignedSize);
    }) == RunStatus::Idle);
}

TEST_CASE("GetThreadInfo type 0x10000 is the TLS address") {
    World w;
    CHECK(w.Run([&] {
        const Out out = w.Svc(0x2C, {0, 0xFFFF8000, 0x10000});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x1FF82000);  // slot 0 of page 0
        CHECK(out[2] == 0);
        CHECK(w.Svc(0x2C, {0, 0xFFFF8000, 0})[0] == kInvalidEnum);
        CHECK(w.Svc(0x2C, {0, 0xDEAD, 0x10000})[0] == kInvalidHandle);
        CHECK(w.Svc(0x2C, {0, 0xFFFF8001, 0x10000})[0] == kInvalidHandle);
    }) == RunStatus::Idle);
}

TEST_CASE("resource limits: GetResourceLimit and the limit and current values") {
    World w;
    CHECK(w.Run([&] {
        // r1 process handle; out r1 a handle to its resource limit.
        const Out got = w.Svc(0x38, {0, 0xFFFF8001});
        REQUIRE(got[0] == kOk);
        const Handle limit = got[1];
        CHECK(w.Svc(0x38, {0, 0xDEAD})[0] == kInvalidHandle);
        // Names: Priority, Commit, Thread, Event, Mutex, Semaphore, Timer, SharedMemory,
        // AddressArbiter, CpuTime.
        for (u32 i = 0; i < 10; i++) {
            w.Poke32(kData + 4 * i, i);
        }
        constexpr u32 kValues = kData + 0x100;
        // r0 values (s64 each), r1 limit, r2 names, r3 count. 0x39 is the limit values (Azahar's
        // APPLICATION limit), 0x3A the current ones.
        CHECK(w.Svc(0x39, {kValues, limit, kData, 10})[0] == kOk);
        const std::vector<u64> expected_limits = {0x18, 0x04000000, 0x20, 0x20, 0x20, 8, 8, 0x10, 2, 0};
        for (u32 i = 0; i < 10; i++) {
            CHECK(w.memory->Read<u64>(kValues + 8 * i) == expected_limits[i]);
        }
        // The current values: this thread, the TLS page of its memory, and what the test made.
        const Handle event = w.NewEvent();
        (void)event;
        CHECK(w.Svc(0x3A, {kValues, limit, kData, 10})[0] == kOk);
        CHECK(w.memory->Read<u64>(kValues + 8 * 0) == 0);   // Priority
        CHECK(w.memory->Read<u64>(kValues + 8 * 2) == 1);   // Thread: this one
        CHECK(w.memory->Read<u64>(kValues + 8 * 3) == 1);   // Event: the one above
        CHECK(w.memory->Read<u64>(kValues + 8 * 4) == 0);   // Mutex
        // A name past the last (10) is refused, after the values before it were written. (For the
        // current values Azahar indexes its array with whatever it is given; this refuses too.)
        w.Poke32(kData + 0x80, 1);  // Commit
        w.Poke32(kData + 0x84, 10);
        CHECK(w.Svc(0x39, {kValues, limit, kData + 0x80, 2})[0] == kInvalidEnum);
        CHECK(w.memory->Read<u64>(kValues) == 0x04000000);
        CHECK(w.Svc(0x3A, {kValues, limit, kData + 0x80, 2})[0] == kInvalidEnum);
        CHECK(w.Svc(0x39, {kValues, 0xDEAD, kData, 1})[0] == kInvalidHandle);
        CHECK(w.Svc(0x3A, {kValues, 0xFFFF8001, kData, 1})[0] == kInvalidHandle);
        // A count of 0 writes nothing.
        CHECK(w.Svc(0x39, {kValues, limit, kData, 0})[0] == kOk);
    }) == RunStatus::Idle);
}

// ---- ports and sessions -----------------------------------------------------------------------------

TEST_CASE("ConnectToPort: names, the handler and the results Azahar gives") {
    World w;
    std::vector<std::string> asked;
    CHECK(w.Run([&] {
        w.PokeString(kData, "srv:");
        // Without a handler every name is unknown.
        CHECK(w.Svc(0x2D, {0, kData})[0] == kNotFound);
        w.svc->SetConnectToPortHandler([&](const std::string& name, std::shared_ptr<Object>* session) {
            asked.push_back(name);
            if (name == "srv:") {
                *session = w.kernel->NewEvent(ResetType::OneShot);  // a stand-in for a session
                return kResultSuccess;
            }
            return Result{kNotFound};
        });
        // r1 the address of the name; out r1 the handle.
        Out out = w.Svc(0x2D, {0, kData});
        CHECK(out[0] == kOk);
        CHECK(out[1] == 0x00008002);
        CHECK(w.kernel->GetObject<Event>(out[1]) != nullptr);
        // An unknown name: the handler's result, no handle.
        w.PokeString(kData + 0x20, "nope");
        out = w.Svc(0x2D, {0, kData + 0x20});
        CHECK(out[0] == kNotFound);
        CHECK(out[1] == 0);
        // The longest name is 11 characters; 12 is too long and never reaches the handler.
        w.PokeString(kData + 0x40, "abcdefghijk");
        CHECK(w.Svc(0x2D, {0, kData + 0x40})[0] == kNotFound);  // 11: asked, unknown
        w.PokeString(kData + 0x60, "abcdefghijkl");
        CHECK(w.Svc(0x2D, {0, kData + 0x60})[0] == kPortNameTooLong);
        // The name address must be mapped.
        CHECK(w.Svc(0x2D, {0, 0x00500000})[0] == kNotFound);
        CHECK(w.Svc(0x2D, {0, 0})[0] == kNotFound);
        // The name ends at an unmapped byte as at a NUL: the last bytes of the page.
        w.memory->WriteBlock(kData + 0xFFC, "srv:", 4);  // the next page is not mapped
        CHECK(w.Svc(0x2D, {0, kData + 0xFFC})[0] == kOk);
    }) == RunStatus::Idle);
    CHECK(asked == std::vector<std::string>{"srv:", "nope", "abcdefghijk", "srv:"});
}

TEST_CASE("SendSyncRequest goes to the handler; a handler that blocks the thread parks it") {
    World w;
    std::vector<std::pair<u32, Handle>> requests;
    std::shared_ptr<Thread> waiting;
    Out immediate, refused, blocked, errored;
    w.Spawn(30, [&] {
        // No handler: the handle is not a session.
        refused = w.Svc(0x32, {0x1234});
        w.svc->SetSendSyncRequestHandler([&](Thread& thread, Handle handle) -> Result {
            requests.emplace_back(thread.thread_id, handle);
            switch (handle) {
            case 1: return kResultSuccess;
            case 2: return kInvalidHandle;
            case 3:
                // The reply is not ready: the thread waits for it.
                thread.status = ThreadStatus::WaitIPC;
                waiting = w.sched->CurrentThreadPtr();
                return kResultSuccess;
            default: return 0xC920181A;
            }
        });
        immediate = w.Svc(0x32, {1, 0x55});
        errored = w.Svc(0x32, {2});
        blocked = w.Svc(0x32, {3, 0x66});
        waiting.reset();
    });
    w.Spawn(31, [&] {
        REQUIRE(waiting != nullptr);
        CHECK(waiting->status == ThreadStatus::WaitIPC);
        // The reply arrives with a result: it becomes r0.
        waiting->ResumeFromWait(Result{0xC920181A});
    });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(refused[0] == kInvalidHandle);
    CHECK(immediate[0] == kOk);
    CHECK(immediate[1] == 0x55);  // no outputs: r1 is left alone
    CHECK(errored[0] == kInvalidHandle);
    CHECK(blocked[0] == 0xC920181A);
    CHECK(blocked[1] == 0x66);
    CHECK(requests == std::vector<std::pair<u32, Handle>>{{1, 1}, {1, 2}, {1, 3}});
}

TEST_CASE("SendSyncRequest: a wake-up with no result leaves the handler's result in r0") {
    World w;
    std::shared_ptr<Thread> waiting;
    Out out;
    w.Spawn(30, [&] {
        w.svc->SetSendSyncRequestHandler([&](Thread& thread, Handle) -> Result {
            thread.status = ThreadStatus::WaitIPC;
            waiting = w.sched->CurrentThreadPtr();
            return kResultSuccess;
        });
        out = w.Svc(0x32, {7});
        waiting.reset();
    });
    w.Spawn(31, [&] { waiting->ResumeFromWait(std::nullopt); });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(out[0] == kOk);
}

// ---- OutputDebugString ------------------------------------------------------------------------------

TEST_CASE("OutputDebugString hands the text to the hook and leaves the registers alone") {
    World w;
    std::vector<std::string> texts;
    SvcHooks hooks;
    hooks.debug_string = [&](std::string_view text) { texts.emplace_back(text); };
    w.svc->SetHooks(hooks);
    CHECK(w.Run([&] {
        w.PokeString(kData, "hello world");
        // r0 address, r1 length.
        const Out out = w.Svc(0x3D, {kData, 5});
        CHECK(out[0] == kData);
        CHECK(out[1] == 5);
        w.Svc(0x3D, {kData, 11});
        w.Svc(0x3D, {kData, 0});            // length 0: the gdb HIO request, ignored
        w.Svc(0x3D, {kData, 0xFFFFFFFF});   // negative length
        w.Svc(0x3D, {kData, 0x7FFFFFFF});   // far too long: ignored, not allocated
        w.Svc(0x3D, {0x00500000, 4});       // unmapped address
        // The text is not cut at a NUL (the length decides); an unmapped part reads as zeros.
        w.memory->WriteBlock(kData + 0xFF8, "ab\0cdefg", 8);
        w.Svc(0x3D, {kData + 0xFF8, 12});
    }) == RunStatus::Idle);
    REQUIRE(texts.size() == 3);
    CHECK(texts[0] == "hello");
    CHECK(texts[1] == "hello world");
    CHECK(texts[2] == std::string("ab\0cdefg\0\0\0\0", 12));
}


TEST_CASE("WaitSynchronization on a thread handle waits until the thread has exited") {
    World w;
    Out join, join_again;
    std::string order;
    w.Spawn(30, [&] {
        // The second thread's handle: generation 2, slot 1. It runs when this one blocks.
        join = w.Svc(0x24, {0x00008002, kNever, kNever, kNever});
        order += "joined ";
        join_again = w.Svc(0x24, {0x00008002, 0, 0, 0});  // a dead thread stays signalled
    });
    w.Spawn(31, [&] { order += "child "; });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(join[0] == kOk);
    CHECK(join_again[0] == kOk);
    CHECK(order == "child joined ");
}

TEST_CASE("address arbiter: a signal of -1 (r3 = 0xFFFFFFFF) wakes every waiter") {
    World w;
    Handle arbiter = 0;
    int woken = 0;
    std::vector<Out> waits(3);
    w.Spawn(30, [&] {
        arbiter = w.Create(0x21, {});
        w.Poke32(kData, 0);
    });
    for (int i = 0; i < 3; i++) {
        w.Spawn(31 + i, [&, i] {
            waits[static_cast<std::size_t>(i)] = w.Svc(0x22, {arbiter, kData, 1, 1, 0, 0});
            woken++;
        });
    }
    w.Spawn(40, [&] {
        CHECK(woken == 0);
        // Type 0 with a count of -1 wakes all of them (the count is a signed word).
        CHECK(w.Svc(0x22, {arbiter, kData, 0, 0xFFFFFFFF, 0, 0})[0] == kOk);
    });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(woken == 3);
    for (const Out& wait : waits) {
        CHECK(wait[0] == kOk);
    }
}

TEST_CASE("SetThreadPriority updates the mutexes the thread waits for: the holder inherits") {
    World w;
    Handle mutex = 0;
    u32 holder_priority_before = 0, holder_priority_after = 0;
    w.Spawn(40, [&] {  // thread 1: holds the mutex for a while
        mutex = w.Create(0x13, {0, 1});
        w.Svc(0x0A, {5000000, 0});
        w.Svc(0x14, {mutex});
    });
    w.Spawn(41, [&] { w.Svc(0x24, {mutex, kNever, kNever, kNever}); w.Svc(0x14, {mutex}); });
    w.Spawn(45, [&] {
        // Thread 2 (handle 0x8002) waits for the mutex that thread 1 (handle 0x1) holds. Thread 1 is
        // the better of the two, so it keeps its priority until thread 2 is raised above it.
        holder_priority_before = w.Svc(0x0B, {0, 0x00000001})[1];
        CHECK(w.Svc(0x0C, {0x00008002, 30})[0] == kOk);
        holder_priority_after = w.Svc(0x0B, {0, 0x00000001})[1];
    });
    CHECK(w.sched->Run({}, true) == RunStatus::Idle);
    CHECK(holder_priority_before == 40);
    CHECK(holder_priority_after == 30);
}

TEST_CASE("a full handle table answers OutOfHandles and the new object is dropped") {
    World w;
    CHECK(w.Run([&] {
        for (int i = 0; i < 4095; i++) {  // the test thread has one of the 4096 slots
            Handle h = 0;
            REQUIRE(w.kernel->AddHandle(&h, w.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
        }
        const Out out = w.Svc(0x17, {0, 0});
        CHECK(out[0] == 0xD8600413);
        CHECK(out[1] == 0);
        CHECK(w.Svc(0x13, {0, 0})[0] == 0xD8600413);
        CHECK(w.Svc(0x27, {0, 0xFFFF8000})[0] == 0xD8600413);
    }) == RunStatus::Idle);
}

#endif  // __has_include("recomp.h")
