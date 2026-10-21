// Derived from Azahar src/video_core/gpu.cpp, src/video_core/gpu.h, src/video_core/pica/pica_core.h,
// src/video_core/pica/pica_core.cpp, src/video_core/pica/regs_external.h,
// src/video_core/pica/regs_internal.h, src/video_core/pica/regs_pipeline.h,
// src/video_core/pica/regs_lcd.h and src/core/hle/service/gsp/gsp_command.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of these files says
// "GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, gpu.h, pica_core.h,
//   pica_core.cpp, regs_external.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_pipeline.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (regs_lcd.h, gsp_command.h)
#include "gpu_null/null_gpu.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

#include "gpu/gx/blitter.h"
#include "gpu/gx/engine.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

namespace mh4n::gpu::null {

namespace {

constexpr const char* kLog = "NullGpu";

// Azahar's virtual addresses (core/memory.h) and the bases of the register windows (gpu.cpp).
constexpr u32 kVramVaddr = 0x1F000000;
constexpr u32 kVramVaddrEnd = kVramVaddr + 0x00600000;
constexpr u32 kVaddrLcd = 0x1ED02000;
constexpr u32 kVaddrGpu = 0x1EF00000;

// Word positions in the register array of PicaCore::Regs (regs_external.h, pica_core.h).
constexpr u32 kRegFill = 4;            // memory_fill_config[2], 4 words each
constexpr u32 kRegFillControl0 = 7;    // the trigger word of unit 0 (and unit 1 four words later)
constexpr u32 kRegFillControl1 = 11;
constexpr u32 kRegFramebuffer = 0x100; // framebuffer_config[2], 0x40 words each
constexpr u32 kRegTransfer = 0x300;    // display_transfer_config, 11 words
constexpr u32 kRegTransferTrigger = kRegTransfer + 6;
constexpr u32 kRegInternal = 0x400;    // RegsInternal, 0x300 words (0x1EF01000)
constexpr u32 kInternalRegCount = 0x300;

// Word positions inside a framebuffer_config.
constexpr u32 kFbSize = 0x17;
constexpr u32 kFbLeft1 = 0x1A;
constexpr u32 kFbLeft2 = 0x1B;
constexpr u32 kFbFormat = 0x1C;
constexpr u32 kFbActive = 0x1E;
constexpr u32 kFbStride = 0x24;
constexpr u32 kFbRight1 = 0x25;
constexpr u32 kFbRight2 = 0x26;

// Internal registers (RegsInternal, ids; regs_internal.h and regs_pipeline.h).
// gs is at 0x280: bool_uniforms, int_uniforms[4] and 4 words of padding come first.
constexpr u32 kGsInputConfig = 0x289;
constexpr u32 kIrqRequest = 0x10;
constexpr u32 kIrqCompare = 0x20;
constexpr u32 kIrqMask = 0x30;
constexpr u32 kIrqAutostop = 0x34;
constexpr u32 kCommandBufferSize = 0x238;     // size[2], bits 0-19, in units of 8 bytes
constexpr u32 kCommandBufferAddress = 0x23A;  // addr[2], bits 0-27, in units of 8 bytes
constexpr u32 kCommandBufferTrigger = 0x23C;  // trigger[2]

// The most command words (headers, values and extra data) one submission is scanned for, jumps
// included: 16M words are 64 MiB, more than VRAM. Azahar has no bound.
constexpr u64 kMaxScanWork = u64{1} << 24;

// LCD registers (regs_lcd.h).
constexpr u32 kLcdColorFillTop = 0x81;
constexpr u32 kLcdColorFillBottom = 0x281;

// ExpandBitsToBytes of pica_core.cpp: a 4-bit mask to a 4-byte one.
constexpr u32 ExpandMask(u32 mask) {
    u32 expanded = 0;
    for (u32 byte = 0; byte < 4; ++byte) {
        if ((mask >> byte) & 1) {
            expanded |= 0xFFu << (8 * byte);
        }
    }
    return expanded;
}

// pica_core.cpp's any_byte_match.
constexpr bool AnyByteMatch(u32 a, u32 b) {
    return ((a & 0xFF) == (b & 0xFF)) || (((a >> 8) & 0xFF) == ((b >> 8) & 0xFF)) ||
           (((a >> 16) & 0xFF) == ((b >> 16) & 0xFF)) || (((a >> 24) & 0xFF) == ((b >> 24) & 0xFF));
}

constexpr bool IsVramVirtual(u32 addr) {
    return addr >= kVramVaddr && addr <= kVramVaddrEnd;
}

// The commands of gsp_command.h (CommandId).
constexpr u32 kCmdRequestDma = 0;
constexpr u32 kCmdSubmitCmdList = 1;
constexpr u32 kCmdMemoryFill = 2;
constexpr u32 kCmdDisplayTransfer = 3;
constexpr u32 kCmdTextureCopy = 4;
constexpr u32 kCmdCacheFlush = 5;
constexpr u32 kCmdCount = 6;

} // namespace

NullGpu::NullGpu(Host& host) : host_(host) {
    // PicaCore::InitializeRegs: the values GSP sets, and the framebuffer defaults of nn::gx.
    regs_[kRegInternal + kIrqAutostop] = 1;
    regs_[kRegInternal + kIrqMask] = 0xFFFFFFF0;
    // "Older versions of libctru didn't initialize this, initialize it here to avoid endless black
    // screen."
    regs_[kRegInternal + kIrqCompare] = 0x12345678;
    // "Tales of Abyss expects this register to have the following default values" (pica_core.cpp:
    // 125-128): gs.max_input_attribute_index = 1 (bits 0-3) and gs.shader_mode = ShaderMode::VS,
    // 0xA0 (bits 24-31).
    regs_[kRegInternal + kGsInputConfig] = 0xA0000001;

    u32* top = &regs_[kRegFramebuffer];
    u32* bottom = &regs_[kRegFramebuffer + 0x40];
    top[kFbLeft1] = 0x181E6000;
    top[kFbLeft2] = 0x1822C800;
    top[kFbRight1] = 0x18273000;
    top[kFbRight2] = 0x182B9800;
    bottom[kFbLeft1] = 0x1848F000;
    bottom[kFbLeft2] = 0x184C7800;
    top[kFbSize] = 240u | (400u << 16);
    bottom[kFbSize] = 240u | (320u << 16);
    for (u32* screen : {top, bottom}) {
        screen[kFbStride] = 3 * 240;
        screen[kFbFormat] = static_cast<u32>(gx::PixelFormat::RGB8);
        screen[kFbActive] = 0;
    }
}

u32 NullGpu::ToPhysical(u32 vaddr) {
    // GPU::VirtualToPhysicalAddress: 0 stays 0; an address in no window is logged and kept.
    if (vaddr == 0) {
        return 0;
    }
    if (const auto physical = host_.VirtualToPhysical(vaddr)) {
        return *physical;
    }
    MH4N_LOG(Error, kLog, "Unknown virtual address @ {:#010X}", vaddr);
    return vaddr;
}

void NullGpu::ReportUnknownAddress(const char* what, u32 addr) {
    // Azahar's UNREACHABLE_MSG. Once per page, and for the first few pages only, so that a game
    // that polls does not flood the log.
    const u32 page = addr >> 12;
    const auto first = reported_pages_.begin();
    const auto known = first + std::min<size_t>(reported_count_, reported_pages_.size());
    if (std::find(first, known, page) != known || reported_count_ >= reported_pages_.size()) {
        return;
    }
    reported_pages_[reported_count_++] = page;
    MH4N_LOG(Error, kLog, "{} unknown GPU address {:#010X}", what, addr);
}

void NullGpu::Signal(Interrupt id, u64 delay_ns) {
    const u32 index = static_cast<u32>(id);
    ++stats_.interrupts[index];
    stats_.last_delay_ns[index] = delay_ns;
    host_.SignalInterrupt(id, pass_delays_ ? delay_ns : 0);
}

void NullGpu::Execute(const GxCommand& command) {
    const u32* w = command.words;
    const u32 id = w[0] & 0xFF;
    if (id < kCmdCount) {
        ++stats_.commands[id];
    }
    switch (id) {
    case kCmdRequestDma:
        Dma(command);
        break;
    case kCmdSubmitCmdList: {
        // address size flags unused[3] do_flush
        const u32 address = w[1];
        const u32 size = w[2];
        // Write to the command buffer GPU registers (BitField::Assign keeps the other bits).
        u32& addr_reg = regs_[kRegInternal + kCommandBufferAddress];
        addr_reg = (addr_reg & ~0x0FFFFFFFu) | ((ToPhysical(address) >> 3) & 0x0FFFFFFFu);
        u32& size_reg = regs_[kRegInternal + kCommandBufferSize];
        size_reg = (size_reg & ~0x000FFFFFu) | ((size >> 3) & 0x000FFFFFu);
        regs_[kRegInternal + kCommandBufferTrigger] = 1;
        // Trigger processing of the command list.
        SubmitCmdList(0);
        break;
    }
    case kCmdMemoryFill: {
        // start1 value1 end1 start2 value2 end2 control1|control2<<16
        const u32 start1 = w[1], value1 = w[2], end1 = w[3];
        const u32 start2 = w[4], value2 = w[5], end2 = w[6];
        const u32 control1 = w[7] & 0xFFFF, control2 = w[7] >> 16;

        // If both buffers are set GSP dispatches PSC0 only.
        const bool has_both_bufs = start1 != 0 && start2 != 0;
        if (start1 != 0) {
            regs_[kRegFill + 0] = ToPhysical(start1) >> 3;
            regs_[kRegFill + 1] = ToPhysical(end1) >> 3;
            regs_[kRegFill + 2] = value1;
            regs_[kRegFill + 3] = control1;
            MemoryFill(0, has_both_bufs ? std::numeric_limits<u32>::max() : 0);
        }
        if (start2 != 0) {
            regs_[kRegFill + 4] = ToPhysical(start2) >> 3;
            regs_[kRegFill + 5] = ToPhysical(end2) >> 3;
            regs_[kRegFill + 6] = value2;
            regs_[kRegFill + 7] = control2;
            MemoryFill(1, has_both_bufs ? 0 : 1);
        }
        break;
    }
    case kCmdDisplayTransfer:
        // in_address out_address in_size out_size flags
        regs_[kRegTransfer + 0] = ToPhysical(w[1]) >> 3;
        regs_[kRegTransfer + 1] = ToPhysical(w[2]) >> 3;
        regs_[kRegTransfer + 3] = w[3];  // input_size
        regs_[kRegTransfer + 2] = w[4];  // output_size
        regs_[kRegTransfer + 4] = w[5];  // flags
        regs_[kRegTransferTrigger] = (regs_[kRegTransferTrigger] & ~1u) | 1u;
        MemoryTransfer();
        break;
    case kCmdTextureCopy:
        // in_address out_address size in_width_gap out_width_gap flags
        regs_[kRegTransfer + 0] = ToPhysical(w[1]) >> 3;
        regs_[kRegTransfer + 1] = ToPhysical(w[2]) >> 3;
        regs_[kRegTransfer + 8] = w[3];   // texture_copy.size
        regs_[kRegTransfer + 9] = w[4];   // texture_copy.input_size
        regs_[kRegTransfer + 10] = w[5];  // texture_copy.output_size
        regs_[kRegTransfer + 4] = w[6];   // flags
        regs_[kRegTransferTrigger] = (regs_[kRegTransferTrigger] & ~1u) | 1u;
        MemoryTransfer();
        break;
    case kCmdCacheFlush:
        // "Rasterizer flushing handled elsewhere": nothing is cached here.
        break;
    default:
        ++stats_.unknown_commands;
        MH4N_LOG(Error, kLog, "Unknown command {:#08X}", id);
        break;
    }
}

void NullGpu::Dma(const GxCommand& command) {
    const u32 source = command.words[1];
    const u32 dest = command.words[2];
    const u32 size = command.words[3];

    // Azahar copies through the application's memory map (CopyBlock); the host only knows the
    // physical areas, which is where a DMA goes: the linear heap and VRAM.
    const auto source_physical = host_.VirtualToPhysical(source);
    const auto dest_physical = host_.VirtualToPhysical(dest);
    if (!source_physical || !dest_physical) {
        MH4N_LOG(Error, kLog, "DMA with an address that is not physical memory: {:#010X} -> {:#010X}",
                 source, dest);
    } else {
        std::span<u8> src = gx::Window(host_, *source_physical, size);
        std::span<u8> dst = gx::Window(host_, *dest_physical, size);
        const size_t count = std::min(src.size(), dst.size());
        if (count < size) {
            MH4N_LOG(Error, kLog, "DMA of {:#x} bytes leaves the memory ({:#x} copied)", size, count);
        }
        if (count != 0) {
            std::memmove(dst.data(), src.data(), count);
        }
    }

    const u64 delay = gx::CalculateDelayNanoseconds(
        gx::GetCopyMode(IsVramVirtual(source), IsVramVirtual(dest)), false, size);
    Signal(Interrupt::DMA, delay);
}

void NullGpu::MemoryFill(u32 index, u32 interrupt_index) {
    // Check if a memory fill was triggered.
    const u32 base = kRegFill + 4 * index;
    const gx::FillRegs config{regs_[base], regs_[base + 1], regs_[base + 2], regs_[base + 3]};
    if (!config.Trigger()) {
        return;
    }

    // Perform memory fill.
    gx::MemoryFill(host_, config);

    // Treat the fill as a texture transfer from VRAM.
    const u64 delay = gx::CalculateDelayNanoseconds(gx::GetCopyMode(true, config.IsVram()), true,
                                                    config.EndAddress() - config.StartAddress());

    // It seems that it won't signal an interrupt if "address_start" is zero.
    if (config.StartAddress() != 0) {
        if (interrupt_index == 0) {
            Signal(Interrupt::PSC0, delay);
        } else if (interrupt_index == 1) {
            Signal(Interrupt::PSC1, delay);
        }
    }

    // Reset "trigger" and set "finished"; confirmed on hardware even if "address_start" is zero.
    u32& control = regs_[base + 3];
    control = (control & ~1u) & ~2u;
    control |= 2u;
}

void NullGpu::MemoryTransfer() {
    // Check if a transfer was triggered.
    gx::TransferRegs config;
    u32* words = &config.input_address;
    for (u32 i = 0; i < 11; ++i) {
        words[i] = regs_[kRegTransfer + i];
    }
    if (!config.Triggered()) {
        return;
    }

    // The transfer itself only makes pixels: it is not performed.
    u64 delay;
    if (config.IsTextureCopy()) {
        delay = gx::CalculateDelayNanoseconds(
            gx::GetCopyMode(config.IsInputVram(), config.IsOutputVram()), true,
            config.texture_copy_size);
    } else {
        // u32 arithmetic, as Azahar's BitField products are.
        const u32 size =
            config.InputWidth() * config.InputHeight() * gx::BytesPerPixel(config.InputFormat());
        delay = gx::CalculateDelayNanoseconds(
            gx::GetCopyMode(config.IsInputVram(), config.IsOutputVram()), true, size);
    }
    ++stats_.skipped_transfers;

    // Complete the transfer.
    regs_[kRegTransferTrigger] &= ~1u;
    Signal(Interrupt::PPF, delay);
}

void NullGpu::SubmitCmdList(u32 channel) {
    // Check if a command list was triggered.
    u32& trigger = regs_[kRegInternal + kCommandBufferTrigger + channel];
    if (trigger == 0) {
        return;
    }
    const u32 paddr = (regs_[kRegInternal + kCommandBufferAddress + channel] & 0x0FFFFFFFu) * 8;
    const u32 size = (regs_[kRegInternal + kCommandBufferSize + channel] & 0x000FFFFFu) * 8;
    ++stats_.command_lists;
    const u32 p3d = ScanCmdList(paddr, size);
    for (u32 i = 0; i < p3d; ++i) {
        // The delay depends on the number of commands and triangles, which this GPU does not count.
        Signal(Interrupt::P3D, 0);
    }
    trigger = 0;
}

u32 NullGpu::ScanCmdList(u32 paddr, u32 size) {
    if (size < 8) {
        return 0;  // not a command: the loop of ProcessCmdList does not run
    }
    const u8* head = host_.PhysicalPointer(paddr, size);
    if (head == nullptr) {
        // Azahar reads the list through GetPhysicalPointer without looking and would crash. One
        // finalize is what a list ends with.
        MH4N_LOG(Error, kLog, "command list at {:#010X} (+{:#x}) is not in physical memory", paddr, size);
        return 1;
    }
    // PicaCore::CommandList: where the scan is. A write to a command buffer trigger register
    // replaces it (CommandList::Reset), as the jump of a hardware command list does.
    u32 length = size / 4;
    u32 index = 0;
    auto word = [](const u8* base, u32 at) {
        u32 value;
        std::memcpy(&value, base + 4 * static_cast<size_t>(at), sizeof(value));
        return value;
    };
    auto reg = [&](u32 id) -> u32& { return regs_[kRegInternal + id]; };

    u32 signalled = 0;
    bool stop_requested = false;
    // A list that jumps to itself runs for ever in Azahar's GPU thread; here it would stop the
    // scheduler thread, so the work of one submission is bounded (a departure, see the header).
    u64 work = 0;
    bool aborted = false;

    // HandleSpecialReg for the finalize register.
    auto finalize = [&]() {
        if (AnyByteMatch(reg(kIrqRequest), reg(kIrqCompare))) {
            ++signalled;
            if (reg(kIrqAutostop) != 0) {
                stop_requested = true;
            }
        }
    };
    // HandleSpecialReg for the command buffer triggers: continue in the list the channel's address
    // and size registers describe, from its first word. What is left of the current list is
    // dropped; stop_requested stays.
    auto jump = [&](u32 channel) {
        const u32 target = (reg(kCommandBufferAddress + channel) & 0x0FFFFFFFu) * 8;
        const u32 target_size = (reg(kCommandBufferSize + channel) & 0x000FFFFFu) * 8;
        ++stats_.command_list_jumps;
        index = 0;
        if (target_size < 8) {
            length = 0;  // no command in it: the loop of ProcessCmdList ends
            return;
        }
        const u8* target_head = host_.PhysicalPointer(target, target_size);
        if (target_head == nullptr) {
            // Azahar would read through a null pointer. As for a first list that is not in
            // memory, the list that was not read counts as one finalize, so that the game's wait
            // for the end of its command lists does not hang; the scan ends.
            MH4N_LOG(Error, kLog,
                     "command list jump to {:#010X} (+{:#x}) is not in physical memory", target,
                     target_size);
            ++signalled;
            stop_requested = true;
            length = 0;
            return;
        }
        head = target_head;
        length = target_size / 4;
    };
    // The handler of a register that has just been written.
    auto special = [&](u32 id) {
        if (id == kIrqRequest) {
            finalize();
        } else if (id == kCommandBufferTrigger || id == kCommandBufferTrigger + 1) {
            jump(id - kCommandBufferTrigger);
        }
    };
    // WriteInternalReg: out-of-range registers are no-ops.
    auto write = [&](u32 id, u32 value, u32 mask) {
        if (id >= kInternalRegCount) {
            return;
        }
        const u32 write_mask = ExpandMask(mask);
        reg(id) = (reg(id) & ~write_mask) | (value & write_mask);
        special(id);
    };

    while (index < length) {
        if (stop_requested) {
            break;
        }
        if (index % 2 != 0) {
            ++index;
        }
        if (index + 1 >= length) {
            break;  // Azahar would read the header past the end of the list
        }
        work += 2;
        if (work > kMaxScanWork) {
            aborted = true;
            break;
        }
        // Read the header and the value to write.
        const u32 value = word(head, index++);
        const u32 header = word(head, index++);
        const u32 id = header & 0xFFFF;
        const u32 mask = (header >> 16) & 0xF;
        const u32 count = (header >> 20) & 0x7FF;
        const bool group = (header >> 31) != 0;

        // Write to the requested PICA register.
        write(id, value, mask);
        if (count == 0) {
            continue;
        }

        // Write any extra parameters as well. Like Azahar, they are taken from where the scan is
        // after the first write: when that write was a jump, from the start of the new list.
        const u32 available = std::min(count, length > index ? length - index : 0);
        if (available < count) {
            MH4N_LOG(Error, kLog, "command list at {:#010X}: a command's data leaves the list", paddr);
        }
        const u8* extra = head + 4 * static_cast<size_t>(index);
        index += available;
        work += available;
        if (available == 0) {
            continue;
        }
        if (!group) {
            // The same register written `available` times in a row (WriteInternalRegBatch): only
            // the last value is stored, and a special register is handled once per value.
            if (id >= kInternalRegCount) {
                continue;
            }
            const u32 write_mask = ExpandMask(mask);
            reg(id) = (reg(id) & ~write_mask) | (word(extra, available - 1) & write_mask);
            if (id == kIrqRequest) {
                for (u32 i = 0; i < available && !stop_requested; ++i) {
                    finalize();
                }
            } else if (id == kCommandBufferTrigger || id == kCommandBufferTrigger + 1) {
                // Azahar resets the list once for each value; the registers read are the same
                // each time, so once is the same.
                jump(id - kCommandBufferTrigger);
            }
        } else {
            // Sequential registers from id + 1 (WriteInternalRegSequential). A jump among them
            // leaves the rest of the values to be written, as they are read from `extra`.
            if (id + 1 >= kInternalRegCount) {
                continue;
            }
            const u32 n = std::min(available, kInternalRegCount - (id + 1));
            for (u32 i = 0; i < n && !stop_requested; ++i) {
                write(id + 1 + i, word(extra, i), mask);
            }
        }
    }
    if (aborted) {
        MH4N_LOG(Error, kLog,
                 "command list at {:#010X} (+{:#x}): the scan stopped after {} words (a list that "
                 "never ends?)",
                 paddr, size, kMaxScanWork);
    }
    return signalled;
}

void NullGpu::SetBufferSwap(u32 screen, const FrameBufferInfo& info) {
    if (screen > 1) {
        // Azahar indexes framebuffer_config[screen_id] without looking.
        MH4N_LOG(Error, kLog, "buffer swap of screen {}", screen);
        return;
    }
    ++stats_.buffer_swaps[screen];
    const u32 physical_left = ToPhysical(info.address_left);
    const u32 physical_right = ToPhysical(info.address_right);

    // Update framebuffer properties.
    u32* framebuffer = &regs_[kRegFramebuffer + 0x40 * screen];
    if (info.active_fb == 0) {
        framebuffer[kFbLeft1] = physical_left;
        framebuffer[kFbRight1] = physical_right;
    } else {
        framebuffer[kFbLeft2] = physical_left;
        framebuffer[kFbRight2] = physical_right;
    }
    framebuffer[kFbStride] = info.stride;
    framebuffer[kFbFormat] = info.format;
    framebuffer[kFbActive] = info.shown_fb;
}

void NullGpu::SetColorFill(u32 fill) {
    lcd_[kLcdColorFillTop] = fill;
    lcd_[kLcdColorFillBottom] = fill;
}

void NullGpu::FlushRegion(u32, u32) {
    ++stats_.flushes;
}

void NullGpu::InvalidateRegion(u32, u32) {
    ++stats_.invalidations;
}

void NullGpu::VBlank(u32 screen) {
    if (screen < 2) {
        ++stats_.vblanks[screen];
    }
}

u32 NullGpu::ReadReg(u32 addr) {
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
    ReportUnknownAddress("Read from", addr);
    return 0;
}

void NullGpu::WriteReg(u32 addr, u32 value) {
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

        // Handle registers that trigger GPU actions.
        if (index == kRegFillControl0) {
            MemoryFill(0, 0);
        } else if (index == kRegFillControl1) {
            MemoryFill(1, 1);
        } else if (index == kRegTransferTrigger) {
            MemoryTransfer();
        } else if (index == kRegInternal + kCommandBufferTrigger) {
            SubmitCmdList(0);
        } else if (index == kRegInternal + kCommandBufferTrigger + 1) {
            SubmitCmdList(1);
        }
        return;
    }
    default:
        break;
    }
    ReportUnknownAddress("Write to", addr);
}

// The runtime's default GPU.
MH4N_REGISTER(GpuFactory, "null", [](Host& host, const Options&) -> std::unique_ptr<Gpu> {
    return std::make_unique<NullGpu>(host);
});

} // namespace mh4n::gpu::null
