// Derived from Azahar src/audio_core/hle/source.cpp, src/audio_core/hle/source.h,
// src/audio_core/hle/shared_memory.h, src/audio_core/hle/filter.h, src/audio_core/codec.h and
// src/audio_core/interpolate.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them
// says "Licensed under GPLv2 or any later version"): Source::Tick, MixInto, Reset, Sleep, Wakeup,
// ParseConfig, GenerateFrame, DequeueBuffer and GetCurrentStatus.
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (source.cpp, source.h)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
// Copyright 2016 Citra Emulator Project (filter.h, codec.h, interpolate.h)
//
// One source of the HLE DSP (N3-02); see source.h.
#include "dsp_hle/source.h"

#include <algorithm>

#include "audio/dsp_model.h"
#include "mh4n/log.h"

namespace mh4n::dsp_hle {

namespace {

constexpr const char* kLog = "Audio.DSP.HLE";

constexpr u32 kMaskAddress = 0xFFFFFFFCu;

unsigned NumChannels(u8 mono_or_stereo) {
    return mono_or_stereo == 2 ? 2 : 1;  // MonoOrStereo::Stereo
}

}  // namespace

u64 BufferByteExtent(u8 format, u8 mono_or_stereo, u32 length) {
    switch (format) {
    case 0:  // PCM8
        return codec::Pcm8Bytes(NumChannels(mono_or_stereo), length);
    case 1:  // PCM16
        return codec::Pcm16Bytes(NumChannels(mono_or_stereo), length);
    case 2:  // ADPCM
        return codec::AdpcmBytes(length);
    default:
        return 0;
    }
}

audio::SourceStatus Source::Tick(audio::SourceConfiguration& config,
                                 const AdpcmCoefficients& coefficients, SampleMemory& memory,
                                 u32 source_id) {
    ParseConfig(config, coefficients, memory, source_id);

    if (state_.enabled) {
        GenerateFrame(memory, source_id);
    }

    return GetCurrentStatus();
}

void Source::MixInto(QuadFrame32& dest, std::size_t intermediate_mix_id) const {
    if (intermediate_mix_id >= state_.gain.size()) {
        MH4N_LOG(Error, kLog, "MixInto: intermediate mix {} does not exist", intermediate_mix_id);
        return;
    }
    const std::array<float, 4>& gains = state_.gain[intermediate_mix_id];
    if (!state_.enabled) {
        ramp_.start[intermediate_mix_id] = gains;
        ramp_.active[intermediate_mix_id] = false;
        return;
    }

    const bool ramp_active = ramp_.active[intermediate_mix_id];
    const std::array<float, 4>& ramp_start = ramp_.start[intermediate_mix_id];
    constexpr float ramp_scale = 1.0f / static_cast<float>(kSamplesPerFrame - 1);

    for (std::size_t samplei = 0; samplei < kSamplesPerFrame; samplei++) {
        const float progress = static_cast<float>(samplei) * ramp_scale;
        const float gain0 =
            ramp_active ? ramp_start[0] + (gains[0] - ramp_start[0]) * progress : gains[0];
        const float gain1 =
            ramp_active ? ramp_start[1] + (gains[1] - ramp_start[1]) * progress : gains[1];
        const float gain2 =
            ramp_active ? ramp_start[2] + (gains[2] - ramp_start[2]) * progress : gains[2];
        const float gain3 =
            ramp_active ? ramp_start[3] + (gains[3] - ramp_start[3]) * progress : gains[3];

        // Conversion from stereo (current_frame) to quadraphonic (dest) occurs here.
        dest[samplei][0] += static_cast<s32>(gain0 * current_frame_[samplei][0]);
        dest[samplei][1] += static_cast<s32>(gain1 * current_frame_[samplei][1]);
        dest[samplei][2] += static_cast<s32>(gain2 * current_frame_[samplei][0]);
        dest[samplei][3] += static_cast<s32>(gain3 * current_frame_[samplei][1]);
    }

    if (ramp_active) {
        ramp_.start[intermediate_mix_id] = gains;
        ramp_.active[intermediate_mix_id] = false;
    }
}

void Source::Reset() {
    current_frame_.fill({});
    state_ = {};
    ramp_ = {};
}

void Source::Sleep() {
    backup_frame_ = current_frame_;
    backup_state_ = state_;
    backup_ramp_ = ramp_;
}

void Source::Wakeup() {
    current_frame_ = backup_frame_;
    state_ = backup_state_;
    ramp_ = backup_ramp_;
    backup_frame_.fill({});
    backup_state_ = {};
    backup_ramp_ = {};
}

void Source::ParseConfig(audio::SourceConfiguration& config, const AdpcmCoefficients& coefficients,
                         SampleMemory& memory, u32 source_id) {
    // Azahar clears each dirty bit as it handles it and the rest at the end; the conditions read
    // bits nothing has cleared yet, so the value at entry is the one to test.
    const u32 bits = config.dirty_raw;
    if (bits == 0) {
        return;
    }
    State& s = state_;

    if (bits & audio::dirty::kReset) {
        Reset();
        MH4N_LOG(Trace, kLog, "source_id={} reset", source_id);
    }

    if (bits & audio::dirty::kPartialReset) {
        s.input_queue = {};
        MH4N_LOG(Trace, kLog, "source_id={} partial_reset", source_id);
    }

    if (bits & audio::dirty::kEnable) {
        s.enabled = config.enable != 0;
        MH4N_LOG(Trace, kLog, "source_id={} enable={}", source_id, s.enabled);
    }

    if (bits & audio::dirty::kSyncCount) {
        s.sync_count = config.sync_count;
        MH4N_LOG(Trace, kLog, "source_id={} sync={}", source_id, s.sync_count);
    }

    if (bits & audio::dirty::kRateMultiplier) {
        s.rate_multiplier = config.rate_multiplier;
        MH4N_LOG(Trace, kLog, "source_id={} rate={}", source_id, s.rate_multiplier);

        // Azahar tests `<= 0`; a NaN passes that test and then fails an assertion in the
        // interpolation, so it is as invalid as a zero here.
        if (!(s.rate_multiplier > 0)) {
            MH4N_LOG(Error, kLog, "Was given an invalid rate multiplier: source_id={} rate={}",
                     source_id, s.rate_multiplier);
            s.rate_multiplier = 1.0f;
            // Note: Actual firmware starts producing garbage if this occurs (Azahar's comment).
        }
    }

    if (bits & audio::dirty::kAdpcmCoefficients) {
        if (source_id < kNumSources) {
            std::copy(std::begin(coefficients.coeff[source_id]), std::end(coefficients.coeff[source_id]),
                      s.adpcm_coeffs.begin());
        } else {
            s.adpcm_coeffs.fill(0);
        }
        MH4N_LOG(Trace, kLog, "source_id={} adpcm update", source_id);
    }

    // Gain: the ramp starts from the gains in force and runs over the next frame's mix.
    constexpr u32 kGainDirty[3] = {audio::dirty::kGain0, audio::dirty::kGain1, audio::dirty::kGain2};
    for (std::size_t mix = 0; mix < 3; ++mix) {
        if (bits & kGainDirty[mix]) {
            ramp_.start[mix] = s.gain[mix];
            ramp_.active[mix] = true;
            for (std::size_t i = 0; i < 4; ++i) {
                s.gain[mix][i] = config.gain[mix][i];
            }
            MH4N_LOG(Trace, kLog, "source_id={} gain {} update", source_id, mix);
        }
    }

    if (bits & audio::dirty::kFiltersEnabled) {
        s.filters.Enable((config.filters_enabled & 1) != 0, (config.filters_enabled & 2) != 0);
        MH4N_LOG(Trace, kLog, "source_id={} enable_simple={} enable_biquad={}", source_id,
                 config.filters_enabled & 1, (config.filters_enabled >> 1) & 1);
    }

    if (bits & audio::dirty::kSimpleFilter) {
        s.filters.ConfigureSimple(config.simple_filter);
        MH4N_LOG(Trace, kLog, "source_id={} simple filter update", source_id);
    }

    if (bits & audio::dirty::kBiquadFilter) {
        s.filters.ConfigureBiquad(config.biquad_filter);
        MH4N_LOG(Trace, kLog, "source_id={} biquad filter update", source_id);
    }

    if (bits & audio::dirty::kInterpolation) {
        s.interpolation_mode = config.interpolation_mode;
        MH4N_LOG(Trace, kLog, "source_id={} interpolation_mode={}", source_id, s.interpolation_mode);
    }

    if (bits & (audio::dirty::kFormat | audio::dirty::kEmbeddedBuffer)) {
        s.format = static_cast<u8>((config.flags1_raw >> 2) & 3);
        MH4N_LOG(Trace, kLog, "source_id={} format={}", source_id, s.format);
    }

    if (bits & (audio::dirty::kMonoOrStereo | audio::dirty::kEmbeddedBuffer)) {
        s.mono_or_stereo = static_cast<u8>(config.flags1_raw & 3);
        MH4N_LOG(Trace, kLog, "source_id={} mono_or_stereo={}", source_id, s.mono_or_stereo);
    }

    // play_position applies only to the embedded buffer, and defaults to 0 without a dirty bit.
    // This will be the starting sample for the first time the buffer is played.
    u32 play_position = 0;
    if (bits & audio::dirty::kPlayPosition) {
        play_position = audio::SwapHalves(config.play_position);
    }

    if (bits & audio::dirty::kPartialEmbeddedBuffer) {
        // As this bit is set by the game, three config options are also updated: buffer_id,
        // flags2_raw.is_looping, and length.
        //
        // A quick and dirty way of extending the current buffer is to just read the whole thing again
        // with the new length. Note that this uses the latched physical address instead of whatever is
        // in config, because that may be invalid.
        const u32 length = audio::SwapHalves(config.length);
        if (audio::IsPhysicalAddressValid(s.current_buffer_physical_address & kMaskAddress)) {
            const unsigned num_channels = NumChannels(s.mono_or_stereo);
            bool valid = false;
            switch (s.format) {
            case kPcm8:
                // Azahar: UNIMPLEMENTED_MSG("PCM8 not handled for partial buffer updates"), and goes on.
                MH4N_LOG(Error, kLog, "source_id={}: PCM8 is not handled for partial buffer updates",
                         source_id);
                break;
            case kPcm16:
                if (BufferByteExtent(kPcm16, s.mono_or_stereo, length) > kMaxBufferBytes) {
                    MH4N_LOG(Error, kLog, "source_id={}: a partial update of {} samples is too long",
                             source_id, length);
                    break;
                }
                s.current_buffer.assign(codec::DecodePcm16(
                    num_channels,
                    memory.Read(s.current_buffer_physical_address & kMaskAddress,
                                static_cast<u32>(codec::Pcm16Bytes(num_channels, length))),
                    length));
                s.current_buffer_length = length;
                s.current_buffer_mono_or_stereo = s.mono_or_stereo;
                s.current_buffer_format = s.format;
                s.current_buffer_is_looping = (config.flags2_raw & 2) != 0;
                valid = true;
                break;
            case kAdpcm:
                MH4N_LOG(Error, kLog, "source_id={}: ADPCM is not handled for partial buffer updates",
                         source_id);
                break;
            default:
                MH4N_LOG(Error, kLog, "source_id={}: format {} is not handled for partial buffer updates",
                         source_id, s.format);
                break;
            }

            // Again, because our interpolation consumes samples instead of using an index, let's just
            // re-consume the samples up to the current sample number.
            if (valid) {
                // "Tomodachi life apparently can decrease config.length when the user skips dialog. I
                // don't know the correct behavior, but to avoid crashing, just reset the current
                // sample number to 0 and don't try to truncate the buffer" (Azahar).
                if (s.current_buffer.size() < s.current_sample_number) {
                    s.current_sample_number = 0;
                } else {
                    s.current_buffer.EraseFront(s.current_sample_number);
                }
            }
        }
        MH4N_LOG(Trace, kLog, "source_id={} partially updating embedded buffer addr={:#010x} len={}",
                 source_id, s.current_buffer_physical_address, length);
    }

    if (bits & audio::dirty::kEmbeddedBuffer) {
        const u32 length = audio::SwapHalves(config.length);
        // Azahar's hack for Luigi's Mansion Dark Moon, which passes an underflowed length (the
        // length is a u32_dsp read as signed).
        if (static_cast<s32>(length) < 0) {
            MH4N_LOG(Error, kLog,
                     "Skipping embedded buffer sample! Application passed in improper value for "
                     "length. addr {:X} length {:X}",
                     audio::SwapHalves(config.physical_address), length);
        } else {
            s.input_queue.push(Buffer{
                audio::SwapHalves(config.physical_address),
                length,
                static_cast<u8>(config.adpcm_ps),
                {config.adpcm_yn[0], config.adpcm_yn[1]},
                (config.flags2_raw & 1) != 0,
                (config.flags2_raw & 2) != 0,
                config.buffer_id,
                s.mono_or_stereo,
                s.format,
                false,
                play_position,
                false,
            });
        }
        MH4N_LOG(Trace, kLog, "enqueuing embedded addr={:#010x} len={} id={} start={}",
                 audio::SwapHalves(config.physical_address), length, config.buffer_id, play_position);
    }

    if ((bits & audio::dirty::kLoopRelated) && audio::SwapHalves(config.loop_related) != 0) {
        MH4N_LOG(Warning, kLog, "Unhandled complex loop with loop_related={:#010x}",
                 audio::SwapHalves(config.loop_related));
    }

    if (bits & audio::dirty::kBufferQueue) {
        for (std::size_t i = 0; i < 4; i++) {
            if (!(config.buffers_dirty & (1u << i))) {
                continue;
            }
            const audio::QueuedBufferConfig& b = config.buffers[i];
            const u32 length = audio::SwapHalves(b.length);
            if (static_cast<s32>(length) < 0) {
                MH4N_LOG(Error, kLog,
                         "Skipping buffer queue sample! Game passed in improper value for length. "
                         "addr {:X} length {:X}",
                         audio::SwapHalves(b.physical_address), length);
            } else {
                s.input_queue.push(Buffer{
                    audio::SwapHalves(b.physical_address),
                    length,
                    static_cast<u8>(b.adpcm_ps),
                    {b.adpcm_yn[0], b.adpcm_yn[1]},
                    b.adpcm_dirty != 0,
                    b.is_looping != 0,
                    b.buffer_id,
                    s.mono_or_stereo,
                    s.format,
                    true,   // from_queue
                    0,      // play_position
                    false,  // has_played
                });
            }
            MH4N_LOG(Trace, kLog, "enqueuing queued {} addr={:#010x} len={} id={}", i,
                     audio::SwapHalves(b.physical_address), length, b.buffer_id);
        }
        config.buffers_dirty = 0;
    }

    config.dirty_raw = 0;
}

void Source::GenerateFrame(SampleMemory& memory, u32 source_id) {
    State& s = state_;
    current_frame_.fill({});

    // A looping PCM buffer can be updated by the application while the DSP is playing it. Refresh
    // only the not-yet-consumed part from memory. current_sample_number is maintained below from the
    // exact amount of input removed by the interpolation, so the memory offset and current_buffer stay
    // in sync (Azahar's comment).
    if (!s.current_buffer.empty() && s.current_buffer_is_looping && s.current_buffer_format == kPcm16 &&
        s.current_buffer_physical_address != 0 && s.current_sample_number < s.current_buffer_length) {
        const unsigned num_channels = NumChannels(s.current_buffer_mono_or_stereo);
        const u32 byte_offset = s.current_sample_number * num_channels * static_cast<u32>(sizeof(s16));
        const u32 physical_address = (s.current_buffer_physical_address & kMaskAddress) + byte_offset;
        if (audio::IsPhysicalAddressValid(physical_address)) {
            const u32 remaining_samples = s.current_buffer_length - s.current_sample_number;
            s.current_buffer.assign(codec::DecodePcm16(
                num_channels,
                memory.Read(physical_address, static_cast<u32>(codec::Pcm16Bytes(num_channels, remaining_samples))),
                remaining_samples));
        }
    }

    if (s.current_buffer.empty()) {
        // TODO(SachinV): Should dequeue happen at the end of the frame generation? (Azahar)
        if (DequeueBuffer(memory, source_id)) {
            return;
        }
        s.enabled = false;
        s.buffer_update = true;
        s.last_buffer_id = s.current_buffer_id;
        s.current_buffer_id = 0;
        return;
    }

    std::size_t frame_position = 0;
    u32 dequeues = 0;
    while (frame_position < current_frame_.size()) {
        if (s.current_buffer.empty()) {
            if (dequeues++ >= audio::kMaxDequeuesPerFrame) {
                // Azahar goes on for ever with a looping buffer of length 0, and goes on longer than
                // this cap with a short looping buffer at a high rate (see dsp_model.h).
                MH4N_LOG(Error, kLog,
                         "source_id={}: {} buffers dequeued in one frame without filling it; the "
                         "frame ends here",
                         source_id, audio::kMaxDequeuesPerFrame);
                break;
            }
            if (!DequeueBuffer(memory, source_id)) {
                break;
            }
        }

        // The interpolation consumes samples from current_buffer and already preserves its own
        // fractional phase/history. Use what it actually consumed instead of reconstructing the source
        // position with a truncated float product (Azahar's comment).
        const std::size_t input_size_before = s.current_buffer.size();

        switch (s.interpolation_mode) {
        case kNone:
            interp::None(s.interp_state, s.current_buffer, s.rate_multiplier, current_frame_,
                         frame_position);
            break;
        case kLinear:
            interp::Linear(s.interp_state, s.current_buffer, s.rate_multiplier, current_frame_,
                           frame_position);
            break;
        case kPolyphase:
            // TODO(merry): Implement polyphase interpolation (Azahar's)
            interp::Linear(s.interp_state, s.current_buffer, s.rate_multiplier, current_frame_,
                           frame_position);
            break;
        default:
            // Azahar's UNIMPLEMENTED() logs and goes round again for ever; this plays as linear.
            if (!s.mode_warned) {
                s.mode_warned = true;
                MH4N_LOG(Error, kLog, "source_id={}: interpolation mode {} is not implemented; linear",
                         source_id, s.interpolation_mode);
            }
            interp::Linear(s.interp_state, s.current_buffer, s.rate_multiplier, current_frame_,
                           frame_position);
            break;
        }

        const std::size_t input_size_after = s.current_buffer.size();
        s.current_sample_number += static_cast<u32>(input_size_before - input_size_after);
    }

    s.filters.ProcessFrame(current_frame_);
}

bool Source::DequeueBuffer(SampleMemory& memory, u32 source_id) {
    State& s = state_;
    // Azahar: ASSERT_MSG(current_buffer.empty(), "Shouldn't dequeue; we still have data").
    // Both callers dequeue only when it is empty.
    s.current_buffer.clear();

    if (s.input_queue.empty()) {
        return false;
    }

    Buffer buf = s.input_queue.top();
    s.input_queue.pop();

    if (buf.adpcm_dirty) {
        s.adpcm_state.yn1 = static_cast<s16>(buf.adpcm_yn[0]);
        s.adpcm_state.yn2 = static_cast<s16>(buf.adpcm_yn[1]);
    }

    // This physical address masking occurs due to how the DSP DMA hardware is configured by the
    // firmware.
    const u32 address = buf.physical_address & kMaskAddress;
    const bool valid_address = audio::IsPhysicalAddressValid(address);
    if (!valid_address ||
        BufferByteExtent(buf.format, buf.mono_or_stereo, buf.length) > kMaxBufferBytes) {
        if (!valid_address) {
            MH4N_LOG(Warning, kLog,
                     "source_id={} buffer_id={} length={}: Invalid physical address {:#010x}",
                     source_id, buf.buffer_id, buf.length, buf.physical_address);
        } else {
            MH4N_LOG(Error, kLog, "source_id={} buffer_id={}: {} samples is too long", source_id,
                     buf.buffer_id, buf.length);
        }
        s.current_buffer.clear();
        return true;
    }

    const unsigned num_channels = NumChannels(buf.mono_or_stereo);
    switch (buf.format) {
    case kPcm8:
        s.current_buffer.assign(codec::DecodePcm8(
            num_channels, memory.Read(address, static_cast<u32>(codec::Pcm8Bytes(num_channels, buf.length))),
            buf.length));
        break;
    case kPcm16:
        s.current_buffer.assign(codec::DecodePcm16(
            num_channels, memory.Read(address, static_cast<u32>(codec::Pcm16Bytes(num_channels, buf.length))),
            buf.length));
        break;
    case kAdpcm:
        s.current_buffer.assign(codec::DecodeAdpcm(
            memory.Read(address, static_cast<u32>(codec::AdpcmBytes(buf.length))), buf.length, s.adpcm_coeffs,
            s.adpcm_state));
        break;
    default:
        // Azahar's UNIMPLEMENTED() logs and leaves the buffer empty, as here.
        MH4N_LOG(Error, kLog, "source_id={} buffer_id={}: unknown format {}", source_id, buf.buffer_id,
                 buf.format);
        break;
    }

    // The first playthrough starts at play_position, loops start at the beginning of the buffer.
    s.current_sample_number = !buf.has_played ? buf.play_position : 0;
    s.current_buffer_physical_address = buf.physical_address;
    s.current_buffer_length = buf.length;
    s.current_buffer_mono_or_stereo = buf.mono_or_stereo;
    s.current_buffer_format = buf.format;
    s.current_buffer_is_looping = buf.is_looping;
    s.current_buffer_id = buf.buffer_id;
    s.last_buffer_id = 0;
    s.buffer_update = buf.from_queue && !buf.has_played;

    if (buf.is_looping) {
        buf.has_played = true;
        s.input_queue.push(buf);
    }

    // Because our interpolation consumes samples instead of using an index, let's just consume the
    // samples up to the current sample number (a position past the end empties the buffer).
    s.current_buffer.EraseFront(s.current_sample_number);

    MH4N_LOG(Trace, kLog,
             "source_id={} buffer_id={} from_queue={} current_buffer.size()={}, buf.has_played={}, "
             "buf.play_position={}",
             source_id, buf.buffer_id, buf.from_queue, s.current_buffer.size(), buf.has_played,
             buf.play_position);
    return true;
}

audio::SourceStatus Source::GetCurrentStatus() {
    audio::SourceStatus ret{};

    // Applications depend on the correct emulation of current_buffer_id_dirty and current_buffer_id
    // to synchronise audio with video (Azahar's comment).
    ret.is_enabled = state_.enabled ? 1 : 0;
    ret.current_buffer_id_dirty = state_.buffer_update ? 1 : 0;
    state_.buffer_update = false;
    ret.sync_count = state_.sync_count;
    ret.buffer_position = audio::SwapHalves(state_.current_sample_number);
    ret.current_buffer_id = state_.current_buffer_id;
    ret.last_buffer_id = state_.last_buffer_id;

    return ret;
}

}  // namespace mh4n::dsp_hle
