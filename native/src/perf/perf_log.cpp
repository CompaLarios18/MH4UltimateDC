// --perf-log FILE (N1-23): see perf_log.h.
#include "perf/perf_log.h"

#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <vector>

#include <fmt/format.h>

#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"

namespace mh4n::perf {

namespace {

constexpr const char* kLog = "Perf";

u64 Minus(u64 now, u64 before) {
    return now > before ? now - before : 0;
}

double Us(u64 ns) {
    return static_cast<double>(ns) / 1000.0;
}

// The main thread's entry of a per-thread map; 0 for no main thread (id 0) or no entry yet.
u64 MainEntry(const std::map<u32, u64>& by_thread, u32 main_thread_id) {
    if (main_thread_id == 0) {
        return 0;
    }
    const auto it = by_thread.find(main_thread_id);
    return it != by_thread.end() ? it->second : 0;
}

class PerfLog {
public:
    PerfLog(Runtime& rt, const std::string& path)
        : rt_(rt), file_(std::fopen(path.c_str(), "w")) {
        if (file_ != nullptr) {
            // A row is about 85 bytes: a big buffer keeps the file out of the frame (one write per
            // ~770 rows at 120 fps is one every few seconds).
            std::setvbuf(file_, nullptr, _IOFBF, 1 << 16);
        }
    }
    ~PerfLog() { Finish(); }
    PerfLog(const PerfLog&) = delete;
    PerfLog& operator=(const PerfLog&) = delete;

    bool ok() const { return file_ != nullptr; }

    void Start() {
        const ProcessImage* image = rt_.Image();
        const std::string header = HeaderLine(image != nullptr ? image->program_id : 0, rt_.GetOptions().Has("--deterministic"));
        std::fprintf(file_, "%s\n%s\n", header.c_str(), ColumnLine());
        before_ = Take();
    }

    // The frame watcher's call, on the scheduler thread. The frame may be seen at a preemption check, where
    // the running thread's time since its last flush is not in the scheduler's counters yet; so the row is
    // not written here but at the next system call's `before` hook (OnSvc), which runs after BeginSvc has
    // flushed them.
    void OnFrame(u32 frame, u64 /*tick*/) {
        if (file_ == nullptr) {
            return;
        }
        pending_.push_back(frame);
    }

    // A system call is starting and the counters are current.
    void OnSvc() {
        if (!pending_.empty()) {
            WritePending();
        }
    }

    void Finish() {
        if (file_ != nullptr) {
            WritePending();
            std::fprintf(file_, "# frames=%llu dropped=0\n", static_cast<unsigned long long>(frames_));
            std::fclose(file_);
            file_ = nullptr;
        }
    }

private:
    // One snapshot for the frames seen since the last row; if several were seen (the game made no system
    // call between them), the first gets the time and the others an empty row.
    void WritePending() {
        if (pending_.empty() || file_ == nullptr) {
            return;
        }
        const Totals now = Take();
        for (const u32 frame : pending_) {
            const std::string row = FormatRow(frame, before_, now);
            std::fprintf(file_, "%s\n", row.c_str());
            before_ = now;
            ++frames_;
        }
        pending_.clear();
    }

    Totals Take() {
        const std::shared_ptr<Thread> main = rt_.MainThread();
        return Snapshot(rt_.Scheduler().Counters(), main ? main->thread_id : 0,
                        static_cast<u64>(rt_.Clock().Source().NowNs()));
    }

    Runtime& rt_;
    std::FILE* file_;
    Totals before_;
    std::vector<u32> pending_;
    u64 frames_ = 0;
};

void StartPerfLog(Runtime& rt) {
    const auto path = rt.GetOptions().Get("--perf-log");
    if (!path) {
        return;  // off: nothing is subscribed, nothing is allocated
    }
    auto log = std::make_shared<PerfLog>(rt, *path);
    if (!log->ok()) {
        MH4N_LOG(Error, kLog, "cannot write {}", *path);
        rt.RequestExit(2, "cannot write " + *path);
        return;
    }
    log->Start();
    PerfLog* const raw = log.get();  // rt.Own keeps it alive as long as the callback
    rt.OnFrame([raw](u32 frame, u64 tick) { raw->OnFrame(frame, tick); });
    SvcHooks hooks;
    hooks.before = [raw](const SvcCall&) { raw->OnSvc(); };
    rt.AddSvcHooks(std::move(hooks));
    rt.OnFinish([log] { log->Finish(); });
    rt.Own(log);
}

}  // namespace

MH4N_OPTION("runner", "--perf-log", "FILE",
            "write a CSV row per game frame (# perf v2): time in guest code of the game's main thread and of "
            "all threads, every thread's time in system calls and in services, idle time, thread switches and "
            "preemptions, and the main thread's own time in system calls and in services "
            "(tools/native/perf_report.py; docs/native/perf-procedure.md)");

MH4N_REGISTER(BootStep, "perf.log", BootStep{Phase::Tracing, StartPerfLog});

Totals Snapshot(const SchedulerCounters& counters, u32 main_thread_id, u64 now_ns) {
    Totals t;
    t.now_ns = now_ns;
    for (const auto& [id, ns] : counters.guest_ns) {
        t.guest_ns += ns;
    }
    t.main_ns = MainEntry(counters.guest_ns, main_thread_id);
    t.main_svc_ns = MainEntry(counters.thread_svc_ns, main_thread_id);
    t.main_service_ns = MainEntry(counters.thread_service_ns, main_thread_id);
    for (const SchedulerCounters::Named& named : counters.svc) {
        t.svc_ns += named.ns;
    }
    for (const auto& [name, named] : counters.services) {
        t.service_ns += named.ns;
    }
    t.idle_ns = counters.idle_ns;
    t.switches = counters.thread_switches;
    t.preemptions = counters.preemptions;
    return t;
}

const char* ColumnLine() {
    return "frame,present_us,main_us,guest_us,svc_us,service_us,idle_us,switches,preempts,main_svc_us,"
           "main_service_us";
}

std::string HeaderLine(u64 program_id, bool virtual_clock) {
    const std::time_t now = std::time(nullptr);
    char date[16] = "0000-00-00";
    if (const std::tm* utc = std::gmtime(&now)) {
        std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
    }
    return fmt::format("# perf v2 {} game=0x{:016x} cpu=native new3ds=off clock={} mode={}", date, program_id,
                       virtual_clock ? "virtual" : "host", virtual_clock ? "deterministic" : "realtime");
}

std::string FormatRow(u32 frame, const Totals& before, const Totals& now) {
    return fmt::format("{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{:.3f},{:.3f}", frame,
                       Us(Minus(now.now_ns, before.now_ns)), Us(Minus(now.main_ns, before.main_ns)),
                       Us(Minus(now.guest_ns, before.guest_ns)), Us(Minus(now.svc_ns, before.svc_ns)),
                       Us(Minus(now.service_ns, before.service_ns)), Us(Minus(now.idle_ns, before.idle_ns)),
                       Minus(now.switches, before.switches), Minus(now.preemptions, before.preemptions),
                       Us(Minus(now.main_svc_ns, before.main_svc_ns)),
                       Us(Minus(now.main_service_ns, before.main_service_ns)));
}

}  // namespace mh4n::perf
