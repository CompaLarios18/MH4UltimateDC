// Derived from Azahar src/video_core/pica/pica_core.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; the file says "Licensed under GPLv2 or any later version"): the position of
// `internal` inside PicaCore::Regs::reg_array (word 0x400).
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h)
//
// The rule that puts the PICA's internal registers together from an INIT record (N2-40).
//
// In Azahar's PicaCore::Regs the internal registers (`internal`) start at word 0x400 of reg_array
// (pica_core.h; the sizes of the structures before them are asserted in regs_external.h). The
// recorder wrote reg_array in two blocks (docs/native/mgpu-format.md section 4): GPU_REGS holds
// words 0..0x431 and PICA_REGS holds words 0x432..0x731. So internal register i is
//
//     GPU_REGS word 0x400 + i       for i below 0x32,
//     PICA_REGS word i - 0x32       for i from 0x32 on (up to kNumRegs - 1 = 0x2FF).
//
// (PICA_REGS' last 0x32 words, 0x2CE to 0x2FF, lie past Azahar's `internal` and are not registers.)
// This is the one place that says so: the command processor's LoadInitState and the capture
// player's own register copy both call it.
#pragma once

#include <cstring>
#include <span>

#include "gpu/pica/regs.h"
#include "mh4n/types.h"

namespace mh4n::gpu::pica {

// The internal registers held in the tail of the GPU_REGS block (words 0x400 to 0x431).
inline constexpr u32 kInternalRegsInGpuBlock = kGpuRegsBlockWords - kExternalRegs;

// What LoadInternalRegisters found.
struct InternalRegsLoaded {
    bool from_gpu_regs = false;  // registers 0 to 0x31 were read (a GPU_REGS block of the right size)
    bool from_pica_regs = false; // registers 0x32 to 0x2FF were read (a PICA_REGS block of the right size)
};

// Writes the internal registers the blocks hold into `regs` (kNumRegs words). A block that is
// empty or has the wrong size is not read, and the registers it would give keep the value they
// have in `regs`.
inline InternalRegsLoaded LoadInternalRegisters(std::span<const u8> gpu_regs,
                                                std::span<const u8> pica_regs,
                                                std::span<u32, kNumRegs> regs) {
    InternalRegsLoaded loaded;
    const auto load = [](const u8* p) {
        u32 value;
        std::memcpy(&value, p, sizeof(value));
        return value;
    };
    if (gpu_regs.size() == static_cast<size_t>(kGpuRegsBlockWords) * 4) {
        for (u32 i = 0; i < kInternalRegsInGpuBlock; ++i) {
            regs[i] = load(gpu_regs.data() + 4 * static_cast<size_t>(kExternalRegs + i));
        }
        loaded.from_gpu_regs = true;
    }
    if (pica_regs.size() == static_cast<size_t>(kNumRegs) * 4) {
        for (u32 i = kInternalRegsInGpuBlock; i < kNumRegs; ++i) {
            regs[i] = load(pica_regs.data() + 4 * static_cast<size_t>(i - kInternalRegsInGpuBlock));
        }
        loaded.from_pica_regs = true;
    }
    return loaded;
}

} // namespace mh4n::gpu::pica
