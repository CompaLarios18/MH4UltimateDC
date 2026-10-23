// The Windows crash handler (N3-13): SetUnhandledExceptionFilter, which runs after the vectored
// handlers (the memory module's fault handler is one) and so after a fault of the guest has been
// told apart from a bug. It keeps to what crash_posix.cpp does: a file opened in advance, a fixed
// buffer, frames as module+offset with the module's file name only, the previous filter called
// last. This file cannot be compiled in the cloud (there is no llvm-mingw there): only the
// app-windows job of .github/workflows/native-app.yml builds and runs it, together with the
// crash test of native/app/tests/app_report_test.cpp.
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstring>

#include "../../src/common/utf8.h"
#include "crash_internal.h"

namespace mh4n::app::report::detail {
namespace {

std::atomic<HANDLE> g_file{INVALID_HANDLE_VALUE};
std::atomic<bool> g_written{false};
std::atomic_flag g_entered;  // (clear when default-initialised, C++20)
bool g_installed = false;    // only read and written by PlatformInstall (under Install's lock)
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

char g_buffer[2048];

const char* ExceptionName(DWORD code) {
    switch (code) {
    case 0xC0000005: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC00000FD: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000094: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC000001D: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000006: return "EXCEPTION_IN_PAGE_ERROR";
    case 0x80000003: return "EXCEPTION_BREAKPOINT";
    default: return "exception";
    }
}

void WriteAll(HANDLE file, const char* data, std::size_t size) {
    while (size > 0) {
        const DWORD chunk = size > 0x10000 ? 0x10000 : static_cast<DWORD>(size);
        DWORD done = 0;
        if (!WriteFile(file, data, chunk, &done, nullptr) || done == 0) {
            return;
        }
        data += done;
        size -= done;
    }
}

void Flush(HANDLE file, Text& text) {
    WriteAll(file, text.data(), text.size());
    text.Clear();
}

// ---- frames -----------------------------------------------------------------------------------------

#if defined(_M_X64) || defined(__x86_64__)
// The frames of the faulting thread, from the exception's own context (the filter's stack is not
// the faulting stack): each frame is unwound with the function's unwind data; a leaf function
// without any has its return address on top of the stack.
int Walk(CONTEXT context, std::uintptr_t* out) {
    int count = 0;
    while (count < kMaxFrames) {
        const DWORD64 pc = context.Rip;
        if (pc == 0) {
            break;
        }
        out[count++] = static_cast<std::uintptr_t>(pc);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        if (function == nullptr) {
            DWORD64 return_address = 0;
            SIZE_T read = 0;
            if (context.Rsp == 0 ||
                !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp),
                                   &return_address, sizeof return_address, &read) ||
                read != sizeof return_address) {
                break;
            }
            context.Rip = return_address;
            context.Rsp += sizeof return_address;
        } else {
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, function, &context, &handler_data,
                             &establisher, nullptr);
        }
    }
    return count;
}
#else
// Other CPUs (arm64): only the exception's address (WriteCrash adds it when Walk finds none).
int Walk(const CONTEXT&, std::uintptr_t*) {
    return 0;
}
#endif

// The module a code address is in and the offset in it; the file name only, never the path.
bool Resolve(std::uintptr_t address, char* name, std::size_t capacity, std::uintptr_t* offset) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        module == nullptr) {
        return false;
    }
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(module, path, sizeof path);
    if (length == 0 || length >= sizeof path) {
        return false;
    }
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    std::size_t n = 0;
    for (; base[n] != '\0' && n + 1 < capacity; ++n) {
        const unsigned char c = static_cast<unsigned char>(base[n]);
        name[n] = c < 0x20 ? '?' : static_cast<char>(c);
    }
    name[n] = '\0';
    *offset = address - reinterpret_cast<std::uintptr_t>(module);
    return true;
}

void WriteCrash(EXCEPTION_POINTERS* info) {
    const HANDLE file = g_file.load(std::memory_order_acquire);
    if (file == INVALID_HANDLE_VALUE || info == nullptr || info->ExceptionRecord == nullptr) {
        return;
    }
    g_written.store(true, std::memory_order_release);
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    std::uintptr_t fault_address = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
    if (record.ExceptionCode == 0xC0000005 && record.NumberParameters >= 2) {
        fault_address = static_cast<std::uintptr_t>(record.ExceptionInformation[1]);
    }
    Text text(g_buffer, sizeof g_buffer);
    AppendPreamble(text);
    text.Str("signal: ");
    text.Str(ExceptionName(record.ExceptionCode));
    text.Str(" (");
    text.Hex(record.ExceptionCode, 8);
    text.Str(")\nfault address: ");
    text.Hex(fault_address, 16);
    text.Put('\n');
    AppendGameState(text);
    text.Str("frames:\n");
    Flush(file, text);

    std::uintptr_t addresses[kMaxFrames];
    int count = 0;
    if (info->ContextRecord != nullptr) {
        count = Walk(*info->ContextRecord, addresses);
    }
    if (count == 0) {
        addresses[count++] = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
    }
    for (int i = 0; i < count; ++i) {
        char module[96];
        std::uintptr_t offset = 0;
        text.Put('#');
        text.Dec(static_cast<std::uint64_t>(i));
        text.Put(' ');
        if (Resolve(addresses[i], module, sizeof module, &offset)) {
            text.Str(module);
            text.Put('+');
            text.HexMin(offset);
        } else {
            text.Str("?+");
            text.HexMin(addresses[i]);
        }
        text.Put('\n');
        Flush(file, text);
    }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    // a second thread's crash, or one inside this filter, only chains on
    if (!g_entered.test_and_set(std::memory_order_acq_rel)) {
        WriteCrash(info);
    }
    if (g_previous != nullptr) {
        return g_previous(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

unsigned long ProcessId() {
    return static_cast<unsigned long>(GetCurrentProcessId());
}

bool PlatformInstall(const std::string& crash_path) {
    const std::wstring wide = utf8::ToWide(crash_path);
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const HANDLE old = g_file.exchange(file, std::memory_order_acq_rel);
    if (old != INVALID_HANDLE_VALUE) {
        CloseHandle(old);
    }
    g_written.store(false, std::memory_order_release);
    if (!g_installed) {
        g_installed = true;
        g_previous = SetUnhandledExceptionFilter(Filter);
    }
    return true;
}

bool PlatformUnused() {
    return !g_written.load(std::memory_order_acquire);
}

void PlatformClose() {
    const HANDLE old = g_file.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    if (old != INVALID_HANDLE_VALUE) {
        CloseHandle(old);
    }
}

}  // namespace mh4n::app::report::detail

#endif  // _WIN32
