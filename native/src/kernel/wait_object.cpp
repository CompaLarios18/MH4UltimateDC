// Derived from Azahar src/core/hle/kernel/wait_object.cpp, event.cpp and semaphore.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the waiter list, the wake-up order
// and the event and semaphore rules.
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project
#include <algorithm>

#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

// ---- WaitObject ----------------------------------------------------------------------------------

void WaitObject::AddWaitingThread(std::shared_ptr<Thread> thread) {
    if (std::find(waiting_.begin(), waiting_.end(), thread) == waiting_.end()) {
        waiting_.push_back(std::move(thread));
    }
}

void WaitObject::RemoveWaitingThread(Thread* thread) {
    // A thread that lists the same object twice is removed twice: the second time is a no-op.
    auto it = std::find_if(waiting_.begin(), waiting_.end(),
                           [thread](const auto& p) { return p.get() == thread; });
    if (it != waiting_.end()) {
        waiting_.erase(it);
    }
}

std::shared_ptr<Thread> WaitObject::GetHighestPriorityReadyThread() const {
    Thread* candidate = nullptr;
    u32 candidate_priority = kThreadPrioLowest + 1;
    for (const auto& thread : waiting_) {
        MH4N_ASSERT(thread->status == ThreadStatus::WaitSynchAny ||
                        thread->status == ThreadStatus::WaitSynchAll ||
                        thread->status == ThreadStatus::WaitHleEvent,
                    "Inconsistent thread statuses in the waiters of {} {}", TypeName(),
                    ObjectId());
        if (thread->current_priority >= candidate_priority) {
            continue;
        }
        if (ShouldWait(thread.get())) {
            continue;
        }
        // Ready if it waits on any, or on all and every other object is available too.
        bool ready_to_run = true;
        if (thread->status == ThreadStatus::WaitSynchAll) {
            ready_to_run = std::none_of(thread->wait_objects.begin(), thread->wait_objects.end(),
                                        [&thread](const std::shared_ptr<WaitObject>& object) {
                                            return object->ShouldWait(thread.get());
                                        });
        }
        if (ready_to_run) {
            candidate = thread.get();
            candidate_priority = thread->current_priority;
        }
    }
    return candidate ? std::static_pointer_cast<Thread>(candidate->shared_from_this()) : nullptr;
}

void WaitObject::WakeupAllWaitingThreads() {
    // The last handle to this object may be one of the wait lists we are about to clear.
    const std::shared_ptr<Object> keep_alive = weak_from_this().lock();
    while (auto thread = GetHighestPriorityReadyThread()) {
        s32 index = -1;  // a wait on all has no index
        if (!thread->IsSleepingOnWaitAll()) {
            Acquire(thread.get());
            index = thread->GetWaitObjectIndex(this);
        } else {
            for (auto& object : thread->wait_objects) {
                object->Acquire(thread.get());
            }
        }
        std::optional<Result> result = kResultSuccess;
        // Before the wait objects are cleared, as Azahar invokes the wake-up callback.
        if (thread->wake_hook) {
            thread->wake_hook(ThreadWakeupReason::Signal, this, result, index);
        }
        for (auto& object : thread->wait_objects) {
            object->RemoveWaitingThread(thread.get());
        }
        thread->wait_objects.clear();
        RemoveWaitingThread(thread.get());  // not needed when wait_objects is consistent
        thread->ResumeFromWait(result, index);
    }
    if (hle_notifier_) {
        hle_notifier_();
    }
}

// ---- Event ---------------------------------------------------------------------------------------

Event::Event(Kernel& kernel, ResetType reset_type) : WaitObject(kernel), reset_type_(reset_type) {}

Event::~Event() {
    if (resource_limit) {
        resource_limit->Release(ResourceLimitType::Event, 1);
    }
}

bool Event::ShouldWait(const Thread*) const {
    return !signaled_;
}

void Event::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "object unavailable!");
    if (reset_type_ == ResetType::OneShot) {
        signaled_ = false;
    }
}

void Event::Signal() {
    signaled_ = true;
    WakeupAllWaitingThreads();
}

void Event::Clear() {
    signaled_ = false;
}

void Event::WakeupAllWaitingThreads() {
    WaitObject::WakeupAllWaitingThreads();
    if (reset_type_ == ResetType::Pulse) {
        signaled_ = false;
    }
}

// ---- Semaphore -----------------------------------------------------------------------------------

Semaphore::Semaphore(Kernel& kernel, s32 initial_count, s32 max_count)
    : WaitObject(kernel), max_count_(max_count), available_count_(initial_count) {}

Semaphore::~Semaphore() {
    if (resource_limit) {
        resource_limit->Release(ResourceLimitType::Semaphore, 1);
    }
}

bool Semaphore::ShouldWait(const Thread*) const {
    return available_count_ <= 0;
}

void Semaphore::Acquire(Thread*) {
    if (available_count_ <= 0) {
        return;
    }
    --available_count_;
}

Result Semaphore::Release(s32* out_count, s32 release_count) {
    // Azahar tests max_count >= release_count + available_count in s32; do it in 64 bits.
    if (static_cast<s64>(max_count_) < static_cast<s64>(release_count) + available_count_) {
        return kResultOutOfRangeKernel;
    }
    *out_count = available_count_;
    available_count_ += release_count;
    WakeupAllWaitingThreads();
    return kResultSuccess;
}

} // namespace mh4n
