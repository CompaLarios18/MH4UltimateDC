// Derived from Azahar src/core/hle/service/gsp/gsp_gpu.cpp, src/core/hle/service/gsp/gsp_interrupt.h,
// src/core/hle/service/gsp/gsp_command.h, src/video_core/pica/regs_external.h and
// src/video_core/renderer_software/sw_blitter.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of these files says "GPLv2 or any later version"): the layout of
// the GSP's interrupt relay queue and how an interrupt is put into it, the words of the GX
// commands, the flags of the transfer registers and the way a texture copy walks its gaps.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (gsp_gpu.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gsp_interrupt.h, regs_external.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
// Copyright 2023 Citra Emulator Project (sw_blitter.cpp)
//
// What the capture player says about a command that failed (N2-03b), and the one rule that keeps
// the GSP service's own writes out of the judgement of the Gpu:
//
//  * The capture's GPU_PAGES are the pages that changed while GPU::Execute ran. With the
//    reference's settings (D25) the GSP delivers an interrupt at once, inside that window, and puts
//    it into its interrupt relay queue, which lives in the GSP's shared memory: the first 0x100
//    bytes of a page (four queues of 0x40 bytes). That page is in GPU_PAGES, but a Gpu does not
//    write it: the service does. ExplainedByInterruptQueue says whether a page's change is exactly
//    what the command's interrupts do to a queue.
//  * DescribeGxCommand and the register variants decode a fill, display transfer, texture copy or
//    DMA into text (formats by name, sizes, scaling, flip, tiling, strides and gaps, addresses as
//    numbers) and into the address ranges the command writes, for the failure report. Nothing in
//    them reads memory: no byte of a capture reaches the text.
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::gpu_capture {

// ---- the GSP's interrupt relay queue ------------------------------------------------------------

inline constexpr u32 kRelayQueueBytes = 0x40;  // sizeof(InterruptRelayQueue)
inline constexpr u32 kRelayQueueCount = 4;     // GSP_GPU::MaxGSPThreads
inline constexpr u32 kRelayQueueSlots = 0x34;  // InterruptRelayQueue::max_slots

// What GSP_GPU::ProcessPendingInterruptImpl's queue_interrupt does to the queue at `queue` (0x40
// bytes: index, number_interrupts, error_code, config, two missed-PDC counters, 0x34 slots) for a
// non-VBlank interrupt `id`.
void PushRelayInterrupt(u8* queue, u32 id);

// True when `after` is `before` with the interrupts `ids` (in order) pushed into one of the four
// queues at the start of the page, and nothing else changed. False for an empty `ids`. Both spans
// are whole pages.
bool ExplainedByInterruptQueue(std::span<const u8> before, std::span<const u8> after,
                               std::span<const u32> ids);

// ---- comparing a page ---------------------------------------------------------------------------

struct PageDifference {
    u32 count = 0;  // bytes that differ
    u32 first = 0;  // offset of the first, inside the page
    u32 last = 0;   // offset of the last
};
// {0, 0, 0} when the two spans are equal; both must have the same size.
PageDifference ComparePages(std::span<const u8> a, std::span<const u8> b);

// ---- decoded commands ---------------------------------------------------------------------------

struct AddressRange {
    u32 begin = 0;   // physical
    u64 length = 0;  // bytes
};

struct CommandInfo {
    enum class Type { None, MemoryFill, DisplayTransfer, TextureCopy, Dma };
    Type type = Type::None;
    // Lines, joined by '\n', each starting with its own label; no trailing newline.
    std::string description;
    // What the command writes, as far as its fields tell (empty when they are refused).
    std::vector<AddressRange> output;
};

// Virtual to physical as GPU::VirtualToPhysicalAddress (the capture host's); nullopt for none.
using ToPhysical = std::function<std::optional<u32>(u32)>;

// The GX command of a record: its eight words as the capture holds them.
CommandInfo DescribeGxCommand(std::span<const u32, 8> words, const ToPhysical& to_physical);

// A command started through the registers. `regs` are the words of the display-transfer block
// (GPU register word 0x300 on, 11 words) or of one memory-fill unit (4 words, `unit` 0 or 1), as
// the player's copy of the registers holds them.
CommandInfo DescribeTransferRegisters(std::span<const u32, 11> regs);
CommandInfo DescribeFillRegisters(u32 unit, std::span<const u32, 4> regs);

// "0x20100000 to 0x2015f000 (389120 bytes, 95 pages)": addresses, a size and a page count.
std::string DescribeRange(u32 begin, u64 length);

// "RGBA8", "RGB8", "RGB565", "RGB5A1", "RGBA4" or "invalid(N)".
std::string PixelFormatName(u32 format);

} // namespace mh4n::gpu_capture
