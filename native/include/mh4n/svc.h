// The SVC table (N1-07): every system call the game makes, decoded from and encoded into the guest's
// registers as the 3DS kernel does, on top of the kernel objects (kernel.h, N1-05), the scheduler
// (scheduler.h, N1-06) and the thread manager (threads.h, N1-27).
//
// Typical use (the runner, N1-09):
//   SvcTable svc(kernel, scheduler, threads);
//   svc.Install(host);                              // CodeHost::SetSvcHandler
//   svc.SetHooks({.before = ..., .complete = ..., .thread_created = ...});   // --trace-svc
//   svc.SetConnectToPortHandler(...);  svc.SetSendSyncRequestHandler(...);   // N1-10
//   ...Run the scheduler...
//   if (svc.Exit()) status = svc.Exit()->status;    // 0 after ExitProcess, 3 after svcBreak
//
// How an SVC runs (`CallSvc`): r15 is set to the address after the instruction (`next_pc & ~1`),
// the call is bracketed with GuestScheduler::BeginSvc/EndSvc, the arguments are decoded from r0-r6
// (the ABI of Azahar's SVCWrapper: the result goes in r0, then the outputs in r1 upward), the call
// runs and the outputs are written. The thread's registers are written by this layer only: the
// scheduler returns a WaitOutcome and never touches r0/r1. Calls that block (WaitSynchronization,
// SleepThread, ArbitrateAddress, SendSyncRequest) switch fibers inside the call and write their
// registers when the thread runs again.
//
// Outputs: a call that fails, and one that leaves an output unset (WaitSynchronizationN with no
// index), writes 0 to its output registers (Azahar's wrapper writes whatever its output variable
// held, which is stack garbage; the reference reads 0 where the conformance programs look); the
// trace comparator drops r1-r3 of failed and timed-out calls.
//
// Unknown SVCs (a number this table does not know, or one Azahar leaves unimplemented): logged once
// per number as `unimplemented svc 0x<n> at <lr>`; the registers stay as they are. The trace hooks
// still run for them.
//
// Tracing: `before` runs when the call starts, `complete` when it completes *for the calling
// thread*: at once for a call that returns, when the thread runs again for one that blocked. A call
// that never completes (svcExitThread, svcExitProcess, a thread that is terminated while it waits)
// stays in PendingCalls(), which the runner writes as `out=-` at the end. For a successful
// CreateThread, `thread_created` runs just before `complete`.
//
// Lifetime: destroy the table before the kernel and the scheduler (it holds references to both and
// has installed a handler in the code host: Install's host must outlive it or the handler be reset).
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mh4n/cpu.h"
#include "mh4n/kernel.h"
#include "mh4n/scheduler.h"
#include "mh4n/threads.h"
#include "mh4n/types.h"

// cpu.h declares RCtx (tools/recomp/runtime/recomp.h) at global scope.

namespace mh4n {

// ---- tracing -------------------------------------------------------------------------------------

// A call as it was made: what N0-04's trace line needs before the call (`in4=` is r4-r6).
struct SvcCall {
    u32 thread_id = 0;
    u32 number = 0;
    u32 pc = 0;   // r15: the address after the SVC instruction, Thumb bit cleared
    u32 lr = 0;   // r14
    std::array<u32, 7> regs{};  // r0-r6
    u64 ticks = 0;  // game clock when the call started
};

// A thread made by a successful svcCreateThread (N0-04's `# thread` line).
struct SvcThreadCreated {
    u32 parent_id = 0;
    u32 thread_id = 0;
    u32 entry = 0;
    u32 arg = 0;
    u32 stack_top = 0;
    u32 priority = 0;
    s32 requested_processor = 0;  // r4 as the guest passed it (-2 means "the process's default")
    s32 resolved_processor = 0;
};

struct SvcHooks {
    std::function<void(const SvcCall&)> before;
    // `out` is r0-r3 when the call completed for its thread.
    std::function<void(const SvcCall&, const std::array<u32, 4>& out)> complete;
    std::function<void(const SvcThreadCreated&)> thread_created;
    // OutputDebugString's text (also logged at Debug level, category "Debug.Emulated").
    std::function<void(std::string_view)> debug_string;
};

// ---- how the run ended -----------------------------------------------------------------------------

struct SvcExit {
    int status = 0;      // 0 after svcExitProcess, 3 after svcBreak
    std::string report;  // svcBreak: the reason and the registers of the thread; empty for ExitProcess
};

// ---- the IPC layer's handlers (N1-10) -----------------------------------------------------------------

// svcConnectToPort: `name` is the port name (at most 11 characters, read from guest memory). On
// success the handler sets *session to the new client session; the SVC layer puts it in the
// process's handle table and returns the handle (kResultOutOfHandles if the table is full). Without
// a handler every name is unknown (0xD88007FA, Azahar's answer for an unknown port).
using ConnectToPortHandler =
    std::function<Result(const std::string& name, std::shared_ptr<Object>* session)>;

// svcSendSyncRequest. The handler looks `session` up in the process's handle table and answers
// kResultInvalidHandle if it is not a client session (Azahar's answer; the SVC layer cannot tell, it
// does not know the session class). The request runs on the calling guest thread. A handler whose
// reply is not ready puts the thread in its wait state (ThreadStatus::WaitIPC, with a wake hook if
// it needs one) and returns; the SVC layer then parks the thread, and when it runs again r0 is the
// result the wake-up delivered, or the handler's result if the wake-up left it empty. Without a
// handler the answer is kResultInvalidHandle.
using SendSyncRequestHandler = std::function<Result(Thread& thread, Handle session)>;

// ---- the table -------------------------------------------------------------------------------------

class SvcTable {
public:
    SvcTable(Kernel& kernel, GuestScheduler& scheduler, ThreadManager& threads);
    ~SvcTable();
    SvcTable(const SvcTable&) = delete;
    SvcTable& operator=(const SvcTable&) = delete;

    // Makes CallSvc the code host's SVC handler.
    void Install(CodeHost& host);

    // One SVC of the running guest thread: `imm` is the number in the instruction, `next_pc` the
    // address after it (bit 0 set for Thumb). Must run on a guest fiber of the scheduler.
    void CallSvc(RCtx& ctx, u32 imm, u32 next_pc);

    void SetHooks(SvcHooks hooks);
    void SetConnectToPortHandler(ConnectToPortHandler handler);
    void SetSendSyncRequestHandler(SendSyncRequestHandler handler);

    // Set by svcExitProcess and svcBreak. The scheduler has been asked to stop (RequestStop) and the
    // calling thread has ended; the runner takes the exit status from here.
    const std::optional<SvcExit>& Exit() const;

    // Calls that have started and not completed, by thread id.
    std::vector<SvcCall> PendingCalls() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mh4n
