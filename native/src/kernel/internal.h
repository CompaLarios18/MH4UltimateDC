// Helpers shared by the files of mh4n_kernel (not a public header).
#pragma once

#include "mh4n/kernel.h"

namespace mh4n::kernel_detail {

inline constexpr u64 kTicksPerSecond = 268111856;  // Azahar's BASE_CLOCK_RATE_ARM11

// Game-clock ticks in `ns` nanoseconds: ns * 268,111,856 / 1e9, rounded down, through a 128-bit
// product (ns * 268111856 overflows 64 bits after about 68 s). Negative values give 0.
inline u64 NsToTicks(s64 ns) {
    if (ns <= 0) {
        return 0;
    }
    const unsigned __int128 product = static_cast<unsigned __int128>(static_cast<u64>(ns)) *
                                      kTicksPerSecond;
    return static_cast<u64>(product / 1000000000u);  // at most 2.5e18: fits
}

} // namespace mh4n::kernel_detail
