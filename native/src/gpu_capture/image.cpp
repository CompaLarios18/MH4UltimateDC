// Derived from Azahar src/video_core/renderer_software/renderer_software.cpp, src/common/color.h,
// src/video_core/pica/regs_external.h, regs_lcd.h, regs_framebuffer.h and pica_core.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): how a displayed screen is made
// from the framebuffer registers (formats, rotation, the colour fill), the colour decoders, the
// numbering of the pixel and colour-buffer formats, and the register layouts (the word positions
// of the display, LCD and colour-buffer registers).
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (renderer_software.cpp)
// Copyright 2014 Citra Emulator Project (color.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (regs_external.h, pica_core.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (regs_lcd.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h)
#include "gpu_capture/image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include <fmt/format.h>

namespace mh4n::gpu_capture {

namespace {

bool Fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

u32 LoadLe32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

// common/color.h (alpha is not compared, so Convert1To8 is not needed)
constexpr u8 Convert4To8(u8 value) {
    return static_cast<u8>((value << 4) | value);
}
constexpr u8 Convert5To8(u8 value) {
    return static_cast<u8>((value << 3) | (value >> 2));
}
constexpr u8 Convert6To8(u8 value) {
    return static_cast<u8>((value << 2) | (value >> 4));
}

u16 LoadLe16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

// Skips whitespace and '#' comments of a PPM header; returns the next token.
bool NextToken(const std::vector<u8>& data, size_t& pos, std::string& token) {
    while (pos < data.size()) {
        if (data[pos] == '#') {
            while (pos < data.size() && data[pos] != '\n') {
                ++pos;
            }
        } else if (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' ||
                   data[pos] == '\n') {
            ++pos;
        } else {
            break;
        }
    }
    token.clear();
    while (pos < data.size() && data[pos] != ' ' && data[pos] != '\t' && data[pos] != '\r' &&
           data[pos] != '\n') {
        token.push_back(static_cast<char>(data[pos++]));
    }
    return !token.empty();
}

// The offset of pixel (x, y), both in 0..7, inside an 8x8 tile (Morton order).
u32 MortonInTile(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
           ((y & 4) << 3);
}

// Pica::FramebufferRegs::ColorFormat to the display format with the same layout.
u32 ColorFormatToDisplay(u32 color_format) {
    switch (color_format) {
    case 0:
        return kRGBA8;
    case 1:
        return kRGB8;
    case 2:
        return kRGB5A1;
    case 3:
        return kRGB565;
    case 4:
        return kRGBA4;
    default:
        return ~0u;
    }
}

// Word indices in the GPU register array (Pica::PicaCore::Regs, framebuffer_config[2]).
constexpr u32 kFbConfigBase = 0x100;
constexpr u32 kFbConfigWords = 0x40;
constexpr u32 kFbSize = 0x17;
constexpr u32 kFbAddressLeft1 = 0x1A;
constexpr u32 kFbAddressLeft2 = 0x1B;
constexpr u32 kFbAddressRight1 = 0x25;
constexpr u32 kFbAddressRight2 = 0x26;
constexpr u32 kFbFormat = 0x1C;
constexpr u32 kFbActive = 0x1E;
constexpr u32 kFbStride = 0x24;
// Word indices in the LCD register array (Pica::RegsLcd).
constexpr u32 kLcdColorFillTop = 0x81;
constexpr u32 kLcdColorFillBottom = 0x281;

} // namespace

// ---- PPM ----------------------------------------------------------------------------------------

bool ReadPpm(const std::string& path, Image& image, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return Fail(error, fmt::format("cannot open {}", path));
    }
    std::vector<u8> data;
    u8 buffer[65536];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        data.insert(data.end(), buffer, buffer + n);
    }
    std::fclose(f);
    size_t pos = 0;
    std::string magic, w, h, maxval;
    if (!NextToken(data, pos, magic) || !NextToken(data, pos, w) || !NextToken(data, pos, h) ||
        !NextToken(data, pos, maxval)) {
        return Fail(error, fmt::format("{}: truncated PPM header", path));
    }
    if (magic != "P6") {
        return Fail(error, fmt::format("{}: not a binary PPM (P6)", path));
    }
    u64 width = 0, height = 0, max = 0;
    try {
        width = std::stoull(w);
        height = std::stoull(h);
        max = std::stoull(maxval);
    } catch (const std::exception&) {
        return Fail(error, fmt::format("{}: malformed PPM header", path));
    }
    if (max != 255 || width == 0 || height == 0 || width > 16384 || height > 16384) {
        return Fail(error, fmt::format("{}: unsupported PPM ({}x{}, maxval {})", path, width, height,
                                       max));
    }
    ++pos; // the single whitespace byte after maxval
    const u64 bytes = width * height * 3;
    if (data.size() < pos || data.size() - pos < bytes) {
        return Fail(error, fmt::format("{}: truncated PPM pixel data", path));
    }
    image.width = static_cast<u32>(width);
    image.height = static_cast<u32>(height);
    image.rgb.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                     data.begin() + static_cast<std::ptrdiff_t>(pos + bytes));
    return true;
}

bool WritePpm(const std::string& path, const Image& image, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return Fail(error, fmt::format("cannot write {}", path));
    }
    std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
    const bool ok = image.rgb.empty() ||
                    std::fwrite(image.rgb.data(), 1, image.rgb.size(), f) == image.rgb.size();
    std::fclose(f);
    return ok ? true : Fail(error, fmt::format("cannot write {}", path));
}

// ---- comparison ---------------------------------------------------------------------------------

ImageMetrics CompareImages(const Image& reference, const Image& native) {
    ImageMetrics m;
    if (reference.width != native.width || reference.height != native.height ||
        reference.rgb.size() != native.rgb.size() ||
        reference.rgb.size() != static_cast<size_t>(reference.width) * reference.height * 3) {
        m.comparable = false;
        return m;
    }
    const u32 w = reference.width, h = reference.height;
    if (w == 0 || h == 0) {
        m.psnr = std::numeric_limits<double>::infinity();
        return m;
    }
    u64 squared = 0;
    const u32 blocks_x = (w + kBlockSize - 1) / kBlockSize;
    const u32 blocks_y = (h + kBlockSize - 1) / kBlockSize;
    std::vector<u64> block_sum(static_cast<size_t>(blocks_x) * blocks_y, 0);
    for (u32 y = 0; y < h; ++y) {
        const u8* a = &reference.rgb[static_cast<size_t>(y) * w * 3];
        const u8* b = &native.rgb[static_cast<size_t>(y) * w * 3];
        for (u32 x = 0; x < w; ++x) {
            u64 abs_sum = 0;
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(int{a[x * 3 + c]} - int{b[x * 3 + c]});
                squared += static_cast<u64>(d * d);
                abs_sum += static_cast<u64>(d);
            }
            block_sum[static_cast<size_t>(y / kBlockSize) * blocks_x + x / kBlockSize] += abs_sum;
        }
    }
    const double mse = static_cast<double>(squared) / (static_cast<double>(w) * h * 3.0);
    m.psnr = mse == 0.0 ? std::numeric_limits<double>::infinity()
                        : 10.0 * std::log10(255.0 * 255.0 / mse);
    for (u32 by = 0; by < blocks_y; ++by) {
        for (u32 bx = 0; bx < blocks_x; ++bx) {
            const u32 bw = std::min(kBlockSize, w - bx * kBlockSize);
            const u32 bh = std::min(kBlockSize, h - by * kBlockSize);
            const double mean = static_cast<double>(block_sum[static_cast<size_t>(by) * blocks_x + bx]) /
                                (static_cast<double>(bw) * bh * 3.0);
            if (mean > m.worst_block) {
                m.worst_block = mean;
                m.worst_block_x = bx * kBlockSize;
                m.worst_block_y = by * kBlockSize;
            }
        }
    }
    return m;
}

Image DifferenceImage(const Image& reference, const Image& native, u32 gain) {
    Image out;
    out.width = std::min(reference.width, native.width);
    out.height = std::min(reference.height, native.height);
    out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const int a = reference.rgb[(static_cast<size_t>(y) * reference.width + x) * 3 + c];
                const int b = native.rgb[(static_cast<size_t>(y) * native.width + x) * 3 + c];
                out.rgb[(static_cast<size_t>(y) * out.width + x) * 3 + c] =
                    static_cast<u8>(std::min<u32>(255, static_cast<u32>(std::abs(a - b)) * gain));
            }
        }
    }
    return out;
}

// ---- colour conversions -------------------------------------------------------------------------

u32 BytesPerPixel(u32 display_format) {
    switch (display_format) {
    case kRGBA8:
        return 4;
    case kRGB8:
        return 3;
    case kRGB565:
    case kRGB5A1:
    case kRGBA4:
        return 2;
    default:
        return 0;
    }
}

Rgb DecodeDisplayPixel(u32 display_format, const u8* bytes) {
    switch (display_format) {
    case kRGBA8:
        return {bytes[3], bytes[2], bytes[1]};
    case kRGB8:
        return {bytes[2], bytes[1], bytes[0]};
    case kRGB565: {
        const u16 pixel = LoadLe16(bytes);
        return {Convert5To8((pixel >> 11) & 0x1F), Convert6To8((pixel >> 5) & 0x3F),
                Convert5To8(pixel & 0x1F)};
    }
    case kRGB5A1: {
        const u16 pixel = LoadLe16(bytes);
        return {Convert5To8((pixel >> 11) & 0x1F), Convert5To8((pixel >> 6) & 0x1F),
                Convert5To8((pixel >> 1) & 0x1F)};
    }
    case kRGBA4: {
        const u16 pixel = LoadLe16(bytes);
        return {Convert4To8((pixel >> 12) & 0xF), Convert4To8((pixel >> 8) & 0xF),
                Convert4To8((pixel >> 4) & 0xF)};
    }
    default:
        return {0, 0, 0};
    }
}

// ---- colour buffers -------------------------------------------------------------------------------

bool ColorBufferConfig::valid() const {
    return width != 0 && height != 0 && ColorFormatToDisplay(format) != ~0u;
}

u32 ColorBufferConfig::BytesPerPixel() const {
    return gpu_capture::BytesPerPixel(ColorFormatToDisplay(format));
}

u32 ColorBufferConfig::ByteSize() const {
    const u32 tiles_x = (width + 7) / 8;
    const u32 tiles_y = (height + 7) / 8;
    return tiles_x * tiles_y * 64 * BytesPerPixel();
}

ColorBufferConfig ColorBufferFromRegisters(const u32* regs) {
    ColorBufferConfig config;
    config.format = (regs[0x117] >> 16) & 7;
    config.address = (regs[0x11D] & 0x0FFFFFFF) * 8;
    config.width = regs[0x11E] & 0x7FF;
    config.height = ((regs[0x11E] >> 12) & 0x3FF) + 1;
    return config;
}

Image DecodeColorBuffer(const u8* data, const ColorBufferConfig& config) {
    Image image;
    image.width = config.width;
    image.height = config.height;
    image.rgb.assign(static_cast<size_t>(config.width) * config.height * 3, 0);
    const u32 display = ColorFormatToDisplay(config.format);
    const u32 bpp = gpu_capture::BytesPerPixel(display);
    if (bpp == 0) {
        return image;
    }
    const u32 tiles_x = (config.width + 7) / 8;
    for (u32 y = 0; y < config.height; ++y) {
        for (u32 x = 0; x < config.width; ++x) {
            const u32 tile = (y / 8) * tiles_x + x / 8;
            const size_t offset = (static_cast<size_t>(tile) * 64 + MortonInTile(x & 7, y & 7)) * bpp;
            const Rgb c = DecodeDisplayPixel(display, data + offset);
            u8* out = &image.rgb[(static_cast<size_t>(y) * config.width + x) * 3];
            out[0] = c.r;
            out[1] = c.g;
            out[2] = c.b;
        }
    }
    return image;
}

// ---- the displayed screens --------------------------------------------------------------------------

void DisplayShadow::Load(const u8* gpu_regs, const u8* lcd_regs) {
    for (u32 i = 0; i < kGpuRegWords; ++i) {
        gpu_[i] = LoadLe32(gpu_regs + 4 * i);
    }
    for (u32 i = 0; i < kLcdRegWords; ++i) {
        lcd_[i] = LoadLe32(lcd_regs + 4 * i);
    }
}

void DisplayShadow::WriteReg(u32 vaddr, u32 value) {
    // GPU::WriteReg: the page picks the array, the offset the word.
    switch (vaddr & 0xFFFFF000) {
    case kLcdBase: {
        const u32 index = (vaddr - kLcdBase) / 4;
        if (index < kLcdRegWords) {
            lcd_[index] = value;
        }
        break;
    }
    case kGpuBase:
    case kGpuBase + 0x1000: {
        const u32 index = (vaddr - kGpuBase) / 4;
        if (index < kGpuRegWords) { // the internal registers are not display state
            gpu_[index] = value;
        }
        break;
    }
    default:
        break;
    }
}

void DisplayShadow::ApplySwap(u32 screen, const gpu::FrameBufferInfo& info, u32 physical_left,
                              u32 physical_right) {
    if (screen > 1) {
        return;
    }
    u32* fb = &gpu_[kFbConfigBase + kFbConfigWords * screen];
    // GPU::SetBufferSwap
    if (info.active_fb == 0) {
        fb[kFbAddressLeft1] = physical_left;
        fb[kFbAddressRight1] = physical_right;
    } else {
        fb[kFbAddressLeft2] = physical_left;
        fb[kFbAddressRight2] = physical_right;
    }
    fb[kFbStride] = info.stride;
    fb[kFbFormat] = info.format;
    fb[kFbActive] = info.shown_fb;
}

void DisplayShadow::ApplyFill(u32 word) {
    // GPU::SetColorFill: both screens.
    lcd_[kLcdColorFillTop] = word;
    lcd_[kLcdColorFillBottom] = word;
}

bool DisplayShadow::BuildScreen(u32 screen, const PhysicalPointerFn& memory, Image& image,
                                std::string* error) const {
    if (screen > 1) {
        return Fail(error, "no such screen");
    }
    // RendererSoftware::LoadFBToScreenInfo
    const u32* fb = &gpu_[kFbConfigBase + kFbConfigWords * screen];
    const u32 color_fill = lcd_[screen == 0 ? kLcdColorFillTop : kLcdColorFillBottom];
    const u32 address = fb[kFbActive] == 0 ? fb[kFbAddressLeft1] : fb[kFbAddressLeft2];
    const u32 format = fb[kFbFormat] & 7;
    const u32 bpp = BytesPerPixel(format);
    if (bpp == 0) {
        return Fail(error, fmt::format("unknown framebuffer format {}", format));
    }
    const u32 pixel_stride = fb[kFbStride] / bpp;
    const u32 height = fb[kFbSize] >> 16; // columns stored
    if (pixel_stride == 0 || height == 0) {
        return Fail(error, "the framebuffer has no size");
    }
    const bool filled = ((color_fill >> 24) & 1) != 0;
    image.width = height;
    image.height = pixel_stride;
    image.rgb.assign(static_cast<size_t>(image.width) * image.height * 3, 0);
    if (filled) {
        for (size_t i = 0; i < static_cast<size_t>(image.width) * image.height; ++i) {
            image.rgb[i * 3 + 0] = static_cast<u8>(color_fill & 0xFF);
            image.rgb[i * 3 + 1] = static_cast<u8>((color_fill >> 8) & 0xFF);
            image.rgb[i * 3 + 2] = static_cast<u8>((color_fill >> 16) & 0xFF);
        }
        return true;
    }
    const u64 stored = static_cast<u64>(height) * pixel_stride * bpp;
    if (stored > 0xFFFFFFFFull) {
        return Fail(error, "the framebuffer is too large");
    }
    const u8* data = memory(address, static_cast<u32>(stored));
    if (data == nullptr) {
        return Fail(error, fmt::format("the framebuffer at 0x{:08x} ({} bytes) is not in memory",
                                       address, stored));
    }
    // Azahar reads index (y * stride + stride - x), which for x = 0 is the first pixel of the
    // next column, and for the last column one pixel past the buffer. Read that pixel when the
    // memory has it, as zero otherwise.
    const u8* extra = memory(address + static_cast<u32>(stored), bpp);
    static const u8 kZero[4] = {0, 0, 0, 0};
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < pixel_stride; ++x) {
            const u64 index = static_cast<u64>(y) * pixel_stride + pixel_stride - x;
            const u8* pixel = index * bpp < stored ? data + index * bpp : (extra ? extra : kZero);
            const Rgb c = DecodeDisplayPixel(format, pixel);
            u8* out = &image.rgb[(static_cast<size_t>(x) * height + y) * 3];
            out[0] = c.r;
            out[1] = c.g;
            out[2] = c.b;
        }
    }
    return true;
}

} // namespace mh4n::gpu_capture
