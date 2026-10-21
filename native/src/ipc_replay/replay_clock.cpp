// Our own code (no Azahar source): the time and preemption sources of a replay runtime.
#include "ipc_replay/replay_clock.h"

#include <algorithm>
#include <atomic>

namespace mh4n::ipc_replay {

struct ReplayTimeSource::State {
    std::atomic<s64> now{0};
    mutable std::mutex mutex;
    std::optional<s64> armed;  // guarded by mutex
};

namespace {

// Keeps the request the scheduler makes for its earliest timer. It never raises the flag: the replay
// runs the timers itself, by moving the clock and calling RunPending.
class RecordingPreemption final : public PreemptionSource {
public:
    explicit RecordingPreemption(std::shared_ptr<ReplayTimeSource::State> state) : state_(std::move(state)) {}

    void Start(std::function<void()>) override {}
    void Stop() override {}
    void Arm(std::optional<s64> deadline_ns) override {
        std::lock_guard lock(state_->mutex);
        state_->armed = deadline_ns;
    }

private:
    std::shared_ptr<ReplayTimeSource::State> state_;
};

}  // namespace

ReplayTimeSource::ReplayTimeSource() : state_(std::make_shared<State>()) {}

s64 ReplayTimeSource::NowNs() {
    return state_->now.load(std::memory_order_acquire);
}

void ReplayTimeSource::SetNs(s64 ns) {
    s64 now = state_->now.load(std::memory_order_acquire);
    while (now < ns && !state_->now.compare_exchange_weak(now, ns, std::memory_order_acq_rel)) {
    }
}

bool ReplayTimeSource::WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                                 std::optional<s64> deadline_ns, const std::function<bool()>& pred) {
    if (pred()) {
        return true;
    }
    if (!deadline_ns) {
        cv.wait(lock, pred);
        return true;
    }
    SetNs(*deadline_ns);
    return pred();
}

std::optional<s64> ReplayTimeSource::NextDeadlineNs() const {
    std::lock_guard lock(state_->mutex);
    return state_->armed;
}

std::unique_ptr<PreemptionSource> ReplayTimeSource::MakePreemption() {
    return std::make_unique<RecordingPreemption>(state_);
}

void RequestTimers::BeginRequest() {
    std::lock_guard lock(mutex_);
    ++request_;
    arming_for_ = request_;
}

void RequestTimers::EndRequest() {
    std::lock_guard lock(mutex_);
    arming_for_ = 0;
}

std::optional<u64> RequestTimers::EarliestOwnTick() const {
    std::lock_guard lock(mutex_);
    std::optional<u64> earliest;
    for (const auto& [id, own] : pending_) {
        if (own.request == request_ && (!earliest || own.tick < *earliest)) {
            earliest = own.tick;
        }
    }
    return earliest;
}

u64 RequestTimers::ScheduleAt(u64 tick, std::function<void()> fn) {
    u64 request = 0;
    {
        std::lock_guard lock(mutex_);
        request = arming_for_;
    }
    if (request == 0) {
        return inner_.ScheduleAt(tick, std::move(fn));
    }
    // The id is known only after the call: the wrapper reads it from a cell filled in before the timer
    // can run (an event that is due runs at the next safe point, never inside ScheduleAt).
    auto cell = std::make_shared<u64>(0);
    const u64 id = inner_.ScheduleAt(
        tick, [this, cell, request, fn = std::move(fn)] { Fire(*cell, request, fn); });
    *cell = id;
    std::lock_guard lock(mutex_);
    pending_[id] = Own{tick, request};
    return id;
}

bool RequestTimers::Cancel(u64 id) {
    {
        std::lock_guard lock(mutex_);
        pending_.erase(id);
    }
    return inner_.Cancel(id);
}

// The timer runs: it is no longer pending, and what it arms belongs to the same request.
void RequestTimers::Fire(u64 id, u64 request, const std::function<void()>& fn) {
    u64 before = 0;
    {
        std::lock_guard lock(mutex_);
        pending_.erase(id);
        before = arming_for_;
        arming_for_ = request;
    }
    struct Restore {
        RequestTimers& timers;
        u64 value;
        ~Restore() {
            std::lock_guard lock(timers.mutex_);
            timers.arming_for_ = value;
        }
    } restore{*this, before};
    fn();
}

RuntimeConfig MakeReplayRuntimeConfig() {
    auto source = std::make_unique<ReplayTimeSource>();
    RuntimeConfig config;
    config.preemption = source->MakePreemption();
    config.time = std::move(source);
    return config;
}

}  // namespace mh4n::ipc_replay
