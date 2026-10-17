// Derived from Azahar src/video_core/texture/texture_decode.cpp, etc1.cpp,
// src/video_core/rasterizer_cache/texture_codec.h, pixel_format.h, rasterizer_cache.h,
// src/video_core/custom_textures/custom_tex_manager.cpp, src/video_core/utils.h,
// src/common/color.h, src/video_core/pica/regs_texturing.h and regs_internal.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version"): the expected values are worked out by hand from
// their formulas (cited at each check), and the register positions are theirs.
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (texture_decode.cpp)
// Copyright 2017 Citra Emulator Project (etc1.cpp)
// Copyright 2022-2026 Citra Emulator Project / Azahar Emulator Project (texture_codec.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pixel_format.h,
//   custom_tex_manager.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_cache.h)
// Copyright 2014 Citra Emulator Project (utils.h, color.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h)
//
// The CityHash64 test vectors and the pseudorandom test data below come from CityHash's own
// test (city-test.cc, https://github.com/google/cityhash), whose licence is:
//
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
// mh4n_gpu_texture (N2-14): the texture decoders, the custom-texture hashes, the cache and
// `--check tex-hashes`.
//
// Where an expected value is a hash it did not come from this code: the CityHash64 values are the
// published ones (city-test.cc), the XXH3 values and the hashes of the test textures were
// computed by a separate Python implementation written from the algorithm (cross-checked against
// all 300 published vectors), and the bytes a decoder must produce are worked out in the comments.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "gpu/texture/cache.h"
#include "gpu/texture/cityhash.h"
#include "gpu/texture/decode.h"
#include "gpu/texture/hash.h"
#include "gpu/texture/tex_hashes.h"
#include "gpu/texture/texture_regs.h"
#include "gpu_capture/capture_host.h"
#include "gpu_capture/mgpu.h"
#include "gpu_capture/player.h"
#include "mh4n/options.h"

#if defined(_WIN32)
#include <process.h>
#define MH4N_GETPID _getpid
#else
#include <unistd.h>
#define MH4N_GETPID getpid
#endif

using namespace mh4n;
using namespace mh4n::gpu::texture;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<u8>;

// ---- city-test.cc: the data and the expected CityHash64 of each test ------------------------
constexpr u64 kCityK0 = 0xc3a5c85c97cb3127ULL;
constexpr size_t kCityDataSize = 1 << 20;
constexpr int kCityTestSize = 300;

// setup() of city-test.cc
const Bytes& CityData() {
    static const Bytes data = [] {
        Bytes bytes(kCityDataSize);
        u64 a = 9;
        u64 b = 777;
        for (size_t i = 0; i < kCityDataSize; i++) {
            a += b;
            b += a;
            a = (a ^ (a >> 41)) * kCityK0;
            b = (b ^ (b >> 41)) * kCityK0 + i;
            bytes[i] = static_cast<u8>(b >> 37);
        }
        return bytes;
    }();
    return data;
}

// testdata[i][0] of city-test.cc: CityHash64 of data[i * i, i * i + i) for i < 299, and of the
// whole 1 MiB for the last.
constexpr u64 kCityVectors[kCityTestSize] = {
    0x9ae16a3b2f90404fULL,     0x541150e87f415e96ULL,     0x0f3786a4b25827c1ULL,
    0xef923a7a1af78eabULL,     0x11df592596f41d88ULL,     0x831f448bdc5600b3ULL,
    0x3eca803e70304894ULL,     0x1b5a063fb4c7f9f1ULL,     0xa0f10149a0e538d6ULL,
    0xfb8d9c70660b910bULL,     0x236827beae282a46ULL,     0xc385e435136ecf7cULL,
    0xe3f6828b6017086dULL,     0x851fff285561dca0ULL,     0x61152a63595a96d9ULL,
    0x44473e03be306c88ULL,     0x03ead5f21d344056ULL,     0x6abbfde37ee03b5bULL,
    0x0943e7ed63b3c080ULL,     0xd72ce05171ef8a1aULL,     0x4182832b52d63735ULL,
    0xd6cdae892584a2cbULL,     0x5c8e90bc267c5ee4ULL,     0xbbd7f30ac310a6f3ULL,
    0x36a097aa49519d97ULL,     0x0dc78cb032c49217ULL,     0x441593e0da922dfeULL,
    0x2ba3883d71cc2133ULL,     0xf2b6d2adf8423600ULL,     0x38fffe7f3680d63cULL,
    0xb7477bf0b9ce37c6ULL,     0x55bdb0e71e3edebdULL,     0x0782fa1b08b475e7ULL,
    0xc5dc19b876d37a80ULL,     0x5e1141711d2d6706ULL,     0x782edf6da001234fULL,
    0xd26285842ff04d44ULL,     0xc6ab830865a6bae6ULL,     0x044b3a1929232892ULL,
    0x4b603d7932a8de4fULL,     0x4ec0b54cf1566affULL,     0xed8b7a4b34954ff7ULL,
    0x5d28b43694176c26ULL,     0x6a1ef3639e1d202eULL,     0x159f4d9e0307b111ULL,
    0xcc0a840725a7e25bULL,     0xa2b27ee22f63c3f1ULL,     0xd8f2f234899bcab3ULL,
    0x584f28543864844fULL,     0xa94be46dd9aa41afULL,     0x9a87bea227491d20ULL,
    0x27688c24958d1a5cULL,     0x5d1d37790a1873adULL,     0x1f03fd18b711eea9ULL,
    0xf0316f286cf527b6ULL,     0x297008bcb3e3401dULL,     0x043c6252411ee3beULL,
    0xce38a9a54fad6599ULL,     0x0270a9305fef70cfULL,     0xe71be7c28e84d119ULL,
    0xb5b58c24b53aaa19ULL,     0x44dd59bd301995cfULL,     0xb4d4789eb6f2630bULL,
    0x12807833c463737cULL,     0xe88419922b87176fULL,     0x105191e0ec8f7f60ULL,
    0xa5b88bf7399a9f07ULL,     0xd08c3f5747d84f50ULL,     0x2f72d12a40044b4bULL,
    0xaa1f61fdc5c2e11eULL,     0x9489b36fe2246244ULL,     0x358d7c0476a044cdULL,
    0xb0c48df14275265aULL,     0xdaa70bb300956588ULL,     0x4ec97a20b6c4c7c2ULL,
    0x5c3323628435a2e8ULL,     0xc1ef26bea260abdbULL,     0x6be7381b115d653aULL,
    0xae3eece1711b2105ULL,     0x376c28588b8fb389ULL,     0x58d943503bb6748fULL,
    0xdfff5989f5cfd9a1ULL,     0x7fb19eb1a496e8f5ULL,     0x5dba5b0dadccdbaaULL,
    0x688bef4b135a6829ULL,     0xd8323be05433a412ULL,     0x3b5404278a55a7fcULL,
    0x2a96a3f96c5e9bbcULL,     0x22bebfdcc26d18ffULL,     0x627a2249ec6bbcc2ULL,
    0x3abaf1667ba2f3e0ULL,     0x3931ac68c5f1b2c9ULL,     0xb98fb0606f416754ULL,
    0x7f7729a33e58fcc4ULL,     0x42a0aa9ce82848b3ULL,     0x6b2c6d38408a4889ULL,
    0x930380a3741e862aULL,     0x94808b5d2aa25f9aULL,     0xb31abb08ae6e3d38ULL,
    0xdccb5534a893ea1aULL,     0x6369163565814de6ULL,     0xedee4ff253d9f9b3ULL,
    0x941993df6e633214ULL,     0x859838293f64cd4cULL,     0xc19b5648e0d9f555ULL,
    0xf963b63b9006c248ULL,     0x6a8aa0852a8c1f3bULL,     0x740428b4d45e5fb8ULL,
    0x658b883b3a872b86ULL,     0x6df0a977da5d27d4ULL,     0xa900275464ae07efULL,
    0x810bc8aa0c40bcb0ULL,     0x22036327deb59ed7ULL,     0x7d14dfa9772b00c8ULL,
    0x2d777cddb912675dULL,     0xf2ec98824e8aa613ULL,     0x5e763988e21f487fULL,
    0x48949dc327bb96adULL,     0xb7c4209fb24a85c5ULL,     0x9c9e5be0943d4b05ULL,
    0x3898bca4dfd6638dULL,     0x5b5d2557400e68e7ULL,     0xa927ed8b2bf09bb6ULL,
    0x8d25746414aedf28ULL,     0xb5bbdb73458712f2ULL,     0x3d32a26e3ab9d254ULL,
    0x9371d3c35fa5e9a5ULL,     0xcbaa3cb8f64f54e0ULL,     0xb2e23e8116c2ba9fULL,
    0x8aa77f52d7868eb9ULL,     0x858fea922c7fe0c3ULL,     0x46ef25fdec8392b1ULL,
    0x8d078f726b2df464ULL,     0x35ea86e6960ca950ULL,     0x8aee9edbc15dd011ULL,
    0xc3e142ba98432ddaULL,     0x123ba6b99c8cd8dbULL,     0xba87acef79d14f53ULL,
    0x0bcd3957d5717dc3ULL,     0x61442ff55609168eULL,     0xdbe4b1b2d174757fULL,
    0x531e8e77b363161cULL,     0xf71e9c926d711e2bULL,     0xcb20ac28f52df368ULL,
    0xe4a794b4acb94b55ULL,     0xcb942e91443e7208ULL,     0xecca7563c203f7baULL,
    0x1652cb940177c8b5ULL,     0x31fed0fc04c13ce8ULL,     0xe7b668947590b9b3ULL,
    0x1de2119923e8ef3cULL,     0x1269df1e69e14fa7ULL,     0x820826d7aba567ffULL,
    0xffe0547e4923cef9ULL,     0x72da8d1b11d8bc8bULL,     0xd62ab4e3f88fc797ULL,
    0xd0f06c28c7b36823ULL,     0x99b7042460d72ec6ULL,     0x4f4dfcfc0ec2bae5ULL,
    0xfe86bf9d4422b9aeULL,     0xa90d81060932dbb0ULL,     0x17938a1b0e7f5952ULL,
    0xde9e0cb0e16f6e6dULL,     0x6d4b876d9b146d1aULL,     0xe698fa3f54e6ea22ULL,
    0x7bc0deed4fb349f7ULL,     0xdb4b15e88533f622ULL,     0x922834735e86ecb2ULL,
    0x30f1d72c812f1eb8ULL,     0x168884267f3817e9ULL,     0x82e78596ee3e56a7ULL,
    0xaa2d6cf22e3cc252ULL,     0x7bf5ffd7f69385c7ULL,     0xe89c8ff9f9c6e34bULL,
    0xa18fbcdccd11e1f4ULL,     0x2d54f40cc4088b17ULL,     0x69276946cb4e87c7ULL,
    0x668174a3f443df1dULL,     0x05e29be847bd5046ULL,     0xcd0d79f2164da014ULL,
    0xe0e6fc0b1628af1dULL,     0x2058927664adfd93ULL,     0xdc107285fd8e1af7ULL,
    0xfbba1afe2e3280f1ULL,     0xbfa10785ddc1011bULL,     0x534cc35f0ee1eb4eULL,
    0x07ca6e3933995dacULL,     0xf0d6044f6efd7598ULL,     0x3d69e52049879d61ULL,
    0x79da242a16acae31ULL,     0x461c82656a74fb57ULL,     0x053c1a66d0b13003ULL,
    0x0d3a2efec0f047e9ULL,     0x43c64d7484f7f9b2ULL,     0xa7dec6ad81cf7fa1ULL,
    0x05408a1df99d4affULL,     0xa8b27a6bcaeeed4bULL,     0x9a952a8246fdc269ULL,
    0xc930841d1d88684fULL,     0x94dc6971e3cf071aULL,     0x07fc98006e25cac9ULL,
    0x0bd781c4454103f6ULL,     0xda60e6b14479f9dfULL,     0x04ca56a348b6c4d3ULL,
    0xebd22d4b70946401ULL,     0x03cc4693d6cbcb0cULL,     0x38908e43f7ba5ef0ULL,
    0x34983ccc6aa40205ULL,     0x86215c45dcac9905ULL,     0x420fc255c38db175ULL,
    0x1d7a31f5bc8fe2f9ULL,     0x94129a84c376a26eULL,     0x1d3a9809dab05c8dULL,
    0x90fa3ccbd60848daULL,     0x2dbb4fc71b554514ULL,     0xb98bf4274d18374aULL,
    0xd6781d0b5e18eb68ULL,     0x226651cf18f4884cULL,     0xa734fb047d3162d6ULL,
    0xc6df6364a24f75a3ULL,     0x0d8d1364c1fbcd10ULL,     0xaae06f9146db885fULL,
    0x8955ef07631e3bccULL,     0xad611c609cfbe412ULL,     0xd5339adc295d5d69ULL,
    0x40d0aeff521375a8ULL,     0x8b2d54ae1a3df769ULL,     0x99c175819b4eae28ULL,
    0x2a418335779b82fcULL,     0x3b1fc6a3d279e67dULL,     0xd97eacdf10f1c3c9ULL,
    0x293a5c1c4e203cd4ULL,     0x4290e018ffaedde7ULL,     0xf919a59cbde8bf2fULL,
    0x1d70a3f5521d7fa4ULL,     0x6af98d7b656d0d7cULL,     0x395b7a8adb96ab75ULL,
    0x3822dd82c7df012fULL,     0x79f7efe4a80b951aULL,     0xae6e59f5f055921aULL,
    0x8959dbbf07387d36ULL,     0x4739613234278a49ULL,     0x420e6c926bc54841ULL,
    0xc8601bab561bc1b7ULL,     0xb2d294931a0e20ebULL,     0x7966f53c37b6c6d7ULL,
    0xbe9bb0abd03b7368ULL,     0xa08d128c5f1649beULL,     0x7c386f0ffe0465acULL,
    0x0bb362094e7ef4f8ULL,     0xcd80dea24321eea4ULL,     0xd599a04125372c3aULL,
    0x0dbbf541e9dfda0aULL,     0xc2ee3288be4fe2bfULL,     0xd86603ced1ed4730ULL,
    0x915263c671b28809ULL,     0x2b67cdd38c307a5eULL,     0x2d107419073b9cd0ULL,
    0xf3e9487ec0e26dfcULL,     0x1160987c8fe86f7dULL,     0xeab8112c560b967bULL,
    0x1addcf0386d35351ULL,     0xd445ba84bf803e09ULL,     0x37235a096a8be435ULL,
    0x763ad6ea2fe1c99dULL,     0xea627fc84cd1b857ULL,     0x1f2ffd79f2cdc0c8ULL,
    0x39a9e146ec4b3210ULL,     0x74cba303e2dd9d6dULL,     0x4cbc2b73a43071e0ULL,
    0x875638b9715d2221ULL,     0xfb686b2782994a8dULL,     0xab21d81a911e6723ULL,
    0x33d013cc0cd46ecfULL,     0x8ca92c7cd39fae5dULL,     0xfdde3b03f018f43eULL,
    0x9c8502050e9c9458ULL,     0x348176ca2fa2fdd2ULL,     0x4a3d3dfbbaea130bULL,
    0xb371f768cdf4edb9ULL,     0x07a1d2e96934f61fULL,     0x8be53d466d4728f2ULL,
    0x829677eb03abf042ULL,     0x0754435bae3496fcULL,     0xfda9877ea8e3805fULL,
    0x2e36f523ca8f5eb5ULL,     0x21a378ef76828208ULL,     0xccdd5600054b16caULL,
    0x7854468f4e0cabd0ULL,     0x7f88db5346d8f997ULL,     0xbb3fb5fb01d60fcfULL,
    0x2e783e1761acd84dULL,     0x392058251cf22accULL,     0xadf5c1e5d6419947ULL,
    0x6bc1db2c2bee5abaULL,     0xb00f898229efa508ULL,     0xb56eb769ce0d9a8cULL,
    0x70c0637675b94150ULL,     0x74c0b8a6821faafeULL,     0x5fb5e48ac7b7fa4fULL
};

Bytes CitySlice(size_t offset, size_t length) {
    const Bytes& data = CityData();
    return Bytes(data.begin() + offset, data.begin() + offset + length);
}

// ---- the 8x8 tile -------------------------------------------------------------------------------
// The picture in video_core/utils.h: the Morton index of texel (x, y), the bottom row (y = 0) first.
constexpr u32 kGrid[8][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21},     {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},   {10, 11, 14, 15, 26, 27, 30, 31},
    {32, 33, 36, 37, 48, 49, 52, 53}, {34, 35, 38, 39, 50, 51, 54, 55},
    {40, 41, 44, 45, 56, 57, 60, 61}, {42, 43, 46, 47, 58, 59, 62, 63},
};

// A tile of `size` bytes where byte k is (7k + 3) & 255.
Bytes PatternTile(size_t size) {
    Bytes tile(size);
    for (size_t k = 0; k < size; ++k) {
        tile[k] = static_cast<u8>((7 * k + 3) & 0xFF);
    }
    return tile;
}

u8 P(size_t k) {
    return static_cast<u8>((7 * k + 3) & 0xFF);
}

Rgba Look(const Bytes& tile, u32 x, u32 y, Format format, bool disable_alpha = false) {
    return LookupTexelInTile(tile.data(), x, y, format, disable_alpha);
}

constexpr Rgba R(u8 r, u8 g, u8 b, u8 a) {
    return {r, g, b, a};
}

// ---- hand-built ETC1 blocks ---------------------------------------------------------------------
// Block layout (etc1.cpp, ETC1Tile): bits 0-15 the low bit of each texel's modifier index (texel
// index 4x + y), 16-31 its sign, bit 32 flip, bit 33 differential mode, bits 34-36 table 2,
// 37-39 table 1, then the base colours: separate mode r1 r2 g1 g2 b1 b2 as nibbles from bit 60
// down; differential mode r g b as 5 bits and a signed 3-bit delta each. Modifier table:
// {2,8} {5,17} {9,29} {13,42} {18,60} {24,80} {33,106} {47,183}.

// Separate colours, no flip. r1 r2 = 8 C, g1 g2 = 4 A, b1 b2 = 2 6 -> bits 56-63 = 0x8C,
// 48-55 = 0x4A, 40-47 = 0x26. Table 1 = 2, table 2 = 7: bits 32-39 = (2 << 5) | (7 << 2) = 0x5C.
// Low halves: lsb 0xA53C, sign 0x0FF0 (texels 4-11 negative).
constexpr u64 kBlockSeparate = 0x8C4A265C0FF0A53CULL;
// Base 1 = (0x88, 0x44, 0x22) with table 1 (9, 29) for columns 0-1; base 2 = (0xCC, 0xAA, 0x66)
// with table 2 (47, 183) for columns 2-3. lsb of texel i = bit i of 0xA53C:
//   i:   0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
//   lsb: 0 0 1 1 1 1 0 0 1 0 1  0  0  1  0  1
// sign (0x0FF0): texels 4..11 are negative.
constexpr Rgba kSeparateTexel[16] = {
    // column 0 (+9, +9, +29, +29 on base 1)
    R(145, 77, 43, 255), R(145, 77, 43, 255), R(165, 97, 63, 255), R(165, 97, 63, 255),
    // column 1 (-29, -29, -9, -9)
    R(107, 39, 5, 255), R(107, 39, 5, 255), R(127, 59, 25, 255), R(127, 59, 25, 255),
    // column 2 (base 2: -183, -47, -183, -47, clamped at 0)
    R(21, 0, 0, 255), R(157, 123, 55, 255), R(21, 0, 0, 255), R(157, 123, 55, 255),
    // column 3 (+47, +183, +47, +183, clamped at 255)
    R(251, 217, 149, 255), R(255, 255, 255, 255), R(251, 217, 149, 255), R(255, 255, 255, 255),
};

// Differential colours, flip. r = 8, dr = +3 (bits 56-63 = 8 << 3 | 3 = 0x43); g = 16, dg = -2
// (110b: 16 << 3 | 6 = 0x86); b = 4, db = +1 (4 << 3 | 1 = 0x21). Table 1 = 0, table 2 = 5, flip,
// differential: bits 32-39 = (0 << 5) | (5 << 2) | 2 | 1 = 0x17. lsb 0x00FF (texels 0-7 use the
// larger modifier), sign 0xFF00 (texels 8-15 negative).
constexpr u64 kBlockDifferential = 0x43862117FF0000FFULL;
// Base 1 = Convert5To8 of (8, 16, 4) = (64 | 2, 128 | 4, 32 | 1) = (66, 132, 33); base 2 = of
// (11, 14, 5) = (88 | 2, 112 | 3, 40 | 1) = (90, 115, 41). With flip the halves are split by row:
// rows 0-1 use base 1 and table 1 (2, 8), rows 2-3 base 2 and table 2 (24, 80).
constexpr Rgba kDifferentialTexel[16] = {
    // column 0 (+8 on base 1 for rows 0-1, +80 on base 2 for rows 2-3)
    R(74, 140, 41, 255), R(74, 140, 41, 255), R(170, 195, 121, 255), R(170, 195, 121, 255),
    // column 1 (same)
    R(74, 140, 41, 255), R(74, 140, 41, 255), R(170, 195, 121, 255), R(170, 195, 121, 255),
    // column 2 (-2 on base 1, -24 on base 2)
    R(64, 130, 31, 255), R(64, 130, 31, 255), R(66, 91, 17, 255), R(66, 91, 17, 255),
    // column 3 (same)
    R(64, 130, 31, 255), R(64, 130, 31, 255), R(66, 91, 17, 255), R(66, 91, 17, 255),
};

void PutBlock(Bytes& out, u64 value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<u8>(value >> (8 * i)));
    }
}

// ---- a scratch directory ------------------------------------------------------------------------
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("mh4n_gpu_texture_" + std::to_string(MH4N_GETPID()) + "_" + std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    fs::path operator/(const std::string& name) const {
        return path_ / name;
    }

private:
    fs::path path_;
};

std::string ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

// ===============================================================================================
// CityHash64
// ===============================================================================================

TEST_CASE("CityHash64 gives the published test vectors, at every length up to 298 and for 1 MiB") {
    const Bytes& data = CityData();
    int checked = 0;
    for (int i = 0; i < kCityTestSize - 1; ++i) {
        const u64 got = CityHash64(data.data() + static_cast<size_t>(i) * i, static_cast<size_t>(i));
        if (got != kCityVectors[i]) {
            FAIL_CHECK("length " << i << ": got " << std::hex << got << ", expected "
                                 << kCityVectors[i]);
        }
        ++checked;
    }
    CHECK(CityHash64(data.data(), kCityDataSize) == kCityVectors[kCityTestSize - 1]);
    CHECK(checked == 299);
    // The empty string is k2, the first vector.
    CHECK(CityHash64(static_cast<const char*>(nullptr), 0) == 0x9ae16a3b2f90404fULL);
}

// ===============================================================================================
// Formats
// ===============================================================================================

TEST_CASE("tile sizes and texture sizes follow CalculateTileSize") {
    // texture_decode.cpp, CalculateTileSize: bytes of an 8x8 tile
    const struct {
        Format format;
        size_t tile;
    } expected[] = {{Format::RGBA8, 256}, {Format::RGB8, 192}, {Format::RGB5A1, 128},
                    {Format::RGB565, 128}, {Format::RGBA4, 128}, {Format::IA8, 128},
                    {Format::RG8, 128},   {Format::I8, 64},    {Format::A8, 64},
                    {Format::IA4, 64},    {Format::I4, 32},    {Format::A4, 32},
                    {Format::ETC1, 32},   {Format::ETC1A4, 64}};
    for (const auto& e : expected) {
        CAPTURE(static_cast<u32>(e.format));
        CHECK(TileBytes(e.format) == e.tile);
        CHECK(EncodedBytes(e.format, 16, 8) == 2 * e.tile);
        CHECK(EncodedBytes(e.format, 8, 24) == 3 * e.tile);
    }
    // pixel_format.h, FORMAT_MAP: bytes per pixel of the decoded form
    CHECK(HashBytesPerTexel(Format::RGBA8) == 4);
    CHECK(HashBytesPerTexel(Format::RGB8) == 3);
    CHECK(HashBytesPerTexel(Format::RGB5A1) == 2);
    CHECK(HashBytesPerTexel(Format::RGB565) == 2);
    CHECK(HashBytesPerTexel(Format::RGBA4) == 2);
    for (Format f : {Format::IA8, Format::RG8, Format::I8, Format::A8, Format::IA4, Format::I4,
                     Format::A4, Format::ETC1, Format::ETC1A4}) {
        CHECK(HashBytesPerTexel(f) == 4);
    }
    CHECK(IsValidFormat(13));
    CHECK_FALSE(IsValidFormat(14));
    CHECK(IsValidSize(8, 8));
    CHECK_FALSE(IsValidSize(12, 8));
    CHECK_FALSE(IsValidSize(0, 8));
    CHECK_FALSE(IsValidSize(8, 4096));
}

// ===============================================================================================
// Decoders: bytes worked out from texture_decode.cpp and color.h
// ===============================================================================================

TEST_CASE("the Morton index of every texel of a tile is the picture of video_core/utils.h") {
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            CHECK(MortonInterleave(x, y) == kGrid[y][x]);
        }
    }
}

TEST_CASE("RGBA8, RGB8 and the byte formats read the texel at the Morton index of (x, y)") {
    // DecodeRGBA8: {bytes[3], bytes[2], bytes[1], bytes[0]}; DecodeRGB8: {b[2], b[1], b[0], 255};
    // IA8: {b[1], b[1], b[1], b[0]}; RG8: {b[1], b[0], 0, 255}; I8: {i, i, i, 255};
    // A8: {0, 0, 0, a}
    const Bytes rgba8 = PatternTile(256);
    const Bytes rgb8 = PatternTile(192);
    const Bytes two = PatternTile(128);
    const Bytes one = PatternTile(64);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const size_t m = kGrid[y][x];
            CAPTURE(x);
            CAPTURE(y);
            CHECK(Look(rgba8, x, y, Format::RGBA8) ==
                  R(P(4 * m + 3), P(4 * m + 2), P(4 * m + 1), P(4 * m)));
            CHECK(Look(rgb8, x, y, Format::RGB8) == R(P(3 * m + 2), P(3 * m + 1), P(3 * m), 255));
            CHECK(Look(two, x, y, Format::IA8) ==
                  R(P(2 * m + 1), P(2 * m + 1), P(2 * m + 1), P(2 * m)));
            CHECK(Look(two, x, y, Format::RG8) == R(P(2 * m + 1), P(2 * m), 0, 255));
            CHECK(Look(one, x, y, Format::I8) == R(P(m), P(m), P(m), 255));
            CHECK(Look(one, x, y, Format::A8) == R(0, 0, 0, P(m)));
        }
    }
}

TEST_CASE("RGBA8 by hand") {
    Bytes tile(256, 0);
    // texel (0, 0) is Morton index 0: bytes 0x11 0x22 0x33 0x44 -> r = 0x44, g = 0x33, b = 0x22,
    // a = 0x11
    tile[0] = 0x11;
    tile[1] = 0x22;
    tile[2] = 0x33;
    tile[3] = 0x44;
    CHECK(Look(tile, 0, 0, Format::RGBA8) == R(0x44, 0x33, 0x22, 0x11));
    // the debug form keeps the colour and sets alpha to 255
    CHECK(Look(tile, 0, 0, Format::RGBA8, true) == R(0x44, 0x33, 0x22, 255));
    // texel (1, 0) is Morton index 1: bytes 4..7
    tile[4] = 0x01;
    tile[7] = 0xFE;
    CHECK(Look(tile, 1, 0, Format::RGBA8) == R(0xFE, 0, 0, 0x01));
}

TEST_CASE("16-bit colour formats by hand") {
    Bytes tile(128, 0);
    const auto put16 = [&](u32 morton, u16 value) {
        tile[morton * 2] = static_cast<u8>(value & 0xFF);
        tile[morton * 2 + 1] = static_cast<u8>(value >> 8);
    };
    // RGB5A1: r = bits 11-15, g = 6-10, b = 1-5, a = bit 0; Convert5To8(v) = (v << 3) | (v >> 2)
    put16(0, 0xF801);  // r 31 -> 255, g 0, b 0, a 1 -> 255
    put16(1, 0x07C0);  // g 31 -> 255, a 0
    put16(2, 0x003E);  // b 31 -> 255, a 0
    put16(3, 0x4A55);  // r 9 -> 74, g 9 -> 74, b 10 -> 82, a 1 -> 255
    CHECK(Look(tile, 0, 0, Format::RGB5A1) == R(255, 0, 0, 255));
    CHECK(Look(tile, 1, 0, Format::RGB5A1) == R(0, 255, 0, 0));
    CHECK(Look(tile, 0, 1, Format::RGB5A1) == R(0, 0, 255, 0));
    CHECK(Look(tile, 1, 1, Format::RGB5A1) == R(74, 74, 82, 255));
    CHECK(Look(tile, 1, 0, Format::RGB5A1, true) == R(0, 255, 0, 255));

    // RGB565: r = bits 11-15, g = 5-10 with Convert6To8(v) = (v << 2) | (v >> 4), b = 0-4
    put16(0, 0xF800);  // (255, 0, 0)
    put16(1, 0x07E0);  // g 63 -> 252 | 3 = 255
    put16(2, 0x001F);  // b 31 -> 255
    put16(3, 0x8410);  // r 16 -> 128 | 4 = 132, g 32 -> 128 | 2 = 130, b 16 -> 132
    CHECK(Look(tile, 0, 0, Format::RGB565) == R(255, 0, 0, 255));
    CHECK(Look(tile, 1, 0, Format::RGB565) == R(0, 255, 0, 255));
    CHECK(Look(tile, 0, 1, Format::RGB565) == R(0, 0, 255, 255));
    CHECK(Look(tile, 1, 1, Format::RGB565) == R(132, 130, 132, 255));

    // RGBA4: r = bits 12-15, g = 8-11, b = 4-7, a = 0-3; Convert4To8(v) = 17 v
    put16(0, 0x1234);
    CHECK(Look(tile, 0, 0, Format::RGBA4) == R(17, 34, 51, 68));
    CHECK(Look(tile, 0, 0, Format::RGBA4, true) == R(17, 34, 51, 255));

    // IA8: bytes {alpha, intensity}
    tile[0] = 0x80;
    tile[1] = 0x20;
    CHECK(Look(tile, 0, 0, Format::IA8) == R(0x20, 0x20, 0x20, 0x80));
    CHECK(Look(tile, 0, 0, Format::IA8, true) == R(0x20, 0x80, 0, 255)); // intensity red, alpha green
    // RG8: bytes {0x11, 0x22} -> r = 0x22, g = 0x11
    tile[0] = 0x11;
    tile[1] = 0x22;
    CHECK(Look(tile, 0, 0, Format::RG8) == R(0x22, 0x11, 0, 255));
}

TEST_CASE("8-bit and 4-bit formats by hand") {
    Bytes tile(64, 0);
    tile[0] = 0x5A;
    CHECK(Look(tile, 0, 0, Format::I8) == R(0x5A, 0x5A, 0x5A, 255));
    tile[0] = 0xC3;
    CHECK(Look(tile, 0, 0, Format::A8) == R(0, 0, 0, 0xC3));
    CHECK(Look(tile, 0, 0, Format::A8, true) == R(0xC3, 0xC3, 0xC3, 255));
    // IA4: intensity in the high nibble, alpha in the low: 0x3C -> i = 3 * 17 = 51, a = 12 * 17 = 204
    tile[0] = 0x3C;
    CHECK(Look(tile, 0, 0, Format::IA4) == R(51, 51, 51, 204));
    CHECK(Look(tile, 0, 0, Format::IA4, true) == R(51, 204, 0, 255));
    // I4 and A4: two texels per byte, the even Morton index in the low nibble. Byte 0 = 0x9D:
    // texel (0, 0) is index 0 -> 0xD * 17 = 221, texel (1, 0) is index 1 -> 9 * 17 = 153.
    tile[0] = 0x9D;
    CHECK(Look(tile, 0, 0, Format::I4) == R(221, 221, 221, 255));
    CHECK(Look(tile, 1, 0, Format::I4) == R(153, 153, 153, 255));
    CHECK(Look(tile, 0, 0, Format::A4) == R(0, 0, 0, 221));
    CHECK(Look(tile, 1, 0, Format::A4) == R(0, 0, 0, 153));
    CHECK(Look(tile, 1, 0, Format::A4, true) == R(153, 153, 153, 255));
    // texel (2, 0) is Morton index 4 -> byte 2, low nibble
    tile[2] = 0x4B;
    CHECK(Look(tile, 2, 0, Format::I4) == R(187, 187, 187, 255));
    // texel (3, 0) is index 5 -> byte 2, high nibble 4 -> 68
    CHECK(Look(tile, 3, 0, Format::I4) == R(68, 68, 68, 255));
}

TEST_CASE("ETC1: separate colours, differential colours with flip, clamping, and the tile's subtiles") {
    // see the blocks above
    Bytes separate;
    PutBlock(separate, kBlockSeparate);
    Bytes differential;
    PutBlock(differential, kBlockDifferential);

    // a tile is four 4x4 subtiles in the order top-left, top-right, bottom-left, bottom-right
    Bytes tile;
    PutBlock(tile, kBlockSeparate);      // subtile 0: x 0-3, y 0-3
    PutBlock(tile, kBlockDifferential);  // subtile 1: x 4-7, y 0-3
    PutBlock(tile, kBlockDifferential);  // subtile 2: x 0-3, y 4-7
    PutBlock(tile, kBlockSeparate);      // subtile 3: x 4-7, y 4-7
    REQUIRE(tile.size() == 32);

    for (u32 x = 0; x < 4; ++x) {
        for (u32 y = 0; y < 4; ++y) {
            CAPTURE(x);
            CAPTURE(y);
            const u32 i = 4 * x + y;
            CHECK(Look(tile, x, y, Format::ETC1) == kSeparateTexel[i]);
            CHECK(Look(tile, 4 + x, y, Format::ETC1) == kDifferentialTexel[i]);
            CHECK(Look(tile, x, 4 + y, Format::ETC1) == kDifferentialTexel[i]);
            CHECK(Look(tile, 4 + x, 4 + y, Format::ETC1) == kSeparateTexel[i]);
        }
    }
    CHECK(Look(tile, 0, 0, Format::ETC1, true) == kSeparateTexel[0]);
}

TEST_CASE("ETC1A4: the 4-bit alpha of a subtile comes first, texel 4x + y at bits 4 * (4x + y)") {
    // alpha 0xFEDCBA9876543210: nibble i = i -> alpha 17 i for texel index i = 4x + y
    Bytes tile;
    for (int sub = 0; sub < 4; ++sub) {
        PutBlock(tile, 0xFEDCBA9876543210ULL);
        PutBlock(tile, sub % 2 == 0 ? kBlockSeparate : kBlockDifferential);
    }
    REQUIRE(tile.size() == 64);
    for (u32 x = 0; x < 4; ++x) {
        for (u32 y = 0; y < 4; ++y) {
            CAPTURE(x);
            CAPTURE(y);
            const u32 i = 4 * x + y;
            Rgba separate = kSeparateTexel[i];
            separate.a = static_cast<u8>(17 * i);
            Rgba differential = kDifferentialTexel[i];
            differential.a = static_cast<u8>(17 * i);
            CHECK(Look(tile, x, y, Format::ETC1A4) == separate);
            CHECK(Look(tile, 4 + x, y, Format::ETC1A4) == differential);
            CHECK(Look(tile, x, 4 + y, Format::ETC1A4) == separate);
            CHECK(Look(tile, 4 + x, 4 + y, Format::ETC1A4) == differential);
        }
    }
    CHECK(Look(tile, 3, 3, Format::ETC1A4, true).a == 255);
}

TEST_CASE("LookupTexture finds the tile by row and column; DecodeToRgba8 keeps memory order") {
    // 16x16 I8 texture: four tiles, tile t holds the pattern shifted by 64 t
    Bytes texture;
    for (u32 t = 0; t < 4; ++t) {
        for (u32 k = 0; k < 64; ++k) {
            texture.push_back(static_cast<u8>(64 * t + k));
        }
    }
    // texel (9, 3): tile column 1, tile row 0 -> tile 1; in-tile (1, 3) -> Morton 11
    CHECK(LookupTexture(texture.data(), 9, 3, 16, Format::I8) == R(64 + 11, 64 + 11, 64 + 11, 255));
    // texel (2, 12): tile row 1, column 0 -> tile 2; in-tile (2, 4) -> Morton 36
    CHECK(LookupTexture(texture.data(), 2, 12, 16, Format::I8) ==
          R(128 + 36, 128 + 36, 128 + 36, 255));

    Bytes out;
    REQUIRE(DecodeToRgba8(texture.data(), texture.size(), 16, 16, Format::I8, out));
    REQUIRE(out.size() == 16 * 16 * 4);
    // row 0 of the image is the first row of tiles, in memory order: pixel (9, 3) is at row 3
    const size_t at = (3 * 16 + 9) * 4;
    CHECK(out[at] == 64 + 11);
    CHECK(out[at + 3] == 255);

    // refusals
    Bytes untouched = {1, 2, 3};
    CHECK_FALSE(DecodeToRgba8(texture.data(), texture.size() - 1, 16, 16, Format::I8, untouched));
    CHECK_FALSE(DecodeToRgba8(texture.data(), texture.size(), 12, 16, Format::I8, untouched));
    CHECK(untouched == Bytes{1, 2, 3});
}

TEST_CASE("DecodeForHash: rows bottom-up, the five colour formats as they are, the rest as RGBA8") {
    // 8x16 RGB565: two tiles, one above the other in memory. Memory row y of the image (y 0-15)
    // comes out as row 15 - y, its texel x at the bytes of Morton index kGrid[y % 8][x].
    Bytes texture = PatternTile(256);
    for (size_t k = 128; k < 256; ++k) {
        texture[k] = static_cast<u8>(P(k) ^ 0x55); // tile 1 differs from tile 0
    }
    Bytes out;
    REQUIRE(DecodeForHash(texture.data(), texture.size(), 8, 16, Format::RGB565, out));
    REQUIRE(out.size() == 8 * 16 * 2);
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const size_t source = (y / 8) * 128 + kGrid[y % 8][x] * 2;
            const size_t target = ((15 - y) * 8 + x) * 2;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(out[target] == texture[source]);
            CHECK(out[target + 1] == texture[source + 1]);
        }
    }

    // 8x8 RGB8 keeps three bytes per texel
    const Bytes rgb8 = PatternTile(192);
    REQUIRE(DecodeForHash(rgb8.data(), rgb8.size(), 8, 8, Format::RGB8, out));
    REQUIRE(out.size() == 8 * 8 * 3);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            for (u32 c = 0; c < 3; ++c) {
                CHECK(out[((7 - y) * 8 + x) * 3 + c] == rgb8[kGrid[y][x] * 3 + c]);
            }
        }
    }

    // 8x8 IA8 decodes to {i, i, i, a}, bottom-up
    const Bytes ia8 = PatternTile(128);
    REQUIRE(DecodeForHash(ia8.data(), ia8.size(), 8, 8, Format::IA8, out));
    REQUIRE(out.size() == 8 * 8 * 4);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const size_t m = kGrid[y][x];
            const size_t at = ((7 - y) * 8 + x) * 4;
            CHECK(out[at] == ia8[2 * m + 1]);
            CHECK(out[at + 1] == ia8[2 * m + 1]);
            CHECK(out[at + 2] == ia8[2 * m + 1]);
            CHECK(out[at + 3] == ia8[2 * m]);
        }
    }

    // 8x8 ETC1: {r, g, b, 255}; a texel of subtile 0 (block rows 0-3 of memory) lands in rows 7..4
    Bytes etc;
    PutBlock(etc, kBlockSeparate);
    PutBlock(etc, kBlockDifferential);
    PutBlock(etc, kBlockDifferential);
    PutBlock(etc, kBlockSeparate);
    REQUIRE(DecodeForHash(etc.data(), etc.size(), 8, 8, Format::ETC1, out));
    REQUIRE(out.size() == 8 * 8 * 4);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool right = x >= 4, bottom = y >= 4;
            const Rgba expected = (right == bottom) ? kSeparateTexel[4 * (x % 4) + (y % 4)]
                                                    : kDifferentialTexel[4 * (x % 4) + (y % 4)];
            const size_t at = ((7 - y) * 8 + x) * 4;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(Rgba{out[at], out[at + 1], out[at + 2], out[at + 3]} == expected);
        }
    }

    // refusals
    CHECK_FALSE(DecodeForHash(texture.data(), 255, 8, 16, Format::RGB565, out));
    CHECK_FALSE(DecodeForHash(texture.data(), 256, 8, 12, Format::RGB565, out));
}

// ===============================================================================================
// The custom-texture hashes
// ===============================================================================================

namespace {
struct HashCase {
    const char* name;
    Format format;
    u32 width, height;
    size_t offset; // into city-test.cc's data
    u64 new_hash, old_hash;
};

// The textures are slices of city-test.cc's data. The expected hashes were computed by a separate
// Python implementation: the new hash is CityHash64 of the slice (T1's is also published, as
// testdata[256][0] of city-test.cc, and checked against it below); the old hash is CityHash64 of
// the slice decoded as texture_codec.h's MortonCopy with converted = false does (rows bottom-up;
// RGBA8, RGB8, RGB5A1, RGB565 and RGBA4 unchanged, other formats as RGBA8).
const HashCase kHashCases[] = {
    {"8x8 RGBA8", Format::RGBA8, 8, 8, 65536, 0x2B67CDD38C307A5EULL, 0x471DA9AFC32ED3ECULL},
    {"8x16 RGB565", Format::RGB565, 8, 16, 1000, 0x6D87603740F11859ULL, 0x94D16EA0A9D94550ULL},
    {"8x8 IA8", Format::IA8, 8, 8, 5000, 0x97448D3035E03C8BULL, 0x3CB78444A54C7608ULL},
    {"16x8 ETC1", Format::ETC1, 16, 8, 9000, 0x1B686D4481CE3F2AULL, 0x931DF042D0B9D9DAULL},
    {"8x8 I4", Format::I4, 8, 8, 12000, 0x7F79CE5E6D4BDE34ULL, 0xE0F58C647F34D664ULL},
    {"8x8 ETC1A4", Format::ETC1A4, 8, 8, 14000, 0x2A794EEB77FC3153ULL, 0x8DF2BD1A28E860D4ULL},
    {"8x8 RGB8", Format::RGB8, 8, 8, 16000, 0x8B4D8DC8220079A2ULL, 0x5B620CEBDA06AFEAULL},
};
} // namespace

TEST_CASE("both hash modes: new over the raw bytes, old over the decoded texture") {
    for (const HashCase& c : kHashCases) {
        CAPTURE(c.name);
        const Bytes raw = CitySlice(c.offset, EncodedBytes(c.format, c.width, c.height));
        const auto hashes = ComputeCustomHashes(raw.data(), raw.size(), c.width, c.height, c.format);
        REQUIRE(hashes.has_value());
        CHECK(hashes->new_hash == c.new_hash);
        CHECK(hashes->old_hash == c.old_hash);
        // more bytes than the texture are fine: only the texture is hashed
        Bytes longer = raw;
        longer.resize(raw.size() + 100, 0xEE);
        const auto same = ComputeCustomHashes(longer.data(), longer.size(), c.width, c.height, c.format);
        REQUIRE(same.has_value());
        CHECK(same->new_hash == c.new_hash);
        CHECK(same->old_hash == c.old_hash);
    }
    // the published vector of the first case: 256 bytes at offset 256 * 256 = testdata[256]
    CHECK(kHashCases[0].new_hash == kCityVectors[256]);
}

TEST_CASE("the hash covers only the bytes of the texture, and refuses what is not one") {
    const Bytes raw = CitySlice(65536, 256);
    CHECK_FALSE(ComputeCustomHashes(raw.data(), 255, 8, 8, Format::RGBA8).has_value());
    CHECK_FALSE(ComputeCustomHashes(raw.data(), 256, 8, 12, Format::RGBA8).has_value());
    CHECK_FALSE(ComputeCustomHashes(raw.data(), 256, 0, 0, Format::RGBA8).has_value());
    // changing one byte changes both hashes
    Bytes changed = raw;
    changed[100] ^= 0x01;
    const auto a = ComputeCustomHashes(raw.data(), raw.size(), 8, 8, Format::RGBA8);
    const auto b = ComputeCustomHashes(changed.data(), changed.size(), 8, 8, Format::RGBA8);
    CHECK(a->new_hash != b->new_hash);
    CHECK(a->old_hash != b->old_hash);
}

TEST_CASE("the file name stem of a pack texture") {
    // custom_tex_manager.cpp, DumpTexture: "tex1_{}x{}_{:016X}_{}_mip{}.png" without the suffix
    CHECK(CustomTextureStem(64, 32, 0x2B67CDD38C307A5EULL, Format::ETC1) ==
          "tex1_64x32_2B67CDD38C307A5E_12");
    CHECK(CustomTextureStem(8, 8, 0xABCULL, Format::RGBA8) == "tex1_8x8_0000000000000ABC_0");
}

// ===============================================================================================
// The cache
// ===============================================================================================

namespace {
constexpr u32 kFcram = 0x20000000;

// An 8x8 I8 texture whose texel (x, y) holds the Morton index's pattern byte P(m), at `paddr`.
void PlaceI8(gpu_capture::CaptureHost& host, u32 paddr, const Bytes& tile) {
    REQUIRE(host.WriteBoth(paddr, tile.data(), static_cast<u32>(tile.size())));
}
} // namespace

TEST_CASE("the cache decodes once, rehashes only after a notification, and decodes again only on a change") {
    gpu_capture::CaptureHost host;
    const Bytes tile = PatternTile(64);
    PlaceI8(host, kFcram + 0x1000, tile);
    TextureCache cache(host);
    const CacheKey key{kFcram + 0x1000, Format::I8, 8, 8};

    const CachedTexture* first = cache.Get(key);
    REQUIRE(first != nullptr);
    CHECK(cache.stats().misses == 1);
    CHECK(first->generation == 1);
    // XXH3 64-bit of the 64 bytes (7k + 3) & 255, from the Python xxhash package
    CHECK(first->content_hash == 0x287EB1FA9E4BE2C1ULL);
    REQUIRE(first->rgba8.size() == 8 * 8 * 4);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const u8 i = P(kGrid[y][x]);
            const u8* px = &first->rgba8[(y * 8 + x) * 4];
            CHECK(px[0] == i);
            CHECK(px[1] == i);
            CHECK(px[2] == i);
            CHECK(px[3] == 255);
        }
    }

    // memory changes with no notification: the cached copy is returned without reading memory
    Bytes changed = tile;
    changed[5] = 0xEE;
    PlaceI8(host, kFcram + 0x1000, changed);
    const CachedTexture* again = cache.Get(key);
    CHECK(again->generation == 1);
    CHECK(again->rgba8[(1 * 8 + 3) * 4] == P(kGrid[1][3])); // still the old texel
    CHECK(cache.stats().hits == 1);
    CHECK(cache.stats().rehashes == 0);

    // a flush: rehashed, the hash differs, decoded again
    cache.OnFlush(kFcram + 0x1000, 64);
    const CachedTexture* updated = cache.Get(key);
    REQUIRE(updated != nullptr);
    CHECK(cache.stats().rehashes == 1);
    CHECK(cache.stats().redecodes == 1);
    CHECK(updated->generation == 2);
    CHECK(updated->content_hash == 0x57937063098255ADULL); // XXH3 of the changed bytes
    // byte 5 is Morton index 5 = texel (3, 1)
    CHECK(kGrid[1][3] == 7);
    CHECK(kGrid[0][3] == 5);
    CHECK(updated->rgba8[(0 * 8 + 3) * 4] == 0xEE);

    // an invalidate that finds the memory the same: rehashed, not decoded
    cache.OnInvalidate(kFcram + 0x1000, 64);
    const CachedTexture* same = cache.Get(key);
    CHECK(same->generation == 2);
    CHECK(cache.stats().rehashes == 2);
    CHECK(cache.stats().unchanged == 1);
    CHECK(cache.stats().redecodes == 1);

    // and then it is a plain hit again
    cache.Get(key);
    CHECK(cache.stats().rehashes == 2);
}

TEST_CASE("the safety net: every notification marks every texture; without it only the overlapping ones") {
    gpu_capture::CaptureHost host;
    PlaceI8(host, kFcram + 0x1000, PatternTile(64));
    PlaceI8(host, kFcram + 0x3000, PatternTile(64));
    const CacheKey a{kFcram + 0x1000, Format::I8, 8, 8};
    const CacheKey b{kFcram + 0x3000, Format::I8, 8, 8};

    {
        TextureCache cache(host); // the safety net is on by default
        CHECK(cache.config().check_all_on_notify);
        cache.Get(a);
        cache.Get(b);
        cache.OnFlush(kFcram + 0x1000, 4); // names only a
        cache.Get(a);
        cache.Get(b);
        CHECK(cache.stats().rehashes == 2); // b was rehashed too
    }
    {
        TextureCache cache(host, {.check_all_on_notify = false});
        cache.Get(a);
        cache.Get(b);
        cache.OnFlush(kFcram + 0x1000, 4);
        cache.Get(a);
        cache.Get(b);
        CHECK(cache.stats().rehashes == 1); // only a
        // ranges: a ends at +64; a flush that starts at the end or ends at the start misses it
        cache.OnFlush(kFcram + 0x1040, 16);
        cache.OnFlush(kFcram + 0x0FF0, 16);
        cache.Get(a);
        CHECK(cache.stats().rehashes == 1);
        cache.OnGpuWrite(kFcram + 0x103F, 1); // its last byte
        cache.Get(a);
        CHECK(cache.stats().rehashes == 2);
        cache.OnFlush(kFcram + 0x0FF8, 16); // its first bytes
        cache.Get(a);
        CHECK(cache.stats().rehashes == 3);
    }
}

TEST_CASE("textures are keyed by address, format and size; unreadable ones are refused") {
    gpu_capture::CaptureHost host;
    PlaceI8(host, kFcram + 0x1000, PatternTile(128));
    TextureCache cache(host);
    const CachedTexture* i8 = cache.Get({kFcram + 0x1000, Format::I8, 8, 8});
    const CachedTexture* a8 = cache.Get({kFcram + 0x1000, Format::A8, 8, 8});
    const CachedTexture* wide = cache.Get({kFcram + 0x1000, Format::I8, 16, 8});
    REQUIRE(i8 != nullptr);
    REQUIRE(a8 != nullptr);
    REQUIRE(wide != nullptr);
    CHECK(cache.size() == 3);
    CHECK(i8->rgba8[3] == 255);
    CHECK(a8->rgba8[0] == 0);
    CHECK(a8->rgba8[3] == P(0));
    CHECK(wide->rgba8.size() == 16 * 8 * 4);

    CHECK(cache.Get({kFcram + 0x1000, Format::I8, 12, 8}) == nullptr); // not whole tiles
    CHECK(cache.Get({0x30000000, Format::I8, 8, 8}) == nullptr);      // no memory there
    CHECK(cache.stats().failures == 2);
    cache.Clear();
    CHECK(cache.size() == 0);
}

// ===============================================================================================
// Texture registers
// ===============================================================================================

TEST_CASE("the textures of a draw: units, types, cube faces") {
    // Register ids of regs_internal.h: main_config 0x80, texture0 0x81..0x85 (border, size,
    // params, lod, address), cube_address 0x86..0x8A, texture0_format 0x8E, texture1 0x91..0x95,
    // texture1_format 0x96, texture2 0x99..0x9D, texture2_format 0x9E.
    std::array<u32, 0x300> regs{};
    regs[0x80] = 0b111;
    // unit 0: 64 wide, 32 high, type 2D (bits 28-30 = 0), address 0x20002000 / 8, RGB565 (3)
    regs[0x82] = (64u << 16) | 32u;
    regs[0x83] = 0;
    regs[0x85] = 0x20002000u / 8;
    regs[0x8E] = 3;
    // unit 1: 8x8 ETC1 (12) at 0x18000000 (VRAM)
    regs[0x92] = (8u << 16) | 8u;
    regs[0x95] = 0x18000000u / 8;
    regs[0x96] = 12;
    // unit 2: 16x16 with format 14, which does not exist
    regs[0x9A] = (16u << 16) | 16u;
    regs[0x9D] = 0x20004000u / 8;
    regs[0x9E] = 14;

    u32 skipped = 0;
    auto textures = TexturesOfDraw(regs.data(), &skipped);
    REQUIRE(textures.size() == 2);
    CHECK(skipped == 1);
    CHECK(textures[0] == TextureRef{0, 0, false, 0x20002000, 64, 32, Format::RGB565});
    CHECK(textures[1] == TextureRef{1, 0, false, 0x18000000, 8, 8, Format::ETC1});

    // projection: one texture; a unit that is not enabled is not read
    regs[0x83] = 3u << 28;
    regs[0x80] = 0b001;
    textures = TexturesOfDraw(regs.data());
    REQUIRE(textures.size() == 1);
    CHECK(textures[0].paddr == 0x20002000);

    // shadow types are not colour textures
    regs[0x83] = 2u << 28;
    CHECK(TexturesOfDraw(regs.data(), &skipped).empty());
    CHECK(skipped == 1);
    regs[0x83] = 4u << 28;
    CHECK(TexturesOfDraw(regs.data()).empty());

    // cube: six faces. The main address holds bits 0-27; faces 1-5 take bits 0-21 from
    // cube_address[0..4] and keep bits 22-27 of the main address (GetCubePhysicalAddress).
    regs[0x83] = 1u << 28;
    regs[0x85] = (0x5u << 22) | 0x100000u;       // +X
    regs[0x86] = 0x100100;                       // -X
    regs[0x87] = 0x100200;                       // +Y
    regs[0x88] = 0x100300;                       // -Y
    regs[0x89] = 0x100400;                       // +Z
    regs[0x8A] = 0x100500;                       // -Z
    textures = TexturesOfDraw(regs.data());
    REQUIRE(textures.size() == 6);
    for (u32 face = 0; face < 6; ++face) {
        CAPTURE(face);
        CHECK(textures[face].cube);
        CHECK(textures[face].face == face);
        CHECK(textures[face].paddr == ((0x5u << 22) | (0x100000u + 0x100 * face)) * 8);
        CHECK(textures[face].width == 64);
    }
}

// ===============================================================================================
// --check tex-hashes on a synthetic capture
// ===============================================================================================

namespace {

void Put16(Bytes& b, u32 v) {
    b.push_back(static_cast<u8>(v));
    b.push_back(static_cast<u8>(v >> 8));
}
void Put32(Bytes& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void Put64(Bytes& b, u64 v) {
    Put32(b, static_cast<u32>(v));
    Put32(b, static_cast<u32>(v >> 32));
}

class Writer {
public:
    Writer() {
        out.insert(out.end(), {'M', 'G', 'P', 'U'});
        Put32(out, 1);
    }
    void Rec(u32 type, const Bytes& payload) {
        Put32(out, type);
        Put32(out, frame);
        Put64(out, ticks += 1000);
        Put32(out, static_cast<u32>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
    }
    void Init() {
        Bytes payload;
        Put32(payload, 11);
        for (u32 kind = 1; kind <= 11; ++kind) {
            const Bytes block(gpu_capture::InitBlockSize(kind), 0);
            Put32(payload, kind);
            Put32(payload, static_cast<u32>(block.size()));
            payload.insert(payload.end(), block.begin(), block.end());
        }
        Rec(gpu_capture::kInit, payload);
    }
    void Mem(u32 addr, const Bytes& data) {
        Bytes p;
        Put32(p, addr);
        Put32(p, static_cast<u32>(data.size()));
        p.insert(p.end(), data.begin(), data.end());
        Rec(gpu_capture::kMem, p);
    }
    void SubmitList(u32 paddr, u32 size, const std::vector<std::array<u32, 2>>& writes) {
        Bytes gx;
        for (u32 w : {1u, paddr, size, 0u, 0u, 0u, 0u, 0u}) Put32(gx, w);
        Rec(gpu_capture::kGx, gx);
        Bytes p;
        Put32(p, static_cast<u32>(writes.size()));
        for (const auto& w : writes) {
            Put16(p, w[0]);
            p.push_back(0xF);
            p.push_back(0);
            Put32(p, w[1]);
        }
        Rec(gpu_capture::kPicaReg, p);
    }
    void Frame(u32 number) {
        frame = number; // the FRAME record's header carries its own number
        Bytes p;
        Put32(p, number);
        Rec(gpu_capture::kFrame, p);
    }
    bool Save(const fs::path& path) const {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<bool>(f);
    }

    Bytes out;
    u32 frame = 0;
    u64 ticks = 0;
};

// A command list of single-register writes: [value, header] pairs (8 bytes each).
Bytes ListBytes(const std::vector<std::array<u32, 2>>& writes) {
    Bytes list;
    for (const auto& w : writes) {
        Put32(list, w[1]);
        Put32(list, w[0] | (0xFu << 16)); // register id, all four bytes
    }
    return list;
}

} // namespace

TEST_CASE("--check tex-hashes lists the textures the draws of a capture use, in both modes") {
    TempDir dir;
    // Memory: command list at +0x1000; unit 0's texture (8x16 RGB565) at +0x2000 and unit 1's (8x8
    // IA8) at +0x3000, slices of city-test.cc's data (cases 2 and 3 above).
    const HashCase& rgb565 = kHashCases[1];
    const HashCase& ia8 = kHashCases[2];
    const std::vector<std::array<u32, 2>> writes = {
        {0x80, 0b011},                             // units 0 and 1 on
        {0x82, (8u << 16) | 16u},                  // unit 0: 8 wide, 16 high
        {0x83, 0},                                 //         type 2D
        {0x85, (kFcram + 0x2000) / 8},             //         address
        {0x8E, 3},                                 //         RGB565
        {0x92, (8u << 16) | 8u},                   // unit 1: 8x8
        {0x95, (kFcram + 0x3000) / 8},
        {0x96, 5},                                 //         IA8
        {0x22E, 0},                                // draw
        {0x22E, 0},                                // the same textures again
    };
    const Bytes list = ListBytes(writes);

    Bytes memory(0x4000, 0);
    std::copy(list.begin(), list.end(), memory.begin() + 0x1000);
    const Bytes tex0 = CitySlice(rgb565.offset, 256);
    const Bytes tex1 = CitySlice(ia8.offset, 128);
    std::copy(tex0.begin(), tex0.end(), memory.begin() + 0x2000);
    std::copy(tex1.begin(), tex1.end(), memory.begin() + 0x3000);

    Writer w;
    w.Init();
    w.Mem(kFcram, memory);
    w.SubmitList(kFcram + 0x1000, static_cast<u32>(list.size()), writes);
    w.Frame(1);
    const fs::path capture = dir / "synthetic.mgpu";
    REQUIRE(w.Save(capture));

    const fs::path output = dir / "hashes.txt";
    std::ostringstream out, err;
    const int status = gpu_capture::RunCapturePlayer(
        {capture.string(), "--gpu", "replay", "--judge", "none", "--check", "tex-hashes",
         "--tex-hashes-out", output.string()},
        out, err);
    INFO(out.str());
    INFO(err.str());
    REQUIRE(status == 0);
    CHECK(out.str().find("tex_new") != std::string::npos);

    // one line per distinct texture, in order of first use, although both draws use both
    char expected[512];
    std::snprintf(expected, sizeof(expected),
                  "# mh4n tex-hashes 1\n"
                  "# use_new_hash=1\n"
                  "%016llX 8x16 3\n"
                  "%016llX 8x8 5\n"
                  "# use_new_hash=0\n"
                  "%016llX 8x16 3\n"
                  "%016llX 8x8 5\n",
                  static_cast<unsigned long long>(rgb565.new_hash),
                  static_cast<unsigned long long>(ia8.new_hash),
                  static_cast<unsigned long long>(rgb565.old_hash),
                  static_cast<unsigned long long>(ia8.old_hash));
    CHECK(ReadFile(output) == expected);
}

TEST_CASE("--check tex-hashes: the text format, and the default output file") {
    CHECK(FormatTexHashes({}, {}) == "# mh4n tex-hashes 1\n# use_new_hash=1\n# use_new_hash=0\n");
    CHECK(FormatTexHashes({{0xABCULL, 64, 32, Format::ETC1}}, {{0x1ULL, 8, 8, Format::RGBA8}}) ==
          "# mh4n tex-hashes 1\n# use_new_hash=1\n0000000000000ABC 64x32 12\n"
          "# use_new_hash=0\n0000000000000001 8x8 0\n");

    // without --tex-hashes-out the list goes next to the capture
    TempDir dir;
    Writer w;
    w.Init();
    w.Frame(1);
    const fs::path capture = dir / "empty.mgpu";
    REQUIRE(w.Save(capture));
    std::ostringstream out, err;
    CHECK(gpu_capture::RunCapturePlayer({capture.string(), "--gpu", "replay", "--judge", "none",
                                         "--check", "tex-hashes"},
                                        out, err) == 0);
    CHECK(fs::exists(capture.string() + ".tex-hashes.txt"));
}
