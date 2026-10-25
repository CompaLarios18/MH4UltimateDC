// The renderer thread and its queue (N2-22): a GPU wrapper that runs another gpu::Gpu on a thread
// of its own (D56, ROADMAP principles 4 and 6).
//
// Execute, WriteReg, SetBufferSwap, SetColorFill, FlushRegion, InvalidateRegion and VBlank are put
// on a queue and return at once; the renderer thread calls the wrapped GPU with them in the order
// they were made. Everything the wrapped GPU signals (Host::SignalInterrupt) it signals from that
// thread, so interrupts reach the runtime in the order the commands completed.
//
// What each call means here:
//  - FlushRegion is queued in order, so the region is marked stale before any later command runs.
//  - InvalidateRegion (the CPU is about to read) returns only when every earlier command is done
//    and what they handed to Host::Post (their writes into guest memory) has been run, on the
//    calling thread, which is the scheduler thread: Host::Post work only runs at a safe point, and
//    the caller is at one. That needs the posted work to pass through QueueHost (below); a wrapped
//    GPU built with the plain host posts straight to the runtime, and its writes land at the next
//    safe point after InvalidateRegion has returned. The wrapper does not decode commands, so
//    "earlier commands that touch the range" is every earlier command (a superset).
//  - ReadReg is answered at once from a mirror of the registers seen through WriteReg, even if the
//    wrapped GPU later changes the register itself (a trigger or busy bit): the mirror holds what
//    the game wrote. A register never written through the wrapper (or since LoadInitState) is
//    read from the wrapped GPU on the renderer thread after everything queued so far, and that
//    wait happens on EVERY such call: the answer is not cached.
//  - VBlank is queued like a command. VBlank(0) ends a frame. `max_frames_in_flight` (default 1) is
//    the engine's own one frame of lag (system-services.md): a VBlank(0) that finds that many
//    frames or more already ended and not yet rendered is counted (`queue_frame_waits`) and does
//    NOT block its caller: a thread that waits for the GPU waits alone (ROADMAP principle 4),
//    and the engine's lag keeps the queue short in practice (the guest thread waits for the
//    interrupts, which come in completion order). `max_frames_queued` (default 4, at least
//    max_frames_in_flight + 1) is a safety cap on the memory a game that does not wait could
//    queue: a VBlank(0) that finds that many frames queued blocks its caller until the renderer
//    has finished one.
//
// Principle 6 (the owner rule, rhi-spike.md 9.4 item 5, rhi.h `OwnerRule`): the renderer thread
// owns every image the wrapped GPU makes and is the only thread that records on them. With
// MH4N_RHI_CHECKS on, every call this wrapper forwards to the wrapped GPU goes through
// ForwardTarget(), which calls the backend's owner-rule handler (`rhi::ReportOwnerRuleBreach`,
// `OwnerRule::WrongThread`) when it runs on any other thread. The thread-affine parts of the
// wrapped GPU (its construction through the QueueHost constructor, every call, its destruction)
// all run on the renderer thread.
//
// Threads: the calls of gpu::Gpu and gpu::GpuDebug are made by the scheduler thread (any one
// thread at a time; the order across threads is not defined). Counters and BlockedCallers may be
// read from any thread. A wrapped GPU never waits for the scheduler (its Post and SignalInterrupt
// are non-blocking). One that waits for the completion of its own Post while the scheduler is
// blocked in InvalidateRegion, at the safety cap, in an unseen ReadReg, in a debug call or in a
// lockstep call would deadlock.
//
// Failures: an exception that the wrapped GPU throws on the renderer thread is logged and the
// queue goes on (the call counts as completed, and a frame whose VBlank(0) threw is released). A
// debug call or an unseen ReadReg, which wait for their result, rethrow it to their caller.
//
// Counters (GpuDebug::TakeFrameCounters, reading resets them), added to the wrapped GPU's own:
//   translate_us       microseconds the renderer thread spent inside the wrapped GPU (its busy
//                      time)
//   queue_wait_us      microseconds callers spent blocked in the wrapper (the safety cap,
//                      InvalidateRegion, an unseen ReadReg, every call in lockstep mode): the
//                      callers' blocked time, not the renderer's idle time
//   queue_frame_waits  how many VBlank(0) calls found `max_frames_in_flight` frames or more
//                      queued or in flight (they did not block; see above)
// With the debug calls (LoadInitState, TakeRegisterWrites, WriteBackColorBuffers) the wrapper runs
// the call on the renderer thread and waits, since they need the results; TakeFrameCounters and
// TakeFrameGpuTimeNs instead return what the renderer thread collected from the wrapped GPU at its
// last VBlank(0) (summed over the frames not read yet), so the runtime can read them without
// waiting for the renderer to catch up. In lockstep mode they are forwarded and answered at once
// like the others.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"

namespace mh4n::gpu::queue {

struct QueueConfig {
    // Every call returns only after the renderer thread has finished it. The capture player uses
    // it (it judges the guest memory and the interrupts right after each call); the runtime does
    // not.
    bool lockstep = false;
    // The engine's lag: frames ended by VBlank(0) and not yet rendered beyond which a VBlank(0) is
    // counted as a frame wait (it does not block). At least 1.
    u32 max_frames_in_flight = 1;
    // The safety cap: a VBlank(0) that finds this many frames queued blocks until one is rendered.
    // At least max_frames_in_flight + 1.
    u32 max_frames_queued = 4;
};

// The Host a wrapped GPU is built with when its writes into guest memory are to be run by
// InvalidateRegion: everything is forwarded to the real host, except that Post keeps a list of the
// work not yet run. Thread-safe like Host::Post and SignalInterrupt. PhysicalPointer and
// VirtualToPhysical are forwarded as they are (the renderer thread reads guest memory through
// them).
//
// GuestPointer is for the scheduler thread only (gpu.h), and every call through a QueueHost is made
// by the wrapped GPU on the renderer thread by construction: it returns nullptr, and with
// MH4N_RHI_CHECKS on it calls the owner-rule handler (OwnerRule::WrongThread) first.
//
// Order: SignalInterrupt goes straight to the real host's queue, while InvalidateRegion runs the
// posted writes early on the scheduler thread, so a write of command N+1 can land before the
// interrupt of command N is delivered. The write of a command is still before its own interrupt
// (both are made by the renderer thread in that order, and a write only ever moves earlier).
class QueueHost final : public Host {
public:
    explicit QueueHost(Host& real) : real_(real) {}

    u8* PhysicalPointer(u32 paddr, u32 size) override { return real_.PhysicalPointer(paddr, size); }
    std::optional<u32> VirtualToPhysical(u32 vaddr) override {
        return real_.VirtualToPhysical(vaddr);
    }
    u8* GuestPointer(u32 vaddr, u32 size) override;
    void SignalInterrupt(Interrupt id, u64 delay_ns) override {
        real_.SignalInterrupt(id, delay_ns);
    }
    u64 InterruptDelayNs() const override { return real_.InterruptDelayNs(); }
    // Hands `work` to the real host's Post. The work runs once: at the real host's safe point, or
    // earlier in RunPostedUpTo, whichever comes first.
    void Post(std::function<void()> work) override;

    // The number of Post calls so far.
    u64 PostedMark() const;
    // Runs, on the calling thread (the scheduler thread), in order, the work of the first `mark`
    // Post calls that has not run yet.
    void RunPostedUpTo(u64 mark);
    // How many posted works the list still holds (the ones that ran are dropped at the next Post or
    // RunPostedUpTo).
    size_t PendingCount() const;

private:
    struct Task {
        std::atomic<bool> taken{false};
        std::function<void()> work;
    };
    static void Run(Task& task);
    void Trim();

    Host& real_;
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<Task>> pending_; // from absolute index `base_`
    u64 base_ = 0;
    u64 posted_ = 0;
};

class QueueGpu final : public Gpu, public GpuDebug {
public:
    // Builds the wrapped GPU on the renderer thread with a QueueHost around `host`.
    using InnerMaker = std::function<std::unique_ptr<Gpu>(Host&)>;
    QueueGpu(Host& host, const InnerMaker& make_inner, QueueConfig config = {});
    // Wraps a GPU that already exists (built on the calling thread, with whatever host it was
    // given).
    explicit QueueGpu(std::unique_ptr<Gpu> inner, QueueConfig config = {});
    // Finishes the queued work, destroys the wrapped GPU on the renderer thread and joins it.
    ~QueueGpu() override;

    QueueGpu(const QueueGpu&) = delete;
    QueueGpu& operator=(const QueueGpu&) = delete;

    // ---- gpu::Gpu ----
    void Execute(const GxCommand& command) override;
    void WriteReg(u32 addr, u32 value) override;
    u32 ReadReg(u32 addr) override;
    void SetBufferSwap(u32 screen, const FrameBufferInfo& info) override;
    void SetColorFill(u32 fill) override;
    void FlushRegion(u32 vaddr, u32 size) override;
    void InvalidateRegion(u32 vaddr, u32 size) override;
    void VBlank(u32 screen) override;

    // ---- gpu::GpuDebug (forwarded to the wrapped GPU when it has them) ----
    void LoadInitState(const InitState& state) override;
    std::optional<std::vector<RegisterWrite>> TakeRegisterWrites() override;
    void WriteBackColorBuffers() override;
    std::vector<Counter> TakeFrameCounters() override;
    std::optional<u64> TakeFrameGpuTimeNs() override;

    // Returns when everything queued so far is done. Posted work is not run.
    void WaitIdle();

    // The wrapped GPU, for a call that is about to be forwarded. With MH4N_RHI_CHECKS on it calls
    // the owner-rule handler when the calling thread is not the renderer thread; `call` names the
    // call for the message. Every forward of this class goes through it (a test calls it from
    // another thread to see the handler run).
    Gpu& ForwardTarget(const char* call);

    // The configuration in use: the one given, with its limits clamped.
    const QueueConfig& Config() const { return config_; }
    // The thread the wrapped GPU runs on.
    std::thread::id RendererThread() const { return renderer_id_; }
    // How many callers are blocked inside the wrapper right now.
    u32 BlockedCallers() const;
    // The posted-work list of the QueueHost, null for a QueueGpu built around an existing GPU.
    QueueHost* Proxy() const { return proxy_.get(); }

private:
    void Start(std::function<void()> make_inner);
    void RendererMain();
    // Puts `item` on the queue and returns its sequence number. `frame_end`: the item is a
    // VBlank(0), so the frame limit applies before it is queued.
    u64 Enqueue(std::function<void()> item, bool frame_end = false);
    // Returns when item `seq` is done; the time spent is counted as waiting when `count`.
    void WaitFor(u64 seq, bool count = true);
    // Runs `fn` on the renderer thread, after everything queued, and waits; rethrows its exception.
    void RunSync(const std::function<void()>& fn, bool count = false);
    // After an asynchronous call: in lockstep mode, waits for it.
    void Finished(u64 seq);
    void CollectFrameStats();
    GpuDebug* Debug() const { return inner_debug_; }

    const QueueConfig config_;
    std::unique_ptr<QueueHost> proxy_; // before inner_: the wrapped GPU is destroyed first
    std::unique_ptr<Gpu> inner_;       // renderer thread only, once the thread runs
    GpuDebug* inner_debug_ = nullptr;
    std::exception_ptr start_error_;

    mutable std::mutex mutex_;
    std::condition_variable work_cv_; // the renderer waits for work
    std::condition_variable done_cv_; // callers wait for completion
    std::deque<std::function<void()>> queue_;
    u64 enqueued_ = 0;
    u64 completed_ = 0;
    u32 frames_in_flight_ = 0;
    u32 blocked_ = 0;
    bool stopping_ = false;
    bool started_ = false;

    std::atomic<u64> busy_ns_{0};
    std::atomic<u64> wait_ns_{0};
    std::atomic<u64> frame_waits_{0};

    std::mutex regs_mutex_;
    std::unordered_map<u32, u32> regs_;

    std::mutex stash_mutex_;
    std::vector<Counter> stash_counters_;
    std::optional<u64> stash_gpu_ns_;

    std::thread::id renderer_id_;
    std::thread thread_;
};

} // namespace mh4n::gpu::queue
