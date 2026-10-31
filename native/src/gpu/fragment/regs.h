// Derived from Azahar src/video_core/pica/regs_internal.h, regs_texturing.h, regs_lighting.h,
// regs_framebuffer.h and regs_rasterizer.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of those files says "Licensed under GPLv2 or any later version"):
// the enumerations the fragment configuration uses, one for one with their enumerators, and the
// positions of the registers it reads (ASSERT_REG_POSITION and the layout of each register
// structure, worked out word by word).
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h,
//   regs_framebuffer.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017 Citra Emulator Project (regs_lighting.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
//
// The fragment-side PICA registers and enumerations (N2-33): what `MakeKey` and `FillUniforms` read
// out of `PicaState::regs`. The command processor's own register names are `gpu/pica/regs.h`; these
// are the ones it does not name. Each field is `Bits(regs[id], first, count)`.
#pragma once

#include "mh4n/types.h"

namespace mh4n::gpu::fragment {

constexpr u32 Bits(u32 word, u32 first, u32 count) {
    return (word >> first) & ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u));
}
constexpr u32 SetBits(u32 word, u32 first, u32 count, u32 value) {
    const u32 mask = ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u)) << first;
    return (word & ~mask) | ((value << first) & mask);
}

// ---- enumerations (values as the registers hold them) -------------------------------------------

namespace regenum {

enum class ScissorMode : u32 { Disabled = 0, Exclude = 1, Include = 3 };
enum class DepthBuffering : u32 { WBuffering = 0, ZBuffering = 1 };
enum class FragmentOperationMode : u32 { Default = 0, Gas = 1, Shadow = 3 };

enum class LogicOp : u32 {
    Clear = 0, And = 1, AndReverse = 2, Copy = 3, Set = 4, CopyInverted = 5, NoOp = 6, Invert = 7,
    Nand = 8, Or = 9, Nor = 10, Xor = 11, Equiv = 12, AndInverted = 13, OrReverse = 14, OrInverted = 15,
};
enum class BlendEquation : u32 { Add = 0, Subtract = 1, ReverseSubtract = 2, Min = 3, Max = 4 };
enum class BlendFactor : u32 {
    Zero = 0, One = 1, SourceColor = 2, OneMinusSourceColor = 3, DestColor = 4, OneMinusDestColor = 5,
    SourceAlpha = 6, OneMinusSourceAlpha = 7, DestAlpha = 8, OneMinusDestAlpha = 9, ConstantColor = 10,
    OneMinusConstantColor = 11, ConstantAlpha = 12, OneMinusConstantAlpha = 13, SourceAlphaSaturate = 14,
};
enum class CompareFunc : u32 {
    Never = 0, Always = 1, Equal = 2, NotEqual = 3, LessThan = 4, LessThanOrEqual = 5, GreaterThan = 6,
    GreaterThanOrEqual = 7,
};

enum class TextureType : u32 { Texture2D = 0, TextureCube = 1, Shadow2D = 2, Projection2D = 3, ShadowCube = 4, Disabled = 5 };
enum class WrapMode : u32 {
    ClampToEdge = 0, ClampToBorder = 1, Repeat = 2, MirroredRepeat = 3, ClampToEdge2 = 4, ClampToBorder2 = 5,
    Repeat2 = 6, Repeat3 = 7,
};
enum class TevOperation : u32 {
    Replace = 0, Modulate = 1, Add = 2, AddSigned = 3, Lerp = 4, Subtract = 5, Dot3_RGB = 6, Dot3_RGBA = 7,
    MultiplyThenAdd = 8, AddThenMultiply = 9,
};
enum class FogMode : u32 { None = 0, Fog = 5, Gas = 7 };

enum class LightingConfig : u32 {
    Config0 = 0, Config1 = 1, Config2 = 2, Config3 = 3, Config4 = 4, Config5 = 5, Config6 = 6, Config7 = 8,
};
enum class LightingLutInput : u32 { NH = 0, VH = 1, NV = 2, LN = 3, SP = 4, CP = 5 };
enum class LightingBumpMode : u32 { None = 0, NormalMap = 1, TangentMap = 2 };

enum class ProcTexClamp : u32 { ToZero = 0, ToEdge = 1, SymmetricalRepeat = 2, MirroredRepeat = 3, Pulse = 4 };
enum class ProcTexCombiner : u32 { U = 0, U2 = 1, V = 2, V2 = 3, Add = 4, Add2 = 5, SqrtAdd2 = 6, Min = 7, Max = 8, RMax = 9 };
enum class ProcTexShift : u32 { None = 0, Odd = 1, Even = 2 };
enum class ProcTexFilter : u32 {
    Nearest = 0, Linear = 1, NearestMipmapNearest = 2, LinearMipmapNearest = 3, NearestMipmapLinear = 4,
    LinearMipmapLinear = 5,
};

} // namespace regenum

// ---- register ids (word index in PicaState::regs) ----------------------------------------------

namespace freg {

// rasterizer
inline constexpr u32 kViewportDepthRange = 0x4D;      // float24 (bits 0-23)
inline constexpr u32 kViewportDepthNearPlane = 0x4E;  // float24
inline constexpr u32 kScissorMode = 0x65;             // bits 0-1
inline constexpr u32 kScissorXY1 = 0x66;              // x1 bits 0-9, y1 bits 16-25
inline constexpr u32 kScissorXY2 = 0x67;              // x2 bits 0-9, y2 bits 16-25
inline constexpr u32 kDepthmapEnable = 0x6D;          // bit 0

// texturing
inline constexpr u32 kMainConfig = 0x80;              // texture{0,1,2}_enable 0-2, texture3_coordinates 8-9,
                                                      // texture3_enable 10, texture2_use_coord1 13
inline constexpr u32 kTexture0 = 0x81;                // five words: border colour, size, params, lod, address
inline constexpr u32 kTexture1 = 0x91;
inline constexpr u32 kTexture2 = 0x99;
inline constexpr u32 kTextureBorderColor = 0;         // offsets inside a unit's five words
inline constexpr u32 kTextureParams = 2;              // wrap_t 8-10, wrap_s 12-14, type 28-30 (unit 0)
inline constexpr u32 kTextureLod = 3;                 // bias bits 0-12 (fixed 1.4.8)
inline constexpr u32 kShadowTexture = 0x8B;           // orthographic bit 0, bias bits 1-23
inline constexpr u32 kProcTex = 0xA8;
inline constexpr u32 kProcTexNoiseU = 0xA9;           // amplitude bits 0-15 (s16, fixed 1.3.12), phase 16-31 (f16)
inline constexpr u32 kProcTexNoiseV = 0xAA;
inline constexpr u32 kProcTexNoiseFrequency = 0xAB;   // u 0-15, v 16-31 (f16)
inline constexpr u32 kProcTexLut = 0xAC;              // filter 0-2, lod_min 3-6, lod_max 7-10, width 11-18, bias_high 19-26
inline constexpr u32 kProcTexLutOffset = 0xAD;        // level0..3, one byte each
inline constexpr u32 kTevStage0 = 0xC0;               // stages at 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8
inline constexpr u32 kTevStage1 = 0xC8;
inline constexpr u32 kTevStage2 = 0xD0;
inline constexpr u32 kTevStage3 = 0xD8;
inline constexpr u32 kTevStage4 = 0xF0;
inline constexpr u32 kTevStage5 = 0xF8;
inline constexpr u32 kTevSources = 0, kTevModifiers = 1, kTevOps = 2, kTevConstColor = 3, kTevScales = 4;
inline constexpr u32 kFogModeAndCombiner = 0xE0;      // fog_mode 0-2, update_mask_rgb 8-11, update_mask_a 12-15, fog_flip 16
inline constexpr u32 kFogColor = 0xE1;                // r 0-7, g 8-15, b 16-23
inline constexpr u32 kTevCombinerBufferColor = 0xFD;

// framebuffer (output merger)
inline constexpr u32 kFragmentOperation = 0x100;      // mode bits 0-1, alphablend_enable bit 8
inline constexpr u32 kAlphaBlending = 0x101;
inline constexpr u32 kLogicOp = 0x102;                // bits 0-3
inline constexpr u32 kBlendConst = 0x103;             // r 0-7, g 8-15, b 16-23, a 24-31
inline constexpr u32 kAlphaTest = 0x104;              // enable bit 0, func 4-6, ref 8-15
inline constexpr u32 kGasFirst = 0x120;               // the 16 gas registers (padding in Azahar's FramebufferRegs)
inline constexpr u32 kGasCount = 16;
inline constexpr u32 kFramebufferShadow = 0x130;      // constant bits 0-15, linear bits 16-31 (f16)

// lighting
inline constexpr u32 kLight0 = 0x140;                 // 8 lights of 0x10 words
inline constexpr u32 kLightStride = 0x10;
inline constexpr u32 kLightSpecular0 = 0, kLightSpecular1 = 1, kLightDiffuse = 2, kLightAmbient = 3;
inline constexpr u32 kLightPositionXY = 4, kLightPositionZ = 5, kLightSpotXY = 6, kLightSpotZ = 7;
inline constexpr u32 kLightConfig = 9, kLightDistAttenBias = 10, kLightDistAttenScale = 11;
inline constexpr u32 kLightingGlobalAmbient = 0x1C0;  // r 20-29, g 10-19, b 0-9
inline constexpr u32 kLightingMaxLightIndex = 0x1C2;  // bits 0-2
inline constexpr u32 kLightingConfig0 = 0x1C3;
inline constexpr u32 kLightingConfig1 = 0x1C4;
inline constexpr u32 kLightingDisable = 0x1C6;        // bit 0
inline constexpr u32 kLightingAbsLutInput = 0x1D0;
inline constexpr u32 kLightingLutInput = 0x1D1;
inline constexpr u32 kLightingLutScale = 0x1D2;
inline constexpr u32 kLightingLightEnable = 0x1D9;    // slot n in bits 4n..4n+2

} // namespace freg

// The registers whose change can change the fragment key (a conservative cover: the key is read again
// when any of them is dirty).
constexpr bool IsFragmentKeyRegister(u32 id) {
    return id == freg::kScissorMode || id == freg::kDepthmapEnable || (id >= 0x80 && id < 0x140) ||
           (id >= 0x140 && id < 0x1E0);
}

} // namespace mh4n::gpu::fragment
