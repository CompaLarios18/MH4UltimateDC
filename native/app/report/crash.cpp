// The platform-independent part of the crash file (N3-13): the values the app publishes, the
// build's commit, and Install's bookkeeping outside the handler (where allocation is fine).
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

#include "../../src/common/utf8.h"
#include "mh4n_version.h"
#include "crash_internal.h"
#include "report.h"

namespace mh4n::app::report {
namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the crash handler reads these two from a signal handler");

std::atomic<std::uint64_t> g_frame{detail::kUnsetValue};
std::atomic<std::uint64_t> g_scene{detail::kUnsetValue};

std::mutex g_install_mutex;
std::string g_crash_path;  // guarded by g_install_mutex; never read by the handler

// Runs at an orderly exit: a crash file the handler never wrote is not left behind.
void RemoveUnusedCrashFile() {
    std::lock_guard lock(g_install_mutex);
    if (g_crash_path.empty() || !detail::PlatformUnused()) {
        return;
    }
    detail::PlatformClose();
    std::error_code ec;
    std::filesystem::remove(utf8::PathFromUtf8(g_crash_path), ec);
}

}  // namespace

namespace detail {

void AppendPreamble(Text& text) {
    text.Str("mh4u-native crash v1\ncommit: ");
    text.Str(MH4N_GIT_REVISION);
    text.Put('\n');
}

void AppendGameState(Text& text) {
    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    const std::uint64_t scene = g_scene.load(std::memory_order_relaxed);
    text.Str("game frame: ");
    if (frame == kUnsetValue) {
        text.Str("unknown");
    } else {
        text.Dec(frame);
    }
    text.Str("\nscene: ");
    if (scene == kUnsetValue) {
        text.Str("unknown");
    } else {
        text.Dec(scene);
    }
    text.Put('\n');
}

}  // namespace detail

void SetGameState(std::uint64_t frame, std::uint64_t scene) {
    g_frame.store(frame, std::memory_order_relaxed);
    g_scene.store(scene, std::memory_order_relaxed);
}

void Install(const std::string& user_dir) {
    namespace fs = std::filesystem;
    if (user_dir.empty()) {
        return;  // no user folder (no --user): nowhere to write, and the working folder is not ours
    }
    std::lock_guard lock(g_install_mutex);

    std::string folder = user_dir;
    if (!folder.empty() && folder.back() != '/' && folder.back() != '\\') {
        folder += '/';
    }
    folder += "log";
    std::error_code ec;
    fs::create_directories(utf8::PathFromUtf8(folder), ec);
    const std::string path = folder + "/crash-" + std::to_string(detail::ProcessId()) + ".txt";

    // an earlier call's file, if the handler never wrote to it, moves with us
    const std::string previous = g_crash_path;
    const bool previous_unused = detail::PlatformUnused();
    if (!detail::PlatformInstall(path)) {
        g_crash_path.clear();
        return;
    }
    g_crash_path = path;
    if (!previous.empty() && previous != path && previous_unused) {
        fs::remove(utf8::PathFromUtf8(previous), ec);
    }
    static std::once_flag exit_hook;
    std::call_once(exit_hook, [] { std::atexit(RemoveUnusedCrashFile); });
}

}  // namespace mh4n::app::report
