// Derived in part from Azahar src/common/assert.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the macro names and what they do (ASSERT and its relatives end the process, UNIMPLEMENTED only logs).
// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project (assert.h)
//
// Stand-in of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py) for common/assert.h: a failed
// ASSERT aborts, as Azahar's does after logging (a case that reaches one is a bad case); UNIMPLEMENTED
// does nothing, as Azahar's only logs and carries on.
#pragma once

#include <cstdio>
#include <cstdlib>

#define ASSERT(_a_)                                                                                \
    do {                                                                                           \
        if (!(_a_)) {                                                                              \
            std::fprintf(stderr, "dsp oracle: assertion failed: %s (%s:%d)\n", #_a_, __FILE__,    \
                         __LINE__);                                                                \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)

#define ASSERT_MSG(_a_, ...) ASSERT(_a_)
#define UNREACHABLE() (std::fprintf(stderr, "dsp oracle: unreachable\n"), std::abort())
#define UNREACHABLE_MSG(...) UNREACHABLE()
#define DEBUG_ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, ...)
#define UNIMPLEMENTED() (void(0))
#define UNIMPLEMENTED_MSG(...) (void(0))
