// The Windows side of the guest memory (N1-02): VirtualAlloc for the reservation and a vectored
// exception handler for the bad accesses. Built and tested by the native-windows job of
// .github/workflows/native.yml (N1-22), and cross-built and run under wine when working without a runner.
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "host.h"

namespace mh4n::host {
namespace {

LONG CALLBACK Handler(PEXCEPTION_POINTERS info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        // ExceptionInformation[0]: 0 read, 1 write, 8 execute; [1]: the address
        const ULONG_PTR kind = record->ExceptionInformation[0];
        const FaultAccess access = kind == 0   ? FaultAccess::Read
                                   : kind == 1 ? FaultAccess::Write
                                               : FaultAccess::Unknown;
        // Does not return for a fault inside a guest reservation.
        fault::HandleFault(static_cast<std::uintptr_t>(record->ExceptionInformation[1]), access);
    }
    return EXCEPTION_CONTINUE_SEARCH; // anyone else's: the next handler
}

} // namespace

std::size_t PageSize() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}

void* Reserve(std::size_t size) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}

void Release(void* base, std::size_t) {
    VirtualFree(base, 0, MEM_RELEASE);
}

bool Commit(void* addr, std::size_t size) {
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool Decommit(void* addr, std::size_t size) {
    return VirtualFree(addr, size, MEM_DECOMMIT) != 0;
}

void InstallFaultHandler() {
    static const PVOID handle = AddVectoredExceptionHandler(1, Handler);
    (void)handle;
}

void InstallThreadFaultStack() {}

} // namespace mh4n::host

#endif // _WIN32
