// Derived from Azahar src/video_core/gpu.cpp and src/core/memory.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): GPU::VirtualToPhysicalAddress
// (the VRAM and linear-heap windows, the order they are tried in, the inclusive upper bounds) and
// the address constants.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h)
#include "gpu_capture/capture_host.h"

#include <array>
#include <cstdlib>
#include <cstring>

namespace mh4n::gpu_capture {

namespace {

struct FreeDeleter {
    void operator()(u8* p) const {
        std::free(p);
    }
};

// Azahar src/core/memory.h
constexpr u32 kVramVaddr = 0x1F000000;
constexpr u32 kVramVaddrEnd = kVramVaddr + kVramSize;
constexpr u32 kLinearHeapVaddr = 0x14000000;
constexpr u32 kLinearHeapVaddrEnd = kLinearHeapVaddr + 0x08000000;
constexpr u32 kNewLinearHeapVaddr = 0x30000000;
constexpr u32 kNewLinearHeapVaddrEnd = kNewLinearHeapVaddr + 0x10000000;

} // namespace

struct CaptureHost::Area {
    u32 base;
    u32 size;
    std::unique_ptr<u8, FreeDeleter> bytes; // calloc: pages nobody writes are never committed
};

struct CaptureHost::Memory {
    std::array<Area, 3> areas;

    Memory() {
        areas[0] = {kFcramPaddr, kFcramSize, nullptr};
        areas[1] = {kVramPaddr, kVramSize, nullptr};
        areas[2] = {kDspRamPaddr, kDspRamSize, nullptr};
        for (auto& area : areas) {
            area.bytes.reset(static_cast<u8*>(std::calloc(area.size, 1)));
        }
    }

    u8* Locate(u32 paddr, u32 size) const {
        for (const auto& area : areas) {
            if (area.bytes != nullptr && paddr >= area.base &&
                static_cast<u64>(paddr) + size <= static_cast<u64>(area.base) + area.size) {
                return area.bytes.get() + (paddr - area.base);
            }
        }
        return nullptr;
    }
};

CaptureHost::CaptureHost()
    : gpu_memory_(std::make_unique<Memory>()), reference_memory_(std::make_unique<Memory>()) {}

CaptureHost::~CaptureHost() = default;

u8* CaptureHost::PhysicalPointer(u32 paddr, u32 size) {
    return gpu_memory_->Locate(paddr, size);
}

const u8* CaptureHost::ReferencePointer(u32 paddr, u32 size) const {
    return reference_memory_->Locate(paddr, size);
}

std::optional<u32> CaptureHost::VirtualToPhysical(u32 vaddr) {
    if (vaddr == 0) {
        return std::nullopt;
    }
    if (vaddr >= kVramVaddr && vaddr <= kVramVaddrEnd) {
        return vaddr - kVramVaddr + kVramPaddr;
    }
    if (vaddr >= kLinearHeapVaddr && vaddr <= kLinearHeapVaddrEnd) {
        return vaddr - kLinearHeapVaddr + kFcramPaddr;
    }
    if (vaddr >= kNewLinearHeapVaddr && vaddr <= kNewLinearHeapVaddrEnd) {
        return vaddr - kNewLinearHeapVaddr + kFcramPaddr;
    }
    // Azahar returns the address unchanged (and logs an error) when it is in none of the windows;
    // for an address in the physical areas that is the right answer.
    if (gpu_memory_->Locate(vaddr, 1) != nullptr) {
        return vaddr;
    }
    return std::nullopt;
}

void CaptureHost::SignalInterrupt(gpu::Interrupt id, u64 delay_ns) {
    std::lock_guard lock(signal_mutex_);
    signalled_.push_back({id, delay_ns});
}

void CaptureHost::Post(std::function<void()> work) {
    work();
}

bool CaptureHost::WriteBoth(u32 paddr, const u8* data, u32 size) {
    u8* gpu_side = gpu_memory_->Locate(paddr, size);
    u8* reference_side = reference_memory_->Locate(paddr, size);
    if (gpu_side == nullptr || reference_side == nullptr) {
        return false;
    }
    std::memcpy(gpu_side, data, size);
    std::memcpy(reference_side, data, size);
    return true;
}

bool CaptureHost::ApplyReferencePage(const PageData& page) {
    u8* reference_side = reference_memory_->Locate(page.address, kPageSize);
    if (reference_side == nullptr) {
        return false;
    }
    std::memcpy(reference_side, page.data.data(), kPageSize);
    return true;
}

bool CaptureHost::ApplyPageToGpu(const PageData& page) {
    u8* gpu_side = gpu_memory_->Locate(page.address, kPageSize);
    if (gpu_side == nullptr) {
        return false;
    }
    std::memcpy(gpu_side, page.data.data(), kPageSize);
    return true;
}

std::vector<SignalledInterrupt> CaptureHost::TakeSignalled() {
    std::lock_guard lock(signal_mutex_);
    std::vector<SignalledInterrupt> taken;
    taken.swap(signalled_);
    return taken;
}

} // namespace mh4n::gpu_capture
