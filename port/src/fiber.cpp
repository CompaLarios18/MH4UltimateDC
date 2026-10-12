#include "fiber.h"

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
