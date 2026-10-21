// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104), for cecd:u's message HMAC (N1-19).
#pragma once

#include <array>
#include <span>

#include "mh4n/types.h"

namespace mh4n::cecd {

std::array<u8, 32> Sha256(std::span<const u8> data);
std::array<u8, 32> HmacSha256(std::span<const u8> key, std::span<const u8> message);

}  // namespace mh4n::cecd
