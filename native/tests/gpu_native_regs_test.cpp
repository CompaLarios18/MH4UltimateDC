// Derived from Azahar src/video_core/pica/pica_core.h (the position of `internal` inside
// PicaCore::Regs::reg_array) and src/video_core/gpu.cpp (GPU::WriteReg and GPU::ReadReg: the
// register windows and the command-buffer triggers) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; both say "Licensed under GPLv2 or any later version"): the expected values are
// worked out from that layout.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, gpu.cpp)
//
// `--gpu native` and the INIT record's registers (N2-40): the internal registers come from the tail
// of GPU_REGS (registers 0 to 0x31) and from PICA_REGS (the rest), and MMIO word 0x400 + n is
// internal register n. The Gpu is made without a device (it makes none before its first draw), so
// these cases run on a machine without Vulkan.
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "gpu/native/native_gpu.h"
#include "gpu/pica/regs.h"
#include "gpu_capture/capture_host.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/types.h"

using namespace mh4n;
using mh4n::gpu::InitBlock;
namespace native = mh4n::gpu::native;
namespace pica = mh4n::gpu::pica;

namespace {

constexpr u32 kMmio = 0x1EF00000;
constexpr u32 kFcram = 0x20000000;
using Bytes = std::vector<u8>;

void Set32(Bytes& b, size_t offset, u32 value) {
    REQUIRE(offset + 4 <= b.size());
    std::memcpy(b.data() + offset, &value, 4);
}

// The INIT record's blocks (mgpu-format.md section 4), zeroed.
struct InitBlocks {
    std::array<Bytes, gpu::kInitBlockCount + 1> data;

    InitBlocks() {
        const size_t sizes[] = {0, 4296, 4096, 3072, 34368, 34368, 256, 24576, 512, 3584, 284, 288};
        for (u32 kind = 1; kind <= gpu::kInitBlockCount; ++kind) {
            data[kind].assign(sizes[kind], 0);
        }
    }
    Bytes& operator[](InitBlock kind) {
        return data[static_cast<u32>(kind)];
    }
    gpu::InitState State() const {
        gpu::InitState state;
        for (u32 kind = 1; kind <= gpu::kInitBlockCount; ++kind) {
            state.blocks[kind] = std::span<const u8>(data[kind]);
        }
        return state;
    }
};

struct Rig {
    gpu_capture::CaptureHost host;
    std::unique_ptr<native::NativeGpu> gpu = native::MakeNativeGpu(host, native::NativeOptions{});

    u32 Mmio(u32 word) const {
        return kMmio + 4 * word;
    }
    // The internal register n, through MMIO.
    u32 Reg(u32 n) {
        return gpu->ReadReg(Mmio(0x400 + n));
    }
    // Puts a list of single-register writes in guest memory; returns its size in bytes.
    u32 Place(const std::vector<std::array<u32, 2>>& writes, u32 paddr) {
        Bytes bytes;
        for (const auto& w : writes) {
            for (const u32 word : {w[1], w[0] | (0xFu << 16)}) { // value, then the header
                bytes.resize(bytes.size() + 4);
                std::memcpy(bytes.data() + bytes.size() - 4, &word, 4);
            }
        }
        REQUIRE(host.WriteBoth(paddr, bytes.data(), static_cast<u32>(bytes.size())));
        return static_cast<u32>(bytes.size());
    }
    // The MMIO way to run a list: channel 0's address and size, then its trigger.
    void RunThroughMmio(const std::vector<std::array<u32, 2>>& writes) {
        const u32 size = Place(writes, kFcram + 0x1000);
        gpu->WriteReg(Mmio(0x400 + 0x23A), (kFcram + 0x1000) >> 3);
        gpu->WriteReg(Mmio(0x400 + 0x238), size >> 3);
        gpu->WriteReg(Mmio(0x400 + 0x23C), 1);
    }
};

} // namespace

TEST_CASE("native: LoadInitState puts internal register i at GPU_REGS word 0x400 + i below 0x32 and PICA_REGS word i - 0x32 above") {
    Rig rig;
    InitBlocks init;
    for (u32 word = 0; word < 0x432; ++word) {
        Set32(init[InitBlock::GpuRegs], 4 * word, 0xA0000000u + word);
    }
    for (u32 word = 0; word < 0x300; ++word) {
        Set32(init[InitBlock::PicaRegs], 4 * word, 0xB0000000u + 0x432 + word);
    }
    rig.gpu->LoadInitState(init.State());

    // (Read back through MMIO: this case alone would also pass if the INIT mapping and the MMIO
    // mapping were both off by the same 0x32 words; the MMIO cases below pin the MMIO mapping.)
    // Each marker names the word of reg_array it came from, which is 0x400 + the register number.
    u32 wrong = 0;
    for (u32 i = 0; i < pica::kNumRegs; ++i) {
        const u32 want = (i < 0x32 ? 0xA0000000u : 0xB0000000u) + 0x400 + i;
        wrong += rig.Reg(i) != want ? 1 : 0;
    }
    CHECK(wrong == 0);
    CHECK(rig.Reg(0x000) == 0xA0000400u);
    CHECK(rig.Reg(0x031) == 0xA0000431u);
    CHECK(rig.Reg(0x032) == 0xB0000432u);
    CHECK(rig.Reg(0x2FF) == 0xB0000432u + 0x2CD);
}

TEST_CASE("native: a 0x432-word GPU_REGS block still loads every external word below 0x400") {
    Rig rig;
    InitBlocks init;
    Set32(init[InitBlock::GpuRegs], 4 * 0x10, 0x1111);
    Set32(init[InitBlock::GpuRegs], 4 * 0x3FF, 0x3333);         // the last external word
    Set32(init[InitBlock::GpuRegs], 4 * 0x400, 0x4444);          // internal register 0, not an external word
    Set32(init[InitBlock::GpuRegs], 4 * 0x431, 0x5555);          // internal register 0x31
    rig.gpu->LoadInitState(init.State());
    // The external words come back through the GX engine's register window...
    CHECK(rig.gpu->ReadReg(rig.Mmio(0x10)) == 0x1111);
    CHECK(rig.gpu->ReadReg(rig.Mmio(0x3FF)) == 0x3333);
    // ...and from word 0x400 on the window is the command processor's.
    CHECK(rig.gpu->ReadReg(rig.Mmio(0x400)) == 0x4444);
    CHECK(rig.Reg(0x31) == 0x5555);
}

TEST_CASE("native: MMIO word 0x400 + n reads and writes internal register n") {
    Rig rig;
    rig.gpu->WriteReg(rig.Mmio(0x400 + 0x000), 0xA0);
    rig.gpu->WriteReg(rig.Mmio(0x400 + 0x2FF), 0xA1);
    rig.gpu->WriteReg(rig.Mmio(0x400 + 0x120), 0xA2);
    rig.gpu->WriteReg(rig.Mmio(0x400 - 1), 0xA3);   // an external word
    rig.gpu->WriteReg(rig.Mmio(0x400 + 0x300), 0xA4); // past the registers: ignored
    CHECK(rig.Reg(0x000) == 0xA0);
    CHECK(rig.Reg(0x2FF) == 0xA1);
    CHECK(rig.Reg(0x120) == 0xA2);
    CHECK(rig.gpu->ReadReg(rig.Mmio(0x432 + 0x120)) == 0); // register 0x152, which nothing wrote
    CHECK(rig.gpu->ReadReg(rig.Mmio(0x400 - 1)) == 0xA3);
    CHECK(rig.gpu->ReadReg(rig.Mmio(0x400 + 0x300)) == 0);
}

TEST_CASE("native: an MMIO write to the command-buffer trigger at word 0x400 + 0x23C runs the list") {
    Rig rig;
    rig.RunThroughMmio({{0x120, 5}, {0x121, 6}});
    CHECK(rig.Reg(0x120) == 5);
    CHECK(rig.Reg(0x121) == 6);
    CHECK(rig.Reg(0x23C) == 0); // the trigger was cleared
}

TEST_CASE("native: a finalize that matches the compare value in GPU_REGS signals P3D once and stops, with autostop in PICA_REGS") {
    // Register 0x020 is GPU_REGS word 0x400 + 0x20; register 0x034 is PICA_REGS word 2 (the rule
    // crosses its boundary there). PICA_REGS word 0x34 is zero: the old reading had autostop off.
    Rig rig;
    InitBlocks init;
    Set32(init[InitBlock::GpuRegs], 4 * (0x400 + 0x20), 0x11223344);
    Set32(init[InitBlock::PicaRegs], 4 * (0x34 - 0x32), 1);
    rig.gpu->LoadInitState(init.State());
    CHECK(rig.Reg(0x020) == 0x11223344);
    CHECK(rig.Reg(0x034) == 1);

    rig.RunThroughMmio({{0x120, 1}, {0x010, 0x11223344}, {0x121, 2}});
    CHECK(rig.Reg(0x120) == 1);
    CHECK(rig.Reg(0x121) == 0); // the list stopped at the finalize
    const auto irqs = rig.host.TakeSignalled();
    REQUIRE(irqs.size() == 1);
    CHECK(irqs[0].id == gpu::Interrupt::P3D);

    // A value that matches no byte of the compare value: nothing is signalled, the list goes on.
    rig.RunThroughMmio({{0x010, 0x55667788}, {0x121, 2}});
    CHECK(rig.Reg(0x121) == 2);
    CHECK(rig.host.TakeSignalled().empty());
}
