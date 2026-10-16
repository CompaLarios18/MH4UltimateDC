// mh4n_cpu (N1-04): the code host. Guest threads on fibers with their own floating-point state, the
// callbacks of recomp.h, the code library's checks, and the test program port/tests/codehost_test.c
// running through the loader and guest memory with a fake SVC handler.
//
// The cases that run the program need the code libraries that native/src/cpu/CMakeLists.txt builds
// into MH4N_CPU_TEST_DATA_DIR (Python 3 and the clang tools); without them those cases say so and
// pass nothing. Assertions stay out of the fibers: a fiber records what it saw and the test
// checks it afterwards.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
// A copy of native/ on its own (see native/src/cpu/CMakeLists.txt) builds no code host.
TEST_CASE("the code host is not built without the repository's fibers and recomp.h") {}
#else

#include <cfenv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include <fmt/format.h>

#include "../src/cpu/callbacks.h"
#include "mh4n/cpu.h"
#include "mh4n/crc32.h"
#include "mh4n/loader.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

namespace {

using mh4n::CodeHost;
using mh4n::CodeHostCallbacks;
using mh4n::CodeLibrary;
using mh4n::GuestMemory;
using mh4n::GuestThread;
using mh4n::Loader;

// ---- the host's own expectations ---------------------------------------------------------------

std::uint32_t ThumbMix(std::uint32_t a, std::uint32_t b) {
    for (int i = 0; i < 16; i++) {
        a = (a << 3 | a >> 29) ^ (b + static_cast<std::uint32_t>(i));
    }
    return a;
}

constexpr std::uint32_t kFpscrNearest = 0x03000000;   // default NaN, flush to zero
constexpr std::uint32_t kFpscrUp = 0x03400000;
constexpr std::uint32_t kFpscrDown = 0x03800000;
constexpr std::uint32_t kFpscrZero = 0x03C00000;      // new 3DS threads start here

std::uint32_t Bits(float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

// The dot product of codehost_test.c, one rounding per operation, under the rounding mode and
// flush-to-zero that `fpscr` selects. Volatile temporaries keep the compiler from fusing or
// folding anything.
float Dot4(std::uint32_t fpscr, const float (&a_in)[4], const float (&b_in)[4]) {
    volatile float a[4] = {a_in[0], a_in[1], a_in[2], a_in[3]};  // not constants: no folding
    volatile float b[4] = {b_in[0], b_in[1], b_in[2], b_in[3]};
    rc_apply_rounding(fpscr);
    volatile float product = a[0] * b[0];
    volatile float sum = product;
    for (int i = 1; i < 4; i++) {
        product = a[i] * b[i];
        sum = sum + product;
    }
    const float result = sum;
    rc_apply_rounding(0);
    return result;
}

// 1/3 is rounded up or down by the mode.
std::uint32_t ThirdBits() {
    volatile float one = 1.0f;
    volatile float three = 3.0f;
    volatile float third = one / three;
    return Bits(third);
}

std::uint32_t ThirdUnder(std::uint32_t fpscr) {
    rc_apply_rounding(fpscr);
    const std::uint32_t bits = ThirdBits();
    rc_apply_rounding(0);
    return bits;
}

bool HostFpuIsDefault() {
    bool ok = std::fegetround() == FE_TONEAREST;
#if defined(__SSE2__) || defined(_M_X64)
    ok = ok && (_mm_getcsr() & 0x8040u) == 0;  // flush to zero and denormals are zero are off
#endif
    return ok;
}

struct Dots {
    std::uint32_t a, b;
};

Dots ExpectedDots(std::uint32_t fpscr) {
    const float a[4] = {0.1f, 0.7f, 1.3f, 3.1f}, b[4] = {1.9f, 2.3f, 0.3f, 0.7f};
    const float c[4] = {7.7f, 0.9f, 2.9f, 1.1f}, d[4] = {0.3f, 5.1f, 0.6f, 9.7f};
    return {Bits(Dot4(fpscr, a, b)), Bits(Dot4(fpscr, c, d))};
}

// ---- the test program --------------------------------------------------------------------------

std::string Join(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += "  " + line + "\n";
    }
    return text;
}

std::string DataDir() {
#ifdef MH4N_CPU_TEST_DATA_DIR
    const std::filesystem::path dir = MH4N_CPU_TEST_DATA_DIR;
    if (std::filesystem::exists(dir / "codehost_test.elf") &&
        std::filesystem::exists(dir / "codehost_native.so")) {
        return dir.string();
    }
#endif
    return {};
}

// With MH4N_REQUIRE_TEST_DATA=1 (CI sets it) a missing program is a failure, not a skip: the
// cases that need it are the done-when of N1-04, and a runner image that loses llvm-objcopy must
// not turn them into silent passes.
bool HaveProgram() {
    if (!DataDir().empty()) {
        return true;
    }
    const char* require = std::getenv("MH4N_REQUIRE_TEST_DATA");
    if (require != nullptr && std::string(require) == "1") {
        FAIL("the test program and its code libraries were not built, and MH4N_REQUIRE_TEST_DATA=1");
    }
    WARN_MESSAGE(false, "skipped: the test program and its code libraries were not built");
    return false;
}

// codehost_test.elf mapped into guest memory, a code host with the program's library, and a fake
// kernel: OutputDebugString (0x3D) and ExitProcess (0x03).
struct Program {
    volatile std::uint32_t preempt_flag = 0;
    std::unique_ptr<GuestMemory> memory;
    std::unique_ptr<Loader> loader;
    std::unique_ptr<mh4n::FcramRegions> regions;
    std::unique_ptr<CodeHost> host;
    std::unique_ptr<GuestThread> thread;
    std::string error;

    // What the fake kernel saw.
    std::vector<std::string> lines;
    std::vector<int> rounding_in_handlers;   // fegetround() at each SVC
    bool exited = false;
    int enters = 0;
    int budget_calls = 0;
    bool raise_preempt_flag = false;         // set the flag at each OutputDebugString
    bool context_matched = true;             // CurrentContext() is the thread's inside the handlers
    bool host_fpu_default_in_handlers = true;
    std::vector<std::uint32_t> missing_targets;
    bool handle_missing = false;

    explicit Program(const std::string& library = "codehost_native.so") {
        const std::string dir = DataDir();
        memory = GuestMemory::Create();
        REQUIRE(memory != nullptr);
        loader = Loader::OpenElf(dir + "/codehost_test.elf", &error);
        REQUIRE_MESSAGE(loader != nullptr, error);
        regions = std::make_unique<mh4n::FcramRegions>(loader->Image().memory_mode);
        REQUIRE_MESSAGE(loader->Map(*memory, *regions, &error), error);

        host = std::make_unique<CodeHost>(*memory);
        auto lib = CodeLibrary::Load(dir + "/" + library, false, &preempt_flag, &error);
        REQUIRE_MESSAGE(lib != nullptr, error);
        host->SetLibrary(std::move(lib));
        host->SetSvcHandler([this](RCtx& ctx, mh4n::u32 imm, mh4n::u32 next_pc) { OnSvc(ctx, imm, next_pc); });
        host->SetBudgetHandler([this](RCtx& ctx) {
            (void)ctx;
            budget_calls++;
            preempt_flag = 0;
            thread->Yield();  // what the scheduler does when it preempts
        });
    }

    void Start(std::uint32_t mode, std::uint32_t fpscr) {
        thread = host->NewThread(1);
        RCtx& ctx = thread->Context();
        ctx.r[0] = mode;
        ctx.r[13] = mh4n::kStackTop;
        ctx.r[15] = loader->Image().entry;
        ctx.fpscr = fpscr;
    }

    // Enters the thread until it exits or fails.
    void RunToEnd() {
        while (!exited && !host->Failure()) {
            host->Enter(*thread);
            enters++;
        }
    }

    void OnSvc(RCtx& ctx, mh4n::u32 imm, mh4n::u32 next_pc) {
        rounding_in_handlers.push_back(std::fegetround());
        host_fpu_default_in_handlers = host_fpu_default_in_handlers && HostFpuIsDefault();
        context_matched = context_matched && CodeHost::CurrentContext() == &ctx &&
                          CodeHost::CurrentThread() == thread.get();
        ctx.r[15] = next_pc & ~1u;
        if (imm == 0x3D) {  // OutputDebugString(r0 = text, r1 = length)
            std::string text(ctx.r[1], '\0');
            if (!memory->ReadBlock(text.data(), ctx.r[0], ctx.r[1])) {
                thread->Fail("OutputDebugString of unmapped memory");
            }
            lines.push_back(text);
            ctx.r[0] = 0;
            // A kernel that leaves its own floating-point mode behind: the host must put the
            // thread's back.
            std::fesetround(FE_UPWARD);
            if (raise_preempt_flag) {
                preempt_flag = 1;
            }
        } else if (imm == 0x03) {  // ExitProcess
            exited = true;
            thread->Yield();
        } else {
            thread->Fail(fmt::format("unexpected svc 0x{:02X}", imm));
        }
    }

    void HandleMissingFunctions() {
        host->SetMissingFunctionHandler([this](RCtx& ctx, mh4n::u32 target) {
            (void)ctx;
            missing_targets.push_back(target);
            context_matched = context_matched && HostFpuIsDefault();
        });
    }
};

}  // namespace

// ---- the code library --------------------------------------------------------------------------

TEST_CASE("a code library that cannot be used is refused with a message a player can act on") {
    std::string error;
    std::uint32_t flag = 0;

    CHECK(CodeLibrary::Load("/no/such/library.so", false, &flag, &error) == nullptr);
    CHECK(error.find("can't load /no/such/library.so") == 0);

    // A shared library that is not a code library.
#if defined(_WIN32)
    const char* other = "kernel32.dll";
#elif defined(__APPLE__)
    const char* other = "/usr/lib/libSystem.B.dylib";
#else
    const char* other = "libm.so.6";
#endif
    CHECK(CodeLibrary::Load(other, false, &flag, &error) == nullptr);
    CHECK_MESSAGE(error.find("is not a recompiled code library") != std::string::npos, error);

    if (!HaveProgram()) {
        return;
    }
    const std::string dir = DataDir();
    CHECK(CodeLibrary::Load(dir + "/codehost_paged.so", false, &flag, &error) == nullptr);
    CHECK_MESSAGE(error.find("was built for Azahar (rc_build_mode 0): rebuild with --native") != std::string::npos,
                  error);
    CHECK(CodeLibrary::Load(dir + "/codehost_native_det.so", false, &flag, &error) == nullptr);
    CHECK_MESSAGE(error.find("deterministic build (rc_build_mode 2): run with --deterministic") != std::string::npos,
                  error);
    CHECK(CodeLibrary::Load(dir + "/codehost_native.so", true, &flag, &error) == nullptr);
    CHECK_MESSAGE(error.find("not a deterministic build (rc_build_mode 1)") != std::string::npos, error);

    auto native = CodeLibrary::Load(dir + "/codehost_native.so", false, &flag, &error);
    REQUIRE_MESSAGE(native != nullptr, error);
    CHECK(!native->Deterministic());
    CHECK(native->FunctionCount() > 0);
    CHECK(native->TextBase() == 0x00100000);
    auto deterministic = CodeLibrary::Load(dir + "/codehost_native_det.so", true, &flag, &error);
    REQUIRE_MESSAGE(deterministic != nullptr, error);
    CHECK(deterministic->Deterministic());
    CHECK(deterministic->TextCrc32() == native->TextCrc32());
}

TEST_CASE("Matches compares the base, the size and the CRC-32 of the loaded text") {
    if (!HaveProgram()) {
        return;
    }
    Program program;
    const auto text = program.loader->TextBytes(*program.memory);
    const CodeLibrary& lib = *program.host->Library();
    const mh4n::u32 base = program.loader->Image().text_base;
    REQUIRE(!text.empty());

    CHECK(lib.Matches(text, base));
    CHECK(program.loader->TextCrc32(*program.memory) == lib.TextCrc32());

    std::string error;
    std::vector<mh4n::u8> changed(text.begin(), text.end());
    changed[changed.size() / 2] ^= 1;
    CHECK(!lib.Matches(changed, base, &error));
    CHECK_MESSAGE(error.find("is not the code") != std::string::npos, error);
    CHECK_MESSAGE(error.find(fmt::format("crc32 {:08X}", mh4n::Crc32(changed.data(), changed.size()))) != std::string::npos,
                  error);
    CHECK_MESSAGE(error.find(fmt::format("crc32 {:08X}", lib.TextCrc32())) != std::string::npos, error);
    CHECK_MESSAGE(error.find("rebuild the code library") != std::string::npos, error);

    CHECK(!lib.Matches(text.first(text.size() - 4), base, &error));
    CHECK(!lib.Matches(text, base + mh4n::kPageSize, &error));
    CHECK(lib.Matches(text, base));
}

// ---- the test program through the code host ------------------------------------------------------

TEST_CASE("codehost_test runs through the code host and prints what the host computes") {
    if (!HaveProgram()) {
        return;
    }
    static const char kText[] = "native code host test";
    const std::uint32_t crc = mh4n::Crc32(kText, sizeof(kText) - 1);
    const std::uint32_t mix = ThumbMix(0x12345678u, 0x9ABCDEF0u);

    // The rounding mode changes the dot products, or the test would prove nothing.
    CHECK(ExpectedDots(kFpscrZero).a != ExpectedDots(kFpscrUp).a);
    CHECK(ExpectedDots(kFpscrZero).b != ExpectedDots(kFpscrUp).b);

    for (const std::uint32_t fpscr : {kFpscrZero, kFpscrNearest, kFpscrUp, kFpscrDown}) {
        CAPTURE(fpscr);
        Program program;
        program.raise_preempt_flag = true;  // every OutputDebugString asks for a yield
        program.Start(0, fpscr);
        program.RunToEnd();

        REQUIRE(program.host->Failure() == nullptr);
        REQUIRE(program.exited);
        const Dots dots = ExpectedDots(fpscr);
        const std::vector<std::string> expected = {
            "codehost_test: start",
            fmt::format("crc32 {:08x}", crc),
            fmt::format("thumb_mix {:08x}", mix),
            fmt::format("dot_a {:08x}", dots.a),
            fmt::format("dot_b {:08x}", dots.b),
            "codehost_test: done",
        };
        INFO("got:\n" << Join(program.lines) << "expected:\n" << Join(expected));
        CHECK(program.lines == expected);

        // The preemption flag reached the budget callback, which gave the host the thread back.
        CHECK(program.budget_calls >= 4);
        CHECK(program.enters == program.budget_calls + 1);
        CHECK(program.context_matched);
        // Handlers run with the host's own floating-point state, whatever the thread's is.
        CHECK(program.host_fpu_default_in_handlers);
        for (const int mode : program.rounding_in_handlers) {
            CHECK(mode == FE_TONEAREST);
        }
        CHECK(HostFpuIsDefault());
        CHECK(CodeHost::CurrentContext() == nullptr);
        CHECK(CodeHost::CurrentThread() == nullptr);
    }
}

TEST_CASE("a call to a function that was not recompiled fails with the address and the caller") {
    if (!HaveProgram()) {
        return;
    }
    SUBCASE("through the library's call callback") {
        Program program;
        program.Start(1, kFpscrZero);
        program.RunToEnd();
        const mh4n::CodeFailure* failure = program.host->Failure();
        REQUIRE(failure != nullptr);
        CHECK(failure->message.rfind("not recompiled: 00700000 (lr 001", 0) == 0);
        CHECK(failure->message.find("); add it to config/extra_entries.txt and rebuild") != std::string::npos);
        CHECK(failure->thread_id == 1);
        CHECK(failure->registers.find("lr =001") != std::string::npos);
        CHECK(program.thread->Failed());
        CHECK(!program.exited);
        CHECK(program.lines.back() == "codehost_test: calling code that was not recompiled");
    }
    SUBCASE("at the thread's entry") {
        Program program;
        program.Start(0, kFpscrZero);
        program.thread->Context().r[15] = 0x00700004;
        program.host->Enter(*program.thread);
        const mh4n::CodeFailure* failure = program.host->Failure();
        REQUIRE(failure != nullptr);
        CHECK(failure->message == "not recompiled: 00700004 (lr 00000000); add it to config/extra_entries.txt and rebuild");
        CHECK(program.lines.empty());
    }
    SUBCASE("the fallback handler gets the call and the thread goes on") {
        Program program;
        program.HandleMissingFunctions();
        program.Start(1, kFpscrZero);
        program.RunToEnd();
        CHECK(program.host->Failure() == nullptr);
        CHECK(program.exited);
        CHECK(program.missing_targets == std::vector<mh4n::u32>{0x00700000});
        CHECK(program.context_matched);  // the handler ran with the host's FPU state
        CHECK(program.lines.back() == "codehost_test: returned from it");
    }
}

TEST_CASE("a guest entry that returns is a failure") {
    if (!HaveProgram()) {
        return;
    }
    Program program;
    program.Start(2, kFpscrZero);  // mode 2: _start returns instead of calling ExitProcess
    program.host->Enter(*program.thread);
    const mh4n::CodeFailure* failure = program.host->Failure();
    REQUIRE(failure != nullptr);
    CHECK(failure->message == fmt::format("thread entry {:08X} returned to 00000000",
                                          program.loader->Image().entry));
}

// ---- threads without a library -----------------------------------------------------------------

TEST_CASE("two guest threads keep their own rounding mode across switches") {
    auto memory = GuestMemory::Create();
    REQUIRE(memory != nullptr);
    CodeHost host(*memory);

    std::vector<std::uint32_t> thirds_a, thirds_b;
    GuestThread* b_ptr = nullptr;
    bool switched = false;
    auto a = host.NewThread(10, [&](GuestThread& self) {
        for (;;) {
            thirds_a.push_back(ThirdBits());
            if (!switched) {
                switched = true;
                self.SwitchTo(*b_ptr);  // directly, without going through the host
            } else {
                self.Yield();
            }
        }
    });
    auto b = host.NewThread(11, [&](GuestThread& self) {
        for (;;) {
            thirds_b.push_back(ThirdBits());
            self.Yield();
        }
    });
    b_ptr = b.get();
    a->Context().fpscr = kFpscrZero;
    b->Context().fpscr = kFpscrUp;

    const std::uint32_t toward_zero = ThirdUnder(kFpscrZero);
    const std::uint32_t up = ThirdUnder(kFpscrUp);
    REQUIRE(toward_zero != up);

    host.Enter(*a);  // A records, switches to B, B records and yields to the host
    CHECK(HostFpuIsDefault());
    host.Enter(*a);  // A resumes after its switch, records, yields
    CHECK(HostFpuIsDefault());
    host.Enter(*b);
    host.Enter(*a);
    host.Enter(*b);
    CHECK(HostFpuIsDefault());

    REQUIRE(thirds_a.size() == 3);
    REQUIRE(thirds_b.size() == 3);
    for (const std::uint32_t bits : thirds_a) {
        CHECK(bits == toward_zero);
    }
    for (const std::uint32_t bits : thirds_b) {
        CHECK(bits == up);
    }
    CHECK(host.Failure() == nullptr);
}

TEST_CASE("the current context is the running thread's, after every switch") {
    auto memory = GuestMemory::Create();
    REQUIRE(memory != nullptr);
    CodeHost host(*memory);

    CHECK(CodeHost::CurrentContext() == nullptr);
    CHECK(CodeHost::CurrentThread() == nullptr);
    struct Seen {
        RCtx* context;
        GuestThread* thread;
    };
    std::vector<Seen> seen;
    const auto note = [&seen] { seen.push_back({CodeHost::CurrentContext(), CodeHost::CurrentThread()}); };
    GuestThread* second = nullptr;
    auto first = host.NewThread(1, [&](GuestThread& self) {
        for (;;) {
            note();
            self.SwitchTo(*second);  // B runs, and yields to the host
            note();                  // entered again: A is running once more
            self.Yield();
        }
    });
    auto other = host.NewThread(2, [&](GuestThread& self) {
        for (;;) {
            note();
            self.Yield();
        }
    });
    second = other.get();

    // A new thread's context: zeroed, flat memory, user mode, the thread itself.
    const RCtx& fresh = first->Context();
    CHECK(fresh.mem == memory->Base());
    CHECK(fresh.cpsr_misc == 0x10);
    CHECK(fresh.user == first.get());
    CHECK(fresh.r[15] == 0);
    CHECK(fresh.fpscr == 0);
    CHECK(first->Id() == 1);

    host.Enter(*first);
    CHECK(CodeHost::CurrentContext() == nullptr);
    host.Enter(*first);
    CHECK(CodeHost::CurrentContext() == nullptr);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0].context == &first->Context());
    CHECK(seen[0].thread == first.get());
    CHECK(seen[1].context == &other->Context());
    CHECK(seen[1].thread == other.get());
    CHECK(seen[2].context == &first->Context());
    CHECK(host.Failure() == nullptr);
}

TEST_CASE("many suspended threads can be created and dropped") {
    auto memory = GuestMemory::Create();
    REQUIRE(memory != nullptr);
    CodeHost host(*memory);
    std::vector<std::unique_ptr<GuestThread>> threads;
    int started = 0;
    for (mh4n::u32 id = 1; id <= 64; id++) {
        threads.push_back(host.NewThread(id, [&started](GuestThread& self) {
            started++;
            for (;;) {
                self.Yield();
            }
        }));
    }
    for (auto& thread : threads) {
        host.Enter(*thread);
    }
    CHECK(started == 64);
    threads.resize(10);  // threads that are suspended in the middle of their entry are dropped
    threads.clear();
    CHECK(host.Failure() == nullptr);
}

// ---- the failure paths ---------------------------------------------------------------------------

TEST_CASE("trap, bad_return and slow memory accesses fail the thread and log the registers") {
    auto memory = GuestMemory::Create();
    REQUIRE(memory != nullptr);
    CodeHost host(*memory);
    std::vector<std::string> logged;
    mh4n::SetLogSink([&logged](const mh4n::LogRecord& record) {
        if (record.category == "Cpu") {
            logged.emplace_back(record.message);
        }
    });

    const auto run = [&host](GuestThread::Entry entry, mh4n::u32 id) {
        auto thread = host.NewThread(id, std::move(entry));
        for (int i = 0; i < 16; i++) {
            thread->Context().r[i] = 0x1000u * static_cast<mh4n::u32>(i) + 0x11;
        }
        thread->Context().fn = 1;
        thread->Context().fc = 1;
        host.Enter(*thread);
        return thread;
    };

    auto trap = run([](GuestThread& self) { CodeHostCallbacks::Trap(&self.Context(), 0x00100234, 1); }, 4);
    const mh4n::CodeFailure* failure = host.Failure();
    REQUIRE(failure != nullptr);
    CHECK(failure->message == "undefined instruction at 00100234");
    CHECK(failure->thread_id == 4);
    CHECK(trap->Failed());
    // r0-r15 and the CPSR: N and C set on top of user mode.
    CHECK(failure->registers.find("r0 =00000011 r1 =00001011 r2 =00002011 r3 =00003011") == 0);
    CHECK(failure->registers.find("r12=0000C011 sp =0000D011 lr =0000E011 pc =0000F011") != std::string::npos);
    CHECK(failure->registers.find("cpsr=A0000010") != std::string::npos);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].find("undefined instruction at 00100234") != std::string::npos);
    CHECK(logged[0].find("r12=0000C011") != std::string::npos);

    // Later failures are logged, but the first one stays the record.
    auto bad = run([](GuestThread& self) { CodeHostCallbacks::BadReturn(&self.Context(), 0x1234, 0x5678); }, 5);
    CHECK(host.Failure()->message == "undefined instruction at 00100234");
    CHECK(bad->Failed());
    REQUIRE(logged.size() == 2);
    CHECK(logged[1].find("returned to 00001234, expected 00005678") != std::string::npos);

    // Every message, from a host with nothing recorded yet.
    struct Case {
        const char* what;
        GuestThread::Entry entry;
        const char* message;
    };
    const Case cases[] = {
        {"unsupported", [](GuestThread& s) { CodeHostCallbacks::Trap(&s.Context(), 0x100, 2); },
         "unsupported instruction at 00000100"},
        {"breakpoint", [](GuestThread& s) { CodeHostCallbacks::Trap(&s.Context(), 0x100, 3); },
         "breakpoint at 00000100"},
        {"bad jump", [](GuestThread& s) { CodeHostCallbacks::Trap(&s.Context(), 0x100, 4); },
         "jump to an invalid address at 00000100"},
        {"slow read", [](GuestThread& s) { CodeHostCallbacks::SlowRead(&s.Context(), 0x2000, 4); },
         "slow read of 4 bytes at 00002000: a flat-memory library was expected, rebuild it with --native"},
        {"slow write", [](GuestThread& s) { CodeHostCallbacks::SlowWrite(&s.Context(), 0x2000, 0xAB, 1); },
         "slow write of 1 bytes (000000AB) at 00002000: a flat-memory library was expected, rebuild it with "
         "--native"},
        {"svc without a handler", [](GuestThread& s) { CodeHostCallbacks::Svc(&s.Context(), 0x3D, 0x100); },
         "svc 0x3D with no SVC handler installed"},
        {"budget without a handler", [](GuestThread& s) { CodeHostCallbacks::Budget(&s.Context()); },
         "preemption requested with no budget handler installed"},
        {"entry returned", [](GuestThread&) {}, "thread entry returned"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.what);
        CodeHost fresh(*memory);
        auto thread = fresh.NewThread(7, c.entry);
        fresh.Enter(*thread);
        REQUIRE(fresh.Failure() != nullptr);
        CHECK(fresh.Failure()->message == c.message);
        CHECK(thread->Failed());
    }
    mh4n::SetLogSink({});
}

#endif  // __has_include("recomp.h")
