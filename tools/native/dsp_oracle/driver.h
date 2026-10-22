// The pieces the two drivers of the DSP oracle share (driver.cpp, driver_frame.cpp): the CRC-32, the hex
// reader, the events of a case and the entry point of a frame case. Nothing of Azahar's.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <string>
#include <vector>

#include "common/common_types.h"

inline u32 Crc32(const void* data, std::size_t size) {
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

inline std::vector<u8> Unhex(const std::string& text) {
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
    // case: c config bytes, d dirty bits, b buffers_dirty bits, a coefficient bytes, m memory bytes,
    //       S sleep, W wakeup
    // fcase: r bytes into the region the frame reads, o OR a u32 there, p OR a u16 there, g generated
    //       s32 samples there, m memory bytes, S sleep, W wakeup
    char op;
    u32 offset;
    std::vector<u8> bytes;
};

// Plays an `fcase` (its header words in `header`, its lines read from `lines` up to `end`) and prints the
// frames' CRCs.
void RunFrameCase(std::istream& header, std::istream& lines);
