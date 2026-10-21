#include "ipc_replay/tool.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <ostream>
#include <system_error>

#include "ipc_replay/replay.h"
#include "ipc_replay/replay_clock.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

namespace mh4n::ipc_replay {

MH4N_OPTION("ipc-replay", "--help", "", "print this help and exit");
MH4N_OPTION("ipc-replay", "--svc-trace", "FILE",
            "the SVC trace of the same run: the objects the requests carry are made from its creating calls");
MH4N_OPTION("ipc-replay", "--service", "NAME",
            "replay only this service and the sessions it hands out (repeatable); default: every service "
            "registered natively");
MH4N_OPTION("ipc-replay", "--continue", "", "report every difference, not only the first of each service");
MH4N_OPTION("ipc-replay", "--game", "DIR",
            "the game folder the services read (their ServiceContext::GameFolder)");
MH4N_OPTION("ipc-replay", "--user", "DIR", "the user directory the services read and write");
MH4N_OPTION("ipc-replay", "--reply-timeout", "MS",
            "how long to wait for a deferred reply before calling it a difference (default 30000)");
MH4N_OPTION("ipc-replay", "--no-time", "",
            "do not advance game time to each record's tick before its request (default: advance, "
            "running the timers that fall due on the way); the clock is then the host's");
MH4N_OPTION("ipc-replay", "--keep-buffers", "",
            "do not write zeros into a write-only buffer before each request: it keeps what the requests "
            "before it left (the trace has no bytes for such a buffer); the default is zeros");
MH4N_OPTION("ipc-replay", "--time-dependent", "FILE",
            "commands whose replies depend on time, compared by result only (repeatable; "
            "config/native/ipc_time_dependent.txt is read first when it exists)");

namespace {

constexpr const char* kDefaultTimeDependent = "config/native/ipc_time_dependent.txt";

std::string UsageText() {
    return "usage: mh4n_ipc_replay TRACE [options]\n"
           "TRACE is the output of --trace-ipc-full. options:\n" +
           Usage("ipc-replay");
}

}  // namespace

int RunIpcReplay(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const Options options = ParseOptions("ipc-replay", args);
    if (!options.ok()) {
        err << "error: " << options.error() << "\n" << UsageText();
        return 2;
    }
    if (options.Has("--help")) {
        out << UsageText();
        return 0;
    }
    if (options.positional().size() != 1) {
        err << "error: expected one TRACE, got " << options.positional().size() << "\n" << UsageText();
        return 2;
    }

    std::string error;
    IpcTrace trace;
    if (!LoadIpcTrace(options.positional()[0], &trace, &error)) {
        err << "error: " << error << "\n";
        return 2;
    }
    if (trace.records.empty()) {
        err << "error: " << options.positional()[0] << ": no request in the trace\n";
        return 2;
    }
    SvcTrace svc;
    const std::optional<std::string> svc_path = options.Get("--svc-trace");
    if (svc_path && !LoadSvcTrace(*svc_path, &svc, &error)) {
        err << "error: " << error << "\n";
        return 2;
    }

    ReplayConfig config;
    config.services = options.All("--service");
    config.keep_going = options.Has("--continue");
    config.advance_time = !options.Has("--no-time");
    config.keep_output_buffers = options.Has("--keep-buffers");
    try {
        const u64 timeout = options.GetUInt("--reply-timeout", 30000);
        if (timeout == 0 || timeout > 3600000) {
            err << "error: --reply-timeout takes 1 to 3600000 ms\n";
            return 2;
        }
        config.reply_timeout = std::chrono::milliseconds(timeout);
    } catch (const OptionError& e) {
        err << "error: " << e.what() << "\n";
        return 2;
    }
    // The error_code overloads: a path that cannot be examined (a symbolic link loop, a directory the
    // user may not enter) is an exit 2, not an exception.
    std::error_code status_error;
    const bool have_default = std::filesystem::exists(kDefaultTimeDependent, status_error);
    if (status_error) {
        err << "error: cannot examine " << kDefaultTimeDependent << ": " << status_error.message() << "\n";
        return 2;
    }
    if (have_default && !config.time_dependent.LoadFile(kDefaultTimeDependent, &error)) {
        err << "error: " << error << "\n";
        return 2;
    }
    for (const std::string& path : options.All("--time-dependent")) {
        if (!config.time_dependent.LoadFile(path, &error)) {
            err << "error: " << error << "\n";
            return 2;
        }
    }
    const std::string game = options.GetString("--game");
    if (!game.empty()) {
        const bool is_directory = std::filesystem::is_directory(game, status_error);
        const bool absent = status_error == std::errc::no_such_file_or_directory ||
                            status_error == std::errc::not_a_directory;
        if (status_error && !absent) {
            err << "error: cannot examine --game " << game << ": " << status_error.message() << "\n";
            return 2;
        }
        if (!is_directory) {
            err << "error: --game " << game << " is not a directory\n";
            return 2;
        }
    }

    // The default clock is the host's; to move game time the replay makes the runtime with its own.
    Runtime runtime(config.advance_time ? MakeReplayRuntimeConfig() : RuntimeConfig{});
    if (!runtime.Prepare(options, {Phase::Devices, Phase::Services}, &error)) {
        err << "error: " << error << "\n";
        return 2;
    }
    ServiceManager* manager = FindServiceManager(runtime);
    if (manager == nullptr) {
        err << "error: the services boot step did not run\n";
        return 2;
    }
    if (!game.empty()) {
        // Prepare gives the services no game folder, so the services run in a second manager that has
        // one; the SVC layer's handlers now point at it.
        ServiceEnvironment env;
        env.game_folder = game;
        env.user_folder = runtime.UserFolder();
        env.options = &runtime.GetOptions();
        env.kernel = &runtime.GetKernel();
        env.scheduler = &runtime.Scheduler();
        env.threads = &runtime.Threads();
        env.interfaces = &runtime.Interfaces();
        env.request_exit = [&runtime](int status, const std::string& reason) {
            runtime.RequestExit(status, reason);
        };
        auto with_game = std::make_shared<ServiceManager>(std::move(env));
        with_game->InstallRegistered();
        with_game->Install(runtime.Svc());
        runtime.Own(with_game);
        manager = with_game.get();
    }

    const ReplayReport report = Replay(runtime, *manager, trace, svc_path ? &svc : nullptr, config);
    if (!report.error.empty()) {
        err << FormatReport(report);
    } else {
        out << FormatReport(report);
        if (report.Replayed() == 0) {
            err << "error: no request was replayed\n";
        }
    }
    return report.ExitStatus();
}

}  // namespace mh4n::ipc_replay
