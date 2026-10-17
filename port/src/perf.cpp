// Frame timing for the performance baseline (N0-15); see perf.h. Nothing of Azahar in here.
#include "perf.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace Port::Perf {

std::atomic<bool> g_enabled{false};

namespace {

constexpr std::size_t kRingSize = 16384; // frames; a power of two
constexpr std::size_t kMaxDepth = 16;    // nested open scopes per thread
constexpr auto kDrainEvery = std::chrono::milliseconds(20);

std::uint64_t SystemClockNs() {
#if defined(_WIN32)
    static const std::uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const auto ticks = static_cast<std::uint64_t>(c.QuadPart);
    // split so that ticks * 1e9 can't overflow
    return (ticks / freq) * 1000000000ull + (ticks % freq) * 1000000000ull / freq;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

std::uint64_t (*g_clock)() = SystemClockNs;

struct Row {
    std::uint64_t frame;
    std::uint64_t present_ns;
    std::uint64_t slot_ns[static_cast<std::size_t>(Slot::Count)];
};

// Time charged to each slot since the last frame boundary (any thread adds, the boundary takes).
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Slot::Count)> g_acc{};

// Open scopes of this thread. The slot's own time is the elapsed time minus what the scopes
// inside it took (`inner_ns`).
struct Open {
    Slot slot;
    std::uint64_t start_ns;
    std::uint64_t inner_ns;
};
struct ThreadStack {
    std::array<Open, kMaxDepth> open;
    std::size_t depth = 0;
};
thread_local ThreadStack t_stack;

// Single producer (the thread that calls FrameBoundary), single consumer (the writer).
std::array<Row, kRingSize> g_ring;
std::atomic<std::uint64_t> g_head{0}; // next to write
std::atomic<std::uint64_t> g_tail{0}; // next to read
std::atomic<std::uint64_t> g_dropped{0};

std::uint64_t g_last_boundary_ns = 0;
bool g_have_boundary = false;
std::uint64_t g_frame_index = 0;

std::mutex g_control; // Start and Stop
std::FILE* g_file = nullptr;
std::thread g_writer;
std::atomic<bool> g_stop{false};
bool g_atexit_registered = false;

void WriteUs(std::FILE* f, std::uint64_t ns) {
    std::fprintf(f, "%llu.%03llu", static_cast<unsigned long long>(ns / 1000),
                 static_cast<unsigned long long>(ns % 1000));
}

std::size_t Drain() {
    std::size_t n = 0;
    std::uint64_t tail = g_tail.load(std::memory_order_relaxed);
    const std::uint64_t head = g_head.load(std::memory_order_acquire);
    for (; tail != head; ++tail, ++n) {
        const Row& r = g_ring[tail & (kRingSize - 1)];
        std::fprintf(g_file, "%llu,", static_cast<unsigned long long>(r.frame));
        WriteUs(g_file, r.present_ns);
        for (const auto ns : r.slot_ns) {
            std::fputc(',', g_file);
            WriteUs(g_file, ns);
        }
        std::fputc('\n', g_file);
    }
    g_tail.store(tail, std::memory_order_release);
    return n;
}

void WriterMain() {
    while (!g_stop.load(std::memory_order_acquire)) {
        Drain();
        std::this_thread::sleep_for(kDrainEvery);
    }
    Drain();
}

void StopAtExit() {
    Stop();
}

} // namespace

std::uint64_t NowNs() {
    return g_clock();
}

void SetClockForTest(std::uint64_t (*clock)()) {
    g_clock = clock ? clock : SystemClockNs;
}

bool Start(const std::string& path, const std::string& header, std::string& error) {
    std::lock_guard lock(g_control);
    if (g_file) {
        error = "the performance log is already open";
        return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        error = "can't create " + path;
        return false;
    }
    std::fprintf(f, "# %s\n", header.c_str());
    std::fputs("frame,present_us,recomp_us,svc_us,gpu_us\n", f);
    g_file = f;
    for (auto& a : g_acc) {
        a.store(0, std::memory_order_relaxed);
    }
    g_head.store(0);
    g_tail.store(0);
    g_dropped.store(0);
    g_have_boundary = false;
    g_frame_index = 0;
    g_stop.store(false);
    g_writer = std::thread(WriterMain);
    if (!g_atexit_registered) {
        g_atexit_registered = true;
        std::atexit(StopAtExit);
    }
    g_enabled.store(true, std::memory_order_release);
    return true;
}

void Stop() {
    std::lock_guard lock(g_control);
    if (!g_file) {
        return;
    }
    g_enabled.store(false, std::memory_order_release);
    g_stop.store(true, std::memory_order_release);
    if (g_writer.joinable()) {
        g_writer.join();
    }
    std::fprintf(g_file, "# frames=%llu dropped=%llu\n", static_cast<unsigned long long>(g_frame_index),
                 static_cast<unsigned long long>(g_dropped.load()));
    std::fclose(g_file);
    g_file = nullptr;
}

std::uint64_t DroppedFrames() {
    return g_dropped.load(std::memory_order_relaxed);
}

void FrameBoundary() {
    if (!Enabled()) {
        return;
    }
    const std::uint64_t now = NowNs();
    std::array<std::uint64_t, static_cast<std::size_t>(Slot::Count)> taken;
    for (std::size_t i = 0; i < taken.size(); ++i) {
        taken[i] = g_acc[i].exchange(0, std::memory_order_relaxed);
    }
    if (g_have_boundary) {
        const std::uint64_t head = g_head.load(std::memory_order_relaxed);
        if (head - g_tail.load(std::memory_order_acquire) >= kRingSize) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        } else {
            Row& r = g_ring[head & (kRingSize - 1)];
            r.frame = g_frame_index;
            r.present_ns = now - g_last_boundary_ns;
            for (std::size_t i = 0; i < taken.size(); ++i) {
                r.slot_ns[i] = taken[i];
            }
            g_head.store(head + 1, std::memory_order_release);
        }
        ++g_frame_index;
    }
    g_have_boundary = true;
    g_last_boundary_ns = now;
}

void Enter(Slot slot) {
    if (!Enabled()) {
        return;
    }
    ThreadStack& s = t_stack;
    if (s.depth == kMaxDepth) {
        return; // too deep to be real: its time stays with the scope around it
    }
    s.open[s.depth++] = Open{slot, NowNs(), 0};
}

void Leave(Slot slot) {
    ThreadStack& s = t_stack;
    if (s.depth == 0 || s.open[s.depth - 1].slot != slot) {
        return;
    }
    const Open o = s.open[--s.depth];
    const std::uint64_t elapsed = NowNs() - o.start_ns;
    const std::uint64_t own = elapsed > o.inner_ns ? elapsed - o.inner_ns : 0;
    g_acc[static_cast<std::size_t>(slot)].fetch_add(own, std::memory_order_relaxed);
    if (s.depth > 0) {
        s.open[s.depth - 1].inner_ns += elapsed;
    }
}

} // namespace Port::Perf
