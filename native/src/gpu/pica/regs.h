// Derived from Azahar src/video_core/pica/regs_internal.h, regs_pipeline.h, regs_shader.h,
// regs_lighting.h, regs_texturing.h, regs_internal.cpp, pica_core.cpp, pica_core.h (the layout of
// PicaCore::Regs) and src/video_core/gpu.cpp (the MMIO base address) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version"): the positions of the internal registers (the
// layout of RegsInternal and of the structures inside it), the bit fields of the registers the
// command processor acts on, and the list of registers that need special handling
// (BuildRegImplFlagsLUT in pica_core.cpp).
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h,
//   regs_pipeline.h, regs_shader.h)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.cpp)
// Copyright 2017 Citra Emulator Project (regs_lighting.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp, pica_core.h,
//   gpu.cpp)
//
// The PICA's internal registers (N2-04): the 0x300 words a command list writes. Only the
// registers the command processor reads or acts on are named here; the rest are plain storage
// that the draw call carries to the backend.
#pragma once

#include "mh4n/types.h"

namespace mh4n::gpu::pica {

// RegsInternal::NUM_REGS. Register ids in a command header above this are ignored.
inline constexpr u32 kNumRegs = 0x300;

// In Azahar's PicaCore::Regs the internal registers follow 0x432 words of external ones, so the
// MMIO address of internal register n is 0x1EF00000 + 4 * (kExternalRegs + n).
inline constexpr u32 kExternalRegs = 0x432;
inline constexpr u32 kGpuMmioBase = 0x1EF00000;

constexpr u32 BitsOf(u32 word, u32 first, u32 count) {
    return (word >> first) & ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u));
}

// A bit field assigned in place: the other bits of the word are kept (BitField::Assign).
constexpr u32 WithBits(u32 word, u32 first, u32 count, u32 value) {
    const u32 mask = ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u)) << first;
    return (word & ~mask) | ((value << first) & mask);
}

// The 4-bit parameter mask of a command header expanded to bytes (pica_core.cpp,
// ExpandBitsToBytes): 0b0101 -> 0x00FF00FF.
constexpr u32 ExpandMask(u32 mask4) {
    u32 result = 0;
    for (u32 i = 0; i < 4; ++i) {
        if ((mask4 >> i) & 1u) {
            result |= 0xFFu << (8 * i);
        }
    }
    return result;
}

constexpr bool InRange(u32 id, u32 first, u32 count) {
    return id >= first && id < first + count;
}

namespace reg {

// ---- interrupt request ---------------------------------------------------------------------
inline constexpr u32 kIrqRequest = 0x010; // GPUREG_FINALIZE
inline constexpr u32 kIrqCompare = 0x020;
inline constexpr u32 kIrqMask = 0x030;
inline constexpr u32 kIrqAutostop = 0x034;

// ---- texturing: procedural texture and fog look-up tables ------------------------------------
inline constexpr u32 kProcTexLutConfig = 0x0AF; // index bits 0-7, table bits 8-11
inline constexpr u32 kProcTexLutData = 0x0B0;   // 8 registers
inline constexpr u32 kFogLutOffset = 0x0E6;     // bits 0-15
inline constexpr u32 kFogLutData = 0x0E8;       // 8 registers

// ---- gas (3dbrew, GPU/Internal Registers; Azahar defines none of them) --------------------------
// GPUREG_GAS_LUT_INDEX: bits 0-15, the index GPUREG_GAS_LUT_DATA writes to. GPUREG_GAS_LUT_DATA
// is listed alone (0x125 is GPUREG_GAS_ACCMAX_FEEDBACK), not as a run of eight aliases like the
// fog data port; the page's "these registers" is read as the port, and the one id is acted on.
inline constexpr u32 kGasLutIndex = 0x123;
inline constexpr u32 kGasLutData = 0x124;

// ---- lighting look-up tables ------------------------------------------------------------------
inline constexpr u32 kLightingLutConfig = 0x1C5; // index bits 0-7, type bits 8-12
inline constexpr u32 kLightingLutData = 0x1C8;   // 8 registers

// ---- pipeline (0x200) -----------------------------------------------------------------------------
inline constexpr u32 kVertexAttributesBase = 0x200; // base_address: bits 1-28, in units of 16 bytes
inline constexpr u32 kIndexArray = 0x227;           // offset bits 0-27, format bit 31 (1 = u16)
inline constexpr u32 kNumVertices = 0x228;
inline constexpr u32 kGeometryStage = 0x229; // use_gs bits 0-1 (2 = yes), variable_primitive bit 31
inline constexpr u32 kVertexOffset = 0x22A;
inline constexpr u32 kTriggerDraw = 0x22E;
inline constexpr u32 kTriggerDrawIndexed = 0x22F;
inline constexpr u32 kFixedAttribIndex = 0x232; // vs_default_attributes_setup.index
inline constexpr u32 kFixedAttribData = 0x233;  // set_value[3]
inline constexpr u32 kCommandBufferSize = 0x238;    // size[2], bits 0-19, in units of 8 bytes
inline constexpr u32 kCommandBufferAddress = 0x23A; // addr[2], bits 0-27, in units of 8 bytes
inline constexpr u32 kCommandBufferTrigger = 0x23C; // trigger[2]
inline constexpr u32 kMaxInputAttribIndex = 0x242;  // bits 0-3
inline constexpr u32 kGsUnitExclusiveConfig = 0x244; // bit 0
inline constexpr u32 kGpuMode = 0x245;
inline constexpr u32 kVsOutmapTotalA = 0x24A; // bits 0-3
inline constexpr u32 kVsOutmapTotalB = 0x251; // bits 0-3
inline constexpr u32 kGsConfig = 0x252; // mode 0-7, fixed_vertex_num-1 8-11, stride-1 12-15, start 16-23
inline constexpr u32 kVariableVertexMainNum = 0x254;
inline constexpr u32 kTriangleTopology = 0x25E; // bits 8-9
inline constexpr u32 kRestartPrimitive = 0x25F;

// ---- the two shader register blocks (ShaderRegs, 0x30 words each) ---------------------------------
inline constexpr u32 kGs = 0x280;
inline constexpr u32 kVs = 0x2B0;

// Offsets inside a block.
inline constexpr u32 kShaderBoolUniforms = 0x00; // bits 0-15
inline constexpr u32 kShaderIntUniforms = 0x01;  // 4 registers, one byte per component
inline constexpr u32 kShaderInputBufferConfig = 0x09; // max input attribute 0-3, to_uniform 8-15, mode 24-31
inline constexpr u32 kShaderMainOffset = 0x0A;        // bits 0-15
inline constexpr u32 kShaderOutputMask = 0x0D;        // bits 0-15
inline constexpr u32 kShaderUniformIndex = 0x10;      // index bits 0-6, float32 format bit 31
inline constexpr u32 kShaderUniformData = 0x11;       // 8 registers
inline constexpr u32 kShaderProgramOffset = 0x1B;
inline constexpr u32 kShaderProgramData = 0x1C; // 8 registers
inline constexpr u32 kShaderSwizzleOffset = 0x25;
inline constexpr u32 kShaderSwizzleData = 0x26; // 8 registers

} // namespace reg

// ShaderRegs::ShaderMode.
inline constexpr u32 kShaderModeGs = 0x08;
inline constexpr u32 kShaderModeVs = 0xA0;

// PipelineRegs::UseGS and GSMode.
inline constexpr u32 kUseGsNo = 0;
inline constexpr u32 kUseGsYes = 2;
inline constexpr u32 kGsModePoint = 0;
inline constexpr u32 kGsModeVariablePrimitive = 1;
inline constexpr u32 kGsModeFixedPrimitive = 2;

// PipelineRegs::TriangleTopology.
inline constexpr u32 kTopologyList = 0;
inline constexpr u32 kTopologyStrip = 1;
inline constexpr u32 kTopologyFan = 2;
inline constexpr u32 kTopologyShader = 3;

// TexturingRegs::ProcTexLutTable.
inline constexpr u32 kProcTexNoise = 0;
inline constexpr u32 kProcTexColorMap = 2;
inline constexpr u32 kProcTexAlphaMap = 3;
inline constexpr u32 kProcTexColor = 4;
inline constexpr u32 kProcTexColorDiff = 5;

// The registers that fill a table or a program one word at a time, eight aliases each: a write to
// any of the eight does the same (pica_core.cpp marks them "supports batch").
constexpr bool IsBatchRegister(u32 id) {
    return InRange(id, reg::kFixedAttribData, 3) ||
           InRange(id, reg::kGs + reg::kShaderUniformData, 8) ||
           InRange(id, reg::kVs + reg::kShaderUniformData, 8) ||
           InRange(id, reg::kGs + reg::kShaderProgramData, 8) ||
           InRange(id, reg::kVs + reg::kShaderProgramData, 8) ||
           InRange(id, reg::kGs + reg::kShaderSwizzleData, 8) ||
           InRange(id, reg::kVs + reg::kShaderSwizzleData, 8) ||
           InRange(id, reg::kLightingLutData, 8) || InRange(id, reg::kFogLutData, 8) ||
           InRange(id, reg::kProcTexLutData, 8);
}

// The registers whose write does more than store the value (BuildRegImplFlagsLUT's
// mark_special): every batch register, and these.
constexpr bool IsSpecialRegister(u32 id) {
    return IsBatchRegister(id) || id == reg::kIrqRequest || id == reg::kTriangleTopology ||
           id == reg::kRestartPrimitive || id == reg::kFixedAttribIndex ||
           id == reg::kGasLutIndex || id == reg::kGasLutData || id == reg::kTriggerDraw || id == reg::kTriggerDrawIndexed ||
           InRange(id, reg::kCommandBufferTrigger, 2) ||
           id == reg::kGs + reg::kShaderBoolUniforms || id == reg::kVs + reg::kShaderBoolUniforms ||
           InRange(id, reg::kGs + reg::kShaderIntUniforms, 4) ||
           InRange(id, reg::kVs + reg::kShaderIntUniforms, 4) ||
           id == reg::kVs + reg::kShaderOutputMask;
}

} // namespace mh4n::gpu::pica
