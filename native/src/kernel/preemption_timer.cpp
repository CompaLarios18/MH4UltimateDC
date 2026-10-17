// The real preemption source (N1-06): one host thread that waits for the deadline and raises the
// scheduler's flag. It never switches fibers and never touches guest memory or kernel objects (the
// trap of the spec): all it calls is the `raise` function it was given.
#include <thread>

#include "mh4n/scheduler.h"

namespace mh4n {

namespace {

class TimerThreadPreemption final : public PreemptionSource {
public:
    explicit TimerThreadPreemption(TimeSource& source) : source_(source) {}
    ~TimerThreadPreemption() override { Stop(); }

    void Start(std::function<void()> raise) override {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) {
            return;
        }
        raise_ = std::move(raise);
        stop_ = false;
        thread_ = std::thread([this] { Loop(); });
    }

    void Stop() override {
        std::thread thread;
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
            thread = std::move(thread_);
        }
        cv_.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }

    void Arm(std::optional<s64> deadline_ns) override {
        {
            std::lock_guard lock(mutex_);
            deadline_ = deadline_ns;
            ++generation_;
        }
        cv_.notify_all();
    }

private:
    void Loop() {
        std::unique_lock lock(mutex_);
        while (!stop_) {
            const u64 generation = generation_;
            const std::optional<s64> deadline = deadline_;
            // Until the request changes or, with a deadline, the deadline passes.
            const bool changed = source_.WaitUntil(cv_, lock, deadline, [&] {
                return stop_ || generation_ != generation;
            });
            if (stop_) {
                break;
            }
            if (!changed && deadline) {
                // One shot: the request is spent. Raised without our lock, so that the scheduler can
                // Arm again from inside whatever the flag makes it do.
                deadline_.reset();
                ++generation_;
                lock.unlock();
                raise_();
                lock.lock();
            }
        }
    }

    TimeSource& source_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<s64> deadline_;
    u64 generation_ = 0;
    bool stop_ = false;
    std::function<void()> raise_;
    std::thread thread_;
};

}  // namespace

std::unique_ptr<PreemptionSource> MakeTimerThreadPreemption(TimeSource& source) {
    return std::make_unique<TimerThreadPreemption>(source);
}

}  // namespace mh4n
