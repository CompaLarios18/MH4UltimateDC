// Derived from Azahar src/core/hle/service/dsp/dsp_dsp.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: the file says "Licensed under GPLv2 or any later version"): the state DSP_DSP
// keeps for all its sessions (semaphore_event, preset_semaphore, interrupt_zero, interrupt_one, pipes),
// the six interrupt slots (max_number_of_interrupt_events), GetInterruptEvent, HasTooManyEventsRegistered
// and SignalInterrupt.
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (dsp_dsp.h)
//
// What all the sessions of dsp::DSP share (N1-18), private to mh4n_services_dsp. Azahar has one DSP_DSP
// object, with one DSP behind it, whatever the number of sessions; here the sessions are services of
// their own and this is the part they have in common: the model of the DSP, the events the
// application registered, the timer that runs an audio frame, and the capture.
//
// Threads (principle 6): everything here belongs to the scheduler thread. The audio frame runs from
// the scheduler's timer queue, never from the host's clock; each deadline is the previous one plus
// kAudioFrameTicks, so a late frame is made up for and the cadence does not drift.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "audio/dsp_layout.h"
#include "audio/dsp_model.h"
#include "audio/mdsp_writer.h"
#include "mh4n/kernel.h"
#include "mh4n/service.h"

namespace mh4n {

// --capture-dsp: the writer and which audio frames go into it (the boot step opens it).
struct DspCapture {
    audio::MdspWriter writer;
    u32 start_frame = 0;  // the first audio frame at or after this game frame is the first recorded
    u32 frames = 0;       // how many audio frames
    bool started = false;
    u64 origin_tick = 0;  // the game clock when the capture was opened: the records' ticks count from it
};

class DspShared : public std::enable_shared_from_this<DspShared> {
public:
    DspShared();
    ~DspShared();
    DspShared(const DspShared&) = delete;
    DspShared& operator=(const DspShared&) = delete;

    // The first session makes the model on the DSP RAM, the events and the frame timer; later calls
    // do nothing.
    // `at_boot`: called when the module is made with the system, the way Azahar makes its DSP: the frames are
    // then on a grid that starts at the clock's first reading (its first tick one frame after it), which in
    // the deterministic mode is the reference's starting tick, not a multiple of the frame (in real time, the
    // first reading of the host-driven clock: the boot step calls this in every mode).
    void Start(ServiceContext& context, bool at_boot = false);

    audio::DspModel& Model() { return *model_; }
    ServiceContext& Context() { return *context_; }

    // dsp::DSP's events: the semaphore event, and the interrupts the application registered.
    std::shared_ptr<Event> semaphore_event;
    std::shared_ptr<Event> interrupt_zero;
    std::shared_ptr<Event> interrupt_one;
    std::array<std::shared_ptr<Event>, audio::kNumPipes> pipes{};
    u16 preset_semaphore = 0;

    // The slot of an interrupt, or null for a type or pipe that does not exist.
    std::shared_ptr<Event>* InterruptEvent(audio::InterruptType type, audio::DspPipe pipe);
    // Actual service implementation only has 6 'slots' for interrupts.
    static constexpr std::size_t kMaxInterruptEvents = 6;
    bool HasTooManyEventsRegistered() const;
    void SignalInterrupt(audio::InterruptType type, audio::DspPipe pipe);

    // For tests: the audio frames the timer has run, and the deadline of the next one.
    u64 FramesRun() const { return frames_run_; }
    u64 NextDeadline() const { return deadline_; }
    // For tests: called at the start of every audio frame with its deadline and the game clock.
    std::function<void(u64 deadline, u64 now)> frame_observer;

private:
    void ScheduleNext();
    void OnTick();
    void CaptureFrame(const u8* dsp_memory);

    ServiceContext* context_ = nullptr;
    std::unique_ptr<audio::DspModel> model_;
    std::shared_ptr<DspCapture> capture_;
    u64 deadline_ = 0;  // the tick of the frame that ran last (or is about to), a multiple of the frame
    u64 timer_id_ = 0;
    u64 frames_run_ = 0;
};

}  // namespace mh4n
