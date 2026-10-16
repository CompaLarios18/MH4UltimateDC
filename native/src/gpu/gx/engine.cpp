// Derived from Azahar src/video_core/gpu.cpp, src/video_core/pica/pica_core.h,
// src/video_core/pica/regs_external.h, src/video_core/pica/regs_lcd.h,
// src/core/hle/service/gsp/gsp_command.h and src/core/memory.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of these files says
// "GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, pica_core.h, regs_external.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (regs_lcd.h, gsp_command.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h)
#include "gpu/gx/engine.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

namespace mh4n::gpu::gx {

namespace {

constexpr const char* kLog = "GxEngine";

// Azahar's addresses (core/memory.h) and register bases (gpu.cpp).
constexpr u32 kVramVaddr = 0x1F000000;
constexpr u32 kVramVaddrEnd = kVramVaddr + 0x00600000;
constexpr u32 kVaddrLcd = 0x1ED02000;
constexpr u32 kVaddrGpu = 0x1EF00000;

// Measured transfer speeds in MiB/s: [0] DMA, [1] texture copy; per CopyMode.
constexpr double kSpeedMibps[2][4] = {
    {190.0, 310.0, 380.0, 380.0},
    {450.0, 3100.0, 5400.0, 5400.0},
};

constexpr double MibpsToNsPerByte(double mib_per_sec) {
    return 1'000'000'000.0 / (mib_per_sec * 1024.0 * 1024.0);
}

constexpr bool IsVramVirtual(u32 addr) {
    return addr >= kVramVaddr && addr <= kVramVaddrEnd;
}

} // namespace

CopyMode GetCopyMode(bool input_vram, bool output_vram) {
    if (!input_vram && !output_vram) {
        return CopyMode::RamToRam;
    } else if (!input_vram && output_vram) {
        return CopyMode::RamToVram;
    } else if (input_vram && !output_vram) {
        return CopyMode::VramToRam;
    }
    return CopyMode::VramToVram;
}

u64 CalculateDelayNanoseconds(CopyMode mode, bool is_texture, size_t size) {
    const double base_ns_per_byte =
        MibpsToNsPerByte(kSpeedMibps[is_texture ? 1 : 0][static_cast<u32>(mode)]);
    return static_cast<u64>(static_cast<double>(size) * base_ns_per_byte);
}

u32 GxEngine::ToPhysical(u32 vaddr) {
    if (vaddr == 0) {
        return 0;
    }
    if (const auto physical = host_.VirtualToPhysical(vaddr)) {
        return *physical;
    }
    MH4N_LOG(Error, kLog, "Unknown virtual address @ {:#010X}", vaddr);
    return vaddr;
}

FillRegs GxEngine::LoadFill(u32 index) const {
    const u32 base = kRegFill0 + 4 * index;
    return {regs_[base], regs_[base + 1], regs_[base + 2], regs_[base + 3]};
}

TransferRegs GxEngine::LoadTransfer() const {
    TransferRegs regs;
    u32* words = &regs.input_address;
    for (u32 i = 0; i < 11; ++i) {
        words[i] = regs_[kRegTransfer + i];
    }
    return regs;
}

void GxEngine::Execute(const GxCommand& command) {
    const u32* w = command.words;
    switch (static_cast<CommandId>(w[0] & 0xFF)) {
    case CommandId::RequestDma:
        Dma(command);
        break;
    case CommandId::SubmitCmdList:
        // No command lists in this engine.
        break;
    case CommandId::MemoryFill: {
        // start1 value1 end1 start2 value2 end2 control1|control2<<16
        const u32 start1 = w[1], value1 = w[2], end1 = w[3];
        const u32 start2 = w[4], value2 = w[5], end2 = w[6];
        const u32 control1 = w[7] & 0xFFFF, control2 = w[7] >> 16;

        // If both buffers are set GSP dispatches PSC0 only.
        const bool has_both_bufs = start1 != 0 && start2 != 0;
        if (start1 != 0) {
            regs_[kRegFill0 + 0] = ToPhysical(start1) >> 3;
            regs_[kRegFill0 + 1] = ToPhysical(end1) >> 3;
            regs_[kRegFill0 + 2] = value1;
            regs_[kRegFill0 + 3] = control1;
            MemoryFill(0, has_both_bufs ? std::numeric_limits<u32>::max() : 0);
        }
        if (start2 != 0) {
            regs_[kRegFill0 + 4] = ToPhysical(start2) >> 3;
            regs_[kRegFill0 + 5] = ToPhysical(end2) >> 3;
            regs_[kRegFill0 + 6] = value2;
            regs_[kRegFill0 + 7] = control2;
            MemoryFill(1, has_both_bufs ? 0 : 1);
        }
        break;
    }
    case CommandId::DisplayTransfer:
        // in_address out_address in_size out_size flags
        regs_[kRegTransfer + 0] = ToPhysical(w[1]) >> 3;
        regs_[kRegTransfer + 1] = ToPhysical(w[2]) >> 3;
        regs_[kRegTransfer + 3] = w[3]; // input_size
        regs_[kRegTransfer + 2] = w[4]; // output_size
        regs_[kRegTransfer + 4] = w[5]; // flags
        regs_[kRegTransfer + 6] = (regs_[kRegTransfer + 6] & ~1u) | 1u;
        MemoryTransfer();
        break;
    case CommandId::TextureCopy:
        // in_address out_address size in_width_gap out_width_gap flags
        regs_[kRegTransfer + 0] = ToPhysical(w[1]) >> 3;
        regs_[kRegTransfer + 1] = ToPhysical(w[2]) >> 3;
        regs_[kRegTransfer + 8] = w[3];  // texture_copy.size
        regs_[kRegTransfer + 9] = w[4];  // texture_copy.input_size
        regs_[kRegTransfer + 10] = w[5]; // texture_copy.output_size
        regs_[kRegTransfer + 4] = w[6];  // flags
        regs_[kRegTransfer + 6] = (regs_[kRegTransfer + 6] & ~1u) | 1u;
        MemoryTransfer();
        break;
    case CommandId::CacheFlush:
        // Nothing is cached here.
        break;
    default:
        MH4N_LOG(Error, kLog, "Unknown command {:#08X}", w[0] & 0xFF);
        break;
    }
}

void GxEngine::Dma(const GxCommand& command) {
    const u32 source = command.words[1];
    const u32 dest = command.words[2];
    const u32 size = command.words[3];

    // Azahar copies through the application's memory map; the addresses a DMA takes are in the
    // linear heap or VRAM, which the host translates.
    const auto source_physical = host_.VirtualToPhysical(source);
    const auto dest_physical = host_.VirtualToPhysical(dest);
    if (!source_physical || !dest_physical) {
        MH4N_LOG(Error, kLog, "DMA with an address that is not physical memory: {:#010X} -> {:#010X}",
                 source, dest);
    } else {
        std::span<u8> src = Window(host_, *source_physical, size);
        std::span<u8> dst = Window(host_, *dest_physical, size);
        const size_t count = std::min(src.size(), dst.size());
        if (count < size) {
            MH4N_LOG(Error, kLog, "DMA of {:#x} bytes leaves the memory ({:#x} copied)", size,
                     count);
        }
        if (count != 0) {
            std::memmove(dst.data(), src.data(), count);
        }
    }

    const u64 delay = CalculateDelayNanoseconds(
        GetCopyMode(IsVramVirtual(source), IsVramVirtual(dest)), false, size);
    host_.SignalInterrupt(Interrupt::DMA, delay);
}

void GxEngine::MemoryFill(u32 index, u32 interrupt_index) {
    // Check if a memory fill was triggered.
    const FillRegs config = LoadFill(index);
    if (!config.Trigger()) {
        return;
    }

    gx::MemoryFill(host_, config);

    // Treat the fill as a texture transfer from VRAM.
    const u64 delay = CalculateDelayNanoseconds(GetCopyMode(true, config.IsVram()), true,
                                                config.EndAddress() - config.StartAddress());

    // It seems that it won't signal an interrupt if "address_start" is zero.
    if (config.StartAddress() != 0) {
        if (interrupt_index == 0) {
            host_.SignalInterrupt(Interrupt::PSC0, delay);
        } else if (interrupt_index == 1) {
            host_.SignalInterrupt(Interrupt::PSC1, delay);
        }
    }

    // Reset "trigger" and set "finished"; confirmed on hardware even if "address_start" is zero.
    u32& control = regs_[kRegFill0 + 4 * index + 3];
    control = (control & ~1u) & ~2u;
    control |= 2u;
}

void GxEngine::MemoryTransfer() {
    // Check if a transfer was triggered.
    const TransferRegs config = LoadTransfer();
    if (!config.Triggered()) {
        return;
    }

    u64 delay;
    if (config.IsTextureCopy()) {
        gx::TextureCopy(host_, config);
        delay = CalculateDelayNanoseconds(GetCopyMode(config.IsInputVram(), config.IsOutputVram()),
                                          true, config.texture_copy_size);
    } else {
        gx::DisplayTransfer(host_, config);
        // u32 arithmetic, as Azahar's BitField products are.
        const u32 size = config.InputWidth() * config.InputHeight() *
                         BytesPerPixel(config.InputFormat());
        delay = CalculateDelayNanoseconds(GetCopyMode(config.IsInputVram(), config.IsOutputVram()),
                                          true, size);
    }

    // Complete the transfer.
    regs_[kRegTransfer + 6] &= ~1u;
    host_.SignalInterrupt(Interrupt::PPF, delay);
}

void GxEngine::SetBufferSwap(u32 screen, const FrameBufferInfo& info) {
    if (screen > 1) {
        MH4N_LOG(Error, kLog, "buffer swap of screen {}", screen);
        return;
    }
    const u32 physical_left = ToPhysical(info.address_left);
    const u32 physical_right = ToPhysical(info.address_right);

    // framebuffer_config[screen]: address_left1 0x1A, address_left2 0x1B, format 0x1C,
    // active_fb 0x1E, stride 0x24, address_right1 0x25, address_right2 0x26.
    u32* framebuffer = &regs_[kRegFramebuffer + 0x40 * screen];
    if (info.active_fb == 0) {
        framebuffer[0x1A] = physical_left;
        framebuffer[0x25] = physical_right;
    } else {
        framebuffer[0x1B] = physical_left;
        framebuffer[0x26] = physical_right;
    }
    framebuffer[0x24] = info.stride;
    framebuffer[0x1C] = info.format;
    framebuffer[0x1E] = info.shown_fb;
}

void GxEngine::SetColorFill(u32 fill) {
    lcd_[kLcdColorFillTop] = fill;
    lcd_[kLcdColorFillBottom] = fill;
}

u32 GxEngine::ReadReg(u32 addr) {
    switch (addr & 0xFFFFF000) {
    case kVaddrLcd: {
        const u32 index = (addr - kVaddrLcd) / sizeof(u32);
        if (index < kLcdRegCount && addr % 4 == 0) {
            return lcd_[index];
        }
        break;
    }
    case kVaddrGpu:
    case kVaddrGpu + 0x1000: {
        const u32 index = (addr - kVaddrGpu) / sizeof(u32);
        if (index < kGpuRegCount && addr % 4 == 0) {
            return regs_[index];
        }
        break;
    }
    default:
        break;
    }
    MH4N_LOG(Error, kLog, "Read from unknown GPU address {:#010X}", addr);
    return 0;
}

void GxEngine::WriteReg(u32 addr, u32 value) {
    switch (addr & 0xFFFFF000) {
    case kVaddrLcd: {
        const u32 index = (addr - kVaddrLcd) / sizeof(u32);
        if (index < kLcdRegCount && addr % 4 == 0) {
            lcd_[index] = value;
            return;
        }
        break;
    }
    case kVaddrGpu:
    case kVaddrGpu + 0x1000: {
        const u32 index = (addr - kVaddrGpu) / sizeof(u32);
        if (index >= kGpuRegCount || addr % 4 != 0) {
            break;
        }
        regs_[index] = value;

        // Registers that trigger GPU actions. The command buffer triggers are not ours.
        if (index == kRegFillControl0) {
            MemoryFill(0, 0);
        } else if (index == kRegFillControl1) {
            MemoryFill(1, 1);
        } else if (index == kRegTransferTrigger) {
            MemoryTransfer();
        }
        return;
    }
    default:
        break;
    }
    MH4N_LOG(Error, kLog, "Write to unknown GPU address {:#010X}", addr);
}

// "cpu-gx" for the capture player: this engine and nothing for command lists.
MH4N_REGISTER(GpuFactory, "cpu-gx", [](Host& host, const Options&) -> std::unique_ptr<Gpu> {
    return std::make_unique<GxEngine>(host);
});

} // namespace mh4n::gpu::gx
