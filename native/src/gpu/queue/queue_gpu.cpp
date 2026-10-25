// The renderer thread and its queue (N2-22); see queue_gpu.h for what each call means.
#include "gpu/queue/queue_gpu.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "gpu/backend/rhi.h"
#include "mh4n/log.h"

namespace mh4n::gpu::queue {

namespace {

using Clock = std::chrono::steady_clock;

u64 ElapsedNs(Clock::time_point since) {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
}

// Adds `more` to `into`, summing counters of the same name.
void MergeCounters(std::vector<Counter>& into, std::vector<Counter> more) {
    for (Counter& counter : more) {
        const auto it = std::find_if(into.begin(), into.end(),
                                     [&](const Counter& c) { return c.name == counter.name; });
        if (it == into.end()) {
            into.push_back(std::move(counter));
        } else {
            it->value += counter.value;
        }
    }
}

} // namespace

// ---- QueueHost -------------------------------------------------------------------------------------

void QueueHost::Run(Task& task) {
    if (!task.taken.exchange(true, std::memory_order_acq_rel)) {
        std::function<void()> work = std::move(task.work);
        task.work = nullptr;
        if (work) {
            work();
        }
    }
}

void QueueHost::Post(std::function<void()> work) {
    auto task = std::make_shared<Task>();
    task->work = std::move(work);
    {
        std::lock_guard lock(mutex_);
        // Forget what has run (the real host's safe point runs a task without telling us).
        while (!pending_.empty() && pending_.front()->taken.load(std::memory_order_acquire)) {
            pending_.pop_front();
            ++base_;
        }
        pending_.push_back(task);
        ++posted_;
    }
    // The closure holds the task, not this object: the real host may run it after we are gone.
    real_.Post([task] { Run(*task); });
}

u64 QueueHost::PostedMark() const {
    std::lock_guard lock(mutex_);
    return posted_;
}

void QueueHost::RunPostedUpTo(u64 mark) {
    std::vector<std::shared_ptr<Task>> due;
    {
        std::lock_guard lock(mutex_);
        const u64 end = std::min(mark, posted_);
        for (u64 index = base_; index < end; ++index) {
            due.push_back(pending_[static_cast<size_t>(index - base_)]);
        }
    }
    for (const auto& task : due) {
        Run(*task);
    }
}

// ---- QueueGpu: life cycle --------------------------------------------------------------------------

QueueGpu::QueueGpu(Host& host, const InnerMaker& make_inner, QueueConfig config)
    : config_(config), proxy_(std::make_unique<QueueHost>(host)) {
    Start([this, &make_inner] { inner_ = make_inner(*proxy_); });
}

QueueGpu::QueueGpu(std::unique_ptr<Gpu> inner, QueueConfig config) : config_(config) {
    Start([this, &inner] { inner_ = std::move(inner); });
}

void QueueGpu::Start(std::function<void()> make_inner) {
    thread_ = std::thread([this, make = std::move(make_inner)] {
        renderer_id_ = std::this_thread::get_id();
        try {
            make();
            if (inner_ == nullptr) {
                throw std::runtime_error("the wrapped GPU could not be created");
            }
            inner_debug_ = dynamic_cast<GpuDebug*>(inner_.get());
        } catch (...) {
            start_error_ = std::current_exception();
        }
        {
            std::lock_guard lock(mutex_);
            started_ = true;
        }
        done_cv_.notify_all();
        if (start_error_) {
            return;
        }
        RendererMain();
    });
    {
        std::unique_lock lock(mutex_);
        done_cv_.wait(lock, [this] { return started_; });
    }
    if (start_error_) {
        thread_.join();
        std::rethrow_exception(start_error_);
    }
}

QueueGpu::~QueueGpu() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void QueueGpu::RendererMain() {
    for (;;) {
        std::function<void()> item;
        {
            std::unique_lock lock(mutex_);
            work_cv_.wait(lock, [this] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) {
                break; // stopping, and everything queued has run
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        const Clock::time_point begin = Clock::now();
        try {
            item();
        } catch (const std::exception& e) {
            MH4N_LOG(Error, "GPU", "queue: a call of the wrapped GPU threw: {}", e.what());
        } catch (...) {
            MH4N_LOG(Error, "GPU", "queue: a call of the wrapped GPU threw something that is not an exception");
        }
        busy_ns_.fetch_add(ElapsedNs(begin), std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex_);
            ++completed_;
        }
        done_cv_.notify_all();
    }
    // The wrapped GPU's own destruction is the renderer thread's too.
    inner_debug_ = nullptr;
    inner_.reset();
}

// ---- QueueGpu: the queue ---------------------------------------------------------------------------

u64 QueueGpu::Enqueue(std::function<void()> item, bool frame_end) {
    std::unique_lock lock(mutex_);
    if (frame_end && frames_in_flight_ >= std::max<u32>(config_.max_frames_in_flight, 1)) {
        const Clock::time_point begin = Clock::now();
        ++blocked_;
        done_cv_.wait(lock, [this] {
            return frames_in_flight_ < std::max<u32>(config_.max_frames_in_flight, 1);
        });
        --blocked_;
        frame_waits_.fetch_add(1, std::memory_order_relaxed);
        wait_ns_.fetch_add(ElapsedNs(begin), std::memory_order_relaxed);
    }
    if (frame_end) {
        ++frames_in_flight_;
    }
    queue_.push_back(std::move(item));
    const u64 seq = ++enqueued_;
    lock.unlock();
    work_cv_.notify_one();
    return seq;
}

void QueueGpu::WaitFor(u64 seq, bool count) {
    std::unique_lock lock(mutex_);
    if (completed_ >= seq) {
        return;
    }
    const Clock::time_point begin = Clock::now();
    ++blocked_;
    done_cv_.wait(lock, [this, seq] { return completed_ >= seq; });
    --blocked_;
    if (count) {
        wait_ns_.fetch_add(ElapsedNs(begin), std::memory_order_relaxed);
    }
}

void QueueGpu::Finished(u64 seq) {
    if (config_.lockstep) {
        WaitFor(seq);
    }
}

void QueueGpu::RunSync(const std::function<void()>& fn, bool count) {
    std::exception_ptr error;
    const u64 seq = Enqueue([&fn, &error] {
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
    });
    WaitFor(seq, count);
    if (error) {
        std::rethrow_exception(error);
    }
}

void QueueGpu::WaitIdle() {
    u64 seq;
    {
        std::lock_guard lock(mutex_);
        seq = enqueued_;
    }
    WaitFor(seq, false);
}

u32 QueueGpu::BlockedCallers() const {
    std::lock_guard lock(mutex_);
    return blocked_;
}

Gpu& QueueGpu::ForwardTarget(const char* call) {
#if MH4N_RHI_CHECKS
    if (std::this_thread::get_id() != renderer_id_) {
        rhi::ReportOwnerRuleBreach(
            rhi::OwnerRule::WrongThread,
            fmt::format("QueueGpu::{} was forwarded to the wrapped GPU on a thread other than the "
                        "renderer thread (the renderer thread owns every image the wrapped GPU makes)",
                        call));
    }
#else
    (void)call;
#endif
    return *inner_;
}

// ---- gpu::Gpu ----------------------------------------------------------------------------------------

void QueueGpu::Execute(const GxCommand& command) {
    Finished(Enqueue([this, command] { ForwardTarget("Execute").Execute(command); }));
}

void QueueGpu::WriteReg(u32 addr, u32 value) {
    {
        std::lock_guard lock(regs_mutex_);
        regs_[addr] = value;
    }
    Finished(Enqueue([this, addr, value] { ForwardTarget("WriteReg").WriteReg(addr, value); }));
}

u32 QueueGpu::ReadReg(u32 addr) {
    {
        std::lock_guard lock(regs_mutex_);
        const auto it = regs_.find(addr);
        if (it != regs_.end()) {
            return it->second;
        }
    }
    // Never written through the wrapper: only the wrapped GPU knows it.
    u32 value = 0;
    RunSync([this, addr, &value] { value = ForwardTarget("ReadReg").ReadReg(addr); }, true);
    return value;
}

void QueueGpu::SetBufferSwap(u32 screen, const FrameBufferInfo& info) {
    Finished(Enqueue([this, screen, info] { ForwardTarget("SetBufferSwap").SetBufferSwap(screen, info); }));
}

void QueueGpu::SetColorFill(u32 fill) {
    Finished(Enqueue([this, fill] { ForwardTarget("SetColorFill").SetColorFill(fill); }));
}

void QueueGpu::FlushRegion(u32 vaddr, u32 size) {
    // In order with the commands: the region is stale before any later command runs.
    Finished(Enqueue([this, vaddr, size] { ForwardTarget("FlushRegion").FlushRegion(vaddr, size); }));
}

void QueueGpu::InvalidateRegion(u32 vaddr, u32 size) {
    // The renderer thread forwards it after every earlier command; the posted work of those commands
    // (and of the wrapped GPU's own write-back for this call) is what the mark counts.
    auto mark = std::make_shared<u64>(0);
    const u64 seq = Enqueue([this, vaddr, size, mark] {
        ForwardTarget("InvalidateRegion").InvalidateRegion(vaddr, size);
        if (proxy_) {
            *mark = proxy_->PostedMark();
        }
    });
    WaitFor(seq);
    if (proxy_) {
        // The caller is the scheduler thread, at a safe point: the writes land before it reads.
        proxy_->RunPostedUpTo(*mark);
    }
}

void QueueGpu::VBlank(u32 screen) {
    const bool frame_end = screen == 0;
    Finished(Enqueue(
        [this, screen, frame_end] {
            // The frame leaves the queue even if the wrapped GPU throws, or the limit would block for ever.
            struct FrameDone {
                QueueGpu* gpu;
                bool active;
                ~FrameDone() {
                    if (!active) {
                        return;
                    }
                    {
                        std::lock_guard lock(gpu->mutex_);
                        --gpu->frames_in_flight_;
                    }
                    gpu->done_cv_.notify_all();
                }
            } done{this, frame_end};
            ForwardTarget("VBlank").VBlank(screen);
            if (frame_end) {
                CollectFrameStats();
            }
        },
        frame_end));
}

// ---- gpu::GpuDebug -----------------------------------------------------------------------------------

// The counters of the wrapped GPU for the frame that just ended, taken on the renderer thread at its
// VBlank(0) so that the runtime can read them without waiting for the queue.
void QueueGpu::CollectFrameStats() {
    if (config_.lockstep || Debug() == nullptr) {
        return;
    }
    std::vector<Counter> counters = Debug()->TakeFrameCounters();
    const std::optional<u64> gpu_ns = Debug()->TakeFrameGpuTimeNs();
    std::lock_guard lock(stash_mutex_);
    MergeCounters(stash_counters_, std::move(counters));
    if (gpu_ns) {
        stash_gpu_ns_ = stash_gpu_ns_.value_or(0) + *gpu_ns;
    }
}

void QueueGpu::LoadInitState(const InitState& state) {
    // The spans are valid only during the call: run it now, on the renderer thread.
    RunSync([this, &state] {
        ForwardTarget("LoadInitState");
        if (Debug() != nullptr) {
            Debug()->LoadInitState(state);
        }
    });
    // The register file is the capture's now: the mirror knows only what was written through us.
    std::lock_guard lock(regs_mutex_);
    regs_.clear();
}

std::optional<std::vector<RegisterWrite>> QueueGpu::TakeRegisterWrites() {
    std::optional<std::vector<RegisterWrite>> writes;
    RunSync([this, &writes] {
        ForwardTarget("TakeRegisterWrites");
        if (Debug() != nullptr) {
            writes = Debug()->TakeRegisterWrites();
        }
    });
    return writes;
}

void QueueGpu::WriteBackColorBuffers() {
    RunSync([this] {
        ForwardTarget("WriteBackColorBuffers");
        if (Debug() != nullptr) {
            Debug()->WriteBackColorBuffers();
        }
    });
}

std::vector<Counter> QueueGpu::TakeFrameCounters() {
    std::vector<Counter> counters;
    if (config_.lockstep) {
        RunSync([this, &counters] {
            ForwardTarget("TakeFrameCounters");
            if (Debug() != nullptr) {
                counters = Debug()->TakeFrameCounters();
            }
        });
    } else {
        std::lock_guard lock(stash_mutex_);
        counters = std::move(stash_counters_);
        stash_counters_.clear();
    }
    counters.push_back({"translate_us", busy_ns_.exchange(0, std::memory_order_relaxed) / 1000});
    counters.push_back({"queue_wait_us", wait_ns_.exchange(0, std::memory_order_relaxed) / 1000});
    counters.push_back({"queue_frame_waits", frame_waits_.exchange(0, std::memory_order_relaxed)});
    return counters;
}

std::optional<u64> QueueGpu::TakeFrameGpuTimeNs() {
    if (config_.lockstep) {
        std::optional<u64> ns;
        RunSync([this, &ns] {
            ForwardTarget("TakeFrameGpuTimeNs");
            if (Debug() != nullptr) {
                ns = Debug()->TakeFrameGpuTimeNs();
            }
        });
        return ns;
    }
    std::lock_guard lock(stash_mutex_);
    return std::exchange(stash_gpu_ns_, std::nullopt);
}

} // namespace mh4n::gpu::queue
