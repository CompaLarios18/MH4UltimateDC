// The POSIX side of the guest memory (N1-02): Linux and macOS. The macOS paths are built and tested
// by the native-macos job of .github/workflows/native.yml (N1-22; Apple silicon has 16 KiB host pages).
#if !defined(_WIN32)

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>

#if defined(__linux__) && defined(__x86_64__)
#include <ucontext.h>
#elif defined(__APPLE__)
#include <sys/ucontext.h>
#endif

#include "host.h"

namespace mh4n::host {
namespace {

struct sigaction g_previous_segv;
struct sigaction g_previous_bus;

// Read or write, if the platform's fault frame says (nothing else is touched: this runs in the
// signal handler).
FaultAccess AccessOf(void* context) {
#if defined(__linux__) && defined(__x86_64__)
    // Bit 1 of the page-fault error code is set for a write.
    const auto* uc = static_cast<const ucontext_t*>(context);
    return (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0 ? FaultAccess::Write : FaultAccess::Read;
#elif defined(__APPLE__) && defined(__x86_64__)
    const auto* uc = static_cast<const ucontext_t*>(context);
    return (uc->uc_mcontext->__es.__err & 2) != 0 ? FaultAccess::Write : FaultAccess::Read;
#elif defined(__APPLE__) && defined(__aarch64__)
    // ESR_EL1: WnR is bit 6 of the syndrome of a data abort.
    const auto* uc = static_cast<const ucontext_t*>(context);
    return (uc->uc_mcontext->__es.__esr & (1u << 6)) != 0 ? FaultAccess::Write : FaultAccess::Read;
#else
    (void)context;
    return FaultAccess::Unknown;
#endif
}

void Handler(int signo, siginfo_t* info, void* context) {
    if (fault::HandleFault(reinterpret_cast<std::uintptr_t>(info->si_addr), AccessOf(context))) {
        return; // not reached: HandleFault ends the process for a fault of ours
    }
    // Not ours: hand it to the handler that was installed before.
    const struct sigaction& previous = signo == SIGSEGV ? g_previous_segv : g_previous_bus;
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signo, info, context);
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signo);
        return;
    }
    // Default action: restore it and return, so the instruction faults again and the process dies
    // the way it would have without us.
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(signo, &dfl, nullptr);
}

// An alternate signal stack for the calling thread, freed when the thread ends.
struct AltStack {
    void* memory = nullptr;
    std::size_t size = 0;
    ~AltStack() {
        if (memory != nullptr) {
            stack_t disable;
            std::memset(&disable, 0, sizeof disable);
            disable.ss_flags = SS_DISABLE;
            sigaltstack(&disable, nullptr);
            munmap(memory, size);
        }
    }
};

} // namespace

std::size_t PageSize() {
    return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
}

void* Reserve(std::size_t size) {
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

void Release(void* base, std::size_t size) {
    munmap(base, size);
}

bool Commit(void* addr, std::size_t size) {
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}

bool Decommit(void* addr, std::size_t size) {
    // Replacing the pages with a fresh inaccessible mapping frees them at once and guarantees that
    // a later Commit sees zeros (madvise(MADV_DONTNEED) does that on Linux only).
    void* p = mmap(addr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return p != MAP_FAILED;
}

void InstallFaultHandler() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction action;
        std::memset(&action, 0, sizeof action);
        action.sa_sigaction = Handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &g_previous_segv);
        sigaction(SIGBUS, &action, &g_previous_bus);
    });
}

void InstallThreadFaultStack() {
    thread_local AltStack stack;
    if (stack.memory != nullptr) {
        return;
    }
    stack_t current;
    if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0) {
        return; // the thread already has one
    }
    const std::size_t size = 64 * 1024;
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        return;
    }
    stack_t alt;
    std::memset(&alt, 0, sizeof alt);
    alt.ss_sp = memory;
    alt.ss_size = size;
    if (sigaltstack(&alt, nullptr) != 0) {
        munmap(memory, size);
        return;
    }
    stack.memory = memory;
    stack.size = size;
}

} // namespace mh4n::host

#endif // !_WIN32
