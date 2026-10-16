// Images for the capture player: PPM files, the comparison thresholds, the displayed screens and
// the PICA colour buffers (N2-02).
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "mh4n/gpu.h"
#include "mh4n/types.h"

namespace mh4n::gpu_capture {

// An RGB image, 3 bytes per pixel, row-major, top row first.
struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgb;

    bool empty() const {
        return rgb.empty();
    }
};

bool ReadPpm(const std::string& path, Image& image, std::string* error);
bool WritePpm(const std::string& path, const Image& image, std::string* error);

// ---- comparison (docs/native/ROADMAP.md section 6) ----------------------------------------------
// A frame passes when its PSNR is at least 40 dB and no 16x16 block has a mean error above
// 8/255, so a missing effect cannot hide in the average.
inline constexpr double kMinPsnrDb = 40.0;
inline constexpr double kMaxBlockError = 8.0; // out of 255
inline constexpr u32 kBlockSize = 16;

struct ImageMetrics {
    bool comparable = true; // false: the sizes differ (psnr and block are then meaningless)
    double psnr = 0.0;      // dB over the R, G and B values; infinity when identical
    double worst_block = 0.0; // the largest block mean of |a - b| over R, G and B, out of 255
    u32 worst_block_x = 0;    // that block's top-left pixel
    u32 worst_block_y = 0;

    bool Passes() const {
        return comparable && psnr >= kMinPsnrDb && worst_block <= kMaxBlockError;
    }
};

// Blocks at the right and bottom edges are smaller when the size is not a multiple of 16; their
// mean is over the pixels they have.
ImageMetrics CompareImages(const Image& reference, const Image& native);

// |reference - native| per channel, times `gain` (clamped to 255).
Image DifferenceImage(const Image& reference, const Image& native, u32 gain = 4);

// ---- colour conversions (Azahar's common/color.h) ------------------------------------------------
struct Rgb {
    u8 r, g, b;
};

// Display / transfer pixel formats (Pica::PixelFormat, regs_external.h).
enum DisplayFormat : u32 { kRGBA8 = 0, kRGB8 = 1, kRGB565 = 2, kRGB5A1 = 3, kRGBA4 = 4 };
u32 BytesPerPixel(u32 display_format); // 0 for a format that does not exist
Rgb DecodeDisplayPixel(u32 display_format, const u8* bytes);

// ---- PICA colour buffers (regs_framebuffer.h) ----------------------------------------------------
// Where a draw writes its colour: registers 0x117 (format), 0x11D (address / 8) and 0x11E
// (size), as the capture's PICA register file holds them.
struct ColorBufferConfig {
    u32 address = 0;  // physical
    u32 format = 0;   // Pica::FramebufferRegs::ColorFormat: RGBA8, RGB8, RGB5A1, RGB565, RGBA4
    u32 width = 0;
    u32 height = 0;

    friend bool operator==(const ColorBufferConfig&, const ColorBufferConfig&) = default;
    bool valid() const;
    u32 BytesPerPixel() const;
    // The bytes the buffer occupies: its size rounded up to whole 8x8 tiles.
    u32 ByteSize() const;
};
// Decodes the registers; `regs` has the 0x300 internal registers.
ColorBufferConfig ColorBufferFromRegisters(const u32* regs);

// Untiles (8x8 Morton tiles) and converts to RGB. `data` has ByteSize() bytes. The image's row 0
// is the first row stored.
Image DecodeColorBuffer(const u8* data, const ColorBufferConfig& config);

// ---- the displayed screens ------------------------------------------------------------------------
using PhysicalPointerFn = std::function<const u8*(u32 paddr, u32 size)>;

// The display registers as the capture's records leave them: Azahar keeps them in its register
// array, so a SWAP record changes them as GPU::SetBufferSwap does, a FILL record as
// GPU::SetColorFill, and an MMIO record as GPU::WriteReg.
class DisplayShadow {
public:
    static constexpr u32 kGpuRegWords = 0x432;
    static constexpr u32 kLcdRegWords = 0x400;
    static constexpr u32 kGpuBase = 0x1EF00000;
    static constexpr u32 kLcdBase = 0x1ED02000;

    // From the INIT record's GPU_REGS and LCD_REGS blocks (little-endian words).
    void Load(const u8* gpu_regs, const u8* lcd_regs);
    void WriteReg(u32 vaddr, u32 value); // ignores addresses it does not hold
    // `physical`: the SWAP record's addresses converted to physical.
    void ApplySwap(u32 screen, const gpu::FrameBufferInfo& info, u32 physical_left,
                   u32 physical_right);
    void ApplyFill(u32 word);
    // A word of the GPU register array (index below 0x432), 0 beyond it.
    u32 GpuReg(u32 index) const {
        return index < kGpuRegWords ? gpu_[index] : 0;
    }

    // Screen 0 is the top screen (left eye), 1 the bottom one. Builds the image the software
    // renderer shows at 1x, the same orientation as the reference PPMs: width = the number of
    // columns stored, height = the pixels per column.
    bool BuildScreen(u32 screen, const PhysicalPointerFn& memory, Image& image,
                     std::string* error) const;

private:
    std::array<u32, kGpuRegWords> gpu_{};
    std::array<u32, kLcdRegWords> lcd_{};
};

} // namespace mh4n::gpu_capture
