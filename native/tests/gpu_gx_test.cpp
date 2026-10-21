// Derived from Azahar src/video_core/renderer_software/sw_blitter.cpp, src/video_core/gpu.cpp,
// src/video_core/pica/regs_external.h, src/video_core/utils.h, src/common/color.h,
// src/core/memory.h and src/core/memory.cpp (the physical memory areas, IsValidPhysicalAddress) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of these files says
// "GPLv2 or any later version"): the algorithms that the expected values below are worked out
// from, and the register layouts.
// Copyright 2023 Citra Emulator Project (sw_blitter.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, regs_external.h)
// Copyright 2014 Citra Emulator Project (utils.h, color.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h, memory.cpp)
//
// mh4n_gpu_gx (N2-03): the GX engine on the CPU, with synthetic images. Every expected value is
// worked out by hand from the algorithm of Azahar's SwBlitter / GPU::Execute (the derivation is
// next to it), never by running the code under test.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "gpu/gx/blitter.h"
#include "gpu/gx/engine.h"
#include "gpu_capture/capture_host.h"
#include "mh4n/gpu.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

using namespace mh4n;
using namespace mh4n::gpu;
using namespace mh4n::gpu::gx;
using mh4n::gpu_capture::CaptureHost;
using mh4n::gpu_capture::SignalledInterrupt;

namespace {

// Virtual addresses of the linear heap (physical 0x20000000 +) and VRAM (physical 0x18000000 +).
constexpr u32 kRam = 0x14000000;
constexpr u32 kRamPhys = 0x20000000;
constexpr u32 kVram = 0x1F000000;
constexpr u32 kVramPhys = 0x18000000;

// The 8x8 tile of Azahar's utils.h comment, row y = 0 first: the index of the texel (x, y).
constexpr u32 kTile[8][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21},   // y = 0
    {2, 3, 6, 7, 18, 19, 22, 23},   // y = 1
    {8, 9, 12, 13, 24, 25, 28, 29}, // y = 2
    {10, 11, 14, 15, 26, 27, 30, 31},
    {32, 33, 36, 37, 48, 49, 52, 53},
    {34, 35, 38, 39, 50, 51, 54, 55},
    {40, 41, 44, 45, 56, 57, 60, 61},
    {42, 43, 46, 47, 58, 59, 62, 63}, // y = 7
};

struct Fixture {
    CaptureHost host;
    GxEngine gpu{host};

    u8* Mem(u32 paddr, u32 size = 1) {
        u8* p = host.PhysicalPointer(paddr, size);
        REQUIRE(p != nullptr);
        return p;
    }
    void PutBytes(u32 paddr, std::initializer_list<u8> bytes) {
        u8* p = Mem(paddr, static_cast<u32>(bytes.size()));
        for (u8 b : bytes) {
            *p++ = b;
        }
    }
    void PutVec(u32 paddr, const std::vector<u8>& bytes) {
        u8* p = Mem(paddr, static_cast<u32>(bytes.size()));
        std::memcpy(p, bytes.data(), bytes.size());
    }
    void Put32(u32 paddr, u32 index, u32 value) {
        u8* p = Mem(paddr + index * 4, 4);
        for (int i = 0; i < 4; ++i) {
            p[i] = static_cast<u8>(value >> (8 * i));
        }
    }
    u32 Get32(u32 paddr, u32 index) {
        const u8* p = Mem(paddr + index * 4, 4);
        return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24);
    }
    void FillBytes(u32 paddr, u32 size, u8 value) {
        std::memset(Mem(paddr, size), value, size);
    }
    std::vector<u8> Read(u32 paddr, u32 size) {
        const u8* p = Mem(paddr, size);
        return {p, p + size};
    }
    std::vector<SignalledInterrupt> Signals() {
        return host.TakeSignalled();
    }

    void Run(std::initializer_list<u32> words) {
        GxCommand command{};
        size_t i = 0;
        for (u32 w : words) {
            command.words[i++] = w;
        }
        gpu.Execute(command);
    }
    void Fill(u32 start, u32 value, u32 end, u32 control) {
        Run({2, start, value, end, 0, 0, 0, control});
    }
    void Transfer(u32 in, u32 out, u32 in_size, u32 out_size, u32 flags) {
        Run({3, in, out, in_size, out_size, flags});
    }
    void Copy(u32 in, u32 out, u32 size, u32 in_gap, u32 out_gap) {
        Run({4, in, out, size, in_gap, out_gap, 8});
    }
};

// The flags word of a transfer: bit 0 flip, 1 input linear, 2 crop, 5 dont_swizzle, formats at
// bits 8 and 12, scaling at bit 24.
constexpr u32 Flags(u32 flip, u32 linear, u32 crop, u32 dont_swizzle, u32 in_format,
                    u32 out_format, u32 scaling) {
    return flip | (linear << 1) | (crop << 2) | (dont_swizzle << 5) | (in_format << 8) |
           (out_format << 12) | (scaling << 24);
}
constexpr u32 Size(u32 width, u32 height) {
    return width | (height << 16);
}
// An RGBA8 texel as the u32 the memory holds (little-endian): the bytes are A, B, G, R.
constexpr u32 Rgba(u32 r, u32 g, u32 b, u32 a) {
    return (r << 24) | (g << 16) | (b << 8) | a;
}

using Signals = std::vector<SignalledInterrupt>;

} // namespace

// ================================================================================================
// Memory fill
// ================================================================================================

TEST_CASE("fill: 32-bit values, the interrupt and the delay") {
    Fixture f;
    // 0x11223344 is stored little-endian: 44 33 22 11, repeated over the 64 KiB range.
    f.FillBytes(kVramPhys, 0x10010, 0xEE);
    f.Fill(kVram, 0x11223344, kVram + 0x10000, 0x201); // trigger + fill_32bit (bit 9)
    const auto bytes = f.Read(kVramPhys, 0x10010);
    for (u32 i = 0; i < 0x10000; ++i) {
        const u8 expected[4] = {0x44, 0x33, 0x22, 0x11};
        REQUIRE(bytes[i] == expected[i % 4]);
    }
    CHECK(bytes[0x10000] == 0xEE); // one past the end is untouched
    // PSC0 (a single buffer). Delay: the fill counts as a texture transfer VRAM -> VRAM, 5400 MiB/s:
    // 65536 B * 1e9 / (5400 * 1048576) = 65536e9 / 5662310400 = 11574.07 -> 11574 ns.
    const Signals signals = f.Signals();
    REQUIRE(signals.size() == 1);
    CHECK(signals[0].id == Interrupt::PSC0);
    CHECK(signals[0].delay_ns == 11574);
}

TEST_CASE("fill: 16-bit values") {
    Fixture f;
    f.FillBytes(kRamPhys, 16, 0xEE);
    // Neither bit 8 nor bit 9: 16-bit fill with the low half of the value, 0xABCD -> CD AB.
    f.Fill(kRam, 0xFFFFABCD, kRam + 8, 0x001);
    CHECK(f.Read(kRamPhys, 10) ==
          std::vector<u8>{0xCD, 0xAB, 0xCD, 0xAB, 0xCD, 0xAB, 0xCD, 0xAB, 0xEE, 0xEE});
}

TEST_CASE("fill: 24-bit values, the last unit overruns the range as in Azahar") {
    Fixture f;
    f.FillBytes(kRamPhys, 16, 0xEE);
    // Bytes 0, 1, 2 of the value are R, G, B and are written in that order: 0x00CCBBAA -> AA BB CC.
    // The range is 8 bytes: units start at 0, 3 and 6 (the loop is `ptr < end`), so the third
    // unit writes bytes 6, 7 and 8 -- byte 8 is past the end.
    f.Fill(kRam, 0x00CCBBAA, kRam + 8, 0x101); // trigger + fill_24bit (bit 8)
    CHECK(f.Read(kRamPhys, 10) ==
          std::vector<u8>{0xAA, 0xBB, 0xCC, 0xAA, 0xBB, 0xCC, 0xAA, 0xBB, 0xCC, 0xEE});
}

TEST_CASE("fill: 24-bit wins over 32-bit when both bits are set") {
    Fixture f;
    f.Fill(kRam, 0x00CCBBAA, kRam + 8, 0x301);
    CHECK(f.Read(kRamPhys, 3) == std::vector<u8>{0xAA, 0xBB, 0xCC});
}

TEST_CASE("fill: two buffers signal PSC0 once, the second alone signals PSC1") {
    Fixture f;
    // GPU::Execute: with both start addresses set, the first unit signals nothing and the
    // second PSC0; with only the second set, PSC1; with only the first, PSC0.
    // 64 bytes, VRAM source -> delay 64e9 / 5662310400 = 11.30 -> 11 ns for both destinations.
    f.Run({2, kVram, 0x01020304, kVram + 64, kRam, 0x05060708, kRam + 64, 0x02010201});
    Signals s = f.Signals();
    REQUIRE(s.size() == 1);
    CHECK(s[0].id == Interrupt::PSC0);
    CHECK(s[0].delay_ns == 11);
    CHECK(f.Get32(kVramPhys, 0) == 0x01020304);
    CHECK(f.Get32(kRamPhys, 15) == 0x05060708);
    CHECK(f.Get32(kRamPhys, 16) == 0);

    f.Run({2, 0, 0, 0, kRam, 0x0A0B0C0D, kRam + 64, 0x02010000});
    s = f.Signals();
    REQUIRE(s.size() == 1);
    CHECK(s[0].id == Interrupt::PSC1);
    CHECK(f.Get32(kRamPhys, 0) == 0x0A0B0C0D);

    f.Run({2, kVram + 128, 0x00000001, kVram + 192, 0, 0, 0, 0x00000201});
    s = f.Signals();
    REQUIRE(s.size() == 1);
    CHECK(s[0].id == Interrupt::PSC0);
}

TEST_CASE("fill: without the trigger bit nothing happens") {
    Fixture f;
    f.FillBytes(kRamPhys, 16, 0xEE);
    f.Fill(kRam, 0x11111111, kRam + 8, 0x200); // fill_32bit, no trigger
    CHECK(f.Read(kRamPhys, 8) == std::vector<u8>(8, 0xEE));
    CHECK(f.Signals().empty());
}

TEST_CASE("fill: an empty or backwards range writes nothing but still signals") {
    Fixture f;
    f.FillBytes(kRamPhys, 16, 0xEE);
    f.Fill(kRam + 8, 0x11111111, kRam + 8, 0x201);
    f.Fill(kRam + 8, 0x11111111, kRam, 0x201);
    CHECK(f.Read(kRamPhys, 16) == std::vector<u8>(16, 0xEE));
    // GPU::MemoryFill signals as long as the start address is not zero. The backwards range has a
    // u32 difference of 0xFFFFFFF8: 4294967288 B * 1e9 / (5400 * 1048576) = 758.5e6 ns.
    const Signals s = f.Signals();
    REQUIRE(s.size() == 2);
    CHECK(s[0].delay_ns == 0);
    CHECK(s[1].delay_ns == 758'518'517);
}

TEST_CASE("fill: the registers show finished and no trigger afterwards") {
    Fixture f;
    f.Fill(kRam, 0x11111111, kRam + 8, 0x201);
    // memory_fill_config[0].control is word 7 of the GPU registers: trigger cleared, finished
    // (bit 1) set, the other bits kept: 0x201 -> 0x202.
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * 7) == 0x202);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * 4) == (kRamPhys >> 3));
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * 5) == ((kRamPhys + 8) >> 3));
}

TEST_CASE("fill: through the registers (WriteReg)") {
    Fixture f;
    f.gpu.WriteReg(0x1EF00000 + 4 * 8, kVramPhys >> 3);        // unit 1: start
    f.gpu.WriteReg(0x1EF00000 + 4 * 9, (kVramPhys + 16) >> 3); // end
    f.gpu.WriteReg(0x1EF00000 + 4 * 10, 0xCAFEBABE);           // value
    CHECK(f.Signals().empty());
    f.gpu.WriteReg(0x1EF00000 + 4 * 11, 0x201); // control: trigger + 32-bit
    CHECK(f.Get32(kVramPhys, 0) == 0xCAFEBABE);
    CHECK(f.Get32(kVramPhys, 3) == 0xCAFEBABE);
    CHECK(f.Get32(kVramPhys, 4) == 0);
    const Signals s = f.Signals();
    REQUIRE(s.size() == 1);
    CHECK(s[0].id == Interrupt::PSC1); // MemoryFill(1, 1)
}

// ================================================================================================
// DMA
// ================================================================================================

TEST_CASE("dma: copies and signals with the delay of the copy mode") {
    // 1 MiB. Speeds (MiB/s, DMA): RAM->RAM 190, RAM->VRAM 310, VRAM->RAM 380, VRAM->VRAM 380;
    // 1 MiB / (S MiB/s) = 1e9 / S ns: 5263157.89, 3225806.45, 2631578.9 -> truncated.
    struct Case {
        u32 src, dst, src_phys, dst_phys;
        u64 delay;
    };
    const Case cases[] = {
        {kRam, kRam + 0x200000, kRamPhys, kRamPhys + 0x200000, 5263157},
        {kRam, kVram, kRamPhys, kVramPhys, 3225806},
        {kVram, kRam, kVramPhys, kRamPhys, 2631578},
        {kVram, kVram + 0x200000, kVramPhys, kVramPhys + 0x200000, 2631578},
    };
    for (const Case& c : cases) {
        Fixture f;
        f.PutBytes(c.src_phys, {1, 2, 3, 4, 5});
        f.Mem(c.src_phys + 0xFFFFF)[0] = 0x77;
        f.Run({0, c.src, c.dst, 0x100000});
        CHECK(f.Read(c.dst_phys, 5) == std::vector<u8>{1, 2, 3, 4, 5});
        CHECK(f.Mem(c.dst_phys + 0xFFFFF)[0] == 0x77);
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].id == Interrupt::DMA);
        CHECK(s[0].delay_ns == c.delay);
    }
}

TEST_CASE("dma: an address the host cannot translate copies nothing and still signals") {
    Fixture f;
    f.FillBytes(kRamPhys, 16, 0xEE);
    f.Run({0, 0x08000000, kRam, 16});
    CHECK(f.Read(kRamPhys, 16) == std::vector<u8>(16, 0xEE));
    const Signals s = f.Signals();
    REQUIRE(s.size() == 1);
    CHECK(s[0].id == Interrupt::DMA);
}

// ================================================================================================
// Display transfer
// ================================================================================================

namespace {

// 16x16 RGBA8 image, texel (x, y) = id y * 16 + x: R = id, G = 255 - id, B = 0x11, A = 0xFF.
u32 Texel(u32 x, u32 y) {
    const u32 id = y * 16 + x;
    return Rgba(id, 255 - id, 0x11, 0xFF);
}

} // namespace

TEST_CASE("transfer: linear to tiled, 16x16 RGBA8") {
    Fixture f;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            f.Put32(kRamPhys, y * 16 + x, Texel(x, y));
        }
    }
    // input_linear, tiles on the output (dont_swizzle = 0), RGBA8 -> RGBA8.
    f.Transfer(kRam, kRam + 0x10000, Size(16, 16), Size(16, 16), Flags(0, 1, 0, 0, 0, 0, 0));
    // Output: tiles of 64 texels, two tiles per row of tiles (width 16 / 8); the row of tiles
    // costs coarse_y * stride = 8 * (16 * 4) bytes = 128 texels, the tile (tx, ty) therefore
    // starts at texel (ty * 2 + tx) * 64 (coarse_x * 8 gives tx * 64), and the texel inside it is
    // kTile[y % 8][x % 8].
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const u32 index = ((y / 8) * 2 + (x / 8)) * 64 + kTile[y % 8][x % 8];
            INFO("texel " << x << "," << y);
            REQUIRE(f.Get32(kRamPhys + 0x10000, index) == Texel(x, y));
        }
    }
    // PPF; delay: 16 * 16 * 4 = 1024 B, FCRAM -> FCRAM texture speed 450 MiB/s:
    // 1024e9 / (450 * 1048576) = 2170.1 -> 2170 ns.
    const Signals s = f.Signals();
    REQUIRE(s.size() == 1);
    CHECK(s[0].id == Interrupt::PPF);
    CHECK(s[0].delay_ns == 2170);
}

TEST_CASE("transfer: tiled to linear, 16x16 RGBA8") {
    Fixture f;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const u32 index = ((y / 8) * 2 + (x / 8)) * 64 + kTile[y % 8][x % 8];
            f.Put32(kRamPhys, index, Texel(x, y));
        }
    }
    f.Transfer(kRam, kRam + 0x10000, Size(16, 16), Size(16, 16), Flags(0, 0, 0, 0, 0, 0, 0));
    // The output is linear: texel (x, y) at y * 16 + x.
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            INFO("texel " << x << "," << y);
            REQUIRE(f.Get32(kRamPhys + 0x10000, y * 16 + x) == Texel(x, y));
        }
    }
}

TEST_CASE("transfer: tiled to tiled (dont_swizzle) and the vertical flip") {
    Fixture f;
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            f.Put32(kRamPhys, kTile[y][x], Texel(x, y));
        }
    }
    f.Transfer(kRam, kRam + 0x10000, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 1, 0, 0, 0));
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            REQUIRE(f.Get32(kRamPhys + 0x10000, kTile[y][x]) == Texel(x, y));
        }
    }
    // Flipped: the texel read from row y is written to row 7 - y.
    f.Transfer(kRam, kRam + 0x20000, Size(8, 8), Size(8, 8), Flags(1, 0, 0, 1, 0, 0, 0));
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            REQUIRE(f.Get32(kRamPhys + 0x20000, kTile[7 - y][x]) == Texel(x, y));
        }
    }
}

TEST_CASE("transfer: linear to linear with the flip") {
    Fixture f;
    f.Put32(kRamPhys, 0, 0x11111111); // row 0
    f.Put32(kRamPhys, 1, 0x22222222); // row 1
    f.Transfer(kRam, kRam + 0x100, Size(1, 2), Size(1, 2), Flags(1, 1, 0, 1, 0, 0, 0));
    // output_y = height - y - 1: row 0 of the input lands in row 1.
    CHECK(f.Get32(kRamPhys + 0x100, 0) == 0x22222222);
    CHECK(f.Get32(kRamPhys + 0x100, 1) == 0x11111111);
}

TEST_CASE("transfer: flip with crop_input_lines skews the output as on hardware") {
    Fixture f;
    // Input 4x2, output 2x2, linear to linear. dst_addr += (in_w - out_w) * (out_h - 1) * 4 =
    // (4 - 2) * 1 * 4 = 8 bytes. The texel (x, y) goes to ((x + (1 - y) * 2) * 4) + 8.
    for (u32 i = 0; i < 8; ++i) {
        f.Put32(kRamPhys, i, 0x01010101 * (i + 1)); // p0..p7, row 0 = p0..p3
    }
    f.Transfer(kRam, kRam + 0x100, Size(4, 2), Size(2, 2), Flags(1, 1, 1, 1, 0, 0, 0));
    // p0 (0,0) -> byte 16 (word 4); p1 (1,0) -> byte 20 (word 5); p4 (0,1) -> byte 8 (word 2);
    // p5 (1,1) -> byte 12 (word 3). Words 0 and 1 (before the shifted start) stay empty.
    CHECK(f.Get32(kRamPhys + 0x100, 0) == 0);
    CHECK(f.Get32(kRamPhys + 0x100, 1) == 0);
    CHECK(f.Get32(kRamPhys + 0x100, 2) == 0x05050505);
    CHECK(f.Get32(kRamPhys + 0x100, 3) == 0x06060606);
    CHECK(f.Get32(kRamPhys + 0x100, 4) == 0x01010101);
    CHECK(f.Get32(kRamPhys + 0x100, 5) == 0x02020202);
}

TEST_CASE("transfer: output formats from one RGBA8 texel") {
    // The texel R = FF, G = 80, B = 40, A = C0 sits in memory as the bytes A, B, G, R:
    // C0 40 80 FF. Encoders (Color.h): RGB565 = r>>3 << 11 | g>>2 << 5 | b>>3
    //   = 31 << 11 | 32 << 5 | 8 = 0xF800 | 0x0400 | 0x0008 = 0xFC08 -> bytes 08 FC;
    // RGB5A1 = 31 << 11 | 16 << 6 | 8 << 1 | (A>>7 = 1) = 0xF800 | 0x0400 | 0x0010 | 1 = 0xFC11
    //   -> 11 FC;
    // RGBA4 = F << 12 | 8 << 8 | 4 << 4 | C = 0xF84C -> 4C F8;
    // RGB8 stores B, G, R: 40 80 FF.
    struct Case {
        u32 format;
        std::vector<u8> bytes;
    };
    const Case cases[] = {
        {0, {0xC0, 0x40, 0x80, 0xFF}}, {1, {0x40, 0x80, 0xFF}}, {2, {0x08, 0xFC}},
        {3, {0x11, 0xFC}},             {4, {0x4C, 0xF8}},
    };
    for (const Case& c : cases) {
        Fixture f;
        f.PutBytes(kRamPhys, {0xC0, 0x40, 0x80, 0xFF});
        f.FillBytes(kRamPhys + 0x100, 8, 0xEE);
        f.Transfer(kRam, kRam + 0x100, Size(1, 1), Size(1, 1), Flags(0, 1, 0, 1, 0, c.format, 0));
        std::vector<u8> expected = c.bytes;
        expected.resize(5, 0xEE); // the byte after the texel is not touched
        INFO("output format " << c.format);
        CHECK(f.Read(kRamPhys + 0x100, 5) == expected);
    }
}

TEST_CASE("transfer: input formats into RGBA8") {
    // Decoders (Color.h) and the bit expansions: 5 bits v -> v << 3 | v >> 2, 6 bits -> v << 2 |
    // v >> 4, 4 bits -> v << 4 | v, 1 bit -> 255 * v. RGBA8 is stored as the bytes A, B, G, R.
    struct Case {
        u32 format;
        std::vector<u8> input;
        std::vector<u8> output; // A, B, G, R
    };
    const Case cases[] = {
        // RGB565 0xFC08: r = 31 -> 248 | 7 = 255, g = 32 -> 128 | 2 = 130 = 0x82,
        // b = 8 -> 64 | 2 = 66 = 0x42, a = 255.
        {2, {0x08, 0xFC}, {0xFF, 0x42, 0x82, 0xFF}},
        // RGB5A1 0xFC11: r = 31 -> 255, g = 16 -> 128 | 4 = 132 = 0x84, b = 8 -> 0x42, a = 1 -> 255.
        {3, {0x11, 0xFC}, {0xFF, 0x42, 0x84, 0xFF}},
        // RGBA4 0xF84C: r = F -> FF, g = 8 -> 88, b = 4 -> 44, a = C -> CC.
        {4, {0x4C, 0xF8}, {0xCC, 0x44, 0x88, 0xFF}},
        // RGB8 bytes B, G, R = 40 80 FF: r = FF, g = 80, b = 40, a = 255.
        {1, {0x40, 0x80, 0xFF}, {0xFF, 0x40, 0x80, 0xFF}},
        // RGB5A1 with the alpha bit clear: 0xFC10 -> a = 0.
        {3, {0x10, 0xFC}, {0x00, 0x42, 0x84, 0xFF}},
    };
    for (const Case& c : cases) {
        Fixture f;
        u8* in = f.Mem(kRamPhys, 4);
        std::memset(in, 0, 4);
        std::memcpy(in, c.input.data(), c.input.size());
        f.Transfer(kRam, kRam + 0x100, Size(1, 1), Size(1, 1), Flags(0, 1, 0, 1, c.format, 0, 0));
        INFO("input format " << c.format);
        CHECK(f.Read(kRamPhys + 0x100, 4) == c.output);
    }
}

TEST_CASE("transfer: 2x downscale in X, tiled input") {
    Fixture f;
    // The first 2x2 subtile of the tile holds the texels (0,0), (1,0), (0,1), (1,1) at indices
    // 0, 1, 2, 3. R = 10, 20, 30, 41; A = 255, 255, 255, 254; G = B = 0.
    f.Put32(kRamPhys, 0, Rgba(10, 0, 0, 255));
    f.Put32(kRamPhys, 1, Rgba(20, 0, 0, 255));
    f.Put32(kRamPhys, 2, Rgba(30, 0, 0, 255));
    f.Put32(kRamPhys, 3, Rgba(41, 0, 0, 254));
    f.FillBytes(kRamPhys + 0x1000, 0x200, 0xEE);
    // Output 8x8 means 4 wide and 8 tall, linear. out(0,0) averages the input texels at indices
    // 0 and 1: R = (10 + 20) / 2 = 15, A = (255 + 255) / 2 = 255. out(0,1) is the input (0,1)
    // and its neighbour in memory (index 3): R = (30 + 41) / 2 = 35 (71 / 2), A = (255 + 254) / 2
    // = 254 (509 / 2). The output word of (x, y) is y * 4 + x. Everything else averages zeros.
    f.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 0, 0, 0, 1));
    CHECK(f.Get32(kRamPhys + 0x1000, 0) == Rgba(15, 0, 0, 255));
    CHECK(f.Get32(kRamPhys + 0x1000, 4) == Rgba(35, 0, 0, 254));
    for (u32 i = 0; i < 32; ++i) {
        if (i != 0 && i != 4) {
            INFO("word " << i);
            CHECK(f.Get32(kRamPhys + 0x1000, i) == 0);
        }
    }
    CHECK(f.Get32(kRamPhys + 0x1000, 32) == 0xEEEEEEEE); // 4 x 8 words written, no more
}

TEST_CASE("transfer: 2x downscale in X and Y, tiled input") {
    Fixture f;
    f.Put32(kRamPhys, 0, Rgba(10, 0, 0, 255));
    f.Put32(kRamPhys, 1, Rgba(20, 0, 0, 255));
    f.Put32(kRamPhys, 2, Rgba(30, 0, 0, 255));
    f.Put32(kRamPhys, 3, Rgba(41, 0, 0, 254));
    f.FillBytes(kRamPhys + 0x1000, 0x200, 0xEE);
    // Output 4x4: out(0,0) averages the texels at indices 0..3: R = (10 + 20 + 30 + 41) / 4 =
    // 101 / 4 = 25, A = (255 + 255 + 255 + 254) / 4 = 1019 / 4 = 254. The other 15 average zeros.
    f.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 0, 0, 0, 2));
    CHECK(f.Get32(kRamPhys + 0x1000, 0) == Rgba(25, 0, 0, 254));
    for (u32 i = 1; i < 16; ++i) {
        INFO("word " << i);
        CHECK(f.Get32(kRamPhys + 0x1000, i) == 0);
    }
    CHECK(f.Get32(kRamPhys + 0x1000, 16) == 0xEEEEEEEE);
}

TEST_CASE("transfer: scaling the next out(x) uses input x * 2") {
    Fixture f;
    // out(1,0) with ScaleX reads the input texel (2,0), index 4, and its neighbour, index 5.
    f.Put32(kRamPhys, 4, Rgba(100, 0, 0, 0));
    f.Put32(kRamPhys, 5, Rgba(201, 0, 0, 0));
    f.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 0, 0, 0, 1));
    CHECK(f.Get32(kRamPhys + 0x1000, 1) == Rgba(150, 0, 0, 0)); // (100 + 201) / 2 = 150
}

TEST_CASE("transfer: unsupported requests change nothing but still signal PPF") {
    SUBCASE("zero input width") {
        Fixture f;
        f.FillBytes(kRamPhys + 0x100, 16, 0xEE);
        f.Transfer(kRam, kRam + 0x100, Size(0, 4), Size(4, 4), Flags(0, 1, 0, 1, 0, 0, 0));
        CHECK(f.Read(kRamPhys + 0x100, 16) == std::vector<u8>(16, 0xEE));
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].id == Interrupt::PPF);
        CHECK(s[0].delay_ns == 0); // 0 * 4 * 4 bytes
    }
    SUBCASE("zero output height") {
        Fixture f;
        f.FillBytes(kRamPhys + 0x100, 16, 0xEE);
        f.Transfer(kRam, kRam + 0x100, Size(4, 4), Size(4, 0), Flags(0, 1, 0, 1, 0, 0, 0));
        CHECK(f.Read(kRamPhys + 0x100, 16) == std::vector<u8>(16, 0xEE));
        CHECK(f.Signals().size() == 1);
    }
    SUBCASE("scaling on linear input") {
        Fixture f;
        f.FillBytes(kRamPhys + 0x100, 16, 0xEE);
        f.Transfer(kRam, kRam + 0x100, Size(4, 4), Size(4, 4), Flags(0, 1, 0, 1, 0, 0, 1));
        CHECK(f.Read(kRamPhys + 0x100, 16) == std::vector<u8>(16, 0xEE));
        CHECK(f.Signals().size() == 1);
    }
    SUBCASE("scaling mode 3") {
        Fixture f;
        f.FillBytes(kRamPhys + 0x100, 16, 0xEE);
        f.Transfer(kRam, kRam + 0x100, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 0, 0, 0, 3));
        CHECK(f.Read(kRamPhys + 0x100, 16) == std::vector<u8>(16, 0xEE));
        CHECK(f.Signals().size() == 1);
    }
    SUBCASE("an address outside memory") {
        Fixture f;
        f.FillBytes(kRamPhys + 0x100, 16, 0xEE);
        f.Transfer(0x08000000, kRam + 0x100, Size(4, 4), Size(4, 4), Flags(0, 1, 0, 1, 0, 0, 0));
        CHECK(f.Read(kRamPhys + 0x100, 16) == std::vector<u8>(16, 0xEE));
        CHECK(f.Signals().size() == 1);
    }
    SUBCASE("a format that does not exist") {
        Fixture f;
        f.FillBytes(kRamPhys + 0x100, 16, 0xEE);
        f.Transfer(kRam, kRam + 0x100, Size(4, 4), Size(4, 4), Flags(0, 1, 0, 1, 5, 0, 0));
        CHECK(f.Read(kRamPhys + 0x100, 16) == std::vector<u8>(16, 0xEE));
        CHECK(f.Signals().size() == 1);
    }
}

TEST_CASE("transfer: delay by the memory the two buffers are in") {
    // 16x16 RGBA8 = 1024 B at texture speeds: RAM -> RAM 450 MiB/s (2170 ns, above),
    // VRAM -> RAM 5400 MiB/s: 1024e9 / 5662310400 = 180.8 -> 180;
    // RAM -> VRAM 3100 MiB/s: 1024e9 / (3100 * 1048576) = 315.0 -> 315.
    {
        Fixture f;
        f.Transfer(kVram, kRam, Size(16, 16), Size(16, 16), Flags(0, 1, 0, 1, 0, 0, 0));
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].delay_ns == 180);
    }
    {
        Fixture f;
        f.Transfer(kRam, kVram, Size(16, 16), Size(16, 16), Flags(0, 1, 0, 1, 0, 0, 0));
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].delay_ns == 315);
    }
}

TEST_CASE("transfer: through the registers (WriteReg)") {
    Fixture f;
    f.Put32(kRamPhys, 0, 0x11111111);
    f.Put32(kRamPhys, 1, 0x22222222);
    const u32 base = 0x1EF00000 + 4 * 0x300;
    f.gpu.WriteReg(base + 0, kRamPhys >> 3);
    f.gpu.WriteReg(base + 4, (kRamPhys + 0x100) >> 3);
    f.gpu.WriteReg(base + 8, Size(2, 1));  // output_size
    f.gpu.WriteReg(base + 12, Size(2, 1)); // input_size
    f.gpu.WriteReg(base + 16, Flags(0, 1, 0, 1, 0, 0, 0));
    CHECK(f.Signals().empty());
    f.gpu.WriteReg(base + 24, 1); // trigger (word 6)
    CHECK(f.Get32(kRamPhys + 0x100, 0) == 0x11111111);
    CHECK(f.Get32(kRamPhys + 0x100, 1) == 0x22222222);
    const Signals s = f.Signals();
    REQUIRE(s.size() == 1);
    CHECK(s[0].id == Interrupt::PPF);
    CHECK((f.gpu.ReadReg(base + 24) & 1) == 0); // the trigger is cleared
    // Writing the trigger with bit 0 clear does nothing.
    f.gpu.WriteReg(base + 24, 0);
    CHECK(f.Signals().empty());
}

// ================================================================================================
// Texture copy
// ================================================================================================

TEST_CASE("texture copy: gaps on the input") {
    Fixture f;
    // size 32; input width 1 unit (16 B) and gap 1 unit; the output has no gap, so its width is
    // the whole size. Chunk 1: min(16, 32, 32) = 16 bytes from offset 0; the input row is used
    // up, so the source skips 16 bytes; chunk 2: min(16, 16, 16) from offset 32.
    for (u32 i = 0; i < 16; ++i) {
        f.PutBytes(kRamPhys + i, {static_cast<u8>(0x10 + i)});
        f.PutBytes(kRamPhys + 16 + i, {0xEE});
        f.PutBytes(kRamPhys + 32 + i, {static_cast<u8>(0x20 + i)});
    }
    f.FillBytes(kRamPhys + 0x100, 48, 0x55);
    f.Copy(kRam, kRam + 0x100, 0x20, 0x00010001, 0);
    std::vector<u8> expected;
    for (u32 i = 0; i < 16; ++i) {
        expected.push_back(static_cast<u8>(0x10 + i));
    }
    for (u32 i = 0; i < 16; ++i) {
        expected.push_back(static_cast<u8>(0x20 + i));
    }
    expected.push_back(0x55); // byte 32 untouched
    CHECK(f.Read(kRamPhys + 0x100, 33) == expected);
    // PPF, delay from the register size (not aligned): 32 B at 450 MiB/s = 32e9/471859200 =
    // 67.8 -> 67.
    const Signals s = f.Signals();
    REQUIRE(s.size() == 1);
    CHECK(s[0].id == Interrupt::PPF);
    CHECK(s[0].delay_ns == 67);
}

TEST_CASE("texture copy: gaps on the output") {
    Fixture f;
    for (u32 i = 0; i < 32; ++i) {
        f.PutBytes(kRamPhys + i, {static_cast<u8>(0x40 + i)});
    }
    f.FillBytes(kRamPhys + 0x100, 48, 0x55);
    // The input is contiguous; the output has width 1 unit and gap 1 unit: 16 bytes at 0, skip
    // 16, 16 bytes at 32.
    f.Copy(kRam, kRam + 0x100, 0x20, 0, 0x00010001);
    const auto out = f.Read(kRamPhys + 0x100, 48);
    for (u32 i = 0; i < 16; ++i) {
        CHECK(out[i] == 0x40 + i);
        CHECK(out[16 + i] == 0x55);
        CHECK(out[32 + i] == 0x50 + i); // input bytes 16..31
    }
}

TEST_CASE("texture copy: the low 4 bits of the size are ignored, a size under 16 is refused") {
    Fixture f;
    for (u32 i = 0; i < 48; ++i) {
        f.PutBytes(kRamPhys + i, {static_cast<u8>(0x40 + i)});
    }
    f.FillBytes(kRamPhys + 0x100, 48, 0x55);
    f.Copy(kRam, kRam + 0x100, 0x2F, 0, 0); // aligned down to 0x20
    const auto out = f.Read(kRamPhys + 0x100, 48);
    CHECK(out[31] == 0x40 + 31);
    CHECK(out[32] == 0x55);
    f.FillBytes(kRamPhys + 0x200, 16, 0x55);
    f.Copy(kRam, kRam + 0x200, 0x0F, 0, 0); // aligned down to 0: real hardware freezes; nothing
    CHECK(f.Read(kRamPhys + 0x200, 16) == std::vector<u8>(16, 0x55));
}

TEST_CASE("texture copy: a gap with zero width is refused") {
    Fixture f;
    f.PutBytes(kRamPhys, {1, 2, 3, 4});
    f.FillBytes(kRamPhys + 0x100, 32, 0x55);
    f.Copy(kRam, kRam + 0x100, 0x20, 0x00010000, 0); // gap 1, width 0
    CHECK(f.Read(kRamPhys + 0x100, 32) == std::vector<u8>(32, 0x55));
    CHECK(f.Signals().size() == 1); // PPF all the same
}

TEST_CASE("texture copy: delay by the memory the buffers are in") {
    // 1 MiB; texture speeds RAM->RAM 450, RAM->VRAM 3100, VRAM->RAM 5400 MiB/s:
    // 1e9/450 = 2222222.2, 1e9/3100 = 322580.6, 1e9/5400 = 185185.2.
    struct Case {
        u32 src, dst;
        u64 delay;
    };
    const Case cases[] = {{kRam, kRam + 0x200000, 2222222},
                          {kRam, kVram, 322580},
                          {kVram, kRam, 185185}};
    for (const Case& c : cases) {
        Fixture f;
        f.Copy(c.src, c.dst, 0x100000, 0, 0);
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].delay_ns == c.delay);
    }
}

// ================================================================================================
// The rest of the Gpu and its registration
// ================================================================================================

TEST_CASE("commands that do nothing here do not touch memory or signal") {
    Fixture f;
    f.Run({1, kRam, 64, 0, 0, 0, 0, 0}); // SubmitCmdList
    f.Run({5, kRam, 64});                // CacheFlush
    f.Run({0x77});                       // unknown
    CHECK(f.Signals().empty());
}

TEST_CASE("buffer swap and colour fill update the registers as Azahar does") {
    Fixture f;
    FrameBufferInfo info{};
    info.active_fb = 0;
    info.address_left = kVram + 0x1000;
    info.address_right = kVram + 0x2000;
    info.stride = 0x780;
    info.format = 2;
    info.shown_fb = 1;
    f.gpu.SetBufferSwap(1, info);
    // framebuffer_config[1] starts at word 0x140; address_left1 is +0x1A, address_right1 +0x25,
    // format +0x1C, active_fb +0x1E, stride +0x24.
    const u32 base = 0x1EF00000 + 4 * 0x140;
    CHECK(f.gpu.ReadReg(base + 4 * 0x1A) == kVramPhys + 0x1000);
    CHECK(f.gpu.ReadReg(base + 4 * 0x25) == kVramPhys + 0x2000);
    CHECK(f.gpu.ReadReg(base + 4 * 0x24) == 0x780);
    CHECK(f.gpu.ReadReg(base + 4 * 0x1C) == 2);
    CHECK(f.gpu.ReadReg(base + 4 * 0x1E) == 1);
    f.gpu.SetColorFill(0x01FF8040);
    CHECK(f.gpu.ReadReg(0x1ED02000 + 4 * 0x81) == 0x01FF8040);
    CHECK(f.gpu.ReadReg(0x1ED02000 + 4 * 0x281) == 0x01FF8040);
}

TEST_CASE("cpu-gx is registered for the capture player") {
    const auto* factory = Registry<GpuFactory>::Instance().Find("cpu-gx");
    REQUIRE(factory != nullptr);
    CaptureHost host;
    const Options options;
    auto gpu = (*factory)(host, options);
    REQUIRE(gpu != nullptr);
    CHECK(dynamic_cast<GxEngine*>(gpu.get()) != nullptr);
}

TEST_CASE("blitter helpers") {
    CHECK(BytesPerPixel(0) == 4);
    CHECK(BytesPerPixel(1) == 3);
    CHECK(BytesPerPixel(2) == 2);
    CHECK(BytesPerPixel(4) == 2);
    CHECK(BytesPerPixel(5) == 0);
    // Texel (3, 5) of the tile table is 39; (8, 0) is the first texel of the next tile (64).
    CHECK(MortonOffset(3, 5, 1) == 39);
    CHECK(MortonOffset(8, 0, 1) == 64);
    // (9, 2): coarse_x = 8 adds 8 * 8 = 64 texels, the texel inside the tile is column 1 of row 2,
    // index 9; (64 + 9) * 4 bytes.
    CHECK(MortonOffset(9, 2, 4) == 73 * 4);
}

// ================================================================================================
// Display transfer: every pixel size (2 and 3 bytes), tiled, scaled, flipped and cropped
// ================================================================================================

namespace {

// A pixel of a 2 or 3 byte format with the id `id`: bytes id, 255 - id (, 0x5A). Every 16-bit
// value is a valid RGB565 / RGBA4 pixel and a same-format transfer is lossless (5 -> 8 -> 5, 6 ->
// 8 -> 6 and 4 -> 8 -> 4 bits come back unchanged), so the bytes arrive as they were.
std::vector<u8> PixelBytes(u32 bpp, u32 id) {
    if (bpp == 2) {
        return {static_cast<u8>(id), static_cast<u8>(255 - id)};
    }
    return {static_cast<u8>(id), static_cast<u8>(255 - id), 0x5A};
}

struct SizedFormat {
    u32 format;
    u32 bpp;
};
// RGB565 (2), RGBA4 (4), RGB8 (1).
constexpr SizedFormat kSizedFormats[] = {{2, 2}, {4, 2}, {1, 3}};

// Four texels of a 16x16 image that tell the terms of the offset apart, with the id y * 16 + x:
// the byte offset in the tiled image is index * bpp, the index worked out here from the tile
// (tx = x / 8, ty = y / 8; two tiles per row of tiles):
//   index = ty * 128 (coarse_y * stride = 8 * 16 = 128 texels) + tx * 64 (coarse_x * 8) + kTile.
struct Spot {
    u32 x, y, id;
    u32 index;
};
constexpr Spot kSpots[] = {
    {8, 0, 8, 64},    // tx 1, ty 0: 0 + 64 + kTile[0][0] (0)
    {0, 8, 128, 128}, // tx 0, ty 1: 128 + 0 + 0
    {9, 10, 169, 201}, // tx 1, ty 1: 128 + 64 + kTile[2][1] (9)
    {15, 15, 255, 255}, // tx 1, ty 1: 128 + 64 + kTile[7][7] (63)
};

} // namespace

TEST_CASE("transfer: 2 and 3 byte formats, linear to tiled, 16x16") {
    for (const SizedFormat& fmt : kSizedFormats) {
        INFO("format " << fmt.format);
        Fixture f;
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                f.PutVec(kRamPhys + (y * 16 + x) * fmt.bpp, PixelBytes(fmt.bpp, y * 16 + x));
            }
        }
        f.Transfer(kRam, kRam + 0x10000, Size(16, 16), Size(16, 16),
                   Flags(0, 1, 0, 0, fmt.format, fmt.format, 0));
        // The four texels of kSpots, at their literal offsets index * bpp (bpp 2: 128, 256, 402,
        // 510; bpp 3: 192, 384, 603, 765), hold the bytes of their id.
        for (const Spot& spot : kSpots) {
            const auto got = f.Read(kRamPhys + 0x10000 + spot.index * fmt.bpp, fmt.bpp);
            INFO("spot " << spot.x << "," << spot.y);
            CHECK(got == PixelBytes(fmt.bpp, spot.id));
        }
        CHECK(kSpots[2].index * fmt.bpp == (fmt.bpp == 2 ? 402u : 603u));
        // All the texels: the tile (tx, ty) starts at (ty * 2 + tx) * 64 texels.
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                const u32 index = ((y / 8) * 2 + (x / 8)) * 64 + kTile[y % 8][x % 8];
                INFO("texel " << x << "," << y);
                REQUIRE(f.Read(kRamPhys + 0x10000 + index * fmt.bpp, fmt.bpp) ==
                        PixelBytes(fmt.bpp, y * 16 + x));
            }
        }
        // Nothing past the image.
        CHECK(f.Read(kRamPhys + 0x10000 + 256 * fmt.bpp, 4) == std::vector<u8>(4, 0));
    }
}

TEST_CASE("transfer: 2 and 3 byte formats, tiled to linear, 16x16") {
    for (const SizedFormat& fmt : kSizedFormats) {
        INFO("format " << fmt.format);
        Fixture f;
        // Only the four texels of kSpots are set, at index * bpp in the tiled input (same offsets
        // as above); the linear output holds them at (y * 16 + x) * bpp: bpp 2: 16, 256, 338,
        // 510; bpp 3: 24, 384, 507, 765 (ids 8, 128, 169, 255). Everything else decodes from
        // zero bytes and encodes back to zero bytes.
        std::vector<u8> expected(16 * 16 * fmt.bpp, 0);
        for (const Spot& spot : kSpots) {
            const auto bytes = PixelBytes(fmt.bpp, spot.id);
            f.PutVec(kRamPhys + spot.index * fmt.bpp, bytes);
            std::copy(bytes.begin(), bytes.end(),
                      expected.begin() + (spot.y * 16 + spot.x) * fmt.bpp);
        }
        f.Transfer(kRam, kRam + 0x10000, Size(16, 16), Size(16, 16),
                   Flags(0, 0, 0, 0, fmt.format, fmt.format, 0));
        CHECK(f.Read(kRamPhys + 0x10000, 16 * 16 * fmt.bpp) == expected);
        CHECK(expected[(9 + 10 * 16) * fmt.bpp] == 0xA9);
    }
}

TEST_CASE("transfer: 3 byte input, scaling 1 and 2 on a tiled image") {
    // RGB8 memory order is B, G, R. Texels by their index in the tile (3 bytes each):
    //   0: B 10 G 20 R 30   1: B 20 G 41 R 51   2: B 08 G 09 R 0B   3: B 0D G 10 R 0F
    //   4: B 64 G 00 R C8   5: B C9 G 03 R C9   (6, 7 and the rest are zero)
    // The neighbour read is the next texel in MEMORY (+3 bytes), not the next in the image.
    const std::vector<std::vector<u8>> texels = {{0x10, 0x20, 0x30}, {0x20, 0x41, 0x51},
                                                 {0x08, 0x09, 0x0B}, {0x0D, 0x10, 0x0F},
                                                 {0x64, 0x00, 0xC8}, {0xC9, 0x03, 0xC9}};
    {
        Fixture f;
        for (u32 i = 0; i < texels.size(); ++i) {
            f.PutVec(kRamPhys + i * 3, texels[i]);
        }
        f.FillBytes(kRamPhys + 0x1000, 0x100, 0xEE);
        // Scaling 1: output 8x8 -> 4 wide, 8 tall, linear, RGB8; out(x, y) averages the texels at
        // index(2x, y) and index + 1 (integer division):
        //  out(0,0): idx 0 + 1: B (16+32)/2 = 24 = 18, G (32+65)/2 = 48 = 30, R (48+81)/2 = 64 = 40
        //  out(1,0): input (2,0) is index 4 (+ 5): B (100+201)/2 = 150 = 96, G (0+3)/2 = 1,
        //            R (200+201)/2 = 200 = C8
        //  out(0,1): input (0,1) is index 2 (+ 3): B (8+13)/2 = 10 = 0A, G (9+16)/2 = 12 = 0C,
        //            R (11+15)/2 = 13 = 0D
        //  out(1,1): index 6 + 7: zero. The output pixel (x, y) is at (y * 4 + x) * 3.
        f.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 0, 1, 1, 1));
        std::vector<u8> expected(4 * 8 * 3, 0);
        const u8 p00[] = {0x18, 0x30, 0x40};
        const u8 p10[] = {0x96, 0x01, 0xC8};
        const u8 p01[] = {0x0A, 0x0C, 0x0D};
        std::copy(p00, p00 + 3, expected.begin() + 0);
        std::copy(p10, p10 + 3, expected.begin() + 3);
        std::copy(p01, p01 + 3, expected.begin() + 12);
        CHECK(f.Read(kRamPhys + 0x1000, 96) == expected);
        CHECK(f.Read(kRamPhys + 0x1000 + 96, 4) == std::vector<u8>(4, 0xEE));
    }
    {
        Fixture f;
        for (u32 i = 0; i < texels.size(); ++i) {
            f.PutVec(kRamPhys + i * 3, texels[i]);
        }
        f.FillBytes(kRamPhys + 0x1000, 0x100, 0xEE);
        // Scaling 2: output 8x8 -> 4x4; out(x, y) averages index(2x, 2y) .. + 3 as
        // ((c + p1) + (p2 + p3)) / 4.
        //  out(0,0): idx 0..3: B (16+32+8+13)/4 = 69/4 = 17 = 11, G (32+65+9+16)/4 = 122/4 = 30
        //            = 1E, R (48+81+11+15)/4 = 155/4 = 38 = 26
        //  out(1,0): idx 4..7: B (100+201+0+0)/4 = 75 = 4B, G (0+3)/4 = 0, R (200+201)/4 = 100 = 64
        //  out(0,1): input (0,2) is index 8: zero.
        f.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 0, 1, 1, 2));
        std::vector<u8> expected(4 * 4 * 3, 0);
        const u8 p00[] = {0x11, 0x1E, 0x26};
        const u8 p10[] = {0x4B, 0x00, 0x64};
        std::copy(p00, p00 + 3, expected.begin() + 0);
        std::copy(p10, p10 + 3, expected.begin() + 3);
        CHECK(f.Read(kRamPhys + 0x1000, 48) == expected);
        CHECK(f.Read(kRamPhys + 0x1000 + 48, 4) == std::vector<u8>(4, 0xEE));
    }
}

namespace {

// RGB565 texels by their index in the tile, little-endian (the bytes in memory are low, high):
//   0: F81F  1: 07E0  2: F800  3: F800  4: FFFF  5: 0841  6: 1234  (7 and the rest are zero)
// Decoded to 8 bits (5 -> v << 3 | v >> 2, 6 -> v << 2 | v >> 4):
//   0: (255, 0, 255)  1: (0, 255, 0)  2, 3: (255, 0, 0)  4: (255, 255, 255)  5: (8, 8, 8)
//   6: r5 = 2 -> 16, g6 = 17 -> 68 | 1 = 69, b5 = 20 -> 160 | 5 = 165
void Put565Tile(Fixture& f) {
    const u16 values[] = {0xF81F, 0x07E0, 0xF800, 0xF800, 0xFFFF, 0x0841, 0x1234};
    for (u32 i = 0; i < 7; ++i) {
        f.PutVec(kRamPhys + i * 2, {static_cast<u8>(values[i] & 0xFF), static_cast<u8>(values[i] >> 8)});
    }
    f.FillBytes(kRamPhys + 0x1000, 0x100, 0xEE);
}

} // namespace

TEST_CASE("transfer: 2 byte input, scaling 1 on a tiled image") {
    Fixture f;
    Put565Tile(f);
    // Scaling 1, RGB565 -> RGB565, output 4 wide x 8 tall, 2 bytes per pixel (the output pixel
    // (x, y) is at (y * 4 + x) * 2). Neighbour = next texel in memory, +2 bytes:
    //  out(0,0): idx 0 + 1: r (255+0)/2 = 127, g (0+255)/2 = 127, b (255+0)/2 = 127 ->
    //            r5 = 15, g6 = 31, b5 = 15: 15 << 11 | 31 << 5 | 15 = 0x7BEF -> EF 7B  (byte 0)
    //  out(1,0): idx 4 + 5: r (255+8)/2 = 131, g 131, b 131 -> 16, 32, 16:
    //            0x8000 | 0x0400 | 0x0010 = 0x8410 -> 10 84  (byte 2)
    //  out(0,1): idx 2 + 3: r 255 -> 31: 0xF800 -> 00 F8  (byte 8)
    //  out(1,1): idx 6 + 7 (zero): r 16/2 = 8, g 69/2 = 34, b 165/2 = 82 -> 1, 8, 10:
    //            0x0800 | 0x0100 | 0x000A = 0x090A -> 0A 09  (byte 10)
    // A neighbour 4 bytes on would read index 6 for out(1,0) and give another value.
    f.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 0, 2, 2, 1));
    std::vector<u8> expected(4 * 8 * 2, 0);
    expected[0] = 0xEF; expected[1] = 0x7B;
    expected[2] = 0x10; expected[3] = 0x84;
    expected[8] = 0x00; expected[9] = 0xF8;
    expected[10] = 0x0A; expected[11] = 0x09;
    CHECK(f.Read(kRamPhys + 0x1000, 64) == expected);
    CHECK(f.Read(kRamPhys + 0x1000 + 64, 4) == std::vector<u8>(4, 0xEE));
}

TEST_CASE("transfer: 2 byte input, scaling 2 on a tiled image") {
    Fixture f;
    Put565Tile(f);
    // Scaling 2: output 4x4, 2 bytes per pixel.
    //  out(0,0): idx 0..3: r (255+0+255+255)/4 = 765/4 = 191, g (0+255+0+0)/4 = 63,
    //            b (255+0+0+0)/4 = 63 -> r5 = 23, g6 = 15, b5 = 7:
    //            23 << 11 | 15 << 5 | 7 = 0xB800 | 0x01E0 | 0x0007 = 0xB9E7 -> E7 B9  (byte 0)
    //  out(1,0): idx 4..7: r (255+8+16+0)/4 = 279/4 = 69, g (255+8+69+0)/4 = 332/4 = 83,
    //            b (255+8+165+0)/4 = 428/4 = 107 -> 8, 20, 13:
    //            0x4000 | 0x0280 | 0x000D = 0x428D -> 8D 42  (byte 2)
    f.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(0, 0, 0, 0, 2, 2, 2));
    std::vector<u8> expected(4 * 4 * 2, 0);
    expected[0] = 0xE7; expected[1] = 0xB9;
    expected[2] = 0x8D; expected[3] = 0x42;
    CHECK(f.Read(kRamPhys + 0x1000, 32) == expected);
    CHECK(f.Read(kRamPhys + 0x1000 + 32, 4) == std::vector<u8>(4, 0xEE));

    // The same with the vertical flip: output_y = output_height - y - 1 uses the SCALED height
    // (4), so the first row of the output lands in row 3: out(0,0) at (3 * 4 + 0) * 2 = 24,
    // out(1,0) at (3 * 4 + 1) * 2 = 26.
    Fixture g;
    Put565Tile(g);
    g.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(1, 0, 0, 0, 2, 2, 2));
    std::vector<u8> flipped(4 * 4 * 2, 0);
    flipped[24] = 0xE7; flipped[25] = 0xB9;
    flipped[26] = 0x8D; flipped[27] = 0x42;
    CHECK(g.Read(kRamPhys + 0x1000, 32) == flipped);
    CHECK(g.Read(kRamPhys + 0x1000 + 32, 4) == std::vector<u8>(4, 0xEE));
}

TEST_CASE("transfer: flip with scaling 1 keeps the full height") {
    Fixture f;
    Put565Tile(f);
    // Scaling 1 only halves the width: the height stays 8, out(x, y) goes to row 7 - y. The
    // values of the scaling-1 test above: out(0,0) -> row 7: (7 * 4 + 0) * 2 = 56 (EF 7B);
    // out(1,0) -> (7 * 4 + 1) * 2 = 58 (10 84); out(0,1) -> row 6: (6 * 4) * 2 = 48 (00 F8);
    // out(1,1) -> (6 * 4 + 1) * 2 = 50 (0A 09).
    f.Transfer(kRam, kRam + 0x1000, Size(8, 8), Size(8, 8), Flags(1, 0, 0, 0, 2, 2, 1));
    std::vector<u8> expected(4 * 8 * 2, 0);
    expected[56] = 0xEF; expected[57] = 0x7B;
    expected[58] = 0x10; expected[59] = 0x84;
    expected[48] = 0x00; expected[49] = 0xF8;
    expected[50] = 0x0A; expected[51] = 0x09;
    CHECK(f.Read(kRamPhys + 0x1000, 64) == expected);
}

TEST_CASE("transfer: crop, with and without the flip, for 2 and 3 byte outputs") {
    // Input 4x2, output 2x2, linear to linear (dont_swizzle with input_linear). Texel (x, y) of
    // the left half has id 0x10 * (y + 1) + x + 1: p00 0x11, p10 0x12, p01 0x21, p11 0x22; the
    // right half (x = 2, 3) is FF. Bytes: id, id + 0xC0 (, 0x77).
    struct Case {
        u32 format, bpp;
    };
    for (const Case& c : {Case{2, 2}, Case{1, 3}}) {
        INFO("format " << c.format);
        const auto px = [&](u32 id) {
            std::vector<u8> bytes = {static_cast<u8>(id), static_cast<u8>(id + 0xC0)};
            if (c.bpp == 3) {
                bytes.push_back(0x77);
            }
            return bytes;
        };
        const auto build = [&](Fixture& f) {
            for (u32 y = 0; y < 2; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    const std::vector<u8> bytes =
                        x < 2 ? px(0x10 * (y + 1) + x + 1) : std::vector<u8>(c.bpp, 0xFF);
                    f.PutVec(kRamPhys + (x + y * 4) * c.bpp, bytes);
                }
            }
            f.FillBytes(kRamPhys + 0x100, 0x40, 0xEE);
        };
        const auto place = [&](std::vector<u8>& image, u32 offset, u32 id) {
            const auto bytes = px(id);
            std::copy(bytes.begin(), bytes.end(), image.begin() + offset);
        };
        const u32 total = 6 * c.bpp;
        {
            // Crop alone: no skew; out(x, y) at (y * 2 + x) * bpp: p00 at 0, p10 at bpp, p01 at
            // 2 bpp, p11 at 3 bpp.
            Fixture f;
            build(f);
            f.Transfer(kRam, kRam + 0x100, Size(4, 2), Size(2, 2),
                       Flags(0, 1, 1, 1, c.format, c.format, 0));
            std::vector<u8> expected(total, 0xEE);
            place(expected, 0, 0x11);
            place(expected, 1 * c.bpp, 0x12);
            place(expected, 2 * c.bpp, 0x21);
            place(expected, 3 * c.bpp, 0x22);
            CHECK(f.Read(kRamPhys + 0x100, total) == expected);
        }
        {
            // Flip + crop: dst += (4 - 2) * (2 - 1) * bpp = 2 bpp bytes (4 for bpp 2, 6 for bpp 3);
            // out(x, y) goes to ((1 - y) * 2 + x) * bpp + 2 bpp: p01 at 2 bpp (byte 4 / 6),
            // p11 at 3 bpp (6 / 9), p00 at 4 bpp (8 / 12), p10 at 5 bpp (10 / 15).
            Fixture f;
            build(f);
            f.Transfer(kRam, kRam + 0x100, Size(4, 2), Size(2, 2),
                       Flags(1, 1, 1, 1, c.format, c.format, 0));
            std::vector<u8> expected(total + 2 * c.bpp, 0xEE);
            place(expected, 2 * c.bpp, 0x21);
            place(expected, 3 * c.bpp, 0x22);
            place(expected, 4 * c.bpp, 0x11);
            place(expected, 5 * c.bpp, 0x12);
            CHECK(f.Read(kRamPhys + 0x100, total + 2 * c.bpp) == expected);
        }
    }
}

TEST_CASE("transfer: the crop skew uses the output pixel size, not the input's") {
    Fixture f;
    // Input RGBA8 4x2 (bytes A, B, G, R), output RGB565 2x2. Skew = (4 - 2) * (2 - 1) * 2 = 4
    // bytes (it would be 8 with the input's 4). Texels are grey v = 8 j: RGB565 = j << 11 |
    // 2j << 5 | j: j = 1: 0x0841, 2: 0x1082, 3: 0x18C3, 4: 0x2104. p00 j 1, p10 j 2, p01 j 3,
    // p11 j 4; the right half is FF.
    const u8 grey[] = {8, 16, 24, 32};
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u8 v = x < 2 ? grey[y * 2 + x] : 0xFF;
            f.PutVec(kRamPhys + (x + y * 4) * 4, {0xFF, v, v, v});
        }
    }
    f.FillBytes(kRamPhys + 0x100, 0x40, 0xEE);
    f.Transfer(kRam, kRam + 0x100, Size(4, 2), Size(2, 2), Flags(1, 1, 1, 1, 0, 2, 0));
    // out(x, y) at ((1 - y) * 2 + x) * 2 + 4: p01 at 4 (C3 18), p11 at 6 (04 21), p00 at 8
    // (41 08), p10 at 10 (82 10).
    CHECK(f.Read(kRamPhys + 0x100, 14) ==
          std::vector<u8>{0xEE, 0xEE, 0xEE, 0xEE, 0xC3, 0x18, 0x04, 0x21, 0x41, 0x08, 0x82, 0x10,
                          0xEE, 0xEE});
}

TEST_CASE("transfer: format pairs beyond RGBA8") {
    // Decoders: 5 bits v -> v << 3 | v >> 2, 6 -> v << 2 | v >> 4, 4 -> v << 4 | v. Encoders:
    // 8 -> 5 is >> 3, 8 -> 6 >> 2, 8 -> 4 >> 4, 8 -> 1 >> 7.
    struct Case {
        u32 in_format, out_format;
        std::vector<u8> input, output;
    };
    const Case cases[] = {
        // RGB565 0xFC08 -> (255, 130, 66, 255) -> RGB5A1: 31 << 11 | 16 << 6 | 8 << 1 | 1 = 0xFC11
        {2, 3, {0x08, 0xFC}, {0x11, 0xFC}},
        // RGBA4 0xF84C -> (255, 136, 68, 204) -> RGB8 (B, G, R): 44 88 FF
        {4, 1, {0x4C, 0xF8}, {0x44, 0x88, 0xFF}},
        // RGB8 (B, G, R) 40 80 FF -> (255, 128, 64, 255) -> RGBA4: F, 8, 4, F = 0xF84F
        {1, 4, {0x40, 0x80, 0xFF}, {0x4F, 0xF8}},
        // RGB5A1 0xFC11 -> (255, 132, 66, 255) -> RGB565: 31 << 11 | 33 << 5 | 8 = 0xFC28
        {3, 2, {0x11, 0xFC}, {0x28, 0xFC}},
        // RGB565 0xFC08 -> (255, 130, 66, 255) -> RGBA4: F, 8, 4, F = 0xF84F
        {2, 4, {0x08, 0xFC}, {0x4F, 0xF8}},
        // RGBA4 0xF84C -> (255, 136, 68, 204) -> RGB5A1: 31 << 11 | 17 << 6 | 8 << 1 | 1 =
        // 0xF800 | 0x0440 | 0x0010 | 1 = 0xFC51
        {4, 3, {0x4C, 0xF8}, {0x51, 0xFC}},
        // RGB5A1 0xFC10 (alpha bit clear) -> (255, 132, 66, 0) -> RGBA4: F, 8, 4, 0 = 0xF840
        {3, 4, {0x10, 0xFC}, {0x40, 0xF8}},
        // RGB8 (B, G, R) 40 80 FF -> (255, 128, 64) -> RGB565: 31 << 11 | 32 << 5 | 8 = 0xFC08
        {1, 2, {0x40, 0x80, 0xFF}, {0x08, 0xFC}},
        // RGBA4 0xF84C -> (255, 136, 68) -> RGB565: 31 << 11 | 34 << 5 | 8 = 0xFC48
        {4, 2, {0x4C, 0xF8}, {0x48, 0xFC}},
    };
    for (const Case& c : cases) {
        Fixture f;
        u8* in = f.Mem(kRamPhys, 4);
        std::memset(in, 0, 4);
        std::memcpy(in, c.input.data(), c.input.size());
        f.FillBytes(kRamPhys + 0x100, 8, 0xEE);
        f.Transfer(kRam, kRam + 0x100, Size(1, 1), Size(1, 1),
                   Flags(0, 1, 0, 1, c.in_format, c.out_format, 0));
        std::vector<u8> expected = c.output;
        expected.resize(5, 0xEE);
        INFO("formats " << c.in_format << " -> " << c.out_format);
        CHECK(f.Read(kRamPhys + 0x100, 5) == expected);
    }
}

TEST_CASE("transfer: formats 5 to 7 are refused as input and as output") {
    for (u32 bad = 5; bad <= 7; ++bad) {
        for (int side = 0; side < 3; ++side) {
            Fixture f;
            f.FillBytes(kRamPhys + 0x100, 16, 0xEE);
            f.PutBytes(kRamPhys, {1, 2, 3, 4});
            const u32 in_format = side != 1 ? bad : 0;
            const u32 out_format = side != 0 ? bad : 0;
            INFO("input " << in_format << " output " << out_format);
            f.Transfer(kRam, kRam + 0x100, Size(1, 1), Size(1, 1),
                       Flags(0, 1, 0, 1, in_format, out_format, 0));
            CHECK(f.Read(kRamPhys + 0x100, 16) == std::vector<u8>(16, 0xEE));
            const Signals s = f.Signals();
            REQUIRE(s.size() == 1);
            CHECK(s[0].id == Interrupt::PPF);
        }
    }
}

// ================================================================================================
// Ranges that leave the end of an area (the part of the code that Azahar does not have)
// ================================================================================================

namespace {

constexpr u32 kVramEnd = 0x18600000;
constexpr u32 kDspEnd = 0x1FF80000;

FillRegs MakeFill(u32 start, u32 end, u32 value, u32 control) {
    FillRegs regs;
    regs.address_start = start >> 3;
    regs.address_end = end >> 3;
    regs.value = value;
    regs.control = control;
    return regs;
}

} // namespace

TEST_CASE("bounds: Window") {
    CaptureHost host;
    // A size of 0 one past the end of an area is valid: an empty span that points at the end.
    {
        const std::span<u8> w = Window(host, kVramEnd, 0);
        CHECK(w.empty());
        CHECK(w.data() == host.PhysicalPointer(kVramEnd, 0));
        CHECK(w.data() != nullptr);
        CHECK(IsValidPhysicalAddress(host, kVramEnd));
        CHECK_FALSE(IsValidPhysicalAddress(host, kVramEnd + 8));
    }
    // A range that starts at the end has an empty prefix (still non-null).
    {
        const std::span<u8> w = Window(host, kVramEnd, 16);
        CHECK(w.empty());
        CHECK(w.data() != nullptr);
    }
    // A start in no area (below, between, above, and at the top of the address space).
    for (const u32 paddr : {0x08000000u, 0x18600008u, 0x1FF80008u, 0x30000008u, 0xFFFFFFF0u}) {
        const std::span<u8> w = Window(host, paddr, 16);
        INFO("paddr " << paddr);
        CHECK(w.empty());
        CHECK(w.data() == nullptr);
        CHECK_FALSE(IsValidPhysicalAddress(host, paddr));
    }
    // Every prefix length from 1 to 40 bytes before the end of VRAM and of DSP RAM, for a request
    // that is longer: the binary search finds exactly the bytes that are left.
    for (const u32 end : {kVramEnd, kDspEnd}) {
        for (u32 left = 1; left <= 40; ++left) {
            const std::span<u8> w = Window(host, end - left, 64);
            INFO("end " << end << " left " << left);
            REQUIRE(w.size() == left);
            CHECK(w.data() == host.PhysicalPointer(end - left, 0));
        }
    }
    // A request that fits is returned whole, and the exact size of the area too.
    CHECK(Window(host, 0x185FFFF0, 16).size() == 16);
    CHECK(Window(host, 0x18000000, 0x600000).size() == 0x600000);
    CHECK(Window(host, 0x18000000, 0x600001).size() == 0x600000);
    CHECK(Window(host, 0x18000000, 0x10000000).size() == 0x600000);
    // The areas do not join: a range from DSP RAM into FCRAM is cut at the end of DSP RAM.
    CHECK(Window(host, 0x1FF7FFF8, 0x80010).size() == 8);
    // A size that would wrap the 32-bit address space is cut to what is left of it.
    CHECK(Window(host, 0x2FFFFFF0, 0xFFFFFFFFull).size() == 16);
}

TEST_CASE("bounds: fill that ends past the end of VRAM writes up to the end") {
    Fixture f;
    // Start 0x185FFFF0 (16 bytes before the end of VRAM), end 0x20000000 (the start of FCRAM,
    // which is a valid address, so the fill is not refused). 32-bit units of 0x11223344: the
    // window is the 16 bytes up to 0x18600000, four units; the other 0x79FFxxxx are dropped.
    f.FillBytes(0x185FFFE0, 0x20, 0xEE);
    f.FillBytes(0x20000000, 16, 0xEE);
    MemoryFill(f.host, MakeFill(0x185FFFF0, 0x20000000, 0x11223344, 0x201));
    std::vector<u8> expected(16, 0xEE);
    for (u32 i = 0; i < 4; ++i) {
        expected.insert(expected.end(), {0x44, 0x33, 0x22, 0x11});
    }
    CHECK(f.Read(0x185FFFE0, 0x20) == expected);
    CHECK(f.Read(0x20000000, 16) == std::vector<u8>(16, 0xEE)); // not written
}

TEST_CASE("bounds: 16-bit fill that runs out of an area") {
    Fixture f;
    // Start 0x1FF7FFF8 (8 bytes before the end of DSP RAM) to 0x20000008: 0x80010 bytes. The
    // units of 2 bytes at offsets 0, 2, 4, 6 are inside (0xBEEF -> EF BE), the rest are dropped.
    f.FillBytes(0x1FF7FFF0, 8, 0xEE);
    f.FillBytes(0x20000000, 16, 0xEE);
    MemoryFill(f.host, MakeFill(0x1FF7FFF8, 0x20000008, 0xBEEF, 0x001));
    CHECK(f.Read(0x1FF7FFF0, 16) ==
          std::vector<u8>{0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEF, 0xBE, 0xEF, 0xBE,
                          0xEF, 0xBE, 0xEF, 0xBE});
    CHECK(f.Read(0x20000000, 16) == std::vector<u8>(16, 0xEE));
}

TEST_CASE("bounds: 24-bit fill whose last unit straddles the end of an area") {
    Fixture f;
    // Same range (8 bytes before the end of DSP RAM). Units of 3 bytes start at the
    // offsets 0, 3, 6, 9, ...: the unit at 6 covers the bytes 6, 7 (inside) and 8 (outside).
    // 0x00CCBBAA is written as AA BB CC; the units from offset 9 on are outside. The straddling
    // unit leaves its two inside bytes AA BB and drops CC.
    f.FillBytes(0x1FF7FFF0, 8, 0xEE);
    f.FillBytes(0x20000000, 16, 0xEE);
    MemoryFill(f.host, MakeFill(0x1FF7FFF8, 0x20000008, 0x00CCBBAA, 0x101));
    CHECK(f.Read(0x1FF7FFF0, 16) ==
          std::vector<u8>{0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xAA, 0xBB, 0xCC, 0xAA,
                          0xBB, 0xCC, 0xAA, 0xBB});
    CHECK(f.Read(0x20000000, 16) == std::vector<u8>(16, 0xEE));
}

TEST_CASE("bounds: a fill with its end outside memory, or its start, is refused") {
    Fixture f;
    f.FillBytes(0x185FFFF0, 16, 0xEE);
    // End 0x18600008 is past the end of VRAM and in no area: refused, nothing is written.
    MemoryFill(f.host, MakeFill(0x185FFFF0, 0x18600008, 0x11223344, 0x201));
    CHECK(f.Read(0x185FFFF0, 16) == std::vector<u8>(16, 0xEE));
    // End exactly at the end of VRAM: valid, the 16 bytes are filled.
    MemoryFill(f.host, MakeFill(0x185FFFF0, kVramEnd, 0x11223344, 0x201));
    CHECK(f.Get32(0x185FFFF0, 3) == 0x11223344);
    CHECK(f.Get32(0x185FFFF0, 0) == 0x11223344);
}

TEST_CASE("bounds: DMA whose size runs past the end of an area copies the part inside") {
    // Source 16 bytes before the end of VRAM, size 0x20: the first 16 bytes are copied, the DMA
    // still signals (size 0x20 at 380 MiB/s is under 1 ns).
    {
        Fixture f;
        for (u32 i = 0; i < 16; ++i) {
            f.PutBytes(0x185FFFF0 + i, {static_cast<u8>(0x30 + i)});
        }
        f.FillBytes(kRamPhys, 0x30, 0xEE);
        f.Run({0, kVram + 0x5FFFF0, kRam, 0x20});
        std::vector<u8> expected(0x30, 0xEE);
        for (u32 i = 0; i < 16; ++i) {
            expected[i] = static_cast<u8>(0x30 + i);
        }
        CHECK(f.Read(kRamPhys, 0x30) == expected);
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].id == Interrupt::DMA);
    }
    // Destination 16 bytes before the end of VRAM: the first 16 bytes of the source arrive.
    {
        Fixture f;
        for (u32 i = 0; i < 0x20; ++i) {
            f.PutBytes(kRamPhys + i, {static_cast<u8>(0x50 + i)});
        }
        f.FillBytes(0x185FFFE0, 0x20, 0xEE);
        f.Run({0, kRam, kVram + 0x5FFFF0, 0x20});
        std::vector<u8> expected(0x20, 0xEE);
        for (u32 i = 0; i < 16; ++i) {
            expected[16 + i] = static_cast<u8>(0x50 + i);
        }
        CHECK(f.Read(0x185FFFE0, 0x20) == expected);
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].id == Interrupt::DMA);
    }
}

TEST_CASE("bounds: texture copy whose size runs past the end of an area") {
    const auto pattern = [](Fixture& f, u32 paddr, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            f.PutBytes(paddr + i, {static_cast<u8>(0x30 + i)});
        }
    };
    // Source 0x18 bytes before the end of VRAM, size 0x40, contiguous: the 0x18 bytes inside are
    // copied in the one chunk, the rest of the destination is not touched.
    {
        Fixture f;
        pattern(f, 0x185FFFE8, 0x18);
        f.FillBytes(kRamPhys, 0x50, 0xEE);
        f.Copy(kVram + 0x5FFFE8, kRam, 0x40, 0, 0);
        std::vector<u8> expected(0x50, 0xEE);
        for (u32 i = 0; i < 0x18; ++i) {
            expected[i] = static_cast<u8>(0x30 + i);
        }
        CHECK(f.Read(kRamPhys, 0x50) == expected);
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].id == Interrupt::PPF);
    }
    // The same source with an input gap (width 1 unit, gap 1 unit): chunk 0 is the 16 bytes at
    // offset 0 (inside), chunk 1 starts at source offset 32 (outside the 0x18 bytes): skipped, as
    // are chunks 2 and 3.
    {
        Fixture f;
        pattern(f, 0x185FFFE8, 0x18);
        f.FillBytes(kRamPhys, 0x50, 0xEE);
        f.Copy(kVram + 0x5FFFE8, kRam, 0x40, 0x00010001, 0);
        std::vector<u8> expected(0x50, 0xEE);
        for (u32 i = 0; i < 16; ++i) {
            expected[i] = static_cast<u8>(0x30 + i);
        }
        CHECK(f.Read(kRamPhys, 0x50) == expected);
        CHECK(f.Signals().size() == 1);
    }
    // Destination 0x18 bytes before the end of VRAM: only 0x18 bytes arrive.
    {
        Fixture f;
        pattern(f, kRamPhys, 0x40);
        f.FillBytes(0x185FFFD0, 0x30, 0xEE);
        f.Copy(kRam, kVram + 0x5FFFE8, 0x40, 0, 0);
        std::vector<u8> expected(0x30, 0xEE);
        for (u32 i = 0; i < 0x18; ++i) {
            expected[0x18 + i] = static_cast<u8>(0x30 + i);
        }
        CHECK(f.Read(0x185FFFD0, 0x30) == expected);
        CHECK(f.Signals().size() == 1);
    }
}

TEST_CASE("bounds: display transfers that run off the end of VRAM") {
    // Output: RGBA8 4x2 linear to linear into the last 16 bytes of VRAM. The image needs 32
    // bytes; row 0 (words 0..3) fits, row 1 is dropped.
    {
        Fixture f;
        for (u32 i = 0; i < 8; ++i) {
            f.Put32(kRamPhys, i, 0x01010101 * (i + 1));
        }
        f.FillBytes(0x185FFFE0, 0x20, 0xEE);
        f.Transfer(kRam, kVram + 0x5FFFF0, Size(4, 2), Size(4, 2), Flags(0, 1, 0, 1, 0, 0, 0));
        std::vector<u8> expected(16, 0xEE);
        for (u32 i = 0; i < 4; ++i) {
            expected.insert(expected.end(), 4, static_cast<u8>(i + 1));
        }
        CHECK(f.Read(0x185FFFE0, 0x20) == expected);
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].id == Interrupt::PPF);
    }
    // Output: RGBA8 6x1 to RGB8 in the last 8 bytes of VRAM. The texel i is (R, G, B) =
    // (0x10 + i, 0x20 + i, 0x30 + i) = memory bytes FF, 30+i, 20+i, 10+i; the RGB8 pixel is
    // stored B, G, R. Pixel 0 is bytes 0..2, pixel 1 is 3..5, pixel 2 is 6..8: 6 and 7 inside,
    // 8 dropped; pixels 3 to 5 are outside. The last 8 bytes: 30 20 10 31 21 11 32 22.
    {
        Fixture f;
        for (u32 i = 0; i < 6; ++i) {
            f.PutVec(kRamPhys + i * 4,
                     {0xFF, static_cast<u8>(0x30 + i), static_cast<u8>(0x20 + i),
                      static_cast<u8>(0x10 + i)});
        }
        f.FillBytes(0x185FFFE8, 0x18, 0xEE);
        f.Transfer(kRam, kVram + 0x5FFFF8, Size(6, 1), Size(6, 1), Flags(0, 1, 0, 1, 0, 1, 0));
        std::vector<u8> expected(16, 0xEE);
        expected.insert(expected.end(), {0x30, 0x20, 0x10, 0x31, 0x21, 0x11, 0x32, 0x22});
        CHECK(f.Read(0x185FFFE8, 0x18) == expected);
        CHECK(f.Signals().size() == 1);
    }
    // Input: RGB8 3x1 whose last pixel straddles the end of VRAM (8 bytes before it): pixel 0 is
    // bytes 01 02 03 (B, G, R), pixel 1 is 04 05 06, pixel 2 is bytes 6, 7 = 07 08 and a byte
    // past the end, read as 0: B 07, G 08, R 00. RGBA8 output memory is A, B, G, R:
    // FF 01 02 03 / FF 04 05 06 / FF 07 08 00.
    {
        Fixture f;
        f.PutBytes(0x185FFFF8, {1, 2, 3, 4, 5, 6, 7, 8});
        f.FillBytes(kRamPhys + 0x100, 16, 0xEE);
        f.Transfer(kVram + 0x5FFFF8, kRam + 0x100, Size(3, 1), Size(3, 1),
                   Flags(0, 1, 0, 1, 1, 0, 0));
        CHECK(f.Read(kRamPhys + 0x100, 13) ==
              std::vector<u8>{0xFF, 0x01, 0x02, 0x03, 0xFF, 0x04, 0x05, 0x06, 0xFF, 0x07, 0x08,
                              0x00, 0xEE});
        CHECK(f.Signals().size() == 1);
    }
}

// ================================================================================================
// N2-03b: the engine writes its output region and nothing else (the GSP's queue is not its)
// ================================================================================================

TEST_CASE("the output of a transfer, a copy and a DMA is all the engine writes; interrupts only signal") {
    // 16 pages of 0xA5; each command's expected region is worked out from its fields.
    auto fresh = [](Fixture& f) { f.FillBytes(kRamPhys, 0x10000, 0xA5); };
    SUBCASE("display transfer, linear to linear 16x16 RGBA8: 1024 bytes at the output") {
        Fixture f;
        fresh(f);
        for (u32 i = 0; i < 256; ++i) f.Put32(kRamPhys + 0x4000, i, 0x01020304 + i);
        f.Transfer(kRam + 0x4000, kRam + 0x8000, Size(16, 16), Size(16, 16), Flags(0, 1, 0, 1, 0, 0, 0));
        for (u32 i = 0; i < 256; ++i) REQUIRE(f.Get32(kRamPhys + 0x8000, i) == 0x01020304 + i);
        // Every other byte of the 16 pages is as it was, the input's included (0x4000..0x4400
        // holds the texels; the rest of the pages is 0xA5).
        const std::vector<u8> all = f.Read(kRamPhys, 0x10000);
        for (u32 i = 0; i < 0x10000; ++i) {
            if ((i >= 0x4000 && i < 0x4400) || (i >= 0x8000 && i < 0x8400)) continue;
            INFO("byte " << i);
            REQUIRE(all[i] == 0xA5);
        }
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].id == Interrupt::PPF);
    }
    SUBCASE("texture copy with gaps: 0x40 bytes in lines of 0x20 bytes with a 0x10 gap on the output") {
        Fixture f;
        fresh(f);
        for (u32 i = 0; i < 16; ++i) f.Put32(kRamPhys + 0x4000, i, 0x11110000 + i);
        // size 0x40, input contiguous (gap 0), output lines of 2 units (0x20 bytes) and a gap of
        // 1 unit (0x10 bytes): the first 0x20 bytes at 0x8000, the second at 0x8030.
        f.Copy(kRam + 0x4000, kRam + 0x8000, 0x40, 0, (1u << 16) | 2);
        for (u32 i = 0; i < 8; ++i) REQUIRE(f.Get32(kRamPhys + 0x8000, i) == 0x11110000 + i);
        for (u32 i = 0; i < 8; ++i) REQUIRE(f.Get32(kRamPhys + 0x8030, i) == 0x11110000 + 8 + i);
        const std::vector<u8> all = f.Read(kRamPhys, 0x10000);
        for (u32 i = 0; i < 0x10000; ++i) {
            if ((i >= 0x4000 && i < 0x4040) || (i >= 0x8000 && i < 0x8020) || (i >= 0x8030 && i < 0x8050)) continue;
            INFO("byte " << i);
            REQUIRE(all[i] == 0xA5);
        }
    }
    SUBCASE("DMA: exactly the bytes asked for") {
        Fixture f;
        fresh(f);
        f.PutBytes(kRamPhys + 0x4000, {1, 2, 3, 4, 5, 6, 7, 8});
        f.Run({0, kRam + 0x4000, kRam + 0x8000, 8});
        CHECK(f.Read(kRamPhys + 0x8000, 8) == std::vector<u8>{1, 2, 3, 4, 5, 6, 7, 8});
        const std::vector<u8> all = f.Read(kRamPhys, 0x10000);
        for (u32 i = 0; i < 0x10000; ++i) {
            if ((i >= 0x4000 && i < 0x4008) || (i >= 0x8000 && i < 0x8008)) continue;
            INFO("byte " << i);
            REQUIRE(all[i] == 0xA5);
        }
        const Signals s = f.Signals();
        REQUIRE(s.size() == 1);
        CHECK(s[0].id == Interrupt::DMA);
    }
}
