// Derived from Azahar src/video_core/pica/pica_core.h and pica_core.cpp (the declarations and
// names of the register-write handlers, the draw functions and the command list), and
// src/video_core/gpu.cpp (GPU::SubmitCmdList, GPU::WriteReg) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of
// those files says "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, pica_core.cpp,
//   gpu.cpp)
//
// The PICA command processor (N2-04): decodes command lists, keeps the register file and the
// state behind it, applies every side effect of a register write as Azahar's PicaCore does, and
// hands each draw to a Backend.
//
// It runs on the renderer's thread (principle 6): only that thread calls it. Guest memory is read
// through gpu::Host.
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "gpu/pica/backend.h"
#include "gpu/pica/state.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"

namespace mh4n::gpu::pica {

class CommandProcessor {
public:
    CommandProcessor(gpu::Host& host, Backend& backend);
    ~CommandProcessor();
    CommandProcessor(const CommandProcessor&) = delete;
    CommandProcessor& operator=(const CommandProcessor&) = delete;

    // The state at the start of a capture, from the INIT record's blocks (mgpu-format.md
    // section 4): the internal registers, both shader setups, the default attributes, the three
    // look-up tables, the immediate-mode state and the geometry pipeline. A block of the wrong
    // size is logged and skipped. Everything is dirty afterwards.
    void LoadInitState(const gpu::InitState& init);

    // PicaCore::ProcessCmdList: runs the command list of `size` bytes at physical address
    // `paddr`, with the registers as they are.
    void ProcessCmdList(u32 paddr, u32 size);

    // GPU::SubmitCmdList: runs the list the command-buffer registers of channel `index` (0 or 1)
    // name, if the channel's trigger register is set, then clears the trigger.
    void SubmitCmdList(u32 index);

    // The GX command SubmitCmdList: writes channel 0's address, size and trigger registers and
    // submits it (GPU::Execute). `paddr` is a physical address.
    void SubmitGxCmdList(u32 paddr, u32 size);

    // GPU::WriteReg to an internal register (the MMIO address minus kGpuMmioBase, in words,
    // minus kExternalRegs): the value is stored as it is, without the side effects a command-list
    // write has, and a write to a command-buffer trigger submits that channel. (Azahar does not
    // mark the register dirty; here it is.)
    void WriteHardwareRegister(u32 id, u32 value);

    // The decoded register writes (one entry per register word the command lists wrote, in
    // order, as the PICA_REG records list them) are only kept while this is on.
    void SetRegisterWriteLog(bool enabled);
    std::vector<gpu::RegisterWrite> TakeRegisterWrites();

    const PicaState& state() const {
        return *state_;
    }
    u32 draws_issued() const {
        return sequence_;
    }
    u32 command_lists_processed() const {
        return lists_;
    }

private:
    struct CommandList {
        u32 paddr = 0;
        u32 length = 0; // words
        u32 index = 0;  // the next word
        const u8* head = nullptr;
    };

    // The word `index` of the current list. Words past the end of the list (Azahar reads them
    // from guest memory too) and lists that are not contiguous in host memory are read one word
    // at a time; an unmapped word reads as 0.
    u32 ListWord(u32 index) const;
    void ResetList(u32 paddr, u32 size);

    // pica_core.cpp's handlers, in the order they are called from the command list loop.
    void WriteInternalReg(u32 id, u32 value, u32 mask, bool& stop_requested);
    void WriteInternalRegBatch(u32 id, std::span<const u32> values, u32 mask, bool& stop_requested);
    void WriteInternalRegSequential(u32 id, std::span<const u32> values, u32 mask,
                                    bool& stop_requested);
    void HandleSpecialReg(u32 id, u32 value, bool& stop_requested);
    void HandleSpecialRegBatch(u32 id, std::span<const u32> values);

    bool GsMirrorsVs() const;
    void SubmitImmediate(u32 word);
    void DrawImmediate();
    void DrawArrays(bool is_indexed);
    void ReconfigureGeometryPipeline();
    void IssueDraw(DrawCall& call);

    void LogWrite(u32 id, u32 mask, u32 value);

    gpu::Host& host_;
    Backend& backend_;
    std::unique_ptr<PicaState> state_;
    DelayGenerator delay_;
    CommandList list_;
    bool log_writes_ = false;
    std::vector<gpu::RegisterWrite> writes_;
    u32 sequence_ = 0;
    u32 lists_ = 0;
};

} // namespace mh4n::gpu::pica
