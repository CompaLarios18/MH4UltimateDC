// The standard reflected CRC-32 (polynomial 0xEDB88320, as zlib's crc32 and the code library's
// text_crc32) (N1-01).
#pragma once

#include <cstddef>

#include "mh4n/types.h"

namespace mh4n {

// CRC-32 of `size` bytes. Pass the previous result as `crc` to continue over a second block:
// Crc32(b, nb, Crc32(a, na)) == Crc32 of a followed by b.
u32 Crc32(const void* data, std::size_t size, u32 crc = 0);

} // namespace mh4n
