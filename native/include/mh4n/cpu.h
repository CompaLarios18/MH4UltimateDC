// The code host (N1-04): loads a code library built with `python3 -m recomp.build --code-library
// --native`, checks that it was made from the loaded program, runs guest threads on fibers and routes
// every runtime callback of tools/recomp/runtime/recomp.h (RHost version 5).
//
// Who does what:
//   - CodeLibrary: the shared library and its function table. It knows nothing about threads.
//   - CodeHost: one per runtime. Owns the library once set, creates GuestThreads, switches into
//     them, and holds the three handlers the other modules install: the SVC dispatcher (the kernel,
//     N1-07), the budget callback (the scheduler's Preempt(), N1-06) and, optionally, the fallback
//     for functions that were not recompiled (the interpreter, N1-28).
//   - GuestThread: an RCtx and a fiber (8 MiB stack). The scheduler decides which one runs.
//
// Switching (all on the host thread that calls Enter; one CodeHost may serve several host threads
// later, each with its own host fiber):
//     host stack --Enter(t)--> t --Yield()--> host stack          (Enter returns)
//                              t --SwitchTo(u)--> u               (Enter still has not returned)
// Enter(t) returns whenever any guest thread yields to the host or fails. A thread that is
// suspended inside a handler (SVC or budget) resumes inside that handler, which is how the kernel
// blocks a thread in the middle of a system call.
//
// Floating point: the host FPU follows FPSCR.RMode and FZ of the running thread's RCtx while guest
// code runs (rc_apply_rounding). It is set back to the host default around every handler call (SVC,
// budget, missing function) and after Enter returns, and the guest's mode is applied again after
// every switch back into a guest fiber and after every handler.
//
// Failures (a trap, a bad return, a function that was not recompiled, an entry that returned, a
// slow memory access) are recorded once in CodeHost::Failure(), logged at Critical with r0-r15 and
// the CPSR, and the failing thread yields to the host and is never resumed. The runtime ends the
// process with exit status 4 when Failure() is set after Enter returns.
//
// Ending a thread: svcExitThread (N1-07) must not hold owning objects (shared_ptr...) on the
// thread's stack when it makes the last switch: a fiber that is never resumed is freed without
// unwinding it. Destroy a GuestThread only when it is not the running one.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "mh4n/types.h"

// tools/recomp/runtime/recomp.h. Include that header (inside extern "C", after the C++ headers: it
// defines short macros such as R) where the registers are needed.
struct RCtx;
struct RCodeInfo;

namespace Port {
class Fiber;
}

namespace mh4n {

class GuestMemory;
class CodeHost;
class GuestThread;
struct CodeHostCallbacks;

// A recompiled guest function: runs until the guest returns, leaving the return target in r15.
using GuestFunction = void (*)(RCtx*);

// ---- the code library -------------------------------------------------------------------------

class CodeLibrary {
public:
    // Loads the library at `path` (dlopen; LoadLibraryExW with LOAD_WITH_ALTERED_SEARCH_PATH on
    // Windows, the UTF-8 path converted) and calls its rc_code_init. `deterministic` is whether
    // the runtime was started with --deterministic: the library must have been built the same way.
    // `preempt_flag` is the scheduler's flag that RC_TICK reads; it must outlive the library.
    // nullptr and a message a player can act on in *error for: a file that cannot be loaded, a
    // file that is not a code library, a library built for Azahar (rc_build_mode 0), a
    // deterministic build without --deterministic and the reverse, a library built against another
    // recomp.h.
    static std::unique_ptr<CodeLibrary> Load(const std::string& path, bool deterministic,
                                             volatile u32* preempt_flag, std::string* error);
    ~CodeLibrary();
    CodeLibrary(const CodeLibrary&) = delete;
    CodeLibrary& operator=(const CodeLibrary&) = delete;

    const std::string& Path() const { return path_; }
    bool Deterministic() const { return deterministic_; }
    // The code it was recompiled from.
    u32 TextBase() const;
    u32 TextSize() const;
    u32 TextCrc32() const;
    u64 TitleId() const;
    u32 FunctionCount() const;
    // The raw tables (functions, hook points, tunables). Valid while the library is loaded.
    const RCodeInfo& Info() const { return *info_; }

    // The recompiled function at `target`; Thumb entries have bit 0 set. nullptr if there is none.
    GuestFunction Find(u32 target) const;

    // Is `text`, the loaded program's text segment at guest address `text_base`
    // (Loader::TextBytes), the code this library was made from: the same base and size and the
    // same CRC-32 as text_crc32? If not, false and a message that names both CRCs in *error. The
    // runtime refuses to start then (exit status 2, D13).
    bool Matches(std::span<const u8> text, u32 text_base, std::string* error = nullptr) const;

private:
    CodeLibrary() = default;
    std::string path_;
    bool deterministic_ = false;
    void* handle_ = nullptr;  // dlopen's handle, or the HMODULE
    std::unique_ptr<RCodeInfo> info_;
};

// ---- failures and handlers --------------------------------------------------------------------

struct CodeFailure {
    std::string message;    // one line, e.g. "not recompiled: 00123456 (lr 00100020); add it ..."
    std::string registers;  // r0-r15 and the CPSR, a few lines
    u32 thread_id = 0;
};

// The kernel's SVC dispatcher (N1-07). `next_pc` is the address after the SVC instruction (bit 0
// set for Thumb); the dispatcher writes r15 itself.
using SvcHandler = std::function<void(RCtx& ctx, u32 imm, u32 next_pc)>;
// The scheduler's Preempt(): the preemption flag was set (or, in a deterministic build, the cycle
// budget ran out). It clears the flag and may switch threads.
using BudgetHandler = std::function<void(RCtx& ctx)>;
// A call to `target` (Thumb bit set as in the function table) that the library has no function for.
// It runs the code to its return, as the interpreter does (N1-28).
using MissingFunctionHandler = std::function<void(RCtx& ctx, u32 target)>;

// ---- guest threads ----------------------------------------------------------------------------

class GuestThread {
public:
    // What a thread runs when it is not given guest code (tests, tools): called on the fiber with
    // the thread's FPU mode applied. Returning from it is a failure, as for guest code.
    using Entry = std::function<void(GuestThread&)>;

    ~GuestThread();
    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;

    u32 Id() const { return id_; }
    CodeHost& Host() const { return host_; }
    // Zeroed at creation except mem (guest address 0 of the host's GuestMemory), cpsr_misc (0x10,
    // user mode) and user (this thread). The creator sets the registers: a guest thread starts at
    // the function at r15 (bit 0 set for Thumb) the first time it is entered.
    RCtx& Context() { return *ctx_; }
    const RCtx& Context() const { return *ctx_; }
    bool Failed() const { return failed_; }

    // The running thread gives control back to the host (Enter returns). It resumes here when it
    // is entered again.
    void Yield();
    // The running thread gives control to `next`, which must not be the running thread nor have
    // failed. The caller resumes here when something switches back to it or enters it.
    void SwitchTo(GuestThread& next);
    // Records a failure (message plus the register dump), logs it and yields; never returns.
    // Called on the thread's own fiber.
    [[noreturn]] void Fail(std::string message);

private:
    friend class CodeHost;
    friend struct CodeHostCallbacks;
    GuestThread(CodeHost& host, u32 id, Entry entry);
    void Main();
    // The host FPU follows the guest's FPSCR, or the host default while a handler runs.
    void ApplyFpuState();
    void EnterHost();
    void LeaveHost();

    CodeHost& host_;
    u32 id_;
    Entry entry_;
    std::unique_ptr<RCtx> ctx_;
    std::unique_ptr<Port::Fiber> fiber_;
    bool in_host_ = false;  // inside a handler: the host's FPU mode applies
    bool failed_ = false;
};

// ---- the host ---------------------------------------------------------------------------------

class CodeHost {
public:
    explicit CodeHost(GuestMemory& memory);
    ~CodeHost();
    CodeHost(const CodeHost&) = delete;
    CodeHost& operator=(const CodeHost&) = delete;

    GuestMemory& Memory() const { return memory_; }

    void SetLibrary(std::unique_ptr<CodeLibrary> library) { library_ = std::move(library); }
    const CodeLibrary* Library() const { return library_.get(); }

    void SetSvcHandler(SvcHandler handler) { svc_ = std::move(handler); }
    void SetBudgetHandler(BudgetHandler handler) { budget_ = std::move(handler); }
    // Without one, a call to a function that was not recompiled fails.
    void SetMissingFunctionHandler(MissingFunctionHandler handler) { missing_ = std::move(handler); }

    // A thread that is not running yet. With an entry it runs that, else the guest function at
    // Context().r[15] through the library.
    std::unique_ptr<GuestThread> NewThread(u32 id, GuestThread::Entry entry = {});

    // From the host stack: runs `thread` until some guest thread yields to the host or fails. Call
    // it only from a host thread that is not inside a guest fiber, and not for a failed thread.
    void Enter(GuestThread& thread);

    // Calls the guest function at `target` on the calling guest thread's context: the library's
    // function, else the missing-function handler, else a failure.
    void Call(GuestThread& thread, u32 target);

    // The first failure of any thread, or nullptr.
    const CodeFailure* Failure() const { return failure_ ? &*failure_ : nullptr; }

    // The RCtx and thread running on this host thread (nullptr on the host stack): the fault
    // handler of N1-02 and the logs use them. Set on every switch into a guest fiber.
    static RCtx* CurrentContext();
    static GuestThread* CurrentThread();

private:
    friend class GuestThread;
    friend struct CodeHostCallbacks;

    GuestMemory& memory_;
    std::unique_ptr<CodeLibrary> library_;
    SvcHandler svc_;
    BudgetHandler budget_;
    MissingFunctionHandler missing_;
    std::optional<CodeFailure> failure_;
};

}  // namespace mh4n
