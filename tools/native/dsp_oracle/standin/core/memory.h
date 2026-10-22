// Derived in part from Azahar src/core/memory.h and src/core/memory.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "Licensed under GPLv2 or any later version"): the physical memory areas, the rule that picks one for an address (GetPhysMemRegionInfo, ends included) and GetPhysicalPointer.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h, memory.cpp)
//
// Stand-in of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py) for core/memory.h: a
// MemorySystem over the case's synthetic memory. Physical addresses are valid in the four areas
// Azahar's GetPhysMemRegionInfo knows (VRAM, DSP RAM, FCRAM of the New 3DS's size, the New 3DS's extra
// RAM; the end of an area counts), as there. Only the first kWindow bytes of FCRAM hold the case's
// data (the harness places it); the rest of every area reads as zeros, which is what unwritten memory
// holds. The pointer of an address that is valid is good for kPad bytes.
#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

#include "common/common_types.h"

namespace Memory {

constexpr PAddr VRAM_PADDR = 0x18000000;
constexpr u32 VRAM_SIZE = 0x00600000;
constexpr PAddr N3DS_EXTRA_RAM_PADDR = 0x1F000000;
constexpr u32 N3DS_EXTRA_RAM_SIZE = 0x00400000;
constexpr PAddr DSP_RAM_PADDR = 0x1FF00000;
constexpr u32 DSP_RAM_SIZE = 0x00080000;
constexpr PAddr FCRAM_PADDR = 0x20000000;
constexpr u32 FCRAM_N3DS_SIZE = 0x10000000;

class MemorySystem {
public:
    static constexpr std::size_t kWindow = 0x400000;  // the case's data: FCRAM's first 4 MiB
    static constexpr std::size_t kPad = 0x400000;     // zeros behind it, and behind every other area

    MemorySystem() : fcram_(kWindow + kPad), zeros_(kWindow + kPad) {}

    // The harness puts the case's data in the window.
    void Place(PAddr address, const u8* data, std::size_t size) {
        std::memcpy(fcram_.data() + (address - FCRAM_PADDR), data, size);
    }

    u8* GetPhysicalPointer(PAddr address) {
        struct Area {
            PAddr start;
            u32 size;
        };
        static constexpr Area areas[] = {
            {VRAM_PADDR, VRAM_SIZE},
            {DSP_RAM_PADDR, DSP_RAM_SIZE},
            {FCRAM_PADDR, FCRAM_N3DS_SIZE},
            {N3DS_EXTRA_RAM_PADDR, N3DS_EXTRA_RAM_SIZE},
        };
        for (const Area& area : areas) {
            // Note: the region end check is inclusive because the user can pass in an address that
            // represents an open right bound (Azahar's comment).
            if (address >= area.start && static_cast<u64>(address) <= static_cast<u64>(area.start) + area.size) {
                if (area.start == FCRAM_PADDR && address - FCRAM_PADDR < kWindow) {
                    return fcram_.data() + (address - FCRAM_PADDR);
                }
                return zeros_.data();
            }
        }
        return nullptr;
    }

private:
    std::vector<u8> fcram_;
    std::vector<u8> zeros_;
};

} // namespace Memory
