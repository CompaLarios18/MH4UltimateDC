// Derived from Azahar src/video_core/renderer_software/sw_blitter.cpp, src/video_core/gpu.cpp,
// src/video_core/pica/regs_external.h, src/video_core/utils.h and src/common/color.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of these files says
// "GPLv2 or any later version"): the algorithms that the expected values below are worked out
// from, and the register layouts.
// Copyright 2023 Citra Emulator Project (sw_blitter.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, regs_external.h)
// Copyright 2014 Citra Emulator Project (utils.h, color.h)
//
// mh4n_gpu_gx (N2-03): the GX engine on the CPU, with synthetic images. Every expected value is
// worked out by hand from the algorithm of Azahar's SwBlitter / GPU::Execute (the derivation is
// next to it), never by running the code under test.
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <vector>

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
