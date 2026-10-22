// Derived from Azahar src/audio_core/hle/source.h, src/audio_core/hle/source.cpp and
// src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each
// of them says "Licensed under GPLv2 or any later version"): Source (its state, the buffer queue and its
// order, Tick, MixInto, ParseConfig, GenerateFrame, DequeueBuffer, GetCurrentStatus, Reset, Sleep and
// Wakeup) and AdpcmCoefficients.
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (source.h, source.cpp)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
//
// One source of the HLE DSP (N3-02): it dequeues its buffers, decodes PCM8, PCM16 and ADPCM,
// interpolates at its rate, filters and mixes into the intermediate mixes, as Azahar's Source does,
// with the status rules of N1-18's audio::SourceModel (native/src/audio/dsp_model.h), which it replaces
// once the service switches over (N3-11). The model counts samples; this decodes them.
//
// What it does as Azahar does (docs/native/snd-tests.md, "Things Azahar does"): the first frame after
// a source is enabled only dequeues and is silent; polyphase plays as linear; the queue is ordered by
// buffer id, the embedded buffer included; a partial embedded update is ignored for ADPCM and PCM8 and
// honoured for PCM16 (a shrink below the play position resets it to 0); ADPCM history is taken only when
// the buffer's dirty flag is set; the buffer's own ps is never read; rate 0 becomes 1.0; an unmapped
// buffer address stops the source; address low bits are masked; disabling a filter resets its
// coefficients; a looping PCM16 buffer's not-yet-consumed part is re-read from memory every frame.
//
// Departures from Azahar, all deliberate (they are those of the model, see dsp_model.h; the DSP memory
// belongs to a game that must keep running):
//  - Where Azahar asserts (a NaN rate multiplier, which passes its `<= 0` test and then fails
//    ASSERT(rate > 0); an interpolation mode of 3, for which it logs and then loops for ever), this logs
//    the error and goes on: a NaN rate becomes 1.0 as a rate of 0 does, mode 3 plays as linear.
//    UNIMPLEMENTED and UNIMPLEMENTED_MSG only log in Azahar and carry on, and so does this (a partial
//    update of PCM8 or ADPCM, a format value of 3: the buffer is left empty).
//  - A source stops dequeueing buffers for a frame after audio::kMaxDequeuesPerFrame, with the rest of
//    the frame unfilled and an error in the log (Azahar loops for ever on a looping buffer of length 0).
//  - A play position beyond the end of a buffer empties it (Azahar erases past the end).
//  - A buffer's address is valid when audio::IsPhysicalAddressValid says so (Azahar's
//    GetPhysMemRegionInfo: VRAM, DSP RAM, FCRAM of the New 3DS's size and the New 3DS's extra RAM, ends
//    included). Inside such an area what the SampleMemory cannot supply (an empty or short span: memory
//    that is not backed, the part of a New 3DS area an Old 3DS lacks) reads as zeros.
//  - A buffer whose bytes in memory (BufferByteExtent) are more than kMaxBufferBytes, 256 MiB, the New
//    3DS's whole FCRAM, is not decoded: it is dropped with an error as one at an invalid address is. No
//    memory area holds such a buffer, so Azahar can only read it by going out of bounds (it allocates up
//    to 16 GB for a length of 2^31 - 1 and reads past its memory). Every buffer that fits is played.
//  - The arithmetic that overflows in Azahar's C++ (the filters' and ADPCM's sums with extreme
//    coefficients) wraps in 32 bits, as its compiled code does.
// The queue is a std::priority_queue ordered by buffer id as Azahar's, so two buffers with one id play
// in the order the standard library's heap gives (as in Azahar; the oracle cases do not depend on it).
#pragma once

#include <array>
#include <cstddef>
#include <queue>
#include <span>
#include <vector>

#include "audio/dsp_layout.h"
#include "dsp_hle/codec.h"
#include "dsp_hle/filter.h"
#include "dsp_hle/interpolate.h"
#include "dsp_hle/types.h"

namespace mh4n::dsp_hle {

// Where a buffer's samples are: guest physical memory (Azahar's MemorySystem::GetPhysicalPointer).
struct SampleMemory {
    virtual ~SampleMemory() = default;
    // The `size` bytes at `physical_address`; an empty span, or one shorter than `size`, when the
    // memory is not mapped (the missing bytes read as zeros). It is valid until the next call.
    virtual std::span<const u8> Read(u32 physical_address, u32 size) = 0;
};

// The ADPCM coefficients of all sources, 16 per source: the table in the shared memory (768 bytes at
// audio::shm::kAdpcmCoefficients), s16 little-endian.
struct AdpcmCoefficients {
    s16 coeff[kNumSources][16];
};
static_assert(sizeof(AdpcmCoefficients) == 768);

// The bytes a buffer of `length` samples occupies in memory: PCM8 one or two bytes a sample, PCM16 two
// or four, ADPCM 8 bytes for each 14 samples (mono_or_stereo is the field's value, 2 is stereo, the
// others mono; a format of 3 has no bytes).
u64 BufferByteExtent(u8 format, u8 mono_or_stereo, u32 length);

// The largest buffer that is decoded, in bytes of memory: more than any memory area holds (the New
// 3DS's FCRAM is the largest).
inline constexpr u64 kMaxBufferBytes = 0x10000000;

class Source final {
public:
    Source() { Reset(); }

    // Resets the internal state.
    void Reset();
    void Sleep();
    void Wakeup();

    // Called once every audio frame: reads the new configuration from the application (clearing its
    // dirty bits in `config`, as the DSP does), runs the frame and returns the status for the
    // application. `coefficients` is the table of all sources: this source's row is `source_id`'s (an id
    // of kNumSources or more has zeros).
    audio::SourceStatus Tick(audio::SourceConfiguration& config, const AdpcmCoefficients& coefficients,
                             SampleMemory& memory, u32 source_id);

    // Mixes this source's output of the last Tick into `dest`, with the gains of the `mix`-th
    // intermediate mix (0 to 2). A mix of 3 or more adds nothing. The gain ramp's state advances, which
    // is why it is mutable: the declaration is const as N3-03's mixer holds the sources.
    void MixInto(QuadFrame32& dest, std::size_t mix) const;

private:
    enum Format : u8 { kPcm8 = 0, kPcm16 = 1, kAdpcm = 2 };
    enum InterpolationMode : u8 { kPolyphase = 0, kLinear = 1, kNone = 2 };

    // Internal representation of a buffer for our buffer queue.
    struct Buffer {
        u32 physical_address;
        u32 length;
        u8 adpcm_ps;
        std::array<u16, 2> adpcm_yn;
        bool adpcm_dirty;
        bool is_looping;
        u16 buffer_id;

        u8 mono_or_stereo;
        u8 format;

        bool from_queue;
        u32 play_position;
        bool has_played;
    };

    struct BufferOrder {
        bool operator()(const Buffer& a, const Buffer& b) const {
            // Lower buffer_id comes first.
            return a.buffer_id > b.buffer_id;
        }
    };

    struct State {
        // State variables
        bool enabled = false;
        u16 sync_count = 0;

        // Mixing
        std::array<std::array<float, 4>, 3> gain = {};

        // Buffer queue
        std::priority_queue<Buffer, std::vector<Buffer>, BufferOrder> input_queue = {};
        u8 mono_or_stereo = 1;  // mono
        u8 format = kAdpcm;

        // Current buffer
        u32 current_sample_number = 0;
        u32 current_buffer_physical_address = 0;
        u32 current_buffer_length = 0;
        u8 current_buffer_mono_or_stereo = 1;
        u8 current_buffer_format = kAdpcm;
        bool current_buffer_is_looping = false;
        interp::StereoBuffer current_buffer = {};

        // buffer_id state
        bool buffer_update = false;
        u16 last_buffer_id = 0;
        u16 current_buffer_id = 0;

        // Decoding state
        std::array<s16, 16> adpcm_coeffs = {};
        codec::AdpcmState adpcm_state = {};

        // Resampling state
        bool mode_warned = false;  // the log says once that the interpolation mode is not one
        float rate_multiplier = 1.0f;
        u8 interpolation_mode = kPolyphase;
        interp::State interp_state = {};

        // Filter state
        SourceFilters filters = {};
    };

    // The gain ramp of each mix: while a gain was changed in the last Tick, the first frame ramps from
    // the old gains to the new over the frame (MixInto advances it).
    struct GainRamp {
        std::array<std::array<float, 4>, 3> start = {};
        std::array<bool, 3> active = {};
    };

    void ParseConfig(audio::SourceConfiguration& config, const AdpcmCoefficients& coefficients,
                     SampleMemory& memory, u32 source_id);
    void GenerateFrame(SampleMemory& memory, u32 source_id);
    bool DequeueBuffer(SampleMemory& memory, u32 source_id);
    audio::SourceStatus GetCurrentStatus();

    StereoFrame16 current_frame_{};
    StereoFrame16 backup_frame_{};
    State state_;
    State backup_state_;
    mutable GainRamp ramp_;
    GainRamp backup_ramp_;
};

}  // namespace mh4n::dsp_hle
