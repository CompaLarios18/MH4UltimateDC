// mh4n_cpu_interp (N1-28): the fallback interpreter. Three kinds of test:
//   - synthetic programs written here as machine words, run through a code host with no code library, so
//     that every function is "missing" and goes to the interpreter: the end of an interpreted call (return
//     address and stack pointer), Thumb and ARM entries, system calls, UNPREDICTABLE encodings, an
//     unsupported one, preemption;
//   - vectors made by native/src/cpu/interp/make_test_data.py: random states run through the C the
//     recompiler emits for random instruction words of every form, and what the interpreter must reproduce
//     (registers, flags, VFP state, a 64 KiB buffer, the system call or trap raised);
//   - the 3DS test program port/tests/svc_test.c run by the native runtime with all its functions recompiled,
//     with its worker functions and functions called through pointers forced into the interpreter, and with
//     no recompiled function at all, against the output Azahar gave (port/tests/expected/svc_test.txt).
// No game data: the programs and words are synthetic.
#include <doctest/doctest.h>

#if !__has_include("recomp.h") || !defined(MH4N_INTERP_AVAILABLE)
TEST_CASE("interp_test needs recomp.h and the generated handlers (the repository around native/, Python 3)" *
          doctest::skip()) {}
#else

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../src/cpu/interp/interp.h"
#include "mh4n/crc32.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"
#include "mh4n/svc.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr mh4n::u32 kCode = 0x00100000;
constexpr mh4n::u32 kStack = 0x00200000;
constexpr mh4n::u32 kReturn = 0x00180000;  // where interpreted calls are told to return (never executed)

// The folder make_test_data.py wrote (empty if the build did not make it). svc_none.so is its last output.
std::string DataDir() {
#ifdef MH4N_INTERP_TEST_DATA_DIR
    const std::string dir = MH4N_INTERP_TEST_DATA_DIR;
    if (std::filesystem::exists(dir + "/svc_none.so")) {
        return dir;
    }
#endif
    return {};
}

bool Required() {
    const char* require = std::getenv("MH4N_REQUIRE_TEST_DATA");
    return require != nullptr && std::string(require) == "1";
}

// The cases that need the data built by make_test_data.py are the done-when of N1-28: a machine that
// lost Python or Unicorn must not turn them into silent passes when the build asks for them.
bool HaveData() {
    if (!DataDir().empty()) {
        return true;
    }
    if (Required()) {
        FAIL("the interpreter's test data was not built, and MH4N_REQUIRE_TEST_DATA=1");
    }
    WARN_MESSAGE(false, "skipped: the interpreter's test data was not built");
    return false;
}

// The vectors need the harness that runs the recompiled side, which exists on Linux and macOS only.
bool HaveVectors() {
    if (!HaveData()) {
        return false;
    }
    if (std::filesystem::exists(DataDir() + "/vectors.bin")) {
        return true;
    }
#if defined(_WIN32)
    WARN_MESSAGE(false, "skipped: no vectors on Windows (the harness that makes them is for Linux and macOS)");
#else
    if (Required()) {
        FAIL("vectors.bin was not built, and MH4N_REQUIRE_TEST_DATA=1");
    }
    WARN_MESSAGE(false, "skipped: vectors.bin was not built");
#endif
    return false;
}

std::string Name(mh4n::u32 route) {
    REQUIRE(route < ih_route_count);
    return ih_route_names[route];
}
std::string Arm(mh4n::u32 word) { return Name(ih_classify_arm(word)); }
std::string Thumb(mh4n::u32 halfword, mh4n::u32 second = 0) {
    return Name(ih_classify_thumb(halfword | (second << 16)));
}

// Collects the log while it lives (the failures under test log at Critical) and puts the default sink back.
struct QuietLog {
    std::vector<std::string> messages;
    explicit QuietLog(LogLevel level = LogLevel::Info) {
        SetLogFilter(LogFilter(level));
        SetLogSink([this](const LogRecord& r) {
            messages.push_back(fmt::format("[{}] {}: {}", LogLevelName(r.level), r.category, r.message));
        });
    }
    ~QuietLog() {
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    bool Has(const std::string& text) const {
        for (const std::string& m : messages) {
            if (m.find(text) != std::string::npos) return true;
        }
        return false;
    }
};

// A code host with no code library (so every call is to a missing function) and the interpreter installed,
// guest memory with code, a stack and a data page, and a fake kernel that records system calls.
struct Machine {
    QuietLog log;
    volatile std::uint32_t preempt_flag = 0;
    std::unique_ptr<GuestMemory> memory;
    std::unique_ptr<CodeHost> host;
    std::unique_ptr<Interpreter> interp;
    std::unique_ptr<GuestThread> thread;
    std::function<void(RCtx&, mh4n::u32, mh4n::u32)> on_svc;
    std::function<void(RCtx&)> on_budget;
    std::vector<std::pair<mh4n::u32, mh4n::u32>> svcs;  // imm, next_pc
    int budget_calls = 0;
    bool finished = false;

    Machine() {
        memory = GuestMemory::Create();
        REQUIRE(memory != nullptr);
        REQUIRE(IsSuccess(memory->Map(kCode, 0xA0000, MemPerm::ReadWriteExecute, MemState::Code)));
        REQUIRE(IsSuccess(memory->Map(kStack - 0x10000, 0x20000, MemPerm::ReadWrite, MemState::Private)));
        host = std::make_unique<CodeHost>(*memory);
        host->SetSvcHandler([this](RCtx& ctx, mh4n::u32 imm, mh4n::u32 next_pc) {
            svcs.emplace_back(imm, next_pc);
            if (on_svc) {
                on_svc(ctx, imm, next_pc);
            }
        });
        host->SetBudgetHandler([this](RCtx& ctx) {
            ++budget_calls;
            preempt_flag = 0;
            if (on_budget) {
                on_budget(ctx);
            }
            thread->Yield();  // what the scheduler does when it preempts
        });
        interp = std::make_unique<Interpreter>(*host, &preempt_flag, "");
        interp->Install();
    }

    void Write32(mh4n::u32 address, const std::vector<mh4n::u32>& words) {
        std::memcpy(memory->Base() + address, words.data(), words.size() * 4);
    }
    void Write16(mh4n::u32 address, const std::vector<std::uint16_t>& halfwords) {
        std::memcpy(memory->Base() + address, halfwords.data(), halfwords.size() * 2);
    }
    mh4n::u32 Read32(mh4n::u32 address) {
        mh4n::u32 v;
        std::memcpy(&v, memory->Base() + address, 4);
        return v;
    }
    RCtx& Ctx() { return thread->Context(); }

    // Calls `target` (Thumb bit set as in the function table) from a guest thread with LR = `lr`; `setup`
    // may set registers first. Enters the thread until the call is over or the host failed.
    void Call(mh4n::u32 target, mh4n::u32 lr, std::function<void(RCtx&)> setup = {}) {
        thread = host->NewThread(1, [this, target, lr, setup](GuestThread& t) {
            RCtx& ctx = t.Context();
            ctx.r[13] = kStack;
            ctx.r[14] = lr;
            if (setup) {
                setup(ctx);
            }
            host->Call(t, target);
            finished = true;
            t.Yield();
        });
        for (int i = 0; i < 1000 && !finished && !host->Failure(); ++i) {
            host->Enter(*thread);
        }
    }
};

}  // namespace

// ---- the dispatcher -------------------------------------------------------------------------------

TEST_CASE("the dispatcher names the form of known words") {
    CHECK(Arm(0xE3A00001) == "arm_dp_imm");        // mov r0, #1
    CHECK(Arm(0xE1A00001) == "arm_dp_imm_shift");  // mov r0, r1
    CHECK(Arm(0xE12FFF1E) == "arm_bx_blx");        // bx lr
    CHECK(Arm(0xE5912000) == "arm_ldst_imm");      // ldr r2, [r1]
    CHECK(Arm(0xE591F000) == "arm_ldrpc");         // ldr pc, [r1]
    CHECK(Arm(0xE8BD8010) == "arm_ldm_stm");       // pop {r4, pc}
    CHECK(Arm(0xEB000010) == "arm_b_bl");
    CHECK(Arm(0xEF00003D) == "arm_svc");
    CHECK(Arm(0xE7F000F0) == "arm_udf");
    CHECK(Arm(0xE1200070) == "arm_bkpt");
    CHECK(Arm(0xE16F0F11) == "arm_clz");           // clz r0, r1
    CHECK(Arm(0xE320F000) == "arm_hint");          // nop
    CHECK(Arm(0xEE300A01) == "vfp_arith");         // vadd.f32 s0, s0, s2
    CHECK(Arm(0xED900A00) == "vfp_ldst");          // vldr s0, [r0]
    CHECK(Arm(0xEC900A02) == "arm_vldm");          // vldmia r0, {s0, s1}
    CHECK(Arm(0xF57FF01F) == "arm_clrex");
    CHECK(Arm(0xFA000000) == "arm_blx_imm");
    // no form takes a coprocessor that does not exist, nor an ARM word with the never-condition that is
    // not one of the unconditional instructions
    CHECK(Arm(0xEE000000) == "unsupported");
    CHECK(Arm(0xF1000000) == "unsupported");

    CHECK(Thumb(0x4770) == "t_hireg");             // bx lr
    CHECK(Thumb(0x2007) == "t_imm8");              // movs r0, #7
    CHECK(Thumb(0xB500) == "t_push_pop");
    CHECK(Thumb(0xDF01) == "t_svc");
    CHECK(Thumb(0xDE00) == "t_udf");
    CHECK(Thumb(0xBE00) == "t_bkpt");
    CHECK(Thumb(0xE000) == "t_b");
    CHECK(Thumb(0xF000, 0xF800) == "t_bl");
    CHECK(Thumb(0xF000, 0xE800) == "t_blx_imm");
    // a BL prefix without its suffix, a suffix on its own, and the BLX suffix with bit 0 set
    CHECK(Thumb(0xF000, 0x0000) == "unsupported_thumb");
    CHECK(Thumb(0xF800) == "unsupported_thumb");
    CHECK(Thumb(0xE800) == "unsupported_thumb");
    CHECK(Thumb(0xF000, 0xE801) == "unsupported_thumb");
}

TEST_CASE("UNPREDICTABLE encodings are told apart") {
    CHECK(ih_unpredictable_arm(0xE0010092) == 0);  // mul r1, r2, r0
    CHECK(ih_unpredictable_arm(0xE0013092) == 1);  // the same with bits that should be zero set
    CHECK(ih_unpredictable_arm(0xE0010F92) == 1);  // the same with pc as the multiplier
    CHECK(ih_unpredictable_arm(0xE12FFF1F) == 1);  // bx pc
    CHECK(ih_unpredictable_arm(0xE1A0F00F) == 0);  // mov pc, pc
    CHECK(ih_unpredictable_arm(0xE8900000) == 1);  // ldm r0, {} (empty list)
    CHECK(ih_unpredictable_arm(0xF57FF01F) == 0);  // unconditional space
    CHECK(ih_unpredictable_thumb(0xB400) == 1);    // push {} (empty list)
    CHECK(ih_unpredictable_thumb(0xB401) == 0);
    CHECK(ih_unpredictable_thumb(0x4408) == 1);    // add r0, r1 in the hi-register form: two low registers
    CHECK(ih_unpredictable_thumb(0x4487) == 0);    // add pc, r0
}

// ---- interpreted calls ------------------------------------------------------------------------------

TEST_CASE("an interpreted function returns to LR, and its entry is recorded") {
    Machine m;
    m.Write32(kCode, {0xE3A00005,    // mov r0, #5
                      0xE2800001,    // add r0, r0, #1
                      0xE12FFF1E});  // bx lr
    m.Call(kCode, kReturn);
    REQUIRE(m.finished);
    CHECK(m.Ctx().r[0] == 6);
    CHECK(m.Ctx().r[15] == kReturn);  // the target a recompiled caller expects is left in r15
    CHECK(m.interp->InstructionCount() == 3);
    CHECK(m.interp->Entries() == std::vector<mh4n::u32>{kCode});
    // logged once, at the first entry
    CHECK(m.log.Has("[Warning] Cpu: interpreting 00100000 (arm), which is not recompiled code (lr 00180000)"));
    m.finished = false;
    m.Call(kCode, kReturn);
    CHECK(m.interp->Entries().size() == 1);
    int logged = 0;
    for (const std::string& line : m.log.messages) logged += line.find("interpreting") != std::string::npos;
    CHECK(logged == 1);
}

TEST_CASE("calls made by interpreted code are interpreted, and the stack comes back") {
    Machine m;
    m.Write32(kCode, {0xE92D4010,                                  // push {r4, lr}
                      0xE3A04003,                                  // mov r4, #3
                      0xEB000002,                                  // bl kCode + 0x18
                      0xE0800004,                                  // add r0, r0, r4
                      0xE8BD8010,                                  // pop {r4, pc}
                      0xE1A00000,                                  // nop
                      0xE3A0000A,                                  // mov r0, #10
                      0xE12FFF1E});                                // bx lr
    m.Call(kCode, kReturn, [](RCtx& ctx) { ctx.r[4] = 0x1234; });
    REQUIRE(m.finished);
    CHECK(m.Ctx().r[0] == 13);
    CHECK(m.Ctx().r[4] == 0x1234);  // pop restored it
    CHECK(m.Ctx().r[13] == kStack);
    CHECK(m.Ctx().r[15] == kReturn);
}

TEST_CASE("Thumb entries, Thumb returns, and the exchange between the states") {
    Machine m;
    m.Write16(kCode, {0x2007,    // movs r0, #7
                      0x3001,    // adds r0, #1
                      0x4770});  // bx lr
    // called as a Thumb function, back to ARM code
    m.Call(kCode | 1, kReturn);
    REQUIRE(m.finished);
    CHECK(m.Ctx().r[0] == 8);
    CHECK(m.Ctx().r[15] == kReturn);

    // back to Thumb code: LR has bit 0 set and so does the target left in r15
    Machine t;
    t.Write16(kCode, {0x2007, 0x3001, 0x4770});
    t.Call(kCode | 1, kReturn | 1);
    REQUIRE(t.finished);
    CHECK(t.Ctx().r[15] == (kReturn | 1));

    // a Thumb BL pair to a Thumb function, then back
    Machine b;
    // 0: push {lr}; 2: bl (the pair at 2..5 reaches 0xA); 6: pop {pc}; 8: nop; 0xA: movs r1, #2; 0xC: bx lr
    b.Write16(kCode, {0xB500, 0xF000, 0xF802, 0xBD00, 0x46C0, 0x2102, 0x4770});
    b.Call(kCode | 1, kReturn | 1);
    REQUIRE(b.finished);
    CHECK(b.Ctx().r[1] == 2);
    CHECK(b.Ctx().r[15] == (kReturn | 1));
}

TEST_CASE("a function entered by a tail call comes back to the caller's own return address") {
    Machine m;
    m.Write32(kCode, {0xE3A00001,    // mov r0, #1
                      0xE12FFF11});  // bx r1
    constexpr mh4n::u32 kCallersReturn = 0x00190000;
    m.Call(kCode, 0x00ABCDE0 /* LR is something else by now */, [](RCtx& ctx) {
        ctx.r[1] = kCallersReturn;
        ctx.expect_next = kCallersReturn;
    });
    REQUIRE(m.finished);
    CHECK(m.Ctx().r[15] == kCallersReturn);
    CHECK(m.Ctx().expect_next == 0);  // consumed
}

TEST_CASE("reaching the return address with the stack pointer below where it was is not a return") {
    Machine m;
    // The function drops sp by 8 and returns; the code at the return address (interpreted too, since the
    // call has not ended) gives the 8 bytes back and jumps to the return address again: now it is a return.
    m.Write32(kCode, {0xE24DD008,    // sub sp, sp, #8
                      0xE12FFF1E});  // bx lr
    m.Write32(kReturn, {0xE28DD008,    // add sp, sp, #8
                        0xE1A0F00E});  // mov pc, lr
    m.Call(kCode, kReturn);
    REQUIRE(m.finished);
    CHECK(m.interp->InstructionCount() == 4);
    CHECK(m.Ctx().r[13] == kStack);
    CHECK(m.Ctx().r[15] == kReturn);
}

TEST_CASE("a system call from interpreted code reaches the kernel, which may change registers") {
    Machine m;
    m.on_svc = [](RCtx& ctx, mh4n::u32, mh4n::u32) { ctx.r[0] = 42; };
    m.Write32(kCode, {0xE3A00001,    // mov r0, #1
                      0xEF000005,    // svc #5
                      0xE1A01000,    // mov r1, r0
                      0xE12FFF1E});
    m.Call(kCode, kReturn);
    REQUIRE(m.finished);
    REQUIRE(m.svcs.size() == 1);
    CHECK(m.svcs[0].first == 5);
    CHECK(m.svcs[0].second == kCode + 8);  // the address after the SVC
    CHECK(m.Ctx().r[1] == 42);

    Machine t;
    t.on_svc = [](RCtx& ctx, mh4n::u32, mh4n::u32) { ctx.r[0] = 9; };
    t.Write16(kCode, {0x2001,    // movs r0, #1
                      0xDF07,    // svc 7
                      0x0001,    // movs r1, r0
                      0x4770});
    t.Call(kCode | 1, kReturn);
    REQUIRE(t.finished);
    REQUIRE(t.svcs.size() == 1);
    CHECK(t.svcs[0].first == 7);
    CHECK(t.svcs[0].second == ((kCode + 4) | 1));  // Thumb code: the next address has bit 0 set
    CHECK(t.Ctx().r[1] == 9);
}

TEST_CASE("an UNPREDICTABLE encoding runs as the nearest defined form and is recorded once") {
    Machine m;
    m.Write32(kCode, {0xE3A03003,    // mov r3, #3
                      0xE0013092,    // L: mul r1, r2, r0 with bits that should be zero set
                      0xE2533001,    // subs r3, r3, #1
                      0x1AFFFFFC,    // bne L
                      0xE12FFF1E});
    m.Call(kCode, kReturn, [](RCtx& ctx) {
        ctx.r[0] = 6;
        ctx.r[2] = 7;
    });
    REQUIRE(m.finished);
    CHECK(m.Ctx().r[1] == 42);
    CHECK(m.interp->UnpredictableAddresses() == std::vector<mh4n::u32>{kCode + 4});
    int logged = 0;
    for (const std::string& line : m.log.messages) logged += line.find("UNPREDICTABLE ARM encoding E0013092 at 00100004") != std::string::npos;
    CHECK(logged == 1);
}

TEST_CASE("an unsupported instruction fails the thread as the recompiled code does") {
    Machine m;
    m.Write32(kCode, {0xE3A00001,
                      0xEE000000});  // cdp p0: no such coprocessor
    m.Call(kCode, kReturn);
    CHECK_FALSE(m.finished);
    REQUIRE(m.host->Failure() != nullptr);
    CHECK(m.host->Failure()->message == "unsupported instruction at 00100004");
    CHECK(m.Ctx().r[0] == 1);

    // an undefined instruction and a breakpoint have their own messages
    Machine u;
    u.Write32(kCode, {0xE7F000F0});
    u.Call(kCode, kReturn);
    REQUIRE(u.host->Failure() != nullptr);
    CHECK(u.host->Failure()->message == "undefined instruction at 00100000");
    Machine b;
    b.Write16(kCode, {0xBE00});
    b.Call(kCode | 1, kReturn);
    REQUIRE(b.host->Failure() != nullptr);
    CHECK(b.host->Failure()->message == "breakpoint at 00100000");

    // a failed condition skips the trap, as in the recompiled code
    Machine c;
    c.Write32(kCode, {0x1E000000,    // cdpne p0 with Z set: skipped
                      0xE12FFF1E});
    c.Call(kCode, kReturn, [](RCtx& ctx) { ctx.fz = 1; });
    CHECK(c.finished);
    CHECK(c.host->Failure() == nullptr);
}

TEST_CASE("preemption: the interpreter yields when the flag is raised and goes on where it was") {
    Machine m;
    m.on_svc = [&m](RCtx&, mh4n::u32, mh4n::u32) { m.preempt_flag = 1; };
    m.Write32(kCode, {0xE3A00000,    // mov r0, #0
                      0xEF000001,    // svc #1: raises the flag
                      0xE2800001,    // add r0, r0, #1
                      0xE2800001,
                      0xE12FFF1E});
    m.Call(kCode, kReturn);
    REQUIRE(m.finished);
    CHECK(m.budget_calls == 1);
    CHECK(m.Ctx().r[0] == 2);
    CHECK(m.Ctx().r[15] == kReturn);
}

// A context switch clears the exclusive monitor (the kernel does it on hardware). The monitor lives in each
// thread's own context, so the thread that was displaced between LDREX and STREX must not store: the thread
// that ran meanwhile has changed the word. (Found by the Windows CI: the whole of svc_test interpreted lost
// increments of its LDREX/STREX counter when the preemption flag, raised by the timer thread, was seen between
// the two.)
TEST_CASE("a preemption between LDREX and STREX makes the STREX fail") {
    constexpr mh4n::u32 kWord = kStack - 0x100;
    const std::vector<mh4n::u32> increment = {0xE1921F9F,    // ldrex r1, [r2]
                                              0xE2811001,    // add r1, r1, #1
                                              0xE1823F91,    // strex r3, r1, [r2]
                                              0xE12FFF1E};   // bx lr
    SUBCASE("nothing runs in between: the store succeeds") {
        Machine m;
        m.Write32(kCode, increment);
        m.Write32(kWord, {5});
        m.Call(kCode, kReturn, [](RCtx& ctx) { ctx.r[2] = kWord; });
        REQUIRE(m.finished);
        CHECK(m.budget_calls == 0);
        CHECK(m.Ctx().r[3] == 0);
        CHECK(m.Read32(kWord) == 6);
    }
    SUBCASE("another thread changed the word while this one was displaced: the store fails") {
        Machine m;
        m.on_budget = [&m](RCtx&) { m.Write32(kWord, {100}); };  // what the thread that ran meanwhile did
        m.Write32(kCode, increment);
        m.Write32(kWord, {5});
        m.Call(kCode, kReturn, [&m](RCtx& ctx) {
            ctx.r[2] = kWord;
            m.preempt_flag = 1;  // seen after the LDREX
        });
        REQUIRE(m.finished);
        CHECK(m.budget_calls == 1);
        CHECK(m.Ctx().r[3] == 1);
        CHECK(m.Read32(kWord) == 100);
    }
    SUBCASE("the same through a system call that lets other threads run") {
        Machine m;
        m.on_svc = [&m](RCtx&, mh4n::u32, mh4n::u32) { m.Write32(kWord, {100}); };
        m.Write32(kCode, {0xE1921F9F,    // ldrex r1, [r2]
                          0xEF000001,    // svc #1
                          0xE2811001,    // add r1, r1, #1
                          0xE1823F91,    // strex r3, r1, [r2]
                          0xE12FFF1E});
        m.Write32(kWord, {5});
        m.Call(kCode, kReturn, [](RCtx& ctx) { ctx.r[2] = kWord; });
        REQUIRE(m.finished);
        CHECK(m.Ctx().r[3] == 1);
        CHECK(m.Read32(kWord) == 100);
    }
}

TEST_CASE("a deterministic library's budget is charged one tick per instruction") {
    Machine m;
    m.interp->SetChargeBudget(true);
    m.on_budget = [&m](RCtx& ctx) { ctx.budget += 4; (void)m; };
    m.Write32(kCode, {0xE3A00000, 0xE2800001, 0xE2800001, 0xE2800001, 0xE2800001, 0xE2800001, 0xE2800001,
                      0xE2800001, 0xE12FFF1E});
    m.Call(kCode, kReturn, [](RCtx& ctx) { ctx.budget = 2; });
    REQUIRE(m.finished);
    // 9 instructions, the first two paid for by the budget of 2; the budget callback gives 4 more each time
    CHECK(m.Ctx().r[0] == 7);
    CHECK(m.budget_calls >= 2);
    CHECK(m.interp->InstructionCount() == 9);
}

// ---- the instruction fetch --------------------------------------------------------------------------

TEST_CASE("ih_step fetches ARM and Thumb words from guest memory and leaves the next address") {
    Machine m;
    m.Write32(kCode, {0xE3A0002A});   // mov r0, #42
    m.Write16(kCode + 0x10, {0x2105, 0xF000, 0xF800});  // movs r1, #5 / the first half of a BL
    m.thread = m.host->NewThread(1, [&](GuestThread& t) {
        RCtx& ctx = t.Context();
        ctx.r[15] = kCode;
        CHECK(ih_step(&ctx) == 0);
        CHECK(ctx.r[0] == 42);
        CHECK(ctx.r[15] == kCode + 4);
        ctx.r[15] = (kCode + 0x10) | 1;
        CHECK(ih_step(&ctx) == 0);
        CHECK(ctx.r[1] == 5);
        CHECK(ctx.r[15] == ((kCode + 0x12) | 1));
        ih_step(&ctx);  // the BL pair: both halfwords are read, the instruction is 4 bytes
        CHECK(ctx.r[15] == (((kCode + 0x12) + 4 + 0) | 1));
        CHECK(ctx.r[14] == (((kCode + 0x12) + 4) | 1));
        m.finished = true;
        t.Yield();
    });
    m.host->Enter(*m.thread);
    CHECK(m.finished);
}

// ---- vectors: random states against the recompiled C ---------------------------------------------------

namespace {

struct Reader {
    std::vector<std::uint8_t> bytes;
    std::size_t pos = 0;
    mh4n::u32 U32() {
        REQUIRE(pos + 4 <= bytes.size());
        mh4n::u32 v;
        std::memcpy(&v, bytes.data() + pos, 4);
        pos += 4;
        return v;
    }
};

constexpr mh4n::u32 kBuffer = 0x30000000;
constexpr mh4n::u32 kBufferSize = 0x10000;
constexpr mh4n::u32 kWindow = 0x6000;
constexpr mh4n::u32 kTlsPage = 0x1FF80000;

void FillBuffer(std::uint8_t* base, mh4n::u32 seed) {
    std::memset(base, 0, kBufferSize);
    mh4n::u32 x = seed;
    for (mh4n::u32 i = 0; i < 4096; ++i) {
        x = x * 1664525u + 1013904223u;
        std::memcpy(base + kWindow + 4 * i, &x, 4);
    }
}

}  // namespace

TEST_CASE("every form, run from random states, gives what the recompiled code gives") {
    if (!HaveVectors()) {
        return;
    }
    QuietLog log;
    Reader in;
    {
        std::ifstream f(DataDir() + "/vectors.bin", std::ios::binary);
        REQUIRE(f.good());
        in.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    REQUIRE(in.bytes.size() > 8);
    REQUIRE(std::memcmp(in.bytes.data(), "IHV1", 4) == 0);
    in.pos = 4;
    auto memory = GuestMemory::Create();
    REQUIRE(memory != nullptr);
    const mh4n::u32 images = in.U32();
    for (mh4n::u32 i = 0; i < images; ++i) {
        const mh4n::u32 base = in.U32(), size = in.U32();
        // the same rounding as the Python harness's regions: a fault in one must be a fault in the other
        REQUIRE(IsSuccess(memory->Map(base, (size + 0x1FFF) & ~0xFFFu, MemPerm::ReadWriteExecute, MemState::Code)));
        REQUIRE(in.pos + size <= in.bytes.size());
        std::memcpy(memory->Base() + base, in.bytes.data() + in.pos, size);
        in.pos += size;
    }
    REQUIRE(IsSuccess(memory->Map(kBuffer, kBufferSize, MemPerm::ReadWrite, MemState::Private)));
    REQUIRE(IsSuccess(memory->Map(kTlsPage, 0x1000, MemPerm::ReadWrite, MemState::Private)));

    struct Vector {
        mh4n::u32 image, address, thumb, word, kind;
        std::array<mh4n::u32, 15> r;
        std::array<mh4n::u32, 6> flags;
        std::array<mh4n::u32, 32> s;
        mh4n::u32 fpscr, tls, seed;
        std::array<mh4n::u32, 16> out_r;
        std::array<mh4n::u32, 6> out_flags;
        std::array<mh4n::u32, 32> out_s;
        mh4n::u32 out_fpscr = 0, out_crc = 0, info0 = 0, info1 = 0;
    };
    std::vector<Vector> vectors(in.U32());
    for (Vector& v : vectors) {
        v.image = in.U32();
        v.address = in.U32();
        v.thumb = in.U32();
        v.word = in.U32();
        v.kind = in.U32();
        for (auto& x : v.r) x = in.U32();
        for (auto& x : v.flags) x = in.U32();
        for (auto& x : v.s) x = in.U32();
        v.fpscr = in.U32();
        v.tls = in.U32();
        v.seed = in.U32();
        if (v.kind == 0) {
            for (auto& x : v.out_r) x = in.U32();
            for (auto& x : v.out_flags) x = in.U32();
            for (auto& x : v.out_s) x = in.U32();
            v.out_fpscr = in.U32();
            v.out_crc = in.U32();
        } else {
            v.info0 = in.U32();
            v.info1 = in.U32();
        }
    }
    REQUIRE(in.pos == in.bytes.size());
    REQUIRE(vectors.size() > 1000);

    auto set_state = [&](RCtx& ctx, const Vector& v) {
        for (int i = 0; i < 15; ++i) ctx.r[i] = v.r[static_cast<std::size_t>(i)];
        ctx.fn = v.flags[0];
        ctx.fz = v.flags[1];
        ctx.fc = v.flags[2];
        ctx.fv = v.flags[3];
        ctx.fq = v.flags[4];
        ctx.ge = v.flags[5];
        for (int i = 0; i < 32; ++i) ctx.f.sw[i] = v.s[static_cast<std::size_t>(i)];
        ctx.fpscr = v.fpscr;
        ctx.tpidruro = v.tls;
        ctx.excl_valid = 0;
        ctx.r[15] = v.address | v.thumb;
        FillBuffer(memory->Base() + kBuffer, v.seed);
    };

    // Returns and system calls: all of them in one guest thread. A trap ends a thread, so each of those
    // gets a thread of its own below.
    CodeHost host(*memory);
    std::vector<std::pair<mh4n::u32, mh4n::u32>> svcs;
    host.SetSvcHandler([&](RCtx&, mh4n::u32 imm, mh4n::u32 next_pc) { svcs.emplace_back(imm, next_pc); });
    std::size_t checked = 0, svc_checked = 0, traps = 0;
    std::vector<std::string> problems;
    auto problem = [&](std::size_t i, const std::string& what) {
        if (problems.size() < 20) {
            problems.push_back(fmt::format("vector {} (word {:08X} at {:08X}{}): {}", i, vectors[i].word,
                                           vectors[i].address, vectors[i].thumb ? ", Thumb" : "", what));
        }
    };
    auto thread = host.NewThread(1, [&](GuestThread& t) {
        RCtx& ctx = t.Context();
        for (std::size_t i = 0; i < vectors.size(); ++i) {
            const Vector& v = vectors[i];
            if (v.kind == 2) {
                continue;
            }
            set_state(ctx, v);
            svcs.clear();
            rc_apply_rounding(ctx.fpscr);
            ih_step(&ctx);
            rc_apply_rounding(0);
            if (v.kind == 1) {
                ++svc_checked;
                // the Python harness reports the address after the SVC without the Thumb bit
                if (svcs.size() != 1 || svcs[0].first != v.info0 || (svcs[0].second & ~1u) != v.info1 ||
                    (svcs[0].second & 1u) != v.thumb) {
                    problem(i, "system call differs");
                }
                continue;
            }
            ++checked;
            std::string diff;
            for (int r = 0; r < 16; ++r) {
                if (ctx.r[r] != v.out_r[static_cast<std::size_t>(r)]) {
                    diff += fmt::format(" r{} {:08X} (want {:08X});", r, ctx.r[r], v.out_r[static_cast<std::size_t>(r)]);
                }
            }
            const mh4n::u32 flags[6] = {ctx.fn, ctx.fz, ctx.fc, ctx.fv, ctx.fq, ctx.ge};
            for (int k = 0; k < 6; ++k) {
                if (flags[k] != v.out_flags[static_cast<std::size_t>(k)]) diff += fmt::format(" flag {};", k);
            }
            for (int k = 0; k < 32; ++k) {
                if (ctx.f.sw[k] != v.out_s[static_cast<std::size_t>(k)]) {
                    diff += fmt::format(" s{} {:08X} (want {:08X});", k, ctx.f.sw[k], v.out_s[static_cast<std::size_t>(k)]);
                }
            }
            if ((ctx.fpscr ^ v.out_fpscr) & ~0x9Fu) diff += " fpscr;";
            if (Crc32(memory->Base() + kBuffer, kBufferSize) != v.out_crc) diff += " memory;";
            if (!diff.empty()) {
                problem(i, diff);
            }
        }
        t.Yield();
    });
    host.Enter(*thread);
    for (const std::string& p : problems) {
        MESSAGE(p);
    }
    CHECK(problems.empty());
    CHECK(checked > 1000);
    CHECK(svc_checked > 10);

    // each trap on a code host of its own
    for (std::size_t i = 0; i < vectors.size() && problems.empty(); ++i) {
        const Vector& v = vectors[i];
        if (v.kind != 2) {
            continue;
        }
        CodeHost trap_host(*memory);
        auto trap_thread = trap_host.NewThread(1, [&](GuestThread& t) {
            set_state(t.Context(), v);
            ih_step(&t.Context());
            t.Yield();   // reached when a failed condition skipped the trap
        });
        trap_host.Enter(*trap_thread);
        // the recompiled code stopped with (pc, code): the host's message names both
        const char* kinds[] = {"trap", "undefined instruction", "unsupported instruction", "breakpoint",
                               "jump to an invalid address"};
        REQUIRE(trap_host.Failure() != nullptr);
        CHECK(trap_host.Failure()->message == fmt::format("{} at {:08X}", kinds[v.info1], v.info0));
        ++traps;
    }
    CHECK(traps > 10);
}

// ---- svc_test, the 3DS test program, through the native runtime --------------------------------------

namespace {

struct ProgramRun {
    int status = -1;
    std::vector<std::string> lines;
};

ProgramRun RunProgram(const std::string& library, const std::string& missing_path) {
    const std::string dir = DataDir();
    ProgramRun run;
    QuietLog log(LogLevel::Error);
    {
        Runtime runtime;
        SvcHooks hooks;
        hooks.debug_string = [&](std::string_view text) { run.lines.emplace_back(text); };
        runtime.AddSvcHooks(std::move(hooks));
        const std::string user = (std::filesystem::temp_directory_path() / "mh4n-interp-test-user").string();
        const Options options = ParseOptions(
            "runner", {dir + "/svc_test.elf", "--code", dir + "/" + library, "--ms", "5000", "--user", user,
                       "--missing-entries", missing_path});
        REQUIRE_MESSAGE(options.ok(), options.error());
        run.status = runtime.Boot(options);
    }
    return run;
}

std::vector<std::string> ExpectedLines() {
#ifdef MH4N_INTERP_EXPECTED_DIR
    std::ifstream f(std::string(MH4N_INTERP_EXPECTED_DIR) + "/svc_test.txt");
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) {
        lines.push_back(line);
    }
    return lines;
#else
    return {};
#endif
}

std::vector<std::string> EntryLines(const std::string& path) {
    std::ifstream f(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) {
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

TEST_CASE("svc_test gives Azahar's output with its worker functions forced into the interpreter") {
    if (!HaveData()) {
        return;
    }
    const std::vector<std::string> expected = ExpectedLines();
    REQUIRE(expected.size() > 10);
    const auto scratch = std::filesystem::temp_directory_path() / "mh4n-interp-test";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    const std::string none_path = (scratch / "none.txt").string();
    const ProgramRun recompiled = RunProgram("svc_all.so", none_path);
    CHECK(recompiled.status == 0);
    CHECK(recompiled.lines == expected);
    CHECK_FALSE(std::filesystem::exists(none_path));  // nothing was interpreted

    const std::string workers_path = (scratch / "workers.txt").string();
    const ProgramRun workers = RunProgram("svc_workers.so", workers_path);
    CHECK(workers.status == 0);
    CHECK(workers.lines == expected);
    const std::vector<std::string> worker_entries = EntryLines(workers_path);
    CHECK(worker_entries.size() >= 1);
    for (const std::string& line : worker_entries) {
        // 0x<address>, the mode and why, separated by tabs, as config/extra_entries.txt has them
        CHECK(line.size() == 10 + 1 + 3 + 1 + 11);
        CHECK(line.substr(0, 2) == "0x");
        CHECK(line.substr(10) == "\tarm\tinterpreted");
    }

    // a second run adds nothing: the entries are already in the file
    RunProgram("svc_workers.so", workers_path);
    CHECK(EntryLines(workers_path) == worker_entries);

    // no function at all: the whole program, from its entry point, runs in the interpreter
    const std::string all_path = (scratch / "all.txt").string();
    const ProgramRun interpreted = RunProgram("svc_none.so", all_path);
    CHECK(interpreted.status == 0);
    CHECK(interpreted.lines == expected);
    const std::vector<std::string> all_entries = EntryLines(all_path);
    CHECK(all_entries.size() >= 2);  // the program's entry point and the worker threads' function
    CHECK(all_entries[0] == "0x00100000\tarm\tinterpreted");
    std::filesystem::remove_all(scratch);
}

TEST_CASE("an empty --missing-entries writes no file") {
    if (!HaveData()) {
        return;
    }
    const auto scratch = std::filesystem::temp_directory_path() / "mh4n-interp-test2";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    const ProgramRun run = RunProgram("svc_none.so", "");
    CHECK(run.status == 0);
    CHECK(run.lines == ExpectedLines());
    std::filesystem::remove_all(scratch);
}

#endif  // recomp.h
