// Derived from Azahar src/audio_core/hle/hle.cpp, src/audio_core/hle/aac_decoder.cpp,
// src/audio_core/hle/decoder.h and src/audio_core/hle/shared_memory.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or
// any later version"): DspHle::Impl (the state, the pipes, Initialize, Sleep, Wakeup, the region switch
// and GenerateCurrentFrame) and the binary pipe's answers (AACDecoder::ProcessRequest, Decode).
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (aac_decoder.cpp)
// Copyright 2018-2025 Citra Emulator Project / Azahar Emulator Project (decoder.h)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
//
// Our own HLE DSP (N3-03): the object that replaces N1-18's audio::DspModel behind the DSP service
// once it is switched over (N3-11). It has the model's public interface and generates each frame as
// Azahar's DspHle::Impl::GenerateCurrentFrame does: the 24 sources are ticked (audio::kNumSources,
// dsp_hle::Source, N3-02) and mixed into the three intermediate mixes, the mixers read the DSP
// configuration and exchange the auxiliary buses with the shared memory, the effects and the master
// volume are applied, and the 160 stereo samples are written to the final samples of the region the
// DSP writes and handed to the frame output.
//
// The frame reads one region and writes the other: the sources' configurations, the ADPCM
// coefficients, the DSP configuration and the aux buses' samples come from the region with the higher
// frame counter (CurrentRegionIndex); the source statuses, the DSP status, the intermediate mix samples
// of an enabled aux bus and the final samples are written to the other. Everything the application
// shares with the DSP lives in the 0x80000 bytes of DSP memory the constructor was given, as bytes, as
// in the model; nothing is kept apart from the state Azahar keeps apart (the sources, the mixers and
// the effects' buffers).
//
// What it does as the model does (audio::DspModel, dsp_model.h has the full list; the second oracle in
// dsp_hle_test.cpp holds the memory equal outside the final samples and the intermediate mix samples,
// which the model leaves silent): the pipes, Initialize / Shutdown / Wakeup / Sleep with the whole DSP
// memory saved and restored, the structure addresses of the audio pipe, the region rule, and every
// departure of the model: an unknown register or state change is logged, not an assertion. Where it
// differs from the model: it decodes, mixes and writes the sound; it takes the sources' departures of
// N3-02 (source.h, D78) and the mixers' (mixers.h, effects.h); a wakeup without a sleep restores zeros
// (Azahar's backup is uninitialised memory); the padding of the DSP status is left as it is.
//
// The binary pipe answers as the model does and as Azahar's AAC decoder does when a decode fails (a
// message with the sample rate 48000, 2 channels and 1024 samples): nothing is decoded. The first
// decode request logs `dsp: AAC decode requested (not supported)` at Warning; AacDecodeRequests()
// counts them all.
//
// Not thread safe: it belongs to the scheduler thread (principle 6).
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "audio/dsp_layout.h"
#include "dsp_hle/mixers.h"
#include "dsp_hle/source.h"

namespace mh4n::dsp_hle {

class Dsp {
public:
    using InterruptHandler = std::function<void(audio::InterruptType type, audio::DspPipe pipe)>;

    // `memory` is the DSP memory, audio::kDspMemorySize bytes of guest memory, which outlives the DSP.
    explicit Dsp(u8* memory);

    void SetInterruptHandler(InterruptHandler handler) { interrupt_ = std::move(handler); }
    // Called at the start of every frame a running DSP generates, before the frame reads or writes
    // anything, with the DSP memory (--capture-dsp).
    void SetFrameStartHook(std::function<void(const u8* dsp_memory)> hook) {
        frame_start_ = std::move(hook);
    }
    // Where the sources' samples are (guest physical memory). Without one every buffer reads as
    // unmapped memory: zeros. It must outlive the DSP or be replaced by nullptr.
    void SetSampleMemory(SampleMemory* memory) { sample_memory_ = memory; }
    // Called with the 160 stereo samples (320 interleaved s16, left first) of each frame generated, after
    // they are written to the shared memory. The span is valid during the call.
    void SetFrameOutput(std::function<void(std::span<const s16>)> output) {
        frame_output_ = std::move(output);
    }

    audio::DspState State() const { return state_; }

    u16 RecvData(u32 register_number);
    bool RecvDataIsReady(u32 register_number) const;
    void SetSemaphore(u16) {}  // "Do nothing in HLE"

    // `pipe` is the channel number the application gave; one that is not a pipe reads as empty.
    std::vector<u8> PipeRead(u32 pipe, std::size_t length);
    std::size_t PipeReadableSize(u32 pipe) const;
    // Azahar's DspHle::PipeWrite: the audio pipe changes the DSP state, the binary pipe answers.
    void PipeWrite(u32 pipe, std::span<const u8> buffer);

    // One audio frame (Azahar's DspHle::Impl::Tick). False, and nothing happens, while the DSP is not
    // on; true when it generated a frame, after which the caller raises the audio pipe's interrupt.
    bool Tick();

    // The region the DSP reads: the higher frame counter, unless one wrapped around.
    std::size_t CurrentRegionIndex() const;

    // The number of AAC decode requests the binary pipe has answered (with a failure) since the DSP was
    // made.
    u64 AacDecodeRequests() const { return aac_decode_requests_; }

private:
    u8* Region(std::size_t index) const { return memory_ + audio::kRegionOffset[index]; }
    void Initialize();
    void Sleep();
    void Wakeup();
    void ResetPipes();
    void WriteU16(audio::DspPipe pipe, u16 value);
    void AudioPipeWriteStructAddresses();
    void RaiseInterrupt(audio::InterruptType type, audio::DspPipe pipe);
    void GenerateCurrentFrame();
    void WriteBinaryResponse(std::span<const u8> request);

    u8* memory_;
    audio::DspState state_ = audio::DspState::Off;
    std::array<std::vector<u8>, audio::kNumPipes> pipes_{};
    std::array<Source, kNumSources> sources_{};
    Mixers mixers_;
    std::vector<u8> backup_memory_;  // empty reads as zeros
    SampleMemory* sample_memory_ = nullptr;
    InterruptHandler interrupt_;
    std::function<void(const u8*)> frame_start_;
    std::function<void(std::span<const s16>)> frame_output_;
    u64 aac_decode_requests_ = 0;
};

}  // namespace mh4n::dsp_hle
