// Derived from Azahar src/core/hle/kernel/thread.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the thread as a wait object, its priority rules, the wake-up and the
// bookkeeping of Stop. The queues, the context switch and the creation are not here.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
#include <algorithm>

#include "internal.h"
#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

Thread::Thread(Kernel& kernel, u32 thread_id_, u32 priority, s32 processor_id_)
    : WaitObject(kernel),
      thread_id(thread_id_),
      nominal_priority(priority),
      current_priority(priority),
      processor_id(processor_id_) {}

Thread::~Thread() {
    // A thread dropped while it holds mutexes (it was never stopped): do not leave them pointing at us.
    for (const auto& mutex : held_mutexes) {
        mutex->lock_count_ = 0;
        mutex->holding_thread_ = nullptr;
    }
    if (timeout_event_ != 0) {
        kernel_.Cancel(timeout_event_);
    }
}

bool Thread::ShouldWait(const Thread*) const {
    return status != ThreadStatus::Dead;
}

void Thread::Acquire(Thread* thread) {
    MH4N_ASSERT(!ShouldWait(thread), "object unavailable!");
}

void Thread::SetPriority(u32 priority) {
    MH4N_ASSERT(priority <= kThreadPrioLowest, "Invalid priority value {}.", priority);
    const u32 old_priority = current_priority;
    nominal_priority = current_priority = priority;
    kernel_.GetScheduler().OnPriorityChanged(*this, old_priority);
}

void Thread::UpdatePriority() {
    u32 best_priority = nominal_priority;
    for (const auto& mutex : held_mutexes) {
        if (mutex->Priority() < best_priority) {
            best_priority = mutex->Priority();
        }
    }
    BoostPriority(best_priority);
}

void Thread::BoostPriority(u32 priority) {
    const u32 old_priority = current_priority;
    current_priority = priority;
    kernel_.GetScheduler().OnPriorityChanged(*this, old_priority);
}

void Thread::BeginSynchWait(std::vector<std::shared_ptr<WaitObject>> objects, bool wait_all) {
    status = wait_all ? ThreadStatus::WaitSynchAll : ThreadStatus::WaitSynchAny;
    const auto self = std::static_pointer_cast<Thread>(shared_from_this());
    for (auto& object : objects) {
        object->AddWaitingThread(self);
    }
    wait_objects = std::move(objects);
}

void Thread::ArmTimeout(s64 ns) {
    CancelTimeout();
    if (ns == -1) {
        return;  // wait forever
    }
    const std::weak_ptr<Thread> weak = std::static_pointer_cast<Thread>(shared_from_this());
    timeout_event_ = kernel_.ScheduleAt(kernel_.NowTicks() + kernel_detail::NsToTicks(ns), [weak] {
        if (auto thread = weak.lock()) {
            thread->OnWaitTimeout();
        }
    });
}

void Thread::CancelTimeout() {
    if (timeout_event_ != 0) {
        kernel_.Cancel(timeout_event_);
        timeout_event_ = 0;
    }
}

void Thread::OnWaitTimeout() {
    timeout_event_ = 0;  // this event has run
    switch (status) {
    case ThreadStatus::WaitSynchAny:
    case ThreadStatus::WaitSynchAll:
    case ThreadStatus::WaitArb:
    case ThreadStatus::WaitHleEvent: {
        std::optional<Result> result = kResultTimeout;
        s32 index = -1;
        // Before the wait objects are cleared, as Azahar invokes the wake-up callback.
        if (wake_hook) {
            wake_hook(ThreadWakeupReason::Timeout, nullptr, result, index);
        }
        for (auto& object : wait_objects) {
            object->RemoveWaitingThread(this);
        }
        wait_objects.clear();
        ResumeFromWait(result, index);
        break;
    }
    case ThreadStatus::WaitSleep:
    case ThreadStatus::WaitIPC:
        ResumeFromWait(std::nullopt);
        break;
    default:
        break;  // already woken some other way
    }
}

void Thread::ResumeFromWait(std::optional<Result> result, s32 index) {
    MH4N_ASSERT(wait_objects.empty(), "Thread {} is waking up while waiting for objects",
                thread_id);
    switch (status) {
    case ThreadStatus::WaitSynchAll:
    case ThreadStatus::WaitSynchAny:
    case ThreadStatus::WaitHleEvent:
    case ThreadStatus::WaitArb:
    case ThreadStatus::WaitSleep:
    case ThreadStatus::WaitIPC:
    case ThreadStatus::Dormant:
        break;
    case ThreadStatus::Ready:
        // Woken more than once before it ran (several objects signalled): the first wake counts.
        return;
    case ThreadStatus::Running:
        MH4N_LOG(Error, "Kernel", "Thread {} has already resumed", thread_id);
        return;
    case ThreadStatus::Dead:
        MH4N_LOG(Error, "Kernel", "Thread {} cannot be resumed: it is dead", thread_id);
        return;
    }
    wake_hook = nullptr;
    CancelTimeout();
    status = ThreadStatus::Ready;
    kernel_.GetScheduler().Wake(std::static_pointer_cast<Thread>(shared_from_this()), result, index);
}

s32 Thread::GetWaitObjectIndex(const WaitObject* object) const {
    const auto match = std::find_if(wait_objects.rbegin(), wait_objects.rend(),
                                    [object](const auto& p) { return p.get() == object; });
    if (match == wait_objects.rend()) {
        return -1;
    }
    return static_cast<s32>(std::distance(match, wait_objects.rend()) - 1);
}

void Thread::Stop() {
    if (status == ThreadStatus::Dead) {
        return;
    }
    const auto keep_alive = std::static_pointer_cast<Thread>(shared_from_this());
    CancelTimeout();
    if (wake_hook) {
        std::optional<Result> result;
        s32 index = -1;
        wake_hook(ThreadWakeupReason::Terminated, nullptr, result, index);
        wake_hook = nullptr;
    }
    status = ThreadStatus::Dead;
    WakeupAllWaitingThreads();
    // Dangling references in the objects it was waiting for.
    for (auto& object : wait_objects) {
        object->RemoveWaitingThread(this);
    }
    wait_objects.clear();
    ReleaseThreadMutexes(this);
}

} // namespace mh4n
