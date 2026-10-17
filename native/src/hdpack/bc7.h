// BC7 encoding for the HD pack (N2-17), a wrapper over bc7enc_rdo's block encoder
// (native/third_party/bc7, MIT or public domain).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::hdpack {

// How hard the encoder tries. The modes it may use are BC7's mode 6 (one subset, 7-bit
// endpoints, 4-bit indices, with alpha) and, when partitions are searched, mode 1 (two subsets,
// opaque blocks only).
struct Bc7Quality {
    u32 uber_level = 0;      // 0 to 4
    u32 max_partitions = 16; // 0 disables mode 1; at most 64
};
// "fast" (mode 6 only), "normal" (the default: mode 6 and mode 1 with 16 partitions) and "high";
// false for any other name.
bool ParseBc7Quality(std::string_view name, Bc7Quality& out);

// Encodes a width x height RGBA8 image (R first, top row first) into BC7 blocks, row-major, one
// 16-byte block per 4x4 pixels; the right and bottom edges repeat their last pixel. `out` is
// (ceil(w/4) * ceil(h/4) * 16) bytes. Thread-safe.
void EncodeBc7(const u8* rgba, u32 width, u32 height, const Bc7Quality& quality, u8* out);

} // namespace mh4n::hdpack
