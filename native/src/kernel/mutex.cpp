// Derived from Azahar src/core/hle/kernel/mutex.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): recursion, the release rules and priority inheritance.
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project
#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

void ReleaseThreadMutexes(Thread* thread) {
    // The mutexes stay alive until the set is cleared; waking may run kernel code, so work on a copy.
    const std::set<std::shared_ptr<Mutex>> held = std::move(thread->held_mutexes);
    thread->held_mutexes.clear();
    for (const auto& mutex : held) {
        mutex->lock_count_ = 0;
        mutex->holding_thread_ = nullptr;
        mutex->WakeupAllWaitingThreads();
    }
}

Mutex::Mutex(Kernel& kernel) : WaitObject(kernel) {}

Mutex::~Mutex() {
    if (resource_limit) {
        resource_limit->Release(ResourceLimitType::Mutex, 1);
    }
}

bool Mutex::ShouldWait(const Thread* thread) const {
    return lock_count_ > 0 && thread != holding_thread_;
}

void Mutex::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "object unavailable!");
    // Only the first acquisition takes the mutex; the others count the recursion.
    if (lock_count_ == 0) {
        priority_ = thread->current_priority;
        thread->held_mutexes.insert(std::static_pointer_cast<Mutex>(shared_from_this()));
        holding_thread_ = thread;
        thread->UpdatePriority();
        kernel_.GetScheduler().PrepareReschedule();
    }
    lock_count_++;
}

Result Mutex::Release(Thread* thread) {
    if (thread != holding_thread_) {
        if (holding_thread_) {
            MH4N_LOG(Error, "Kernel",
                     "Tried to release a mutex (owned by thread id {}) from a different thread id "
                     "{}",
                     holding_thread_->thread_id, thread ? thread->thread_id : 0u);
        }
        return kResultWrongLockingThread;
    }
    // Cannot happen (a holder has a positive count), but the real kernel checks it too.
    if (lock_count_ <= 0) {
        return kResultInvalidMutexState;
    }
    lock_count_--;
    // Only the last release frees the mutex.
    if (lock_count_ == 0) {
        const std::shared_ptr<Mutex> self = std::static_pointer_cast<Mutex>(shared_from_this());
        holding_thread_->held_mutexes.erase(self);
        holding_thread_->UpdatePriority();
        holding_thread_ = nullptr;
        WakeupAllWaitingThreads();
        kernel_.GetScheduler().PrepareReschedule();
    }
    return kResultSuccess;
}

void Mutex::AddWaitingThread(std::shared_ptr<Thread> thread) {
    WaitObject::AddWaitingThread(thread);
    thread->pending_mutexes.insert(std::static_pointer_cast<Mutex>(shared_from_this()));
    UpdatePriority();
}

void Mutex::RemoveWaitingThread(Thread* thread) {
    WaitObject::RemoveWaitingThread(thread);
    thread->pending_mutexes.erase(std::static_pointer_cast<Mutex>(shared_from_this()));
    UpdatePriority();
}

void Mutex::UpdatePriority() {
    if (!holding_thread_) {
        return;
    }
    u32 best_priority = kThreadPrioLowest;
    for (const auto& waiter : GetWaitingThreads()) {
        if (waiter->current_priority < best_priority) {
            best_priority = waiter->current_priority;
        }
    }
    if (best_priority != priority_) {
        priority_ = best_priority;
        holding_thread_->UpdatePriority();
    }
}

} // namespace mh4n
