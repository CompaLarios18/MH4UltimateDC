// Derived from Azahar src/core/hle/kernel/object.h, handle_table.h, wait_object.h, event.h,
// mutex.h, semaphore.h, timer.h, address_arbiter.h, shared_memory.h, resource_limit.h, thread.h,
// process.h, kernel.h, errors.h and svc.cpp (result code values) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// thread.h, kernel.h and svc.cpp say so, the other sources say GPL-2.0-or-later):
// the object types and their names (HandleType, ResetType, ThreadStatus, ArbitrationType,
// ResourceLimitType, MemoryPermission), the layout of the handle table and the result codes.
// Copyright 2018 Citra Emulator Project (object.h)
// Copyright 2014 Citra Emulator Project (handle_table.h, wait_object.h, event.h, mutex.h,
// semaphore.h, address_arbiter.h, shared_memory.h)
// Copyright 2015 Citra Emulator Project (timer.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (resource_limit.h, process.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.h, kernel.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp)
// Copyright 2017 Citra Emulator Project (errors.h)
// PPSSPP Project (thread.h, kernel.h, second notice: Licensed under GPLv2 or any later version)
//
// Kernel objects and the handle table (N1-05): the kernel's objects and their rules, independent
// of how threads run. The scheduler (N1-06) plugs in through `Scheduler`; the SVCs (N1-07), the
// threads' creation (N1-27) and the services build on this header.
//
// Rules that hold for every class here:
//  - Only the scheduler's host thread touches kernel objects (principle 6 of
//    docs/native/ROADMAP.md). `Kernel::Post` is the exception: any host thread may call it.
//  - Objects are owned through std::shared_ptr and made by the `Kernel::New*` functions (the
//    objects use shared_from_this). Behaviour, result codes and edge cases are Azahar's; where this
//    file departs from it, the comment says so.
//  - A thread that waits is on the waiter list of each object it waits for, and the object is in
//    the thread's `wait_objects`; a wake-up removes both links, so finished waits leave no cycles.
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "mh4n/memory.h"
#include "mh4n/types.h"

namespace mh4n {

class GameClock;  // mh4n/clock.h (N1-06)
class Kernel;
class Thread;
class Process;
class Mutex;
class AddressArbiter;
class ResourceLimit;

using Handle = u32;

// ---- result codes (Azahar src/core/hle/kernel/errors.h, svc.cpp; the values are its comments') ---

inline constexpr Result kResultSuccess = 0;
inline constexpr Result kResultOutOfHandles = 0xD8600413;
inline constexpr Result kResultInvalidHandle = 0xD8E007F7;
inline constexpr Result kResultTimeout = 0x09401BFE;
inline constexpr Result kResultWrongLockingThread = 0xD8E0041F;   // Mutex::Release by another thread
inline constexpr Result kResultInvalidMutexState = 0xD8A007FF;     // released with a zero lock count
inline constexpr Result kResultInvalidCombinationKernel = 0xD90007EE;  // semaphore initial > max
inline constexpr Result kResultOutOfRangeKernel = 0xD8E007FD;      // semaphore release past the max
inline constexpr Result kResultInvalidEnumValueFnd = 0xD8E093ED;   // unknown arbitration type
inline constexpr Result kResultInvalidCombination = 0xE0E01BEE;    // shared memory permissions
inline constexpr Result kResultWrongPermission = 0xD900182E;       // shared memory permissions
inline constexpr Result kResultOutOfMemory = 0xD86007F3;           // see Kernel::NewSharedMemory
static_assert(kResultOutOfHandles == MakeResult(19, 1, 3, 27));
static_assert(kResultInvalidHandle == MakeResult(1015, 1, 7, 27));
static_assert(kResultTimeout == MakeResult(1022, 6, 10, 1));
static_assert(kResultWrongLockingThread == MakeResult(31, 1, 7, 27));
static_assert(kResultInvalidMutexState == MakeResult(1023, 1, 5, 27));
static_assert(kResultInvalidCombinationKernel == MakeResult(1006, 1, 8, 27));
static_assert(kResultOutOfRangeKernel == MakeResult(1021, 1, 7, 27));
static_assert(kResultInvalidEnumValueFnd == MakeResult(1005, 36, 7, 27));
static_assert(kResultInvalidCombination == MakeResult(1006, 6, 7, 28));
static_assert(kResultWrongPermission == MakeResult(46, 6, 8, 27));
static_assert(kResultOutOfMemory == MakeResult(1011, 1, 3, 27));

// ---- plain enumerations --------------------------------------------------------------------------

enum class HandleType : u32 {
    Unknown,
    Event,
    Mutex,
    SharedMemory,
    Thread,
    Process,
    AddressArbiter,
    Semaphore,
    Timer,
    ResourceLimit,
    CodeSet,
    ClientPort,
    ServerPort,
    ClientSession,
    ServerSession,
};

enum class ResetType { OneShot, Sticky, Pulse };

// The SVC-level permission of a shared memory block (not the MemPerm of guest memory). The low
// three bits are MemPerm's; DontCare is bit 28.
enum class MemoryPermission : u32 {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    ReadWrite = Read | Write,
    Execute = 1u << 2,
    ReadExecute = Read | Execute,
    WriteExecute = Write | Execute,
    ReadWriteExecute = Read | Write | Execute,
    DontCare = 1u << 28,
};

enum ThreadPriority : u32 {
    kThreadPrioHighest = 0,
    kThreadPrioUserlandMax = 24,
    kThreadPrioDefault = 48,
    kThreadPrioLowest = 63,
};

enum class ThreadStatus {
    Running,
    Ready,
    WaitArb,       // on an address arbiter
    WaitSleep,     // SleepThread, or a timed wait of the scheduler's own
    WaitIPC,       // for the reply of an IPC request
    WaitSynchAny,  // WaitSynchronization1, or N with wait_all = false
    WaitSynchAll,  // WaitSynchronizationN with wait_all = true
    WaitHleEvent,  // an HLE handler paused the thread
    Dormant,       // created, not yet made ready
    Dead,          // ran to completion, or was terminated
};

// Terminated: the thread was stopped while waiting (only WakeHook sees it).
enum class ThreadWakeupReason { Signal, Timeout, Terminated };

// ---- the scheduler's side (implemented by N1-06; tests use a fake) --------------------------------

class Scheduler {
public:
    virtual ~Scheduler() = default;

    // The wait of `thread` is over and the kernel has set its status to Ready. The scheduler puts
    // it on its ready queue and arranges the return value of the call that blocked it: `result`
    // goes in r0 unless it is empty (a sleep or an IPC wait leaves the registers alone), and
    // `index` >= 0 is the output of WaitSynchronizationN (wait on any); -1 means no output.
    virtual void Wake(const std::shared_ptr<Thread>& thread, std::optional<Result> result,
                      s32 index) = 0;
    // The current priority of `thread` was set (Thread::SetPriority, BoostPriority and so
    // UpdatePriority, i.e. mutex priority inheritance). Called every time, also when the value did
    // not change, because Azahar moves a Ready thread to the back of its queue in that case too.
    // The thread can be in any state (a mutex holder that waits, runs or is ready).
    virtual void OnPriorityChanged(Thread& thread, u32 old_priority) = 0;
    // Azahar's KernelSystem::PrepareReschedule: a mutex was acquired or fully released.
    virtual void PrepareReschedule() = 0;
    // The thread running on the scheduler's host thread (nullptr outside any guest thread).
    virtual Thread* CurrentThread() = 0;

    // The game clock and its timer queue (N1-06's clock.h; ticks are 268,111,856 Hz).
    virtual u64 NowTicks() = 0;
    // Runs `fn` on the scheduler thread when the clock reaches `tick` (at once if it is past).
    // The returned id is never 0.
    virtual u64 ScheduleAt(u64 tick, std::function<void()> fn) = 0;
    // True if the event was still pending and is now cancelled.
    virtual bool Cancel(u64 id) = 0;
    // Hands `fn` to the scheduler thread; callable from any host thread.
    virtual void Post(std::function<void()> fn) = 0;
};

// ---- objects ---------------------------------------------------------------------------------------

class Object : public std::enable_shared_from_this<Object> {
public:
    explicit Object(Kernel& kernel);
    virtual ~Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    // A number for logs, unique per kernel (Azahar's GetObjectId; starts at 0).
    u32 ObjectId() const { return object_id_; }
    virtual HandleType GetHandleType() const = 0;
    virtual const char* TypeName() const = 0;
    const std::string& Name() const { return name_; }
    void SetName(std::string name) { name_ = std::move(name); }
    // Event, Mutex, Thread, Semaphore, Timer, ServerPort and ServerSession (not Process, as in
    // Azahar).
    bool IsWaitable() const;

protected:
    Kernel& kernel_;

private:
    u32 object_id_;
    std::string name_;
};

// The object behind a handle if it is a T (or, for WaitObject, any waitable object).
template <class T>
std::shared_ptr<T> ObjectCast(std::shared_ptr<Object> object) {
    if (object == nullptr) {
        return nullptr;
    }
    if constexpr (std::is_same_v<T, Object>) {
        return object;
    } else if constexpr (requires { T::kHandleType; }) {
        return object->GetHandleType() == T::kHandleType ? std::static_pointer_cast<T>(object)
                                                          : nullptr;
    } else {
        return object->IsWaitable() ? std::static_pointer_cast<T>(object) : nullptr;  // WaitObject
    }
}

// ---- the handle table (one per process) ------------------------------------------------------------

inline constexpr Handle kHandleCurrentThread = 0xFFFF8000;
inline constexpr Handle kHandleCurrentProcess = 0xFFFF8001;

// 4096 slots; a handle is generation | slot << 15. One generation counter for the table, from 1,
// wrapping from 0x8000 back to 1; the last slot freed is the first reused. This is Azahar's scheme,
// so handle values match the reference when calls come in the same order. The two pseudo-handles
// resolve through the Kernel; they are not in the table (IsValid and Close reject them).
// Departure: a handle whose slot field is 4096 or more is invalid; Azahar truncates the slot to 16
// bits, so a handle with garbage in the top bits could alias a real one.
class HandleTable {
public:
    static constexpr std::size_t kMaxCount = 4096;

    explicit HandleTable(Kernel& kernel);
    ~HandleTable();
    HandleTable(const HandleTable&) = delete;
    HandleTable& operator=(const HandleTable&) = delete;

    // kResultOutOfHandles when the table is full.
    Result Create(Handle* out, std::shared_ptr<Object> object);
    // kResultInvalidHandle for a handle that is not valid.
    Result Duplicate(Handle* out, Handle handle);
    Result Close(Handle handle);
    bool IsValid(Handle handle) const;
    std::shared_ptr<Object> GetGeneric(Handle handle) const;
    template <class T>
    std::shared_ptr<T> Get(Handle handle) const {
        return ObjectCast<T>(GetGeneric(handle));
    }
    // Closes every handle and resets the free list (the generation counter keeps counting).
    void Clear();

private:
    std::array<std::shared_ptr<Object>, kMaxCount> objects_;
    // The generation the handle was created with; for an empty slot, the next free slot.
    std::array<u16, kMaxCount> generations_;
    u16 next_generation_ = 1;
    u16 next_free_slot_ = 0;
    Kernel& kernel_;
};

// ---- waiting ---------------------------------------------------------------------------------------

class WaitObject : public Object {
public:
    using Object::Object;
    // True if `thread` has to wait until the object is available.
    virtual bool ShouldWait(const Thread* thread) const = 0;
    // Takes the object for `thread` (it must not have to wait).
    virtual void Acquire(Thread* thread) = 0;
    virtual void AddWaitingThread(std::shared_ptr<Thread> thread);
    virtual void RemoveWaitingThread(Thread* thread);
    // Wakes the waiting threads that can run, best priority first (the first one in the list among
    // equals), acquiring the object for each; a wait on all also acquires the other objects.
    virtual void WakeupAllWaitingThreads();
    // The best-priority waiting thread that can be woken now, or nullptr.
    std::shared_ptr<Thread> GetHighestPriorityReadyThread() const;
    const std::vector<std::shared_ptr<Thread>>& GetWaitingThreads() const { return waiting_; }
    // Called at the end of every WakeupAllWaitingThreads (the DSP's events use it).
    void SetHleNotifier(std::function<void()> callback) { hle_notifier_ = std::move(callback); }

private:
    std::vector<std::shared_ptr<Thread>> waiting_;
    std::function<void()> hle_notifier_;
};

class Event final : public WaitObject {
public:
    static constexpr HandleType kHandleType = HandleType::Event;
    Event(Kernel& kernel, ResetType reset_type);
    ~Event() override;
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "Event"; }
    ResetType GetResetType() const { return reset_type_; }
    bool IsSignaled() const { return signaled_; }

    bool ShouldWait(const Thread* thread) const override;
    void Acquire(Thread* thread) override;
    // Pulse events clear themselves after waking the waiters (also when there are none).
    void WakeupAllWaitingThreads() override;
    void Signal();
    void Clear();

    std::shared_ptr<ResourceLimit> resource_limit;  // released when the event is destroyed

private:
    ResetType reset_type_;
    bool signaled_ = false;
};

class Mutex final : public WaitObject {
public:
    static constexpr HandleType kHandleType = HandleType::Mutex;
    explicit Mutex(Kernel& kernel);
    ~Mutex() override;
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "Mutex"; }

    int LockCount() const { return lock_count_; }
    // The best priority among the waiters (the holder runs at least that well), valid while held.
    u32 Priority() const { return priority_; }
    Thread* HoldingThread() const { return holding_thread_; }

    // Free, or held by `thread` already (recursion).
    bool ShouldWait(const Thread* thread) const override;
    // Takes it (the first time: priority = the thread's current priority, the holder's priority is
    // recomputed) or increments the recursion count.
    void Acquire(Thread* thread) override;
    // kResultWrongLockingThread unless `thread` holds it; kResultInvalidMutexState if the count is
    // 0. Only the last release frees it, wakes the waiters and recomputes the holder's priority.
    Result Release(Thread* thread);
    // Priority inheritance: the best priority among the waiters. No transitive propagation, as in
    // Azahar.
    void UpdatePriority();
    void AddWaitingThread(std::shared_ptr<Thread> thread) override;
    void RemoveWaitingThread(Thread* thread) override;

    std::shared_ptr<ResourceLimit> resource_limit;

private:
    friend void ReleaseThreadMutexes(Thread* thread);
    friend class Thread;
    int lock_count_ = 0;
    u32 priority_ = kThreadPrioLowest;
    // Not owning: the holder owns the mutex through Thread::held_mutexes (no cycle).
    Thread* holding_thread_ = nullptr;
};

// Releases every mutex the thread holds, whatever the recursion count (a thread that ends).
void ReleaseThreadMutexes(Thread* thread);

class Semaphore final : public WaitObject {
public:
    static constexpr HandleType kHandleType = HandleType::Semaphore;
    Semaphore(Kernel& kernel, s32 initial_count, s32 max_count);
    ~Semaphore() override;
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "Semaphore"; }
    s32 MaxCount() const { return max_count_; }
    s32 AvailableCount() const { return available_count_; }

    bool ShouldWait(const Thread* thread) const override;
    void Acquire(Thread* thread) override;
    // *out_count = the count before the call. kResultOutOfRangeKernel (nothing changes) if the
    // new count would pass the maximum.
    Result Release(s32* out_count, s32 release_count);

    std::shared_ptr<ResourceLimit> resource_limit;

private:
    s32 max_count_;
    s32 available_count_;
};

class Timer final : public WaitObject {
public:
    static constexpr HandleType kHandleType = HandleType::Timer;
    Timer(Kernel& kernel, ResetType reset_type);
    ~Timer() override;
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "Timer"; }
    ResetType GetResetType() const { return reset_type_; }
    s64 InitialDelay() const { return initial_delay_; }
    s64 IntervalDelay() const { return interval_delay_; }
    bool IsArmed() const { return event_ != 0; }
    bool IsSignaled() const { return signaled_; }

    bool ShouldWait(const Thread* thread) const override;
    void Acquire(Thread* thread) override;
    void WakeupAllWaitingThreads() override;  // Pulse timers clear themselves afterwards

    // Cancels any pending shot, then arms the timer: the first shot after `initial_ns` (a delay of
    // 0 signals at once), then one every `interval_ns` (0: only once). Both are nanoseconds on the
    // game clock; negative values count as 0.
    void Set(s64 initial_ns, s64 interval_ns);
    void Cancel();
    void Clear();
    // Signals the timer and, for a periodic one, schedules the next shot one interval after this
    // one's deadline (so the period does not drift). Called from the scheduler's timer queue.
    void Fire();

    std::shared_ptr<ResourceLimit> resource_limit;

private:
    void Schedule(u64 deadline);

    ResetType reset_type_;
    s64 initial_delay_ = 0;
    s64 interval_delay_ = 0;
    bool signaled_ = false;
    u64 event_ = 0;     // the pending timer-queue event, 0 if none
    u64 deadline_ = 0;  // when that event is due
};

enum class ArbitrationType : u32 {
    Signal,
    WaitIfLessThan,
    DecrementAndWaitIfLessThan,
    WaitIfLessThanWithTimeout,
    DecrementAndWaitIfLessThanWithTimeout,
};

class AddressArbiter final : public Object {
public:
    static constexpr HandleType kHandleType = HandleType::AddressArbiter;
    explicit AddressArbiter(Kernel& kernel);
    ~AddressArbiter() override;
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "Arbiter"; }

    // svcArbitrateAddress for the calling `thread`. Signal wakes `value` threads waiting on
    // `address`, the best priority first (the earliest among equals), or all of them if value < 0.
    // The wait types compare the signed word at `address` with `value` and, if it is less, make
    // the thread wait (the Decrement types first store word - 1); an unreadable address reads as 0
    // as in Azahar. The timeout types return kResultTimeout even when they did not wait, and a
    // thread woken by a signal keeps that result, as in Azahar; a timeout of -1 never expires.
    // An unknown type returns kResultInvalidEnumValueFnd. Azahar's extra 1614 ticks charged to a
    // Signal that wakes nobody are not modelled (the native clock is the host's). A thread that
    // waits has status WaitArb when this returns; the scheduler switches away from it when the
    // SVC returns. A timeout or the end of the thread (Thread::Stop) takes it off the list.
    Result ArbitrateAddress(std::shared_ptr<Thread> thread, ArbitrationType type, u32 address,
                            s32 value, s64 timeout_ns);
    const std::vector<std::shared_ptr<Thread>>& WaitingThreads() const { return waiting_; }

    std::shared_ptr<ResourceLimit> resource_limit;

private:
    void WaitThread(std::shared_ptr<Thread> thread, u32 address, Result wake_result,
                    std::optional<s64> timeout_ns);
    u64 ResumeAllThreads(u32 address);
    bool ResumeHighestPriorityThread(u32 address);
    void RemoveThread(Thread* thread);

    std::vector<std::shared_ptr<Thread>> waiting_;
    std::vector<Result> wake_results_;  // parallel to waiting_: what the thread's call returns
};

class SharedMemory final : public Object {
public:
    static constexpr HandleType kHandleType = HandleType::SharedMemory;
    SharedMemory(Kernel& kernel);
    ~SharedMemory() override;
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "SharedMemory"; }

    u32 Size() const { return size_; }
    // For a block created at address 0: its FCRAM offset (physical address - kFcramPaddr).
    u32 LinearHeapPhysicalOffset() const { return linear_heap_phys_offset_; }
    bool IsMapped() const { return mapped_; }
    u32 MappedAddress() const { return mapped_at_; }

    // svcMapMemoryBlock, with Azahar's checks in its order. Results: kResultInvalidCombination,
    // kResultWrongPermission, kResultInvalidAddress (outside [0x08000000, 0x14000000)),
    // kResultInvalidAddressState (the target range is not free), else the result of the mapping.
    // A block created at address 0 maps at kLinearHeapVaddr + its FCRAM offset when `address` is 0.
    // The block's contents move into the guest range on the first map, which becomes its home.
    // Mapping a block at a second address while it is mapped, or an app-created block anywhere,
    // would alias host pages: unsupported, logged, and answered with kResultInvalidAddressState.
    Result Map(Process& target, u32 address, MemoryPermission permissions,
               MemoryPermission other_permissions);
    // The contents move back to the host when it is unmapped. kResultInvalidAddressState unless
    // `address` is where the block is mapped (Azahar unmaps whatever range it is given).
    Result Unmap(Process& target, u32 address);
    // Where the contents are now: the guest range while mapped (or, for a block the app created
    // over its own memory, always), else host memory. Valid until the next Map or Unmap.
    u8* GetPointer(u32 offset = 0);

private:
    friend class Kernel;
    struct Piece {
        u32 offset;  // FCRAM offset
        u32 size;
    };
    FcramRegion region_ = FcramRegion::Base;
    u32 size_ = 0;
    u32 linear_heap_phys_offset_ = 0;
    // An app-created block: where it lives in its owner. A kernel-created one: kHeapVaddr + offset.
    // A block created at address 0: 0.
    u32 base_address_ = 0;
    MemoryPermission permissions_ = MemoryPermission::None;
    MemoryPermission other_permissions_ = MemoryPermission::None;
    std::weak_ptr<Process> owner_process_;
    std::vector<Piece> pieces_;      // FCRAM held by a kernel-created or address-0 block
    std::vector<u8> host_data_;      // its contents while it is not mapped
    bool mapped_ = false;
    u32 mapped_at_ = 0;
};

class Thread final : public WaitObject {
public:
    static constexpr HandleType kHandleType = HandleType::Thread;
    // A bare thread object: Dormant, nominal = current = `priority`. N1-27 adds the stack, the TLS
    // and the registers; N1-06 queues it.
    Thread(Kernel& kernel, u32 thread_id, u32 priority, s32 processor_id);
    ~Thread() override;
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "Thread"; }

    // Signalled when the thread has ended.
    bool ShouldWait(const Thread* thread) const override;
    void Acquire(Thread* thread) override;

    u32 GetPriority() const { return current_priority; }
    // Sets nominal and current priority.
    void SetPriority(u32 priority);
    // current = the best of nominal and the priorities of the mutexes held (priority inheritance).
    void UpdatePriority();
    void BoostPriority(u32 priority);

    // Starts a wait on `objects` (the caller has checked that it has to wait): sets the status to
    // WaitSynchAny or WaitSynchAll, joins each object's waiter list and fills `wait_objects`.
    void BeginSynchWait(std::vector<std::shared_ptr<WaitObject>> objects, bool wait_all);
    // After `ns` nanoseconds of game time OnWaitTimeout runs; -1 means never. A negative value
    // other than -1 expires at once, as in Azahar. Replaces any earlier timeout.
    void ArmTimeout(s64 ns);
    void CancelTimeout();
    // The timeout expired (called from the timer queue). A synch, arbiter or HLE wait ends with
    // kResultTimeout; a sleep or IPC wait ends without a result. Other states are ignored.
    void OnWaitTimeout();
    // Ends a wait: cancels the timeout, drops `wake_hook`, sets the status to Ready and calls
    // Scheduler::Wake. Ignored (logged) if the thread is already Ready, Running or Dead.
    void ResumeFromWait(std::optional<Result> result, s32 index = -1);
    // The index of the last entry of `wait_objects` that is `object`, as Azahar's
    // GetWaitObjectIndex (a handle listed twice reports its last position).
    s32 GetWaitObjectIndex(const WaitObject* object) const;
    bool IsSleepingOnWaitAll() const { return status == ThreadStatus::WaitSynchAll; }
    // The thread ends: Dead, waiters of the thread object woken, left every wait list, mutexes
    // released. The scheduler has already taken it off its queues; resource and TLS release are
    // N1-27's.
    void Stop();

    u32 thread_id;
    ThreadStatus status = ThreadStatus::Dormant;
    u32 nominal_priority;  // as set by the application
    u32 current_priority;  // after priority inheritance
    s32 processor_id;
    u32 entry_point = 0;
    u32 stack_top = 0;
    u32 tls_address = 0;
    u32 wait_address = 0;  // the arbitration address while WaitArb
    u64 last_running_ticks = 0;
    std::weak_ptr<Process> owner_process;
    std::set<std::shared_ptr<Mutex>> held_mutexes;
    std::set<std::shared_ptr<Mutex>> pending_mutexes;  // mutexes it waits for
    std::vector<std::shared_ptr<WaitObject>> wait_objects;

    // Lets the code that blocked the thread choose its wake-up (Azahar's WakeupCallback; the IPC
    // layer uses it, N1-10). Called before the thread leaves the wait lists with the object that
    // signalled (nullptr for a timeout); it may change the result and the index.
    using WakeHook = std::function<void(ThreadWakeupReason reason, WaitObject* object,
                                        std::optional<Result>& result, s32& index)>;
    WakeHook wake_hook;
    // For the scheduler's use (its fiber, its queue position).
    std::shared_ptr<void> scheduler_data;

private:
    u64 timeout_event_ = 0;  // the pending timer-queue event of ArmTimeout, 0 if none
};

// ---- resource limits and the process ---------------------------------------------------------------

enum class ResourceLimitCategory : u8 { Application = 0, SysApplet = 1, LibApplet = 2, Other = 3 };

enum class ResourceLimitType : u32 {
    Priority = 0,
    Commit = 1,
    Thread = 2,
    Event = 3,
    Mutex = 4,
    Semaphore = 5,
    Timer = 6,
    SharedMemory = 7,
    AddressArbiter = 8,
    CpuTime = 9,
    Max = 10,
};

class ResourceLimit final : public Object {
public:
    static constexpr HandleType kHandleType = HandleType::ResourceLimit;
    ResourceLimit(Kernel& kernel, ResourceLimitCategory category);
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "ResourceLimit"; }
    ResourceLimitCategory GetCategory() const { return category_; }

    s32 GetCurrentValue(ResourceLimitType type) const;
    s32 GetLimitValue(ResourceLimitType type) const;
    void SetCurrentValue(ResourceLimitType type, s32 value);
    void SetLimitValue(ResourceLimitType type, s32 value);
    // As in Azahar both always succeed: going past the limit (or below 0) only logs an error.
    bool Reserve(ResourceLimitType type, s32 amount);
    bool Release(ResourceLimitType type, s32 amount);

private:
    ResourceLimitCategory category_;
    std::array<s32, static_cast<std::size_t>(ResourceLimitType::Max)> limit_values_{};
    std::array<s32, static_cast<std::size_t>(ResourceLimitType::Max)> current_values_{};
};

enum class ProcessStatus { Created, Running, Exited };

// The application's process id in Azahar: the first process is made with ++next_process_id from 10.
inline constexpr u32 kApplicationProcessId = 11;

class Process final : public Object {
public:
    static constexpr HandleType kHandleType = HandleType::Process;
    Process(Kernel& kernel, u32 process_id);
    ~Process() override;
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "Process"; }

    HandleTable handle_table;
    std::shared_ptr<ResourceLimit> resource_limit;
    u32 process_id;
    ProcessStatus status = ProcessStatus::Created;
    FcramRegion memory_region = FcramRegion::Application;  // flags.memory_region
    bool shared_device_mem = false;
    u16 kernel_version = 0;
    u8 ideal_processor = 0;
    u32 memory_used = 0;  // bytes; shared memory blocks at address 0 add to it
    u64 creation_time_ticks = 0;
};

// ---- the facade ------------------------------------------------------------------------------------

class Kernel {
public:
    // `memory_mode` is the system memory mode (FcramRegions::ValidMemoryMode); the three FCRAM
    // regions and the APPLICATION resource limit follow from it.
    Kernel(GuestMemory& memory, u32 memory_mode);
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // N1-06 plugs in its scheduler and clock before anything runs.
    void SetScheduler(Scheduler* scheduler) { scheduler_ = scheduler; }
    void SetClock(GameClock* clock) { clock_ = clock; }

    GuestMemory& Memory() { return memory_; }
    FcramRegions& Regions() { return regions_; }
    GameClock& Clock();  // asserts that SetClock was called
    Scheduler& GetScheduler();

    // The scheduler's timer queue and host work (declared here; the scheduler implements them).
    u64 NowTicks() { return GetScheduler().NowTicks(); }
    u64 ScheduleAt(u64 tick, std::function<void()> fn) {
        return GetScheduler().ScheduleAt(tick, std::move(fn));
    }
    bool Cancel(u64 id) { return GetScheduler().Cancel(id); }
    void Post(std::function<void()> fn) { GetScheduler().Post(std::move(fn)); }  // any host thread

    Thread* CurrentThread() { return GetScheduler().CurrentThread(); }
    std::shared_ptr<Process> CurrentProcess() const { return process_; }

    // ---- making objects (not named Create*: windows.h defines CreateEvent and friends) ----
    std::shared_ptr<Event> NewEvent(ResetType reset_type, std::string name = {});
    // initial_locked: the current thread takes it.
    std::shared_ptr<Mutex> NewMutex(bool initial_locked, std::string name = {});
    // kResultInvalidCombinationKernel unless initial_count <= max_count.
    Result NewSemaphore(std::shared_ptr<Semaphore>* out, s32 initial_count, s32 max_count,
                        std::string name = {});
    std::shared_ptr<Timer> NewTimer(ResetType reset_type, std::string name = {});
    std::shared_ptr<AddressArbiter> NewAddressArbiter(std::string name = {});
    // The block svcCreateMemoryBlock makes. address != 0: over the owner's memory, which must be
    // Private and ReadWrite and becomes Locked (the result of the state change otherwise). address
    // 0: `size` bytes taken from `region` with FcramRegions::LinearAllocate (first fit); Azahar
    // asserts when the region is full, this returns kResultOutOfMemory. The caller reserves the
    // resource limit's SharedMemory slot first (the SVC does); the destructor releases it.
    Result NewSharedMemory(std::shared_ptr<SharedMemory>* out, std::shared_ptr<Process> owner,
                           u32 size, MemoryPermission permissions,
                           MemoryPermission other_permissions, u32 address, FcramRegion region,
                           std::string name = {});
    // A block the system makes (HID, GSP, the shared font): `size` bytes held with HeapAllocate from
    // the SYSTEM region, contents in host memory until the app maps it, base address
    // kHeapVaddr + offset. nullptr if the region is full.
    std::shared_ptr<SharedMemory> NewSharedMemoryForApplet(u32 offset, u32 size,
                                                           MemoryPermission permissions,
                                                           MemoryPermission other_permissions,
                                                           std::string name = {});
    // A bare thread with the next thread id (from 1, never reused).
    std::shared_ptr<Thread> NewThread(std::string name, u32 priority, s32 processor_id,
                                      std::shared_ptr<Process> owner = nullptr);
    // The process: id kApplicationProcessId for the first, then the next numbers. It becomes the
    // current process, with the APPLICATION resource limit.
    std::shared_ptr<Process> NewProcess(std::string name);
    std::shared_ptr<ResourceLimit> ApplicationResourceLimit() const { return application_limit_; }

    // ---- handles of the current process ----
    Result AddHandle(Handle* out, std::shared_ptr<Object> object);
    template <class T>
    std::shared_ptr<T> GetObject(Handle handle) const {
        return process_ ? process_->handle_table.Get<T>(handle) : nullptr;
    }

    u32 GenerateObjectId() { return next_object_id_++; }

private:
    GuestMemory& memory_;
    FcramRegions regions_;
    Scheduler* scheduler_ = nullptr;
    GameClock* clock_ = nullptr;
    u32 next_object_id_ = 0;
    u32 next_thread_id_ = 1;
    u32 next_process_id_ = 10;
    std::shared_ptr<ResourceLimit> application_limit_;
    std::shared_ptr<Process> process_;
};

} // namespace mh4n
