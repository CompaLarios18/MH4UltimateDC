// Derived from Azahar src/core/hle/kernel/kernel.cpp, process.cpp, resource_limit.cpp, mutex.cpp,
// semaphore.cpp, shared_memory.cpp, event.cpp, timer.cpp and address_arbiter.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: kernel.cpp says so, the other sources say GPL-2.0-or-later): the object
// factories, the process numbering and the APPLICATION resource
// limit values.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (kernel.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp,
// resource_limit.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (mutex.cpp, semaphore.cpp,
// shared_memory.cpp, event.cpp, address_arbiter.cpp)
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (timer.cpp)
#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

// ---- ResourceLimit -------------------------------------------------------------------------------

ResourceLimit::ResourceLimit(Kernel& kernel, ResourceLimitCategory category)
    : Object(kernel), category_(category) {}

s32 ResourceLimit::GetCurrentValue(ResourceLimitType type) const {
    return current_values_[static_cast<std::size_t>(type)];
}

s32 ResourceLimit::GetLimitValue(ResourceLimitType type) const {
    return limit_values_[static_cast<std::size_t>(type)];
}

void ResourceLimit::SetCurrentValue(ResourceLimitType type, s32 value) {
    current_values_[static_cast<std::size_t>(type)] = value;
}

void ResourceLimit::SetLimitValue(ResourceLimitType type, s32 value) {
    limit_values_[static_cast<std::size_t>(type)] = value;
}

bool ResourceLimit::Reserve(ResourceLimitType type, s32 amount) {
    const auto index = static_cast<std::size_t>(type);
    const s32 limit = limit_values_[index];
    const s32 new_value = current_values_[index] + amount;
    // Azahar logs and goes on ("fix all resource limit bugs and return an error" is its TODO).
    if (new_value > limit) {
        MH4N_LOG(Error, "Kernel", "New value {} exceeds limit {} for resource type {}", new_value,
                 limit, static_cast<u32>(type));
    }
    current_values_[index] = new_value;
    return true;
}

bool ResourceLimit::Release(ResourceLimitType type, s32 amount) {
    const auto index = static_cast<std::size_t>(type);
    const s32 value = current_values_[index];
    if (amount > value) {
        MH4N_LOG(Error, "Kernel", "Amount {} exceeds current value {} for resource type {}",
                 amount, value, static_cast<u32>(type));
    }
    current_values_[index] = value - amount;
    return true;
}

// ---- Process -------------------------------------------------------------------------------------

Process::Process(Kernel& kernel, u32 process_id_)
    : Object(kernel), handle_table(kernel), process_id(process_id_) {}

Process::~Process() = default;

// ---- Kernel --------------------------------------------------------------------------------------

Kernel::Kernel(GuestMemory& memory, u32 memory_mode) : memory_(memory), regions_(memory_mode) {
    // The APPLICATION resource limit (Azahar's ResourceLimitList). PM makes the Commit value match
    // the size of the APPLICATION region.
    application_limit_ = std::make_shared<ResourceLimit>(*this, ResourceLimitCategory::Application);
    application_limit_->SetName("Applications");
    application_limit_->SetLimitValue(ResourceLimitType::Priority, 0x18);
    application_limit_->SetLimitValue(ResourceLimitType::Commit,
                                      static_cast<s32>(regions_.Size(FcramRegion::Application)));
    application_limit_->SetLimitValue(ResourceLimitType::Thread, 0x20);
    application_limit_->SetLimitValue(ResourceLimitType::Event, 0x20);
    application_limit_->SetLimitValue(ResourceLimitType::Mutex, 0x20);
    application_limit_->SetLimitValue(ResourceLimitType::Semaphore, 0x8);
    application_limit_->SetLimitValue(ResourceLimitType::Timer, 0x8);
    application_limit_->SetLimitValue(ResourceLimitType::SharedMemory, 0x10);
    application_limit_->SetLimitValue(ResourceLimitType::AddressArbiter, 0x2);
    application_limit_->SetLimitValue(ResourceLimitType::CpuTime, 0);  // PREEMPTION_DISABLED
}

Kernel::~Kernel() {
    // The handles go first (a process may hold a handle to itself): shared memory gives its FCRAM
    // back to regions_ when it is destroyed.
    if (process_) {
        process_->handle_table.Clear();
    }
    process_.reset();
}

GameClock& Kernel::Clock() {
    MH4N_ASSERT(clock_ != nullptr, "Kernel::Clock: no clock was set");
    return *clock_;
}

Scheduler& Kernel::GetScheduler() {
    MH4N_ASSERT(scheduler_ != nullptr, "Kernel: no scheduler was set");
    return *scheduler_;
}

std::shared_ptr<Event> Kernel::NewEvent(ResetType reset_type, std::string name) {
    auto event = std::make_shared<Event>(*this, reset_type);
    event->SetName(std::move(name));
    return event;
}

std::shared_ptr<Mutex> Kernel::NewMutex(bool initial_locked, std::string name) {
    auto mutex = std::make_shared<Mutex>(*this);
    mutex->SetName(std::move(name));
    // Taken by the current thread if it starts locked.
    if (initial_locked) {
        Thread* thread = CurrentThread();
        MH4N_ASSERT(thread != nullptr, "NewMutex(initial_locked) without a current thread");
        mutex->Acquire(thread);
    }
    return mutex;
}

Result Kernel::NewSemaphore(std::shared_ptr<Semaphore>* out, s32 initial_count, s32 max_count,
                            std::string name) {
    if (initial_count > max_count) {
        return kResultInvalidCombinationKernel;
    }
    *out = std::make_shared<Semaphore>(*this, initial_count, max_count);
    (*out)->SetName(std::move(name));
    return kResultSuccess;
}

std::shared_ptr<Timer> Kernel::NewTimer(ResetType reset_type, std::string name) {
    auto timer = std::make_shared<Timer>(*this, reset_type);
    timer->SetName(std::move(name));
    return timer;
}

std::shared_ptr<AddressArbiter> Kernel::NewAddressArbiter(std::string name) {
    auto arbiter = std::make_shared<AddressArbiter>(*this);
    arbiter->SetName(std::move(name));
    return arbiter;
}

Result Kernel::NewSharedMemory(std::shared_ptr<SharedMemory>* out, std::shared_ptr<Process> owner,
                               u32 size, MemoryPermission permissions,
                               MemoryPermission other_permissions, u32 address, FcramRegion region,
                               std::string name) {
    auto shared_memory = std::make_shared<SharedMemory>(*this);
    shared_memory->owner_process_ = owner;
    shared_memory->SetName(std::move(name));
    shared_memory->size_ = size;
    shared_memory->region_ = region;
    shared_memory->permissions_ = permissions;
    shared_memory->other_permissions_ = other_permissions;

    if (address == 0) {
        // A block from FCRAM, linearly (first fit from the bottom of the region).
        const auto offset = regions_.LinearAllocate(region, size);
        if (!offset) {
            MH4N_LOG(Error, "Kernel", "Not enough space in region to allocate shared memory!");
            return kResultOutOfMemory;  // Azahar asserts here
        }
        shared_memory->pieces_ = {{*offset, size}};
        shared_memory->host_data_.assign(size, 0);
        shared_memory->linear_heap_phys_offset_ = *offset;
        if (owner != nullptr) {
            owner->memory_used += size;
        }
    } else {
        MH4N_ASSERT(owner != nullptr, "a shared memory block over memory needs an owner");
        // The memory is already there, mapped in the owner: lock it.
        const Result result =
            memory_.ChangeState(address, size, MemState::Private, MemPerm::ReadWrite,
                                MemState::Locked, static_cast<MemPerm>(static_cast<u32>(permissions) & 7u));
        if (IsError(result)) {
            return result;
        }
    }
    shared_memory->base_address_ = address;
    *out = std::move(shared_memory);
    return kResultSuccess;
}

std::shared_ptr<SharedMemory> Kernel::NewSharedMemoryForApplet(u32 offset, u32 size,
                                                               MemoryPermission permissions,
                                                               MemoryPermission other_permissions,
                                                               std::string name) {
    // From the SYSTEM region, heap style: from the top, possibly in pieces.
    const std::vector<FcramRange> ranges = regions_.HeapAllocate(FcramRegion::System, size);
    if (ranges.empty()) {
        MH4N_LOG(Error, "Kernel", "Not enough space in region to allocate shared memory!");
        return nullptr;
    }
    auto shared_memory = std::make_shared<SharedMemory>(*this);
    shared_memory->SetName(std::move(name));
    shared_memory->size_ = size;
    shared_memory->region_ = FcramRegion::System;
    shared_memory->permissions_ = permissions;
    shared_memory->other_permissions_ = other_permissions;
    for (const FcramRange& range : ranges) {
        shared_memory->pieces_.push_back({range.offset, range.size});
    }
    shared_memory->host_data_.assign(size, 0);
    shared_memory->base_address_ = kHeapVaddr + offset;
    return shared_memory;
}

std::shared_ptr<Thread> Kernel::NewThread(std::string name, u32 priority, s32 processor_id,
                                          std::shared_ptr<Process> owner) {
    auto thread = std::make_shared<Thread>(*this, next_thread_id_++, priority, processor_id);
    thread->SetName(std::move(name));
    thread->owner_process = owner ? owner : process_;
    return thread;
}

std::shared_ptr<Process> Kernel::NewProcess(std::string name) {
    auto process = std::make_shared<Process>(*this, ++next_process_id_);
    process->SetName(std::move(name));
    process->resource_limit = application_limit_;
    process->creation_time_ticks = scheduler_ ? scheduler_->NowTicks() : 0;
    process_ = process;
    return process;
}

Result Kernel::AddHandle(Handle* out, std::shared_ptr<Object> object) {
    MH4N_ASSERT(process_ != nullptr, "Kernel::AddHandle without a process");
    return process_->handle_table.Create(out, std::move(object));
}

} // namespace mh4n
