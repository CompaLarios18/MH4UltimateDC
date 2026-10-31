// Derived from Azahar src/video_core/shader/generator/pica_fs_config.h (the `StructHash` scheme and its
// LAYOUT_HASH and FIELD_HASH macros) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says
// "Licensed under GPLv3 or any later version"). The mixing function (`HashCombine`) is ours, so the values are not
// Azahar's.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_fs_config.h)
//
// Version hashes of the structures that are saved or compared across runs (N2-33), in the manner of Azahar's
// `StructHash`: a consteval function per structure that mixes its version number, its size, its alignment and the
// offset and size of each member, plus the hashes of the nested structures.
//
// A structure `T` writes
//
//     static consteval u64 StructHash() {
//         constexpr u64 STRUCT_VERSION = 0;
//         using T = Self;
//         return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(a), MH4N_FIELD_HASH(b),
//                            Nested::StructHash());
//     }
//
// and a stored list (N2-16) keeps the combined hash in its header: a change of a member's offset or
// size, or a new member, changes the value. Replacing a member by another of the same type in the same
// place does not, so such a change bumps STRUCT_VERSION by hand.
#pragma once

#include <cstddef>

#include "mh4n/types.h"

namespace mh4n::gpu::fragment {

// The finalizer of splitmix64.
constexpr u64 HashMix(u64 x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

constexpr u64 HashCombine(u64 seed, u64 value) {
    return HashMix(seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2)));
}

template <class... Rest>
constexpr u64 HashCombine(u64 seed, u64 value, Rest... rest) {
    return HashCombine(HashCombine(seed, value), static_cast<u64>(rest)...);
}

} // namespace mh4n::gpu::fragment

#define MH4N_LAYOUT_HASH static_cast<::mh4n::u64>(sizeof(T)), static_cast<::mh4n::u64>(alignof(T))
#define MH4N_FIELD_HASH(x) static_cast<::mh4n::u64>(offsetof(T, x)), static_cast<::mh4n::u64>(sizeof(x))
