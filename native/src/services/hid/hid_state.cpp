// Derived from Azahar src/core/hle/service/hid/hid.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): the module's construction, its update
// events and the enabling of the sensors (see the header next to this file).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
#include "hid_state.h"

#include <memory>

#include "mh4n/log.h"
#include "mh4n/scheduler.h"

namespace mh4n::hid {

namespace {
constexpr const char* kLog = "Service.HID";
}

std::shared_ptr<Shared> GetShared(ServiceContext& context) {
    std::shared_ptr<Shared> shared = context.State<Shared>("hid");
    shared->Ensure(context);
    return shared;
}

void Shared::Ensure(ServiceContext& context) {
    if (initialised_) {
        return;
    }
    initialised_ = true;
    context_ = &context;
    scheduler_ = &context.Scheduler();
    Kernel& kernel = context.GetKernel();

    // CreateSharedMemory(nullptr, 0x1000, ReadWrite, Read, 0, BASE): 0x1000 bytes from the BASE
    // region, read-only for the application.
    const Result result = kernel.NewSharedMemory(&memory_, nullptr, kSharedMemorySize,
                                                 MemoryPermission::ReadWrite, MemoryPermission::Read,
                                                 0, FcramRegion::Base, "HID:SharedMemory");
    if (IsError(result)) {
        MH4N_LOG(Error, kLog, "cannot allocate the shared memory: 0x{:08X}", result);
        memory_.reset();
    }
    pad_or_touch_1_ = kernel.NewEvent(ResetType::OneShot, "HID:EventPadOrTouch1");
    pad_or_touch_2_ = kernel.NewEvent(ResetType::OneShot, "HID:EventPadOrTouch2");
    accelerometer_event_ = kernel.NewEvent(ResetType::OneShot, "HID:EventAccelerometer");
    gyroscope_event_ = kernel.NewEvent(ResetType::OneShot, "HID:EventGyroscope");
    debug_pad_event_ = kernel.NewEvent(ResetType::OneShot, "HID:EventDebugPad");

    origin_ = scheduler_->NowTicks();
    SchedulePad(origin_ + kPadUpdateTicks);
}

u8* Shared::MemoryPointer() const {
    return memory_ ? memory_->GetPointer() : nullptr;
}

InputState Shared::Sample(u64 tick) const {
    InputSource* input = context_->Interfaces().input;
    return input != nullptr ? input->Sample(tick) : InputState{};
}

// Each update comes `period` after the tick the last one was due at, not after the tick it ran at,
// as Azahar's `period - cycles_late`: a late event does not shift the ones after it.
void Shared::SchedulePad(u64 due) {
    std::weak_ptr<Shared> weak = weak_from_this();
    pad_timer_ = scheduler_->ScheduleAt(due, [weak, due] {
        if (const auto self = weak.lock()) {
            self->RunPadUpdate(due);
            self->SchedulePad(due + kPadUpdateTicks);
        }
    });
}

void Shared::ScheduleAccelerometer(u64 due) {
    std::weak_ptr<Shared> weak = weak_from_this();
    accelerometer_timer_ = scheduler_->ScheduleAt(due, [weak, due] {
        if (const auto self = weak.lock()) {
            self->RunAccelerometerUpdate(due);
            self->ScheduleAccelerometer(due + kAccelerometerUpdateTicks);
        }
    });
}

void Shared::ScheduleGyroscope(u64 due) {
    std::weak_ptr<Shared> weak = weak_from_this();
    gyroscope_timer_ = scheduler_->ScheduleAt(due, [weak, due] {
        if (const auto self = weak.lock()) {
            self->RunGyroscopeUpdate(due);
            self->ScheduleGyroscope(due + kGyroscopeUpdateTicks);
        }
    });
}

void Shared::RunPadUpdate(u64 tick) {
    module_.UpdatePad(MemoryPointer(), Sample(tick), tick);
    // Signal both handles when there's an update to Pad or touch
    pad_or_touch_1_->Signal();
    pad_or_touch_2_->Signal();
}

void Shared::RunAccelerometerUpdate(u64 tick) {
    module_.UpdateAccelerometer(MemoryPointer(), Sample(tick), tick);
    accelerometer_event_->Signal();
}

void Shared::RunGyroscopeUpdate(u64 tick) {
    module_.UpdateGyroscope(MemoryPointer(), Sample(tick), tick);
    gyroscope_event_->Signal();
}

// Schedules the update event if the sensor was just enabled, one period from now.
void Shared::EnableAccelerometer() {
    if (module_.EnableAccelerometer()) {
        ScheduleAccelerometer(scheduler_->NowTicks() + kAccelerometerUpdateTicks);
    }
}

// Unschedules the update event if the sensor was just disabled.
void Shared::DisableAccelerometer() {
    if (module_.DisableAccelerometer()) {
        scheduler_->Cancel(accelerometer_timer_);
    }
}

void Shared::EnableGyroscope() {
    if (module_.EnableGyroscope()) {
        ScheduleGyroscope(scheduler_->NowTicks() + kGyroscopeUpdateTicks);
    }
}

void Shared::DisableGyroscope() {
    if (module_.DisableGyroscope()) {
        scheduler_->Cancel(gyroscope_timer_);
    }
}

}  // namespace mh4n::hid
