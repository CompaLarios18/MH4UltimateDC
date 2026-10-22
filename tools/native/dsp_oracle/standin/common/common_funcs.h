// Derived in part from Azahar src/common/common_funcs.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): INSERT_PADDING_BYTES and its helpers.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (common_funcs.h)
// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project (common_funcs.h, second notice)
//
// Stand-in of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py) for common/common_funcs.h: the
// padding macros of the shared-memory structures, and nothing else.
#pragma once

#include "common/common_types.h"

#define CONCAT2(x, y) DO_CONCAT2(x, y)
#define DO_CONCAT2(x, y) x##y

#define INSERT_PADDING_BYTES(num_bytes) u8 CONCAT2(pad, __LINE__)[(num_bytes)]
#define INSERT_PADDING_WORDS(num_words) u32 CONCAT2(pad, __LINE__)[(num_words)]
