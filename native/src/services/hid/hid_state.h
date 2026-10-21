// Derived from Azahar src/core/hle/service/hid/hid.cpp and src/core/hle/service/hid/hid.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later
// version"): what the HID module owns (the shared memory, the five events, the three update events
// and when they start) and the service commands.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
//
// The HID module's state (N1-16): what hid:USER and hid:SPVR share, kept in the service context
// under the key "hid". Made by the `services.ipc.hid` boot step, as Azahar makes its module when
// the system is made: the pad update starts one period after that, whether or not the game has
// opened hid:USER yet. Everything runs on the scheduler thread (principle 6): the updates are
// events of the scheduler's timer queue, so they come at exact ticks of the game clock and never
// at the host's time. Each update asks the input source for the state at the tick it is due at.
#pragma once

#include <memory>

#include "hid_module.h"
#include "mh4n/kernel.h"
#include "mh4n/service.h"

namespace mh4n {
class GuestScheduler;
class Runtime;
}  // namespace mh4n

namespace mh4n::hid {

class Shared : public std::enable_shared_from_this<Shared> {
public:
    // Makes the shared memory and the events and starts the pad update. Once; later calls do
    // nothing.
    void Ensure(ServiceContext& context);

    bool Initialised() const { return initialised_; }
    // The tick the module was made at: the first pad update is due one period after it.
    u64 Origin() const { return origin_; }

    const std::shared_ptr<SharedMemory>& Memory() const { return memory_; }
    // pad_or_touch_1, pad_or_touch_2, accelerometer, gyroscope, debug pad
    const std::shared_ptr<Event>& PadOrTouch1() const { return pad_or_touch_1_; }
    const std::shared_ptr<Event>& PadOrTouch2() const { return pad_or_touch_2_; }
    const std::shared_ptr<Event>& AccelerometerEvent() const { return accelerometer_event_; }
    const std::shared_ptr<Event>& GyroscopeEvent() const { return gyroscope_event_; }
    const std::shared_ptr<Event>& DebugPadEvent() const { return debug_pad_event_; }

    Module& Logic() { return module_; }

    // The service commands.
    void EnableAccelerometer();
    void DisableAccelerometer();
    void EnableGyroscope();
    void DisableGyroscope();

    // The blocks of the memory as the game sees them: null until the memory exists.
    u8* MemoryPointer() const;

    // One update, due at `tick` (the timer events call these; tests may too). Each samples the input
    // source (neutral if there is none), writes the block, signals the event and does NOT
    // reschedule.
    void RunPadUpdate(u64 tick);
    void RunAccelerometerUpdate(u64 tick);
    void RunGyroscopeUpdate(u64 tick);

private:
    InputState Sample(u64 tick) const;
    void SchedulePad(u64 due);
    void ScheduleAccelerometer(u64 due);
    void ScheduleGyroscope(u64 due);

    bool initialised_ = false;
    u64 origin_ = 0;
    ServiceContext* context_ = nullptr;
    GuestScheduler* scheduler_ = nullptr;
    std::shared_ptr<SharedMemory> memory_;
    std::shared_ptr<Event> pad_or_touch_1_, pad_or_touch_2_, accelerometer_event_, gyroscope_event_,
        debug_pad_event_;
    Module module_;
    u64 pad_timer_ = 0;
    u64 accelerometer_timer_ = 0;
    u64 gyroscope_timer_ = 0;
};

// The shared state of a service manager, made and started if it was not yet.
std::shared_ptr<Shared> GetShared(ServiceContext& context);

}  // namespace mh4n::hid
