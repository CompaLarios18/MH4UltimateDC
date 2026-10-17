// Derived from Azahar src/core/hle/kernel/address_arbiter.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the five arbitration types, the
// wake-up order and the results.
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project
#include <algorithm>

#include "mh4n/assert.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {

AddressArbiter::AddressArbiter(Kernel& kernel) : Object(kernel) {}

AddressArbiter::~AddressArbiter() {
    if (resource_limit) {
        resource_limit->Release(ResourceLimitType::AddressArbiter, 1);
    }
}

void AddressArbiter::WaitThread(std::shared_ptr<Thread> thread, u32 address, Result wake_result,
                                std::optional<s64> timeout_ns) {
    thread->wait_address = address;
    thread->status = ThreadStatus::WaitArb;
    // A timeout or the end of the thread takes it off our list (a signal does it itself).
    Thread* raw = thread.get();
    const std::weak_ptr<AddressArbiter> weak =
        std::static_pointer_cast<AddressArbiter>(shared_from_this());
    thread->wake_hook = [weak, raw](ThreadWakeupReason reason, WaitObject*,
                                    std::optional<Result>&, s32&) {
        if (reason != ThreadWakeupReason::Signal) {
            if (auto arbiter = weak.lock()) {
                arbiter->RemoveThread(raw);
            }
        }
    };
    if (timeout_ns) {
        thread->ArmTimeout(*timeout_ns);
    }
    waiting_.push_back(std::move(thread));
    wake_results_.push_back(wake_result);
}

void AddressArbiter::RemoveThread(Thread* thread) {
    for (std::size_t i = 0; i < waiting_.size(); ++i) {
        if (waiting_[i].get() == thread) {
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
            wake_results_.erase(wake_results_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

u64 AddressArbiter::ResumeAllThreads(u32 address) {
    // The threads waiting on this address, in the order they arrived.
    std::vector<std::pair<std::shared_ptr<Thread>, Result>> woken;
    for (std::size_t i = 0; i < waiting_.size();) {
        MH4N_ASSERT(waiting_[i]->status == ThreadStatus::WaitArb,
                    "Inconsistent AddressArbiter state");
        if (waiting_[i]->wait_address == address) {
            woken.emplace_back(std::move(waiting_[i]), wake_results_[i]);
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
            wake_results_.erase(wake_results_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (auto& [thread, result] : woken) {
        thread->ResumeFromWait(result);
    }
    return woken.size();
}

bool AddressArbiter::ResumeHighestPriorityThread(u32 address) {
    // The real kernel picks the first thread in the list when several have the best priority
    // (lower value = better), which is what the strict comparison does.
    std::size_t best = waiting_.size();
    for (std::size_t i = 0; i < waiting_.size(); ++i) {
        MH4N_ASSERT(waiting_[i]->status == ThreadStatus::WaitArb,
                    "Inconsistent AddressArbiter state");
        if (waiting_[i]->wait_address != address) {
            continue;
        }
        if (best == waiting_.size() || waiting_[i]->current_priority < waiting_[best]->current_priority) {
            best = i;
        }
    }
    if (best == waiting_.size()) {
        return false;
    }
    const std::shared_ptr<Thread> thread = std::move(waiting_[best]);
    const Result result = wake_results_[best];
    waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(best));
    wake_results_.erase(wake_results_.begin() + static_cast<std::ptrdiff_t>(best));
    thread->ResumeFromWait(result);
    return true;
}

Result AddressArbiter::ArbitrateAddress(std::shared_ptr<Thread> thread, ArbitrationType type,
                                        u32 address, s32 value, s64 timeout_ns) {
    GuestMemory& memory = kernel_.Memory();
    // Azahar's Read32 of an unmapped address logs and gives 0.
    auto read_word = [&]() -> s32 {
        const auto word = memory.Read<s32>(address);
        if (!word) {
            MH4N_LOG(Error, "Kernel", "arbiter: read of unmapped address 0x{:08X}", address);
        }
        return word.value_or(0);
    };

    switch (type) {
    case ArbitrationType::Signal:
        if (value < 0) {
            ResumeAllThreads(address);
        } else {
            for (s32 i = 0; i < value; i++) {
                ResumeHighestPriorityThread(address);
            }
        }
        break;
    case ArbitrationType::WaitIfLessThan:
        if (read_word() < value) {
            WaitThread(std::move(thread), address, kResultSuccess, std::nullopt);
        }
        break;
    case ArbitrationType::WaitIfLessThanWithTimeout:
        if (read_word() < value) {
            WaitThread(std::move(thread), address, kResultTimeout, timeout_ns);
        }
        break;
    case ArbitrationType::DecrementAndWaitIfLessThan: {
        const s32 word = read_word();
        if (word < value) {
            // Only change the memory value if the thread should wait.
            if (!memory.Write<s32>(address, static_cast<s32>(static_cast<u32>(word) - 1u))) {
                MH4N_LOG(Error, "Kernel", "arbiter: write to unmapped address 0x{:08X}", address);
            }
            WaitThread(std::move(thread), address, kResultSuccess, std::nullopt);
        }
        break;
    }
    case ArbitrationType::DecrementAndWaitIfLessThanWithTimeout: {
        const s32 word = read_word();
        if (word < value) {
            if (!memory.Write<s32>(address, static_cast<s32>(static_cast<u32>(word) - 1u))) {
                MH4N_LOG(Error, "Kernel", "arbiter: write to unmapped address 0x{:08X}", address);
            }
            WaitThread(std::move(thread), address, kResultTimeout, timeout_ns);
        }
        break;
    }
    default:
        MH4N_LOG(Error, "Kernel", "arbiter: unknown type={}", static_cast<u32>(type));
        return kResultInvalidEnumValueFnd;
    }

    // The calls that use a timeout always return the Timeout error, even if they did not sleep.
    if (type == ArbitrationType::WaitIfLessThanWithTimeout ||
        type == ArbitrationType::DecrementAndWaitIfLessThanWithTimeout) {
        return kResultTimeout;
    }
    return kResultSuccess;
}

} // namespace mh4n
