// Frame timing for the performance baseline (N0-15): where the time of a frame goes.
//
// Four numbers per frame, written as one CSV row each (--perf-log in the runner, perf_log in
// mh4u-app.ini):
//   present_us  host time between two vertical blanks (the GPU's present of Azahar), the
//               frame time the player sees
//   recomp_us   host time in the recompiled code, the interpreter fallback included, without
//               the system calls and the GPU work done inside it
//   svc_us      host time in system calls (the kernel and the services), without the GPU
//               commands they ran
//   gpu_us      host time in the GPU's command processing (every GSP command and register
//               write: command lists, display transfers, memory fills), as far as it runs on
//               the emulation thread. Azahar's Vulkan renderer has worker threads, so the
//               GPU's own time is present_us minus the rest, not a number measured here.
// Times are exclusive (a system call inside the code, a GPU command inside the system call:
// each microsecond is counted once, in the innermost of the three).
//
// This file includes nothing of Azahar, so the native runtime can use it as it is. A timer costs
// a clock read at each end (QueryPerformanceCounter / clock_gettime) and one atomic add; with
// no log it is one relaxed load. Nothing allocates or writes a file on the emulation thread:
// finished frames go through a fixed ring to a writer thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace Port::Perf {

/// What a scope is charged to.
enum class Slot : std::uint32_t { Recomp, Svc, Gpu, Count };

/// True while a log is being written (Start succeeded, Stop not yet called).
extern std::atomic<bool> g_enabled;

inline bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

/// Opens `path` and starts logging; false (with `error` set) if the file can't be created.
/// `header` is the first line of the file, without the leading "# " (the tools' header:
/// "perf v1 <ISO date> game=0x<title id> cpu=<cpu> new3ds=<on|off>"). Stop is registered with
/// atexit, so a process that just returns still gets its last frames written.
bool Start(const std::string& path, const std::string& header, std::string& error);

/// Writes what is left, closes the file. Safe to call twice, or without Start.
void Stop();

/// A vertical blank: closes the frame that was running and starts the next. The first call only
/// starts the first frame. Call it from one thread (the one that runs the emulation).
void FrameBoundary();

/// Frames dropped because the writer thread fell behind (the ring is 16384 frames).
std::uint64_t DroppedFrames();

/// Explicit begin and end of a scope, for hooks that arrive as two callbacks. `Leave` closes the
/// innermost open scope of this thread if it is `slot`, and does nothing otherwise (a scope
/// opened before the log started).
void Enter(Slot slot);
void Leave(Slot slot);

/// Charges its lifetime to `slot`.
class Scope {
public:
    explicit Scope(Slot slot_) : slot(slot_), on(Enabled()) {
        if (on) {
            Enter(slot);
        }
    }
    ~Scope() {
        if (on) {
            Leave(slot);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Slot slot;
    bool on;
};

/// The monotonic clock, in nanoseconds.
std::uint64_t NowNs();

/// For tests: replaces the clock (null: the real one). Not thread safe; call it before Start.
void SetClockForTest(std::uint64_t (*clock)());

/// Puts the timers on a GPU hook set (Pica::CaptureHooks of the Azahar patch, or any struct with
/// the same std::function members): the GPU time is the time between `execute_begin` and
/// `execute_end`, and between `write_reg_begin` and `write_reg_end`; a frame ends at the
/// interrupt `is_vblank` accepts (the first display interrupt, PDC0). Hooks that are already set
/// keep running (they are called after the timer starts and before it stops). Does nothing to
/// the hooks the struct doesn't have. The capture of --capture-gpu sets the same hooks and
/// would replace these: the two don't run together.
template <class Hooks, class IsVBlank>
void ChainGpuHooks(Hooks& hooks, IsVBlank is_vblank) {
    hooks.execute_begin = [prev = std::move(hooks.execute_begin)](const auto& command) {
        Enter(Slot::Gpu);
        if (prev) {
            prev(command);
        }
    };
    hooks.execute_end = [prev = std::move(hooks.execute_end)]() {
        if (prev) {
            prev();
        }
        Leave(Slot::Gpu);
    };
    hooks.write_reg_begin = [prev = std::move(hooks.write_reg_begin)](auto address, auto value) {
        Enter(Slot::Gpu);
        if (prev) {
            prev(address, value);
        }
    };
    hooks.write_reg_end = [prev = std::move(hooks.write_reg_end)]() {
        if (prev) {
            prev();
        }
        Leave(Slot::Gpu);
    };
    hooks.interrupt = [prev = std::move(hooks.interrupt), is_vblank](auto id, auto delay_ns) {
        if (is_vblank(id)) {
            FrameBoundary();
        }
        if (prev) {
            prev(id, delay_ns);
        }
    };
}

} // namespace Port::Perf
