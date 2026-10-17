// Derived from Azahar src/core/hle/kernel/handle_table.cpp, wait_object.cpp, event.cpp, mutex.cpp,
// semaphore.cpp, timer.cpp, address_arbiter.cpp, shared_memory.cpp, resource_limit.cpp, thread.cpp,
// errors.h, svc.cpp, kernel.cpp, kernel.h, process.cpp, vm_manager.cpp, memory.cpp and object.cpp
// (all in src/core/hle/kernel/; object.cpp for the waitable classification), and
// src/core/memory.cpp (an unmapped Read32 gives 0) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: svc.cpp, kernel.cpp and kernel.h say "GPLv3 or any later version"; every other
// one of those files says "GPLv2 or any later version"): the expected values, worked out from
// their algorithms (process, thread and object numbering from kernel.cpp, kernel.h and
// process.cpp; the merging of blocks from vm_manager.cpp; the region sizes and where HeapAllocate
// takes from, from kernel/memory.cpp), and the Block helper, which restates the wait set-up of
// SVC::WaitSynchronization1/N.
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (handle_table.cpp,
//   wait_object.cpp, event.cpp, mutex.cpp, semaphore.cpp, address_arbiter.cpp, shared_memory.cpp)
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (timer.cpp)
// Copyright 2018-2024 Citra Emulator Project / Azahar Emulator Project (object.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (resource_limit.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.cpp, svc.cpp,
//   kernel.cpp, kernel.h)
// PPSSPP Project (kernel.h, second notice: Licensed under GPLv2 or any later version)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp)
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (vm_manager.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (kernel/memory.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (core/memory.cpp)
// Copyright 2017 Citra Emulator Project (errors.h)
//
// mh4n_kernel (N1-05): kernel objects, the handle table and their rules, driven by a fake
// scheduler (a recording Scheduler with a manual game clock and timer queue).
//
// The expected values were worked out by hand from the Azahar sources named above, at the pinned
// commit, with the derivation in a comment next to each number: never by running this code.
// The Block helper restates the wait set-up of SVC::WaitSynchronization1/N from Azahar's
// src/core/hle/kernel/svc.cpp (GPL-3.0-or-later; Copyright 2014-2026 Citra Emulator Project /
// Azahar Emulator Project).
// Clock arithmetic: 268,111,856 ticks per second, so 1 s = 268111856 ticks and 0.5 s = 134055928.
#include <doctest/doctest.h>

#include <map>
#include <mutex>
#include <thread>
#include <tuple>

#include "mh4n/kernel.h"
#include "mh4n/log.h"

using namespace mh4n;

namespace {

constexpr u64 kSecond = 268111856;  // ticks

// ---- the fake scheduler ----------------------------------------------------------------------------

class FakeScheduler final : public Scheduler {
public:
    struct WakeRecord {
        std::shared_ptr<Thread> thread;
        std::optional<Result> result;
        s32 index;
    };
    struct PriorityChange {
        Thread* thread;
        u32 old_priority;
        u32 new_priority;
        friend bool operator==(const PriorityChange&, const PriorityChange&) = default;
    };

    std::vector<WakeRecord> wakes;
    std::vector<PriorityChange> priority_changes;
    int reschedules = 0;
    Thread* current = nullptr;
    u64 now = 0;
    std::vector<std::function<void()>> posted;

    void Wake(const std::shared_ptr<Thread>& thread, std::optional<Result> result,
              s32 index) override {
        wakes.push_back({thread, result, index});
    }
    void OnPriorityChanged(Thread& thread, u32 old_priority) override {
        priority_changes.push_back({&thread, old_priority, thread.current_priority});
    }
    void PrepareReschedule() override { ++reschedules; }
    Thread* CurrentThread() override { return current; }
    u64 NowTicks() override { return now; }
    u64 ScheduleAt(u64 tick, std::function<void()> fn) override {
        const u64 id = next_id_++;
        events_.emplace(std::make_pair(tick, id), std::move(fn));
        ticks_of_[id] = tick;
        return id;
    }
    bool Cancel(u64 id) override {
        const auto it = ticks_of_.find(id);
        if (it == ticks_of_.end()) {
            return false;
        }
        events_.erase({it->second, id});
        ticks_of_.erase(it);
        return true;
    }
    void Post(std::function<void()> fn) override {
        std::lock_guard lock(post_mutex_);
        posted.push_back(std::move(fn));
    }

    // Runs the events due at or before `tick` in order, the clock reading each event's tick while
    // it runs, then leaves the clock at `tick`.
    void RunUntil(u64 tick) {
        while (!events_.empty() && events_.begin()->first.first <= tick) {
            auto node = events_.extract(events_.begin());
            const u64 id = node.key().second;
            now = std::max(now, node.key().first);
            ticks_of_.erase(id);
            node.mapped()();
        }
        now = std::max(now, tick);
    }
    std::size_t Pending() const { return events_.size(); }
    std::optional<u64> NextTick() const {
        return events_.empty() ? std::nullopt : std::optional<u64>(events_.begin()->first.first);
    }

private:
    u64 next_id_ = 1;
    std::map<std::pair<u64, u64>, std::function<void()>> events_;  // (tick, id) -> fn
    std::map<u64, u64> ticks_of_;                                  // id -> tick
    std::mutex post_mutex_;
};

struct Env {
    std::unique_ptr<GuestMemory> memory;
    FakeScheduler sched;
    std::unique_ptr<Kernel> kernel;
    std::shared_ptr<Process> process;

    explicit Env(u32 memory_mode = 0) {
        memory = GuestMemory::Create();
        REQUIRE(memory != nullptr);
        kernel = std::make_unique<Kernel>(*memory, memory_mode);
        kernel->SetScheduler(&sched);
        process = kernel->NewProcess("app");
    }
    // Threads that still wait hold the objects they wait for, and the objects hold them: end
    // them so that nothing is left in a cycle.
    ~Env() {
        for (auto& thread : threads_) {
            thread->Stop();
        }
        threads_.clear();
        sched.wakes.clear();
        process.reset();
        kernel.reset();
    }

    std::shared_ptr<Thread> NewThread(u32 priority, s32 core = 0) {
        auto thread = kernel->NewThread("t", priority, core);
        thread->status = ThreadStatus::Running;
        threads_.push_back(thread);
        return thread;
    }

private:
    std::vector<std::shared_ptr<Thread>> threads_;
};

// What WaitSynchronization1 and N do for the calling thread (svc.cpp): acquire at once if the
// call can be satisfied, else block. Returns true if the thread blocked.
bool Block(Thread& thread, std::vector<std::shared_ptr<WaitObject>> objects, bool wait_all,
           s64 timeout_ns = -1) {
    if (wait_all) {
        bool all_available = true;
        for (auto& object : objects) {
            all_available = all_available && !object->ShouldWait(&thread);
        }
        if (all_available) {
            for (auto& object : objects) {
                object->Acquire(&thread);
            }
            return false;
        }
    } else {
        for (auto& object : objects) {
            if (!object->ShouldWait(&thread)) {
                object->Acquire(&thread);
                return false;
            }
        }
    }
    thread.BeginSynchWait(std::move(objects), wait_all);
    thread.ArmTimeout(timeout_ns);
    return true;
}

bool Block(Thread& thread, std::shared_ptr<WaitObject> object, s64 timeout_ns = -1) {
    return Block(thread, std::vector<std::shared_ptr<WaitObject>>{std::move(object)}, false,
                 timeout_ns);
}

}  // namespace

// ---- HandleTable -----------------------------------------------------------------------------------

TEST_CASE("handle numbering: generation | slot << 15, the last slot freed is reused first") {
    Env env;
    auto& table = env.process->handle_table;
    Handle h1 = 0, h2 = 0, h3 = 0, h4 = 0, h5 = 0;
    // Slots are taken 0, 1, 2 and the generation counter (starting at 1) gives 1, 2, 3:
    // 1 | 0<<15, 2 | 1<<15, 3 | 2<<15.
    CHECK(table.Create(&h1, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    CHECK(table.Create(&h2, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    CHECK(table.Create(&h3, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    CHECK(h1 == 0x00000001);
    CHECK(h2 == 0x00008002);
    CHECK(h3 == 0x00010003);

    // Closing the second pushes slot 1 on the free list; the next create reuses it with the next
    // generation, 4: 4 | 1<<15 = 0x00008004. The one after takes slot 3 (the next never-used
    // slot), generation 5: 5 | 3<<15 = 0x00018005.
    CHECK(table.Close(h2) == kResultSuccess);
    CHECK(table.Create(&h4, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    CHECK(h4 == 0x00008004);
    CHECK(table.Create(&h5, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    CHECK(h5 == 0x00018005);

    // The old handle of slot 1 (generation 2) is stale; the others are fine.
    CHECK_FALSE(table.IsValid(h2));
    CHECK(table.IsValid(h1));
    CHECK(table.IsValid(h3));
    CHECK(table.IsValid(h4));
}

TEST_CASE("handle free list: last in, first out") {
    Env env;
    auto& table = env.process->handle_table;
    Handle h[4] = {};
    for (Handle& handle : h) {
        REQUIRE(table.Create(&handle, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    }
    CHECK(h[0] == 0x00000001);
    CHECK(h[3] == 0x00018004);
    // Free slot 0, then slot 2: the free list is 2, 0, then the never-used slots from 4.
    REQUIRE(table.Close(h[0]) == kResultSuccess);
    REQUIRE(table.Close(h[2]) == kResultSuccess);
    Handle a = 0, b = 0, c = 0;
    REQUIRE(table.Create(&a, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    REQUIRE(table.Create(&b, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    REQUIRE(table.Create(&c, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    CHECK(a == 0x00010005);  // slot 2, generation 5
    CHECK(b == 0x00000006);  // slot 0, generation 6
    CHECK(c == 0x00020007);  // slot 4, generation 7
}

TEST_CASE("handle generation counts 1 to 0x7FFF and wraps back to 1") {
    Env env;
    auto& table = env.process->handle_table;
    auto event = env.kernel->NewEvent(ResetType::OneShot);
    // Create and close in slot 0 over and over: the k-th handle is k (generation k, slot 0).
    Handle handle = 0;
    for (u32 k = 1; k <= 0x7FFF; ++k) {
        REQUIRE(table.Create(&handle, event) == kResultSuccess);
        REQUIRE(handle == k);
        REQUIRE(table.Close(handle) == kResultSuccess);
    }
    // The counter reached 0x8000 and went to 1 (generation 0 is never used).
    REQUIRE(table.Create(&handle, event) == kResultSuccess);
    CHECK(handle == 0x00000001);
    // And the generation after that is 2, in slot 1 as slot 0 is taken: 2 | 1<<15.
    Handle second = 0;
    REQUIRE(table.Create(&second, event) == kResultSuccess);
    CHECK(second == 0x00008002);
}

TEST_CASE("handle table: 4096 slots, then kResultOutOfHandles") {
    Env env;
    auto& table = env.process->handle_table;
    auto event = env.kernel->NewEvent(ResetType::Sticky);
    std::vector<Handle> handles(HandleTable::kMaxCount);
    for (Handle& handle : handles) {
        REQUIRE(table.Create(&handle, event) == kResultSuccess);
    }
    // The 4096th: generation 4096 = 0x1000, slot 4095 = 0xFFF: 0x1000 | 0xFFF << 15, and
    // 0xFFF << 15 = 0x7FF8000 (4096 << 15 is 0x8000000, one slot less is 0x8000 less).
    CHECK(handles.back() == 0x07FF9000);
    Handle extra = 0xDEADBEEF;
    CHECK(table.Create(&extra, event) == 0xD8600413);
    CHECK(kResultOutOfHandles == 0xD8600413);
    CHECK(extra == 0xDEADBEEF);  // untouched on failure
    // Free slot 100 (handle 101 | 100 << 15 = 0x00320065): it is reused with generation 4097.
    REQUIRE(handles[100] == 0x00320065);
    REQUIRE(table.Close(handles[100]) == kResultSuccess);
    REQUIRE(table.Create(&extra, event) == kResultSuccess);
    CHECK(extra == (4097u | (100u << 15)));  // 0x00321001
    CHECK(extra == 0x00321001);
}

TEST_CASE("handle table: invalid, stale and duplicated handles") {
    Env env;
    auto& table = env.process->handle_table;
    auto event = env.kernel->NewEvent(ResetType::OneShot);
    Handle h = 0;
    REQUIRE(table.Create(&h, event) == kResultSuccess);

    // Closing twice, closing a handle that never existed, and handle 0 are all invalid handles.
    CHECK(table.Close(h) == kResultSuccess);
    CHECK(table.Close(h) == 0xD8E007F7);
    CHECK(kResultInvalidHandle == 0xD8E007F7);
    CHECK(table.Close(0x00008002) == kResultInvalidHandle);
    CHECK(table.Close(0) == kResultInvalidHandle);
    CHECK(table.GetGeneric(h) == nullptr);
    CHECK(table.Get<Event>(h) == nullptr);
    // A slot field of 4096 or more is never valid (Azahar would truncate it to 16 bits and alias
    // slot 0).
    REQUIRE(table.Create(&h, event) == kResultSuccess);  // slot 0, generation 2: 0x00000002
    CHECK(h == 0x00000002);
    CHECK_FALSE(table.IsValid(0x80000002));
    CHECK(table.Get<Event>(0x80000002) == nullptr);

    // Duplicate: another handle to the same object; an invalid one fails and leaves `out`.
    Handle dup = 0;
    CHECK(table.Duplicate(&dup, h) == kResultSuccess);
    CHECK(dup == 0x00008003);  // slot 1, generation 3
    CHECK(table.Get<Event>(dup) == event);
    CHECK(table.Get<Event>(h) == event);
    Handle none = 0x1234;
    CHECK(table.Duplicate(&none, 0x00020009) == kResultInvalidHandle);
    CHECK(none == 0x1234);
    // Closing one handle keeps the object alive through the other.
    CHECK(table.Close(h) == kResultSuccess);
    CHECK(table.Get<Event>(dup) == event);
}

TEST_CASE("handle table: Get<T> checks the type, Get<WaitObject> the waitable ones") {
    Env env;
    auto& table = env.process->handle_table;
    Handle he = 0, hm = 0, hs = 0, ht = 0, ha = 0, hshm = 0, hth = 0;
    std::shared_ptr<Semaphore> semaphore;
    REQUIRE(env.kernel->NewSemaphore(&semaphore, 0, 1) == kResultSuccess);
    auto shared_memory = env.kernel->NewSharedMemoryForApplet(0, 0x1000, MemoryPermission::Read,
                                                              MemoryPermission::Read);
    REQUIRE(shared_memory != nullptr);
    auto thread = env.kernel->NewThread("t", 40, 0);
    REQUIRE(table.Create(&he, env.kernel->NewEvent(ResetType::OneShot)) == kResultSuccess);
    REQUIRE(table.Create(&hm, env.kernel->NewMutex(false)) == kResultSuccess);
    REQUIRE(table.Create(&hs, semaphore) == kResultSuccess);
    REQUIRE(table.Create(&ht, env.kernel->NewTimer(ResetType::Pulse)) == kResultSuccess);
    REQUIRE(table.Create(&ha, env.kernel->NewAddressArbiter()) == kResultSuccess);
    REQUIRE(table.Create(&hshm, shared_memory) == kResultSuccess);
    REQUIRE(table.Create(&hth, thread) == kResultSuccess);

    CHECK(table.Get<Event>(he) != nullptr);
    CHECK(table.Get<Mutex>(he) == nullptr);
    CHECK(table.Get<Event>(hm) == nullptr);
    CHECK(table.Get<Mutex>(hm) != nullptr);
    CHECK(table.Get<Semaphore>(hs) == semaphore);
    CHECK(table.Get<Timer>(ht) != nullptr);
    CHECK(table.Get<AddressArbiter>(ha) != nullptr);
    CHECK(table.Get<SharedMemory>(hshm) == shared_memory);
    CHECK(table.Get<Thread>(hth) == thread);
    CHECK(table.Get<Object>(hshm) == shared_memory);

    // Waitable: event, mutex, semaphore, timer, thread. Not: arbiter, shared memory, process.
    for (Handle waitable : {he, hm, hs, ht, hth}) {
        CHECK(table.Get<WaitObject>(waitable) != nullptr);
    }
    CHECK(table.Get<WaitObject>(ha) == nullptr);
    CHECK(table.Get<WaitObject>(hshm) == nullptr);
    Handle hp = 0;
    REQUIRE(table.Create(&hp, env.process) == kResultSuccess);
    CHECK(table.Get<WaitObject>(hp) == nullptr);
    CHECK(table.Get<Process>(hp) == env.process);
    CHECK(env.process->GetHandleType() == HandleType::Process);
}

TEST_CASE("pseudo-handles 0xFFFF8000 (current thread) and 0xFFFF8001 (current process)") {
    Env env;
    auto& table = env.process->handle_table;
    CHECK(kHandleCurrentThread == 0xFFFF8000);
    CHECK(kHandleCurrentProcess == 0xFFFF8001);
    auto thread = env.NewThread(30);

    // No current thread yet.
    CHECK(table.GetGeneric(kHandleCurrentThread) == nullptr);
    env.sched.current = thread.get();
    CHECK(table.Get<Thread>(kHandleCurrentThread) == thread);
    CHECK(table.Get<WaitObject>(kHandleCurrentThread) == thread);  // a thread is waitable
    CHECK(table.Get<Event>(kHandleCurrentThread) == nullptr);
    CHECK(table.Get<Process>(kHandleCurrentProcess) == env.process);
    CHECK(table.Get<Thread>(kHandleCurrentProcess) == nullptr);
    CHECK(env.kernel->GetObject<Thread>(kHandleCurrentThread) == thread);

    // They follow the current thread.
    auto other = env.NewThread(10);
    env.sched.current = other.get();
    CHECK(table.Get<Thread>(kHandleCurrentThread) == other);

    // They are not in the table: not valid, cannot be closed; they can be duplicated into a real
    // handle (Azahar's Duplicate resolves through GetGeneric).
    CHECK_FALSE(table.IsValid(kHandleCurrentThread));
    CHECK_FALSE(table.IsValid(kHandleCurrentProcess));
    CHECK(table.Close(kHandleCurrentThread) == kResultInvalidHandle);
    CHECK(table.Close(kHandleCurrentProcess) == kResultInvalidHandle);
    Handle dup = 0;
    CHECK(table.Duplicate(&dup, kHandleCurrentThread) == kResultSuccess);
    CHECK(dup == 0x00000001);
    env.sched.current = thread.get();
    CHECK(table.Get<Thread>(dup) == other);  // the real handle keeps its thread
}

TEST_CASE("process and thread numbering follow Azahar") {
    Env env;
    // The first process is made with ++next_process_id from 10.
    CHECK(env.process->process_id == 11);
    CHECK(env.process->process_id == kApplicationProcessId);
    CHECK(env.kernel->CurrentProcess() == env.process);
    CHECK(env.process->resource_limit == env.kernel->ApplicationResourceLimit());
    CHECK(env.process->memory_region == FcramRegion::Application);
    auto second = env.kernel->NewProcess("other");
    CHECK(second->process_id == 12);
    CHECK(env.kernel->CurrentProcess() == second);

    // Thread ids from 1, never reused.
    auto a = env.kernel->NewThread("a", 48, 0);
    auto b = env.kernel->NewThread("b", 48, 1);
    CHECK(a->thread_id == 1);
    CHECK(b->thread_id == 2);
    b.reset();
    CHECK(env.kernel->NewThread("c", 48, 0)->thread_id == 3);
    CHECK(a->status == ThreadStatus::Dormant);
    CHECK(a->nominal_priority == 48);
    CHECK(a->current_priority == 48);
    CHECK(a->processor_id == 0);
    CHECK(a->owner_process.lock() == second);

    // Object ids count up from 0 and are unique.
    auto e1 = env.kernel->NewEvent(ResetType::OneShot);
    auto e2 = env.kernel->NewEvent(ResetType::OneShot);
    CHECK(e2->ObjectId() == e1->ObjectId() + 1);
}

// ---- events ----------------------------------------------------------------------------------------

TEST_CASE("event, one-shot: a signal wakes the best-priority waiter and is consumed") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::OneShot);
    auto a = env.NewThread(30);
    auto b = env.NewThread(20);
    CHECK(event->GetResetType() == ResetType::OneShot);
    REQUIRE(Block(*a, event));
    REQUIRE(Block(*b, event));
    CHECK(a->status == ThreadStatus::WaitSynchAny);

    // b has the better priority (20 < 30); its Acquire clears the event, so a still has to wait.
    event->Signal();
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == b);
    CHECK(env.sched.wakes[0].result == kResultSuccess);
    CHECK(env.sched.wakes[0].index == 0);
    CHECK(b->status == ThreadStatus::Ready);
    CHECK(a->status == ThreadStatus::WaitSynchAny);
    CHECK_FALSE(event->IsSignaled());
    CHECK(event->GetWaitingThreads().size() == 1);

    event->Signal();
    REQUIRE(env.sched.wakes.size() == 2);
    CHECK(env.sched.wakes[1].thread == a);
    CHECK(event->GetWaitingThreads().empty());
    CHECK(a->wait_objects.empty());
}

TEST_CASE("event, one-shot: signalled with nobody waiting, the next waiter takes it") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::OneShot);
    auto a = env.NewThread(30);
    event->Signal();
    CHECK(event->IsSignaled());
    CHECK_FALSE(Block(*a, event));  // acquired at once
    CHECK_FALSE(event->IsSignaled());
    CHECK(Block(*a, event));  // now it waits
}

TEST_CASE("event, equal priorities: the first waiter in the list goes first") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::OneShot);
    auto a = env.NewThread(30);
    auto b = env.NewThread(30);
    REQUIRE(Block(*a, event));
    REQUIRE(Block(*b, event));
    event->Signal();
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == a);
}

TEST_CASE("event, sticky: stays signalled until cleared, wakes every waiter in priority order") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::Sticky);
    auto a = env.NewThread(30);
    auto b = env.NewThread(20);
    auto c = env.NewThread(40);
    REQUIRE(Block(*a, event));
    REQUIRE(Block(*b, event));
    REQUIRE(Block(*c, event));
    event->Signal();
    REQUIRE(env.sched.wakes.size() == 3);
    CHECK(env.sched.wakes[0].thread == b);  // 20
    CHECK(env.sched.wakes[1].thread == a);  // 30
    CHECK(env.sched.wakes[2].thread == c);  // 40
    CHECK(event->IsSignaled());
    CHECK_FALSE(event->ShouldWait(a.get()));
    event->Clear();
    CHECK(event->ShouldWait(a.get()));
}

TEST_CASE("event, pulse: wakes the waiters and clears itself, also with nobody waiting") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::Pulse);
    auto a = env.NewThread(30);
    auto b = env.NewThread(20);
    REQUIRE(Block(*a, event));
    REQUIRE(Block(*b, event));
    event->Signal();
    CHECK(env.sched.wakes.size() == 2);
    CHECK_FALSE(event->IsSignaled());

    // A pulse nobody sees is lost: the next wait blocks.
    auto c = env.NewThread(10);
    event->Signal();
    CHECK_FALSE(event->IsSignaled());
    CHECK(Block(*c, event));
}

TEST_CASE("the HLE notifier runs after every wake-up pass") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::Sticky);
    int notified = 0;
    event->SetHleNotifier([&] { ++notified; });
    event->Signal();
    event->Signal();
    CHECK(notified == 2);
}

// ---- waiting ---------------------------------------------------------------------------------------

TEST_CASE("wait on any: woken by the first object, with its index") {
    Env env;
    auto e0 = env.kernel->NewEvent(ResetType::OneShot);
    auto e1 = env.kernel->NewEvent(ResetType::OneShot);
    auto e2 = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    REQUIRE(Block(*thread, {e0, e1, e2}, false));
    CHECK(thread->wait_objects.size() == 3);
    CHECK(e1->GetWaitingThreads().size() == 1);

    e1->Signal();
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].result == kResultSuccess);
    CHECK(env.sched.wakes[0].index == 1);
    CHECK(thread->status == ThreadStatus::Ready);
    // It left every waiter list, so a later signal of another object does nothing.
    CHECK(thread->wait_objects.empty());
    CHECK(e0->GetWaitingThreads().empty());
    CHECK(e2->GetWaitingThreads().empty());
    e0->Signal();
    CHECK(env.sched.wakes.size() == 1);
    CHECK(e0->IsSignaled());  // nobody took it
}

TEST_CASE("wait on any: an object listed twice reports its last position") {
    Env env;
    auto e0 = env.kernel->NewEvent(ResetType::OneShot);
    auto e1 = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    REQUIRE(Block(*thread, {e0, e1, e0}, false));
    CHECK(e0->GetWaitingThreads().size() == 1);  // one entry in the waiter list
    e0->Signal();
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].index == 2);
}

TEST_CASE("wait on any: an object that is available is taken without blocking") {
    Env env;
    auto e0 = env.kernel->NewEvent(ResetType::OneShot);
    auto e1 = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    e1->Signal();
    CHECK_FALSE(Block(*thread, {e0, e1}, false));
    CHECK_FALSE(e1->IsSignaled());
    CHECK(env.sched.wakes.empty());
}

TEST_CASE("wait on all: wakes once, when the last object is signalled, and takes them all") {
    Env env;
    auto e0 = env.kernel->NewEvent(ResetType::OneShot);
    auto e1 = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    REQUIRE(Block(*thread, {e0, e1}, true));
    CHECK(thread->status == ThreadStatus::WaitSynchAll);
    CHECK(thread->IsSleepingOnWaitAll());

    e0->Signal();
    CHECK(env.sched.wakes.empty());
    CHECK(e0->IsSignaled());
    e1->Signal();
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].result == kResultSuccess);
    CHECK(env.sched.wakes[0].index == -1);  // a wait on all has no index
    // One-shot events were consumed by the wake-up.
    CHECK_FALSE(e0->IsSignaled());
    CHECK_FALSE(e1->IsSignaled());
    CHECK(e0->GetWaitingThreads().empty());
    CHECK(e1->GetWaitingThreads().empty());
}

TEST_CASE("wait on all: with every object available the call is satisfied at once") {
    Env env;
    auto e0 = env.kernel->NewEvent(ResetType::OneShot);
    auto e1 = env.kernel->NewEvent(ResetType::Sticky);
    auto thread = env.NewThread(30);
    e0->Signal();
    e1->Signal();
    CHECK_FALSE(Block(*thread, {e0, e1}, true));
    CHECK_FALSE(e0->IsSignaled());
    CHECK(e1->IsSignaled());
}

TEST_CASE("wait with a timeout: kResultTimeout at the deadline, and the thread leaves the lists") {
    Env env;
    auto e0 = env.kernel->NewEvent(ResetType::OneShot);
    auto e1 = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    // 1 s = 268111856 ticks (1e9 * 268111856 / 1e9).
    REQUIRE(Block(*thread, {e0, e1}, false, 1'000'000'000));
    env.sched.RunUntil(kSecond - 1);
    CHECK(env.sched.wakes.empty());
    env.sched.RunUntil(kSecond);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].result == 0x09401BFE);
    CHECK(kResultTimeout == 0x09401BFE);
    CHECK(env.sched.wakes[0].index == -1);
    CHECK(thread->status == ThreadStatus::Ready);
    CHECK(thread->wait_objects.empty());
    CHECK(e0->GetWaitingThreads().empty());
    CHECK(e1->GetWaitingThreads().empty());
    e0->Signal();
    CHECK(env.sched.wakes.size() == 1);
}

TEST_CASE("wait with a timeout: a wait on all times out too") {
    Env env;
    auto e0 = env.kernel->NewEvent(ResetType::OneShot);
    auto e1 = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    REQUIRE(Block(*thread, {e0, e1}, true, 500'000'000));
    e0->Signal();
    env.sched.RunUntil(kSecond / 2);  // 134055928 ticks
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].result == kResultTimeout);
    CHECK(e0->IsSignaled());  // never acquired
}

TEST_CASE("wait with a timeout of -1 never expires; a signal cancels a pending timeout") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    REQUIRE(Block(*thread, event, -1));
    CHECK(env.sched.Pending() == 0);
    env.sched.RunUntil(100 * kSecond);
    CHECK(env.sched.wakes.empty());
    event->Signal();
    CHECK(env.sched.wakes.size() == 1);

    // With a timeout: the signal comes first, and the timeout must not fire later (Azahar leaves
    // the event queued and it can wake the thread out of an unrelated wait).
    auto other = env.kernel->NewEvent(ResetType::OneShot);
    thread->status = ThreadStatus::Running;
    REQUIRE(Block(*thread, event, 10'000'000'000));
    CHECK(env.sched.Pending() == 1);
    event->Signal();
    CHECK(env.sched.wakes.size() == 2);
    CHECK(env.sched.Pending() == 0);
    thread->status = ThreadStatus::Running;
    REQUIRE(Block(*thread, other, -1));
    env.sched.RunUntil(1000 * kSecond);
    CHECK(env.sched.wakes.size() == 2);
    CHECK(thread->status == ThreadStatus::WaitSynchAny);
}

TEST_CASE("wait with a timeout of 0 ns expires at the next run of the timer queue") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    env.sched.now = 5000;
    REQUIRE(Block(*thread, event, 0));
    CHECK(env.sched.NextTick() == 5000);
    env.sched.RunUntil(5000);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].result == kResultTimeout);
}

TEST_CASE("a wake hook can change the result and sees the signalling object or the timeout") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::OneShot);
    auto thread = env.NewThread(30);
    std::vector<std::pair<ThreadWakeupReason, const WaitObject*>> calls;
    thread->wake_hook = [&](ThreadWakeupReason reason, WaitObject* object,
                            std::optional<Result>& result, s32& index) {
        calls.emplace_back(reason, object);
        result = 0x1234;
        index = 7;
    };
    REQUIRE(Block(*thread, event, 1'000'000'000));
    event->Signal();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].first == ThreadWakeupReason::Signal);
    CHECK(calls[0].second == event.get());
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].result == 0x1234);
    CHECK(env.sched.wakes[0].index == 7);
    CHECK_FALSE(static_cast<bool>(thread->wake_hook));  // cleared by the wake-up

    thread->status = ThreadStatus::Running;
    thread->wake_hook = [&](ThreadWakeupReason reason, WaitObject* object,
                            std::optional<Result>&, s32&) { calls.emplace_back(reason, object); };
    REQUIRE(Block(*thread, event, 1'000'000'000));
    env.sched.RunUntil(env.sched.now + kSecond);
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].first == ThreadWakeupReason::Timeout);
    CHECK(calls[1].second == nullptr);
    CHECK(env.sched.wakes[1].result == kResultTimeout);
}

TEST_CASE("resuming: a sleeping thread wakes without a result, and a second wake is ignored") {
    Env env;
    auto thread = env.NewThread(30);
    thread->status = ThreadStatus::WaitSleep;
    thread->ArmTimeout(1'000'000'000);
    env.sched.RunUntil(kSecond);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK_FALSE(env.sched.wakes[0].result.has_value());
    CHECK(thread->status == ThreadStatus::Ready);
    thread->ResumeFromWait(kResultSuccess);  // already Ready: the first wake counts
    CHECK(env.sched.wakes.size() == 1);

    // A Dormant thread starts with the same call.
    auto fresh = env.kernel->NewThread("fresh", 20, 0);
    fresh->ResumeFromWait(std::nullopt);
    CHECK(fresh->status == ThreadStatus::Ready);
    CHECK(env.sched.wakes.size() == 2);
}

TEST_CASE("a thread is a wait object, signalled when it ends") {
    Env env;
    auto target = env.NewThread(40);
    auto waiter = env.NewThread(30);
    CHECK(target->ShouldWait(waiter.get()));
    REQUIRE(Block(*waiter, target));
    target->Stop();
    CHECK(target->status == ThreadStatus::Dead);
    CHECK_FALSE(target->ShouldWait(waiter.get()));
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == waiter);
    CHECK(env.sched.wakes[0].index == 0);
    CHECK(env.sched.wakes[0].result == kResultSuccess);
    // A wait on a dead thread returns at once, and stopping twice is harmless.
    auto late = env.NewThread(20);
    CHECK_FALSE(Block(*late, target));
    target->Stop();
    CHECK(env.sched.wakes.size() == 1);
}

TEST_CASE("a thread that ends while it waits leaves the waiter lists") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::Sticky);
    auto thread = env.NewThread(30);
    REQUIRE(Block(*thread, event, 1'000'000'000));
    thread->Stop();
    CHECK(event->GetWaitingThreads().empty());
    CHECK(thread->wait_objects.empty());
    CHECK(env.sched.Pending() == 0);
    event->Signal();  // nobody to wake
    CHECK(env.sched.wakes.empty());
}

TEST_CASE("thread priority changes are reported to the scheduler") {
    Env env;
    auto thread = env.NewThread(48);
    thread->SetPriority(10);
    CHECK(thread->nominal_priority == 10);
    CHECK(thread->current_priority == 10);
    CHECK(thread->GetPriority() == 10);
    thread->BoostPriority(5);
    CHECK(thread->nominal_priority == 10);
    CHECK(thread->current_priority == 5);
    thread->UpdatePriority();  // no mutex held: back to the nominal priority
    CHECK(thread->current_priority == 10);
    const std::vector<FakeScheduler::PriorityChange> expected = {
        {thread.get(), 48, 10}, {thread.get(), 10, 5}, {thread.get(), 5, 10}};
    CHECK(env.sched.priority_changes == expected);
}

// ---- mutexes ---------------------------------------------------------------------------------------

TEST_CASE("mutex: created locked, it belongs to the current thread") {
    Env env;
    auto thread = env.NewThread(48);
    env.sched.current = thread.get();
    auto mutex = env.kernel->NewMutex(true);
    CHECK(mutex->LockCount() == 1);
    CHECK(mutex->HoldingThread() == thread.get());
    CHECK(mutex->Priority() == 48);
    CHECK(thread->held_mutexes.count(mutex) == 1);
    CHECK(env.sched.reschedules == 1);
    CHECK(mutex->GetHandleType() == HandleType::Mutex);
    // Acquiring recomputes the holder's priority even when nothing changes.
    const std::vector<FakeScheduler::PriorityChange> expected = {{thread.get(), 48, 48}};
    CHECK(env.sched.priority_changes == expected);
}

TEST_CASE("mutex: recursion counts, only the last release frees it") {
    Env env;
    auto a = env.NewThread(48);
    auto b = env.NewThread(30);
    auto mutex = env.kernel->NewMutex(false);
    CHECK_FALSE(mutex->ShouldWait(a.get()));
    mutex->Acquire(a.get());
    CHECK(mutex->ShouldWait(b.get()));
    CHECK_FALSE(mutex->ShouldWait(a.get()));  // the holder never waits for itself
    mutex->Acquire(a.get());
    CHECK(mutex->LockCount() == 2);
    CHECK(env.sched.reschedules == 1);  // only the first acquisition reschedules

    CHECK(mutex->Release(a.get()) == kResultSuccess);
    CHECK(mutex->LockCount() == 1);
    CHECK(mutex->HoldingThread() == a.get());
    CHECK(a->held_mutexes.count(mutex) == 1);
    CHECK(mutex->Release(a.get()) == kResultSuccess);
    CHECK(mutex->LockCount() == 0);
    CHECK(mutex->HoldingThread() == nullptr);
    CHECK(a->held_mutexes.empty());
    CHECK(env.sched.reschedules == 2);
    CHECK_FALSE(mutex->ShouldWait(b.get()));
}

TEST_CASE("mutex: a release by another thread, or of a free mutex, is kResultWrongLockingThread") {
    Env env;
    auto a = env.NewThread(48);
    auto b = env.NewThread(30);
    auto mutex = env.kernel->NewMutex(false);
    // Free: no holder, so any caller is "another thread".
    CHECK(mutex->Release(a.get()) == 0xD8E0041F);
    CHECK(kResultWrongLockingThread == 0xD8E0041F);
    mutex->Acquire(a.get());
    CHECK(mutex->Release(b.get()) == kResultWrongLockingThread);
    CHECK(mutex->LockCount() == 1);
    CHECK(mutex->HoldingThread() == a.get());
    CHECK(a->held_mutexes.count(mutex) == 1);
    // The invalid-state result of a holder with a zero count is Azahar's (1023, kernel, state).
    CHECK(kResultInvalidMutexState == 0xD8A007FF);
}

TEST_CASE("mutex: the final release hands it to the waiter") {
    Env env;
    auto a = env.NewThread(48);
    auto b = env.NewThread(30);
    auto mutex = env.kernel->NewMutex(false);
    mutex->Acquire(a.get());
    mutex->Acquire(a.get());
    REQUIRE(Block(*b, mutex));
    CHECK(b->pending_mutexes.count(mutex) == 1);

    CHECK(mutex->Release(a.get()) == kResultSuccess);  // 2 -> 1: nobody woken
    CHECK(env.sched.wakes.empty());
    CHECK(mutex->Release(a.get()) == kResultSuccess);  // 1 -> 0: b takes it
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == b);
    CHECK(env.sched.wakes[0].result == kResultSuccess);
    CHECK(env.sched.wakes[0].index == 0);
    CHECK(mutex->HoldingThread() == b.get());
    CHECK(mutex->LockCount() == 1);
    CHECK(b->held_mutexes.count(mutex) == 1);
    CHECK(b->pending_mutexes.empty());
    CHECK(a->held_mutexes.empty());
}

TEST_CASE("mutex: priority inheritance, hand-traced from Azahar's UpdatePriority") {
    Env env;
    auto a = env.NewThread(48);  // the holder
    auto b = env.NewThread(30);
    auto c = env.NewThread(10);
    auto mutex = env.kernel->NewMutex(false);

    // a takes the mutex: mutex priority = a's current priority 48; a's priority = min(48, 48).
    mutex->Acquire(a.get());
    CHECK(mutex->Priority() == 48);

    // b (30) waits: best waiter 30 != 48, so mutex = 30 and a is boosted to min(48, 30) = 30.
    REQUIRE(Block(*b, mutex));
    CHECK(mutex->Priority() == 30);
    CHECK(a->current_priority == 30);
    CHECK(a->nominal_priority == 48);

    // c (10) waits with a timeout: the best waiter is 10, a is boosted to 10.
    REQUIRE(Block(*c, mutex, 1'000'000'000));
    CHECK(mutex->Priority() == 10);
    CHECK(a->current_priority == 10);

    // c times out and leaves: the best waiter is b again (30), so a drops to 30.
    env.sched.RunUntil(kSecond);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == c);
    CHECK(env.sched.wakes[0].result == kResultTimeout);
    CHECK(mutex->Priority() == 30);
    CHECK(a->current_priority == 30);
    CHECK(c->pending_mutexes.empty());

    // a releases: it falls back to its nominal 48 (no mutex held), then b takes the mutex:
    // priority = b's current 30; after b leaves the waiter list nobody waits, so the mutex's
    // priority becomes 63 (the lowest) while b keeps min(30, 63) = 30.
    CHECK(mutex->Release(a.get()) == kResultSuccess);
    CHECK(a->current_priority == 48);
    CHECK(mutex->HoldingThread() == b.get());
    CHECK(b->current_priority == 30);
    CHECK(mutex->Priority() == 63);
    REQUIRE(env.sched.wakes.size() == 2);
    CHECK(env.sched.wakes[1].thread == b);

    // What the scheduler was told about a, call by call (also the no-op boosts):
    // acquire 48->48, b waits 48->30, c waits 30->10, c leaves 10->30, release 30->48.
    std::vector<std::pair<u32, u32>> a_changes;
    for (const auto& change : env.sched.priority_changes) {
        if (change.thread == a.get()) {
            a_changes.emplace_back(change.old_priority, change.new_priority);
        }
    }
    const std::vector<std::pair<u32, u32>> expected = {{48, 48}, {48, 30}, {30, 10}, {10, 30}, {30, 48}};
    CHECK(a_changes == expected);
    // Reschedule requests: a's acquire, b's acquire, a's final release.
    CHECK(env.sched.reschedules == 3);
}

TEST_CASE("mutex: a thread that ends releases every mutex it holds, whatever the count") {
    Env env;
    auto a = env.NewThread(48);
    auto b = env.NewThread(30);
    auto m1 = env.kernel->NewMutex(false);
    auto m2 = env.kernel->NewMutex(false);
    m1->Acquire(a.get());
    m1->Acquire(a.get());
    m2->Acquire(a.get());
    REQUIRE(Block(*b, m1));
    a->Stop();
    CHECK(a->held_mutexes.empty());
    CHECK(m2->LockCount() == 0);
    CHECK(m2->HoldingThread() == nullptr);
    // b took m1 when it was released.
    CHECK(m1->HoldingThread() == b.get());
    CHECK(m1->LockCount() == 1);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == b);
}

// ---- semaphores ------------------------------------------------------------------------------------

TEST_CASE("semaphore: creation needs initial <= max") {
    Env env;
    std::shared_ptr<Semaphore> semaphore;
    CHECK(env.kernel->NewSemaphore(&semaphore, 3, 2) == 0xD90007EE);
    CHECK(kResultInvalidCombinationKernel == 0xD90007EE);
    CHECK(semaphore == nullptr);
    CHECK(env.kernel->NewSemaphore(&semaphore, 2, 2) == kResultSuccess);
    CHECK(semaphore->AvailableCount() == 2);
    CHECK(semaphore->MaxCount() == 2);
    CHECK(env.kernel->NewSemaphore(&semaphore, 0, 0) == kResultSuccess);
}

TEST_CASE("semaphore: release returns the previous count; past the maximum is an error") {
    Env env;
    std::shared_ptr<Semaphore> semaphore;
    REQUIRE(env.kernel->NewSemaphore(&semaphore, 2, 5) == kResultSuccess);
    s32 previous = -99;
    CHECK(semaphore->Release(&previous, 2) == kResultSuccess);
    CHECK(previous == 2);
    CHECK(semaphore->AvailableCount() == 4);
    CHECK(semaphore->Release(&previous, 1) == kResultSuccess);
    CHECK(previous == 4);
    CHECK(semaphore->AvailableCount() == 5);

    // 1 + 5 > 5: 0xD8E007FD, nothing changes and the output is not written.
    previous = -99;
    CHECK(semaphore->Release(&previous, 1) == 0xD8E007FD);
    CHECK(kResultOutOfRangeKernel == 0xD8E007FD);
    CHECK(previous == -99);
    CHECK(semaphore->AvailableCount() == 5);
    CHECK(semaphore->Release(&previous, 0x7FFFFFFF) == kResultOutOfRangeKernel);  // no overflow
    CHECK(semaphore->AvailableCount() == 5);
    CHECK(semaphore->Release(&previous, 0) == kResultSuccess);  // 0 + 5 <= 5
    CHECK(previous == 5);
}

TEST_CASE("semaphore: a wait takes one count; a release wakes as many waiters as it adds") {
    Env env;
    std::shared_ptr<Semaphore> semaphore;
    REQUIRE(env.kernel->NewSemaphore(&semaphore, 1, 5) == kResultSuccess);
    auto a = env.NewThread(30);
    auto b = env.NewThread(20);
    auto c = env.NewThread(40);
    CHECK_FALSE(Block(*a, semaphore));  // takes the only count
    CHECK(semaphore->AvailableCount() == 0);
    REQUIRE(Block(*a, semaphore));
    REQUIRE(Block(*b, semaphore));
    REQUIRE(Block(*c, semaphore));

    s32 previous = -1;
    // One count: only the best priority (b, 20) gets it; its Acquire leaves nothing for the rest.
    CHECK(semaphore->Release(&previous, 1) == kResultSuccess);
    CHECK(previous == 0);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == b);
    CHECK(semaphore->AvailableCount() == 0);
    // Two counts: a (30) then c (40).
    CHECK(semaphore->Release(&previous, 2) == kResultSuccess);
    CHECK(previous == 0);
    REQUIRE(env.sched.wakes.size() == 3);
    CHECK(env.sched.wakes[1].thread == a);
    CHECK(env.sched.wakes[2].thread == c);
    CHECK(semaphore->AvailableCount() == 0);
}

// ---- timers ----------------------------------------------------------------------------------------

TEST_CASE("timer, one-shot: fires once at the initial delay, Acquire consumes the signal") {
    Env env;
    auto timer = env.kernel->NewTimer(ResetType::OneShot);
    CHECK(timer->GetHandleType() == HandleType::Timer);
    CHECK_FALSE(timer->IsArmed());
    timer->Set(1'000'000'000, 0);  // 1e9 ns = 268111856 ticks
    CHECK(timer->IsArmed());
    CHECK(timer->InitialDelay() == 1'000'000'000);
    CHECK(timer->IntervalDelay() == 0);
    CHECK(env.sched.NextTick() == kSecond);
    env.sched.RunUntil(kSecond - 1);
    CHECK_FALSE(timer->IsSignaled());
    env.sched.RunUntil(kSecond);
    CHECK(timer->IsSignaled());
    CHECK_FALSE(timer->IsArmed());  // no interval: done
    CHECK(env.sched.Pending() == 0);

    auto thread = env.NewThread(30);
    CHECK_FALSE(Block(*thread, timer));
    CHECK_FALSE(timer->IsSignaled());  // OneShot: consumed
}

TEST_CASE("timer, sticky: stays signalled until cleared") {
    Env env;
    auto timer = env.kernel->NewTimer(ResetType::Sticky);
    auto thread = env.NewThread(30);
    timer->Set(1'000'000'000, 0);
    env.sched.RunUntil(kSecond);
    CHECK_FALSE(Block(*thread, timer));
    CHECK(timer->IsSignaled());
    timer->Clear();
    CHECK_FALSE(timer->IsSignaled());
    CHECK(Block(*thread, timer));
}

TEST_CASE("timer, pulse: wakes the waiters, then clears; unseen it is lost") {
    Env env;
    auto timer = env.kernel->NewTimer(ResetType::Pulse);
    auto thread = env.NewThread(30);
    REQUIRE(Block(*thread, timer));
    timer->Set(1'000'000'000, 0);
    env.sched.RunUntil(kSecond);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == thread);
    CHECK_FALSE(timer->IsSignaled());

    // Nobody waits at the second shot: cleared at once, so a later wait blocks.
    timer->Set(1'000'000'000, 0);
    env.sched.RunUntil(2 * kSecond);
    CHECK_FALSE(timer->IsSignaled());
    thread->status = ThreadStatus::Running;
    CHECK(Block(*thread, timer));
}

TEST_CASE("timer, periodic: shots at the initial delay and then every interval, without drift") {
    Env env;
    auto timer = env.kernel->NewTimer(ResetType::Pulse);
    int shots = 0;
    timer->SetHleNotifier([&] { ++shots; });  // runs once per shot
    auto thread = env.NewThread(30);
    REQUIRE(Block(*thread, timer));
    // Initial 1 s = 268111856 ticks, interval 0.5 s = 134055928 ticks: shots at 268111856,
    // 402167784, 536223712, 670279640, 804335568, 938391496 and 1072447424.
    timer->Set(1'000'000'000, 500'000'000);
    CHECK(timer->IntervalDelay() == 500'000'000);
    env.sched.RunUntil(268111855);
    CHECK(shots == 0);
    env.sched.RunUntil(268111856);
    CHECK(shots == 1);
    CHECK(env.sched.wakes.size() == 1);
    CHECK(timer->IsArmed());
    CHECK(env.sched.NextTick() == 402167784);
    thread->status = ThreadStatus::Running;
    REQUIRE(Block(*thread, timer));
    env.sched.RunUntil(402167783);
    CHECK(shots == 1);
    env.sched.RunUntil(402167784);
    CHECK(shots == 2);
    CHECK(env.sched.wakes.size() == 2);
    // The clock jumps far ahead (the host was late): the missed shots fire back to back, each on
    // its own deadline. Up to 1e9 ticks: 268111856 + k * 134055928 for k = 0..5 (938391496 <= 1e9
    // < 1072447424), so three more shots.
    env.sched.RunUntil(1'000'000'000);
    CHECK(shots == 6);
    CHECK(env.sched.NextTick() == 1072447424);
}

TEST_CASE("timer: Cancel, Clear and re-arming") {
    Env env;
    auto timer = env.kernel->NewTimer(ResetType::Sticky);
    timer->Set(1'000'000'000, 500'000'000);
    env.sched.RunUntil(kSecond);
    CHECK(timer->IsSignaled());
    timer->Clear();
    CHECK_FALSE(timer->IsSignaled());
    timer->Cancel();
    CHECK_FALSE(timer->IsArmed());
    CHECK(env.sched.Pending() == 0);
    env.sched.RunUntil(10 * kSecond);
    CHECK_FALSE(timer->IsSignaled());

    // Set replaces the previous arming: only the 2 s shot (536223712 ticks) happens.
    const u64 base = env.sched.now;
    timer->Set(1'000'000'000, 0);
    timer->Set(2'000'000'000, 0);
    CHECK(env.sched.Pending() == 1);
    env.sched.RunUntil(base + kSecond);
    CHECK_FALSE(timer->IsSignaled());
    env.sched.RunUntil(base + 2 * kSecond);
    CHECK(timer->IsSignaled());
}

TEST_CASE("timer: an initial delay of 0 signals at once, then runs on its interval") {
    Env env;
    auto timer = env.kernel->NewTimer(ResetType::Sticky);
    env.sched.now = 1000;
    timer->Set(0, 1'000'000'000);
    CHECK(timer->IsSignaled());
    CHECK(timer->IsArmed());
    timer->Clear();
    // Next shot one interval after the immediate one: 1000 + 268111856.
    CHECK(env.sched.NextTick() == 268112856);
    env.sched.RunUntil(268112855);
    CHECK_FALSE(timer->IsSignaled());
    env.sched.RunUntil(268112856);
    CHECK(timer->IsSignaled());
}

TEST_CASE("timer: delays past 68 s need the 128-bit conversion") {
    Env env;
    auto timer = env.kernel->NewTimer(ResetType::OneShot);
    // 100 s: 1e11 * 268111856 = 2.68e19 overflows 64 bits (1.8e19). 100 * 268111856 =
    // 26811185600 ticks.
    timer->Set(100'000'000'000, 0);
    CHECK(env.sched.NextTick() == 26811185600ull);
    // A delay that is not a whole number of seconds rounds down: 1500 ns * 268111856 / 1e9 =
    // 402.167784 -> 402 ticks.
    timer->Set(1500, 0);
    CHECK(env.sched.NextTick() == 402);
}

TEST_CASE("timer: a destroyed timer takes its pending shot out of the queue") {
    Env env;
    {
        auto timer = env.kernel->NewTimer(ResetType::OneShot);
        timer->Set(1'000'000'000, 1'000'000'000);
        CHECK(env.sched.Pending() == 1);
    }
    CHECK(env.sched.Pending() == 0);
    env.sched.RunUntil(10 * kSecond);
}

// ---- address arbiters ------------------------------------------------------------------------------

namespace {

constexpr u32 kArbAddress = 0x08000010;
constexpr u32 kArbOther = 0x08000020;

struct ArbiterEnv : Env {
    std::shared_ptr<AddressArbiter> arbiter;
    ArbiterEnv() {
        REQUIRE(memory->Map(0x08000000, 0x1000, MemPerm::ReadWrite, MemState::Private) ==
                kResultSuccess);
        arbiter = kernel->NewAddressArbiter("arb");
    }
    void Poke(u32 address, s32 value) { REQUIRE(memory->Write<s32>(address, value)); }
    s32 Peek(u32 address) { return memory->Read<s32>(address).value(); }
    Result Arbitrate(const std::shared_ptr<Thread>& thread, ArbitrationType type, u32 address,
                     s32 value, s64 timeout_ns = -1) {
        return arbiter->ArbitrateAddress(thread, type, address, value, timeout_ns);
    }
};

}  // namespace

TEST_CASE("arbiter, signal: wakes `count` threads, best priority first, earliest among equals") {
    ArbiterEnv env;
    auto t1 = env.NewThread(30);
    auto t2 = env.NewThread(10);
    auto t3 = env.NewThread(20);
    auto t4 = env.NewThread(20);
    auto t5 = env.NewThread(5);  // waits on another address
    env.Poke(kArbAddress, 0);
    env.Poke(kArbOther, 0);
    for (auto* t : {&t1, &t2, &t3, &t4}) {
        CHECK(env.Arbitrate(*t, ArbitrationType::WaitIfLessThan, kArbAddress, 1) == kResultSuccess);
    }
    CHECK(env.Arbitrate(t5, ArbitrationType::WaitIfLessThan, kArbOther, 1) == kResultSuccess);
    CHECK(t1->status == ThreadStatus::WaitArb);
    CHECK(t1->wait_address == kArbAddress);
    CHECK(env.arbiter->WaitingThreads().size() == 5);

    // List: t1(30) t2(10) t3(20) t4(20) t5(5, other address). Signal 1: t2.
    CHECK(env.Arbitrate(t1, ArbitrationType::Signal, kArbAddress, 1) == kResultSuccess);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == t2);
    CHECK(env.sched.wakes[0].result == kResultSuccess);
    CHECK(env.sched.wakes[0].index == -1);
    CHECK(t2->status == ThreadStatus::Ready);
    // Signal 2 of t1(30) t3(20) t4(20): t3 then t4 (equal priority: list order), not t5.
    env.Arbitrate(t1, ArbitrationType::Signal, kArbAddress, 2);
    REQUIRE(env.sched.wakes.size() == 3);
    CHECK(env.sched.wakes[1].thread == t3);
    CHECK(env.sched.wakes[2].thread == t4);
    // Signal more than there are: the rest (t1) and nothing from the other address.
    env.Arbitrate(t1, ArbitrationType::Signal, kArbAddress, 10);
    REQUIRE(env.sched.wakes.size() == 4);
    CHECK(env.sched.wakes[3].thread == t1);
    CHECK(env.arbiter->WaitingThreads().size() == 1);
    CHECK(t5->status == ThreadStatus::WaitArb);
}

TEST_CASE("arbiter, signal with a negative count wakes all, in arrival order") {
    ArbiterEnv env;
    auto t1 = env.NewThread(30);
    auto t2 = env.NewThread(10);
    auto t3 = env.NewThread(20);
    env.Poke(kArbAddress, 0);
    for (auto* t : {&t1, &t2, &t3}) {
        env.Arbitrate(*t, ArbitrationType::WaitIfLessThan, kArbAddress, 1);
    }
    CHECK(env.Arbitrate(t1, ArbitrationType::Signal, kArbAddress, -1) == kResultSuccess);
    REQUIRE(env.sched.wakes.size() == 3);
    // All of them, in the order they started waiting (not by priority).
    CHECK(env.sched.wakes[0].thread == t1);
    CHECK(env.sched.wakes[1].thread == t2);
    CHECK(env.sched.wakes[2].thread == t3);
    CHECK(env.arbiter->WaitingThreads().empty());
    // Signalling when nobody waits is harmless.
    CHECK(env.Arbitrate(t1, ArbitrationType::Signal, kArbAddress, 1) == kResultSuccess);
    CHECK(env.sched.wakes.size() == 3);
}

TEST_CASE("arbiter, wait if less than: a signed comparison of the word at the address") {
    ArbiterEnv env;
    auto thread = env.NewThread(30);
    env.Poke(kArbAddress, 5);
    // 5 < 6: waits. 5 < 5: does not.
    CHECK(env.Arbitrate(thread, ArbitrationType::WaitIfLessThan, kArbAddress, 5) == kResultSuccess);
    CHECK(thread->status == ThreadStatus::Running);
    CHECK(env.arbiter->WaitingThreads().empty());
    CHECK(env.Arbitrate(thread, ArbitrationType::WaitIfLessThan, kArbAddress, 6) == kResultSuccess);
    CHECK(thread->status == ThreadStatus::WaitArb);
    CHECK(env.arbiter->WaitingThreads().size() == 1);
    env.Arbitrate(thread, ArbitrationType::Signal, kArbAddress, 1);

    // 0xFFFFFFFF is -1: less than 0. 0x80000000 is INT_MIN: less than -5.
    auto t2 = env.NewThread(30);
    env.Poke(kArbAddress, -1);
    env.Arbitrate(t2, ArbitrationType::WaitIfLessThan, kArbAddress, 0);
    CHECK(t2->status == ThreadStatus::WaitArb);
    env.Arbitrate(t2, ArbitrationType::Signal, kArbAddress, -1);
    auto t3 = env.NewThread(30);
    env.Poke(kArbAddress, static_cast<s32>(0x80000000u));
    env.Arbitrate(t3, ArbitrationType::WaitIfLessThan, kArbAddress, -5);
    CHECK(t3->status == ThreadStatus::WaitArb);
}

TEST_CASE("arbiter, decrement and wait: the word is decremented only when the thread waits") {
    ArbiterEnv env;
    auto thread = env.NewThread(30);
    env.Poke(kArbAddress, 5);
    CHECK(env.Arbitrate(thread, ArbitrationType::DecrementAndWaitIfLessThan, kArbAddress, 6) ==
          kResultSuccess);
    CHECK(thread->status == ThreadStatus::WaitArb);
    CHECK(env.Peek(kArbAddress) == 4);
    auto t2 = env.NewThread(30);
    // 4 < 4 is false: no wait and no decrement.
    CHECK(env.Arbitrate(t2, ArbitrationType::DecrementAndWaitIfLessThan, kArbAddress, 4) ==
          kResultSuccess);
    CHECK(t2->status == ThreadStatus::Running);
    CHECK(env.Peek(kArbAddress) == 4);
}

TEST_CASE("arbiter, timeout types: always kResultTimeout, and a timeout takes the thread off") {
    ArbiterEnv env;
    auto thread = env.NewThread(30);
    env.Poke(kArbAddress, 5);
    // Not waiting (5 < 5 false): still the Timeout code, and no timer is armed.
    CHECK(env.Arbitrate(thread, ArbitrationType::WaitIfLessThanWithTimeout, kArbAddress, 5,
                        1'000'000'000) == 0x09401BFE);
    CHECK(thread->status == ThreadStatus::Running);
    CHECK(env.sched.Pending() == 0);

    // Waiting with 1 s: woken by the timeout at 268111856 ticks with the Timeout result, and
    // removed from the arbiter, so a later signal finds nobody.
    CHECK(env.Arbitrate(thread, ArbitrationType::WaitIfLessThanWithTimeout, kArbAddress, 6,
                        1'000'000'000) == kResultTimeout);
    CHECK(thread->status == ThreadStatus::WaitArb);
    CHECK(env.sched.NextTick() == kSecond);
    env.sched.RunUntil(kSecond - 1);
    CHECK(env.sched.wakes.empty());
    env.sched.RunUntil(kSecond);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].result == kResultTimeout);
    CHECK(env.sched.wakes[0].index == -1);
    CHECK(thread->status == ThreadStatus::Ready);
    CHECK(env.arbiter->WaitingThreads().empty());
    env.Arbitrate(thread, ArbitrationType::Signal, kArbAddress, -1);
    CHECK(env.sched.wakes.size() == 1);
}

TEST_CASE("arbiter, timeout types: a signal before the timeout keeps Azahar's result and cancels it") {
    ArbiterEnv env;
    auto thread = env.NewThread(30);
    env.Poke(kArbAddress, 0);
    CHECK(env.Arbitrate(thread, ArbitrationType::DecrementAndWaitIfLessThanWithTimeout, kArbAddress,
                        1, 5'000'000'000) == kResultTimeout);
    CHECK(env.Peek(kArbAddress) == -1);
    CHECK(env.sched.Pending() == 1);
    env.Arbitrate(thread, ArbitrationType::Signal, kArbAddress, 1);
    REQUIRE(env.sched.wakes.size() == 1);
    // The call returned kResultTimeout when it was made, and nothing changes r0 afterwards.
    CHECK(env.sched.wakes[0].result == kResultTimeout);
    CHECK(env.sched.Pending() == 0);
    env.sched.RunUntil(100 * kSecond);
    CHECK(env.sched.wakes.size() == 1);

    // The types without a timeout are woken with Success.
    auto t2 = env.NewThread(30);
    env.Arbitrate(t2, ArbitrationType::WaitIfLessThan, kArbAddress, 1);
    env.Arbitrate(t2, ArbitrationType::Signal, kArbAddress, 1);
    REQUIRE(env.sched.wakes.size() == 2);
    CHECK(env.sched.wakes[1].result == kResultSuccess);
}

TEST_CASE("arbiter, timeout of -1 never expires, 0 expires at once") {
    ArbiterEnv env;
    auto t1 = env.NewThread(30);
    auto t2 = env.NewThread(30);
    env.Poke(kArbAddress, 0);
    env.sched.now = 777;
    env.Arbitrate(t1, ArbitrationType::WaitIfLessThanWithTimeout, kArbAddress, 1, -1);
    CHECK(env.sched.Pending() == 0);
    env.Arbitrate(t2, ArbitrationType::WaitIfLessThanWithTimeout, kArbAddress, 1, 0);
    CHECK(env.sched.NextTick() == 777);
    env.sched.RunUntil(777);
    REQUIRE(env.sched.wakes.size() == 1);
    CHECK(env.sched.wakes[0].thread == t2);
    CHECK(t1->status == ThreadStatus::WaitArb);
}

TEST_CASE("arbiter: an unknown type is kResultInvalidEnumValueFnd; unreadable memory reads as 0") {
    ArbiterEnv env;
    auto thread = env.NewThread(30);
    env.Poke(kArbAddress, 5);
    CHECK(env.Arbitrate(thread, static_cast<ArbitrationType>(5), kArbAddress, 6) == 0xD8E093ED);
    CHECK(kResultInvalidEnumValueFnd == 0xD8E093ED);
    CHECK(thread->status == ThreadStatus::Running);
    CHECK(env.Peek(kArbAddress) == 5);

    // Nothing is mapped at 0x08100000: Azahar's Read32 gives 0, so 0 < 1 waits; the decrement's
    // write is lost without a crash.
    SetLogSink([](const LogRecord&) {});
    CHECK(env.Arbitrate(thread, ArbitrationType::WaitIfLessThan, 0x08100000, 1) == kResultSuccess);
    CHECK(thread->status == ThreadStatus::WaitArb);
    auto t2 = env.NewThread(30);
    CHECK(env.Arbitrate(t2, ArbitrationType::DecrementAndWaitIfLessThan, 0x08100000, 1) ==
          kResultSuccess);
    CHECK(t2->status == ThreadStatus::WaitArb);
    SetLogSink({});
}

TEST_CASE("arbiter: a thread that ends while it waits is dropped from the arbiter") {
    ArbiterEnv env;
    auto thread = env.NewThread(30);
    env.Poke(kArbAddress, 0);
    env.Arbitrate(thread, ArbitrationType::WaitIfLessThan, kArbAddress, 1);
    REQUIRE(env.arbiter->WaitingThreads().size() == 1);
    thread->Stop();
    CHECK(env.arbiter->WaitingThreads().empty());
    env.Arbitrate(thread, ArbitrationType::Signal, kArbAddress, -1);
    CHECK(env.sched.wakes.empty());
}

// ---- shared memory ---------------------------------------------------------------------------------

TEST_CASE("shared memory at address 0: linear first fit from the BASE region, mapped in the linear heap") {
    Env env;  // memory mode 0: APPLICATION 0x04000000, SYSTEM 0x02C00000, BASE 0x01400000
    // BASE starts at 0x04000000 + 0x02C00000 = 0x06C00000 (FCRAM offset).
    CHECK(env.kernel->Regions().Base(FcramRegion::Base) == 0x06C00000);
    std::shared_ptr<SharedMemory> first, second, third;
    REQUIRE(env.kernel->NewSharedMemory(&first, env.process, 0x3000, MemoryPermission::ReadWrite,
                                        MemoryPermission::DontCare, 0, FcramRegion::Base,
                                        "first") == kResultSuccess);
    CHECK(first->LinearHeapPhysicalOffset() == 0x06C00000);
    CHECK(first->Size() == 0x3000);
    CHECK(env.process->memory_used == 0x3000);
    CHECK(env.kernel->Regions().Used(FcramRegion::Base) == 0x3000);
    REQUIRE(env.kernel->NewSharedMemory(&second, env.process, 0x1000, MemoryPermission::ReadWrite,
                                        MemoryPermission::DontCare, 0, FcramRegion::Base) ==
            kResultSuccess);
    CHECK(second->LinearHeapPhysicalOffset() == 0x06C03000);

    // Mapped at kLinearHeapVaddr + the FCRAM offset: 0x14000000 + 0x06C00000 = 0x1AC00000.
    CHECK(first->Map(*env.process, 0, MemoryPermission::ReadWrite, MemoryPermission::DontCare) ==
          kResultSuccess);
    CHECK(first->MappedAddress() == 0x1AC00000);
    CHECK(first->IsMapped());
    CHECK(env.memory->Query(0x1AC00000) ==
          MemoryInfo{0x1AC00000, 0x3000, MemPerm::ReadWrite, MemState::Shared});
    CHECK(second->Map(*env.process, 0, MemoryPermission::ReadWrite, MemoryPermission::DontCare) ==
          kResultSuccess);
    CHECK(second->MappedAddress() == 0x1AC03000);
    // The two pieces of FCRAM are adjacent (0x06C03000 ends the first), so the block list
    // merges them as Azahar's VMAs do: one block of 0x4000.
    CHECK(env.memory->Query(0x1AC00000) ==
          MemoryInfo{0x1AC00000, 0x4000, MemPerm::ReadWrite, MemState::Shared});

    // The memory is zero, and services reach it through the guest range.
    CHECK(env.memory->Read<u32>(0x1AC00000) == 0u);
    u8* pointer = first->GetPointer(0x10);
    CHECK(pointer == env.memory->Base() + 0x1AC00010);
    *reinterpret_cast<u32*>(pointer) = 0xCAFEBABE;
    CHECK(env.memory->Read<u32>(0x1AC00010) == 0xCAFEBABE);

    // Unmapping brings the contents home; mapping again restores them. Unmapping anywhere else
    // fails and changes nothing.
    CHECK(first->Unmap(*env.process, 0x1AC01000) == kResultInvalidAddressState);
    CHECK(first->IsMapped());
    CHECK(first->Unmap(*env.process, 0x1AC00000) == kResultSuccess);
    CHECK_FALSE(first->IsMapped());
    CHECK(env.memory->Query(0x1AC00000).value().state == MemState::Free);
    CHECK(env.memory->Query(0x1AC03000) ==
          MemoryInfo{0x1AC03000, 0x1000, MemPerm::ReadWrite, MemState::Shared});
    CHECK(*reinterpret_cast<u32*>(first->GetPointer(0x10)) == 0xCAFEBABE);  // host memory now
    CHECK(first->Map(*env.process, 0, MemoryPermission::ReadWrite, MemoryPermission::DontCare) ==
          kResultSuccess);
    CHECK(env.memory->Read<u32>(0x1AC00010) == 0xCAFEBABE);

    // Destroying a block gives its FCRAM back and its bytes to the process: after unmapping the
    // first, a new 0x3000 block takes the same first-fit offset again.
    REQUIRE(first->Unmap(*env.process, 0x1AC00000) == kResultSuccess);
    first.reset();
    CHECK(env.process->memory_used == 0x1000);
    CHECK(env.kernel->Regions().Used(FcramRegion::Base) == 0x1000);
    REQUIRE(env.kernel->NewSharedMemory(&third, env.process, 0x3000, MemoryPermission::ReadWrite,
                                        MemoryPermission::DontCare, 0, FcramRegion::Base) ==
            kResultSuccess);
    CHECK(third->LinearHeapPhysicalOffset() == 0x06C00000);
    CHECK(env.process->memory_used == 0x4000);
    // The map clears the pages: a new block starts zeroed, not with the old contents.
    CHECK(third->Map(*env.process, 0, MemoryPermission::ReadWrite, MemoryPermission::DontCare) ==
          kResultSuccess);
    CHECK(env.memory->Read<u32>(0x1AC00010) == 0u);
}

TEST_CASE("shared memory at address 0 follows the memory mode's BASE region") {
    Env env(6);  // New 3DS: APPLICATION 0x07C00000 + SYSTEM 0x06400000 = 0x0E000000
    std::shared_ptr<SharedMemory> block;
    REQUIRE(env.kernel->NewSharedMemory(&block, env.process, 0x2000, MemoryPermission::ReadWrite,
                                        MemoryPermission::DontCare, 0, FcramRegion::Base) ==
            kResultSuccess);
    CHECK(block->LinearHeapPhysicalOffset() == 0x0E000000);
    // Even on a New 3DS the target is in the old linear heap window: 0x14000000 + 0x0E000000.
    CHECK(block->Map(*env.process, 0, MemoryPermission::ReadWrite, MemoryPermission::DontCare) ==
          kResultSuccess);
    CHECK(block->MappedAddress() == 0x22000000);
}

TEST_CASE("shared memory at address 0 can be mapped at an address of the caller's choosing") {
    Env env;
    std::shared_ptr<SharedMemory> block;
    REQUIRE(env.kernel->NewSharedMemory(&block, env.process, 0x2000, MemoryPermission::ReadWrite,
                                        MemoryPermission::DontCare, 0, FcramRegion::Base) ==
            kResultSuccess);
    CHECK(block->Map(*env.process, 0x10000000, MemoryPermission::Read, MemoryPermission::DontCare) ==
          kResultSuccess);
    CHECK(env.memory->Query(0x10000000) ==
          MemoryInfo{0x10000000, 0x2000, MemPerm::Read, MemState::Shared});
    // A second mapping of the same block is unsupported (host aliasing).
    SetLogSink([](const LogRecord&) {});
    CHECK(block->Map(*env.process, 0x10100000, MemoryPermission::Read, MemoryPermission::DontCare) ==
          kResultInvalidAddressState);
    SetLogSink({});
    CHECK(env.memory->Query(0x10100000).value().state == MemState::Free);
}

TEST_CASE("shared memory Map: the permission rules, in Azahar's order, with their result codes") {
    Env env;
    constexpr auto RW = MemoryPermission::ReadWrite;
    constexpr auto R = MemoryPermission::Read;
    constexpr auto DC = MemoryPermission::DontCare;
    std::shared_ptr<SharedMemory> automatic;
    REQUIRE(env.kernel->NewSharedMemory(&automatic, env.process, 0x1000, RW, DC, 0, FcramRegion::Base) ==
            kResultSuccess);
    // 1. A block that was allocated automatically maps only with other = DontCare.
    CHECK(automatic->Map(*env.process, 0, RW, R) == 0xE0E01BEE);
    CHECK(kResultInvalidCombination == 0xE0E01BEE);

    // Kernel-created blocks have a base address, so: owner is "nobody", and `other_permissions`
    // of the block applies to every mapper.
    auto hid = env.kernel->NewSharedMemoryForApplet(0, 0x1000, R, R, "hid");
    REQUIRE(hid != nullptr);
    // 2. The requested permissions must lie within the block's other permissions: RW is not in R.
    CHECK(hid->Map(*env.process, 0x08100000, RW, R) == kResultInvalidCombination);
    // 3. Such a block cannot be mapped with other = DontCare.
    CHECK(hid->Map(*env.process, 0x08100000, R, DC) == kResultInvalidCombination);
    // 4. other = Read, but the block's own permissions (RW) are not within it: wrong permission.
    auto gsp = env.kernel->NewSharedMemoryForApplet(0, 0x1000, RW, RW, "gsp");
    REQUIRE(gsp != nullptr);
    CHECK(gsp->Map(*env.process, 0x08100000, R, R) == 0xD900182E);
    CHECK(kResultWrongPermission == 0xD900182E);
    // Nothing was mapped by the failed attempts.
    CHECK(env.memory->Query(0x08100000).value().state == MemState::Free);
}

TEST_CASE("shared memory Map: the owner's permissions apply to the owner, the block's to others") {
    Env env;
    constexpr auto RW = MemoryPermission::ReadWrite;
    constexpr auto R = MemoryPermission::Read;
    // Over the app's own memory (see the next test for the Locked state): owner RW, others R.
    REQUIRE(env.memory->Map(0x08400000, 0x1000, MemPerm::ReadWrite, MemState::Private) ==
            kResultSuccess);
    std::shared_ptr<SharedMemory> block;
    REQUIRE(env.kernel->NewSharedMemory(&block, env.process, 0x1000, RW, R, 0x08400000,
                                        FcramRegion::Base) == kResultSuccess);
    Process stranger(*env.kernel, 99);
    // A stranger asking for RW: RW & ~R != 0 (the block's other permissions are R).
    CHECK(block->Map(stranger, 0x08500000, RW, R) == kResultInvalidCombination);
    // The owner asking for RW passes rule 2 (its own permissions are RW), rule 3 wants a real
    // `other` (Read here), rule 4: block permissions RW within Read? No: wrong permission.
    CHECK(block->Map(*env.process, 0x08500000, RW, R) == kResultWrongPermission);
}

TEST_CASE("shared memory Map: address range and the target must be free") {
    Env env;
    constexpr auto R = MemoryPermission::Read;
    auto block = env.kernel->NewSharedMemoryForApplet(0, 0x1000, R, R, "b");
    REQUIRE(block != nullptr);
    // Below the heap, and ending at or after 0x14000000 (the end of the shared memory area).
    CHECK(block->Map(*env.process, 0x07FFF000, R, R) == 0xE0E01BF5);
    CHECK(kResultInvalidAddress == 0xE0E01BF5);
    CHECK(block->Map(*env.process, 0x13FFF000, R, R) == kResultInvalidAddress);  // 0x13FFF000 + 0x1000
    CHECK(block->Map(*env.process, 0xFFFFF000, R, R) == kResultInvalidAddress);  // no 32-bit wrap
    // 0x13FFE000 + 0x1000 < 0x14000000 is fine.
    CHECK(block->Map(*env.process, 0x13FFE000, R, R) == kResultSuccess);
    CHECK(block->Unmap(*env.process, 0x13FFE000) == kResultSuccess);

    // Not free: the target is already mapped, or free but cut short by a neighbour.
    REQUIRE(env.memory->Map(0x08100000, 0x1000, MemPerm::ReadWrite, MemState::Private) ==
            kResultSuccess);
    CHECK(block->Map(*env.process, 0x08100000, R, R) == 0xE0A01BF5);
    CHECK(kResultInvalidAddressState == 0xE0A01BF5);
    auto big = env.kernel->NewSharedMemoryForApplet(0, 0x2000, R, R, "big");
    REQUIRE(big != nullptr);
    CHECK(big->Map(*env.process, 0x080FF000, R, R) == kResultInvalidAddressState);
    CHECK(big->Map(*env.process, 0x08101000, R, R) == kResultSuccess);
}

TEST_CASE("kernel-created shared memory: held in the SYSTEM region, its contents move on the first map") {
    Env env;
    constexpr auto R = MemoryPermission::Read;
    const auto system_free_before = env.kernel->Regions().FreeBlocks(FcramRegion::System);
    REQUIRE(system_free_before.size() == 1);
    CHECK(system_free_before[0] == FcramRange{0x04000000, 0x02C00000});

    auto block = env.kernel->NewSharedMemoryForApplet(0, 0x2000, R, R, "hid");
    REQUIRE(block != nullptr);
    // HeapAllocate takes from the top of the region: [0x06C00000 - 0x2000, 0x06C00000).
    const auto free_after = env.kernel->Regions().FreeBlocks(FcramRegion::System);
    REQUIRE(free_after.size() == 1);
    CHECK(free_after[0] == FcramRange{0x04000000, 0x02BFE000});
    CHECK(env.kernel->Regions().Used(FcramRegion::System) == 0x2000);

    // Before it is mapped the contents are in host memory (zero), and the service fills them.
    u8* host = block->GetPointer();
    CHECK(host[0] == 0);
    host[0] = 0x11;
    host[0x1FFF] = 0x22;
    CHECK_FALSE(block->IsMapped());
    CHECK(block->Map(*env.process, 0x08100000, R, R) == kResultSuccess);
    CHECK(env.memory->Query(0x08100000) ==
          MemoryInfo{0x08100000, 0x2000, MemPerm::Read, MemState::Shared});
    // Copied to the guest range, which is now where services find the block.
    CHECK(env.memory->Read<u8>(0x08100000) == 0x11);
    CHECK(env.memory->Read<u8>(0x08101FFF) == 0x22);
    CHECK(block->GetPointer() == env.memory->Base() + 0x08100000);
    *block->GetPointer(1) = 0x33;
    CHECK(env.memory->Read<u8>(0x08100001) == 0x33);
    CHECK(block->Unmap(*env.process, 0x08100000) == kResultSuccess);
    CHECK(block->GetPointer()[1] == 0x33);

    // Destroying it gives the SYSTEM memory back.
    block.reset();
    CHECK(env.kernel->Regions().Used(FcramRegion::System) == 0);
    CHECK(env.kernel->Regions().FreeBlocks(FcramRegion::System) == system_free_before);
}

TEST_CASE("kernel-created shared memory: a block taken in pieces maps one piece after the other") {
    Env env;
    constexpr auto R = MemoryPermission::Read;
    auto& regions = env.kernel->Regions();
    // Fragment the SYSTEM region: take 0x1000 pieces from the top and free every other one, then
    // ask for 0x2000, which cannot be one piece any more.
    std::vector<std::shared_ptr<SharedMemory>> keep;
    for (int i = 0; i < 4; ++i) {
        keep.push_back(env.kernel->NewSharedMemoryForApplet(0, 0x1000, R, R, "f"));
        REQUIRE(keep.back() != nullptr);
    }
    // Pieces, from the top: [0x06BFF000,+0x1000) (keep[0]), [0x06BFE000,..) (1), 0x06BFD000 (2),
    // 0x06BFC000 (3). Free 0 and 2: free space is [.., 0x06BFC000)? No: 3 is at the bottom, so
    // free are the top piece, the piece at 0x06BFD000, and everything below keep[3].
    keep[0].reset();
    keep[2].reset();
    auto spread = env.kernel->NewSharedMemoryForApplet(0, 0x2000, R, R, "spread");
    REQUIRE(spread != nullptr);
    // HeapAllocate walks the free intervals from the top: the top piece [0x06BFF000, 0x06C00000)
    // first, then [0x06BFD000, 0x06BFE000). Two pieces, returned in ascending order.
    CHECK(regions.Used(FcramRegion::System) == 0x4000);  // keep[1], keep[3], spread
    CHECK(spread->Map(*env.process, 0x08100000, R, R) == kResultSuccess);
    // Pieces mapped back to back with their own FCRAM backings (not contiguous): two blocks.
    CHECK(env.memory->Query(0x08100000) ==
          MemoryInfo{0x08100000, 0x1000, MemPerm::Read, MemState::Shared});
    CHECK(env.memory->Query(0x08101000) ==
          MemoryInfo{0x08101000, 0x1000, MemPerm::Read, MemState::Shared});
    // The contents are one run of bytes across the pieces.
    CHECK(spread->Unmap(*env.process, 0x08100000) == kResultSuccess);
    spread->GetPointer()[0x1000] = 0x77;
    CHECK(spread->Map(*env.process, 0x08200000, R, R) == kResultSuccess);
    CHECK(env.memory->Read<u8>(0x08201000) == 0x77);
}

TEST_CASE("shared memory over the app's own memory: Locked while the block lives") {
    Env env;
    auto limit = env.kernel->ApplicationResourceLimit();
    // The SVC reserves the slot first; the block's destructor releases it.
    limit->Reserve(ResourceLimitType::SharedMemory, 1);
    REQUIRE(env.memory->Map(0x08300000, 0x2000, MemPerm::ReadWrite, MemState::Private) ==
            kResultSuccess);
    std::shared_ptr<SharedMemory> block;
    REQUIRE(env.kernel->NewSharedMemory(&block, env.process, 0x2000, MemoryPermission::Read,
                                        MemoryPermission::Read, 0x08300000,
                                        FcramRegion::Base) == kResultSuccess);
    CHECK(env.memory->Query(0x08300000) ==
          MemoryInfo{0x08300000, 0x2000, MemPerm::Read, MemState::Locked});
    CHECK(env.process->memory_used == 0);  // only address-0 blocks count
    CHECK(env.kernel->Regions().Used(FcramRegion::Base) == 0);
    // The contents are the app's: always reached through the guest range.
    CHECK(block->GetPointer(8) == env.memory->Base() + 0x08300008);

    // It cannot be mapped anywhere else (host aliasing is unsupported), and not on itself.
    SetLogSink([](const LogRecord&) {});
    CHECK(block->Map(*env.process, 0x08600000, MemoryPermission::Read, MemoryPermission::Read) ==
          kResultInvalidAddressState);
    SetLogSink({});
    CHECK(block->Map(*env.process, 0x08300000, MemoryPermission::Read, MemoryPermission::Read) ==
          kResultInvalidAddressState);

    CHECK(limit->GetCurrentValue(ResourceLimitType::SharedMemory) == 1);
    block.reset();
    CHECK(env.memory->Query(0x08300000) ==
          MemoryInfo{0x08300000, 0x2000, MemPerm::ReadWrite, MemState::Private});
    CHECK(limit->GetCurrentValue(ResourceLimitType::SharedMemory) == 0);
}

TEST_CASE("shared memory over memory that is not Private and ReadWrite fails and releases the slot") {
    Env env;
    auto limit = env.kernel->ApplicationResourceLimit();
    limit->Reserve(ResourceLimitType::SharedMemory, 1);
    std::shared_ptr<SharedMemory> block;
    // Nothing mapped there: the state change fails with Azahar's invalid-address-state result.
    CHECK(env.kernel->NewSharedMemory(&block, env.process, 0x1000, MemoryPermission::ReadWrite,
                                      MemoryPermission::ReadWrite, 0x08300000,
                                      FcramRegion::Base) == 0xE0A01BF5);
    CHECK(block == nullptr);
    CHECK(limit->GetCurrentValue(ResourceLimitType::SharedMemory) == 0);  // balanced
    CHECK(env.process->memory_used == 0);
}

TEST_CASE("shared memory at address 0 with no room in the region is kResultOutOfMemory") {
    Env env;
    // The failed blocks are destroyed and release a resource limit slot nobody reserved: logged.
    SetLogSink([](const LogRecord&) {});
    std::shared_ptr<SharedMemory> block;
    // BASE is 0x01400000 bytes.
    CHECK(env.kernel->NewSharedMemory(&block, env.process, 0x01401000, MemoryPermission::ReadWrite,
                                      MemoryPermission::DontCare, 0, FcramRegion::Base) ==
          0xD86007F3);
    CHECK(block == nullptr);
    CHECK(env.kernel->NewSharedMemoryForApplet(0, 0x02C01000, MemoryPermission::Read,
                                               MemoryPermission::Read) == nullptr);
    // A region that is partly used: the second block does not fit after the first.
    REQUIRE(env.kernel->NewSharedMemory(&block, env.process, 0x01000000, MemoryPermission::ReadWrite,
                                        MemoryPermission::DontCare, 0, FcramRegion::Base) ==
            kResultSuccess);
    std::shared_ptr<SharedMemory> second;
    CHECK(env.kernel->NewSharedMemory(&second, env.process, 0x00500000, MemoryPermission::ReadWrite,
                                      MemoryPermission::DontCare, 0, FcramRegion::Base) ==
          kResultOutOfMemory);
    CHECK(env.process->memory_used == 0x01000000);
    SetLogSink({});
}

// ---- resource limits -------------------------------------------------------------------------------

TEST_CASE("the APPLICATION resource limit has Azahar's values for the memory mode") {
    // Commit is the size of the APPLICATION region: mode 0 0x04000000, mode 2 0x06000000, mode 6
    // (New 3DS) 0x07C00000 (memory_region_sizes in memory.cpp).
    for (const auto& [mode, commit] : std::vector<std::pair<u32, s32>>{
             {0, 0x04000000}, {2, 0x06000000}, {3, 0x05000000}, {6, 0x07C00000}}) {
        Env env(mode);
        const auto limit = env.kernel->ApplicationResourceLimit();
        CHECK(limit->GetCategory() == ResourceLimitCategory::Application);
        CHECK(limit->GetLimitValue(ResourceLimitType::Commit) == commit);
        CHECK(limit->GetLimitValue(ResourceLimitType::Priority) == 0x18);
        CHECK(limit->GetLimitValue(ResourceLimitType::Thread) == 0x20);
        CHECK(limit->GetLimitValue(ResourceLimitType::Event) == 0x20);
        CHECK(limit->GetLimitValue(ResourceLimitType::Mutex) == 0x20);
        CHECK(limit->GetLimitValue(ResourceLimitType::Semaphore) == 0x8);
        CHECK(limit->GetLimitValue(ResourceLimitType::Timer) == 0x8);
        CHECK(limit->GetLimitValue(ResourceLimitType::SharedMemory) == 0x10);
        CHECK(limit->GetLimitValue(ResourceLimitType::AddressArbiter) == 0x2);
        CHECK(limit->GetLimitValue(ResourceLimitType::CpuTime) == 0);  // PREEMPTION_DISABLED
        for (u32 t = 0; t < static_cast<u32>(ResourceLimitType::Max); ++t) {
            CHECK(limit->GetCurrentValue(static_cast<ResourceLimitType>(t)) == 0);
        }
    }
}

TEST_CASE("resource limit: Reserve and Release always succeed, going past only logs") {
    Env env;
    const auto limit = env.kernel->ApplicationResourceLimit();
    std::vector<std::string> errors;
    SetLogSink([&](const LogRecord& record) {
        if (record.level == LogLevel::Error) {
            errors.emplace_back(record.message);
        }
    });
    CHECK(limit->Reserve(ResourceLimitType::AddressArbiter, 2));  // limit 2: fine
    CHECK(errors.empty());
    CHECK(limit->Reserve(ResourceLimitType::AddressArbiter, 1));  // 3 > 2: logged, still done
    CHECK(limit->GetCurrentValue(ResourceLimitType::AddressArbiter) == 3);
    CHECK(errors.size() == 1);
    CHECK(limit->Release(ResourceLimitType::AddressArbiter, 3));
    CHECK(limit->GetCurrentValue(ResourceLimitType::AddressArbiter) == 0);
    CHECK(errors.size() == 1);
    CHECK(limit->Release(ResourceLimitType::AddressArbiter, 1));  // below 0: logged
    CHECK(limit->GetCurrentValue(ResourceLimitType::AddressArbiter) == -1);
    CHECK(errors.size() == 2);
    SetLogSink({});
    limit->SetCurrentValue(ResourceLimitType::Thread, 5);
    limit->SetLimitValue(ResourceLimitType::Thread, 9);
    CHECK(limit->GetCurrentValue(ResourceLimitType::Thread) == 5);
    CHECK(limit->GetLimitValue(ResourceLimitType::Thread) == 9);
}

TEST_CASE("destroying an object gives its resource limit slot back") {
    Env env;
    const auto limit = env.kernel->ApplicationResourceLimit();
    auto reserve = [&](auto& object, ResourceLimitType type) {
        limit->Reserve(type, 1);
        object->resource_limit = limit;
    };
    {
        auto event = env.kernel->NewEvent(ResetType::OneShot);
        auto mutex = env.kernel->NewMutex(false);
        std::shared_ptr<Semaphore> semaphore;
        REQUIRE(env.kernel->NewSemaphore(&semaphore, 0, 1) == kResultSuccess);
        auto timer = env.kernel->NewTimer(ResetType::OneShot);
        auto arbiter = env.kernel->NewAddressArbiter();
        reserve(event, ResourceLimitType::Event);
        reserve(mutex, ResourceLimitType::Mutex);
        reserve(semaphore, ResourceLimitType::Semaphore);
        reserve(timer, ResourceLimitType::Timer);
        reserve(arbiter, ResourceLimitType::AddressArbiter);
        CHECK(limit->GetCurrentValue(ResourceLimitType::Event) == 1);
        CHECK(limit->GetCurrentValue(ResourceLimitType::Mutex) == 1);
        CHECK(limit->GetCurrentValue(ResourceLimitType::Semaphore) == 1);
        CHECK(limit->GetCurrentValue(ResourceLimitType::Timer) == 1);
        CHECK(limit->GetCurrentValue(ResourceLimitType::AddressArbiter) == 1);
    }
    for (auto type : {ResourceLimitType::Event, ResourceLimitType::Mutex, ResourceLimitType::Semaphore,
                      ResourceLimitType::Timer, ResourceLimitType::AddressArbiter}) {
        CHECK(limit->GetCurrentValue(type) == 0);
    }
}

// ---- the Kernel facade -----------------------------------------------------------------------------

TEST_CASE("the Kernel forwards the timer queue and host work to the scheduler") {
    Env env;
    int ran = 0;
    const u64 id = env.kernel->ScheduleAt(500, [&] { ++ran; });
    CHECK(id != 0);
    CHECK(env.kernel->NowTicks() == 0);
    env.sched.now = 100;
    CHECK(env.kernel->NowTicks() == 100);
    const u64 cancelled = env.kernel->ScheduleAt(600, [&] { ran += 100; });
    CHECK(env.kernel->Cancel(cancelled));
    CHECK_FALSE(env.kernel->Cancel(cancelled));
    env.sched.RunUntil(1000);
    CHECK(ran == 1);

    // Post may be called from any host thread.
    std::thread other([&] { env.kernel->Post([&] { ran += 10; }); });
    other.join();
    REQUIRE(env.sched.posted.size() == 1);
    env.sched.posted[0]();
    CHECK(ran == 11);

    auto thread = env.NewThread(30);
    CHECK(env.kernel->CurrentThread() == nullptr);
    env.sched.current = thread.get();
    CHECK(env.kernel->CurrentThread() == thread.get());
}

TEST_CASE("handles of the current process: AddHandle and GetObject") {
    Env env;
    auto event = env.kernel->NewEvent(ResetType::Sticky, "ev");
    Handle h = 0;
    CHECK(env.kernel->AddHandle(&h, event) == kResultSuccess);
    CHECK(h == 0x00000001);
    CHECK(env.kernel->GetObject<Event>(h) == event);
    CHECK(env.kernel->GetObject<Mutex>(h) == nullptr);
    CHECK(event->Name() == "ev");
    CHECK(std::string(event->TypeName()) == "Event");
}
