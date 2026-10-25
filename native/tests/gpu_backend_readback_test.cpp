// Derived from Azahar src/video_core/rasterizer_cache/texture_codec.h and src/common/color.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later, as the files say: "Licensed under GPLv2 or any later version")
// Copyright 2022-2026 Citra Emulator Project / Azahar Emulator Project (texture_codec.h)
// Copyright 2014 Citra Emulator Project (color.h)
//
// The tables below restate texture_codec.h's MORTON_ROW and MORTON_X and the byte orders and bit positions of color.h's Encode*.
//
// The backend's readback module (N2-28): ReadbackToGuest (render targets into guest memory in the PICA's Morton-tiled
// layouts and formats, bit-exact), ReadImage and the GPU timestamps of a frame with a readback in it. Run on whatever
// Vulkan device the machine has (lavapipe in the cloud and in Linux CI); a case that needs a device skips with a message
// without one, and fails under MH4N_REQUIRE_VULKAN=1.
//
// Every expected byte below is worked out by hand from the formats; none is computed with the code under test. The
// layout model (`GuestOffset`) is written out from Azahar's MORTON_ROW / MORTON_X tables, the other way round than the
// shader's bit arithmetic, and is itself pinned by literal bytes (see `the_tiles_are_8x8_morton_with_the_last_row_first`).
//
// Layout the tests pin (docs/native/rhi.md, "Sync and readback (N2-28)"): 8x8 tiles left to right then the next tile row;
// inside a tile the pixel (x, y) is at Morton index x0 y0 x1 y1 x2 y2 (x in the even bits); guest row 0 is the LAST row of
// the rect in the image. RGBA8 is A,B,G,R in memory, RGB8 B,G,R, the 16-bit formats little-endian words with red in the
// high bits, D16 a 16-bit word, D24 three bytes, D24S8 the depth in the low 24 bits and the stencil in the top byte.
//
// Lavapipe has D24S8 and the PC does not: the runs with MH4N_RHI_MASK=d24s8 (and the `Cap::D24S8` legs the depth cases run
// themselves) are the PC's path. Run it as the PC's paths would: MH4N_RHI_MASK=d24s8,timeline ctest --test-dir build/native
// -R '^gpu_backend_readback_test$'.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpu/backend/sync/sync.h"
#include "gpu/backend/targets/impl.h"
#include "gpu/backend/targets/upload.h"
#include "support/vulkan_device.h"

#include "gpu_backend_sync_shaders.inc"

using mh4n::s32;
using mh4n::u16;
using mh4n::u32;
using mh4n::u64;
using mh4n::u8;
namespace test = mh4n::test;
using namespace mh4n::gpu::rhi;

namespace {

// ---- helpers ---------------------------------------------------------------------------------------------------

template <size_t N>
ShaderCode Spv(const uint32_t (&words)[N]) {
    ShaderCode code;
    code.spirv = std::span<const u32>(words, N);
    return code;
}

struct BreachCapture {
    std::mutex mutex;
    std::vector<OwnerRule> rules;
    OwnerRuleHandler previous;
    BreachCapture() {
        previous = SetOwnerRuleHandler([this](const OwnerRuleBreach& breach) {
            std::lock_guard lock(mutex);
            rules.push_back(breach.rule);
        });
    }
    ~BreachCapture() { SetOwnerRuleHandler(previous); }
    int Count(OwnerRule rule) {
        std::lock_guard lock(mutex);
        return static_cast<int>(std::count(rules.begin(), rules.end(), rule));
    }
    size_t Total() {
        std::lock_guard lock(mutex);
        return rules.size();
    }
};

#define REQUIRE_CHECKS_COMPILED_IN()                                                                                   \
    do {                                                                                                               \
        if (!kRhiChecks) {                                                                                             \
            WARN_MESSAGE(false, "skipped: the owner-rule checks are compiled out (MH4N_RHI_CHECKS=OFF)");              \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

std::shared_ptr<Backend> MakeBackend(std::initializer_list<Cap> masked = {}) {
    BackendOptions options;
    for (Cap cap : masked) options.mask = options.mask.With(cap);
    return test::RequireBackend(options);
}

std::shared_ptr<Image> MakeImage(Backend& backend, Format format, u32 width, u32 height) {
    ImageDesc desc;
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.name = "test image";
    Result<std::shared_ptr<Image>> image = backend.CreateImage(desc);
    REQUIRE_MESSAGE(image.ok(), image.error());
    return *image;
}

std::shared_ptr<Pipeline> MakePipeline(Backend& backend, ShaderCode vertex, ShaderCode fragment, Format color, Format depth,
                                       const FixedFunctionState& state = {}) {
    GraphicsPipelineDesc desc;
    desc.vertex = vertex;
    desc.fragment = fragment;
    desc.color_format = color;
    desc.depth_format = depth;
    desc.state = state;
    desc.name = "readback test";
    Result<std::shared_ptr<Pipeline>> pipeline = backend.CreateGraphicsPipeline(desc);
    REQUIRE_MESSAGE(pipeline.ok(), pipeline.error());
    return *pipeline;
}

ClearValue Color(float r, float g, float b, float a) {
    ClearValue value;
    value.color = {r, g, b, a};
    return value;
}

ClearValue Depth(float depth, u32 stencil) {
    ClearValue value;
    value.depth = depth;
    value.stencil = stencil;
    return value;
}

void Run(Backend& backend, Recorder&& rec) {
    backend.WaitIdle(backend.Submit(std::move(rec)));
}

void ClearAndWait(Backend& backend, Image& image, const ClearValue& value) {
    Recorder rec = backend.BeginRecording();
    rec.ClearImage(image, value);
    Run(backend, std::move(rec));
}

// Fills an image from host bytes in the image's stored format (tightly packed rows).
void Fill(Backend& backend, Image& image, u32 width, u32 height, const std::vector<u8>& bytes) {
    Recorder rec = backend.BeginRecording();
    BufferImageRegion region;
    region.image_rect = Rect{0, 0, width, height};
    Result<void> upload = UploadToImage(backend, rec, image, region, bytes);
    REQUIRE_MESSAGE(upload.ok(), upload.error());
    Run(backend, std::move(rec));
}

std::vector<u8> Readback(Backend& backend, const Image& image, u32 width, u32 height, PicaFormat format, u32 bytes_per_pixel,
                         Rect rect = {}) {
    if (rect.width == 0) rect = Rect{0, 0, width, height};
    std::vector<u8> bytes(size_t(rect.width) * rect.height * bytes_per_pixel, 0xCC);
    Result<void> result = backend.ReadbackToGuest(image, rect, format, bytes);
    REQUIRE_MESSAGE(result.ok(), result.error());
    return bytes;
}

std::vector<u8> Repeat(const std::vector<u8>& pixel, size_t count) {
    std::vector<u8> bytes;
    for (size_t i = 0; i < count; ++i) bytes.insert(bytes.end(), pixel.begin(), pixel.end());
    return bytes;
}

// ---- the layout model, from Azahar's tables -------------------------------------------------------------------------

// The byte offset in the tiled buffer of the pixel at guest column gx and guest row gy of a buffer `width` wide, for
// `bpp` bytes per pixel: tiles left to right then up the guest rows, 64 pixels per tile in Morton order. Azahar's
// MORTON_ROW and MORTON_X (texture_codec.h): the index of (x, y) inside a tile is MORTON_ROW[y] + MORTON_X[x].
size_t GuestOffset(u32 width, u32 gx, u32 gy, u32 bpp) {
    static const u32 kRow[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    static const u32 kX[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    const size_t tile = size_t(gy / 8) * (width / 8) + gx / 8;
    return (tile * 64 + kRow[gy % 8] + kX[gx % 8]) * bpp;
}

// The tiled bytes of an image `width` x `height` whose pixel at image (x, iy) has the bytes `pixel(x, iy)`:
// guest row g is image row height - 1 - g.
template <typename F>
std::vector<u8> Tiled(u32 width, u32 height, u32 bpp, F pixel) {
    std::vector<u8> bytes(size_t(width) * height * bpp, 0xEE);
    for (u32 gy = 0; gy < height; ++gy) {
        for (u32 gx = 0; gx < width; ++gx) {
            const std::vector<u8> p = pixel(gx, height - 1 - gy);
            REQUIRE(p.size() == bpp);
            std::memcpy(bytes.data() + GuestOffset(width, gx, gy, bpp), p.data(), bpp);
        }
    }
    return bytes;
}

// The RGBA8 pattern: texel (x, iy) = (R = 16 x, G = 32 iy, B = 0x55, A = 0xFF) for x < 16, iy < 8.
std::vector<u8> PatternImageBytes() {
    std::vector<u8> bytes;
    for (u32 iy = 0; iy < 8; ++iy) {
        for (u32 x = 0; x < 16; ++x) {
            bytes.insert(bytes.end(), {static_cast<u8>(16 * x), static_cast<u8>(32 * iy), 0x55, 0xFF});
        }
    }
    return bytes;
}

std::vector<u8> Le16(u32 word) {
    return {static_cast<u8>(word & 0xFF), static_cast<u8>((word >> 8) & 0xFF)};
}

}  // namespace

// ---- colour ----------------------------------------------------------------------------------------------------------

TEST_CASE("colour/clear_and_readback_of_every_colour_format_into_guest_memory") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    // RGBA8: the clear (1.0, 0.0, 0.2, 0.6) is (255, 0, 51, 153) = (0xFF, 0x00, 0x33, 0x99); memory order A, B, G, R.
    {
        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 16, 8);
        ClearAndWait(*backend, *image, Color(1.0f, 0.0f, 0.2f, 0.6f));
        CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgba8, 4) == Repeat({0x99, 0x33, 0x00, 0xFF}, 128));
        // RGB8 from the same image: alpha dropped, memory order B, G, R
        CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgb8, 3) == Repeat({0x33, 0x00, 0xFF}, 128));
    }
    // Rgb8 as the renderer makes it (the format name, stored as RGBA8)
    {
        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgb8, 8, 8);
        ClearAndWait(*backend, *image, Color(1.0f, 0.0f, 0.2f, 0.6f));
        CHECK(Readback(*backend, *image, 8, 8, PicaFormat::Rgb8, 3) == Repeat({0x33, 0x00, 0xFF}, 64));
    }
    // The 16-bit formats, from the PICA names (stored natively or as RGBA8, whatever the device chose) with components
    // that are exact in their bit width: RGB565 (r5 21, g6 42, b5 10) = 21 << 11 | 42 << 5 | 10 = 0xAD4A;
    // RGB5A1 (21, 10, 5, 1) = 21 << 11 | 10 << 6 | 5 << 1 | 1 = 0xAA8B; RGBA4 (10, 5, 12, 3) = 0xA5C3.
    struct Case16 {
        Format name;
        PicaFormat pica;
        ClearValue clear;
        u32 word;
    };
    const Case16 cases[] = {
        {Format::Rgb565, PicaFormat::Rgb565, Color(21.0f / 31, 42.0f / 63, 10.0f / 31, 1.0f), 0xAD4A},
        {Format::Rgb5A1, PicaFormat::Rgb5A1, Color(21.0f / 31, 10.0f / 31, 5.0f / 31, 1.0f), 0xAA8B},
        {Format::Rgba4, PicaFormat::Rgba4, Color(10.0f / 15, 5.0f / 15, 12.0f / 15, 3.0f / 15), 0xA5C3},
    };
    for (const Case16& c : cases) {
        INFO("PICA format " << static_cast<int>(c.pica));
        std::shared_ptr<Image> image = MakeImage(*backend, c.name, 16, 8);
        ClearAndWait(*backend, *image, c.clear);
        CHECK(Readback(*backend, *image, 16, 8, c.pica, 2) == Repeat(Le16(c.word), 128));
    }
    // The same words from an RGBA8 image (the fallback of a device without the 16-bit formats): components cut from the
    // top, as Azahar's Convert8To5, Convert8To6 and Convert8To4 do
    {
        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 16, 8);
        ClearAndWait(*backend, *image, cases[0].clear);
        CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgb565, 2) == Repeat(Le16(0xAD4A), 128));
        ClearAndWait(*backend, *image, cases[1].clear);
        CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgb5A1, 2) == Repeat(Le16(0xAA8B), 128));
        ClearAndWait(*backend, *image, cases[2].clear);
        CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgba4, 2) == Repeat(Le16(0xA5C3), 128));
    }
}

TEST_CASE("colour/every_stored_component_order_of_the_16_bit_formats_gives_the_pica_word") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    // A pixel whose three colour components differ, as the PICA word and as the word each Vulkan order stores:
    // Vulkan names the FIRST letter the highest bits (rhi.h, Format). RGB565 (r 21, g 42, b 10): the PICA word is
    // r<<11 | g<<5 | b = 0xAD4A; B5G6R5 stores b<<11 | g<<5 | r = 10 << 11 | 42 << 5 | 21 = 0x5555 (0x5000 + 0x540 + 0x15 = 0x5555).
    // RGB5A1 (r 21, g 10, b 5, a 1): PICA r<<11 | g<<6 | b<<1 | a = 0xAA8B; B5G5R5A1 b<<11 | g<<6 | r<<1 | a = 5<<11 | 10<<6 | 21<<1 | 1
    // = 0x2800 + 0x280 + 0x2A + 1 = 0x2AAB; A1R5G5B5 a<<15 | r<<10 | g<<5 | b = 0x8000 + 0x5400 + 0x140 + 5 = 0xD545.
    // RGBA4 (r 10, g 5, b 12, a 3): PICA 0xA5C3; B4G4R4A4 b<<12 | g<<8 | r<<4 | a = 0xC000 + 0x500 + 0xA0 + 3 = 0xC5A3.
    struct Order {
        Format stored;
        PicaFormat pica;
        u32 stored_word;
        u32 pica_word;
    };
    const Order orders[] = {
        {Format::R5G6B5, PicaFormat::Rgb565, 0xAD4A, 0xAD4A},     {Format::B5G6R5, PicaFormat::Rgb565, 0x5555, 0xAD4A},
        {Format::R5G5B5A1, PicaFormat::Rgb5A1, 0xAA8B, 0xAA8B},   {Format::B5G5R5A1, PicaFormat::Rgb5A1, 0x2AAB, 0xAA8B},
        {Format::A1R5G5B5, PicaFormat::Rgb5A1, 0xD545, 0xAA8B},   {Format::R4G4B4A4, PicaFormat::Rgba4, 0xA5C3, 0xA5C3},
        {Format::B4G4R4A4, PicaFormat::Rgba4, 0xC5A3, 0xA5C3},
    };
    for (const Order& o : orders) {
        INFO("stored format " << static_cast<int>(o.stored));
        if (!backend->QueryFormat(o.stored).sampled) {
            WARN_MESSAGE(false, "skipped: the device cannot sample this order");
            continue;
        }
        std::shared_ptr<Image> image = MakeImage(*backend, o.stored, 8, 8);
        Fill(*backend, *image, 8, 8, Repeat(Le16(o.stored_word), 64));
        CHECK(Readback(*backend, *image, 8, 8, o.pica, 2) == Repeat(Le16(o.pica_word), 64));
    }
}

TEST_CASE("colour/the_tiles_are_8x8_morton_with_the_last_row_first") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 16, 8);
    Fill(*backend, *image, 16, 8, PatternImageBytes());
    const std::vector<u8> got = Readback(*backend, *image, 16, 8, PicaFormat::Rgba8, 4);
    REQUIRE(got.size() == 512);
    // By hand: guest row g is image row 7 - g; the pixel at tile index p is (x0 y0 x1 y1 x2 y2 from the low bit up):
    //   p = 0  (0,0): image (0, 7): R 0x00, G 7*32 = 0xE0      p = 1  (1,0): R 0x10, G 0xE0
    //   p = 2  (0,1): image (0, 6): R 0x00, G 6*32 = 0xC0      p = 3  (1,1): R 0x10, G 0xC0
    //   p = 4  (2,0): R 0x20, G 0xE0                            p = 8  (0,2): image (0, 5): G 5*32 = 0xA0
    //   p = 16 (4,0): R 0x40, G 0xE0                            p = 32 (0,4): image (0, 3): G 3*32 = 0x60
    //   p = 63 (7,7): image (7, 0): R 7*16 = 0x70, G 0
    // and the second tile (offset 64 pixels) starts at image column 8: p = 0 is R 8*16 = 0x80, G 0xE0. Bytes are A, B, G, R.
    const auto at = [&](size_t pixel) { return std::vector<u8>(got.begin() + pixel * 4, got.begin() + pixel * 4 + 4); };
    CHECK(at(0) == std::vector<u8>{0xFF, 0x55, 0xE0, 0x00});
    CHECK(at(1) == std::vector<u8>{0xFF, 0x55, 0xE0, 0x10});
    CHECK(at(2) == std::vector<u8>{0xFF, 0x55, 0xC0, 0x00});
    CHECK(at(3) == std::vector<u8>{0xFF, 0x55, 0xC0, 0x10});
    CHECK(at(4) == std::vector<u8>{0xFF, 0x55, 0xE0, 0x20});
    CHECK(at(8) == std::vector<u8>{0xFF, 0x55, 0xA0, 0x00});
    CHECK(at(16) == std::vector<u8>{0xFF, 0x55, 0xE0, 0x40});
    CHECK(at(32) == std::vector<u8>{0xFF, 0x55, 0x60, 0x00});
    CHECK(at(63) == std::vector<u8>{0xFF, 0x55, 0x00, 0x70});
    CHECK(at(64) == std::vector<u8>{0xFF, 0x55, 0xE0, 0x80});
    // and the whole buffer against the model written from Azahar's tables
    const auto rgba8 = [](u32 x, u32 iy) { return std::vector<u8>{0xFF, 0x55, static_cast<u8>(32 * iy), static_cast<u8>(16 * x)}; };
    CHECK(got == Tiled(16, 8, 4, rgba8));

    // RGB8: three bytes B, G, R per pixel, so words of the output straddle two pixels
    const auto rgb8 = [](u32 x, u32 iy) { return std::vector<u8>{0x55, static_cast<u8>(32 * iy), static_cast<u8>(16 * x)}; };
    CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgb8, 3) == Tiled(16, 8, 3, rgb8));
    // The 16-bit formats from the 8-bit components, cut from the top:
    // RGB565 r>>3 << 11 | g>>2 << 5 | b>>3 with b = 0x55 >> 3 = 10: (2x) << 11 | (8 iy) << 5 | 10
    const auto rgb565 = [](u32 x, u32 iy) { return Le16((2 * x) << 11 | (8 * iy) << 5 | 10); };
    CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgb565, 2) == Tiled(16, 8, 2, rgb565));
    // RGB5A1: (2x) << 11 | (4 iy) << 6 | 10 << 1 | 1 (the alpha 0xFF >> 7)
    const auto rgb5a1 = [](u32 x, u32 iy) { return Le16((2 * x) << 11 | (4 * iy) << 6 | 10 << 1 | 1); };
    CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgb5A1, 2) == Tiled(16, 8, 2, rgb5a1));
    // RGBA4: x << 12 | (2 iy) << 8 | (0x55 >> 4 = 5) << 4 | 0xF
    const auto rgba4 = [](u32 x, u32 iy) { return Le16(x << 12 | (2 * iy) << 8 | 5 << 4 | 0xF); };
    CHECK(Readback(*backend, *image, 16, 8, PicaFormat::Rgba4, 2) == Tiled(16, 8, 2, rgba4));

    // A sub-rect of tiles: the second tile alone (x 8..15), 8x8, is the second tile of the whole buffer
    const std::vector<u8> second = Readback(*backend, *image, 16, 8, PicaFormat::Rgba8, 4, Rect{8, 0, 8, 8});
    CHECK(second == std::vector<u8>(got.begin() + 256, got.end()));
}

TEST_CASE("colour/a_taller_image_orders_tile_rows_from_the_last_image_row_up") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    // 8 wide, 16 tall: guest tile row 0 is the LAST 8 image rows. Texel (x, iy) = (R = 8 x, G = 16 iy, B = iy / 8 * 0x80, A = 0xFF).
    std::vector<u8> bytes;
    for (u32 iy = 0; iy < 16; ++iy) {
        for (u32 x = 0; x < 8; ++x) bytes.insert(bytes.end(), {static_cast<u8>(8 * x), static_cast<u8>(16 * iy), static_cast<u8>(iy / 8 * 0x80), 0xFF});
    }
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 8, 16);
    Fill(*backend, *image, 8, 16, bytes);
    const std::vector<u8> got = Readback(*backend, *image, 8, 16, PicaFormat::Rgba8, 4);
    // By hand: the first pixel of the buffer is guest (0, 0) = image (0, 15): R 0, G 15*16 = 0xF0, B 1*0x80 = 0x80 -> A,B,G,R = FF 80 F0 00;
    // the first pixel of the second tile (offset 64 pixels) is guest (0, 8) = image (0, 7): G 0x70, B 0 -> FF 00 70 00.
    CHECK(std::vector<u8>(got.begin(), got.begin() + 4) == std::vector<u8>{0xFF, 0x80, 0xF0, 0x00});
    CHECK(std::vector<u8>(got.begin() + 256, got.begin() + 260) == std::vector<u8>{0xFF, 0x00, 0x70, 0x00});
    const auto pixel = [](u32 x, u32 iy) { return std::vector<u8>{0xFF, static_cast<u8>(iy / 8 * 0x80), static_cast<u8>(16 * iy), static_cast<u8>(8 * x)}; };
    CHECK(got == Tiled(8, 16, 4, pixel));
}

// ---- depth -----------------------------------------------------------------------------------------------------------

namespace {

// The depths the cases compare across formats: exact 24-bit values c, cleared as the float c / 16777215 (the nearest float; for 0x800000
// that is 0x3F000001, 0.5 + 2^-24, since 0x800000 / 16777215 = 0.5 + 2.98e-8 lies above the midpoint 0.5 + 2^-25 of 0.5 and 0.5 + 2^-24).
// The pass reads a depth as uint(depth * 16777215.0), Azahar's rule: for these floats the product is the integer c (all 2^24 values
// checked once in single precision), so a D24S8 image and the D32S8 that stands in for it give the same bytes. A float between two
// 24-bit values is not in this list on purpose: 0.25 * 16777215 = 4194303.75 truncates to 0x3FFFFF on D32S8, while a D24 target stores
// the nearest value, 4194304 = 0x400000 (see `kQuarter`).
struct DepthCase {
    float clear;
    u32 d24;
};
const DepthCase kDepths[] = {
    {0.0f, 0x000000},
    {float(0x800000) / 16777215.0f, 0x800000},
    {1.0f, 0xFFFFFF},
};
// 0.25: what each format gives, worked out by hand. D32S8 holds 0.25 exactly: 4194303.75 truncated is 0x3FFFFF. A D24S8 target
// converts the float it is given to 24 bits itself, and lavapipe's rounds to nearest (4194304 = 0x400000, measured here; a driver
// that truncates would give 0x3FFFFF: the PC's D24 path is the D32S8 one, so this value is lavapipe's, `unverified` elsewhere).
constexpr float kQuarter = 0.25f;
u32 QuarterD24(Format stored) { return stored == Format::D32S8 || stored == Format::D32Float ? 0x3FFFFF : 0x400000; }

std::vector<u8> D24S8Bytes(u32 d24, u32 stencil) {
    return {static_cast<u8>(d24 & 0xFF), static_cast<u8>((d24 >> 8) & 0xFF), static_cast<u8>((d24 >> 16) & 0xFF), static_cast<u8>(stencil)};
}

}  // namespace

TEST_CASE("depth/d24s8_and_the_masked_d32s8_give_the_same_bytes_for_every_depth_and_stencil") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        std::shared_ptr<Image> image = MakeImage(*backend, Format::D24S8, 16, 8);
        // the format the device stores: D24S8 where it has it, D32S8 where it does not (the PC) or the bit is masked
        CHECK(image->GetFormat() == (backend->Caps().d24s8 ? Format::D24S8 : Format::D32S8));
        if (masked) CHECK(image->GetFormat() == Format::D32S8);
        for (const DepthCase& depth : kDepths) {
            for (const u32 stencil : {0u, 0xFFu}) {
                INFO("depth " << depth.d24 << " stencil " << stencil << " masked " << masked);
                ClearAndWait(*backend, *image, Depth(depth.clear, stencil));
                // D24S8: depth in the low 24 bits, stencil in the top byte: bytes d0, d1, d2, s
                CHECK(Readback(*backend, *image, 16, 8, PicaFormat::D24S8, 4) == Repeat(D24S8Bytes(depth.d24, stencil), 128));
                // D24: the three depth bytes
                CHECK(Readback(*backend, *image, 16, 8, PicaFormat::D24, 3) ==
                      Repeat({static_cast<u8>(depth.d24 & 0xFF), static_cast<u8>((depth.d24 >> 8) & 0xFF), static_cast<u8>(depth.d24 >> 16)}, 128));
            }
        }
        // 0.25: pinned per format, not compared (see kQuarter): 0x3FFFFF on D32S8, 0x400000 on lavapipe's D24S8
        ClearAndWait(*backend, *image, Depth(kQuarter, 0x5A));
        const u32 quarter = QuarterD24(image->GetFormat());
        CHECK(Readback(*backend, *image, 16, 8, PicaFormat::D24S8, 4) == Repeat(D24S8Bytes(quarter, 0x5A), 128));
        if (masked) CHECK(quarter == 0x3FFFFF);  // a D32S8 image truncates: the unmasked mutant "rounds to nearest" gives 0x400000
    }
}

TEST_CASE("depth/d16_and_d32_float") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    // D16: 1/3 * 65535 = 21845 = 0x5555 exactly, 1.0 is 0xFFFF, 0 is 0
    std::shared_ptr<Image> d16 = MakeImage(*backend, Format::D16Unorm, 8, 8);
    for (const auto& [clear, word] : {std::pair{0.0f, 0x0000u}, std::pair{1.0f / 3.0f, 0x5555u}, std::pair{1.0f, 0xFFFFu}}) {
        ClearAndWait(*backend, *d16, Depth(clear, 0));
        CHECK(Readback(*backend, *d16, 8, 8, PicaFormat::D16, 2) == Repeat(Le16(word), 64));
    }
    // D32 float read as D24 (the depth without a stencil), truncated as Azahar's EncodeD24 does: 0.25 * 16777215 = 4194303.75 -> 0x3FFFFF,
    // 1.0 -> 0xFFFFFF
    std::shared_ptr<Image> d32 = MakeImage(*backend, Format::D32Float, 8, 8);
    ClearAndWait(*backend, *d32, Depth(0.25f, 0));
    CHECK(Readback(*backend, *d32, 8, 8, PicaFormat::D24, 3) == Repeat({0xFF, 0xFF, 0x3F}, 64));
    ClearAndWait(*backend, *d32, Depth(1.0f, 0));
    CHECK(Readback(*backend, *d32, 8, 8, PicaFormat::D24, 3) == Repeat({0xFF, 0xFF, 0xFF}, 64));
}

TEST_CASE("depth/an_image_left_in_the_attachment_layout_by_a_pass_open_at_submit_is_read_back_right") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        for (const Format depth_name : {Format::D24S8, Format::D16Unorm}) {
            INFO("masked " << masked << " depth format " << static_cast<int>(depth_name));
            std::shared_ptr<Image> color = MakeImage(*backend, Format::Rgba8Unorm, 8, 8);
            std::shared_ptr<Image> depth = MakeImage(*backend, depth_name, 8, 8);
            FixedFunctionState state;
            state.depth_test = true;
            state.depth_write = true;
            state.depth_compare = CompareOp::Always;
            const std::shared_ptr<Pipeline> draw = MakePipeline(*backend, Spv(spv_fs_vert_z25), Spv(spv_red_frag), Format::Rgba8Unorm, depth->GetFormat(), state);
            Recorder rec = backend->BeginRecording();
            TargetPassDesc pass;
            pass.color = color.get();
            pass.depth = depth.get();
            pass.color_load = LoadOp::Clear;
            pass.depth_load = LoadOp::Clear;
            pass.clear_color = {0, 0, 1, 1};
            pass.clear_depth = 1.0f;
            pass.clear_stencil = 0x5A;
            rec.BeginTargetPass(pass);
            rec.SetPipeline(*draw);
            rec.Draw(3);  // depth 0.25 over the whole image
            backend->WaitIdle(backend->Submit(std::move(rec)));  // the pass is still open: no barrier follows it
            // the depth image is still in plume's attachment layout, and the layout plume holds says so
            CHECK(depth->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::DEPTH_WRITE);
            if (depth_name == Format::D24S8) {
                // 0.25 (the draw's depth): 0x3FFFFF truncated from the D32S8 image's float, 0x400000 as lavapipe's D24S8 stores it
                // (kQuarter); the stencil is the clear's 0x5A
                CHECK(Readback(*backend, *depth, 8, 8, PicaFormat::D24S8, 4) ==
                      Repeat(D24S8Bytes(QuarterD24(depth->GetFormat()), 0x5A), 64));
            } else {
                // D16: 0.25 * 65535 = 16383.75 -> 16384 = 0x4000
                CHECK(Readback(*backend, *depth, 8, 8, PicaFormat::D16, 2) == Repeat(Le16(0x4000), 64));
            }
            // the first use moved it to GENERAL; the colour image drew red where the depth test passed (all of it)
            CHECK(depth->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::GENERAL);
            CHECK(Readback(*backend, *color, 8, 8, PicaFormat::Rgba8, 4) == Repeat({0xFF, 0x00, 0x00, 0xFF}, 64));
        }
    }
}

// ---- ReadImage ---------------------------------------------------------------------------------------------------------

TEST_CASE("read_image/a_cleared_image_returns_the_clear_colour_in_the_images_own_format") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    {
        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        ClearAndWait(*backend, *image, Color(1.0f, 0.0f, 0.2f, 0.6f));
        Result<std::vector<u8>> bytes = backend->ReadImage(*image, 0, 0, 4, 4);
        REQUIRE_MESSAGE(bytes.ok(), bytes.error());
        CHECK(*bytes == Repeat({0xFF, 0x00, 0x33, 0x99}, 16));  // R, G, B, A in memory order
    }
    {
        std::shared_ptr<Image> image = MakeImage(*backend, Format::R8Unorm, 4, 4);
        ClearAndWait(*backend, *image, Color(0.2f, 0, 0, 0));
        Result<std::vector<u8>> bytes = backend->ReadImage(*image, 0, 0, 4, 4);
        REQUIRE_MESSAGE(bytes.ok(), bytes.error());
        CHECK(*bytes == Repeat({0x33}, 16));  // 0.2 * 255 = 51
    }
    if (backend->QueryFormat(Format::R5G6B5).sampled) {
        std::shared_ptr<Image> image = MakeImage(*backend, Format::R5G6B5, 4, 4);
        ClearAndWait(*backend, *image, Color(21.0f / 31, 42.0f / 63, 10.0f / 31, 1.0f));
        Result<std::vector<u8>> bytes = backend->ReadImage(*image, 0, 0, 4, 4);
        REQUIRE_MESSAGE(bytes.ok(), bytes.error());
        CHECK(*bytes == Repeat(Le16(0xAD4A), 16));  // the stored word, little endian
    }
    {
        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba16Float, 4, 4);
        ClearAndWait(*backend, *image, Color(0.5f, 1.0f, 0.0f, 0.25f));
        Result<std::vector<u8>> bytes = backend->ReadImage(*image, 0, 0, 4, 4);
        REQUIRE_MESSAGE(bytes.ok(), bytes.error());
        // half floats: 0.5 = 0x3800, 1.0 = 0x3C00, 0.0, 0.25 = 0x3400
        CHECK(*bytes == Repeat({0x00, 0x38, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x34}, 16));
    }
    {
        std::shared_ptr<Image> image = MakeImage(*backend, Format::D16Unorm, 4, 4);
        ClearAndWait(*backend, *image, Depth(1.0f / 3.0f, 0));
        Result<std::vector<u8>> bytes = backend->ReadImage(*image, 0, 0, 4, 4);
        REQUIRE_MESSAGE(bytes.ok(), bytes.error());
        CHECK(*bytes == Repeat(Le16(0x5555), 16));
    }
    // depth and stencil: the depth plane (4 bytes per texel) then the stencil plane (1 byte per texel), as patch 11's copy
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> b = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!b) return;
        std::shared_ptr<Image> image = MakeImage(*b, Format::D24S8, 4, 4);
        ClearAndWait(*b, *image, Depth(1.0f, 0x3C));
        Result<std::vector<u8>> bytes = b->ReadImage(*image, 0, 0, 4, 4);
        REQUIRE_MESSAGE(bytes.ok(), bytes.error());
        REQUIRE(bytes->size() == 16 * 4 + 16);
        for (size_t i = 0; i < 16; ++i) {
            // depth 1.0: D24 0xFFFFFF in the low 24 bits (the top byte of a D24 texel is not defined), or 1.0f = 0x3F800000
            if (image->GetFormat() == Format::D24S8) {
                CHECK(((*bytes)[i * 4] == 0xFF && (*bytes)[i * 4 + 1] == 0xFF && (*bytes)[i * 4 + 2] == 0xFF));
            } else {
                CHECK(std::vector<u8>(bytes->begin() + i * 4, bytes->begin() + i * 4 + 4) == std::vector<u8>{0x00, 0x00, 0x80, 0x3F});
            }
            CHECK((*bytes)[64 + i] == 0x3C);
        }
    }
}

TEST_CASE("read_image/a_region_comes_row_by_row_without_padding_and_a_bad_region_is_an_error") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 16, 8);
    Fill(*backend, *image, 16, 8, PatternImageBytes());
    // the region x 3..7, y 2..5 (5 x 4): row iy = 2 + r holds, for column x = 3 + c, (16 x, 32 iy, 0x55, 0xFF)
    Result<std::vector<u8>> bytes = backend->ReadImage(*image, 3, 2, 5, 4);
    REQUIRE_MESSAGE(bytes.ok(), bytes.error());
    std::vector<u8> expected;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 5; ++c) expected.insert(expected.end(), {static_cast<u8>(16 * (3 + c)), static_cast<u8>(32 * (2 + r)), 0x55, 0xFF});
    }
    CHECK(*bytes == expected);
    CHECK(bytes->size() == 5 * 4 * 4);                       // no padding between the rows
    CHECK((*bytes)[0] == 0x30);                              // texel (3, 2): R = 48
    CHECK((*bytes)[1] == 0x40);                              // G = 64
    CHECK_FALSE(backend->ReadImage(*image, 12, 0, 5, 1).ok());   // 12 + 5 > 16
    CHECK_FALSE(backend->ReadImage(*image, 0, 0, 0, 1).ok());    // empty
    CHECK_FALSE(backend->ReadImage(*image, 0, 7, 1, 2).ok());    // 7 + 2 > 8
    std::shared_ptr<Image> bc = MakeImage(*backend, Format::Bc1Unorm, 8, 8);
    // block-compressed: not rows of texels (with the bc7 bit masked the image is RGBA8 and reads like one)
    CHECK(backend->ReadImage(*bc, 0, 0, 4, 4).ok() == (bc->GetFormat() != Format::Bc1Unorm));
}

// ---- errors and the owner rule -------------------------------------------------------------------------------------------

TEST_CASE("errors/a_bad_rect_destination_or_format_is_an_error_not_a_crash") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 16, 8);
    ClearAndWait(*backend, *image, Color(0, 0, 0, 1));
    std::vector<u8> big(16 * 8 * 4);
    CHECK(backend->ReadbackToGuest(*image, Rect{0, 0, 16, 8}, PicaFormat::Rgba8, big).ok());
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{0, 0, 12, 8}, PicaFormat::Rgba8, big).ok());   // not whole tiles
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{0, 0, 16, 0}, PicaFormat::Rgba8, big).ok());   // empty
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{8, 0, 16, 8}, PicaFormat::Rgba8, big).ok());   // outside
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{4, 0, 8, 8}, PicaFormat::Rgba8, big).ok());    // not on a tile
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{-8, 0, 8, 8}, PicaFormat::Rgba8, big).ok());
    std::vector<u8> small(16 * 8 * 4 - 1);
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{0, 0, 16, 8}, PicaFormat::Rgba8, small).ok());  // one byte short
    // formats an image of this kind cannot give
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{0, 0, 16, 8}, PicaFormat::D24S8, big).ok());
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{0, 0, 16, 8}, PicaFormat::D16, big).ok());
    std::shared_ptr<Image> depth = MakeImage(*backend, Format::D24S8, 8, 8);
    CHECK_FALSE(backend->ReadbackToGuest(*depth, Rect{0, 0, 8, 8}, PicaFormat::Rgba8, big).ok());
    CHECK_FALSE(backend->ReadbackToGuest(*depth, Rect{0, 0, 8, 8}, PicaFormat::D16, big).ok());
    if (backend->QueryFormat(Format::R5G6B5).sampled) {
        std::shared_ptr<Image> packed = MakeImage(*backend, Format::R5G6B5, 8, 8);
        CHECK_FALSE(backend->ReadbackToGuest(*packed, Rect{0, 0, 8, 8}, PicaFormat::Rgba8, big).ok());  // 5 bits cannot fill 8
        CHECK_FALSE(backend->ReadbackToGuest(*packed, Rect{0, 0, 8, 8}, PicaFormat::Rgba4, big).ok());  // another layout
        CHECK_FALSE(backend->ReadbackToGuest(*packed, Rect{0, 0, 8, 8}, PicaFormat::Rgb5A1, big).ok());
    }
    // an image the calling thread's open recorder has recorded on: a readback sees only what has been submitted
    Recorder open = backend->BeginRecording();
    open.ClearImage(*image, Color(1, 1, 1, 1));
    CHECK_FALSE(backend->ReadbackToGuest(*image, Rect{0, 0, 16, 8}, PicaFormat::Rgba8, big).ok());
    CHECK_FALSE(backend->ReadImage(*image, 0, 0, 4, 4).ok());
    Run(*backend, std::move(open));
    CHECK(backend->ReadbackToGuest(*image, Rect{0, 0, 16, 8}, PicaFormat::Rgba8, big).ok());
    CHECK(big == Repeat({0xFF, 0xFF, 0xFF, 0xFF}, 128));
}

TEST_CASE("owner_rule/a_readback_from_a_thread_that_does_not_own_the_image_calls_the_handler_and_one_after_a_hand_over_does_not") {
    REQUIRE_CHECKS_COMPILED_IN();
    for (const bool timeline_masked : {false, true}) {
        std::shared_ptr<Backend> backend = timeline_masked ? MakeBackend({Cap::Timeline}) : MakeBackend();
        if (!backend) return;
        BreachCapture breaches;
        const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 8, 8);  // owned by this thread
        Recorder rec = backend->BeginRecording();
        TargetPassDesc pass;
        pass.color = image.get();
        pass.color_load = LoadOp::Clear;
        rec.BeginTargetPass(pass);
        rec.SetPipeline(*red);
        rec.Draw(3);
        const TimelineValue produced = backend->Submit(std::move(rec));  // the pass is open at Submit
        const std::vector<u8> red_pixels = Repeat({0xFF, 0x00, 0x00, 0xFF}, 64);

        // the owner reads it: no breach
        CHECK(Readback(*backend, *image, 8, 8, PicaFormat::Rgba8, 4) == red_pixels);
        CHECK(breaches.Total() == 0);

        // another thread that was not handed the image: one breach for each call, and the call still returns
        std::thread stranger([&] {
            std::vector<u8> bytes(8 * 8 * 4);
            CHECK(backend->ReadbackToGuest(*image, Rect{0, 0, 8, 8}, PicaFormat::Rgba8, bytes).ok());
            CHECK(backend->ReadImage(*image, 0, 0, 8, 8).ok());
        });
        stranger.join();
        CHECK(breaches.Count(OwnerRule::ForeignThread) == 2);
        CHECK(breaches.Total() == 2);

        // handed over by the value that produced it: the receiver reads it without a breach, and the readback carries the wait itself
        std::promise<std::thread::id> id;
        std::promise<void> handed;
        std::vector<u8> from_receiver;
        std::vector<u8> image_from_receiver;
        std::thread receiver([&] {
            id.set_value(std::this_thread::get_id());
            handed.get_future().wait();
            from_receiver.assign(8 * 8 * 4, 0);
            CHECK(backend->ReadbackToGuest(*image, Rect{0, 0, 8, 8}, PicaFormat::Rgba8, from_receiver).ok());
            Result<std::vector<u8>> pixels = backend->ReadImage(*image, 0, 0, 8, 8);
            CHECK(pixels.ok());
            if (pixels.ok()) image_from_receiver = *pixels;
        });
        HandOver(*image, id.get_future().get(), Wait{produced, Stage::FragmentShader});
        handed.set_value();
        receiver.join();
        CHECK(from_receiver == red_pixels);
        CHECK(image_from_receiver == Repeat({0xFF, 0x00, 0x00, 0xFF}, 64));  // R, G, B, A
        CHECK(breaches.Total() == 2);  // no new breach: not a ForeignThread one, and not a RequiredWait one
        CHECK(breaches.Count(OwnerRule::RequiredWait) == 0);
    }
}

// ---- timestamps ------------------------------------------------------------------------------------------------------

TEST_CASE("timestamps/a_frame_with_a_clear_and_a_readback_has_a_time_and_the_next_frame_follows_it") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 16, 16);
    CHECK(backend->FrameGpuTimeNs() == 0);  // nothing has been timed
    sync::FrameTimestamps previous;
    for (int frame = 0; frame < 3; ++frame) {
        // The frame spans two recorders: Begin and the clear in the first, the readback between them (its own internal
        // submission, inside the frame), the End in a recorder submitted after the readback.
        Recorder rec = backend->BeginRecording();
        rec.MarkFrame(FrameMark::Begin);
        rec.ClearImage(*image, Color(1, 0, 0, 1));
        const TimelineValue first = backend->Submit(std::move(rec));
        CHECK(Readback(*backend, *image, 16, 16, PicaFormat::Rgba8, 4) == Repeat({0xFF, 0x00, 0x00, 0xFF}, 256));
        Recorder end = backend->BeginRecording();
        end.MarkFrame(FrameMark::End);
        const TimelineValue last = backend->Submit(std::move(end));
        REQUIRE(last > first + 1);  // the readback's submission is between the two
        backend->WaitIdle(last);
        const u64 ns = backend->FrameGpuTimeNs();
        CHECK(ns > 0);
        const sync::FrameTimestamps stamps = sync::LastFrameTimestamps(*backend);
        // increasing values: End after Begin, and each frame after the one before
        CHECK(stamps.end_ns > stamps.begin_ns);
        CHECK(stamps.end_ns - stamps.begin_ns == ns);
        CHECK(stamps.begin_ns >= previous.end_ns);
        previous = stamps;
    }
}

TEST_CASE("readback/two_threads_reading_back_different_images_at_once_get_their_own_bytes") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    // One readback runs at a time per backend (a mutex around the pass, the buffers and the binding set): two threads, one reading
    // a colour image and one a depth image (different kinds of pass, the same output buffer), 25 times each. ThreadSanitizer covers the race.
    constexpr int kRounds = 25;
    std::atomic<int> colour_right{0}, depth_right{0};
    std::thread colour([&] {
        ImageDesc desc;
        desc.format = Format::Rgba8Unorm;
        desc.width = desc.height = 16;
        Result<std::shared_ptr<Image>> image = backend->CreateImage(desc);
        if (!image.ok()) return;
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(**image, Color(1.0f, 0.0f, 0.2f, 0.6f));
        backend->WaitIdle(backend->Submit(std::move(rec)));
        const std::vector<u8> expected = Repeat({0x99, 0x33, 0x00, 0xFF}, 256);
        for (int i = 0; i < kRounds; ++i) {
            std::vector<u8> bytes(16 * 16 * 4);
            if (backend->ReadbackToGuest(**image, Rect{0, 0, 16, 16}, PicaFormat::Rgba8, bytes).ok() && bytes == expected) {
                colour_right.fetch_add(1);
            }
        }
    });
    std::thread depth([&] {
        ImageDesc desc;
        desc.format = Format::D24S8;
        desc.width = desc.height = 16;
        Result<std::shared_ptr<Image>> image = backend->CreateImage(desc);
        if (!image.ok()) return;
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(**image, Depth(1.0f, 0x7E));
        backend->WaitIdle(backend->Submit(std::move(rec)));
        const std::vector<u8> expected = Repeat({0xFF, 0xFF, 0xFF, 0x7E}, 256);
        for (int i = 0; i < kRounds; ++i) {
            std::vector<u8> bytes(16 * 16 * 4);
            if (backend->ReadbackToGuest(**image, Rect{0, 0, 16, 16}, PicaFormat::D24S8, bytes).ok() && bytes == expected) {
                depth_right.fetch_add(1);
            }
        }
    });
    colour.join();
    depth.join();
    CHECK(colour_right.load() == kRounds);
    CHECK(depth_right.load() == kRounds);
}
