// The hooks' platform on the native runtime (N1-20). See the header next to this file.
//
// The Azahar build's platform (platform_azahar.cpp of the port) is the model for what each method
// answers; the pinned Azahar sources were read for the two things it takes from the emulator:
// GameSeconds is Timing::GetGlobalTicks() over BASE_CLOCK_RATE_ARM11 (core_timing.cpp: the ticks the
// emulated CPU has run, so it stops with the emulation), which here is the game clock's reading over
// the same rate; IsRunning is System::IsPoweredOn().
#include "platform/platform_native.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include "../common/utf8.h"
#include "hooks.h"
#include "mh4n/clock.h"
#include "mh4n/cpu.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

extern "C" {
#include "recomp.h"
}

// Last, so that the names windows.h defines as macros reach none of the headers above.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mh4n::platform {

namespace {

constexpr const char* kLog = "Platform";

MH4N_OPTION("runner", "--set", "MS|fN:KEY=VALUE",
            "at that game time (ms) or game frame (fN), change one of the hooks' options that can change "
            "while the game runs, e.g. 0:fps=120 or f600:fps=60 (repeatable)");
MH4N_OPTION("runner", "--options", "FILE",
            "the host's own settings in the layout of mh4u-app.ini ([Section] lines, key = value lines); "
            "aspect_ratio is the one the hooks read and set");

// Names of the aspect ratios the hooks may set (the port's Settings::AspectRatio names, as
// platform_azahar.cpp lists them).
constexpr const char* kAspectRatios[] = {"default", "16:9", "4:3", "21:9", "16:10", "stretch"};

bool KnownAspectRatio(const std::string& value) {
    return std::find(std::begin(kAspectRatios), std::end(kAspectRatios), value) != std::end(kAspectRatios);
}

std::string Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

bool IsAscii(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

#if defined(_WIN32)
// GetShortPathNameW for a UTF-8 folder; nothing when Windows has no short name for it (8.3 names can be
// turned off for a volume, and the folder must exist).
std::optional<std::string> WindowsShortPath(const std::string& folder) {
    std::wstring wide = utf8::ToWide(folder);
    for (wchar_t& c : wide) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    const DWORD needed = GetShortPathNameW(wide.c_str(), nullptr, 0);  // with the terminating null
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written = GetShortPathNameW(wide.c_str(), buffer.data(), needed);
    if (written == 0 || written >= needed) {
        return std::nullopt;  // failed, or the name grew between the two calls
    }
    buffer.resize(written);
    return utf8::FromWide(buffer);
}
#endif

// Platforms by runtime, for NativePlatform::Find.
std::mutex g_registry_mutex;
std::map<Runtime*, NativePlatform*>& Registry() {
    static std::map<Runtime*, NativePlatform*> registry;
    return registry;
}

}  // namespace

ShortPathFunction HostShortPathFunction() {
#if defined(_WIN32)
    return WindowsShortPath;
#else
    return {};
#endif
}

std::string ResolveUserDir(const std::string& user_dir, const ShortPathFunction& short_path,
                           std::string* warning) {
    if (user_dir.empty() || IsAscii(user_dir) || !short_path) {
        return user_dir;
    }
    // The call takes the folder without its trailing separator (as a name, not as a directory entry).
    std::string folder = user_dir;
    std::string separator;
    while (folder.size() > 1 && (folder.back() == '/' || folder.back() == '\\')) {
        separator = std::string(1, folder.back());
        folder.pop_back();
    }
    if (separator.empty()) {
        separator = "/";
    }
    const std::optional<std::string> short_name = short_path(folder);
    if (short_name && !short_name->empty() && IsAscii(*short_name)) {
        std::string result = *short_name;
        while (result.size() > 1 && (result.back() == '/' || result.back() == '\\')) {
            result.pop_back();
        }
        return result + separator;
    }
    if (warning != nullptr) {
        *warning = "the --user folder " + folder +
                   " has a character outside ASCII in its name and Windows has no short (8.3) name for it "
                   "(short names can be turned off for a volume, and the folder must exist): the hooks open "
                   "mh4u.ini through an ANSI path and cannot read it from there, so they run on their "
                   "defaults; use a --user folder whose path is ASCII, or turn on short names for the "
                   "volume (fsutil 8dot3name)";
    }
    return kNoUserDir;
}

bool ParseAppOptions(std::string_view text, std::map<std::string, std::string>* options,
                     std::string* error) {
    std::map<std::string, std::string> parsed;
    std::size_t line_number = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++line_number;
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            continue;  // a section header: the keys are looked up without it
        }
        const auto eq = line.find('=');
        const std::string key = eq == std::string::npos ? std::string() : Trim(line.substr(0, eq));
        if (key.empty()) {
            *error = "line " + std::to_string(line_number) + ": expected key = value";
            return false;
        }
        parsed[key] = Trim(line.substr(eq + 1));
    }
    *options = std::move(parsed);
    return true;
}

// ---- NativePlatform ----------------------------------------------------------------------------------

NativePlatform::NativePlatform(Runtime& runtime, std::string user_dir,
                               std::map<std::string, std::string> app_options, ShortPathFunction short_path)
    : runtime_(runtime), user_dir_(std::move(user_dir)), app_options_(std::move(app_options)) {
    const auto it = app_options_.find("aspect_ratio");
    if (it != app_options_.end() && KnownAspectRatio(it->second)) {
        aspect_ratio_ = it->second;
    }
    if (user_dir_.empty()) {
        // The hooks would open ./mh4u.ini: a stray file in the current directory would silently change
        // fps and widescreen. Hand them a folder that holds nothing (D44).
        user_dir_ = kNoUserDir;
        MH4N_LOG(Info, kLog, "no --user folder: the hooks use their defaults and read no mh4u.ini");
    } else {
        std::string warning;
        user_dir_ = ResolveUserDir(user_dir_, short_path, &warning);
        if (!warning.empty()) {
            MH4N_LOG(Warning, kLog, "{}", warning);
        }
    }
    std::lock_guard lock(g_registry_mutex);
    Registry()[&runtime_] = this;
}

NativePlatform::~NativePlatform() {
    {
        std::lock_guard lock(g_registry_mutex);
        auto& registry = Registry();
        const auto it = registry.find(&runtime_);
        if (it != registry.end() && it->second == this) {
            registry.erase(it);
        }
    }
    // The hooks keep the pointer they were given: back to their null platform.
    Port::Hooks::SetPlatform(nullptr);
}

void NativePlatform::Log(Port::LogLevel level, const std::string& message) {
    // The category is the Azahar build's (LOG_*(Frontend, ...)).
    switch (level) {
    case Port::LogLevel::Debug:
        MH4N_LOG(Debug, "Frontend", "{}", message);
        break;
    case Port::LogLevel::Info:
        MH4N_LOG(Info, "Frontend", "{}", message);
        break;
    case Port::LogLevel::Warning:
        MH4N_LOG(Warning, "Frontend", "{}", message);
        break;
    case Port::LogLevel::Error:
        MH4N_LOG(Error, "Frontend", "{}", message);
        break;
    }
}

void NativePlatform::SetRefreshRate(double hz) {
    {
        std::lock_guard lock(mutex_);
        refresh_hz_ = hz;
    }
    // 0 is the default (59.83 Hz); the GSP's sink turns the rate into the VBlank period.
    runtime_.SetRefreshRate(hz);
}

void NativePlatform::SetTopScreenRows(std::uint32_t rows) {
    std::lock_guard lock(mutex_);
    top_rows_ = rows;
}

double NativePlatform::GameSeconds() {
    // The game clock, not the host's: it stands still while the clock is paused and counts the
    // game's ticks whatever the host's speed.
    return static_cast<double>(runtime_.Clock().Now()) / static_cast<double>(kTicksPerSecond);
}

bool NativePlatform::IsRunning() {
    return runtime_.MainThread() != nullptr && !runtime_.ExitStatus().has_value();
}

void NativePlatform::RefreshScreenLayout() {
    // Nothing to redo until the renderer (N2b) has a layout; the count is for it and the tests.
    std::lock_guard lock(mutex_);
    ++layout_refreshes_;
}

void NativePlatform::SetScreenOverlay(const Port::ScreenOverlay& overlay) {
    std::lock_guard lock(mutex_);
    overlay_ = overlay;
}

void NativePlatform::SetTopScreenWidth(std::uint32_t width) {
    std::lock_guard lock(mutex_);
    top_width_ = width;
}

bool NativePlatform::SetOption(const std::string& key, const std::string& value) {
    if (key != "aspect_ratio" || !KnownAspectRatio(value)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    aspect_ratio_ = value;
    return true;
}

std::string NativePlatform::GetOption(const std::string& key) {
    if (key != "aspect_ratio") {
        return {};
    }
    std::lock_guard lock(mutex_);
    return aspect_ratio_;
}

void NativePlatform::InstallNetSync(Port::NetSyncMode mode, double slack_ms, std::vector<std::string>& log) {
    // Local only until N4: there is no room to keep the clocks of other consoles together with, so
    // nothing is installed. The lines are those the Azahar build's Install writes for the same
    // options (the port's netsync.cpp), so that the hooks' log reads the same in both.
    constexpr double kDefaultSlackMs = 200.0;  // netsync.h
    if (mode == Port::NetSyncMode::Off) {
        log.push_back("net sync off");
        return;
    }
    if (mode == Port::NetSyncMode::All) {
        log.push_back("net sync with every port console in the room");
    }
    if (slack_ms > 0.0 && slack_ms != kDefaultSlackMs) {
        log.push_back("net sync slack " + [&] {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.0f", slack_ms);
            return std::string(buffer);
        }() + " ms");
    }
}

void NativePlatform::VoteEndSkip(bool vote) {
    std::lock_guard lock(mutex_);
    end_skip_vote_ = vote;
}

bool NativePlatform::EndSkipAgreed() {
    // No room, no peers: this console's say-so is everyone's (the hooks ask only after their own
    // vote; with peers they would wait for the others).
    return true;
}

std::uint32_t NativePlatform::TopScreenRows() const {
    std::lock_guard lock(mutex_);
    return top_rows_;
}

std::uint32_t NativePlatform::TopScreenWidth() const {
    std::lock_guard lock(mutex_);
    return top_width_;
}

Port::ScreenOverlay NativePlatform::Overlay() const {
    std::lock_guard lock(mutex_);
    return overlay_;
}

std::uint64_t NativePlatform::LayoutRefreshes() const {
    std::lock_guard lock(mutex_);
    return layout_refreshes_;
}

double NativePlatform::RequestedRefreshRate() const {
    std::lock_guard lock(mutex_);
    return refresh_hz_;
}

bool NativePlatform::EndSkipVote() const {
    std::lock_guard lock(mutex_);
    return end_skip_vote_;
}

std::map<std::string, std::string> NativePlatform::AppOptions() const {
    std::lock_guard lock(mutex_);
    return app_options_;
}

std::shared_ptr<NativePlatform> NativePlatform::Find(Runtime& runtime) {
    // The runtime owns the platform (Own); the registry only points at it.
    std::lock_guard lock(g_registry_mutex);
    const auto it = Registry().find(&runtime);
    if (it == Registry().end()) {
        return nullptr;
    }
    return it->second->weak_from_this().lock();
}

// ---- --set ---------------------------------------------------------------------------------------------

#if MH4N_HAVE_SCHEDULE

namespace {

struct PendingChange {
    Schedule::OptionChange change;
    bool done = false;
};

struct ChangeSet {
    Runtime* runtime = nullptr;
    u64 start_tick = 0;
    ApplyOption apply;
    std::vector<PendingChange> changes;

    void Run(PendingChange& pending) {
        if (pending.done) {
            return;
        }
        pending.done = true;
        const double ms = static_cast<double>(runtime->Clock().Now() - start_tick) /
                          (static_cast<double>(kTicksPerSecond) / 1000.0);
        if (!apply(pending.change.key, pending.change.value)) {
            const std::string message = "--set: " + pending.change.key +
                                        " is not an option that can change while the game runs";
            MH4N_LOG(Error, kLog, "{}", message);
            runtime->RequestExit(2, message);
            return;
        }
        MH4N_LOG(Info, kLog, "set {} = {} at {:.0f} ms", pending.change.key, pending.change.value, ms);
    }
};

}  // namespace

void ScheduleOptionChanges(Runtime& runtime, const std::vector<Schedule::OptionChange>& changes,
                           ApplyOption apply) {
    auto set = std::make_shared<ChangeSet>();
    set->runtime = &runtime;
    set->start_tick = runtime.Clock().Now();
    set->apply = std::move(apply);
    for (const Schedule::OptionChange& change : changes) {
        set->changes.push_back({change, false});
    }
    runtime.Own(set);

    bool any_frame = false;
    for (PendingChange& pending : set->changes) {
        if (pending.change.when.by_frame) {
            any_frame = true;
            continue;
        }
        // Due when the game time in ms has reached the change's: the first tick at or after it.
        const double ticks = std::ceil(pending.change.when.ms * (static_cast<double>(kTicksPerSecond) / 1000.0));
        if (!(ticks > 0.0)) {
            continue;  // 0 ms: applied below, before the program runs
        }
        const u64 due = ticks >= 1.0e18 ? u64{1000000000000000000} : static_cast<u64>(ticks);
        runtime.Scheduler().ScheduleAt(set->start_tick + due, [set, &pending] { set->Run(pending); });
    }
    if (any_frame) {
        runtime.OnFrame([set](u32 frame, u64) {
            for (PendingChange& pending : set->changes) {
                if (!pending.done && pending.change.when.by_frame && frame >= pending.change.when.frame) {
                    set->Run(pending);
                }
            }
        });
    }
    // The changes for time 0, in the order given, before anything runs.
    for (PendingChange& pending : set->changes) {
        if (!pending.change.when.by_frame && !(std::ceil(pending.change.when.ms) > 0.0)) {
            set->Run(pending);
        }
    }
}

#endif  // MH4N_HAVE_SCHEDULE

// ---- the started step ----------------------------------------------------------------------------------

std::shared_ptr<NativePlatform> InstallPlatform(Runtime& runtime) {
    const Options& options = runtime.GetOptions();

    std::map<std::string, std::string> app_options;
    if (const auto path = options.Get("--options")) {
        // The file of the player's command line (UTF-8 text, D45): opened through the wide-character path.
        std::ifstream file(utf8::PathFromUtf8(*path), std::ios::binary);
        std::string error;
        std::ostringstream text;
        if (!file) {
            error = "--options: cannot read " + *path;
        } else {
            text << file.rdbuf();
            if (!ParseAppOptions(text.str(), &app_options, &error)) {
                error = "--options " + *path + ": " + error;
            } else if (const auto it = app_options.find("aspect_ratio");
                       it != app_options.end() && !KnownAspectRatio(it->second)) {
                error = "--options " + *path + ": aspect_ratio " + it->second + " is not one of default, 16:9, 4:3, 21:9, 16:10, stretch";
            }
        }
        if (!error.empty()) {
            MH4N_LOG(Error, kLog, "{}", error);
            runtime.RequestExit(2, error);
            return nullptr;
        }
    }

    // The hooks open <UserDir()>mh4u.ini: the separator is part of what they get, as from Azahar.
    // Without --user it is empty, and NativePlatform turns that into kNoUserDir.
    std::string user_dir = runtime.UserFolder();
    if (!user_dir.empty() && user_dir.back() != '/' && user_dir.back() != '\\') {
        user_dir.push_back('/');
    }
    auto platform = std::make_shared<NativePlatform>(runtime, std::move(user_dir), std::move(app_options));
    runtime.Own(platform);
    Port::Hooks::SetPlatform(platform.get());

    return platform;
}

bool InstallOptionChanges(Runtime& runtime) {
    const Options& options = runtime.GetOptions();
#if MH4N_HAVE_SCHEDULE
    Schedule::Plan plan;
    for (const std::string& arg : options.All("--set")) {
        std::string error;
        if (!Schedule::ParseOptionChange(arg, plan, error)) {
            error = "--set " + arg + ": " + error;
            MH4N_LOG(Error, kLog, "{}", error);
            runtime.RequestExit(2, error);
            return false;
        }
    }
    if (!plan.option_changes.empty()) {
        ScheduleOptionChanges(runtime, plan.option_changes, [](const std::string& key, const std::string& value) {
            return Port::Hooks::SetOption(key, value);
        });
    }
#else
    if (options.Has("--set")) {
        const std::string message = "--set needs port/src/schedule.cpp, which is not around native/";
        MH4N_LOG(Error, kLog, "{}", message);
        runtime.RequestExit(2, message);
        return false;
    }
#endif
    return true;
}

void InstallHooks(const RCodeInfo& info) {
    for (const std::string& line : Port::Hooks::Install(info)) {
        // The Azahar build logs these as LOG_INFO(Core_ARM11, "hooks: {}", line).
        MH4N_LOG(Info, "Core.ARM11", "hooks: {}", line);
    }
}

namespace {

// Phase `started`: the code library is loaded and the main thread exists but has not run. As in the
// Azahar build (ARM_Recomp::LoadCode): the platform, then the hooks on the library. The --set changes
// come last: Hooks::Install reads its options afresh, so a change for 0 ms made before it is lost.
void StartedStep(Runtime& runtime) {
    if (!InstallPlatform(runtime)) {
        return;
    }
    if (const CodeLibrary* library = runtime.GetCodeHost().Library()) {
        InstallHooks(library->Info());
    }
    InstallOptionChanges(runtime);
}

MH4N_REGISTER(BootStep, "platform.hooks", BootStep{Phase::Started, StartedStep});

}  // namespace

}  // namespace mh4n::platform

// hooks.cpp calls recompiled game functions from its hooks (CallGame) through the runtime contract's
// rc_call, which the program that links the hooks provides: the Azahar build defines it in
// the port's arm_recomp.cpp. Here a guest function is called on the guest thread whose context the hook
// received (RCtx::user, set by the code host), exactly as the code library's own indirect calls are
// routed (CodeHostCallbacks::Call). A code library keeps its own rc_call hidden.
extern "C" void rc_call(RCtx* ctx, u32 target) {
    auto* thread = static_cast<mh4n::GuestThread*>(ctx->user);
    thread->Host().Call(*thread, target);
}
