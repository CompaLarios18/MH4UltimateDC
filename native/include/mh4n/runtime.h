// The runtime (N1-09): guest memory, the kernel, the code host, the scheduler, the SVC table, the
// loaded program and the host interfaces, owned in one place and booted in a fixed order. The
// native runner (native/runner) is a thin command line around it; the replay tool (N1-11) uses
// Prepare. Later modules add their devices, services, traces and options to it without editing it:
//
//   - options:     MH4N_OPTION("runner", "--name", "ARG", "help"), read from `rt.GetOptions()`;
//   - boot steps:  MH4N_REGISTER(BootStep, "my.step", BootStep{Phase::Services, [](Runtime& rt) {...}});
//   - devices:     MH4N_REGISTER(gpu::GpuFactory / InputSourceFactory / AudioSinkFactory, "name", ...);
//   - frames:      rt.OnFrame([](u32 frame, u64 tick) {...});
//   - SVC traces:  rt.AddSvcHooks({...});
//   - state:       rt.Own(std::make_shared<MyState>()) keeps an object for the runtime's lifetime.
//
// Boot order (Boot):
//   1. the program is loaded (Loader::OpenGameFolder or OpenElf);
//   2. memory, the game clock (paused: game time starts when the scheduler does), the code host,
//      the kernel, the scheduler, the thread manager and the SVC table are made, and the process;
//   3. the segments, the configuration memory and the shared page are mapped (Loader::Map,
//      SystemPages::Map);
//   4. the code library is loaded and checked against the text segment (--code; none with a main
//      thread entry set by SetMainThreadEntry);
//   5. the boot steps of the phases Devices, Services and Tracing run, in phase order and then by
//      name;
//   6. the main thread is created (ThreadManager::CreateMainThread);
//   7. the steps of the phase Started run (the main thread exists and has not run);
//   8. the clock starts and the scheduler runs until the program exits or a limit is reached.
//
// Exit statuses of Boot (as mh4u_runner's): 0 when the program exits or a --ms / --frames limit is
// reached; 2 when loading fails (a missing file, a code library that does not match, a bad option);
// 3 on a runtime error (svcBreak, a fatal error through err:f; a bad memory access ends the
// process with 3 by itself, memory.h); 4 when the recompiled code stops (code that was not
// recompiled, a bad return, a trap).
//
// Threads (principle 6): everything here runs on the host thread that calls Boot or Prepare, except
// the functions documented as callable from any thread (gpu::Host's SignalInterrupt and Post,
// SetRefreshRate).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/cpu.h"
#include "mh4n/gpu.h"
#include "mh4n/host.h"
#include "mh4n/kernel.h"
#include "mh4n/loader.h"
#include "mh4n/memory.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/scheduler.h"
#include "mh4n/svc.h"
#include "mh4n/threads.h"
#include "mh4n/types.h"

namespace mh4n {

// The game's frame counter (a u32 in the game's memory): the frame watcher reads it at every safe
// point of the scheduler. Programs that do not map this address have no frames.
inline constexpr u32 kFrameCounterAddress = 0x010571d4;

// The memory mode Prepare uses unless it is given one: Dev1, the mode of ELF test programs.
inline constexpr u32 kPrepareMemoryMode = 2;

// When a boot step runs (docs/native/wp/N1.md, N1-09).
enum class Phase {
    Devices,   // the host interfaces: GPU, input source, audio sink
    Services,  // ports and services (N1-10 on)
    Tracing,   // traces and captures
    Started,   // after the main thread exists, before it runs
};
const char* PhaseName(Phase phase);

class Runtime;
namespace detail {
class RuntimeGpuHost;  // runtime/gpu_host.h
}

// Registered by name: MH4N_REGISTER(BootStep, "services.srv", BootStep{Phase::Services, ...}). The
// steps of a phase run in name order.
struct BootStep {
    Phase phase;
    std::function<void(Runtime&)> run;
};

// Called by the frame watcher on the scheduler thread when the frame counter changes: the new value
// and the game clock's ticks. The first reading counts as a change.
using FrameCallback = std::function<void(u32 frame, u64 tick)>;
using GpuInterruptSink = std::function<void(gpu::Interrupt)>;
using RefreshRateSink = std::function<void(double hz)>;

struct RuntimeConfig {
    // The host's time. Default: SteadyTimeSource (and, with it, the timer thread as the source of
    // preemption). Tests pass a ManualTimeSource.
    std::unique_ptr<TimeSource> time;
    // Default: MakeTimerThreadPreemption(time) for the default time source, none otherwise.
    std::unique_ptr<PreemptionSource> preemption;
};

class Runtime {
public:
    explicit Runtime(RuntimeConfig config = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // ---- starting ------------------------------------------------------------------------------

    // Guest memory, the kernel and the process, and only the boot steps of `phases` (in phase
    // order, then by name), with no program and no code library: what the replay tool needs
    // (N1-11). The process is made with Kernel::NewProcess("prepared"); the clock runs from now.
    // `memory_mode` is FcramRegions' mode. False, and a message in *error, if the host refuses the
    // address space. Call it, or Boot, once.
    bool Prepare(const Options& options, const std::vector<Phase>& phases, std::string* error = nullptr,
                 u32 memory_mode = kPrepareMemoryMode);

    // Everything in the order above. `options` is the runner's: positional()[0] is PROGRAM (a game
    // folder or an ELF), --code the code library, and the rest as docs/native/runner.md lists. Logs
    // what goes wrong and returns the exit status.
    int Boot(const Options& options);

    // For tests and tools: the main thread runs this C++ function instead of the guest code, and
    // Boot needs no code library. It must end the thread (GuestScheduler::ExitCurrent) or the
    // process (svcExitProcess). Call before Boot.
    void SetMainThreadEntry(GuestThread::Entry entry);

    // ---- what the runtime owns (valid after Prepare or Boot) -------------------------------------

    const Options& GetOptions() const { return options_; }
    GuestMemory& Memory() { return *memory_; }
    Kernel& GetKernel() { return *kernel_; }
    GameClock& Clock() { return *clock_; }
    CodeHost& GetCodeHost() { return *code_host_; }
    GuestScheduler& Scheduler() { return *scheduler_; }
    ThreadManager& Threads() { return *threads_; }
    SvcTable& Svc() { return *svc_; }
    // Null after Prepare.
    Loader* GetLoader() { return loader_.get(); }
    const ProcessImage* Image() const { return loader_ ? &loader_->Image() : nullptr; }
    // Null before Boot has mapped the pages.
    SystemPages* Pages() { return pages_.get(); }
    // The main thread, once it exists.
    std::shared_ptr<Thread> MainThread() const { return main_thread_; }
    // PROGRAM as given; the game folder (empty for an ELF program) and --user (empty if not given).
    const std::string& ProgramPath() const { return program_path_; }
    const std::string& GameFolder() const { return game_folder_; }
    const std::string& UserFolder() const { return user_folder_; }
    // --init-time (default kDefaultInitTime), seconds since the Unix epoch.
    u64 InitTime() const { return init_time_; }

    // ---- the host interfaces ---------------------------------------------------------------------

    // The pointers are filled by the `devices` step (or by a test, directly). The runtime owns what
    // the factories made.
    HostInterfaces& Interfaces() { return interfaces_; }
    // The runtime takes ownership and updates Interfaces(); an empty pointer clears the interface.
    void SetGpu(std::unique_ptr<gpu::Gpu> gpu);
    void SetInputSource(std::unique_ptr<InputSource> input);
    void SetAudioSink(std::unique_ptr<AudioSink> audio);
    // The runtime's side of the GPU contract: the physical view of guest memory, interrupts and
    // Post through the kernel. Valid after Prepare or Boot.
    gpu::Host& GpuHost();
    // GSP sets the sink (N1-17); the host's gpu::Host posts each interrupt to the kernel and calls
    // it on the scheduler thread, after `delay_ns` of game time.
    void SetGpuInterruptSink(GpuInterruptSink sink);
    // GSP sets the sink (N1-17); the platform calls SetRefreshRate (N1-20) from any thread, and
    // the sink runs on the scheduler thread. Setting a sink when a rate is already known calls it
    // at once.
    void SetRefreshRateSink(RefreshRateSink sink);
    void SetRefreshRate(double hz);
    double RefreshRate() const;

    // ---- extension points --------------------------------------------------------------------------

    // Subscribers of the frame watcher.
    void OnFrame(FrameCallback callback);
    // One look at the frame counter (the frame watcher does it at every safe point; tests and tools
    // can do it by hand). Calls the subscribers if the counter changed since the last look, and
    // stops the run when it reaches --frames.
    void PollFrame();
    // Adds trace hooks; every subscriber is called, in the order they were added. May be called
    // before Boot. (The runtime's own --trace-svc is one of them.)
    void AddSvcHooks(SvcHooks hooks);
    // Keeps `object` alive until the runtime is destroyed; destroyed in reverse order, before the
    // kernel, so it may hold references to the runtime's parts.
    void Own(std::shared_ptr<void> object);
    // `fn` runs once when Boot's run is over (before anything is destroyed), in reverse order of
    // registration: the place to finish and close a trace.
    void OnFinish(std::function<void()> fn);
    // Ends the run with `status` (the first request wins) and asks the scheduler to stop. Callable
    // from the scheduler thread; `reason` is logged. A boot step that cannot do its job (a trace
    // file that cannot be written) calls it with 2: Boot returns that status before the program
    // runs. (svcExitProcess and svcBreak end the run through the SVC table, not through this.)
    void RequestExit(int status, const std::string& reason);
    std::optional<int> ExitStatus() const { return exit_status_; }

private:
    bool CreateWorld(u32 memory_mode, std::string* error);
    void RunPhases(const std::vector<Phase>& phases);
    void InstallSvcHooks();
    void RunFinishers();
    void ScheduleSharedPageUpdate(u64 due_tick);
    int Run(const Options& options);

    std::unique_ptr<TimeSource> time_;
    std::unique_ptr<PreemptionSource> preemption_;
    Options options_;
    std::string program_path_, game_folder_, user_folder_;
    u64 init_time_ = kDefaultInitTime;
    GuestThread::Entry main_entry_;

    std::unique_ptr<Loader> loader_;
    std::unique_ptr<GuestMemory> memory_;
    std::unique_ptr<GameClock> clock_;
    std::unique_ptr<CodeHost> code_host_;
    std::unique_ptr<Kernel> kernel_;
    std::unique_ptr<GuestScheduler> scheduler_;
    std::unique_ptr<ThreadManager> threads_;
    std::unique_ptr<SvcTable> svc_;
    std::unique_ptr<SystemPages> pages_;
    std::shared_ptr<Thread> main_thread_;
    std::unique_ptr<detail::RuntimeGpuHost> gpu_host_;

    HostInterfaces interfaces_;
    std::unique_ptr<gpu::Gpu> owned_gpu_;
    std::unique_ptr<InputSource> owned_input_;
    std::unique_ptr<AudioSink> owned_audio_;

    std::vector<FrameCallback> frame_callbacks_;
    bool have_frame_ = false;
    u32 last_frame_ = 0;
    std::optional<u32> stop_frame_;
    std::vector<SvcHooks> svc_hooks_;
    std::vector<std::shared_ptr<void>> owned_;
    std::vector<std::function<void()>> finishers_;
    std::optional<int> exit_status_;
    bool prepared_ = false;
    bool finished_ = false;
    std::string limit_reason_;  // "frame limit reached" or "time limit reached"
    std::atomic<double> refresh_hz_{0.0};
    RefreshRateSink refresh_sink_;
};

}  // namespace mh4n
