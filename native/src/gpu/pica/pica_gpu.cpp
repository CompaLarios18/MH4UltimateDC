// Derived from Azahar src/video_core/gpu.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; the file says "Licensed under GPLv2 or any later version"):
// GPU::Execute for SubmitCmdList, GPU::WriteReg and GPU::ReadReg for the register windows, and
// GPU::VirtualToPhysicalAddress's treatment of an address it does not know.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp)
//
// `--gpu pica-only` (N2-04): the command processor behind the Gpu interface, with a backend that
// only counts the draws. It does no GX work other than command lists (use it with
// `--judge none`, and `--check registers`), and draws nothing.
#include <array>
#include <cstring>
#include <memory>

#include "gpu/pica/command_processor.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"

namespace mh4n::gpu::pica {
namespace {

constexpr u32 kVaddrLcd = 0x1ED02000;
constexpr u32 kLcdRegs = 0x400;
constexpr u32 kGxSubmitCmdList = 1;

class CountingBackend final : public Backend {
public:
    void Draw(const DrawCall& draw) override {
        ++draws;
        if (draw.kind == DrawKind::Immediate) {
            ++immediate_draws;
        }
    }
    u64 draws = 0;
    u64 immediate_draws = 0;
};

class PicaOnlyGpu final : public gpu::Gpu, public gpu::GpuDebug {
public:
    explicit PicaOnlyGpu(gpu::Host& host) : host_(host), processor_(host, backend_) {
        processor_.SetRegisterWriteLog(true);
    }

    void Execute(const gpu::GxCommand& command) override {
        if ((command.words[0] & 0xFF) != kGxSubmitCmdList) {
            return; // fills, transfers, copies and DMA are the GX engine's (N2-03)
        }
        // GPU::VirtualToPhysicalAddress: 0 stays 0, and an address in none of the windows is
        // taken to be physical already.
        const u32 vaddr = command.words[1];
        const u32 paddr = vaddr == 0 ? 0 : host_.VirtualToPhysical(vaddr).value_or(vaddr);
        processor_.SubmitGxCmdList(paddr, command.words[2]);
    }

    void WriteReg(u32 addr, u32 value) override {
        const u32 window = addr & 0xFFFFF000;
        if (window == kVaddrLcd) {
            const u32 index = (addr - kVaddrLcd) / 4;
            if (index < kLcdRegs) {
                lcd_[index] = value;
            }
        } else if (window == kGpuMmioBase || window == kGpuMmioBase + 0x1000) {
            const u32 index = (addr - kGpuMmioBase) / 4;
            if (index < kExternalRegs) {
                external_[index] = value;
            } else if (index - kExternalRegs < kNumRegs) {
                processor_.WriteHardwareRegister(index - kExternalRegs, value);
            }
        } else {
            MH4N_LOG(Error, "PicaCore", "write to unknown GPU address {:#010x}", addr);
        }
    }

    u32 ReadReg(u32 addr) override {
        const u32 window = addr & 0xFFFFF000;
        if (window == kVaddrLcd) {
            const u32 index = (addr - kVaddrLcd) / 4;
            return index < kLcdRegs ? lcd_[index] : 0;
        }
        if (window == kGpuMmioBase || window == kGpuMmioBase + 0x1000) {
            const u32 index = (addr - kGpuMmioBase) / 4;
            if (index < kExternalRegs) {
                return external_[index];
            }
            if (index - kExternalRegs < kNumRegs) {
                return processor_.state().regs[index - kExternalRegs];
            }
        }
        return 0;
    }

    void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}

    // ---- GpuDebug ----------------------------------------------------------------------------
    void LoadInitState(const gpu::InitState& init) override {
        processor_.LoadInitState(init);
        const auto gpu_regs = init.Block(gpu::InitBlock::GpuRegs);
        if (gpu_regs.size() == kExternalRegs * 4) {
            std::memcpy(external_.data(), gpu_regs.data(), gpu_regs.size());
        }
        const auto lcd_regs = init.Block(gpu::InitBlock::LcdRegs);
        if (lcd_regs.size() == kLcdRegs * 4) {
            std::memcpy(lcd_.data(), lcd_regs.data(), lcd_regs.size());
        }
    }

    std::optional<std::vector<gpu::RegisterWrite>> TakeRegisterWrites() override {
        return processor_.TakeRegisterWrites();
    }

    std::vector<gpu::Counter> TakeFrameCounters() override {
        std::vector<gpu::Counter> counters = {
            {"pica_draws", backend_.draws - reported_draws_},
            {"pica_immediate_draws", backend_.immediate_draws - reported_immediate_},
            {"pica_cmdlists", processor_.command_lists_processed() - reported_lists_},
        };
        reported_draws_ = backend_.draws;
        reported_immediate_ = backend_.immediate_draws;
        reported_lists_ = processor_.command_lists_processed();
        return counters;
    }

private:
    gpu::Host& host_;
    CountingBackend backend_;
    CommandProcessor processor_;
    std::array<u32, kExternalRegs> external_{};
    std::array<u32, kLcdRegs> lcd_{};
    u64 reported_draws_ = 0;
    u64 reported_immediate_ = 0;
    u64 reported_lists_ = 0;
};

} // namespace

MH4N_REGISTER(gpu::GpuFactory, "pica-only",
              [](gpu::Host& host, const Options&) -> std::unique_ptr<gpu::Gpu> {
                  return std::make_unique<PicaOnlyGpu>(host);
              });

} // namespace mh4n::gpu::pica
