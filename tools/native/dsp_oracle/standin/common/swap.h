// Derived in part from Azahar src/common/swap.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "version 2.0 or later versions"): the names LETag, BETag, AddEndian and the _le aliases.
// Copyright (c) 2012- PPSSPP Project / Dolphin Project (swap.h)
//
// Stand-in of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py) for common/swap.h: the host and
// the 3DS are little-endian, so every little-endian type is the plain type and the tag keeps the
// bytes. (The big-endian types are not used by the audio code.)
#pragma once

#include "common/common_types.h"

struct KeepTag {};
struct SwapTag {};

template <typename T, typename Tag>
struct AddEndian {
    using type = T;
};

using LETag = KeepTag;
using BETag = SwapTag;

using u16_le = u16;
using u32_le = u32;
using u64_le = u64;
using s16_le = s16;
using s32_le = s32;
using s64_le = s64;
using float_le = float;
using double_le = double;
