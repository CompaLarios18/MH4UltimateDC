// Derived from Azahar src/video_core/renderer_software/sw_blitter.cpp, src/video_core/gpu.cpp,
// src/video_core/pica/regs_external.h, src/video_core/utils.h, src/common/color.h,
// src/core/memory.h and src/core/memory.cpp (the physical memory areas, IsValidPhysicalAddress) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of these files says
// "GPLv2 or any later version"): the register layouts and the algorithms of the memory fill,
// the display transfer and the texture copy.
// Copyright 2023 Citra Emulator Project (sw_blitter.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, regs_external.h)
// Copyright 2014 Citra Emulator Project (utils.h, color.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h, memory.cpp)
//
// The blitter of the GX engine (N2-03): the three operations that work on memory, as Azahar's
// SwBlitter does them, bit for bit. They take the register words the GPU holds (the commands of
// the GSP are turned into them by engine.cpp) and work on physical memory through a gpu::Host.
//
// Where Azahar reads or writes without looking (an address range that leaves the memory area, a
// pixel format that is not one of the five), this code reads zeros, skips the write or does
// nothing, and logs it: a game cannot rely on those, and a bad range must not crash the host.
#pragma once

#include <span>

#include "mh4n/gpu.h"
#include "mh4n/types.h"

namespace mh4n::gpu::gx {

enum class PixelFormat : u32 { RGBA8 = 0, RGB8 = 1, RGB565 = 2, RGB5A1 = 3, RGBA4 = 4 };

// 4, 3, 2, 2, 2 for the five formats, 0 for the invalid values 5 to 7.
u32 BytesPerPixel(u32 format);

// The offset of the pixel (x, y) inside its 8x8 tile in Morton order, plus the tile's place in
// its row of tiles, times the pixel size: Azahar's VideoCore::GetMortonOffset. It does not
// include the offset of the row of tiles (coarse_y * stride).
u32 MortonOffset(u32 x, u32 y, u32 bytes_per_pixel);

// The words of one memory fill unit (Pica::MemoryFillConfig, 4 words).
struct FillRegs {
    u32 address_start = 0; // physical address >> 3
    u32 address_end = 0;   // physical address >> 3
    u32 value = 0;         // the 32-bit value; the low 16 bits for 16-bit fills; bytes 0, 1, 2 are R, G, B
    u32 control = 0;       // bit 0 trigger, bit 1 finished, bit 8 fill 24-bit, bit 9 fill 32-bit

    u32 StartAddress() const {
        return address_start * 8;
    }
    u32 EndAddress() const {
        return address_end * 8;
    }
    bool Trigger() const {
        return (control & 1) != 0;
    }
    // Azahar's IsVRAM: not inside the 128 MiB of the Old 3DS FCRAM.
    bool IsVram() const;
};

// The words of the transfer engine (Pica::DisplayTransferConfig, 0x2C bytes), in register order.
struct TransferRegs {
    u32 input_address = 0;  // physical address >> 3
    u32 output_address = 0; // physical address >> 3
    u32 output_size = 0;    // width in the low 16 bits, height in the high 16
    u32 input_size = 0;
    u32 flags = 0;
    u32 unused = 0;
    u32 trigger = 0; // bit 0
    u32 unused2 = 0;
    u32 texture_copy_size = 0;        // the low 4 bits are ignored
    u32 texture_copy_input_size = 0;  // width in the low 16 bits, gap in the high 16, in 16-byte units
    u32 texture_copy_output_size = 0; // the same for the output

    u32 InputAddress() const {
        return input_address * 8;
    }
    u32 OutputAddress() const {
        return output_address * 8;
    }
    u32 OutputWidth() const {
        return output_size & 0xFFFF;
    }
    u32 OutputHeight() const {
        return output_size >> 16;
    }
    u32 InputWidth() const {
        return input_size & 0xFFFF;
    }
    u32 InputHeight() const {
        return input_size >> 16;
    }
    bool FlipVertically() const {
        return (flags & 1) != 0;
    }
    bool InputLinear() const {
        return (flags & 2) != 0;
    }
    bool CropInputLines() const {
        return (flags & 4) != 0;
    }
    bool IsTextureCopy() const {
        return (flags & 8) != 0;
    }
    bool DontSwizzle() const {
        return (flags & 0x20) != 0;
    }
    u32 InputFormat() const {
        return (flags >> 8) & 7;
    }
    u32 OutputFormat() const {
        return (flags >> 12) & 7;
    }
    // 0 no scaling, 1 halve the width, 2 halve both; 3 is not implemented (Azahar neither).
    u32 Scaling() const {
        return (flags >> 24) & 3;
    }
    bool Triggered() const {
        return (trigger & 1) != 0;
    }
    bool IsInputVram() const;
    bool IsOutputVram() const;
};

// Azahar's SwBlitter::MemoryFill / DisplayTransfer / TextureCopy. They do not signal interrupts.
void MemoryFill(Host& host, const FillRegs& config);
void DisplayTransfer(Host& host, const TransferRegs& config);
void TextureCopy(Host& host, const TransferRegs& config);

// Up to `size` bytes of physical memory from `paddr`, fewer when the area ends first, empty when
// `paddr` is in no area.
std::span<u8> Window(Host& host, u32 paddr, u64 size);

// Azahar's MemorySystem::IsValidPhysicalAddress: in an area, the address one past its end included.
bool IsValidPhysicalAddress(Host& host, u32 paddr);

} // namespace mh4n::gpu::gx
