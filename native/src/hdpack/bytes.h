// Little-endian reads and writes of the archive and the journal (N2-17).
#pragma once

#include "mh4n/types.h"

namespace mh4n::hdpack {

inline void Put32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<u8>(v >> (8 * i));
    }
}
inline void Put64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<u8>(v >> (8 * i));
    }
}
inline u32 Get32(const u8* p) {
    u32 v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}
inline u64 Get64(const u8* p) {
    u64 v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

} // namespace mh4n::hdpack
