// The POSIX crash handler (N3-13): Linux and macOS. Everything from Handler on is
// async-signal-safe: open(2), read(2), write(2), close(2), sigaction(2), raise(3), fixed buffers
// and integer formatting by hand. The two exceptions are backtrace() (its unwinder loads libgcc
// the first time it runs, so Install calls it once in advance) and, on macOS only, dladdr(). The
// Windows counterpart is crash_win32.cpp; macOS (SIGBUS, dladdr) is checked only by the
// native-app.yml job of the macOS runner.
#if !defined(_WIN32)

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>

#if defined(__GLIBC__) || defined(__APPLE__)
#define MH4N_HAVE_BACKTRACE 1
#include <execinfo.h>
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#endif
#if defined(__linux__)
#include <ucontext.h>
#endif

#include "crash_internal.h"

namespace mh4n::app::report::detail {
namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr int kSignalCount = static_cast<int>(sizeof kSignals / sizeof kSignals[0]);

struct sigaction g_previous[kSignalCount];
std::atomic<int> g_fd{-1};
std::atomic<bool> g_written{false};
std::atomic_flag g_entered;  // (clear when default-initialised, C++20)
bool g_handlers_installed = false;  // only read and written by PlatformInstall (under Install's lock)

char g_buffer[2048];

int IndexOf(int signo) {
    for (int i = 0; i < kSignalCount; ++i) {
        if (kSignals[i] == signo) {
            return i;
        }
    }
    return -1;
}

const char* SignalName(int signo) {
    switch (signo) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

void WriteAll(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
}

void Flush(int fd, Text& text) {
    WriteAll(fd, text.data(), text.size());
    text.Clear();
}

// ---- frames -----------------------------------------------------------------------------------------

// The module a code address is in, and its offset in the module: the name only (the part of the
// path after the last '/'), never the path.
struct Frame {
    char module[96];
    std::uintptr_t offset;
};

void SetModuleName(Frame& frame, const char* path) {
    const char* name = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    std::size_t n = 0;
    for (; name[n] != '\0' && n + 1 < sizeof frame.module; ++n) {
        const unsigned char c = static_cast<unsigned char>(name[n]);
        frame.module[n] = c < 0x20 ? '?' : static_cast<char>(c);
    }
    frame.module[n] = '\0';
}

#if defined(__linux__)

// A line of /proc/self/maps, read through a small buffer: "start-end perms offset dev inode  path".
struct MapsReader {
    int fd;
    char chunk[4096];
    ssize_t length = 0;
    ssize_t position = 0;

    // The next line without its newline in `line` (cut at `capacity - 1`); false at the end.
    bool Next(char* line, std::size_t capacity) {
        std::size_t n = 0;
        bool any = false;
        for (;;) {
            if (position >= length) {
                length = read(fd, chunk, sizeof chunk);
                position = 0;
                if (length < 0 && errno == EINTR) {
                    length = 0;
                    continue;
                }
                if (length <= 0) {
                    line[n] = '\0';
                    return any;
                }
            }
            const char c = chunk[position++];
            any = true;
            if (c == '\n') {
                line[n] = '\0';
                return true;
            }
            if (n + 1 < capacity) {
                line[n++] = c;
            }
        }
    }
};

bool ParseHex(const char*& p, std::uintptr_t* value) {
    std::uintptr_t v = 0;
    bool any = false;
    for (;; ++p) {
        const char c = *p;
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            break;
        }
        v = v << 4 | static_cast<std::uintptr_t>(digit);
        any = true;
    }
    *value = v;
    return any;
}

std::uint32_t HashPath(const char* path) {
    std::uint32_t h = 2166136261u;
    for (; *path != '\0'; ++path) {
        h = (h ^ static_cast<unsigned char>(*path)) * 16777619u;
    }
    return h;
}

bool Resolve(std::uintptr_t address, Frame* frame) {
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    MapsReader reader;
    reader.fd = fd;
    char line[4352];
    std::uintptr_t base = 0;
    std::uint32_t base_hash = 0;
    bool found = false;
    while (!found && reader.Next(line, sizeof line)) {
        const char* p = line;
        std::uintptr_t start = 0, end = 0, file_offset = 0;
        if (!ParseHex(p, &start) || *p++ != '-' || !ParseHex(p, &end) || *p++ != ' ') {
            continue;
        }
        while (*p != '\0' && *p != ' ') {  // the permissions
            ++p;
        }
        if (*p++ != ' ' || !ParseHex(p, &file_offset)) {
            continue;
        }
        while (*p == ' ') {
            ++p;
        }
        while (*p != '\0' && *p != ' ') {  // the device
            ++p;
        }
        while (*p == ' ') {
            ++p;
        }
        while (*p != '\0' && *p != ' ') {  // the inode
            ++p;
        }
        while (*p == ' ') {
            ++p;
        }
        const char* path = p;
        const std::uint32_t hash = HashPath(path);
        if (file_offset == 0 && *path != '\0') {  // the first mapping of a file: its load address
            base = start;
            base_hash = hash;
        }
        if (address >= start && address < end) {
            found = true;
            if (*path == '\0') {
                std::memcpy(frame->module, "?", 2);
                frame->offset = address;
            } else {
                SetModuleName(*frame, path);
                frame->offset = address - (hash == base_hash ? base : start - file_offset);
            }
        }
    }
    close(fd);
    return found;
}

#elif defined(__APPLE__)

bool Resolve(std::uintptr_t address, Frame* frame) {
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(address), &info) == 0 || info.dli_fname == nullptr) {
        return false;
    }
    SetModuleName(*frame, info.dli_fname);
    frame->offset = address - reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    return true;
}

#else

bool Resolve(std::uintptr_t, Frame*) {
    return false;
}

#endif

// The instruction the signal interrupted, where the platform's context says (0 otherwise): the
// frames above it belong to the handler and are left out.
std::uintptr_t InterruptedPc(void* context) {
#if defined(__linux__) && defined(__x86_64__)
    return static_cast<std::uintptr_t>(
        static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    return static_cast<std::uintptr_t>(static_cast<const ucontext_t*>(context)->uc_mcontext.pc);
#else
    (void)context;
    return 0;
#endif
}

void WriteFrames(int fd, void* context) {
    Text text(g_buffer, sizeof g_buffer);
    text.Str("frames:\n");
    Flush(fd, text);
#if defined(MH4N_HAVE_BACKTRACE)
    void* addresses[kMaxFrames];
    const int count = backtrace(addresses, kMaxFrames);
    int first = 0;
    const std::uintptr_t pc = InterruptedPc(context);
    for (int i = 0; pc != 0 && i < count; ++i) {
        if (reinterpret_cast<std::uintptr_t>(addresses[i]) == pc) {
            first = i;
            break;
        }
    }
    for (int i = first; i < count; ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(addresses[i]);
        Frame frame;
        text.Put('#');
        text.Dec(static_cast<std::uint64_t>(i - first));
        text.Put(' ');
        if (Resolve(address, &frame)) {
            text.Str(frame.module);
            text.Put('+');
            text.HexMin(frame.offset);
        } else {
            text.Str("?+");
            text.HexMin(address);
        }
        text.Put('\n');
        Flush(fd, text);
    }
#else
    (void)context;
#endif
}

void WriteCrash(int signo, siginfo_t* info, void* context) {
    const int fd = g_fd.load(std::memory_order_acquire);
    if (fd < 0) {
        return;
    }
    g_written.store(true, std::memory_order_release);
    Text text(g_buffer, sizeof g_buffer);
    AppendPreamble(text);
    text.Str("signal: ");
    text.Str(SignalName(signo));
    text.Str(" (");
    text.Dec(static_cast<std::uint64_t>(signo));
    text.Str(")\nfault address: ");
    text.Hex(info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0, 16);
    text.Put('\n');
    AppendGameState(text);
    Flush(fd, text);
    WriteFrames(fd, context);
}

// The handler that was installed before ours gets the signal; without one, the default action.
void Chain(int signo, siginfo_t* info, void* context) {
    const int index = IndexOf(signo);
    if (index >= 0) {
        const struct sigaction& previous = g_previous[index];
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
            previous.sa_sigaction(signo, info, context);
            return;
        }
        if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            previous.sa_handler(signo);
            return;
        }
    }
    // The default action: restore it. A fault of the instruction itself happens again when the
    // handler returns and ends the process; a signal sent with kill or raise (abort() does) is
    // sent again here and arrives when the handler returns.
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(signo, &dfl, nullptr);
    if (info == nullptr || info->si_code <= 0) {
        raise(signo);
    }
}

// If a handler installed before ours recovers (returns without ending the process), g_entered and
// g_written stay set: a later crash is not written, and the exit hook keeps the stale file. That
// cannot happen with the memory module (it ends the process on a guest fault), so it is only
// documented here.
void Handler(int signo, siginfo_t* info, void* context) {
    // a second thread's crash, or one inside this handler, only chains on
    if (!g_entered.test_and_set(std::memory_order_acq_rel)) {
        WriteCrash(signo, info, context);
    }
    Chain(signo, info, context);
}

// An alternate stack for the calling thread when it has none, so that a stack overflow can be
// reported. (Threads that were started later get theirs from the memory module or have none.)
void EnsureAlternateStack() {
    stack_t current;
    if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0) {
        return;
    }
    const std::size_t size = 128 * 1024;
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        return;
    }
    stack_t alt;
    std::memset(&alt, 0, sizeof alt);
    alt.ss_sp = memory;  // kept for the life of the process
    alt.ss_size = size;
    sigaltstack(&alt, nullptr);
}

}  // namespace

unsigned long ProcessId() {
    return static_cast<unsigned long>(getpid());
}

bool PlatformInstall(const std::string& crash_path) {
    const int fd = open(crash_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const int old = g_fd.exchange(fd, std::memory_order_acq_rel);
    if (old >= 0) {
        close(old);
    }
    g_written.store(false, std::memory_order_release);
    if (!g_handlers_installed) {
        g_handlers_installed = true;
#if defined(MH4N_HAVE_BACKTRACE)
        void* warm[2];
        backtrace(warm, 2);  // loads the unwinder now, not inside a handler
#endif
        EnsureAlternateStack();
        for (int i = 0; i < kSignalCount; ++i) {
            struct sigaction action;
            std::memset(&action, 0, sizeof action);
            action.sa_sigaction = Handler;
            action.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&action.sa_mask);
            sigaction(kSignals[i], &action, &g_previous[i]);
        }
    }
    return true;
}

bool PlatformUnused() {
    return !g_written.load(std::memory_order_acquire);
}

void PlatformClose() {
    const int old = g_fd.exchange(-1, std::memory_order_acq_rel);
    if (old >= 0) {
        close(old);
    }
}

}  // namespace mh4n::app::report::detail

#endif  // !_WIN32
