// Derived from Azahar src/core/hw/y2r.cpp and src/common/color.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"); see y2r_convert.h.
// Copyright 2015 Citra Emulator Project (y2r.cpp)
// Copyright 2014 Citra Emulator Project (color.h)
#include "y2r_convert.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "mh4n/log.h"

namespace mh4n::y2r {

namespace {

constexpr std::size_t kMaxTiles = 1024 / 8;
constexpr std::size_t kTileSize = 8 * 8;
using ImageTile = std::array<u32, kTileSize>;

// Converts an image strip from the source YUV format into individual 8x8 RGB32 tiles.
void ConvertYuvToRgb(InputFormat format, const u8* input_y, const u8* input_u, const u8* input_v,
                     ImageTile output[], unsigned width, unsigned height,
                     const CoefficientSet& coefficients) {
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            s32 Y, U, V;
            if (format == InputFormat::YUV422_Indiv8 || format == InputFormat::YUV422_Indiv16) {
                Y = input_y[y * width + x];
                U = input_u[(y * width + x) / 2];
                V = input_v[(y * width + x) / 2];
            } else if (format == InputFormat::YUV420_Indiv8 || format == InputFormat::YUV420_Indiv16) {
                Y = input_y[y * width + x];
                U = input_u[((y / 2) * width + x) / 2];
                V = input_v[((y / 2) * width + x) / 2];
            } else {  // YUYV422_Interleaved
                Y = input_y[(y * width + x) * 2];
                U = input_y[(y * width + (x / 2) * 2) * 2 + 1];
                V = input_y[(y * width + (x / 2) * 2) * 2 + 3];
            }

            // This conversion process is bit-exact with hardware, as far as could be tested.
            const auto& c = coefficients;
            const s32 cY = c[0] * Y;

            s32 r = cY + c[1] * V;
            s32 g = cY - c[2] * V - c[3] * U;
            s32 b = cY + c[4] * U;

            const s32 rounding_offset = 0x18;
            r = (r >> 3) + c[5] + rounding_offset;
            g = (g >> 3) + c[6] + rounding_offset;
            b = (b >> 3) + c[7] + rounding_offset;

            const unsigned tile = x / 8;
            const unsigned tile_x = x % 8;
            u32* out = &output[tile][y * 8 + tile_x];
            *out = (static_cast<u32>(std::clamp(r >> 5, 0, 0xFF)) << 24) |
                   (static_cast<u32>(std::clamp(g >> 5, 0, 0xFF)) << 16) |
                   (static_cast<u32>(std::clamp(b >> 5, 0, 0xFF)) << 8);
        }
    }
}

// Simulates an incoming CDMA transfer. `n` converts 16-bit formats to 8-bit. False when the
// transfer cannot be simulated.
bool ReceiveData(GuestMemory& memory, u8* output, ConversionBuffer& buf, std::size_t n,
                 std::size_t amount_of_data) {
    const std::size_t output_unit = buf.transfer_unit / n;
    if (output_unit == 0 || amount_of_data % output_unit != 0) {
        MH4N_LOG(Error, "Service.Y2R", "transfer unit {} does not divide the data ({})", buf.transfer_unit,
                 amount_of_data);
        return false;
    }
    std::vector<u8> unit(buf.transfer_unit);
    while (amount_of_data > 0) {
        if (!memory.ReadBlock(unit.data(), buf.address, buf.transfer_unit)) {
            std::fill(unit.begin(), unit.end(), u8{0});
        }
        for (std::size_t i = 0; i < output_unit; ++i) {
            output[i] = unit[i * n];
        }
        output += output_unit;
        buf.address += buf.transfer_unit + buf.gap;
        buf.image_size -= buf.transfer_unit;
        amount_of_data -= output_unit;
    }
    return true;
}

constexpr u8 To5(u8 v) { return v >> 3; }
constexpr u8 To6(u8 v) { return v >> 2; }
constexpr u8 To1(u8 v) { return v >> 7; }

// Converts the intermediate RGB32 format to the final output format while simulating an outgoing CDMA
// transfer.
bool SendData(GuestMemory& memory, OutputFormat format, const u32* input, ConversionBuffer& buf,
              int amount_of_data, u8 alpha) {
    // Azahar never ends with a unit of 0 (the inner loop adds no pixel and the outer one spins forever).
    if (buf.transfer_unit == 0) {
        MH4N_LOG(Error, "Service.Y2R", "the output transfer unit is 0");
        return false;
    }
    std::vector<u8> unit;
    while (amount_of_data > 0) {
        unit.clear();
        // Azahar converts a full unit even when the strip has fewer pixels left: it reads past the pixel
        // buffer. Here the pixels the strip has are converted and the rest of the unit is zero bytes, so
        // the bytes written and the address advance are the same without reading past the buffer.
        while (unit.size() < buf.transfer_unit && amount_of_data > 0) {
            const u32 color = *input++;
            const u8 r = static_cast<u8>(color >> 24), g = static_cast<u8>(color >> 16),
                     b = static_cast<u8>(color >> 8), a = alpha;
            switch (format) {
            case OutputFormat::RGBA8:
                unit.insert(unit.end(), {a, b, g, r});
                break;
            case OutputFormat::RGB8:
                unit.insert(unit.end(), {b, g, r});
                break;
            case OutputFormat::RGB5A1: {
                const u16 data = static_cast<u16>((To5(r) << 11) | (To5(g) << 6) | (To5(b) << 1) | To1(a));
                unit.push_back(static_cast<u8>(data));
                unit.push_back(static_cast<u8>(data >> 8));
                break;
            }
            case OutputFormat::RGB565: {
                const u16 data = static_cast<u16>((To5(r) << 11) | (To6(g) << 5) | To5(b));
                unit.push_back(static_cast<u8>(data));
                unit.push_back(static_cast<u8>(data >> 8));
                break;
            }
            }
            amount_of_data -= 1;
        }
        if (unit.size() < buf.transfer_unit) unit.resize(buf.transfer_unit, u8{0});
        memory.WriteBlock(buf.address, unit.data(), static_cast<u32>(unit.size()));
        buf.address += buf.transfer_unit + buf.gap;
        buf.image_size -= buf.transfer_unit;
    }
    return true;
}

constexpr std::array<u8, kTileSize> kLinearLut = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,
    44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};

constexpr std::array<u8, kTileSize> kMortonLut = {
    0,  1,  4,  5,  16, 17, 20, 21, 2,  3,  6,  7,  18, 19, 22, 23, 8,  9,  12, 13, 24, 25,
    28, 29, 10, 11, 14, 15, 26, 27, 30, 31, 32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39,
    50, 51, 54, 55, 40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63};

void RotateTile0(const ImageTile& input, ImageTile& output, int height, const u8* out_map) {
    for (int i = 0; i < height * 8; ++i) {
        output[out_map[i]] = input[static_cast<std::size_t>(i)];
    }
}
void RotateTile90(const ImageTile& input, ImageTile& output, int height, const u8* out_map) {
    int out_i = 0;
    for (int x = 0; x < 8; ++x) {
        for (int y = height - 1; y >= 0; --y) {
            output[out_map[out_i++]] = input[static_cast<std::size_t>(y * 8 + x)];
        }
    }
}
void RotateTile180(const ImageTile& input, ImageTile& output, int height, const u8* out_map) {
    int out_i = 0;
    for (int i = height * 8 - 1; i >= 0; --i) {
        output[out_map[out_i++]] = input[static_cast<std::size_t>(i)];
    }
}
void RotateTile270(const ImageTile& input, ImageTile& output, int height, const u8* out_map) {
    int out_i = 0;
    for (int x = 8 - 1; x >= 0; --x) {
        for (int y = 0; y < height; ++y) {
            output[out_map[out_i++]] = input[static_cast<std::size_t>(y * 8 + x)];
        }
    }
}
void WriteTileToOutput(u32* output, const ImageTile& tile, int height, int line_stride) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < 8; ++x) {
            output[y * line_stride + x] = tile[static_cast<std::size_t>(y * 8 + x)];
        }
    }
}

}  // namespace

bool PerformConversion(GuestMemory& memory, ConversionConfiguration cvt) {
    if (cvt.input_line_width % 8 != 0 || cvt.input_line_width == 0 ||
        (cvt.block_alignment == BlockAlignment::Block8x8 && cvt.input_lines % 8 != 0)) {
        MH4N_LOG(Error, "Service.Y2R", "invalid line width {} or lines {}", cvt.input_line_width,
                 cvt.input_lines);
        return false;
    }
    const std::size_t num_tiles = cvt.input_line_width / 8;
    if (num_tiles > kMaxTiles) return false;

    // Buffer used as a CDMA source/target.
    std::vector<u8> data_buffer(static_cast<std::size_t>(cvt.input_line_width) * 8 * 4, 0);
    // Intermediate storage for decoded 8x8 image tiles. Always stored as RGB32.
    std::vector<ImageTile> tiles(num_tiles);
    ImageTile tmp_tile{};

    const u8* tile_remap = cvt.block_alignment == BlockAlignment::Linear ? kLinearLut.data() : kMortonLut.data();

    for (unsigned y = 0; y < cvt.input_lines; y += 8) {
        const unsigned row_height = std::min<unsigned>(cvt.input_lines - y, 8u);
        const std::size_t row_data_size = static_cast<std::size_t>(row_height) * cvt.input_line_width;

        u8* input_y = data_buffer.data();
        u8* input_u = input_y + 8 * cvt.input_line_width;
        u8* input_v = input_u + 8 * cvt.input_line_width / 2;

        bool ok = true;
        switch (cvt.input_format) {
        case InputFormat::YUV422_Indiv8:
            ok = ReceiveData(memory, input_y, cvt.src_Y, 1, row_data_size) &&
                 ReceiveData(memory, input_u, cvt.src_U, 1, row_data_size / 2) &&
                 ReceiveData(memory, input_v, cvt.src_V, 1, row_data_size / 2);
            break;
        case InputFormat::YUV420_Indiv8:
            ok = ReceiveData(memory, input_y, cvt.src_Y, 1, row_data_size) &&
                 ReceiveData(memory, input_u, cvt.src_U, 1, row_data_size / 4) &&
                 ReceiveData(memory, input_v, cvt.src_V, 1, row_data_size / 4);
            break;
        case InputFormat::YUV422_Indiv16:
            ok = ReceiveData(memory, input_y, cvt.src_Y, 2, row_data_size) &&
                 ReceiveData(memory, input_u, cvt.src_U, 2, row_data_size / 2) &&
                 ReceiveData(memory, input_v, cvt.src_V, 2, row_data_size / 2);
            break;
        case InputFormat::YUV420_Indiv16:
            ok = ReceiveData(memory, input_y, cvt.src_Y, 2, row_data_size) &&
                 ReceiveData(memory, input_u, cvt.src_U, 2, row_data_size / 4) &&
                 ReceiveData(memory, input_v, cvt.src_V, 2, row_data_size / 4);
            break;
        case InputFormat::YUYV422_Interleaved:
            input_u = nullptr;
            input_v = nullptr;
            ok = ReceiveData(memory, input_y, cvt.src_YUYV, 1, row_data_size * 2);
            break;
        default:
            return false;
        }
        if (!ok) return false;
        ConvertYuvToRgb(cvt.input_format, input_y, input_u, input_v, tiles.data(), cvt.input_line_width,
                        row_height, cvt.coefficients);

        u32* output_buffer = reinterpret_cast<u32*>(data_buffer.data());
        for (std::size_t i = 0; i < num_tiles; ++i) {
            int image_strip_width = 0;
            int output_stride = 0;
            switch (cvt.rotation) {
            case Rotation::None:
                RotateTile0(tiles[i], tmp_tile, static_cast<int>(row_height), tile_remap);
                image_strip_width = cvt.input_line_width;
                output_stride = 8;
                break;
            case Rotation::Clockwise_90:
                RotateTile90(tiles[i], tmp_tile, static_cast<int>(row_height), tile_remap);
                image_strip_width = 8;
                output_stride = 8 * static_cast<int>(row_height);
                break;
            case Rotation::Clockwise_180:
                // For 180 and 270 degree rotations the order of the tiles in the strip is inverted too
                RotateTile180(tiles[num_tiles - i - 1], tmp_tile, static_cast<int>(row_height), tile_remap);
                image_strip_width = cvt.input_line_width;
                output_stride = 8;
                break;
            case Rotation::Clockwise_270:
                RotateTile270(tiles[num_tiles - i - 1], tmp_tile, static_cast<int>(row_height), tile_remap);
                image_strip_width = 8;
                output_stride = 8 * static_cast<int>(row_height);
                break;
            }
            switch (cvt.block_alignment) {
            case BlockAlignment::Linear:
                WriteTileToOutput(output_buffer, tmp_tile, static_cast<int>(row_height), image_strip_width);
                output_buffer += output_stride;
                break;
            case BlockAlignment::Block8x8:
                WriteTileToOutput(output_buffer, tmp_tile, 8, 8);
                output_buffer += kTileSize;
                break;
            }
        }

        if (!SendData(memory, cvt.output_format, reinterpret_cast<const u32*>(data_buffer.data()), cvt.dst,
                      static_cast<int>(row_data_size), static_cast<u8>(cvt.alpha))) {
            return false;
        }
    }
    return true;
}

}  // namespace mh4n::y2r
