#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// macOS keeps its ucontext routines (deprecated, still there) behind _XOPEN_SOURCE, which
// in turn hides BSD names such as MAP_ANONYMOUS unless _DARWIN_C_SOURCE is defined too
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#endif
#include "fiber.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace Port {

#ifdef _WIN32

struct Fiber::Impl {
    void* handle = nullptr;
    bool owns_thread = false; // converted the thread into a fiber
    std::function<void()> entry;
};

static void WINAPI FiberMain(void* param) {
    auto* impl = static_cast<Fiber::Impl*>(param);
    impl->entry();
    std::fprintf(stderr, "fiber entry returned\n");
    std::abort();
}

std::unique_ptr<Fiber> Fiber::FromCurrentThread() {
    auto impl = std::make_unique<Impl>();
    if (IsThreadAFiber()) {
        impl->handle = GetCurrentFiber();
    } else {
        impl->handle = ConvertThreadToFiber(nullptr);
        impl->owns_thread = true;
    }
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

std::unique_ptr<Fiber> Fiber::Create(std::function<void()> entry, std::size_t stack_size) {
    auto impl = std::make_unique<Impl>();
    impl->entry = std::move(entry);
    impl->handle = CreateFiberEx(64 * 1024, stack_size, FIBER_FLAG_FLOAT_SWITCH, FiberMain, impl.get());
    if (!impl->handle) {
        std::fprintf(stderr, "CreateFiberEx failed\n");
        std::abort();
    }
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

Fiber::~Fiber() {
    if (impl->owns_thread) {
        ConvertFiberToThread();
    } else if (impl->entry && impl->handle) {
        DeleteFiber(impl->handle);
    }
}

void Fiber::SwitchTo(Fiber& next) {
    SwitchToFiber(next.impl->handle);
}

#elif defined(__ANDROID__)

// Android's bionic has <ucontext.h> but no getcontext/makecontext/swapcontext, so the
// context switch is the small AArch64 routine in fiber_arm64.S. Android builds here are
// arm64-v8a only (see android/app/build.gradle.kts).
#if !defined(__aarch64__)
#error "the MH4U port's Android fibers are AArch64-only"
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

namespace {

struct Arm64Context {
    std::uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30;
    std::uint64_t sp;
    std::uint64_t d8, d9, d10, d11, d12, d13, d14, d15;
    std::uint64_t arg; // x0 when the context is first entered
};

static_assert(offsetof(Arm64Context, sp) == 96);
static_assert(offsetof(Arm64Context, d8) == 104);
static_assert(offsetof(Arm64Context, arg) == 168);
static_assert(sizeof(Arm64Context) == 176);

extern "C" void mh4u_fiber_switch(Arm64Context* from, Arm64Context* to);

} // namespace

struct Fiber::Impl {
    Arm64Context context{};
    void* stack = nullptr;
    std::size_t stack_size = 0; // including the guard page
    std::function<void()> entry;
};

extern "C" void mh4u_fiber_entry(void* impl_ptr) {
    auto* impl = static_cast<Fiber::Impl*>(impl_ptr);
    impl->entry();
    std::fprintf(stderr, "fiber entry returned\n");
    std::abort();
}

std::unique_ptr<Fiber> Fiber::FromCurrentThread() {
    return std::unique_ptr<Fiber>(new Fiber(std::make_unique<Impl>()));
}

std::unique_ptr<Fiber> Fiber::Create(std::function<void()> entry, std::size_t stack_size) {
    auto impl = std::make_unique<Impl>();
    impl->entry = std::move(entry);
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    stack_size = (stack_size + page - 1) / page * page;
    impl->stack_size = stack_size + page;
    // Reserve lazily; the lowest page stays inaccessible to catch stack overflows.
    impl->stack = mmap(nullptr, impl->stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (impl->stack == MAP_FAILED) {
        std::perror("mmap fiber stack");
        std::abort();
    }
    mprotect(impl->stack, page, PROT_NONE);
    auto* top = static_cast<std::uint8_t*>(impl->stack) + impl->stack_size;
    top = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(top) &
                                          ~std::uintptr_t{15});
    impl->context.sp = reinterpret_cast<std::uint64_t>(top);
    impl->context.x29 = 0; // frame pointer chain ends here
    impl->context.x30 = reinterpret_cast<std::uint64_t>(&mh4u_fiber_entry);
    impl->context.arg = reinterpret_cast<std::uint64_t>(impl.get());
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

Fiber::~Fiber() {
    if (impl->stack) {
        munmap(impl->stack, impl->stack_size);
    }
}

void Fiber::SwitchTo(Fiber& next) {
    mh4u_fiber_switch(&impl->context, &next.impl->context);
}

#else

struct Fiber::Impl {
    ucontext_t context{};
    void* stack = nullptr;
    std::size_t stack_size = 0; // including the guard page
    std::function<void()> entry;
};

static void FiberMain(unsigned hi, unsigned lo) {
    auto* impl = reinterpret_cast<Fiber::Impl*>((static_cast<std::uintptr_t>(hi) << 32) | lo);
    impl->entry();
    std::fprintf(stderr, "fiber entry returned\n");
    std::abort();
}

std::unique_ptr<Fiber> Fiber::FromCurrentThread() {
    return std::unique_ptr<Fiber>(new Fiber(std::make_unique<Impl>()));
}

std::unique_ptr<Fiber> Fiber::Create(std::function<void()> entry, std::size_t stack_size) {
    auto impl = std::make_unique<Impl>();
    impl->entry = std::move(entry);
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    stack_size = (stack_size + page - 1) / page * page;
    impl->stack_size = stack_size + page;
    // Reserve lazily; the lowest page stays inaccessible to catch stack overflows.
    impl->stack = mmap(nullptr, impl->stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (impl->stack == MAP_FAILED) {
        std::perror("mmap fiber stack");
        std::abort();
    }
    mprotect(impl->stack, page, PROT_NONE);
    getcontext(&impl->context);
    impl->context.uc_stack.ss_sp = static_cast<char*>(impl->stack) + page;
    impl->context.uc_stack.ss_size = stack_size;
    impl->context.uc_link = nullptr;
    const auto p = reinterpret_cast<std::uintptr_t>(impl.get());
    makecontext(&impl->context, reinterpret_cast<void (*)()>(FiberMain), 2,
                static_cast<unsigned>(p >> 32), static_cast<unsigned>(p & 0xFFFFFFFFu));
    return std::unique_ptr<Fiber>(new Fiber(std::move(impl)));
}

Fiber::~Fiber() {
    if (impl->stack) {
        munmap(impl->stack, impl->stack_size);
    }
}

void Fiber::SwitchTo(Fiber& next) {
    swapcontext(&impl->context, &next.impl->context);
}

#endif

Fiber::Fiber(std::unique_ptr<Impl> impl_) : impl(std::move(impl_)) {}

} // namespace Port
