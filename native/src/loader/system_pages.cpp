// Derived from Azahar src/core/hle/kernel/config_mem.cpp, shared_page.cpp and memory.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the initial contents of the
// configuration memory (firmware 11.17.0) and of the shared page, the memory fields MemoryInit
// adds, and UpdateTimeCallback / GetSystemTimeSince2000.
// Copyright 2014 Citra Emulator Project (config_mem.cpp)
// Copyright 2015 Citra Emulator Project (shared_page.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (memory.cpp)
#include <cstring>

#include "mh4n/loader.h"

namespace mh4n {
namespace {

void Put8(u8* page, u32 offset, u8 value) {
    page[offset] = value;
}
void Put32(u8* page, u32 offset, u32 value) {
    for (int i = 0; i < 4; ++i) {
        page[offset + i] = static_cast<u8>(value >> (8 * i));
    }
}
void Put64(u8* page, u32 offset, u64 value) {
    for (int i = 0; i < 8; ++i) {
        page[offset + i] = static_cast<u8>(value >> (8 * i));
    }
}

}  // namespace

void FillConfigMemory(u8* page, const FcramRegions& regions) {
    std::memset(page, 0, kConfigMemorySize);
    using namespace config_mem;
    // Values extracted from firmware 11.17.0-50E
    Put8(page, kKernelVersionMin, 0x3a);
    Put8(page, kKernelVersionMaj, 0x2);
    Put64(page, kNsTid, 0x0004013000008002);
    Put32(page, kSysCoreVer, 0x2);
    Put8(page, kUnitInfo, 0x1);  // Bit 0 set for Retail
    Put8(page, kPrevFirm, 0x1);
    Put32(page, kCtrSdkVer, 0x0000F450);
    Put8(page, kFirmVersionMin, 0x3a);
    Put8(page, kFirmVersionMaj, 0x2);
    Put32(page, kFirmSysCoreVer, 0x2);
    Put32(page, kFirmCtrSdkVer, 0x0000F450);
    // KernelSystem::MemoryInit
    Put32(page, kAppMemType, regions.MemoryMode());
    Put32(page, kAppMemAlloc, regions.Size(FcramRegion::Application));
    Put32(page, kSysMemAlloc, regions.Size(FcramRegion::System));
    Put32(page, kBaseMemAlloc, regions.Size(FcramRegion::Base));
}

void FillSharedPage(u8* page) {
    std::memset(page, 0, kSharedPageSize);
    using namespace shared_page;
    Put8(page, kRunningHw, 0x1);  // product
    // Some games wait until this value becomes 0x1, before asking running_hw
    Put8(page, kUnknownValue, 0x1);
    // A completely full battery (PTM ChargeLevels::CompletelyFull = 5), adapter connected, charging
    Put8(page, kBatteryState, static_cast<u8>(1 | (1 << 1) | (5 << 2)));
    // Wi-Fi: best link level, state Internet (a connection faked from the NDM service)
    Put8(page, kWifiLinkLevel, 3);
    Put8(page, kWifiState, 2);
    // 3D slider at 0.0: all-zero bytes
}

Result SystemPages::Map(GuestMemory& memory, const FcramRegions& regions) {
    // Azahar backs the two pages with two separate objects, so Query sees two blocks, not one:
    // give them backings that are not contiguous (opaque tokens, not physical addresses).
    constexpr u64 kConfigMemoryBacking = 0x100000000ull;
    constexpr u64 kSharedPageBacking = 0x200000000ull;
    Result result = memory.Map(kConfigMemoryVaddr, kConfigMemorySize, MemPerm::Read, MemState::Shared,
                               kConfigMemoryBacking);
    if (IsError(result)) {
        return result;
    }
    result = memory.Map(kSharedPageVaddr, kSharedPageSize, MemPerm::Read, MemState::Shared,
                         kSharedPageBacking);
    if (IsError(result)) {
        return result;
    }
    u8 config[kConfigMemorySize];
    FillConfigMemory(config, regions);
    memory.WriteBlock(kConfigMemoryVaddr, config, kConfigMemorySize);
    u8 shared[kSharedPageSize];
    FillSharedPage(shared);
    memory.WriteBlock(kSharedPageVaddr, shared, kSharedPageSize);
    memory_ = &memory;
    UpdateSharedPage(0);  // Azahar schedules the first update at tick 0
    return 0;
}

u64 SystemPages::SystemTimeSince2000(u64 ticks) const {
    // GetGlobalTimeUs: ticks * 1000000 / clock, without overflowing for long runs
    const u64 us = (ticks / kBaseClockRateArm11) * 1000000 +
                   (ticks % kBaseClockRateArm11) * 1000000 / kBaseClockRateArm11;
    const u64 now_ms = init_time_ * 1000 + us / 1000;
    const u64 epoch_ms = kUnixTimeOf2000 * 1000;
    // Only when the time is after 2000 is it the 3DS time
    return now_ms > epoch_ms ? now_ms - epoch_ms : 0;
}

u64 SystemPages::SystemTimeSince1900(u64 ticks) const {
    return kMillisecondsFrom1900To2000 + SystemTimeSince2000(ticks);
}

void SystemPages::UpdateSharedPage(u64 ticks) {
    using namespace shared_page;
    if (!memory_) {
        return;
    }
    const u32 base = kSharedPageVaddr;
    const u32 counter = memory_->Read<u32>(base + kDateTimeCounter).value_or(0);
    const u32 block = base + (counter % 2 ? kDateTime0 : kDateTime1);
    memory_->Write<u64>(block + kDtDateTime, SystemTimeSince1900(ticks));
    memory_->Write<u64>(block + kDtUpdateTick, ticks);
    memory_->Write<u64>(block + kDtTickToSecond, kBaseClockRateArm11);
    memory_->Write<u64>(block + kDtTickOffset, 0);
    memory_->Write<u32>(base + kDateTimeCounter, counter + 1);
}

void SystemPages::Set3DSlider(float value) {
    if (memory_) {
        memory_->Write<float>(kSharedPageVaddr + shared_page::kSliderState3d, value);
    }
}

}  // namespace mh4n
