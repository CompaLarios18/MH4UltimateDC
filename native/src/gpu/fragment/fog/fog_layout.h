// Written for this package from 3dbrew (page GPU/Internal Registers, revision 23943): nothing is copied from Azahar here.
//
// The layout of what `fog/` reserves in N2-33's key and uniform block (N2-10), shared by fog.cpp and its test.
//
// THE RESERVED GAS KEY WORD (`FragmentKey::gas.raw`, filled by `FogFeature::Key`, canonical: a bit is set only when the
// shader reads it):
//     bit 0  density pass      GPUREG_COLOR_OPERATION (0x100) bits 0-1, "fragment operation mode", is 1 = Gas   [3dbrew]
//     bit 1  light-factor LUT  GPUREG_GAS_LIGHT_Z_COLOR (0x122) bit 8 is 1, and the shading pass is on (fog mode Gas,
//                              not the density pass)                                                           [3dbrew]
//
// THE RESERVED GAS UNIFORM REGION (`FragmentBlock::gas`, 16 register words and 16 table words). N2-33's comment in
// uniforms.h calls the register words "0x120-0x12F"; the layout here keeps 0x120-0x126 at their natural positions and
// puts the three gas registers outside that run after them, so words 10 to 15 are 0:
//     regs[0] 0x120 GPUREG_GAS_LIGHT_XY      regs[4] 0x124 GPUREG_GAS_LUT_DATA (the last word written; unused)
//     regs[1] 0x121 GPUREG_GAS_LIGHT_Z       regs[5] 0x125 GPUREG_GAS_ACCMAX_FEEDBACK
//     regs[2] 0x122 GPUREG_GAS_LIGHT_Z_COLOR regs[6] 0x126 GPUREG_GAS_DELTAZ_DEPTH
//     regs[3] 0x123 GPUREG_GAS_LUT_INDEX     regs[7] 0x0E0 GPUREG_TEXENV_UPDATE_BUFFER (fog mode, density source)
//     regs[8] 0x0E4 GPUREG_GAS_ATTENUATION   regs[9] 0x0E5 GPUREG_GAS_ACCMAX
// The table words are `PicaState::gas.lut[0..15]` as the data port received them.
#pragma once

#include "mh4n/types.h"

namespace mh4n::gpu::fragment::fog {

inline constexpr u32 kKeyDensityPass = 1u << 0;
inline constexpr u32 kKeyLightFactorInput = 1u << 1;

inline constexpr u32 kRegLightXY = 0;
inline constexpr u32 kRegLightZ = 1;
inline constexpr u32 kRegLightZColor = 2;
inline constexpr u32 kRegLutIndex = 3;
inline constexpr u32 kRegLutData = 4;
inline constexpr u32 kRegAccMaxFeedback = 5;
inline constexpr u32 kRegDeltaZDepth = 6;
inline constexpr u32 kRegFogMode = 7;
inline constexpr u32 kRegAttenuation = 8;
inline constexpr u32 kRegAccMax = 9;
inline constexpr u32 kRegWordsUsed = 10;

}  // namespace mh4n::gpu::fragment::fog
