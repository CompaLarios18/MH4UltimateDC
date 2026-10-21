// Derived from Azahar src/core/hw/y2r.cpp, src/core/hw/y2r.h, src/core/hle/service/cam/y2r_u.h and
// src/common/color.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says
// "GPLv2 or any later version"): the conversion configuration and its enumerations, and the
// conversion itself (PerformConversion).
// Copyright 2015 Citra Emulator Project (y2r.cpp, y2r.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (y2r_u.h)
// Copyright 2014 Citra Emulator Project (color.h)
//
// The Y2R conversion (N1-19): the pixels of a YUV image in guest memory become an RGB image in guest
// memory, as the hardware does it and Azahar reproduces it.
#pragma once

#include <array>

#include "mh4n/memory.h"
#include "mh4n/types.h"

namespace mh4n::y2r {

enum class InputFormat : u8 {
    YUV422_Indiv8 = 0,
    YUV420_Indiv8 = 1,
    YUV422_Indiv16 = 2,
    YUV420_Indiv16 = 3,
    YUYV422_Interleaved = 4,
};
enum class OutputFormat : u8 { RGBA8 = 0, RGB8 = 1, RGB5A1 = 2, RGB565 = 3 };
enum class Rotation : u8 { None = 0, Clockwise_90 = 1, Clockwise_180 = 2, Clockwise_270 = 3 };
enum class BlockAlignment : u8 { Linear = 0, Block8x8 = 1 };

using CoefficientSet = std::array<s16, 8>;

// One DMA stream: where it is, how much is left (without the gaps), the size of a transfer and the
// bytes skipped between transfers.
struct ConversionBuffer {
    u32 address = 0;
    u32 image_size = 0;
    u16 transfer_unit = 0;
    u16 gap = 0;
};

struct ConversionConfiguration {
    InputFormat input_format = InputFormat::YUV422_Indiv8;
    OutputFormat output_format = OutputFormat::RGBA8;
    Rotation rotation = Rotation::None;
    BlockAlignment block_alignment = BlockAlignment::Linear;
    u16 input_line_width = 0;
    u16 input_lines = 0;
    CoefficientSet coefficients{};
    u8 padding = 0;
    u16 alpha = 0;
    ConversionBuffer src_Y, src_U, src_V, src_YUYV;
    ConversionBuffer dst;
};

// Converts the whole image. Memory that cannot be read gives zeros and memory that cannot be written
// is skipped (Azahar reads and writes host pointers). True if it ran to the end.
//
// Departures from Azahar:
//  - an input transfer unit of 0, or one that does not divide the data, ends the conversion early
//    (Azahar's ReceiveData asserts); an output transfer unit of 0 ends it early too (Azahar's SendData
//    would loop forever);
//  - in the last strip of a Linear image, when the strip's pixel count is not a multiple of the output
//    unit's pixel count, Azahar's SendData converts a whole unit and so reads past its pixel buffer
//    (it does not assert). Here the pixels the strip has are written and the rest of the unit is zero
//    bytes: the bytes written and the address advance are those of Azahar, nothing is read past the
//    buffer, and the bytes past the real pixels are zeros where Azahar writes whatever it read;
//  - Azahar writes the output through a host pointer that advances by the bytes written plus the gap;
//    with an output unit that is not a multiple of the pixel size it drifts from the descriptor's
//    address (which advances by the unit plus the gap). Here every unit is written at the descriptor's
//    address.
bool PerformConversion(GuestMemory& memory, ConversionConfiguration conversion);

}  // namespace mh4n::y2r
