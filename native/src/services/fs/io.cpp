// The I/O threads of the file system services and the timing of their replies (N1-12, D22, D26).
//
// A read runs on an I/O thread; its reply is posted back to the scheduler thread, where it is written
// into the client's command buffer and buffers. With `--io-latency azahar` the reply is held until the
// game clock has passed the request's time plus Azahar's modelled delay (the clock, not the host's
// wall clock: a host that reads fast or slow gives the same answer tick), and, if the host is slower
// than that, until the data is read.
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "fs_state.h"
#include "mh4n/clock.h"
#include "mh4n/log.h"

namespace mh4n::fs {

namespace {
constexpr const char* kLog = "Service.FS";
constexpr unsigned kIoThreads = 2;
}  // namespace

bool ParseIoLatency(const std::string& text, IoLatency* out) {
    if (text == "azahar") {
        *out = IoLatency::Azahar;
        return true;
    }
    if (text == "none") {
        *out = IoLatency::None;
        return true;
    }
    return false;
}

// ---- the pool -------------------------------------------------------------------------------------------------------

class IoPool {
public:
    explicit IoPool(Kernel& kernel) : kernel_(kernel) {}
    ~IoPool() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        wake_.notify_all();
        for (std::thread& thread : threads_) {
            thread.join();
        }
    }
    IoPool(const IoPool&) = delete;
    IoPool& operator=(const IoPool&) = delete;

    // Scheduler thread (the threads are started on the first job). A job that has not started when the
    // pool is destroyed is dropped.
    void Submit(std::function<void()> job) {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        queue_.push_back(std::move(job));
        if (threads_.size() < kIoThreads && threads_.size() < queue_.size() + busy_) {
            threads_.emplace_back([this] { Loop(); });
        }
        wake_.notify_one();
    }

    // Any thread: hands `fn` to the scheduler thread, unless the pool is being destroyed. The mutex is
    // held while the kernel is asked, so a pool that is closing cannot be in the middle of it.
    void PostToScheduler(std::function<void()> fn) {
        std::lock_guard lock(mutex_);
        if (!closed_) {
            kernel_.Post(std::move(fn));
        }
    }

private:
    void Loop() {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return closed_ || !queue_.empty(); });
            if (closed_) {
                return;
            }
            std::function<void()> job = std::move(queue_.front());
            queue_.pop_front();
            ++busy_;
            lock.unlock();
            try {
                job();
            } catch (const std::exception& e) {
                MH4N_LOG(Error, kLog, "an I/O job threw: {}", e.what());
            } catch (...) {
                MH4N_LOG(Error, kLog, "an I/O job threw");
            }
            lock.lock();
            --busy_;
        }
    }

    Kernel& kernel_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    std::size_t busy_ = 0;
    bool closed_ = false;
};

void MakeIoPool(FsState& state, Kernel& kernel) {
    state.pool = std::make_unique<IoPool>(kernel);
}

FsState::FsState() = default;
FsState::~FsState() = default;

// ---- an operation -------------------------------------------------------------------------------------------

namespace {

// Joins the two things a reply waits for. Both marks come on the scheduler thread, in either order.
class Gate {
public:
    Gate(std::function<void()> finish, bool due, bool done)
        : finish_(std::move(finish)), due_(due), done_(done) {}
    void MarkDue() {
        due_ = true;
        Check();
    }
    void MarkDone() {
        done_ = true;
        Check();
    }

private:
    void Check() {
        if (due_ && done_ && finish_) {
            std::function<void()> finish = std::move(finish_);
            finish_ = nullptr;
            finish();
        }
    }
    std::function<void()> finish_;
    bool due_;
    bool done_;
};

}  // namespace

void StartIo(FsState& fs, ServiceContext& context, u64 delay_ns, std::function<void()> work,
             std::function<void()> finish) {
    Kernel& kernel = context.GetKernel();
    const bool waits_for_clock = NeedsDelay(fs, delay_ns);
    auto gate = std::make_shared<Gate>(std::move(finish), !waits_for_clock, !work);
    if (waits_for_clock) {
        // The deadline is an exact tick of the game clock: the request's time plus the delay.
        kernel.ScheduleAt(kernel.NowTicks() + NsToTicks(delay_ns), [gate] { gate->MarkDue(); });
    }
    if (work) {
        IoPool* pool = fs.pool.get();
        std::atomic<int>* in_flight = &fs.io_in_flight;
        in_flight->fetch_add(1);
        pool->Submit([pool, in_flight, gate, work = std::move(work)] {
            try {
                work();
            } catch (const std::exception& e) {
                MH4N_LOG(Error, kLog, "an I/O operation threw: {}", e.what());
            } catch (...) {
                MH4N_LOG(Error, kLog, "an I/O operation threw");
            }
            pool->PostToScheduler([gate] { gate->MarkDone(); });
            in_flight->fetch_sub(1);
        });
    } else if (!waits_for_clock) {
        gate->MarkDone();  // nothing to wait for: finish now
    }
}

}  // namespace mh4n::fs
