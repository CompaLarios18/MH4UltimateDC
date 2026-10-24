#include "mh4n/runtime.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "../common/utf8.h"
#include "../kernel/deterministic.h"
#include "gpu_host.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "timer_resolution.h"

#if defined(MH4N_HAVE_SCHEDULE)
#include "schedule.h"
#endif

namespace mh4n {

namespace {

constexpr const char* kLog = "Runtime";
constexpr u32 kDefaultSliceUs = 1000;
constexpr double kDefaultStopMs = 10000.0;

// --init-time in microseconds (the default without the option); false and a message that names the
// option and the text when it is malformed or out of range (the caller ends the run with status 2).
bool ReadInitTime(const Options& options, u64* micros, std::string* error) {
    const std::optional<std::string> text = options.Get("--init-time");
    if (!text) {
        *micros = kDefaultInitTime * 1000000;
        return true;
    }
    const InitTimeParse parsed = ParseInitTime(*text);
    if (!parsed.ok) {
        *error = "bad --init-time '" + *text + "': " + parsed.error;
        return false;
    }
    *micros = parsed.micros;
    return true;
}

// The run's limits: --ms (milliseconds, or f<N> for a frame) and --frames. As mh4u_runner: without
// --ms but with --frames there is no time limit, and without either the run stops after 10 s.
struct Limits {
    double stop_ms = kDefaultStopMs;
    bool has_stop_ms = false;
    std::optional<u32> stop_frame;
    bool no_time_limit() const { return stop_frame.has_value() && !has_stop_ms; }
};

#if defined(MH4N_HAVE_SCHEDULE)
bool ParseLimits(const Options& options, Limits* out, std::string* error) {
    Schedule::Plan plan;
    if (const auto ms = options.Get("--ms"); ms && !Schedule::ParseStop(*ms, plan, *error)) {
        *error = "bad --ms " + *ms + " (" + *error + ")";
        return false;
    }
    if (const auto frames = options.Get("--frames");
        frames && !Schedule::ParseFrames(*frames, plan, *error)) {
        *error = "bad --frames " + *frames + " (" + *error + ")";
        return false;
    }
    out->stop_ms = plan.stop_ms;
    out->has_stop_ms = plan.has_stop_ms;
    if (plan.has_stop_frame) {
        out->stop_frame = plan.stop_frame;
    }
    return true;
}
#else
// Without port/src/schedule.cpp (a copy of native/ on its own): the same two forms, milliseconds
// and f<N> / a number of frames.
bool ParseWhole(const std::string& text, u32* out) {
    if (text.empty() || text.size() > 10 || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    const unsigned long long value = std::stoull(text);
    if (value > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<u32>(value);
    return true;
}

bool ParseLimits(const Options& options, Limits* out, std::string* error) {
    if (const auto ms = options.Get("--ms")) {
        u32 frame = 0;
        if (!ms->empty() && (*ms)[0] == 'f') {
            if (!ParseWhole(ms->substr(1), &frame)) {
                *error = "bad --ms " + *ms + " (want milliseconds, or f<frame>)";
                return false;
            }
            out->stop_frame = frame;
        } else {
            try {
                std::size_t used = 0;
                out->stop_ms = std::stod(*ms, &used);
                if (used != ms->size() || !(out->stop_ms >= 0.0)) {
                    throw std::invalid_argument("ms");
                }
            } catch (const std::exception&) {
                *error = "bad --ms " + *ms + " (want milliseconds, or f<frame>)";
                return false;
            }
            out->has_stop_ms = true;
        }
    }
    if (const auto frames = options.Get("--frames")) {
        u32 frame = 0;
        if (!ParseWhole(*frames, &frame)) {
            *error = "bad --frames " + *frames + " (want a number of frames)";
            return false;
        }
        out->stop_frame = frame;
    }
    return true;
}
#endif

// --deterministic (N1-25): the runtime's default clock and timer thread give way to the virtual clock
// and its preemption source (kernel/deterministic.h). A clock the caller made (a test's) is kept if it
// already is the virtual one; any other cannot run the mode.
bool UseVirtualTime(std::unique_ptr<TimeSource>& time, std::unique_ptr<PreemptionSource>& preemption,
                    VirtualTimeSource** virtual_time, DeterministicPreemption** virtual_preemption,
                    std::string* error) {
    if (auto* existing = dynamic_cast<VirtualTimeSource*>(time.get())) {
        auto* source = dynamic_cast<DeterministicPreemption*>(preemption.get());
        if (source == nullptr) {
            *error = "--deterministic: the virtual clock needs a DeterministicPreemption";
            return false;
        }
        *virtual_time = existing;
        *virtual_preemption = source;
        return true;
    }
    if (dynamic_cast<SteadyTimeSource*>(time.get()) == nullptr) {
        *error = "--deterministic needs the runtime's own clock, not another one";
        return false;
    }
    auto source = std::make_unique<VirtualTimeSource>();
    auto stepping = std::make_unique<DeterministicPreemption>(*source);
    *virtual_time = source.get();
    *virtual_preemption = stepping.get();
    preemption.reset();  // the timer thread (never started) refers to the clock below
    time = std::move(source);
    preemption = std::move(stepping);
    return true;
}

// --deterministic (N1-25): the reference runner's default mode gives the console the Wi-Fi MAC
// 02:00:00:00:00:01 (port/README.md, the determinism table), where the cfg service would draw a random one
// and keep it in the user folder. So that two runs from the same folder see the same MAC, a folder that
// exists and has none gets that address in sysdata/mac.txt first (the file cfg reads; a folder that
// already has one keeps it, as does a run with no folder).
void SeedMacAddress(const std::string& user_folder) {
    if (user_folder.empty()) {
        return;
    }
    std::error_code ec;
    const std::filesystem::path folder = utf8::PathFromUtf8(user_folder);
    if (!std::filesystem::is_directory(folder, ec)) {
        return;
    }
    const std::filesystem::path path = folder / "sysdata" / "mac.txt";
    if (std::filesystem::exists(path, ec)) {
        return;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary) << "02:00:00:00:00:01";
}

}  // namespace

const char* PhaseName(Phase phase) {
    switch (phase) {
    case Phase::Devices: return "devices";
    case Phase::Services: return "services";
    case Phase::Tracing: return "tracing";
    case Phase::Started: return "started";
    }
    return "?";
}

Runtime::Runtime(RuntimeConfig config)
    : time_(std::move(config.time)), preemption_(std::move(config.preemption)) {
    if (!time_) {
        time_ = std::make_unique<SteadyTimeSource>();
        if (!preemption_) {
            preemption_ = MakeTimerThreadPreemption(*time_);
        }
    }
}

Runtime::~Runtime() {
    if (scheduler_) {
        scheduler_->Shutdown();
    }
    RunFinishers();
    // Reverse order of Own(): the objects may refer to the parts below.
    while (!owned_.empty()) {
        owned_.pop_back();
    }
    frame_callbacks_.clear();
    owned_audio_.reset();
    owned_input_.reset();
    owned_gpu_.reset();
    interfaces_ = {};
    main_thread_.reset();
    svc_.reset();
    threads_.reset();
    pages_.reset();
    gpu_host_.reset();
    kernel_.reset();
    scheduler_.reset();
    code_host_.reset();
    clock_.reset();
    memory_.reset();
    loader_.reset();
    preemption_.reset();
    time_.reset();
}

// ---- building the world ------------------------------------------------------------------------

bool Runtime::CreateWorld(u32 memory_mode, std::string* error) {
    MH4N_ASSERT(!prepared_, "Prepare or Boot called twice");
    if (!FcramRegions::ValidMemoryMode(memory_mode)) {
        *error = "invalid memory mode " + std::to_string(memory_mode);
        return false;
    }
    // --deterministic (N1-25): the clock is virtual and counts the cycles the code library charges.
    VirtualTimeSource* virtual_time = nullptr;
    DeterministicPreemption* virtual_preemption = nullptr;
    if (options_.Has("--seed") && !options_.Has("--deterministic")) {
        *error = "--seed needs --deterministic";
        return false;
    }
    if (options_.Has("--deterministic") &&
        !UseVirtualTime(time_, preemption_, &virtual_time, &virtual_preemption, error)) {
        return false;
    }
    memory_ = GuestMemory::Create();
    if (!memory_) {
        *error = "cannot reserve the guest address space";
        return false;
    }
    clock_ = std::make_unique<GameClock>(*time_);
    if (virtual_time != nullptr) {
        // The reference runner's emulated clock does not start at 0 (port/README.md): the game sees the
        // same ticks, and the shared page the same date, as in its recordings.
        virtual_time->SetStartTicks(kDeterministicStartTicks);  // both cores' clocks, as Azahar's two timers
    }
    code_host_ = std::make_unique<CodeHost>(*memory_);
    kernel_ = std::make_unique<Kernel>(*memory_, memory_mode);
    GuestScheduler::Options scheduler_options;
    scheduler_options.slice_us = static_cast<u32>(
        options_.GetUInt("--slice-us", virtual_time != nullptr ? kDeterministicSliceUs : kDefaultSliceUs));
    scheduler_ = std::make_unique<GuestScheduler>(*kernel_, *code_host_, *clock_, *time_,
                                                  preemption_.get(), scheduler_options);
    threads_ = std::make_unique<ThreadManager>(*kernel_, *scheduler_);
    svc_ = std::make_unique<SvcTable>(*kernel_, *scheduler_, *threads_);
    svc_->Install(*code_host_);
    if (virtual_time != nullptr) {
        CycleDriverOptions driver_options;
        driver_options.seeded = options_.Has("--seed");
        driver_options.seed = options_.GetUInt("--seed", 0);
        driver_options.slice_us = scheduler_options.slice_us;
        auto driver = std::make_shared<CycleDriver>(*code_host_, *scheduler_, *svc_, *virtual_time,
                                                    *virtual_preemption, driver_options);
        driver->Install();
        Own(driver);
        if (driver_options.seeded) {
            MH4N_LOG(Info, kLog, "deterministic mode: seed {}, slices of {} us on average",
                     driver_options.seed, driver_options.slice_us);
        } else {
            MH4N_LOG(Info, kLog, "deterministic mode: no seed, slices of {} us", driver_options.slice_us);
        }
    }
    gpu_host_ = std::make_unique<detail::RuntimeGpuHost>(*memory_, *kernel_);
    scheduler_->OnSafePoint([this] { PollFrame(); });
    InstallSvcHooks();
    prepared_ = true;
    return true;
}

bool Runtime::Prepare(const Options& options, const std::vector<Phase>& phases, std::string* error,
                      u32 memory_mode) {
    std::string local;
    std::string& message = error ? *error : local;
    try {
        options_ = options;
        user_folder_ = options_.GetString("--user");
        if (!ReadInitTime(options_, &init_time_us_, &message)) {
            return false;
        }
        if (!CreateWorld(memory_mode, &message)) {
            return false;
        }
    } catch (const OptionError& e) {
        message = e.what();
        return false;
    }
    kernel_->NewProcess("prepared");
    RunPhases(phases);
    return true;
}

void Runtime::RunPhases(const std::vector<Phase>& phases) {
    for (const Phase phase : {Phase::Devices, Phase::Services, Phase::Tracing, Phase::Started}) {
        if (std::find(phases.begin(), phases.end(), phase) == phases.end()) {
            continue;
        }
        // The registry is in name order.
        for (const auto& [name, step] : Registry<BootStep>::Instance().All()) {
            if (step.phase == phase) {
                MH4N_LOG(Debug, kLog, "boot step {} ({})", name, PhaseName(phase));
                step.run(*this);
            }
        }
    }
}

// ---- booting -----------------------------------------------------------------------------------

void Runtime::SetMainThreadEntry(GuestThread::Entry entry) {
    main_entry_ = std::move(entry);
}

int Runtime::Boot(const Options& options) {
    try {
        return Run(options);
    } catch (const OptionError& e) {
        MH4N_LOG(Error, kLog, "{}", e.what());
        return 2;
    }
}

// Boot: steps 1 to 8 of the header's list.
int Runtime::Run(const Options& options) {
    options_ = options;
    if (options_.positional().size() != 1) {
        MH4N_LOG(Error, kLog, "expected one PROGRAM (a game folder or an ELF), got {}",
                 options_.positional().size());
        return 2;
    }
    program_path_ = options_.positional()[0];
    user_folder_ = options_.GetString("--user");
    Limits limits;
    std::string error;
    if (!ReadInitTime(options_, &init_time_us_, &error)) {
        MH4N_LOG(Error, kLog, "{}", error);
        return 2;
    }
    if (!ParseLimits(options_, &limits, &error)) {
        MH4N_LOG(Error, kLog, "{}", error);
        return 2;
    }
    stop_frame_ = limits.stop_frame;

    // 1. the program
    const bool is_folder = std::filesystem::is_directory(utf8::PathFromUtf8(program_path_));
    loader_ = is_folder ? Loader::OpenGameFolder(program_path_, &error)
                        : Loader::OpenElf(program_path_, &error);
    if (!loader_) {
        MH4N_LOG(Error, kLog, "cannot load {}: {}", program_path_, error);
        return 2;
    }
    if (is_folder) {
        game_folder_ = program_path_;
    }
    const ProcessImage& image = loader_->Image();

    // 2. the world; the clock is stopped afterwards (it keeps the ticks of the setup, so the process
    // is created at a tick above 0) and runs again when the scheduler starts
    if (!CreateWorld(image.memory_mode, &error)) {
        MH4N_LOG(Error, kLog, "{}", error);
        return 2;
    }
    const bool deterministic = options_.Has("--deterministic");
    if (!deterministic) {
        clock_->Pause();  // (a virtual clock does not move during the setup: nothing to hold)
    }
    kernel_->NewProcess(image.name);

    // 3. the segments and the two system pages
    if (!loader_->Map(*memory_, kernel_->Regions(), &error)) {
        MH4N_LOG(Error, kLog, "cannot map {}: {}", program_path_, error);
        return 2;
    }
    // Process::HeapAllocate charges every segment (the stack too) to the process and to its Commit
    // limit; the game sizes its heaps from what is left of that limit (found by N1-21: without the
    // charge it asked for more linear memory than the region has).
    const auto process = kernel_->CurrentProcess();
    for (const SegmentMapping& segment : image.segments) {
        process->memory_used += segment.size;
        process->resource_limit->Reserve(ResourceLimitType::Commit, static_cast<s32>(segment.size));
    }
    pages_ = std::make_unique<SystemPages>(SystemPages::FromMicroseconds(init_time_us_));
    if (const Result result = pages_->Map(*memory_, kernel_->Regions()); IsError(result)) {
        MH4N_LOG(Error, kLog, "cannot map the configuration memory and the shared page: {:08X}", result);
        return 2;
    }
    if (deterministic) {
        // Azahar's first update of the shared page is an event at tick 0 of a timer that starts at the
        // fixed starting tick, so it runs at that tick and writes the date for it (port/README.md).
        pages_->UpdateSharedPage(clock_->Now());
    }

    // 4. the code library, and the check that it was made from this program
    if (const auto path = options_.Get("--code")) {
        auto library = CodeLibrary::Load(*path, deterministic, scheduler_->PreemptFlag(), &error);
        if (!library) {
            MH4N_LOG(Error, kLog, "{}", error);
            return 2;
        }
        if (!library->Matches(loader_->TextBytes(*memory_), image.text_base, &error)) {
            MH4N_LOG(Error, kLog, "the code library {} does not match {}: {}", *path, program_path_, error);
            return 2;
        }
        code_host_->SetLibrary(std::move(library));
    } else if (!main_entry_) {
        MH4N_LOG(Error, kLog, "the program needs its recompiled code: --code LIB");
        return 2;
    }

    if (deterministic) {
        SeedMacAddress(user_folder_);
    }

    // 5. devices, services, tracing
    RunPhases({Phase::Devices, Phase::Services, Phase::Tracing});
    if (exit_status_) {
        return *exit_status_;
    }

    // 6. the main thread
    if (const Result result = threads_->CreateMainThread(image, &main_thread_, main_entry_);
        IsError(result)) {
        MH4N_LOG(Error, kLog, "cannot create the main thread: {:08X}", result);
        return 2;
    }

    // 7. started
    RunPhases({Phase::Started});
    if (exit_status_) {
        return *exit_status_;
    }

    // 8. run, with the host's timer tick at 1 ms (see timer_resolution.h)
    const detail::TimerResolutionScope timer_resolution;
    clock_->Resume();
    const u64 start = clock_->Now();
    ScheduleSharedPageUpdate(start + kSharedPageUpdateIntervalTicks);
    if (!limits.no_time_limit()) {
        const u64 ticks = static_cast<u64>(limits.stop_ms * (static_cast<double>(kTicksPerSecond) / 1000.0));
        scheduler_->ScheduleAt(start + ticks, [this] {
            limit_reason_ = "time limit reached";
            scheduler_->RequestStop();
        });
    }
    PollFrame();

    const RunStatus status = scheduler_->Run();

    int result = 0;
    if (status == RunStatus::Failed) {
        const CodeFailure* failure = code_host_->Failure();
        MH4N_LOG(Error, kLog, "recompiled code stopped: {}", failure ? failure->message : std::string("?"));
        result = 4;
    } else if (exit_status_) {
        result = *exit_status_;
    } else if (const auto& exit = svc_->Exit()) {
        result = exit->status;
        MH4N_LOG(Info, kLog, "process exited (status {})", exit->status);
    } else if (!limit_reason_.empty()) {
        MH4N_LOG(Info, kLog, "{}", limit_reason_);
    }
    MH4N_LOG(Info, kLog, "game time: {:.1f} ms",
             static_cast<double>(clock_->Now() - start) / (static_cast<double>(kTicksPerSecond) / 1000.0));
    RunFinishers();
    return result;
}

// The date on the shared page advances every hour of game time (Azahar's UpdateTimeCallback
// reschedules itself); SystemPages::Map made the update for tick 0.
void Runtime::ScheduleSharedPageUpdate(u64 due_tick) {
    scheduler_->ScheduleAt(due_tick, [this, due_tick] {
        pages_->UpdateSharedPage(clock_->Now());
        ScheduleSharedPageUpdate(due_tick + kSharedPageUpdateIntervalTicks);
    });
}

// ---- the frame watcher and the extension points -----------------------------------------------

void Runtime::PollFrame() {
    if (!memory_) {
        return;
    }
    const std::optional<u32> frame = memory_->Read<u32>(kFrameCounterAddress);
    if (!frame) {
        return;
    }
    if (!have_frame_ || *frame != last_frame_) {
        have_frame_ = true;
        last_frame_ = *frame;
        const u64 tick = clock_->Now();
        for (std::size_t i = 0; i < frame_callbacks_.size(); ++i) {
            const FrameCallback callback = frame_callbacks_[i];  // a callback may subscribe another
            callback(*frame, tick);
        }
    }
    if (stop_frame_ && *frame >= *stop_frame_ && limit_reason_.empty()) {
        limit_reason_ = "frame limit reached: frame " + std::to_string(*frame);
        scheduler_->RequestStop();
    }
}

void Runtime::OnFrame(FrameCallback callback) {
    frame_callbacks_.push_back(std::move(callback));
}

void Runtime::AddSvcHooks(SvcHooks hooks) {
    svc_hooks_.push_back(std::move(hooks));
    if (svc_) {
        InstallSvcHooks();
    }
}

void Runtime::InstallSvcHooks() {
    auto any = [this](auto member) {
        return std::any_of(svc_hooks_.begin(), svc_hooks_.end(),
                           [member](const SvcHooks& h) { return static_cast<bool>(h.*member); });
    };
    SvcHooks combined;
    if (any(&SvcHooks::before)) {
        combined.before = [this](const SvcCall& call) {
            for (const SvcHooks& h : svc_hooks_) {
                if (h.before) {
                    h.before(call);
                }
            }
        };
    }
    if (any(&SvcHooks::complete)) {
        combined.complete = [this](const SvcCall& call, const std::array<u32, 4>& out) {
            for (const SvcHooks& h : svc_hooks_) {
                if (h.complete) {
                    h.complete(call, out);
                }
            }
        };
    }
    if (any(&SvcHooks::thread_created)) {
        combined.thread_created = [this](const SvcThreadCreated& created) {
            for (const SvcHooks& h : svc_hooks_) {
                if (h.thread_created) {
                    h.thread_created(created);
                }
            }
        };
    }
    if (any(&SvcHooks::debug_string)) {
        combined.debug_string = [this](std::string_view text) {
            for (const SvcHooks& h : svc_hooks_) {
                if (h.debug_string) {
                    h.debug_string(text);
                }
            }
        };
    }
    svc_->SetHooks(std::move(combined));
}

void Runtime::Own(std::shared_ptr<void> object) {
    owned_.push_back(std::move(object));
}

void Runtime::OnFinish(std::function<void()> fn) {
    finishers_.push_back(std::move(fn));
}

void Runtime::RunFinishers() {
    if (finished_) {
        return;
    }
    finished_ = true;
    while (!finishers_.empty()) {
        const auto fn = std::move(finishers_.back());
        finishers_.pop_back();
        fn();
    }
}

void Runtime::RequestExit(int status, const std::string& reason) {
    if (exit_status_) {
        return;
    }
    exit_status_ = status;
    MH4N_LOG(Info, kLog, "exit requested (status {}): {}", status, reason);
    if (scheduler_) {
        scheduler_->RequestStop();
    }
}

// ---- the host interfaces ---------------------------------------------------------------------

void Runtime::SetGpu(std::unique_ptr<gpu::Gpu> gpu) {
    owned_gpu_ = std::move(gpu);
    interfaces_.gpu = owned_gpu_.get();
}

void Runtime::SetInputSource(std::unique_ptr<InputSource> input) {
    owned_input_ = std::move(input);
    interfaces_.input = owned_input_.get();
}

void Runtime::SetAudioSink(std::unique_ptr<AudioSink> audio) {
    owned_audio_ = std::move(audio);
    interfaces_.audio = owned_audio_.get();
}

gpu::Host& Runtime::GpuHost() {
    MH4N_ASSERT(gpu_host_ != nullptr, "GpuHost before Prepare or Boot");
    return *gpu_host_;
}

void Runtime::SetGpuInterruptSink(GpuInterruptSink sink) {
    gpu_host_->SetInterruptSink(std::move(sink));
}

}  // namespace mh4n

namespace mh4n {

void Runtime::SetRefreshRateSink(RefreshRateSink sink) {
    refresh_sink_ = std::move(sink);
    const double hz = refresh_hz_.load();
    if (refresh_sink_ && hz > 0.0) {
        refresh_sink_(hz);
    }
}

void Runtime::SetRefreshRate(double hz) {
    refresh_hz_.store(hz);
    if (!kernel_) {
        return;
    }
    // The reference (the patch's GPU::SetRefreshRate) sets frame_ticks at once on the emulation
    // thread. Guest code runs on the scheduler thread (every fiber shares its host thread), so a call
    // from guest code is on the right thread and the sink can run now: the VBlank due next already
    // gets the new period. CodeHost::CurrentThread is a thread-local: it is null on any other host
    // thread (the platform's, a tool's) and on the scheduler's own host stack (timer events and posted
    // work), and those go through the kernel's queue, to the next safe point.
    if (CodeHost::CurrentThread() != nullptr) {
        refresh_inline_calls_.fetch_add(1);
        if (refresh_sink_) {
            refresh_sink_(hz);
        }
        return;
    }
    const u64 inline_calls = refresh_inline_calls_.load();
    kernel_->Post([this, hz, inline_calls] {
        // An inline call made since this one was posted is newer: the rate it set stands.
        if (refresh_sink_ && refresh_inline_calls_.load() == inline_calls) {
            refresh_sink_(hz);
        }
    });
}

double Runtime::RefreshRate() const {
    return refresh_hz_.load();
}

}  // namespace mh4n
