// Helpers shared by the files of mh4n_kernel (not a public header).
#pragma once

#include "mh4n/clock.h"
#include "mh4n/kernel.h"

namespace mh4n::kernel_detail {

// Game-clock ticks in `ns` nanoseconds (clock.h's conversion, 128-bit exact). Negative values give 0.
inline u64 NsToTicks(s64 ns) {
    if (ns <= 0) {
        return 0;
    }
    return ::mh4n::NsToTicks(static_cast<u64>(ns));
}

} // namespace mh4n::kernel_detail
