// Derived from Azahar src/video_core/pica/pica_core.cpp (the register side effects of
// triangle_topology and restart_primitive, ~line 592) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; the file says "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp)
//
// See primitive_assembly.h.
#include "gpu/cpuvertex/primitive_assembly.h"

namespace mh4n::gpu::cpuvertex {

void PrimitiveAssembler::ApplyRegisterWrites(const pica::PicaState& state) {
    if (state.dirty.Test(pica::reg::kTriangleTopology)) {
        Reconfigure(static_cast<Topology>((state.regs[pica::reg::kTriangleTopology] >> 8) & 3u));
    }
    if (state.dirty.Test(pica::reg::kRestartPrimitive)) {
        Reset();
    }
}

} // namespace mh4n::gpu::cpuvertex
