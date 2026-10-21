// Derived from Azahar src/video_core/gpu.cpp, src/video_core/gpu.h, src/video_core/pica/pica_core.h,
// src/video_core/pica/pica_core.cpp, src/video_core/pica/regs_external.h,
// src/video_core/pica/regs_internal.h, src/video_core/pica/regs_pipeline.h,
// src/video_core/pica/regs_lcd.h and src/core/hle/service/gsp/gsp_command.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of these files says
// "GPLv2 or any later version"): what each GX command does and which interrupt it signals, the
// positions of the GPU registers, their initial values (PicaCore::InitializeRegs), the layout of a
// command list and the registers that finalize one.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, gpu.h, pica_core.h,
//   pica_core.cpp, regs_external.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_pipeline.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (regs_lcd.h, gsp_command.h)
//
// The null GPU (N1-17): a gpu::Gpu that completes every GX command at once and draws nothing. It
// signals the interrupts Azahar's VideoCore::GPU signals for each command (PSC0 and PSC1 for fills,
// PPF for transfers and texture copies, DMA, and P3D for command lists), keeps the register file so
// that reads give back what was written, performs the memory fills and the DMA copies (cheap, and
// they keep guest memory realistic), and skips the display transfers and texture copies, which
// only make pixels. Registered as "null", the runtime's default GPU.
//
// Command lists: the list is not run, but it is read far enough to know how many P3D interrupts
// Azahar's PicaCore::ProcessCmdList would signal for it: one for each write to the finalize
// register (irq_request) whose value matches irq_compare in some byte, up to the first one when
// irq_autostop is set. The game's command-list progress counts those interrupts. Every write of
// the list to a register below 0x300 is applied to the register file, as the PICA core does
// (drawing registers hold their values; nothing draws). A write to a command buffer trigger
// register (0x23C, 0x23D) continues the scan in the list the address and size registers name,
// from its first word, and drops the rest of the current one (PicaCore::HandleSpecialReg). The
// scan never draws, so the P3D counts are exact only for the finalize rule above.
//
// Departures from Azahar in the scan (N1-17 report): a list or a jump target that is not in
// physical memory counts as one finalize and is logged (Azahar crashes); the data of a command
// that leaves its list is cut; the words scanned for one submission are bounded (16M, Azahar has
// no bound and would run for ever on a list that jumps to itself).
//
// Departure in DMA: Azahar's CopyBlock copies between any two mapped addresses. gpu::Host only knows
// the physical areas, so a DMA whose source or destination is outside the linear heap and VRAM
// copies nothing, is logged, and still signals DMA. A plain guest-memory access in gpu::Host would
// remove the difference (a question for the orchestrator).
//
// The delay of each interrupt: Azahar's GPU passes the time the work would take on a console
// (DelayGenerator); its GSP ignores it with the reference's settings (D25), so the interrupt
// reaches the game at once. The delay is computed for every interrupt and kept in Stats. It is
// handed to Host::SignalInterrupt only with SetPassDelays(true): the runtime's host applies it as
// game time before the GSP sees the interrupt, which D25 says must not happen (see the N1-17
// report).
//
// Threads (principle 6): only the scheduler thread calls it.
#pragma once

#include <array>

#include "mh4n/gpu.h"
#include "mh4n/types.h"

namespace mh4n::gpu::null {

inline constexpr u32 kGpuRegCount = 0x732;  // PicaCore::Regs::NUM_REGS: external 0x400, internal 0x300
inline constexpr u32 kLcdRegCount = 0x400;

// Counters of what the GPU was asked to do and what it signalled.
struct Stats {
    u64 commands[6] = {};      // executed, by GX command id
    u64 unknown_commands = 0;  // GX commands with another id
    u64 interrupts[7] = {};    // signalled, by gpu::Interrupt
    // The delay Azahar's GPU passes with the last interrupt of each kind (nanoseconds).
    u64 last_delay_ns[7] = {};
    u64 vblanks[2] = {};
    u64 buffer_swaps[2] = {};
    u64 skipped_transfers = 0;  // display transfers and texture copies, which draw nothing
    u64 command_lists = 0;      // lists read (GX SubmitCmdList and the command buffer registers)
    u64 command_list_jumps = 0; // writes to a command buffer trigger inside a list
    u64 flushes = 0;            // FlushRegion calls
    u64 invalidations = 0;      // InvalidateRegion calls
};

class NullGpu final : public Gpu {
public:
    explicit NullGpu(Host& host);

    void Execute(const GxCommand& command) override;
    void WriteReg(u32 addr, u32 value) override;
    u32 ReadReg(u32 addr) override;
    void SetBufferSwap(u32 screen, const FrameBufferInfo& info) override;
    void SetColorFill(u32 fill) override;
    void FlushRegion(u32 vaddr, u32 size) override;
    void InvalidateRegion(u32 vaddr, u32 size) override;
    void VBlank(u32 screen) override;

    // Whether the interrupts carry the delay Azahar's GPU passes (default: no, they carry 0).
    void SetPassDelays(bool pass) { pass_delays_ = pass; }
    bool PassDelays() const { return pass_delays_; }
    const Stats& GetStats() const { return stats_; }

    // The register file, by word (0x1EF00000 + 4 * index), for tests.
    u32 GpuRegister(u32 index) const { return regs_[index]; }

private:
    u32 ToPhysical(u32 vaddr);
    void ReportUnknownAddress(const char* what, u32 addr);
    void Signal(Interrupt id, u64 delay_ns);
    void Dma(const GxCommand& command);
    void MemoryFill(u32 index, u32 interrupt_index);
    void MemoryTransfer();
    // GPU::SubmitCmdList for command buffer channel 0 or 1: reads the list if the channel's
    // trigger register is set, signals its P3D interrupts and clears the trigger.
    void SubmitCmdList(u32 channel);
    // PicaCore::ProcessCmdList without the drawing; returns the number of P3D interrupts it signals.
    u32 ScanCmdList(u32 paddr, u32 size);

    Host& host_;
    bool pass_delays_ = false;
    std::array<u32, kGpuRegCount> regs_{};
    std::array<u32, kLcdRegCount> lcd_{};
    // Addresses outside the two register windows are logged once per 4 KiB page.
    std::array<u32, 8> reported_pages_{};
    u32 reported_count_ = 0;
    Stats stats_;
};

}  // namespace mh4n::gpu::null
