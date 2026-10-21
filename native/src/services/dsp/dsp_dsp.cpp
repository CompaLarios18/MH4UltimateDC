// Derived from Azahar src/core/hle/service/dsp/dsp_dsp.cpp, src/core/hle/service/dsp/dsp_dsp.h,
// src/audio_core/hle/hle.cpp and src/core/hle/result.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or any later version"): the commands of
// dsp::DSP, their command ids, parameters, replies and names (the table of FunctionInfo), the
// translation of a DSP address (ConvertProcessAddressFromDspDram), the changes the application makes
// to the pipes (WriteProcessPipe: the bytes the game leaves at random in a message are cleared),
// the six interrupt slots and the result for too many events, the interrupts the DSP raises
// (SignalInterrupt, the audio tick that raises the audio pipe's) and the audio frame's timer
// (AudioTickCallback).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (dsp_dsp.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (dsp_dsp.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (result.h)
//
// The dsp::DSP service (N1-18): the application's way to the DSP. The DSP behind it is the model of
// mh4n_audio (the sound is N3's), which advances once every audio frame, 1,310,720 ticks, from the
// timer queue of the scheduler: the application's sound thread waits for the interrupt of the audio
// pipe and then writes the next region, so the cadence is part of what the game can observe.
//
// Departures from Azahar:
//  - A channel, an interrupt number or a register that Azahar asserts on, a message too short for its
//    pipe, and a ReadPipe for more than the pipe holds (which "hangs the hardware"), are logged and
//    answered: the register as register 0, the others with an error result (RegisterInterruptEvents:
//    a descriptor 1005 of the DSP module) or an empty buffer. A LoadComponent whose size is larger
//    than its buffer reads only what the buffer holds.
//  - The firmware is identified in the log by the CRC-32 of its bytes (Azahar logs a CityHash).
//  - GetHeadphoneStatus answers "not inserted" (Azahar's setting simulate_headphones_plugged is off).
//  - All sessions share one DSP, as Azahar's single DSP_DSP object does, and so do their events.
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include "audio/dsp_layout.h"
#include "audio/dsp_model.h"
#include "dsp_shared.h"
#include "mh4n/assert.h"
#include "mh4n/crc32.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.DSP";

// ErrorModule::DSP, and the pieces of the results below (Azahar's result.h).
constexpr u32 kModuleDsp = 41;
constexpr u32 kDescriptionInvalidResultValue = 1023;
constexpr u32 kDescriptionInvalidEnumValue = 1005;
constexpr u32 kSummaryOutOfResource = 3;
constexpr u32 kSummaryInvalidArgument = 7;
constexpr u32 kLevelStatus = 25;
constexpr u32 kLevelPermanent = 27;
// "Ran out of space to register interrupts".
constexpr Result kResultTooManyInterruptEvents =
    MakeResult(kDescriptionInvalidResultValue, kModuleDsp, kSummaryOutOfResource, kLevelStatus);
static_assert(kResultTooManyInterruptEvents == 0xC860A7FF);
// Where Azahar asserts on an interrupt or channel that does not exist.
constexpr Result kResultInvalidInterrupt =
    MakeResult(kDescriptionInvalidEnumValue, kModuleDsp, kSummaryInvalidArgument, kLevelPermanent);

// The offset ConvertProcessAddressFromDspDram adds to the DSP RAM's address in the application's space
// (kDspRamVaddr, memory.h) for a DSP word address.
constexpr u32 kDspDramOffset = 0x40000;

}  // namespace

// ---- what the sessions share -------------------------------------------------------------------------------

DspShared::DspShared() = default;

DspShared::~DspShared() {
    if (timer_id_ != 0 && context_ != nullptr) {
        context_->GetKernel().Cancel(timer_id_);
    }
}

void DspShared::Start(ServiceContext& context) {
    if (model_ != nullptr) {
        return;
    }
    u8* const memory = context.Memory().PhysicalPointer(kDspRamPaddr, kDspRamSize);
    MH4N_ASSERT(memory != nullptr, "the DSP RAM is not mapped");
    context_ = &context;
    model_ = std::make_unique<audio::DspModel>(memory);
    model_->SetInterruptHandler(
        [this](audio::InterruptType type, audio::DspPipe pipe) { SignalInterrupt(type, pipe); });
    capture_ = context.State<DspCapture>("dsp::capture");
    model_->SetFrameStartHook([this](const u8* dsp_memory) { CaptureFrame(dsp_memory); });

    semaphore_event = context.GetKernel().NewEvent(ResetType::OneShot, "DSP_DSP::semaphore_event");
    semaphore_event->SetHleNotifier([this] { model_->SetSemaphore(preset_semaphore); });

    // The frames are on a grid from tick 0, as Azahar's are (its first tick is one frame after the
    // start of the core timing): a DSP that is first used later runs on the same grid.
    deadline_ = context.GetKernel().NowTicks() / audio::kAudioFrameTicks * audio::kAudioFrameTicks;
    ScheduleNext();
}

void DspShared::ScheduleNext() {
    // From the previous deadline, not from now: a frame that runs late is made up for (Azahar
    // reschedules with `audio_frame_ticks - cycles_late`).
    deadline_ += audio::kAudioFrameTicks;
    timer_id_ = context_->GetKernel().ScheduleAt(
        deadline_, [weak = weak_from_this()] {
            if (const std::shared_ptr<DspShared> self = weak.lock()) {
                self->OnTick();
            }
        });
}

void DspShared::OnTick() {
    timer_id_ = 0;
    ++frames_run_;
    if (frame_observer) {
        frame_observer(deadline_, context_->GetKernel().NowTicks());
    }
    if (model_->Tick()) {
        // The frame goes to the sink: silence, since nothing is mixed.
        static const std::array<s16, 2 * audio::kSamplesPerFrame> silence{};
        if (AudioSink* const sink = context_->Interfaces().audio) {
            sink->Queue(silence.data(), audio::kSamplesPerFrame);
        }
        // TODO(merry) in Azahar: "Signal all the other interrupts as appropriate."
        SignalInterrupt(audio::InterruptType::Pipe, audio::DspPipe::Audio);
    }
    ScheduleNext();
}

void DspShared::CaptureFrame(const u8* dsp_memory) {
    DspCapture& capture = *capture_;
    if (!capture.writer.IsOpen()) {
        return;
    }
    // The game's frame counter, read now; a program that does not map it has no frames.
    u32 frame = 0;
    if (const auto value = context_->Memory().Read<u32>(kFrameCounterAddress)) {
        frame = *value;
    }
    if (!capture.started && frame < capture.start_frame) {
        return;
    }
    capture.started = true;
    const u64 ticks = context_->GetKernel().NowTicks() - capture.origin_tick;
    capture.writer.AddRegions(frame, ticks, dsp_memory + audio::kRegionOffset[0],
                              dsp_memory + audio::kRegionOffset[1], audio::kRegionSize);
    if (capture.writer.Frames() >= capture.frames) {
        capture.writer.Close();  // the hook stays set and does nothing from now on
    }
}

std::shared_ptr<Event>* DspShared::InterruptEvent(audio::InterruptType type, audio::DspPipe pipe) {
    switch (type) {
    case audio::InterruptType::Zero:
        return &interrupt_zero;
    case audio::InterruptType::One:
        return &interrupt_one;
    case audio::InterruptType::Pipe: {
        const std::size_t index = static_cast<std::size_t>(pipe);
        return index < pipes.size() ? &pipes[index] : nullptr;
    }
    default:
        return nullptr;
    }
}

bool DspShared::HasTooManyEventsRegistered() const {
    std::size_t number = static_cast<std::size_t>(
        std::count_if(pipes.begin(), pipes.end(), [](const auto& event) { return event != nullptr; }));
    if (interrupt_zero != nullptr) {
        ++number;
    }
    if (interrupt_one != nullptr) {
        ++number;
    }
    return number >= kMaxInterruptEvents;
}

void DspShared::SignalInterrupt(audio::InterruptType type, audio::DspPipe pipe) {
    MH4N_LOG(Trace, kLog, "called, type={}, pipe={}", static_cast<u32>(type), static_cast<u32>(pipe));
    if (std::shared_ptr<Event>* const slot = InterruptEvent(type, pipe)) {
        if (*slot) {
            (*slot)->Signal();
        }
    }
}

// ---- the service -----------------------------------------------------------------------------------------------

namespace {

class DspDsp final : public Service {
public:
    explicit DspDsp(ServiceContext& context)
        : Service(context, "dsp::DSP"), shared_(context.State<DspShared>("dsp::DSP")) {
        shared_->Start(context);
        Register(0x0001, "RecvData", &DspDsp::RecvData);
        Register(0x0002, "RecvDataIsReady", &DspDsp::RecvDataIsReady);
        Register(0x0003, "SendData");
        Register(0x0004, "SendDataIsEmpty");
        Register(0x0005, "SendFifoEx");
        Register(0x0006, "RecvFifoEx");
        Register(0x0007, "SetSemaphore", &DspDsp::SetSemaphore);
        Register(0x0008, "GetSemaphore");
        Register(0x0009, "ClearSemaphore");
        Register(0x000A, "MaskSemaphore");
        Register(0x000B, "CheckSemaphoreRequest");
        Register(0x000C, "ConvertProcessAddressFromDspDram",
                 &DspDsp::ConvertProcessAddressFromDspDram);
        Register(0x000D, "WriteProcessPipe", &DspDsp::WriteProcessPipe);
        Register(0x000E, "ReadPipe", &DspDsp::ReadPipe);
        Register(0x000F, "GetPipeReadableSize", &DspDsp::GetPipeReadableSize);
        Register(0x0010, "ReadPipeIfPossible", &DspDsp::ReadPipeIfPossible);
        Register(0x0011, "LoadComponent", &DspDsp::LoadComponent);
        Register(0x0012, "UnloadComponent", &DspDsp::UnloadComponent);
        Register(0x0013, "FlushDataCache", &DspDsp::FlushDataCache);
        Register(0x0014, "InvalidateDCache", &DspDsp::InvalidateDataCache);
        Register(0x0015, "RegisterInterruptEvents", &DspDsp::RegisterInterruptEvents);
        Register(0x0016, "GetSemaphoreEventHandle", &DspDsp::GetSemaphoreEventHandle);
        Register(0x0017, "SetSemaphoreMask", &DspDsp::SetSemaphoreMask);
        Register(0x0018, "GetPhysicalAddress");
        Register(0x0019, "GetVirtualAddress");
        Register(0x001A, "SetIirFilterI2S1_cmd1");
        Register(0x001B, "SetIirFilterI2S1_cmd2");
        Register(0x001C, "SetIirFilterEQ");
        Register(0x001D, "ReadMultiEx_SPI2");
        Register(0x001E, "WriteMultiEx_SPI2");
        Register(0x001F, "GetHeadphoneStatus", &DspDsp::GetHeadphoneStatus);
        Register(0x0020, "ForceHeadphoneOut", &DspDsp::ForceHeadphoneOut);
        Register(0x0021, "GetIsDspOccupied");
    }

private:
    audio::DspModel& Dsp() { return shared_->Model(); }

    // Inputs:  0: 0x00010040, 1: the register number
    // Outputs: 0: 0x00010080, 1: result, 2: u16, the value in the register
    void RecvData(Request& request) {
        const u32 register_number = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u16>(Dsp().RecvData(register_number));
        MH4N_LOG(Debug, kLog, "register_number={}", register_number);
    }

    // Inputs:  0: 0x00020040, 1: the register number
    // Outputs: 0: 0x00020080, 1: result, 2: 0 not ready, 1 ready
    void RecvDataIsReady(Request& request) {
        const u32 register_number = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(Dsp().RecvDataIsReady(register_number));
        MH4N_LOG(Debug, kLog, "register_number={}", register_number);
    }

    // Inputs:  0: 0x00070040, 1: u16, the semaphore value
    void SetSemaphore(Request& request) {
        const u16 semaphore_value = request.Pop<u16>();
        Dsp().SetSemaphore(semaphore_value);
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Info, kLog, "called, semaphore_value={:04X}", semaphore_value);
    }

    // Inputs:  0: 0x000C0040, 1: the address (a word address in the DSP's DRAM)
    // Outputs: 0: 0x000C0080, 1: result, 2: the address in the application's space
    void ConvertProcessAddressFromDspDram(Request& request) {
        const u32 address = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        // "There is a per-region offset missing in this calculation (that seems to be always zero)."
        reply.Push<u32>((address << 1) + (kDspRamVaddr + kDspDramOffset));
        MH4N_LOG(Debug, kLog, "address=0x{:08X}", address);
    }

    // Inputs:  0: 0x000D0082, 1: the channel, 2: the size, 3: the static buffer descriptor, 4: its address
    void WriteProcessPipe(Request& request) {
        const u32 channel = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const StaticBuffer* const message = request.PopStaticBuffer();
        std::vector<u8> buffer;
        if (message != nullptr) {
            buffer = message->data;
        }

        // This behaviour was confirmed by RE. The likely reason for this is that games tend to pass in
        // garbage at these bytes because they read random bytes off the stack.
        bool usable = true;
        switch (static_cast<audio::DspPipe>(channel)) {
        case audio::DspPipe::Audio:
            if (buffer.size() >= 4) {
                buffer[2] = 0;
                buffer[3] = 0;
            } else {
                usable = false;  // Azahar asserts
            }
            break;
        case audio::DspPipe::Binary:
            if (buffer.size() >= 8) {
                buffer[4] = 1;
                buffer[5] = 0;
                buffer[6] = 0;
                buffer[7] = 0;
            } else {
                usable = false;  // Azahar asserts
            }
            break;
        default:
            MH4N_LOG(Error, kLog, "Unknown pipe {}", channel);
            break;
        }
        if (usable) {
            Dsp().PipeWrite(channel, buffer);
        } else {
            MH4N_LOG(Error, kLog, "pipe {}: a message of {} bytes is too short; it is ignored",
                     channel, buffer.size());
        }

        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Debug, kLog, "channel={}, size=0x{:X}, buffer_size={:X}", channel, size,
                 buffer.size());
    }

    // Inputs:  0: 0x000E00C0, 1: the channel, 2: the peer, 3: the size
    // Outputs: 0: 0x000E0042, 1: result, 2: the static buffer descriptor, 3: its address
    void ReadPipe(Request& request) {
        const u32 channel = request.Pop<u32>();
        const u32 peer = request.Pop<u32>();
        const u16 size = request.Pop<u16>();
        const std::size_t readable = Dsp().PipeReadableSize(channel);
        std::vector<u8> pipe_buffer;
        if (readable >= size) {
            pipe_buffer = Dsp().PipeRead(channel, size);
        } else {
            // "No more data is in pipe. Hardware hangs in this case; Should never happen." Azahar
            // aborts here.
            MH4N_LOG(Error, kLog, "ReadPipe of {} bytes from pipe {} with {} readable", size, channel,
                     readable);
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(std::move(pipe_buffer), 0);
        MH4N_LOG(Debug, kLog, "channel={}, peer={}, size=0x{:04X}, pipe_readable_size=0x{:04X}", channel,
                 peer, size, readable);
    }

    // Inputs:  0: 0x000F0080, 1: the channel, 2: the peer
    // Outputs: 0: 0x000F0080, 1: result, 2: u16, the readable size
    void GetPipeReadableSize(Request& request) {
        const u32 channel = request.Pop<u32>();
        const u32 peer = request.Pop<u32>();
        const u16 readable = static_cast<u16>(Dsp().PipeReadableSize(channel));
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u16>(readable);
        MH4N_LOG(Debug, kLog, "channel={}, peer={}, return pipe_readable_size=0x{:04X}", channel, peer,
                 readable);
    }

    // Inputs:  0: 0x001000C0, 1: the channel, 2: the peer, 3: the size
    // Outputs: 0: 0x00100082, 1: result, 2: u16, the bytes read, 3: the static buffer descriptor,
    //          4: its address
    void ReadPipeIfPossible(Request& request) {
        const u32 channel = request.Pop<u32>();
        const u32 peer = request.Pop<u32>();
        const u16 size = request.Pop<u16>();
        const u16 readable = static_cast<u16>(Dsp().PipeReadableSize(channel));
        std::vector<u8> pipe_buffer;
        if (readable >= size) {
            pipe_buffer = Dsp().PipeRead(channel, size);
        }
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<u16>(static_cast<u16>(pipe_buffer.size()));
        reply.PushStaticBuffer(std::move(pipe_buffer), 0);
        MH4N_LOG(Debug, kLog, "channel={}, peer={}, size=0x{:04X}, pipe_readable_size=0x{:04X}", channel,
                 peer, size, readable);
    }

    // Inputs:  0: 0x001100C2, 1: the size, 2: u16 program mask, u16 data mask, 3: the mapped buffer
    //          descriptor, 4: its address
    // Outputs: 0: 0x00110042, 1: result, 2: true, 3: the mapped buffer descriptor, 4: its id
    void LoadComponent(Request& request) {
        const u32 size = request.Pop<u32>();
        const u16 prog_mask = request.Pop<u16>();
        const u16 data_mask = request.Pop<u16>();
        const MappedBuffer* const buffer = request.PopMappedBuffer();
        if (buffer == nullptr) {
            request.MakeReply(1, 0).Push(kResultInvalidBufferDescriptor);
            return;
        }
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push(true);
        reply.PushMappedBuffer(*buffer);

        // The firmware is only identified: the HLE needs no DSP program.
        const u32 readable = std::min(size, buffer->Size());
        std::vector<u8> component_data(readable);
        if (readable != 0 && buffer->Read(component_data.data(), 0, readable)) {
            MH4N_LOG(Info, kLog, "Firmware crc32: {:#010x}",
                     Crc32(component_data.data(), component_data.size()));
        }
        MH4N_LOG(Info, kLog, "called size=0x{:X}, prog_mask=0x{:04X}, data_mask=0x{:04X}", size,
                 prog_mask, data_mask);
    }

    // Inputs:  0: 0x00120000
    void UnloadComponent(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Info, kLog, "called");
    }

    // Inputs:  0: 0x00130082, 1: the address, 2: the size, 3: the copy handle descriptor, 4: the process
    void FlushDataCache(Request& request) {
        const u32 address = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const auto process = request.PopObject<Process>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Trace, kLog, "called address=0x{:08X}, size=0x{:X}, process={}", address, size,
                 process ? process->process_id : 0);
    }

    // Inputs:  0: 0x00140082, as FlushDataCache
    void InvalidateDataCache(Request& request) {
        const u32 address = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const auto process = request.PopObject<Process>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Trace, kLog, "called address=0x{:08X}, size=0x{:X}, process={}", address, size,
                 process ? process->process_id : 0);
    }

    // Inputs:  0: 0x00150082, 1: the interrupt, 2: the channel, 3: the copy handle descriptor, 4: the
    //          event (0 unregisters)
    void RegisterInterruptEvents(Request& request) {
        const u32 interrupt = request.Pop<u32>();
        const u32 channel = request.Pop<u32>();
        const auto event = request.PopObject<Event>();

        if (interrupt >= static_cast<u32>(audio::InterruptType::Count) || channel >= audio::kNumPipes) {
            // Azahar asserts.
            MH4N_LOG(Error, kLog, "Invalid type or pipe: interrupt = {}, channel = {}", interrupt,
                     channel);
            request.MakeReply(1, 0).Push(kResultInvalidInterrupt);
            return;
        }
        const auto type = static_cast<audio::InterruptType>(interrupt);
        const auto pipe = static_cast<audio::DspPipe>(channel);
        std::shared_ptr<Event>& slot = *shared_->InterruptEvent(type, pipe);

        Reply& reply = request.MakeReply(1, 0);
        if (event) {  // register the interrupt event
            if (shared_->HasTooManyEventsRegistered()) {
                MH4N_LOG(Info, kLog,
                         "Ran out of space to register interrupts (Attempted to register "
                         "interrupt={}, channel={}, event={})",
                         interrupt, channel, event->Name());
                reply.Push(kResultTooManyInterruptEvents);
                return;
            }
            slot = event;
            MH4N_LOG(Info, kLog, "Registered interrupt={}, channel={}, event={}", interrupt, channel,
                     event->Name());
        } else {  // otherwise unregister it
            slot = nullptr;
            MH4N_LOG(Info, kLog, "Unregistered interrupt={}, channel={}", interrupt, channel);
        }
        reply.Push(kResultSuccess);
    }

    // Outputs: 0: 0x00160042, 1: result, 2: the translation descriptor, 3: the semaphore event
    void GetSemaphoreEventHandle(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(shared_->semaphore_event);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    // Inputs:  0: 0x00170040, 1: u16, the mask
    void SetSemaphoreMask(Request& request) {
        shared_->preset_semaphore = request.Pop<u16>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called mask=0x{:04X}", shared_->preset_semaphore);
    }

    // Outputs: 0: 0x001F0080, 1: result, 2: u8, 0 not inserted, 1 inserted
    void GetHeadphoneStatus(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u8>(0);
    }

    // Inputs:  0: 0x00200040, 1: u8, 0 don't force, 1 force
    void ForceHeadphoneOut(Request& request) {
        const u8 force = request.Pop<u8>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Debug, kLog, "(STUBBED) called, force={}", force);
    }

    std::shared_ptr<DspShared> shared_;
};

// Ten sessions at most (Azahar's DefaultMaxSessions).
MH4N_REGISTER(ServiceFactory, "dsp::DSP",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<DspDsp>(context); },
                             kDefaultMaxSessions));

}  // namespace

}  // namespace mh4n
