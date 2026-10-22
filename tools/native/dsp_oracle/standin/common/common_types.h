// Derived in part from Azahar src/common/common_types.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "either version 2 of the License, or (at your option) any later version"): the integer and address type names.
// Copyright (C) 2005-2012 Gekko Emulator (common_types.h)
//
// Stand-in of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py) for common/common_types.h: the
// integer types, f32 and the two address types the audio code uses, and nothing else.
#pragma once

#include <array>
#include <cstdint>

typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;

typedef std::int8_t s8;
typedef std::int16_t s16;
typedef std::int32_t s32;
typedef std::int64_t s64;

typedef float f32;
typedef double f64;

typedef u32 VAddr;
typedef u32 PAddr;
