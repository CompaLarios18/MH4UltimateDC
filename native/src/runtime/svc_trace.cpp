// --trace-svc FILE: every system call in N0-04's format (tools/native/tracefmt.py), written by a
// `tracing` boot step of the runtime:
//
//   # svctrace v1 <date> game=0x<title id> cpu=native new3ds=off
//   # thread <id> parent=<id> entry=<hex> arg=<hex> stack=<hex> prio=<n> core=<requested>/<resolved>
//   <thread> <svc> pc=<hex> lr=<hex> <r0> <r1> <r2> <r3> t=<ticks> in4=<r4>,<r5>,<r6> out=<r0>,<r1>,<r2>,<r3>
//
// A call's line is written when it completes for its thread (the SVC layer's `complete` hook), so a
// call that blocked comes after other threads' later calls. A call that never completed gets
// `out=-` at the end of the run. The main thread's line (parent=0) is written before the first
// call or thread line, from the thread the runtime made. Ticks are game-clock ticks since the
// scheduler started.
#include <memory>
#include <string>

#include <fmt/format.h>

#include "mh4n/log.h"
#include "mh4n/runtime.h"
#include "trace_util.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Runtime";

class SvcTracer {
public:
    SvcTracer(Runtime& rt, const std::string& path)
        : rt_(rt), file_(path), epoch_(detail::TraceEpoch(rt.GetOptions())) {}
    bool ok() const { return file_.ok(); }

    void WriteHeader() {
        const ProcessImage* image = rt_.Image();
        file_.Write(detail::TraceHeader("svctrace", image ? image->program_id : 0));
    }

    SvcHooks Hooks() {
        SvcHooks hooks;
        hooks.thread_created = [this](const SvcThreadCreated& t) {
            WriteMainThread();
            file_.Write(fmt::format("# thread {} parent={} entry={:08X} arg={:08X} stack={:08X} prio={} "
                                    "core={}/{}\n",
                                    t.thread_id, t.parent_id, t.entry, t.arg, t.stack_top, t.priority,
                                    t.requested_processor, t.resolved_processor));
        };
        hooks.complete = [this](const SvcCall& call, const std::array<u32, 4>& out) {
            WriteMainThread();
            file_.Write(Head(call) +
                        fmt::format(" out={:08X},{:08X},{:08X},{:08X}\n", out[0], out[1], out[2], out[3]));
        };
        return hooks;
    }

    // The run is over: the calls that never completed, then the file.
    void Finish() {
        WriteMainThread();
        for (const SvcCall& call : rt_.Svc().PendingCalls()) {
            file_.Write(Head(call) + " out=-\n");
        }
        file_.Close();
    }

private:
    std::string Head(const SvcCall& call) const {
        return fmt::format("{} {:02X} pc={:08X} lr={:08X} {:08X} {:08X} {:08X} {:08X} t={} "
                           "in4={:08X},{:08X},{:08X}",
                           call.thread_id, call.number, call.pc, call.lr, call.regs[0], call.regs[1],
                           call.regs[2], call.regs[3], detail::TraceTick(call.ticks, epoch_), call.regs[4],
                           call.regs[5], call.regs[6]);
    }

    void WriteMainThread() {
        if (main_written_) {
            return;
        }
        const std::shared_ptr<Thread> thread = rt_.MainThread();
        if (!thread) {
            return;
        }
        main_written_ = true;
        file_.Write(fmt::format("# thread {} parent=0 entry={:08X} arg={:08X} stack={:08X} prio={} "
                                "core={}/{}\n",
                                thread->thread_id, thread->entry_point, 0u, thread->stack_top,
                                thread->nominal_priority, thread->processor_id, thread->processor_id));
    }

    Runtime& rt_;
    detail::TraceFile file_;
    u64 epoch_;
    bool main_written_ = false;
};

void StartSvcTrace(Runtime& rt) {
    const auto path = rt.GetOptions().Get("--trace-svc");
    if (!path) {
        return;
    }
    auto tracer = std::make_shared<SvcTracer>(rt, *path);
    if (!tracer->ok()) {
        MH4N_LOG(Error, kLog, "cannot write {}", *path);
        rt.RequestExit(2, "cannot write " + *path);
        return;
    }
    tracer->WriteHeader();
    rt.AddSvcHooks(tracer->Hooks());
    rt.OnFinish([tracer] { tracer->Finish(); });
    rt.Own(tracer);
}

MH4N_REGISTER(BootStep, "runtime.trace-svc", BootStep{Phase::Tracing, StartSvcTrace});

}  // namespace

}  // namespace mh4n
