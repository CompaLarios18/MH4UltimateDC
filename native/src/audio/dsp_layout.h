// Derived from Azahar src/audio_core/hle/shared_memory.h, src/audio_core/hle/common.h,
// src/audio_core/audio_types.h, src/audio_core/hle/hle.cpp, src/audio_core/hle/decoder.h and
// src/core/hle/service/dsp/dsp_dsp.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// each of them says "Licensed under GPLv2 or any later version"): the two regions of the DSP memory and
// the structures in them (SharedMemory and the offsets of its fields, SourceConfiguration,
// SourceStatus, DspConfiguration, DspStatus, with the middle-endian u32_dsp), the number of sources
// and pipes, the pipe names and the DSP states, the interrupt types, the ticks of an audio frame
// (audio_frame_ticks) and the binary pipe's message (BinaryMessage).
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
// Copyright 2016 Citra Emulator Project (common.h)
// Copyright 2017 Citra Emulator Project (audio_types.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2018-2025 Citra Emulator Project / Azahar Emulator Project (decoder.h)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (dsp_dsp.h)
//
// The memory the application shares with the DSP (N1-18). The DSP memory is 0x80000 bytes at the
// physical address 0x1FF00000; the application sees two regions of 0x8000 bytes in it, at offsets
// 0x50000 and 0x70000, used as a double buffer (the region with the higher frame counter is the one
// the DSP reads; the DSP writes its answer to the other). Both are plain bytes here: the model reads
// and writes fields at the offsets below, never through a structure laid over guest memory, and the
// structures defined here (the source configuration and status) are copied in and out with memcpy.
// The host is little-endian (ipc.h asserts it); u32_dsp values have their 16-bit halves swapped.
#pragma once

#include <cstddef>
#include <cstring>

#include "mh4n/types.h"

namespace mh4n::audio {

// ---- time and sizes ---------------------------------------------------------------------------------

inline constexpr u32 kSamplesPerFrame = 160;
// 160 samples * 4096 teaklite cycles * 2 ARM11 cycles: "verified against a rough hardware test"
// (Azahar's audio_frame_ticks). One audio frame, in ticks of the 268,111,856 Hz clock.
inline constexpr u64 kAudioFrameTicks = kSamplesPerFrame * 4096ull * 2ull;
static_assert(kAudioFrameTicks == 1310720);

inline constexpr u32 kNumSources = 24;
inline constexpr u32 kNumPipes = 8;

// ---- the DSP memory ------------------------------------------------------------------------------------

inline constexpr u32 kDspMemorySize = 0x80000;
inline constexpr u32 kRegionOffset[2] = {0x50000, 0x70000};
inline constexpr u32 kRegionSize = 0x8000;

// The fields of one region (offsets from its start). Azahar's SharedMemory lists them in this order;
// the sizes are those of its structures.
namespace shm {
inline constexpr u32 kDspStatus = 0x400 * 2;                  // 32 bytes
inline constexpr u32 kDspDebug = kDspStatus + 32;             // 0x130 words
inline constexpr u32 kFinalSamples = kDspDebug + 0x130 * 2;   // 160 stereo s16
inline constexpr u32 kSourceStatuses = kFinalSamples + 640;   // 24 * 12 bytes
inline constexpr u32 kCompressor = kSourceStatuses + 24 * 12;
inline constexpr u32 kDspConfiguration = kCompressor + 0xD20 * 2;  // 196 bytes
inline constexpr u32 kIntermediateMixSamples = kDspConfiguration + 196;  // 2 * 2560 bytes
inline constexpr u32 kSourceConfigurations = kIntermediateMixSamples + 5120;  // 24 * 192 bytes
inline constexpr u32 kAdpcmCoefficients = kSourceConfigurations + 24 * 192;   // 768 bytes
inline constexpr u32 kUnknown10 = kAdpcmCoefficients + 768;   // 0x100 words
inline constexpr u32 kUnknown11 = kUnknown10 + 0x100 * 2;     // 0xC0 words
inline constexpr u32 kUnknown12 = kUnknown11 + 0xC0 * 2;      // 0x180 words
inline constexpr u32 kUnknown13 = kUnknown12 + 0x180 * 2;     // 0xA words
inline constexpr u32 kUnknown14 = kUnknown13 + 0xA * 2;       // 0x13A3 words
inline constexpr u32 kFrameCounter = kUnknown14 + 0x13A3 * 2;  // u16, the last of the region
static_assert(kSourceStatuses == 0xD00, "the status blocks are where capfmt.py reads them");
static_assert(kFrameCounter == 0x7FFE && kFrameCounter + 2 == kRegionSize,
              "the frame counter is the last u16 of the region");

inline constexpr u32 kIntermediateMixSize = 2560;  // one of the two mixes (4 channels * 160 * s32)
inline constexpr u32 kFinalSamplesSize = 640;
}  // namespace shm

// ---- pipes, states, interrupts -------------------------------------------------------------------------------

enum class DspPipe : u32 { Debug = 0, Dma = 1, Audio = 2, Binary = 3 };
enum class DspState : u8 { Off, On, Sleeping };

// "There are three types of interrupts" (Service::DSP::InterruptType).
enum class InterruptType : u32 { Zero = 0, One = 1, Pipe = 2, Count };

// ---- the source configuration (192 bytes) ------------------------------------------------------------------------

// The bits of SourceConfiguration::dirty_raw.
namespace dirty {
inline constexpr u32 kFormat = 1u << 0;
inline constexpr u32 kMonoOrStereo = 1u << 1;
inline constexpr u32 kAdpcmCoefficients = 1u << 2;
inline constexpr u32 kPartialEmbeddedBuffer = 1u << 3;
inline constexpr u32 kPartialReset = 1u << 4;
inline constexpr u32 kEnable = 1u << 16;
inline constexpr u32 kInterpolation = 1u << 17;
inline constexpr u32 kRateMultiplier = 1u << 18;
inline constexpr u32 kBufferQueue = 1u << 19;
inline constexpr u32 kLoopRelated = 1u << 20;
inline constexpr u32 kPlayPosition = 1u << 21;
inline constexpr u32 kFiltersEnabled = 1u << 22;
inline constexpr u32 kSimpleFilter = 1u << 23;
inline constexpr u32 kBiquadFilter = 1u << 24;
inline constexpr u32 kGain0 = 1u << 25;
inline constexpr u32 kGain1 = 1u << 26;
inline constexpr u32 kGain2 = 1u << 27;
inline constexpr u32 kSyncCount = 1u << 28;
inline constexpr u32 kReset = 1u << 29;
inline constexpr u32 kEmbeddedBuffer = 1u << 30;
}  // namespace dirty

// The DSP's 32-bit values: the 16-bit halves swapped (Azahar's u32_dsp).
constexpr u32 SwapHalves(u32 value) {
    return (value << 16) | (value >> 16);
}

// One entry of the buffer queue (SourceConfiguration::Configuration::Buffer).
struct QueuedBufferConfig {
    u32 physical_address;  // u32_dsp
    u32 length;            // u32_dsp, in samples
    u16 adpcm_ps;
    u16 adpcm_yn[2];
    u8 adpcm_dirty;
    u8 is_looping;
    u16 buffer_id;
    u8 padding[2];
};
static_assert(sizeof(QueuedBufferConfig) == 20);

// SourceConfiguration::Configuration, field for field. The fields marked u32_dsp are stored with
// their halves swapped (use SwapHalves).
struct SourceConfiguration {
    u32 dirty_raw;
    float gain[3][4];
    float rate_multiplier;
    u8 interpolation_mode;
    u8 padding0;
    u16 filters_enabled;
    s16 simple_filter[2];
    s16 biquad_filter[5];
    u16 buffers_dirty;
    QueuedBufferConfig buffers[4];
    u32 loop_related;  // u32_dsp
    u8 enable;
    u8 padding1;
    u16 sync_count;
    u32 play_position;  // u32_dsp
    u8 padding2[4];
    // The embedded buffer
    u32 physical_address;  // u32_dsp
    u32 length;            // u32_dsp, in samples
    u16 flags1_raw;        // bits 0-1 mono or stereo (1, 2), bits 2-3 format (0 PCM8, 1 PCM16, 2 ADPCM), bit 5 fade in
    u16 adpcm_ps;
    u16 adpcm_yn[2];
    u16 flags2_raw;        // bit 0 adpcm_dirty, bit 1 is_looping
    u16 buffer_id;
};
static_assert(sizeof(SourceConfiguration) == 192);
static_assert(offsetof(SourceConfiguration, rate_multiplier) == 52);
static_assert(offsetof(SourceConfiguration, buffers_dirty) == 74);
static_assert(offsetof(SourceConfiguration, buffers) == 76);
static_assert(offsetof(SourceConfiguration, loop_related) == 156);
static_assert(offsetof(SourceConfiguration, enable) == 160);
static_assert(offsetof(SourceConfiguration, play_position) == 164);
static_assert(offsetof(SourceConfiguration, physical_address) == 172);
static_assert(offsetof(SourceConfiguration, flags1_raw) == 180);
static_assert(offsetof(SourceConfiguration, buffer_id) == 190);

// SourceStatus::Status (12 bytes).
struct SourceStatus {
    u8 is_enabled;
    u8 current_buffer_id_dirty;
    u16 sync_count;
    u32 buffer_position;  // u32_dsp
    u16 current_buffer_id;
    u16 last_buffer_id;
};
static_assert(sizeof(SourceStatus) == 12);

// The fields of DspConfiguration (196 bytes) the model touches: the dirty bits and the two aux bus
// enables, and the delay effects' own dirty words.
namespace dspconfig {
inline constexpr u32 kDirtyRaw = 0;  // u32
inline constexpr u32 kAuxBusEnable0Dirty = 1u << 8;
inline constexpr u32 kAuxBusEnable1Dirty = 1u << 9;
inline constexpr u32 kDelayEffect0Dirty = 1u << 10;
inline constexpr u32 kDelayEffect1Dirty = 1u << 11;
inline constexpr u32 kAuxBusEnable = 40;       // u16[2]
inline constexpr u32 kDelayEffect = 44;        // two DelayEffects of 20 bytes, each starting with a u16 dirty word
inline constexpr u32 kDelayEffectSize = 20;
}  // namespace dspconfig

// ---- the binary pipe -----------------------------------------------------------------------------------------------

// The messages of the binary pipe (32 bytes): a header of the codec, the command and a result, and 24
// bytes whose meaning depends on both.
inline constexpr u16 kCodecDecodeAac = 1;
inline constexpr u16 kCommandInit = 0;
inline constexpr u16 kCommandEncodeDecode = 1;
inline constexpr u32 kResultStatusSuccess = 0;
inline constexpr u32 kResultStatusError = 1;
inline constexpr u32 kBinaryMessageSize = 32;

}  // namespace mh4n::audio
