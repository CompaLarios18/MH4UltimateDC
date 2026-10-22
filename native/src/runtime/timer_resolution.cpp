// See timer_resolution.h. Our own code.
#include "timer_resolution.h"

#include <atomic>
#include <mutex>

#include "mh4n/log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mh4n::detail {

namespace {
// The scopes alive (read without the lock by live_count) and, on Windows, the opt-out they hold.
// (The mutex is used on Windows only: elsewhere a scope just counts, and -Werror must not see it unused.)
[[maybe_unused]] std::mutex g_mutex;
std::atomic<int> g_live{0};
}  // namespace

int TimerResolutionScope::live_count() {
    return g_live.load();
}

#if defined(_WIN32)

namespace {
using TimePeriodFn = UINT(WINAPI*)(UINT);

// winmm.dll is loaded when it is needed, so that the module links without it.
struct WinMm {
    HMODULE module = nullptr;
    TimePeriodFn begin = nullptr;
    TimePeriodFn end = nullptr;
    WinMm() {
        module = LoadLibraryW(L"winmm.dll");
        if (module != nullptr) {
            begin = reinterpret_cast<TimePeriodFn>(reinterpret_cast<void*>(GetProcAddress(module, "timeBeginPeriod")));
            end = reinterpret_cast<TimePeriodFn>(reinterpret_cast<void*>(GetProcAddress(module, "timeEndPeriod")));
        }
    }
};
WinMm& Mm() {
    static WinMm mm;
    return mm;
}
constexpr UINT kTimeOk = 0;  // TIMERR_NOERROR

// Windows 11 ignores a process's timer-resolution request (and may run it on efficiency cores) when it
// counts the process as background: one with no visible window, which a headless run always is. Asking
// not to be throttled makes the request count (seen by N1-21 on one PC: the same G2 run had about 1.0
// VBlank per frame started from a console and about 1.3 started hidden). The structure and the flags are
// those of PROCESS_POWER_THROTTLING_STATE; kernel32 is asked at run time, so older systems just do
// without.
struct PowerThrottlingState {
    ULONG version;
    ULONG control_mask;
    ULONG state_mask;
};
constexpr ULONG kPowerThrottlingVersion = 1;
constexpr ULONG kThrottleExecutionSpeed = 0x1;
constexpr ULONG kThrottleIgnoreTimerResolution = 0x4;
constexpr int kProcessPowerThrottling = 4;  // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling

using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using GetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

struct ProcessInformationApi {
    SetProcessInformationFn set = nullptr;
    GetProcessInformationFn get = nullptr;
    ProcessInformationApi() {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel != nullptr) {
            set = reinterpret_cast<SetProcessInformationFn>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "SetProcessInformation")));
            get = reinterpret_cast<GetProcessInformationFn>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "GetProcessInformation")));
        }
    }
};
const ProcessInformationApi& ProcessApi() {
    static const ProcessInformationApi api;
    return api;
}

// The opt-out the live scopes hold (under g_mutex): the flags in force and the state to give back.
unsigned g_opt_out_flags = 0;
PowerThrottlingState g_original{kPowerThrottlingVersion, 0, 0};

// Called by the first scope. A control bit with a clear state bit turns that kind of throttling off.
// The request with both flags is tried first; a Windows 10 build that knows EcoQoS but not
// IGNORE_TIMER_RESOLUTION refuses it, and then EXECUTION_SPEED alone is asked for.
void OptOutOfPowerThrottling() {
    const ProcessInformationApi& api = ProcessApi();
    g_opt_out_flags = 0;
    if (api.set == nullptr) {
        MH4N_LOG(Info, "Runtime", "power-throttling opt-out not available on this Windows");
        return;
    }
    if (api.get != nullptr) {
        PowerThrottlingState before{kPowerThrottlingVersion, 0, 0};
        if (api.get(GetCurrentProcess(), kProcessPowerThrottling, &before, sizeof(before)) != 0) {
            g_original = before;
            g_original.version = kPowerThrottlingVersion;
        }
    }
    constexpr ULONG kBoth = kThrottleExecutionSpeed | kThrottleIgnoreTimerResolution;
    PowerThrottlingState state{kPowerThrottlingVersion, kBoth, 0};
    if (api.set(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state)) != 0) {
        g_opt_out_flags = kBoth;
        MH4N_LOG(Info, "Runtime", "power-throttling opt-out applied (execution speed and timer resolution)");
        return;
    }
    const DWORD both_error = GetLastError();
    state = PowerThrottlingState{kPowerThrottlingVersion, kThrottleExecutionSpeed, 0};
    if (api.set(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state)) != 0) {
        g_opt_out_flags = kThrottleExecutionSpeed;
        MH4N_LOG(Info,
                 "Runtime",
                 "power-throttling opt-out applied for execution speed only (the timer-resolution flag was refused, error {})",
                 static_cast<unsigned>(both_error));
        return;
    }
    MH4N_LOG(Warning,
             "Runtime",
             "the host refused the power-throttling opt-out (error {}): a hidden window may make it ignore the 1 ms tick",
             static_cast<unsigned>(GetLastError()));
}

// Called by the last scope: the system manages the process again, as it did before the first one.
void RestorePowerThrottling() {
    if (g_opt_out_flags == 0) {
        return;
    }
    const ProcessInformationApi& api = ProcessApi();
    PowerThrottlingState state = g_original;
    state.version = kPowerThrottlingVersion;
    if (api.set != nullptr) {
        api.set(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state));
    }
    g_opt_out_flags = 0;
}

const char* DescribeOptOut(unsigned flags) {
    switch (flags) {
    case kThrottleExecutionSpeed | kThrottleIgnoreTimerResolution:
        return "applied (execution speed and timer resolution)";
    case kThrottleExecutionSpeed:
        return "applied for execution speed only";
    default:
        return "not applied";
    }
}
}  // namespace

unsigned TimerResolutionScope::throttling_opt_out_flags() {
    std::lock_guard lock(g_mutex);
    return g_opt_out_flags;
}

TimerResolutionScope::TimerResolutionScope(unsigned milliseconds) : milliseconds_(milliseconds) {
    std::lock_guard lock(g_mutex);
    if (g_live.fetch_add(1) == 0) {
        OptOutOfPowerThrottling();
    }
    WinMm& mm = Mm();
    if (mm.begin != nullptr && mm.end != nullptr && mm.begin(milliseconds_) == kTimeOk) {
        active_ = true;
        MH4N_LOG(Info, "Runtime", "host timer tick requested: {} ms (power-throttling opt-out {})", milliseconds_,
                 DescribeOptOut(g_opt_out_flags));
    } else {
        MH4N_LOG(Warning, "Runtime",
                 "the host refused a timer tick of {} ms: sleeps and VBlanks may be up to 15 ms late", milliseconds_);
    }
}

TimerResolutionScope::~TimerResolutionScope() {
    std::lock_guard lock(g_mutex);
    if (active_) {
        Mm().end(milliseconds_);
    }
    if (g_live.fetch_sub(1) == 1) {
        RestorePowerThrottling();
    }
}

#else

unsigned TimerResolutionScope::throttling_opt_out_flags() {
    return 0;
}

TimerResolutionScope::TimerResolutionScope(unsigned milliseconds) : milliseconds_(milliseconds) {
    ++g_live;
}
TimerResolutionScope::~TimerResolutionScope() {
    --g_live;
}

#endif

}  // namespace mh4n::detail
