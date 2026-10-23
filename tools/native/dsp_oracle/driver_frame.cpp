// Derived from Azahar src/audio_core/hle/hle.cpp, src/audio_core/hle/shared_memory.h,
// src/audio_core/hle/mixers.h and src/audio_core/hle/source.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or any later version"): DspHle::Impl::CurrentRegionIndex, ReadRegion, WriteRegion, GenerateCurrentFrame, Initialize, Sleep and Wakeup (copied, with the Impl's members as local objects instead of compiling hle.cpp, which needs the whole DspHle: the memory system, the core timing and the AAC decoder), and the layouts of SharedMemory and DspMemory.
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (mixers.h, source.h)
//
// The frame cases of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py, N3-03): the whole frame on
// Azahar's own code. The driver plays the application: before each frame it writes the case's events into
// the region the DSP reads next (the one it wrote in the frame before), sets that region's frame counter
// one above the other's, lets Azahar's GenerateCurrentFrame run, and prints four CRC-32:
//   final samples        the 160 stereo s16 the frame wrote to the write region
//   statuses             the 24 source status blocks and the first 4 bytes of the DSP status (its 28
//                        padding bytes are indeterminate in Azahar: an uninitialised local)
//   mix buses            the intermediate mix samples of the write region (an enabled aux bus's)
//   configuration        the read region's 24 source configurations and DSP configuration (the dirty
//                        bits the frame cleared)
// A frame in which the DSP is asleep prints zeros. No value is computed here but the CRCs.
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "audio_core/hle/mixers.h"
#include "audio_core/hle/shared_memory.h"
#include "audio_core/hle/source.h"
#include "core/memory.h"
#include "driver.h"

namespace {

namespace HLE = AudioCore::HLE;
using AudioCore::StereoFrame16;
using AudioCore::QuadFrame32;

[[noreturn]] void Die(const char* what) {
    std::fprintf(stderr, "driver_frame: %s\n", what);
    std::exit(2);
}

// The Impl's state (hle.cpp), as plain members.
struct Dsp {
    Memory::MemorySystem memory;
    std::unique_ptr<HLE::DspMemory> dsp_memory = std::make_unique<HLE::DspMemory>();
    HLE::DspMemory backup_dsp_memory;
    std::array<HLE::Source, HLE::num_sources> sources;
    HLE::Mixers mixers{};
    bool on = true;  // the application has initialised it (Initialize on zeroed memory changes nothing)

    template <std::size_t... I>
    static std::array<HLE::Source, HLE::num_sources> MakeSources(std::index_sequence<I...>) {
        return {{HLE::Source(I)...}};
    }

    Dsp() : sources(MakeSources(std::make_index_sequence<HLE::num_sources>{})) {
        dsp_memory->raw_memory.fill(0);
        backup_dsp_memory.raw_memory.fill(0);
        for (auto& source : sources) {
            source.SetMemory(memory);
        }
    }

    std::size_t CurrentRegionIndex() const {
        // The region with the higher frame counter is chosen unless there is wraparound.
        // This function only returns a 0 or 1.
        const u16 frame_counter_0 = dsp_memory->region_0.frame_counter;
        const u16 frame_counter_1 = dsp_memory->region_1.frame_counter;

        if (frame_counter_0 == 0xFFFFu && frame_counter_1 != 0xFFFEu) {
            // Wraparound has occurred.
            return 1;
        }

        if (frame_counter_1 == 0xFFFFu && frame_counter_0 != 0xFFFEu) {
            // Wraparound has occurred.
            return 0;
        }

        return (frame_counter_0 > frame_counter_1) ? 0 : 1;
    }

    HLE::SharedMemory& ReadRegion() {
        return CurrentRegionIndex() == 0 ? dsp_memory->region_0 : dsp_memory->region_1;
    }

    HLE::SharedMemory& WriteRegion() {
        return CurrentRegionIndex() != 0 ? dsp_memory->region_0 : dsp_memory->region_1;
    }

    StereoFrame16 GenerateCurrentFrame() {
        HLE::SharedMemory& read = ReadRegion();
        HLE::SharedMemory& write = WriteRegion();

        std::array<QuadFrame32, 3> intermediate_mixes = {};

        // Generate intermediate mixes
        for (std::size_t i = 0; i < HLE::num_sources; i++) {
            write.source_statuses.status[i] =
                sources[i].Tick(read.source_configurations.config[i], read.adpcm_coefficients.coeff[i]);
            for (std::size_t mix = 0; mix < 3; mix++) {
                sources[i].MixInto(intermediate_mixes[mix], mix);
            }
        }

        // Generate final mix
        write.dsp_status = mixers.Tick(read.dsp_configuration, read.intermediate_mix_samples,
                                       write.intermediate_mix_samples, intermediate_mixes);

        StereoFrame16 output_frame = mixers.GetOutput();

        // Write current output frame to the shared memory region
        for (std::size_t samplei = 0; samplei < output_frame.size(); samplei++) {
            for (std::size_t channeli = 0; channeli < output_frame[0].size(); channeli++) {
                write.final_samples.pcm16[samplei][channeli] = output_frame[samplei][channeli];
            }
        }

        return output_frame;
    }

    void Sleep() {
        backup_dsp_memory.raw_memory = dsp_memory->raw_memory;
        mixers.Sleep();
        for (auto& s : sources) {
            s.Sleep();
        }
        on = false;
    }

    void Wakeup() {
        dsp_memory->raw_memory = backup_dsp_memory.raw_memory;
        backup_dsp_memory.raw_memory.fill(0);
        mixers.Wakeup();
        for (auto& s : sources) {
            s.Wakeup();
        }
        on = true;
    }
};

// The generator of the aux buses' samples (dsp_oracle.py repeats it): signed 32-bit values in
// -amplitude..amplitude from two draws of the 16-bit LCG.
std::vector<u8> GeneratePcm32(u32 seed, u32 size, u32 amplitude) {
    u32 state = seed;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state >> 16;
    };
    std::vector<u8> out;
    for (u32 i = 0; i + 4 <= size; i += 4) {
        const u32 raw = (next() << 16) | next();
        const s32 value = static_cast<s32>(static_cast<s64>(raw % (2ull * amplitude + 1)) - amplitude);
        u8 bytes[4];
        std::memcpy(bytes, &value, 4);
        out.insert(out.end(), bytes, bytes + 4);
    }
    out.resize(size, 0);
    return out;
}

u32 LoadU32(const std::vector<u8>& bytes, std::size_t at) {
    u32 value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

std::string Hex8(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

}  // namespace

void RunFrameCase(std::istream& header, std::istream& lines) {
    std::string name;
    u32 frames = 0, first_counter = 0;
    u32 sc, ac, dc, mc, ss, fs, ds;
    header >> name >> frames >> first_counter >> sc >> ac >> dc >> mc >> ss >> fs >> ds;
    // The offsets the script wrote (in a region) are Azahar's.
    if (sc != offsetof(HLE::SharedMemory, source_configurations) ||
        ac != offsetof(HLE::SharedMemory, adpcm_coefficients) ||
        dc != offsetof(HLE::SharedMemory, dsp_configuration) ||
        mc != offsetof(HLE::SharedMemory, intermediate_mix_samples) ||
        ss != offsetof(HLE::SharedMemory, source_statuses) ||
        fs != offsetof(HLE::SharedMemory, final_samples) || ds != offsetof(HLE::SharedMemory, dsp_status)) {
        Die("the script's offsets in the shared memory are not Azahar's");
    }

    Dsp dsp;
    std::vector<std::pair<u32, Event>> events;  // (frame, event)
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream in(line);
        std::string word;
        in >> word;
        if (word == "end") {
            break;
        } else if (word == "mem") {
            std::string address, hex;
            in >> address >> hex;
            const std::vector<u8> data = Unhex(hex);
            dsp.memory.Place(static_cast<u32>(std::stoul(address, nullptr, 16)), data.data(), data.size());
        } else if (word == "ev") {
            std::string op, hex;
            u32 frame = 0;
            Event e{};
            in >> frame >> op >> e.offset >> hex;
            e.op = op[0];
            e.bytes = Unhex(hex);
            events.emplace_back(frame, e);
        } else if (!word.empty()) {
            Die("bad line in a frame case");
        }
    }

    const u32 region_base[2] = {HLE::region0_offset, HLE::region1_offset};
    u8* const raw = dsp.dsp_memory->raw_memory.data();
    for (u32 frame = 0; frame < frames; ++frame) {
        const std::size_t target = (frame + 1) % 2;  // the region the DSP reads in this frame
        u8* const region = raw + region_base[target];
        for (const auto& [at, e] : events) {
            if (at != frame) {
                continue;
            }
            switch (e.op) {
            case 'r':
                if (e.offset + e.bytes.size() > 0x8000) {
                    Die("a write outside the region");
                }
                std::memcpy(region + e.offset, e.bytes.data(), e.bytes.size());
                break;
            case 'o': {
                u32 word;
                std::memcpy(&word, region + e.offset, 4);
                word |= LoadU32(e.bytes, 0);
                std::memcpy(region + e.offset, &word, 4);
                break;
            }
            case 'p': {
                u16 word;
                std::memcpy(&word, region + e.offset, 2);
                word = static_cast<u16>(word | (e.bytes[0] | (e.bytes[1] << 8)));
                std::memcpy(region + e.offset, &word, 2);
                break;
            }
            case 'g': {  // seed, size, amplitude, crc-32 of the samples
                const std::vector<u8> data = GeneratePcm32(LoadU32(e.bytes, 0), LoadU32(e.bytes, 4), LoadU32(e.bytes, 8));
                if (Crc32(data.data(), data.size()) != LoadU32(e.bytes, 12)) {
                    Die("the generator of the samples differs from the script's");
                }
                std::memcpy(region + e.offset, data.data(), data.size());
                break;
            }
            case 'm':
                dsp.memory.Place(e.offset, e.bytes.data(), e.bytes.size());
                break;
            case 'S':
                dsp.Sleep();
                break;
            case 'W':
                dsp.Wakeup();
                break;
            default:
                Die("unknown event");
            }
        }
        const u16 counter = static_cast<u16>(first_counter + frame + 1);
        std::memcpy(region + 0x7FFE, &counter, 2);

        std::string row = "00000000000000000000000000000000";
        if (dsp.on) {
            if (dsp.CurrentRegionIndex() != target) {
                Die("the frame counters do not pick the region the application wrote");
            }
            HLE::SharedMemory& read = dsp.ReadRegion();
            HLE::SharedMemory& write = dsp.WriteRegion();
            dsp.GenerateCurrentFrame();
            std::vector<u8> statuses(sizeof(write.source_statuses) + 4);
            std::memcpy(statuses.data(), &write.source_statuses, sizeof(write.source_statuses));
            std::memcpy(statuses.data() + sizeof(write.source_statuses), &write.dsp_status, 4);
            std::vector<u8> configuration(sizeof(read.source_configurations) + sizeof(read.dsp_configuration));
            std::memcpy(configuration.data(), &read.source_configurations, sizeof(read.source_configurations));
            std::memcpy(configuration.data() + sizeof(read.source_configurations), &read.dsp_configuration,
                        sizeof(read.dsp_configuration));
            row = Hex8(Crc32(&write.final_samples, sizeof(write.final_samples))) +
                  Hex8(Crc32(statuses.data(), statuses.size())) +
                  Hex8(Crc32(&write.intermediate_mix_samples, sizeof(write.intermediate_mix_samples))) +
                  Hex8(Crc32(configuration.data(), configuration.size()));
        }
        std::printf("frame %s %u %s %s %s %s\n", name.c_str(), frame, row.substr(0, 8).c_str(),
                    row.substr(8, 8).c_str(), row.substr(16, 8).c_str(), row.substr(24, 8).c_str());
    }
}
