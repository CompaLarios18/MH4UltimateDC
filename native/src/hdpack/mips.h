// Mip chains for the HD pack (N2-17).
#pragma once

#include <vector>

#include "mh4n/types.h"

namespace mh4n::hdpack {

// The chain of an RGBA8 image from level 0 (a copy of the input) down to 1x1, each level half
// the one before (rounded down, at least 1). A level pixel is the mean of the 2x2 pixels of the
// level above, with the last row or column repeated where the size is odd. With
// `alpha_weighted` the colour channels are weighted by alpha, so that transparent pixels (whose
// colour is arbitrary) do not bleed into their neighbours; the result is plain RGBA again.
// Integer arithmetic only, so the same input always gives the same bytes.
std::vector<std::vector<u8>> BuildMipChain(const u8* rgba, u32 width, u32 height,
                                           bool alpha_weighted);

} // namespace mh4n::hdpack
