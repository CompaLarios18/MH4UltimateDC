// Derived from Azahar src/common/cityhash.h and src/common/cityhash.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6. Both files carry Google's CityHash notice (MIT
// licence, below) and no other; this file keeps it, so it is MIT-licensed, not GPL.
// Copyright (c) 2011 Google, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// CityHash, by Geoff Pike and Jyrki Alakuijala
//
// CityHash64 of version 1.1, exactly as Azahar has it: the custom-texture hashes of its texture
// packs (src/video_core/rasterizer_cache/rasterizer_cache.h, ComputeHash) are this function over
// the texture's bytes, so a community HD pack finds its files only if every bit matches.
// Only CityHash64 is ported; the 128-bit variants are not used.
#pragma once

#include <cstddef>

#include "mh4n/types.h"

namespace mh4n::gpu::texture {

// Hash function for a byte array (little-endian hosts and big-endian ones give the same value).
u64 CityHash64(const char* buf, std::size_t len);

inline u64 CityHash64(const u8* data, std::size_t len) {
    return CityHash64(reinterpret_cast<const char*>(data), len);
}

} // namespace mh4n::gpu::texture
