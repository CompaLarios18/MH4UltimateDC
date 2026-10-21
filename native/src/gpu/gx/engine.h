// Derived from Azahar src/video_core/gpu.cpp, src/video_core/pica/pica_core.h,
// src/video_core/pica/regs_external.h, src/video_core/pica/regs_lcd.h,
// src/core/hle/service/gsp/gsp_command.h and src/core/memory.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of these files says
// "GPLv2 or any later version"): the GSP command payloads, the GPU register positions and the
// interrupt delays.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, pica_core.h, regs_external.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (regs_lcd.h, gsp_command.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h)
//
// The GX engine on the CPU (N2-03): a gpu::Gpu that performs the DMA, memory fill, display
// transfer and texture copy commands, writes the same registers and signals the same interrupts,
// with the same delays, as Azahar's VideoCore::GPU. It does nothing for command lists (SubmitCmdList),
// the display (buffer swaps only update the registers) or the cache. Registered as "cpu-gx".
//
// An interrupt is only signalled through Host::SignalInterrupt. What the GSP service does with it
// (Azahar writes it into its interrupt relay queue in the GSP's shared memory, at once, when the
// GPU timings are not simulated) is not this engine's: that page is in a capture's GPU_PAGES for
// every command that signals one, and the capture player leaves it out of the judgement when it
// is exactly that write (docs/native/capture-player.md, N2-03b).
#pragma once

#include <array>
#include <memory>

#include "gpu/gx/blitter.h"
#include "mh4n/gpu.h"

namespace mh4n::gpu::gx {

// The speed table of Azahar's DelayGenerator, in nanoseconds for `size` bytes.
enum class CopyMode : u32 { RamToRam = 0, RamToVram = 1, VramToRam = 2, VramToVram = 3 };
CopyMode GetCopyMode(bool input_vram, bool output_vram);
u64 CalculateDelayNanoseconds(CopyMode mode, bool is_texture, size_t size);

// The GSP command ids.
enum class CommandId : u32 {
    RequestDma = 0,
    SubmitCmdList = 1,
    MemoryFill = 2,
    DisplayTransfer = 3,
    TextureCopy = 4,
    CacheFlush = 5,
};

// Word positions in the GPU register array (0x1EF00000 + 4 * position).
inline constexpr u32 kGpuRegCount = 0x732;
inline constexpr u32 kRegFill0 = 4;               // memory_fill_config[0]; [1] is 4 words further
inline constexpr u32 kRegFillControl0 = 4 + 3;    // the trigger word of unit 0
inline constexpr u32 kRegFillControl1 = 8 + 3;
inline constexpr u32 kRegFramebuffer = 0x100;     // framebuffer_config[0]; [1] is 0x40 words further
inline constexpr u32 kRegTransfer = 0x300;        // display_transfer_config
inline constexpr u32 kRegTransferTrigger = 0x300 + 6;
// Word positions in the LCD register array (0x1ED02000 + 4 * position).
inline constexpr u32 kLcdRegCount = 0x400;
inline constexpr u32 kLcdColorFillTop = 0x81;
inline constexpr u32 kLcdColorFillBottom = 0x281;

class GxEngine final : public Gpu {
public:
    explicit GxEngine(Host& host) : host_(host) {}

    void Execute(const GxCommand& command) override;
    void WriteReg(u32 addr, u32 value) override;
    u32 ReadReg(u32 addr) override;
    void SetBufferSwap(u32 screen, const FrameBufferInfo& info) override;
    void SetColorFill(u32 fill) override;
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}

    // Azahar's GPU::VirtualToPhysicalAddress through the host; an address the host does not know
    // stays as it is (Azahar logs it and does the same).
    u32 ToPhysical(u32 vaddr);

private:
    void Dma(const GxCommand& command);
    void MemoryFill(u32 index, u32 interrupt_index);
    void MemoryTransfer();
    FillRegs LoadFill(u32 index) const;
    TransferRegs LoadTransfer() const;

    Host& host_;
    std::array<u32, kGpuRegCount> regs_{};
    std::array<u32, kLcdRegCount> lcd_{};
};

} // namespace mh4n::gpu::gx
