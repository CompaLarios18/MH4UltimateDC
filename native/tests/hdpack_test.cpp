// Tests for the HD pack pre-processor (N2-17): the pack rules, the mip chains, the BC7 encoder
// (checked with a decoder written here from the BC7 specification), the archive and the
// resumable conversion. Every texture is made in the test; no real pack is read.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <doctest/doctest.h>
#include <lodepng.h>

#include "gpu/texture/hash.h"
#include "hdpack/archive.h"
#include "hdpack/bc7.h"
#include "hdpack/convert.h"
#include "hdpack/image.h"
#include "hdpack/json.h"
#include "hdpack/mips.h"
#include "hdpack/pack.h"
#include "hdpack/tool.h"
#include "mh4n/crc32.h"

using namespace mh4n;
using namespace mh4n::hdpack;
namespace fs = std::filesystem;

namespace {

// ---- a BC7 decoder for modes 1 and 6, from the BC7 specification ------------------------------
// (the layout of the Khronos Data Format Specification / Microsoft's BC7 format description)
// Mode 6: 7 mode bits (0000001 read LSB first: six zeros then a one), R0 R1 G0 G1 B0 B1 A0 A1 at
//   7 bits each, a p-bit for each endpoint, 16 4-bit indices (the first has 3 bits). 128 bits.
// Mode 1: 2 mode bits (0 then 1), a 6-bit partition number, R0 R1 R2 R3 G0..G3 B0..B3 at 6 bits,
//   a p-bit for each of the two subsets (shared by both its endpoints), 16 3-bit indices (the
//   first, and the one at the anchor of subset 1, have 2 bits). 128 bits.
// Partition tables of the two-subset shapes are those of the specification (a fact table, taken
// from the public-domain bc7decomp.cpp of bc7enc_rdo).
constexpr unsigned char kPartition2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1},
    {0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},
    {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},
    {0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1},
    {0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},
    {0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1},
    {0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},
    {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1},
    {0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0},
    {0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0},
    {0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0},
    {0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0},
    {0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0},
    {0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},
    {0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},
    {0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0},
    {0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},
    {0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1},
    {0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0},
    {0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},
    {0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0},
    {0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0},
    {0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},
    {0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1},
    {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0},
    {0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},
    {0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},
    {0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1},
    {0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0},
    {0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},
    {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1},
    {0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1},
    {0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0},
    {0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1},
};
constexpr unsigned char kAnchor2[64] = {15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15};

constexpr int kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Pixel {
    int r, g, b, a;
};

class Bits {
public:
    explicit Bits(const u8* block) : block_(block) {}
    unsigned Read(int count) {
        unsigned value = 0;
        for (int i = 0; i < count; ++i, ++pos_) {
            value |= static_cast<unsigned>((block_[pos_ / 8] >> (pos_ % 8)) & 1) << i;
        }
        return value;
    }
    int position() const {
        return pos_;
    }

private:
    const u8* block_;
    int pos_ = 0;
};

int Expand(int value, int bits) { // an endpoint with its p-bit appended -> 8 bits
    return (value << (8 - bits)) | (value >> (2 * bits - 8));
}

int Lerp(int a, int b, int w) {
    return (a * (64 - w) + b * w + 32) >> 6;
}

// Returns the mode (1 or 6), or -1 for anything else.
int DecodeBlock(const u8* block, Pixel out[16]) {
    Bits bits(block);
    int mode = 0;
    while (mode < 8 && bits.Read(1) == 0) {
        ++mode;
    }
    if (mode == 6) {
        int ep[2][4];
        for (int c = 0; c < 4; ++c) {
            for (int e = 0; e < 2; ++e) {
                ep[e][c] = static_cast<int>(bits.Read(7));
            }
        }
        // order above is R0 R1 G0 G1 B0 B1 A0 A1: channel-major, endpoint-minor
        const int p0 = static_cast<int>(bits.Read(1));
        const int p1 = static_cast<int>(bits.Read(1));
        for (int c = 0; c < 4; ++c) {
            ep[0][c] = (ep[0][c] << 1) | p0;
            ep[1][c] = (ep[1][c] << 1) | p1;
        }
        for (int i = 0; i < 16; ++i) {
            const int w = kWeights4[bits.Read(i == 0 ? 3 : 4)];
            out[i] = {Lerp(ep[0][0], ep[1][0], w), Lerp(ep[0][1], ep[1][1], w),
                      Lerp(ep[0][2], ep[1][2], w), Lerp(ep[0][3], ep[1][3], w)};
        }
        return bits.position() == 128 ? 6 : -1;
    }
    if (mode == 1) {
        const int partition = static_cast<int>(bits.Read(6));
        int ep[4][3]; // [endpoint][channel]
        for (int c = 0; c < 3; ++c) {
            for (int e = 0; e < 4; ++e) {
                ep[e][c] = static_cast<int>(bits.Read(6));
            }
        }
        const int p[2] = {static_cast<int>(bits.Read(1)), static_cast<int>(bits.Read(1))};
        for (int e = 0; e < 4; ++e) {
            for (int c = 0; c < 3; ++c) {
                const int v = (ep[e][c] << 1) | p[e / 2]; // 7 bits
                ep[e][c] = Expand(v, 7);
            }
        }
        for (int i = 0; i < 16; ++i) {
            const int subset = kPartition2[partition][i];
            const bool anchor = (i == 0) || (subset == 1 && i == kAnchor2[partition]);
            const int w = kWeights3[bits.Read(anchor ? 2 : 3)];
            out[i] = {Lerp(ep[subset * 2][0], ep[subset * 2 + 1][0], w),
                      Lerp(ep[subset * 2][1], ep[subset * 2 + 1][1], w),
                      Lerp(ep[subset * 2][2], ep[subset * 2 + 1][2], w), 255};
        }
        return bits.position() == 128 ? 1 : -1;
    }
    return -1;
}

struct ErrorStats {
    int max_error = 0;
    double mean_error = 0;
    int modes[8] = {};
};

// Decodes `blocks` (row-major, ceil(w/4) x ceil(h/4)) and compares with `rgba`.
ErrorStats Compare(const std::vector<u8>& blocks, const std::vector<u8>& rgba, u32 w, u32 h) {
    ErrorStats stats;
    const u32 bw = (w + 3) / 4;
    double sum = 0;
    u64 count = 0;
    for (u32 by = 0; by < (h + 3) / 4; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            Pixel px[16];
            const int mode = DecodeBlock(&blocks[(by * bw + bx) * 16], px);
            REQUIRE_MESSAGE(mode > 0, "block (" << bx << "," << by << ") is not a mode 1 or 6 block");
            ++stats.modes[mode];
            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    const u32 sx = bx * 4 + x;
                    const u32 sy = by * 4 + y;
                    if (sx >= w || sy >= h) {
                        continue;
                    }
                    const u8* src = &rgba[(static_cast<std::size_t>(sy) * w + sx) * 4];
                    const Pixel& d = px[y * 4 + x];
                    const int e[4] = {std::abs(d.r - src[0]), std::abs(d.g - src[1]),
                                      std::abs(d.b - src[2]), std::abs(d.a - src[3])};
                    for (int v : e) {
                        stats.max_error = std::max(stats.max_error, v);
                        sum += v;
                        ++count;
                    }
                }
            }
        }
    }
    stats.mean_error = count ? sum / static_cast<double>(count) : 0;
    return stats;
}

// ---- synthetic textures and packs -------------------------------------------------------------

// A diagonal ramp: every channel is a linear function of x + y, so the colours of any 4x4 block
// lie on one line, which is what a one-subset BC7 block can represent closely.
std::vector<u8> Gradient(u32 w, u32 h, bool alpha_ramp) {
    std::vector<u8> px(static_cast<std::size_t>(w) * h * 4);
    const u32 span = std::max(1u, w + h - 2);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            const u32 t = (x + y) * 255 / span;
            p[0] = static_cast<u8>(t);
            p[1] = static_cast<u8>(255 - t * 3 / 4);
            p[2] = static_cast<u8>(40 + t / 2);
            p[3] = alpha_ramp ? static_cast<u8>(255 - t * 200 / 255) : 255;
        }
    }
    return px;
}

// Top half and bottom half of two flat colours (each 4x4 block is flat when h is a multiple of 8).
std::vector<u8> TwoBands(u32 w, u32 h) {
    std::vector<u8> px(static_cast<std::size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            const bool top = y < h / 2;
            p[0] = top ? 50 : 220;
            p[1] = top ? 100 : 30;
            p[2] = top ? 200 : 90;
            p[3] = 255;
        }
    }
    return px;
}

std::vector<u8> PngBytes(const std::vector<u8>& rgba, u32 w, u32 h) {
    std::vector<unsigned char> out;
    REQUIRE(lodepng::encode(out, rgba.data(), w, h, LCT_RGBA, 8) == 0);
    return out;
}

class Scratch {
public:
    Scratch() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / ("mh4n_hdpack_test_" + std::to_string(stamp) + "_" +
                                              std::to_string(counter++));
        fs::create_directories(root_ / "pack");
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    // The pack folder (files written with Write() and named by path() are inside it).
    std::string path(const std::string& relative = {}) const {
        return relative.empty() ? (root_ / "pack").string() : (root_ / "pack" / relative).string();
    }
    // A file next to the pack folder, for outputs.
    std::string out(const std::string& name) const {
        return (root_ / name).string();
    }
    void Write(const std::string& relative, const void* data, std::size_t size) const {
        const fs::path p = root_ / "pack" / relative;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    void Write(const std::string& relative, const std::string& text) const {
        Write(relative, text.data(), text.size());
    }
    void Write(const std::string& relative, const std::vector<u8>& bytes) const {
        Write(relative, bytes.data(), bytes.size());
    }

private:
    fs::path root_;
};

std::string StemOf(u32 w, u32 h, u64 hash) {
    return gpu::texture::CustomTextureStem(w, h, hash, gpu::texture::Format::RGBA8);
}

constexpr const char* kPackJsonNew =
    R"({"author":"t","options":{"skip_mipmap":false,"flip_png_files":false,"use_new_hash":true}})";

std::vector<u8> ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A little-endian DDS header (+ DX10 header) for the tests.
std::vector<u8> DdsFile(u32 w, u32 h, u32 mips, u32 dxgi, const std::vector<u8>& payload) {
    std::vector<u8> f(4 + 124 + 20, 0);
    auto put = [&](std::size_t at, u32 v) {
        for (int i = 0; i < 4; ++i) {
            f[at + i] = static_cast<u8>(v >> (8 * i));
        }
    };
    std::memcpy(f.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);
    put(12, h);
    put(16, w);
    put(28, mips);
    put(4 + 72, 32);
    put(4 + 76, 0x4);
    put(4 + 80, 0x30315844); // "DX10"
    put(4 + 104, 0x1000 | 0x8 | 0x400000); // caps
    put(4 + 124, dxgi);
    put(4 + 124 + 4, 3);  // 2D
    put(4 + 124 + 12, 1); // array size
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

} // namespace

// ---- the pack rules ----------------------------------------------------------------------------

TEST_CASE("file names: the stem is the inverse of CustomTextureStem") {
    for (const u64 hash : {u64{0}, u64{0xAB}, u64{0xDEADBEEFCAFE1234ull}, ~u64{0}}) {
        const std::string stem = StemOf(256, 128, hash);
        const std::optional<TextureStem> parsed = ParseTextureStem(stem);
        REQUIRE(parsed.has_value());
        CHECK(parsed->width == 256);
        CHECK(parsed->height == 128);
        CHECK(parsed->hash == hash);
        CHECK(parsed->format == 0);
        // trailing text such as _mip0 is ignored, as Azahar's sscanf does
        CHECK(ParseTextureStem(stem + "_mip0")->hash == hash);
    }
    CHECK_FALSE(ParseTextureStem("tex2_4x4_00_0").has_value());
    CHECK_FALSE(ParseTextureStem("tex1_4x4_zz_0").has_value());
    CHECK_FALSE(ParseTextureStem("tex1_4x4_AB").has_value());
    CHECK_FALSE(ParseTextureStem("hello").has_value());
    CHECK(ParseTextureStem("tex1_4x4_ab_5")->hash == 0xAB); // %llX takes lower case
}

TEST_CASE("pack.json: options are required, hashes come from the textures map") {
    std::string error;
    auto pack = ParsePackJson(
        R"({"options":{"skip_mipmap":true,"flip_png_files":false,"use_new_hash":true},
            // a comment, as Azahar's parser ignores them
            "textures":{"1F":"a/b.png","0x20":["c.png","d.png"],"zz":"e.png","30":7}})",
        &error);
    REQUIRE_MESSAGE(pack.has_value(), error);
    CHECK(pack->options.has_pack_json);
    CHECK(pack->options.skip_mipmap);
    CHECK_FALSE(pack->options.flip_png_files);
    CHECK(pack->options.use_new_hash);
    CHECK(pack->hashes_by_file.at("b.png") == std::vector<u64>{0x1F}); // directories are dropped
    CHECK(pack->hashes_by_file.at("c.png") == std::vector<u64>{0x20});
    CHECK(pack->hashes_by_file.count("e.png") == 0);

    CHECK_FALSE(ParsePackJson("{", &error).has_value());
    CHECK_FALSE(ParsePackJson(R"({"textures":{}})", &error).has_value());
    CHECK_FALSE(ParsePackJson(R"({"options":{"skip_mipmap":true}})", &error).has_value());
    CHECK(error.find("flip_png_files") != std::string::npos);
    CHECK_FALSE(ParsePackJson(R"({"options":{"skip_mipmap":1,"flip_png_files":true,"use_new_hash":true}})",
                              &error)
                    .has_value());
}

TEST_CASE("scan: Azahar's file-name rules and legacy defaults") {
    Scratch dir;
    const std::vector<u8> png = PngBytes(Gradient(4, 4, false), 4, 4);
    dir.Write("a/" + StemOf(4, 4, 0x11) + ".png", png);
    dir.Write("a/" + StemOf(4, 4, 0x11) + ".norm.png", png);
    dir.Write("b/" + StemOf(4, 4, 0x12) + "_mip0.png", png);
    dir.Write("c/custom_name.png", png);              // needs the pack.json map
    dir.Write("c/" + StemOf(4, 4, 0x13) + ".a.b.png", png); // four parts: ignored
    dir.Write("c/readme.txt", "hi");
    dir.Write("c/" + StemOf(4, 4, 0x14) + ".dds", png);

    // No pack.json: old hash, skipped mipmaps (so the DDS is ignored), flip stays true.
    std::string error;
    auto pack = ScanPack(dir.path(), &error);
    REQUIRE_MESSAGE(pack.has_value(), error);
    CHECK_FALSE(pack->options.has_pack_json);
    CHECK_FALSE(pack->options.use_new_hash);
    CHECK(pack->options.skip_mipmap);
    CHECK(pack->options.flip_png_files);
    CHECK(pack->stats.ignored_dds == 1);
    CHECK(pack->stats.ignored_names == 2); // the four-part name and readme.txt
    CHECK(pack->stats.files_without_hash == 1); // custom_name.png
    CHECK(pack->materials.size() == 2);
    CHECK(pack->materials.at(0x11).file[0] >= 0);
    CHECK(pack->materials.at(0x11).file[1] >= 0);
    CHECK(pack->materials.at(0x12).file[1] == -1);

    // With a pack.json: the map names custom_name.png and the DDS counts.
    dir.Write("pack.json",
              R"({"options":{"skip_mipmap":false,"flip_png_files":true,"use_new_hash":true},
                  "textures":{"77":"custom_name.png","78":["custom_name.png"]}})");
    pack = ScanPack(dir.path(), &error);
    REQUIRE_MESSAGE(pack.has_value(), error);
    CHECK(pack->options.use_new_hash);
    CHECK(pack->stats.ignored_dds == 0);
    CHECK(pack->materials.count(0x77) == 1);
    CHECK(pack->materials.count(0x78) == 1);
    CHECK(pack->materials.count(0x14) == 1);

    CHECK_FALSE(ScanPack(dir.path("nowhere"), &error).has_value());
}

TEST_CASE("scan: a second file for the same map of one hash is ignored") {
    Scratch dir;
    const std::vector<u8> png = PngBytes(Gradient(4, 4, false), 4, 4);
    dir.Write(StemOf(4, 4, 0x21) + ".png", png);
    dir.Write("x/" + StemOf(4, 4, 0x21) + ".png", png);
    std::string error;
    auto pack = ScanPack(dir.path(), &error);
    REQUIRE(pack.has_value());
    CHECK(pack->stats.duplicate_maps == 1);
    CHECK(pack->materials.at(0x21).file[0] == 0); // the first in path order
}

// ---- mip chains --------------------------------------------------------------------------------

TEST_CASE("mip chain: sizes, means and alpha weighting") {
    const std::vector<u8> src = {10, 20, 30, 255, 30, 40, 50, 255, 50, 60, 70, 255, 70, 80, 90, 255};
    auto chain = BuildMipChain(src.data(), 2, 2, false);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0] == src);
    CHECK(chain[1] == std::vector<u8>{40, 50, 60, 255});

    // transparent pixels do not bleed their colour into the level below when weighting by alpha
    const std::vector<u8> sprite = {200, 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0};
    CHECK(BuildMipChain(sprite.data(), 2, 2, true)[1] == std::vector<u8>{200, 0, 0, 64});
    CHECK(BuildMipChain(sprite.data(), 2, 2, false)[1][1] > 100);

    const auto odd = BuildMipChain(Gradient(13, 7, false).data(), 13, 7, false);
    REQUIRE(odd.size() == FullMipCount(13, 7));
    CHECK(odd.size() == 4);
    CHECK(odd.back().size() == 4);
    CHECK(odd[1].size() == 6 * 3 * 4);
    CHECK(FullMipCount(1, 1) == 1);
    CHECK(FullMipCount(64, 32) == 7);
}

// ---- the BC7 encoder, against the decoder above ------------------------------------------------

TEST_CASE("BC7: flat blocks decode exactly, smooth content within 3/255") {
    for (const char* quality_name : {"fast", "normal", "high"}) {
        CAPTURE(quality_name);
        Bc7Quality quality;
        REQUIRE(ParseBc7Quality(quality_name, quality));

        const u32 w = 16;
        const u32 h = 16;
        const std::vector<u8> flat = TwoBands(w, h);
        std::vector<u8> blocks(LevelBytes(PixelFormat::BC7, w, h));
        EncodeBc7(flat.data(), w, h, quality, blocks.data());
        ErrorStats stats = Compare(blocks, flat, w, h);
        CHECK(stats.max_error <= 1);

        const std::vector<u8> grad = Gradient(w, h, true);
        EncodeBc7(grad.data(), w, h, quality, blocks.data());
        stats = Compare(blocks, grad, w, h);
        CHECK_MESSAGE(stats.max_error <= 3, "max error " << stats.max_error);
        CHECK(stats.mean_error <= 1.0);
        CHECK(stats.modes[6] > 0); // alpha < 255 forces mode 6
    }
}

TEST_CASE("BC7: sizes that are not a multiple of 4 repeat the edge pixels") {
    Bc7Quality quality;
    REQUIRE(ParseBc7Quality("normal", quality));
    for (const auto& size : {std::pair<u32, u32>{13, 7}, {1, 1}, {2, 9}, {5, 3}}) {
        const u32 w = size.first;
        const u32 h = size.second;
        CAPTURE(w);
        CAPTURE(h);
        const std::vector<u8> grad = Gradient(w, h, false);
        std::vector<u8> blocks(LevelBytes(PixelFormat::BC7, w, h));
        CHECK(blocks.size() == ((w + 3) / 4) * ((h + 3) / 4) * 16);
        EncodeBc7(grad.data(), w, h, quality, blocks.data());
        const ErrorStats stats = Compare(blocks, grad, w, h);
        CHECK_MESSAGE(stats.max_error <= 4, "max error " << stats.max_error);
    }
}

TEST_CASE("BC7: two-colour blocks use mode 1 or 6 and stay within 4/255") {
    // Each 4x4 block holds two colours split along a diagonal: a two-subset shape.
    const u32 w = 32;
    const u32 h = 32;
    std::vector<u8> px(static_cast<std::size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const bool a = ((x % 4) + (y % 4)) < 4;
            const u32 block = (y / 4) * 8 + (x / 4);
            u8* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<u8>(a ? 20 + block : 230 - block);
            p[1] = static_cast<u8>(a ? 200 - block : 40 + block);
            p[2] = static_cast<u8>(a ? 90 : 160);
            p[3] = 255;
        }
    }
    Bc7Quality quality;
    REQUIRE(ParseBc7Quality("high", quality));
    std::vector<u8> blocks(LevelBytes(PixelFormat::BC7, w, h));
    EncodeBc7(px.data(), w, h, quality, blocks.data());
    const ErrorStats stats = Compare(blocks, px, w, h);
    CHECK_MESSAGE(stats.max_error <= 4, "max error " << stats.max_error);
    CHECK(stats.modes[1] + stats.modes[6] == 64);
    CHECK(ParseBc7Quality("fast", quality));
    CHECK_FALSE(ParseBc7Quality("best", quality));
}

TEST_CASE("PNG and DDS decoding") {
    const std::vector<u8> grad = Gradient(8, 4, true);
    Image image;
    std::string error;
    const std::vector<u8> png = PngBytes(grad, 8, 4);
    REQUIRE(DecodePng(png.data(), png.size(), false, image, error));
    CHECK(image.width == 8);
    CHECK(image.height == 4);
    CHECK(image.levels[0] == grad);
    REQUIRE(DecodePng(png.data(), png.size(), true, image, error));
    CHECK(std::equal(image.levels[0].begin(), image.levels[0].begin() + 32, grad.begin() + 96));
    CHECK_FALSE(DecodePng(png.data(), 20, false, image, error));
    CHECK_FALSE(DecodeDds(png.data(), png.size(), image, error));

    // DDS: RGBA8 through DXGI 28, and a BC7 payload kept as it is with its two levels
    const std::vector<u8> rgba = Gradient(8, 4, false);
    std::vector<u8> dds = DdsFile(8, 4, 1, 28, rgba);
    REQUIRE_MESSAGE(DecodeDds(dds.data(), dds.size(), image, error), error);
    CHECK(image.format == PixelFormat::RGBA8);
    CHECK(image.levels[0] == rgba);

    std::vector<u8> payload(LevelBytes(PixelFormat::BC7, 8, 4) + LevelBytes(PixelFormat::BC7, 4, 2) +
                            LevelBytes(PixelFormat::BC7, 2, 1) + LevelBytes(PixelFormat::BC7, 1, 1));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<u8>(i * 7);
    }
    dds = DdsFile(8, 4, 4, 98, payload);
    REQUIRE_MESSAGE(DecodeDds(dds.data(), dds.size(), image, error), error);
    CHECK(image.format == PixelFormat::BC7);
    CHECK(image.levels.size() == 4);
    CHECK(image.levels[0].size() == 32);
    dds.pop_back();
    CHECK_FALSE(DecodeDds(dds.data(), dds.size(), image, error));
}

// ---- the archive ------------------------------------------------------------------------------

TEST_CASE("archive: round trip, lookup and damage") {
    Scratch dir;
    const std::string path = dir.out("a.bin");
    ArchiveWriter writer;
    std::string error;
    REQUIRE(writer.Begin(path, true, 0, error));
    const std::vector<u8> blob(16 + 16, 0x5A); // a 4x4 and a 1x1... as BC7: 16 + 16 bytes
    const auto offset = writer.Append(blob.data(), blob.size(), error);
    REQUIRE(offset.has_value());
    CHECK(*offset == kArchiveHeaderSize);

    // Until Finish the file is not an archive.
    ArchiveReader reader;
    CHECK_FALSE(reader.Open(path, error));

    ArchiveEntry a;
    a.hash = 0x99;
    a.offset = *offset;
    a.size = blob.size();
    a.width = 4;
    a.height = 2;
    a.mip_count = 2;
    a.crc = Crc32(blob.data(), blob.size());
    ArchiveEntry b = a; // the same blob under another hash and as a normal map
    b.hash = 0x05;
    ArchiveEntry c = a;
    c.type = MapType::Normal;
    REQUIRE(writer.Finish({a, b, c}, error));
    REQUIRE_MESSAGE(reader.Open(path, error), error);
    CHECK(reader.use_new_hash());
    REQUIRE(reader.entries().size() == 3);
    CHECK(reader.entries()[0].hash == 0x05); // sorted by hash
    CHECK(reader.Find(0x99) != nullptr);
    CHECK(reader.Find(0x99, MapType::Normal) != nullptr);
    CHECK(reader.Find(0x05, MapType::Normal) == nullptr);
    CHECK(reader.Find(0x06) == nullptr);
    CHECK(reader.Verify(*reader.Find(0x99), error));
    std::vector<u8> level;
    REQUIRE(reader.ReadLevel(*reader.Find(0x99), 1, level, error));
    CHECK(level.size() == 16);
    CHECK_FALSE(reader.ReadLevel(*reader.Find(0x99), 2, level, error));

    // damage: a flipped byte in the index, a cut file
    std::vector<u8> bytes = ReadAll(path);
    std::vector<u8> bad = bytes;
    bad[bad.size() - 5] ^= 0xFF;
    { std::ofstream f(dir.out("bad.bin"), std::ios::binary); f.write(reinterpret_cast<const char*>(bad.data()), static_cast<std::streamsize>(bad.size())); }
    CHECK_FALSE(reader.Open(dir.out("bad.bin"), error));
    // Entries that are wrong but carry a correct index CRC (Finish writes whatever it is given).
    auto refused = [&](const char* name, auto&& change) {
        ArchiveWriter w;
        std::string err;
        REQUIRE(w.Begin(dir.out(name), true, 0, err));
        const auto at = w.Append(blob.data(), blob.size(), err);
        REQUIRE(at.has_value());
        ArchiveEntry x = a;
        x.offset = *at;
        change(x);
        REQUIRE(w.Finish({x}, err));
        ArchiveReader r;
        CHECK_FALSE(r.Open(dir.out(name), err));
    };
    refused("mip40.bin", [](ArchiveEntry& x) { x.mip_count = 40; });
    refused("mip255.bin", [](ArchiveEntry& x) { x.mip_count = 255; });
    refused("offbig.bin", [](ArchiveEntry& x) { x.offset = ~u64{0} - 8; });
    refused("sizebig.bin", [](ArchiveEntry& x) { x.size = ~u64{0} - 8; });
    refused("w0.bin", [](ArchiveEntry& x) { x.width = 0; });
    refused("hbig.bin", [](ArchiveEntry& x) { x.height = kMaxDimension + 1; });
    refused("mip3.bin", [](ArchiveEntry& x) { x.mip_count = 4; }); // 4x2 has only 3 levels
    bytes.resize(bytes.size() - 1);
    { std::ofstream f(dir.out("cut.bin"), std::ios::binary); f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }
    CHECK_FALSE(reader.Open(dir.out("cut.bin"), error));
}

// ---- the conversion ----------------------------------------------------------------------------

namespace {

struct Source {
    std::string stem;
    u32 w, h;
    std::vector<u8> rgba;
};

// Builds a pack of PNGs with a pack.json, returns the sources in the order of their hashes.
std::vector<Source> MakePack(const Scratch& dir, const char* json, bool flip) {
    std::vector<Source> sources;
    auto add = [&](u64 hash, u32 w, u32 h, std::vector<u8> rgba, const std::string& suffix = ".png") {
        Source s{StemOf(w, h, hash), w, h, std::move(rgba)};
        std::vector<u8> shown = s.rgba;
        dir.Write("tex/" + s.stem + suffix, PngBytes(shown, w, h));
        if (flip) {
            FlipRows(s.rgba.data(), w, h);
        }
        sources.push_back(std::move(s));
    };
    add(0x01, 16, 16, Gradient(16, 16, false));
    add(0x02, 13, 7, Gradient(13, 7, true));
    add(0x03, 64, 32, TwoBands(64, 32));
    add(0x04, 1, 1, Gradient(1, 1, false));
    dir.Write("pack.json", json);
    return sources;
}

ConvertOptions BaseOptions(const Scratch& dir, const std::string& out) {
    ConvertOptions options;
    options.pack_dir = dir.path();
    options.out_path = dir.out(out);
    options.threads = 2;
    REQUIRE(ParseBc7Quality("normal", options.quality));
    return options;
}

} // namespace

TEST_CASE("convert: synthetic PNGs become an archive whose BC7 blocks decode back") {
    Scratch dir;
    const std::vector<Source> sources = MakePack(dir, kPackJsonNew, false);
    ConvertOptions options = BaseOptions(dir, "out.bin");
    const ConvertResult result = Convert(options);
    REQUIRE_MESSAGE(result.status == ConvertResult::Status::Ok, result.error);
    CHECK(result.progress.done == 4);
    CHECK(result.progress.failed == 0);
    CHECK(result.textures == 4);
    CHECK(result.dropped_materials == 0);
    CHECK_FALSE(fs::exists(options.out_path + ".part"));
    CHECK_FALSE(fs::exists(options.out_path + ".journal"));

    ArchiveReader reader;
    std::string error;
    REQUIRE_MESSAGE(reader.Open(options.out_path, error), error);
    CHECK(reader.use_new_hash()); // from the pack.json
    CHECK(reader.file_size() == result.archive_bytes);
    REQUIRE(reader.entries().size() == 4);

    u64 hash = 1;
    for (const Source& source : sources) {
        CAPTURE(source.stem);
        const ArchiveEntry* entry = reader.Find(hash++);
        REQUIRE(entry != nullptr);
        CHECK(entry->width == source.w);
        CHECK(entry->height == source.h);
        CHECK(entry->format == PixelFormat::BC7);
        CHECK(entry->mip_count == FullMipCount(source.w, source.h));
        CHECK(reader.Verify(*entry, error));

        const auto chain = BuildMipChain(source.rgba.data(), source.w, source.h, true);
        REQUIRE(chain.size() == entry->mip_count);
        u32 lw = source.w;
        u32 lh = source.h;
        for (u32 level = 0; level < entry->mip_count; ++level) {
            std::vector<u8> blocks;
            REQUIRE(reader.ReadLevel(*entry, level, blocks, error));
            REQUIRE(blocks.size() == LevelBytes(PixelFormat::BC7, lw, lh));
            const ErrorStats stats = Compare(blocks, chain[level], lw, lh);
            // The error bound of one block is half an index step: a block that holds the whole
            // 0..255 ramp (the small mip levels) has steps of 255/15 = 17 with 4-bit indices, so
            // up to 8.5; the large levels have much smaller ranges per block and stay within 4.
            const int bound = level >= 2 ? 8 : 4;
            CHECK_MESSAGE(stats.max_error <= bound,
                          "level " << level << " max error " << stats.max_error);
            lw = std::max(1u, lw >> 1);
            lh = std::max(1u, lh >> 1);
        }
    }
}

TEST_CASE("convert: flip_png_files, the old-hash flag, normal maps and one file for two hashes") {
    Scratch dir;
    const char* json = R"({"options":{"skip_mipmap":false,"flip_png_files":true,"use_new_hash":false},
                           "textures":{"0A":"shared.png","0B":"shared.png"}})";
    const std::vector<Source> sources = MakePack(dir, json, true);
    const std::vector<u8> shared = TwoBands(16, 16);
    dir.Write("tex/shared.png", PngBytes(shared, 16, 16));
    dir.Write("tex/" + StemOf(16, 16, 0x01) + ".norm.png", PngBytes(TwoBands(16, 16), 16, 16));
    // a normal map of another size, and a normal map with no colour map: both drop their hash
    dir.Write("tex/" + StemOf(16, 16, 0x02) + ".norm.png", PngBytes(TwoBands(16, 16), 16, 16));
    dir.Write("tex/" + StemOf(8, 8, 0x30) + ".norm.png", PngBytes(TwoBands(8, 8), 8, 8));

    ConvertOptions options = BaseOptions(dir, "out2.bin");
    const ConvertResult result = Convert(options);
    REQUIRE_MESSAGE(result.status == ConvertResult::Status::Ok, result.error);
    CHECK(result.dropped_materials == 2); // 0x02 (size differs) and 0x30 (no colour map)

    ArchiveReader reader;
    std::string error;
    REQUIRE_MESSAGE(reader.Open(options.out_path, error), error);
    CHECK_FALSE(reader.use_new_hash());
    CHECK(reader.Find(0x02) == nullptr);
    CHECK(reader.Find(0x30, MapType::Normal) == nullptr);
    REQUIRE(reader.Find(0x01) != nullptr);
    REQUIRE(reader.Find(0x01, MapType::Normal) != nullptr);
    const ArchiveEntry* a = reader.Find(0x0A);
    const ArchiveEntry* b = reader.Find(0x0B);
    REQUIRE((a != nullptr && b != nullptr));
    CHECK(a->offset == b->offset); // converted once, indexed twice
    CHECK(reader.entries().size() == 6);

    // the pack flips: the first row of the blocks is the bottom band of the PNG
    std::vector<u8> blocks;
    REQUIRE(reader.ReadLevel(*a, 0, blocks, error));
    Pixel px[16];
    REQUIRE(DecodeBlock(blocks.data(), px) > 0);
    CHECK(std::abs(px[0].r - 220) <= 1); // the PNG's bottom band is red 220 once flipped to the top
}

TEST_CASE("convert: DDS in a pack that keeps mipmaps; unreadable files are reported") {
    Scratch dir;
    dir.Write("pack.json", kPackJsonNew);
    std::vector<u8> payload(LevelBytes(PixelFormat::BC7, 8, 8) + LevelBytes(PixelFormat::BC7, 4, 4) +
                            LevelBytes(PixelFormat::BC7, 2, 2) + LevelBytes(PixelFormat::BC7, 1, 1));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<u8>(i * 13 + 1);
    }
    dir.Write(StemOf(8, 8, 0x41) + ".dds", DdsFile(8, 8, 4, 98, payload));
    dir.Write(StemOf(8, 8, 0x42) + ".png", std::string("this is not a PNG"));
    dir.Write(StemOf(8, 8, 0x43) + ".ktx", std::string("KTX"));
    dir.Write(StemOf(8, 8, 0x44) + ".png", PngBytes(TwoBands(8, 8), 8, 8));

    ConvertOptions options = BaseOptions(dir, "out3.bin");
    const ConvertResult result = Convert(options);
    REQUIRE_MESSAGE(result.status == ConvertResult::Status::Ok, result.error);
    CHECK(result.progress.failed == 2);
    CHECK(result.failures.size() == 2);
    CHECK(result.dropped_materials == 2);
    CHECK(result.textures == 2);

    ArchiveReader reader;
    std::string error;
    REQUIRE_MESSAGE(reader.Open(options.out_path, error), error);
    const ArchiveEntry* dds = reader.Find(0x41);
    REQUIRE(dds != nullptr);
    CHECK(dds->mip_count == 4);
    std::vector<u8> blob;
    REQUIRE(reader.ReadBlob(*dds, blob, error));
    CHECK(blob == payload); // block-compressed data is kept as it is
}

TEST_CASE("convert: an interrupted run continues and gives the same archive") {
    Scratch dir;
    MakePack(dir, kPackJsonNew, false);

    ConvertOptions whole = BaseOptions(dir, "whole.bin");
    whole.threads = 1;
    REQUIRE(Convert(whole).status == ConvertResult::Status::Ok);

    // Stop after two files (the cancel poll comes before each file: the third poll says stop).
    ConvertOptions options = BaseOptions(dir, "resumed.bin");
    options.threads = 1;
    int polls = 0;
    options.cancel = [&] { return ++polls >= 3; };
    ConvertResult first = Convert(options);
    CHECK(first.status == ConvertResult::Status::Cancelled);
    CHECK(first.progress.done == 2);
    CHECK(fs::exists(options.out_path + ".part"));
    CHECK(fs::exists(options.out_path + ".journal"));
    CHECK_FALSE(fs::exists(options.out_path));

    // A crash can leave half a record in the journal and extra bytes after the last blob.
    {
        std::ofstream journal(options.out_path + ".journal", std::ios::binary | std::ios::app);
        journal.write("\x07\x00\x00\x00xx", 6);
        std::ofstream part(options.out_path + ".part", std::ios::binary | std::ios::app);
        part.write("garbage garbage", 15);
    }
    options.cancel = nullptr;
    const ConvertResult second = Convert(options);
    REQUIRE_MESSAGE(second.status == ConvertResult::Status::Ok, second.error);
    CHECK(second.progress.resumed == 2);
    CHECK(second.progress.done == 4);
    CHECK_FALSE(fs::exists(options.out_path + ".part"));
    CHECK_FALSE(fs::exists(options.out_path + ".journal"));
    CHECK(ReadAll(options.out_path) == ReadAll(whole.out_path));

    // A changed setting (here the quality) makes the old work unusable: it starts again.
    ConvertOptions third = BaseOptions(dir, "resumed.bin");
    third.threads = 1;
    polls = 0;
    third.cancel = [&] { return ++polls >= 2; };
    CHECK(Convert(third).status == ConvertResult::Status::Cancelled);
    ConvertOptions fourth = third;
    fourth.cancel = nullptr;
    REQUIRE(ParseBc7Quality("fast", fourth.quality));
    const ConvertResult changed = Convert(fourth);
    REQUIRE(changed.status == ConvertResult::Status::Ok);
    CHECK(changed.progress.resumed == 0);
}

TEST_CASE("convert: a file changed since the interrupted run is converted again") {
    Scratch dir;
    MakePack(dir, kPackJsonNew, false);
    ConvertOptions options = BaseOptions(dir, "changed.bin");
    options.threads = 1;
    int polls = 0;
    options.cancel = [&] { return ++polls >= 3; };
    CHECK(Convert(options).status == ConvertResult::Status::Cancelled);
    // replace the first texture with a bigger picture under the same name
    dir.Write("tex/" + StemOf(16, 16, 0x01) + ".png", PngBytes(TwoBands(32, 32), 32, 32));
    options.cancel = nullptr;
    const ConvertResult result = Convert(options);
    REQUIRE_MESSAGE(result.status == ConvertResult::Status::Ok, result.error);
    CHECK(result.progress.resumed == 1);
    ArchiveReader reader;
    std::string error;
    REQUIRE(reader.Open(options.out_path, error));
    CHECK(reader.Find(0x01)->width == 32);
}

TEST_CASE("the tool: arguments, progress lines and exit codes") {
    Scratch dir;
    MakePack(dir, kPackJsonNew, false);
    std::ostringstream out;
    std::ostringstream err;
    const std::string archive = dir.out("tool.bin");
    CHECK(RunHdPack({dir.path(), "-o", archive, "--threads", "2", "--verify"}, out, err) == 0);
    CHECK_MESSAGE(out.str().find("hdpack: 4/4 textures (100.0%)") != std::string::npos, out.str());
    CHECK(out.str().find("verified 4 entries") != std::string::npos);
    CHECK(fs::exists(archive));
    fs::remove(archive);

    std::ostringstream err2;
    CHECK(RunHdPack({dir.path()}, out, err2) == 2);                       // no -o
    CHECK(RunHdPack({dir.path(), "-o", archive, "--quality", "x"}, out, err2) == 2);
    CHECK(RunHdPack({dir.path("missing"), "-o", archive}, out, err2) == 2);
    CHECK(RunHdPack({dir.path(), "-o", archive, "--bogus"}, out, err2) == 2);
    dir.Write("pack.json", std::string("{not json"));
    CHECK(RunHdPack({dir.path(), "-o", archive}, out, err2) == 2);
    CHECK(err2.str().find("pack.json") != std::string::npos);
}
