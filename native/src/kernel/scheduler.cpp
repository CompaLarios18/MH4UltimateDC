// Derived from Azahar src/core/hle/kernel/thread.cpp, src/core/hle/kernel/svc.cpp, src/core/core.cpp
// and src/common/thread_queue_list.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// svc.cpp and core.cpp say so; thread.cpp and thread_queue_list.h say GPL-2.0-or-later): the rules,
// not the code. ThreadManager::PopNextReadyThread and SwitchContext (the running thread continues
// unless a strictly better priority is ready, a displaced thread goes back to the front of its
// queue, a woken one to the back, ThreadQueueList::pop_first_better and move), Reschedule, the
// sleep and wake-up paths and the CpuLimiter hook point, SVC::SleepThread (what 0 ns does), and
// SVC::WaitSynchronization1/N (what is acquired at once, when a timeout of 0 refuses, the index
// of a wait on any). The queues, fibers, host-thread structure, timer queue and clock are ours.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.cpp, svc.cpp,
//   core.cpp, thread_queue_list.h)
// Copyright 2014 PPSSPP Project (thread_queue_list.h, second notice: Licensed under GPLv2 or any
//   later version)
//
// The scheduler (N1-06): ready queues per virtual core, thread selection, blocking, the timer queue,
// safe points and the counters.
#include "mh4n/scheduler.h"

#include <algorithm>
#include <bit>

#include "mh4n/assert.h"
#include "mh4n/log.h"

namespace mh4n {

struct GuestScheduler::SchedThread {
    struct Category {
        enum class Kind { Guest, Svc, Service } kind = Kind::Guest;
        u32 svc = 0;
        SchedulerCounters::Named* service = nullptr;
    };

    u32 thread_id = 0;
    std::unique_ptr<GuestThread> guest;
    std::optional<Result> wake_result;
    s32 wake_index = -1;
    // The ready queue the thread is in (priority and core as they were when it was queued).
    bool in_queue = false;
    u32 queue_priority = 0;
    int queue_core = 0;
    Category cat;
    std::vector<Category> saved_categories;  // under BeginService
    u64* guest_ns = nullptr;
};

// ---- construction ------------------------------------------------------------------------------------

GuestScheduler::GuestScheduler(Kernel& kernel, CodeHost& host, GameClock& clock, TimeSource& time,
                               PreemptionSource* preemption, Options options)
    : kernel_(kernel),
      host_(host),
      clock_(clock),
      time_(time),
      preemption_(preemption),
      slice_us_(options.slice_us) {
    kernel_.SetScheduler(this);
    kernel_.SetClock(&clock_);
    host_.SetBudgetHandler([this](RCtx&) { Preempt(); });
    clock_.SetChangeCallback([this] { RaiseFlag(); });
    if (preemption_ != nullptr) {
        preemption_->Start([this] { RaiseFlag(); });
    }
}

GuestScheduler::~GuestScheduler() {
    if (preemption_ != nullptr) {
        preemption_->Stop();
    }
    clock_.SetChangeCallback({});
    host_.SetBudgetHandler({});
    Shutdown();
}

void GuestScheduler::Shutdown() {
    for (auto& [raw, thread] : threads_) {
        thread->CancelTimeout();
    }
    for (Core& core : cores_) {
        for (auto& queue : core.ready) {
            queue.clear();
        }
        core.mask = 0;
        core.current = nullptr;
    }
    timers_.clear();
    timer_ticks_.clear();
    graveyard_.clear();
    threads_.clear();
    acct_ = nullptr;
}

// ---- per-thread data and the ready queues ------------------------------------------------------------

GuestScheduler::SchedThread& GuestScheduler::Sd(Thread& thread) {
    if (!thread.scheduler_data) {
        auto data = std::make_shared<SchedThread>();
        data->thread_id = thread.thread_id;
        thread.scheduler_data = std::move(data);
    }
    return *static_cast<SchedThread*>(thread.scheduler_data.get());
}

int GuestScheduler::CoreOf(const Thread& thread) const {
    if (thread.processor_id == 0 || thread.processor_id == 1) {
        return thread.processor_id;
    }
    MH4N_LOG(Error, "Scheduler", "thread {} has processor {}: only cores 0 and 1 exist, using 0",
             thread.thread_id, thread.processor_id);
    return 0;
}

void GuestScheduler::Enqueue(Thread& thread, bool front) {
    SchedThread& sd = Sd(thread);
    MH4N_ASSERT(!sd.in_queue, "thread {} is already in a ready queue", thread.thread_id);
    const int core = CoreOf(thread);
    const u32 priority = thread.current_priority;
    auto& queue = cores_[core].ready[priority];
    if (front) {
        queue.push_front(&thread);
    } else {
        queue.push_back(&thread);
    }
    cores_[core].mask |= u64{1} << priority;
    sd.in_queue = true;
    sd.queue_priority = priority;
    sd.queue_core = core;
}

void GuestScheduler::Dequeue(Thread& thread) {
    SchedThread& sd = Sd(thread);
    if (!sd.in_queue) {
        return;
    }
    Core& core = cores_[sd.queue_core];
    auto& queue = core.ready[sd.queue_priority];
    queue.erase(std::remove(queue.begin(), queue.end(), &thread), queue.end());
    if (queue.empty()) {
        core.mask &= ~(u64{1} << sd.queue_priority);
    }
    sd.in_queue = false;
}

bool GuestScheduler::Runnable(int core) const {
    return cores_[core].current != nullptr || cores_[core].mask != 0;
}

// Something on `core` that should run now: a thread on an idle core, or one better than what the core
// has. Real cores run in parallel, so it does not wait for its core's turn in the slices.
bool GuestScheduler::Urgent(int core) const {
    const Core& c = cores_[core];
    if (c.mask == 0) {
        return false;
    }
    if (c.current == nullptr) {
        return true;
    }
    return static_cast<u32>(std::countr_zero(c.mask)) < c.current->current_priority;
}

// A core's current thread that stopped running (it blocked, ended or was queued again) is not on
// the core any more.
void GuestScheduler::Normalize(Core& core) {
    if (core.current != nullptr && core.current->status != ThreadStatus::Running) {
        core.current->last_running_ticks = clock_.Now();
        core.current = nullptr;
    }
}

Thread* GuestScheduler::PopNextReady(Core& core) {
    for (;;) {
        Thread* const current = core.current;
        Thread* next = nullptr;
        // Azahar's PopNextReadyThread: with a running thread only a strictly better priority
        // displaces it; with none, the best of the queues.
        if (core.mask != 0) {
            const u32 best = static_cast<u32>(std::countr_zero(core.mask));
            if (current == nullptr || best < current->current_priority) {
                auto& queue = core.ready[best];
                next = queue.front();
                queue.pop_front();
                if (queue.empty()) {
                    core.mask &= ~(u64{1} << best);
                }
                Sd(*next).in_queue = false;
                if (next->status == ThreadStatus::Dead) {
                    // Ended without going through Terminate/ExitCurrent: it must not run.
                    MH4N_LOG(Error, "Scheduler", "dead thread {} was in a ready queue",
                             next->thread_id);
                    continue;
                }
            }
        }
        if (next == nullptr) {
            next = current;
        }
        if (next == nullptr) {
            return nullptr;
        }
        if (core.filter && core.filter(*next)) {
            // Held back (Azahar's DoTimeLimit): it is not Ready or Running any more.
            if (next == core.current) {
                core.current = nullptr;
            }
            continue;
        }
        return next;
    }
}

void GuestScheduler::ChooseCore(s64 now_ns) {
    Normalize(cores_[0]);
    Normalize(cores_[1]);
    const int a = active_;
    const int b = 1 - a;
    const bool run_a = Runnable(a);
    const bool run_b = Runnable(b);
    const bool expired = slice_active_ && now_ns >= slice_end_ns_;
    int pick = a;
    if (!run_a && run_b) {
        pick = b;
    } else if (run_a && run_b && (expired || (cores_[a].current != nullptr && Urgent(b)))) {
        // (A core that has not picked its thread yet is about to; it does not give way first.)
        pick = b;
    }
    const s64 slice_ns = static_cast<s64>(slice_us_) * 1000;
    if (pick != a) {
        if (cores_[a].current != nullptr) {
            ++counters_.preemptions;  // it could still run
        }
        active_ = pick;
        ++counters_.core_switches;
        slice_end_ns_ = now_ns + slice_ns;
    } else if (expired || !slice_active_) {
        slice_end_ns_ = now_ns + slice_ns;
    }
    slice_active_ = true;
}

Thread* GuestScheduler::Dispatch() {
    ChooseCore(time_.NowNs());
    Core& core = cores_[active_];
    Normalize(core);
    Thread* const previous = core.current;
    Thread* const next = PopNextReady(core);
    if (next != core.current) {
        // Azahar's SwitchContext: a displaced thread that could still run keeps its place at the
        // front of its queue.
        if (previous != nullptr && core.current == previous) {
            previous->status = ThreadStatus::Ready;
            previous->last_running_ticks = clock_.Now();
            Enqueue(*previous, /*front=*/true);
            ++counters_.preemptions;
        }
        if (next != nullptr) {
            next->status = ThreadStatus::Running;
        }
        core.current = next;
    }
    reschedule_pending_ = false;
    if (next == nullptr) {
        slice_active_ = false;
    }
    guest_running_ = next != nullptr;
    UpdateArm();
    return next;
}

// ---- the Scheduler interface of kernel.h -------------------------------------------------------------

void GuestScheduler::Wake(const std::shared_ptr<Thread>& thread, std::optional<Result> result,
                          s32 index) {
    MH4N_ASSERT(threads_.count(thread.get()) != 0, "thread {} was woken but never attached",
                thread->thread_id);
    SchedThread& sd = Sd(*thread);
    sd.wake_result = result;
    sd.wake_index = index;
    if (!sd.in_queue) {
        Enqueue(*thread, /*front=*/false);  // Azahar: ResumeFromWait pushes to the back
    }
    NeedReschedule(CoreOf(*thread));
}

void GuestScheduler::OnPriorityChanged(Thread& thread, u32) {
    // Azahar moves a Ready thread to the back of the queue of its new priority, also when nothing
    // changed.
    if (thread.scheduler_data != nullptr && Sd(thread).in_queue) {
        Dequeue(thread);
        Enqueue(thread, /*front=*/false);
    }
    NeedReschedule(CoreOf(thread));
}

void GuestScheduler::PrepareReschedule() {
    reschedule_pending_ = true;
}

void GuestScheduler::NeedReschedule(int core) {
    reschedule_pending_ = true;
    if (CodeHost::CurrentThread() == nullptr || in_pending_) {
        return;  // the host loop, or the Preempt that runs this, dispatches next
    }
    // Guest code is running: make it reach a preemption check if the change matters to it. On the
    // other core that is a thread that should run now; on this one, a better thread than the
    // running one (a call that is not an SVC, such as code driven by tests, has no EndSvc). The
    // running thread may be mid-block here (its status already says so but Dispatch has not
    // noticed), so a core's current thread counts only while it is Running.
    const Core& c = cores_[core];
    if (c.mask == 0) {
        return;
    }
    const u32 best = static_cast<u32>(std::countr_zero(c.mask));
    const bool running = c.current != nullptr && c.current->status == ThreadStatus::Running;
    if (!running || best < c.current->current_priority) {
        if (core != active_ || running) {
            RaiseFlag();
        }
    }
}

Thread* GuestScheduler::CurrentThread() {
    return CodeHost::CurrentThread() != nullptr ? cores_[active_].current : nullptr;
}

std::shared_ptr<Thread> GuestScheduler::CurrentThreadPtr() {
    Thread* thread = CurrentThread();
    if (thread == nullptr) {
        return nullptr;
    }
    const auto it = threads_.find(thread);
    return it != threads_.end() ? it->second : nullptr;
}

Thread* GuestScheduler::ThreadOnCore(int core) const {
    return cores_[core].current;
}

std::size_t GuestScheduler::ReadyCount(int core) const {
    std::size_t count = 0;
    for (const auto& queue : cores_[core].ready) {
        count += queue.size();
    }
    return count;
}

// ---- the timer queue and host work -------------------------------------------------------------------

u64 GuestScheduler::ScheduleAt(u64 tick, std::function<void()> fn) {
    const u64 id = next_timer_id_++;
    const auto front_before = timers_.empty() ? std::pair<u64, u64>{~u64{0}, 0} : timers_.begin()->first;
    timers_.emplace(std::make_pair(tick, id), std::move(fn));
    timer_ticks_.emplace(id, tick);
    if (tick <= clock_.Now()) {
        RaiseFlag();  // due: run it at the next safe point
    }
    if (timers_.begin()->first != front_before) {
        UpdateArm();
    }
    return id;
}

bool GuestScheduler::Cancel(u64 id) {
    const auto it = timer_ticks_.find(id);
    if (it == timer_ticks_.end()) {
        return false;
    }
    const bool was_front = timers_.begin()->first == std::make_pair(it->second, id);
    timers_.erase({it->second, id});
    timer_ticks_.erase(it);
    if (was_front) {
        UpdateArm();
    }
    return true;
}

void GuestScheduler::Post(std::function<void()> fn) {
    {
        std::lock_guard lock(mutex_);
        posted_.push_back(std::move(fn));
        __atomic_store_n(&preempt_flag_, 1u, __ATOMIC_RELEASE);
    }
    cv_.notify_all();
}

void GuestScheduler::RaiseFlag() {
    {
        // Under the lock, so that an idle scheduler between its test of the predicate and its wait
        // cannot miss the wake-up.
        std::lock_guard lock(mutex_);
        __atomic_store_n(&preempt_flag_, 1u, __ATOMIC_RELEASE);
    }
    cv_.notify_all();
}

void GuestScheduler::RequestStop() {
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
        __atomic_store_n(&preempt_flag_, 1u, __ATOMIC_RELEASE);
    }
    cv_.notify_all();
}

bool GuestScheduler::RunDueTimers() {
    const u64 now = clock_.Now();
    bool ran = false;
    while (!timers_.empty() && timers_.begin()->first.first <= now) {
        auto node = timers_.extract(timers_.begin());
        timer_ticks_.erase(node.key().second);
        node.mapped()();
        ran = true;
    }
    return ran;
}

bool GuestScheduler::ProcessPending(bool until_quiet) {
    MH4N_ASSERT(!in_pending_, "posted work or a timer event re-entered the scheduler");
    in_pending_ = true;
    bool ran = false;
    for (;;) {
        // Cleared first: anything raised after this point is seen at the next safe point.
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard lock(mutex_);
            __atomic_store_n(&preempt_flag_, 0u, __ATOMIC_RELEASE);
            batch.swap(posted_);
        }
        for (auto& fn : batch) {
            fn();
            ran = true;
        }
        const bool timers = RunDueTimers();
        ran = ran || timers;
        if (!until_quiet) {
            break;
        }
        std::lock_guard lock(mutex_);
        const bool more = !posted_.empty() ||
                          (!timers_.empty() && timers_.begin()->first.first <= clock_.Now());
        if (!more) {
            break;
        }
    }
    in_pending_ = false;
    return ran;
}

bool GuestScheduler::RunPending() {
    const bool ran = ProcessPending(/*until_quiet=*/true);
    UpdateArm();
    return ran;
}

void GuestScheduler::OnSafePoint(std::function<void()> hook) {
    hooks_.push_back(std::move(hook));
    UpdateArm();
}

void GuestScheduler::ClearSafePointHooks() {
    hooks_.clear();
    UpdateArm();
}

void GuestScheduler::RunHooks() {
    for (std::size_t i = 0; i < hooks_.size(); ++i) {
        const auto hook = hooks_[i];  // a hook may register another
        hook();
    }
}

void GuestScheduler::SetSelectionFilter(int core, SelectionFilter filter) {
    MH4N_ASSERT(core == 0 || core == 1, "core {}", core);
    cores_[core].filter = std::move(filter);
}

// The asynchronous source is asked for the earliest thing that needs a preemption check while a
// guest runs: the next timer, and the end of the slice if the other core has work or hooks wait.
void GuestScheduler::UpdateArm() {
    if (preemption_ == nullptr) {
        return;
    }
    std::optional<s64> deadline;
    if (!timers_.empty()) {
        deadline = clock_.HostNsFor(timers_.begin()->first.first);
    }
    if (guest_running_ && (Runnable(1 - active_) || !hooks_.empty())) {
        deadline = deadline ? std::min(*deadline, slice_end_ns_) : slice_end_ns_;
    }
    if (deadline != last_armed_) {
        last_armed_ = deadline;
        preemption_->Arm(deadline);
    }
}

// ---- counters ----------------------------------------------------------------------------------------

void GuestScheduler::AccountTo(SchedThread* to) {
    const s64 now = time_.NowNs();
    if (acct_ != nullptr && now > acct_start_ns_) {
        const u64 elapsed = static_cast<u64>(now - acct_start_ns_);
        switch (acct_->cat.kind) {
        case SchedThread::Category::Kind::Guest:
            if (acct_->guest_ns == nullptr) {
                acct_->guest_ns = &counters_.guest_ns[acct_->thread_id];
            }
            *acct_->guest_ns += elapsed;
            break;
        case SchedThread::Category::Kind::Svc:
            counters_.svc[acct_->cat.svc & 0xFF].ns += elapsed;
            break;
        case SchedThread::Category::Kind::Service:
            acct_->cat.service->ns += elapsed;
            break;
        }
    }
    acct_ = to;
    acct_start_ns_ = now;
}

void GuestScheduler::FlushCategory() {
    AccountTo(acct_);
}

void GuestScheduler::BeginSvc(u32 number) {
    Thread* thread = CurrentThread();
    MH4N_ASSERT(thread != nullptr, "BeginSvc outside guest code");
    SchedThread& sd = Sd(*thread);
    FlushCategory();
    sd.cat = {SchedThread::Category::Kind::Svc, number & 0xFF, nullptr};
    ++counters_.svc[number & 0xFF].calls;
    RunHooks();
}

void GuestScheduler::EndSvc() {
    if (__atomic_load_n(&preempt_flag_, __ATOMIC_ACQUIRE) != 0) {
        Preempt();
    } else if (reschedule_pending_) {
        Reschedule();
    }
    Thread* thread = CurrentThread();
    MH4N_ASSERT(thread != nullptr, "EndSvc outside guest code");
    FlushCategory();
    Sd(*thread).cat = {};
}

void GuestScheduler::BeginService(std::string_view name) {
    Thread* thread = CurrentThread();
    MH4N_ASSERT(thread != nullptr, "BeginService outside guest code");
    SchedThread& sd = Sd(*thread);
    auto it = counters_.services.find(name);
    if (it == counters_.services.end()) {
        it = counters_.services.emplace(std::string(name), SchedulerCounters::Named{}).first;
    }
    FlushCategory();
    sd.saved_categories.push_back(sd.cat);
    sd.cat = {SchedThread::Category::Kind::Service, 0, &it->second};
    ++it->second.calls;
}

void GuestScheduler::EndService() {
    Thread* thread = CurrentThread();
    MH4N_ASSERT(thread != nullptr, "EndService outside guest code");
    SchedThread& sd = Sd(*thread);
    MH4N_ASSERT(!sd.saved_categories.empty(), "EndService without BeginService");
    FlushCategory();
    sd.cat = sd.saved_categories.back();
    sd.saved_categories.pop_back();
}

// ---- threads -----------------------------------------------------------------------------------------

GuestThread& GuestScheduler::Attach(const std::shared_ptr<Thread>& thread,
                                    GuestThread::Entry entry) {
    SchedThread& sd = Sd(*thread);
    MH4N_ASSERT(sd.guest == nullptr, "thread {} already has a fiber", thread->thread_id);
    sd.guest = host_.NewThread(thread->thread_id, std::move(entry));
    threads_.emplace(thread.get(), thread);
    return *sd.guest;
}

GuestThread& GuestScheduler::AttachFunction(const std::shared_ptr<Thread>& thread,
                                            std::function<void()> body) {
    return Attach(thread, [this, body = std::move(body)](GuestThread&) {
        body();
        ExitCurrent();
    });
}

void GuestScheduler::Start(const std::shared_ptr<Thread>& thread) {
    MH4N_ASSERT(thread->status == ThreadStatus::Dormant, "Start of thread {}, which is not dormant",
                thread->thread_id);
    MH4N_ASSERT(threads_.count(thread.get()) != 0 && Sd(*thread).guest != nullptr,
                "Start of thread {}, which has no fiber", thread->thread_id);
    thread->status = ThreadStatus::Ready;
    Enqueue(*thread, /*front=*/false);
    NeedReschedule(CoreOf(*thread));
}

void GuestScheduler::ExitCurrent() {
    GuestThread* const self = CodeHost::CurrentThread();
    MH4N_ASSERT(self != nullptr, "ExitCurrent outside guest code");
    Thread* const thread = cores_[active_].current;
    MH4N_ASSERT(thread != nullptr, "ExitCurrent: no running thread");
    // Kept alive by a member until a thread other than this one reaps it: the fiber we are on
    // belongs to it.
    graveyard_.push_back(threads_.at(thread));
    thread->Stop();
    cores_[active_].current = nullptr;
    SwitchFromSelf(self);
    MH4N_UNREACHABLE();
}

void GuestScheduler::Terminate(const std::shared_ptr<Thread>& thread) {
    if (CodeHost::CurrentThread() != nullptr && cores_[active_].current == thread.get()) {
        ExitCurrent();
    }
    if (thread->status == ThreadStatus::Dead) {
        return;
    }
    Dequeue(*thread);
    for (Core& core : cores_) {
        if (core.current == thread.get()) {
            core.current = nullptr;
        }
    }
    if (threads_.count(thread.get()) != 0) {
        graveyard_.push_back(thread);
    }
    thread->Stop();
    reschedule_pending_ = true;
}

void GuestScheduler::Reap() {
    if (graveyard_.empty()) {
        return;
    }
    std::vector<std::shared_ptr<Thread>> dead;
    dead.swap(graveyard_);
    for (const auto& thread : dead) {
        threads_.erase(thread.get());
    }
}

// ---- switching ---------------------------------------------------------------------------------------

// Called on the fiber of `self`, the thread that holds the host thread. Picks what should run and
// switches to it; returns when `self` is the thread to run again (at once if it still is).
void GuestScheduler::SwitchFromSelf(GuestThread* self) {
    Thread* const next = Dispatch();
    if (next == nullptr) {
        // Nothing to run: the host loop waits for work, then enters whatever should run.
        AccountTo(nullptr);
        self->Yield();
        Reap();
        return;
    }
    SchedThread& sd = Sd(*next);
    MH4N_ASSERT(sd.guest != nullptr, "thread {} is ready but has no fiber", next->thread_id);
    if (sd.guest.get() == self) {
        return;
    }
    ++counters_.thread_switches;
    AccountTo(&sd);
    self->SwitchTo(*sd.guest);
    Reap();
}

void GuestScheduler::Reschedule() {
    GuestThread* const self = CodeHost::CurrentThread();
    MH4N_ASSERT(self != nullptr, "Reschedule outside guest code");
    SwitchFromSelf(self);
}

bool GuestScheduler::StopWanted() {
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_) {
            return true;
        }
    }
    return until_ && until_();
}

void GuestScheduler::Preempt() {
    ++counters_.preempt_checks;
    last_armed_.reset();  // the deadline that raised the flag is spent
    ProcessPending(/*until_quiet=*/false);
    RunHooks();
    GuestThread* const self = CodeHost::CurrentThread();
    MH4N_ASSERT(self != nullptr, "Preempt outside guest code");
    if (StopWanted()) {
        // Give the host stack the thread back; Run enters it again if it is called again.
        AccountTo(nullptr);
        self->Yield();
        Reap();
    }
    SwitchFromSelf(self);
}

// ---- blocking ----------------------------------------------------------------------------------------

WaitOutcome GuestScheduler::Park(Thread& thread) {
    MH4N_ASSERT(CodeHost::CurrentThread() != nullptr && cores_[active_].current == &thread,
                "Park of thread {}, which is not the running thread", thread.thread_id);
    MH4N_ASSERT(thread.status != ThreadStatus::Running, "Park of thread {}, which is running",
                thread.thread_id);
    SchedThread& sd = Sd(thread);
    sd.wake_result.reset();
    sd.wake_index = -1;
    Reschedule();
    MH4N_ASSERT(thread.status == ThreadStatus::Running, "thread {} resumed in state {}",
                thread.thread_id, static_cast<int>(thread.status));
    return {sd.wake_result, sd.wake_index, true};
}

WaitOutcome GuestScheduler::Block(Thread& thread, std::vector<std::shared_ptr<WaitObject>> objects,
                                  bool wait_all, s64 timeout_ns) {
    MH4N_ASSERT(CodeHost::CurrentThread() != nullptr && cores_[active_].current == &thread,
                "Block of thread {}, which is not the running thread", thread.thread_id);
    if (wait_all) {
        const bool all_available =
            std::all_of(objects.begin(), objects.end(),
                        [&thread](const auto& object) { return !object->ShouldWait(&thread); });
        if (all_available) {
            for (auto& object : objects) {
                object->Acquire(&thread);
            }
            return {kResultSuccess, -1, false};  // the output index is not set
        }
    } else {
        for (std::size_t i = 0; i < objects.size(); ++i) {
            if (!objects[i]->ShouldWait(&thread)) {
                objects[i]->Acquire(&thread);
                return {kResultSuccess, static_cast<s32>(i), false};
            }
        }
    }
    if (timeout_ns == 0) {
        return {kResultTimeout, -1, false};
    }
    thread.BeginSynchWait(std::move(objects), wait_all);
    thread.ArmTimeout(timeout_ns);
    return Park(thread);
}

void GuestScheduler::Sleep(s64 ns) {
    Thread* const thread = CurrentThread();
    MH4N_ASSERT(thread != nullptr, "Sleep outside guest code");
    if (ns == 0) {
        // Azahar's SleepThread(0): with no thread ready on this core nothing happens. Otherwise
        // Azahar makes the thread wait for a zero delay, which puts it behind the threads of its
        // priority that are ready; and the ones of lower priority run until that zero-delay event
        // is processed, a time slice at most. Both cases are kept, without the wait of the first:
        // the thread goes to the back of its queue at once.
        const Core& core = cores_[CoreOf(*thread)];
        if (core.mask == 0) {
            return;
        }
        if (static_cast<u32>(std::countr_zero(core.mask)) <= thread->current_priority) {
            thread->status = ThreadStatus::Ready;
            Enqueue(*thread, /*front=*/false);
            Reschedule();
            return;
        }
        // Only lower priorities are ready: wait for the zero delay (it is due at once, so the next
        // preemption check wakes the thread and it displaces whatever runs).
    }
    thread->status = ThreadStatus::WaitSleep;
    thread->ArmTimeout(ns);
    Park(*thread);
}

// ---- the host loop -----------------------------------------------------------------------------------

void GuestScheduler::IdleWait() {
    const s64 start = time_.NowNs();
    std::optional<s64> deadline;
    if (!timers_.empty()) {
        deadline = clock_.HostNsFor(timers_.begin()->first.first);
    }
    {
        std::unique_lock lock(mutex_);
        time_.WaitUntil(cv_, lock, deadline, [this] {
            return !posted_.empty() || stop_requested_ ||
                   __atomic_load_n(&preempt_flag_, __ATOMIC_ACQUIRE) != 0;
        });
    }
    const s64 end = time_.NowNs();
    if (end > start) {
        counters_.idle_ns += static_cast<u64>(end - start);
    }
}

RunStatus GuestScheduler::Run(const std::function<bool()>& until, bool return_when_idle) {
    MH4N_ASSERT(CodeHost::CurrentThread() == nullptr, "Run called from guest code");
    until_ = until;
    struct Cleanup {
        GuestScheduler& self;
        ~Cleanup() { self.until_ = {}; }
    } cleanup{*this};
    for (;;) {
        if (host_.Failure() != nullptr) {
            return RunStatus::Failed;
        }
        Reap();
        ProcessPending(/*until_quiet=*/false);
        RunHooks();
        {
            bool stop = false;
            {
                std::lock_guard lock(mutex_);
                stop = stop_requested_;
                stop_requested_ = false;
            }
            if (stop || (until_ && until_())) {
                return RunStatus::Stopped;
            }
        }
        Thread* const next = Dispatch();
        if (next == nullptr) {
            if (return_when_idle) {
                std::lock_guard lock(mutex_);
                if (posted_.empty() && timers_.empty()) {
                    return RunStatus::Idle;
                }
            }
            IdleWait();
            continue;
        }
        SchedThread& sd = Sd(*next);
        MH4N_ASSERT(sd.guest != nullptr, "thread {} is ready but has no fiber", next->thread_id);
        ++counters_.thread_switches;
        AccountTo(&sd);
        host_.Enter(*sd.guest);
        AccountTo(nullptr);
        guest_running_ = false;
    }
}

}  // namespace mh4n
