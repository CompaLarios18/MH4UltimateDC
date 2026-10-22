// See timer_resolution.h. Our own code.
#include "timer_resolution.h"

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
// not to be throttled makes the request count (found by N1-21: the same G2 run had 1.0 VBlank per frame
// started from a console and 1.29 started hidden). The structure and the flags are those of
// PROCESS_POWER_THROTTLING_STATE; kernel32 is asked at run time, so older systems just do without.
struct PowerThrottlingState {
    ULONG version;
    ULONG control_mask;
    ULONG state_mask;
};
constexpr ULONG kPowerThrottlingVersion = 1;
constexpr ULONG kThrottleExecutionSpeed = 0x1;
constexpr ULONG kThrottleIgnoreTimerResolution = 0x4;
constexpr int kProcessPowerThrottling = 4;  // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling

void OptOutOfPowerThrottling() {
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    static const bool done = [] {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel == nullptr) {
            return true;
        }
        const auto set = reinterpret_cast<SetProcessInformationFn>(
            reinterpret_cast<void*>(GetProcAddress(kernel, "SetProcessInformation")));
        if (set == nullptr) {
            return true;
        }
        // A control bit with a clear state bit turns that kind of throttling off.
        PowerThrottlingState state{kPowerThrottlingVersion,
                                   kThrottleExecutionSpeed | kThrottleIgnoreTimerResolution, 0};
        set(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state));
        return true;
    }();
    (void)done;
}
}  // namespace

TimerResolutionScope::TimerResolutionScope(unsigned milliseconds) : milliseconds_(milliseconds) {
    OptOutOfPowerThrottling();
    WinMm& mm = Mm();
    if (mm.begin != nullptr && mm.end != nullptr && mm.begin(milliseconds_) == kTimeOk) {
        active_ = true;
    } else {
        MH4N_LOG(Warning, "Runtime",
                 "the host refused a timer tick of {} ms: sleeps and VBlanks may be up to 15 ms late", milliseconds_);
    }
}

TimerResolutionScope::~TimerResolutionScope() {
    if (active_) {
        Mm().end(milliseconds_);
    }
}

#else

TimerResolutionScope::TimerResolutionScope(unsigned milliseconds) : milliseconds_(milliseconds) {}
TimerResolutionScope::~TimerResolutionScope() = default;

#endif

}  // namespace mh4n::detail
