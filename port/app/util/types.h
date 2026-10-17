// Fixed-width integer names the app's files use, so that they need nothing from Azahar's
// common/ for them. The same aliases as Azahar's common_types.h, so a file that includes both
// compiles (an alias declared twice for one type is allowed).
#pragma once

#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
