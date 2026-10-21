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

ReplayTimeSource::~ReplayTimeSource() = default;

RequestTimers* ReplayTimeSource::InstallTimers(Kernel& kernel) {
    if (timers_ == nullptr) {
        timers_ = std::make_unique<RequestTimers>(kernel.GetScheduler());
        kernel_ = &kernel;
        kernel.SetScheduler(timers_.get());
    }
    return kernel_ == &kernel ? timers_.get() : nullptr;
}

RequestTimers::RequestTimers(Scheduler& inner) : inner_(inner), book_(std::make_shared<Book>()) {}

void RequestTimers::SetTracking(bool on) {
    std::lock_guard lock(book_->mutex);
    book_->tracking.store(on, std::memory_order_release);
    book_->pending.clear();
    book_->arming_for = 0;
}

void RequestTimers::BeginRequest() {
    std::lock_guard lock(book_->mutex);
    if (!book_->tracking.load(std::memory_order_acquire)) {
        return;
    }
    ++book_->request;
    book_->arming_for = book_->request;
    // Only the current request's timers are looked at, and a timer that runs erases its own entry
    // if it has one: the older entries can go.
    book_->pending.clear();
}

void RequestTimers::EndRequest() {
    std::lock_guard lock(book_->mutex);
    book_->arming_for = 0;
}

std::optional<u64> RequestTimers::EarliestOwnTick() const {
    std::lock_guard lock(book_->mutex);
    if (!book_->tracking.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::optional<u64> earliest;
    for (const auto& [id, own] : book_->pending) {
        if (own.request == book_->request && (!earliest || own.tick < *earliest)) {
            earliest = own.tick;
        }
    }
    return earliest;
}

u64 RequestTimers::ScheduleAt(u64 tick, std::function<void()> fn) {
    u64 request = 0;
    {
        std::lock_guard lock(book_->mutex);
        if (book_->tracking.load(std::memory_order_acquire)) {
            request = book_->arming_for;
        }
    }
    if (request == 0) {
        return inner_.ScheduleAt(tick, std::move(fn));
    }
    // The id is known only after the call: the wrapper reads it from a cell filled in before the timer
    // can run (an event that is due runs at the next safe point, never inside ScheduleAt).
    auto cell = std::make_shared<u64>(0);
    const u64 id = inner_.ScheduleAt(tick, [book = book_, cell, request, fn = std::move(fn)] {
        Fire(book, *cell, request, fn);
    });
    *cell = id;
    std::lock_guard lock(book_->mutex);
    book_->pending[id] = Own{tick, request};
    return id;
}

bool RequestTimers::Cancel(u64 id) {
    {
        std::lock_guard lock(book_->mutex);
        book_->pending.erase(id);
    }
    return inner_.Cancel(id);
}

// The timer runs: it is no longer pending, and what it arms belongs to the same request. Outside a
// replay it is just its function.
void RequestTimers::Fire(const std::shared_ptr<Book>& book, u64 id, u64 request,
                         const std::function<void()>& fn) {
    if (!book->tracking.load(std::memory_order_acquire)) {
        fn();
        return;
    }
    u64 before = 0;
    {
        std::lock_guard lock(book->mutex);
        book->pending.erase(id);
        before = book->arming_for;
        book->arming_for = request;
    }
    struct Restore {
        Book& book;
        u64 value;
        ~Restore() {
            std::lock_guard lock(book.mutex);
            book.arming_for = value;
        }
    } restore{*book, before};
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
