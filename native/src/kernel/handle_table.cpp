// Derived from Azahar src/core/hle/kernel/handle_table.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the slot and generation scheme, the free list and the lookup rules.
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project
#include <utility>

#include "mh4n/kernel.h"
#include "mh4n/log.h"

namespace mh4n {
namespace {

constexpr u32 kGenerationBits = 15;
constexpr u32 kGenerationMask = (1u << kGenerationBits) - 1;

constexpr u32 SlotOf(Handle handle) {
    return handle >> kGenerationBits;
}
constexpr u16 GenerationOf(Handle handle) {
    return static_cast<u16>(handle & kGenerationMask);
}

} // namespace

HandleTable::HandleTable(Kernel& kernel) : kernel_(kernel) {
    next_generation_ = 1;
    Clear();
}

HandleTable::~HandleTable() = default;

Result HandleTable::Create(Handle* out, std::shared_ptr<Object> object) {
    const u16 slot = next_free_slot_;
    if (slot >= kMaxCount) {
        return kResultOutOfHandles;
    }
    next_free_slot_ = generations_[slot];
    const u16 generation = next_generation_++;
    // The generation has 15 bits, and CTR-OS does not use 0: wrap straight to 1.
    if (next_generation_ >= (1u << kGenerationBits)) {
        next_generation_ = 1;
    }
    generations_[slot] = generation;
    objects_[slot] = std::move(object);
    *out = generation | (static_cast<u32>(slot) << kGenerationBits);
    return kResultSuccess;
}

Result HandleTable::Duplicate(Handle* out, Handle handle) {
    std::shared_ptr<Object> object = GetGeneric(handle);
    if (!object) {
        return kResultInvalidHandle;
    }
    return Create(out, std::move(object));
}

Result HandleTable::Close(Handle handle) {
    if (!IsValid(handle)) {
        return kResultInvalidHandle;
    }
    const u32 slot = SlotOf(handle);
    // Keep the object until the table is consistent again: its destructor may run kernel code.
    std::shared_ptr<Object> released = std::move(objects_[slot]);
    objects_[slot] = nullptr;
    generations_[slot] = next_free_slot_;
    next_free_slot_ = static_cast<u16>(slot);
    released.reset();
    return kResultSuccess;
}

bool HandleTable::IsValid(Handle handle) const {
    const u32 slot = SlotOf(handle);
    return slot < kMaxCount && objects_[slot] != nullptr && generations_[slot] == GenerationOf(handle);
}

std::shared_ptr<Object> HandleTable::GetGeneric(Handle handle) const {
    if (handle == kHandleCurrentThread) {
        Thread* thread = kernel_.CurrentThread();
        return thread ? thread->shared_from_this() : nullptr;
    }
    if (handle == kHandleCurrentProcess) {
        return kernel_.CurrentProcess();
    }
    if (!IsValid(handle)) {
        return nullptr;
    }
    return objects_[SlotOf(handle)];
}

void HandleTable::Clear() {
    std::array<std::shared_ptr<Object>, kMaxCount> released;
    for (u32 i = 0; i < kMaxCount; ++i) {
        generations_[i] = static_cast<u16>(i + 1);
        released[i] = std::move(objects_[i]);
        objects_[i] = nullptr;
    }
    next_free_slot_ = 0;
    // Destroyed here, with the table already reset.
}

} // namespace mh4n
