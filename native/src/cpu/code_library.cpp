// Loading a code library and handing it the host's callbacks (N1-04). The Windows branch
// (LoadLibraryExW) and the macOS one (dlopen, as Linux) run in the native-windows and native-macos
// jobs of .github/workflows/native.yml (N1-22).
#include <algorithm>
#include <filesystem>
#include <string>

#include <fmt/format.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef Yield  // winbase.h defines it as an empty macro; GuestThread::Yield (mh4n/cpu.h) is a method
#else
#include <dlfcn.h>
#endif

#include "callbacks.h"
#include "mh4n/cpu.h"
#include "mh4n/crc32.h"
#include "recomp_c.h"  // last: it defines macros

namespace mh4n {

namespace {

// The loaders search their library path for a name without a directory. A file of that name in
// the current directory is what `--code lib.so` means, so it gets an absolute path; any other
// name is left to the loader's search (a system library). The altered search path of Windows needs
// an absolute path too.
std::filesystem::path ResolvePath(const std::filesystem::path& path) {
    if (path.has_parent_path() || std::filesystem::exists(path)) {
        return std::filesystem::absolute(path);
    }
    return path;
}

// ---- the platform's dynamic loader ------------------------------------------------------------

#if defined(_WIN32)

std::wstring ToWide(const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
    return wide;
}

std::string LastError() {
    char* text = nullptr;
    const DWORD code = GetLastError();
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = text ? text : "";
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
        message.pop_back();
    }
    return message.empty() ? fmt::format("error {}", code) : message;
}

void* OpenLibrary(const std::string& path, std::string* error) {
    const std::wstring wide = ResolvePath(std::filesystem::path(ToWide(path))).wstring();
    void* handle = LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!handle) {
        *error = LastError();
    }
    return handle;
}

void* FindSymbol(void* handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}

void CloseLibrary(void* handle) {
    FreeLibrary(static_cast<HMODULE>(handle));
}

#else

void* OpenLibrary(const std::string& path, std::string* error) {
    void* handle = dlopen(ResolvePath(std::filesystem::path(path)).string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* message = dlerror();
        *error = message ? message : "dlopen failed";
    }
    return handle;
}

void* FindSymbol(void* handle, const char* name) {
    return dlsym(handle, name);
}

void CloseLibrary(void* handle) {
    dlclose(handle);
}

#endif

// The RHost callbacks have C linkage in recomp.h, so these do too.
extern "C" {
void HostCall(RCtx* ctx, u32 target) { CodeHostCallbacks::Call(ctx, target); }
void HostSvc(RCtx* ctx, u32 imm, u32 next_pc) { CodeHostCallbacks::Svc(ctx, imm, next_pc); }
void HostTrap(RCtx* ctx, u32 pc, u32 kind) { CodeHostCallbacks::Trap(ctx, pc, kind); }
void HostBadReturn(RCtx* ctx, u32 got, u32 expected) { CodeHostCallbacks::BadReturn(ctx, got, expected); }
void HostBudget(RCtx* ctx) { CodeHostCallbacks::Budget(ctx); }
u32 HostSlowRead(RCtx* ctx, u32 address, u32 size) { return CodeHostCallbacks::SlowRead(ctx, address, size); }
void HostSlowWrite(RCtx* ctx, u32 address, u32 value, u32 size) {
    CodeHostCallbacks::SlowWrite(ctx, address, value, size);
}
}

}  // namespace

std::unique_ptr<CodeLibrary> CodeLibrary::Load(const std::string& path, bool deterministic,
                                               volatile u32* preempt_flag, std::string* error) {
    std::string scratch;
    std::string& message = error ? *error : scratch;

    std::string load_error;
    void* handle = OpenLibrary(path, &load_error);
    if (!handle) {
        message = fmt::format("can't load {}: {}", path, load_error);
        return nullptr;
    }
    const auto fail = [&](std::string text) -> std::unique_ptr<CodeLibrary> {
        CloseLibrary(handle);
        message = std::move(text);
        return nullptr;
    };

    const auto init = reinterpret_cast<RCodeInit>(FindSymbol(handle, RC_CODE_INIT_SYMBOL));
    if (!init) {
        return fail(fmt::format("{} is not a recompiled code library (it has no {})", path,
                                RC_CODE_INIT_SYMBOL));
    }
    const auto* mode_symbol = static_cast<const u32*>(FindSymbol(handle, "rc_build_mode"));
    if (!mode_symbol) {
        return fail(fmt::format("{} was built by an older recompiler (it has no rc_build_mode): "
                                "rebuild it with --native",
                                path));
    }
    const u32 mode = *mode_symbol;
    if (mode == 0) {
        return fail(fmt::format("{} was built for Azahar (rc_build_mode 0): rebuild with --native", path));
    }
    if (mode > 2) {
        return fail(fmt::format("{} has an unknown rc_build_mode {}: rebuild it with this version's "
                                "tools",
                                path, mode));
    }
    if (mode == 2 && !deterministic) {
        return fail(fmt::format("{} is a deterministic build (rc_build_mode 2): run with "
                                "--deterministic, or rebuild without --deterministic",
                                path));
    }
    if (mode == 1 && deterministic) {
        return fail(fmt::format("{} is not a deterministic build (rc_build_mode 1): rebuild with "
                                "--native --deterministic, or run without --deterministic",
                                path));
    }

    const RHost host{RC_HOST_VERSION, sizeof(RCtx), HostCall,       HostSvc,       HostTrap,
                     HostBadReturn,   HostBudget,   HostSlowRead,   HostSlowWrite, preempt_flag};
    auto info = std::make_unique<RCodeInfo>();
    if (!init(&host, info.get())) {
        return fail(fmt::format("{} was built against a different recomp.h (host version or context "
                                "size): rebuild it with this version's tools",
                                path));
    }

    std::unique_ptr<CodeLibrary> library(new CodeLibrary());
    library->path_ = path;
    library->deterministic_ = deterministic;
    library->handle_ = handle;
    library->info_ = std::move(info);
    return library;
}

CodeLibrary::~CodeLibrary() {
    if (handle_) {
        CloseLibrary(handle_);
    }
}

u32 CodeLibrary::TextBase() const { return info_->text_base; }
u32 CodeLibrary::TextSize() const { return info_->text_size; }
u32 CodeLibrary::TextCrc32() const { return info_->text_crc32; }
u64 CodeLibrary::TitleId() const { return info_->title_id; }
u32 CodeLibrary::FunctionCount() const { return info_->count; }

GuestFunction CodeLibrary::Find(u32 target) const {
    // The table is sorted by address; an entry's mode is its Thumb bit.
    const RFuncEntry* first = info_->table;
    const RFuncEntry* last = first + info_->count;
    const u32 address = target & ~1u;
    const RFuncEntry* it = std::lower_bound(first, last, address,
                                            [](const RFuncEntry& entry, u32 value) { return entry.addr < value; });
    for (; it != last && it->addr == address; ++it) {
        if (it->mode == (target & 1u)) {
            return it->fn;
        }
    }
    return nullptr;
}

bool CodeLibrary::Matches(std::span<const u8> text, u32 text_base, std::string* error) const {
    const u32 crc = Crc32(text.data(), text.size());
    if (text_base == info_->text_base && text.size() == info_->text_size && crc == info_->text_crc32) {
        return true;
    }
    if (error) {
        *error = fmt::format("the program's code (0x{:08X}+0x{:X}, crc32 {:08X}) is not the code {} was "
                             "built from (0x{:08X}+0x{:X}, crc32 {:08X}): rebuild the code library "
                             "from this version of the game",
                             text_base, text.size(), crc, path_, info_->text_base, info_->text_size,
                             info_->text_crc32);
    }
    return false;
}

}  // namespace mh4n
