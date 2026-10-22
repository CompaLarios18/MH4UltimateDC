// Derived from Azahar src/audio_core/hle/hle.cpp, src/audio_core/hle/source.h and
// src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "Licensed under GPLv2 or any later version"): the way DspHle::Impl::GenerateCurrentFrame calls a Source (Tick with the configuration and the source's row of ADPCM coefficients, then MixInto for each of the three intermediate mixes), and the layouts of SourceConfiguration, AdpcmCoefficients and SourceStatus that the cases' byte writes address.
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (source.h)
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
//
// The driver of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py): compiled with Azahar's
// codec.cpp, interpolate.cpp, filter.cpp and source.cpp, it reads the cases the script wrote on
// standard input, plays each against one Azahar Source and prints a CRC-32 of what comes out of every
// frame. No value is computed here but the CRCs. The format is in dsp_oracle.py.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "audio_core/hle/shared_memory.h"
#include "audio_core/hle/source.h"
#include "core/memory.h"

namespace {

using AudioCore::HLE::AdpcmCoefficients;
using AudioCore::HLE::Source;
using AudioCore::HLE::SourceConfiguration;
using AudioCore::HLE::SourceStatus;

u32 Crc32(const void* data, std::size_t size) {
    static u32 table[256];
    static bool ready = false;
    if (!ready) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        ready = true;
    }
    u32 crc = 0xFFFFFFFFu;
    const u8* bytes = static_cast<const u8*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::vector<u8> Unhex(const std::string& text) {
    std::vector<u8> out;
    if (text == "-") {
        return out;
    }
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<u8>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

struct Event {
    u32 frame;
    char op;  // c config bytes, d dirty bits, b buffers_dirty bits, a coefficient bytes, m memory bytes, S sleep, W wakeup
    u32 offset;
    std::vector<u8> bytes;
};

struct Case {
    std::string name;
    u32 frames = 0;
    u32 source_id = 0;
    std::vector<std::pair<u32, std::vector<u8>>> memory;
    std::vector<Event> events;
};

u32 LoadU32(const std::vector<u8>& bytes) {
    u32 value = 0;
    std::memcpy(&value, bytes.data(), std::min<std::size_t>(4, bytes.size()));
    return value;
}

void Run(const Case& c) {
    Memory::MemorySystem memory;
    for (const auto& [address, data] : c.memory) {
        memory.Place(address, data.data(), data.size());
    }
    Source source(c.source_id);
    source.SetMemory(memory);

    static_assert(sizeof(SourceConfiguration::Configuration) == 192);
    SourceConfiguration::Configuration config;
    std::memset(&config, 0, sizeof(config));
    AdpcmCoefficients coefficients;
    std::memset(&coefficients, 0, sizeof(coefficients));

    for (u32 frame = 0; frame < c.frames; ++frame) {
        for (const Event& e : c.events) {
            if (e.frame != frame) {
                continue;
            }
            switch (e.op) {
            case 'c':
                std::memcpy(reinterpret_cast<u8*>(&config) + e.offset, e.bytes.data(), e.bytes.size());
                break;
            case 'd': {
                u32 dirty;
                std::memcpy(&dirty, &config, 4);
                dirty |= LoadU32(e.bytes);
                std::memcpy(&config, &dirty, 4);
                break;
            }
            case 'b':
                config.buffers_dirty = static_cast<u16>(config.buffers_dirty | LoadU32(e.bytes));
                break;
            case 'a':
                std::memcpy(reinterpret_cast<u8*>(&coefficients) + e.offset, e.bytes.data(),
                            e.bytes.size());
                break;
            case 'm':
                memory.Place(e.offset, e.bytes.data(), e.bytes.size());
                break;
            case 'S':
                source.Sleep();
                break;
            case 'W':
                source.Wakeup();
                break;
            default:
                std::fprintf(stderr, "driver: unknown event %c\n", e.op);
                std::exit(2);
            }
        }

        const SourceStatus::Status status = source.Tick(config, coefficients.coeff[c.source_id]);
        AudioCore::QuadFrame32 mixes[3] = {};
        for (std::size_t mix = 0; mix < 3; ++mix) {
            source.MixInto(mixes[mix], mix);
        }
        std::printf("frame %s %u %08x %08x %08x %08x\n", c.name.c_str(), frame,
                    Crc32(&mixes[0], sizeof(mixes[0])), Crc32(&mixes[1], sizeof(mixes[1])),
                    Crc32(&mixes[2], sizeof(mixes[2])), Crc32(&status, sizeof(status)));
    }
}

}  // namespace

int main() {
    static_assert(sizeof(SourceStatus::Status) == 12);
    Case current;
    std::string line;
    bool open = false;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string word;
        in >> word;
        if (word == "case") {
            current = Case{};
            in >> current.name >> current.frames >> current.source_id;
            if (current.source_id >= AudioCore::HLE::num_sources) {
                std::fprintf(stderr, "driver: source %u\n", current.source_id);
                return 2;
            }
            open = true;
        } else if (word == "mem" && open) {
            std::string address, hex;
            in >> address >> hex;
            current.memory.emplace_back(static_cast<u32>(std::stoul(address, nullptr, 16)), Unhex(hex));
        } else if (word == "ev" && open) {
            std::string op, hex;
            Event e{};
            in >> e.frame >> op >> e.offset >> hex;
            e.op = op[0];
            e.bytes = Unhex(hex);
            current.events.push_back(e);
        } else if (word == "end" && open) {
            Run(current);
            open = false;
        } else if (!word.empty()) {
            std::fprintf(stderr, "driver: bad line %s\n", line.c_str());
            return 2;
        }
    }
    return 0;
}
