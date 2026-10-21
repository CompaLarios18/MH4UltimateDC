// SHA-256 (FIPS 180-4), for cfg's GetTransferableId, which hashes with Azahar's CryptoPP::SHA256.
#pragma once

#include <array>
#include <cstddef>

#include "mh4n/types.h"

namespace mh4n::cfg {

std::array<u8, 32> Sha256(const u8* data, std::size_t size);

}  // namespace mh4n::cfg
