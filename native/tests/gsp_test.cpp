// Derived from Azahar src/core/hle/service/gsp/gsp_gpu.cpp, src/core/hle/service/gsp/gsp_gpu.h,
// src/core/hle/service/gsp/gsp_command.h, src/core/hle/service/gsp/gsp_interrupt.h,
// src/core/hle/service/gsp/gsp_lcd.cpp, src/video_core/gpu.cpp, src/video_core/gpu.h,
// src/video_core/pica/pica_core.cpp, src/video_core/pica/pica_core.h,
// src/video_core/pica/regs_external.h, src/video_core/pica/regs_internal.h, src/core/memory.cpp and
// src/core/core_timing.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them
// says "GPLv2 or any later version"): the expected values, worked out by hand from the same
// algorithms: the headers and result codes of the gsp::Gpu commands, the layout of the interrupt
// relay queue, the framebuffer updates and the GX command buffer, the order of the register-write
// checks, the delays of the DelayGenerator (the speed table, for the sizes the tests use), the
// interrupts each GX command signals, the command-list finalize rule, the VBlank length and what a
// DMA between mapped ranges copies (MemorySystem::CopyBlock).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (gsp_gpu.cpp, gsp_gpu.h)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h, regs_external.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gsp_interrupt.h, gpu.cpp,
//   gpu.h, pica_core.cpp, pica_core.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.cpp)
// Copyright 2015 Citra Emulator Project (gsp_lcd.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
//
// The gsp::Gpu service and the null GPU (N1-17). The null GPU is tested on its own against a fake
// host. The service is tested through the SVC layer: the guest connects to gsp::Gpu by name and
// sends requests as the game does, from a main thread that runs a C++ function
// (Runtime::SetMainThreadEntry); time is a ManualTimeSource, so the VBlank cadence is exact.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("gsp_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gpu_null/null_gpu.h"
#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"
#include "../src/services/gsp/gsp.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

// ===================================================================================================
// The null GPU on its own
// ===================================================================================================

constexpr u32 kFcramPaddr = 0x20000000;
constexpr u32 kVramPaddr = 0x18000000;
constexpr u32 kLinearVaddr = 0x14000000;  // FCRAM
constexpr u32 kVramVaddr = 0x1F000000;
constexpr u32 kArena = 0x100000;  // the part of FCRAM and of VRAM the fake host has

// A gpu::Host with 1 MiB of FCRAM and of VRAM, which records the interrupts.
class FakeHost : public gpu::Host {
public:
    struct Signal {
        gpu::Interrupt id;
        u64 delay_ns;
    };

    std::vector<u8> fcram = std::vector<u8>(kArena);
    std::vector<u8> vram = std::vector<u8>(kArena);
    std::vector<Signal> signals;

    u8* PhysicalPointer(u32 paddr, u32 size) override {
        auto in = [&](u32 base, std::vector<u8>& arena) -> u8* {
            if (paddr >= base && static_cast<u64>(paddr) + size <= static_cast<u64>(base) + kArena) {
                return arena.data() + (paddr - base);
            }
            return nullptr;
        };
        if (u8* p = in(kFcramPaddr, fcram)) {
            return p;
        }
        return in(kVramPaddr, vram);
    }
    std::optional<u32> VirtualToPhysical(u32 vaddr) override {
        if (vaddr >= kVramVaddr && vaddr <= kVramVaddr + 0x600000) {
            return vaddr - kVramVaddr + kVramPaddr;
        }
        if (vaddr >= kLinearVaddr && vaddr <= kLinearVaddr + 0x08000000) {
            return vaddr - kLinearVaddr + kFcramPaddr;
        }
        return std::nullopt;
    }
    void SignalInterrupt(gpu::Interrupt id, u64 delay_ns) override { signals.push_back({id, delay_ns}); }
    void Post(std::function<void()> work) override { work(); }

    // The interrupts in order, without their delays.
    std::vector<gpu::Interrupt> Ids() const {
        std::vector<gpu::Interrupt> ids;
        for (const Signal& s : signals) {
            ids.push_back(s.id);
        }
        return ids;
    }
    u8* Fcram(u32 paddr_offset) { return fcram.data() + paddr_offset; }
    u8* Vram(u32 paddr_offset) { return vram.data() + paddr_offset; }
};

constexpr u32 kHeapVaddr = 0x08000000;   // the application's heap: guest memory with no physical view
constexpr u32 kOtherVaddr = 0x0C000000;  // more of the same, for a copy within guest memory

// A host that also serves guest memory (Host::GuestPointer), as the runtime's does: the two
// guest-only ranges above, and the fake's FCRAM and VRAM at their virtual addresses.
class GuestFakeHost final : public FakeHost {
public:
    std::vector<u8> heap = std::vector<u8>(kArena);
    std::vector<u8> other = std::vector<u8>(kArena);
    int guest_pointer_calls = 0;

    u8* GuestPointer(u32 vaddr, u32 size) override {
        ++guest_pointer_calls;
        auto in = [&](u32 base, std::vector<u8>& arena) -> u8* {
            if (vaddr >= base && static_cast<u64>(vaddr) + size <= static_cast<u64>(base) + kArena) {
                return arena.data() + (vaddr - base);
            }
            return nullptr;
        };
        struct Range {
            u32 base;
            std::vector<u8>* arena;
        };
        const Range ranges[] = {{kHeapVaddr, &heap},
                                {kOtherVaddr, &other},
                                {kLinearVaddr, &fcram},
                                {kVramVaddr, &vram}};
        for (const Range& range : ranges) {
            if (u8* p = in(range.base, *range.arena)) {
                return p;
            }
        }
        return nullptr;
    }
};

gpu::GxCommand Gx(std::initializer_list<u32> words) {
    gpu::GxCommand command{};
    std::copy(words.begin(), words.end(), command.words);
    return command;
}

using gpu::Interrupt;

template <class HostType>
struct BasicNullFixture {
    HostType host;
    gpu::null::NullGpu gpu{host};
    std::vector<std::string> logged;
    BasicNullFixture() {
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { logged.push_back(std::string(r.message)); });
    }
    ~BasicNullFixture() {
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    bool Logged(const std::string& needle) const {
        for (const std::string& line : logged) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};


using NullFixture = BasicNullFixture<FakeHost>;

TEST_CASE("null GPU: the registry has it as 'null'") {
    CHECK(Registry<gpu::GpuFactory>::Instance().Find("null") != nullptr);
}

TEST_CASE("null GPU: a memory fill fills, signals PSC0 and sets the finished bit") {
    NullFixture f;
    // start1 value1 end1 start2 value2 end2 control1 | control2 << 16; control 0x201 = trigger, 32 bits
    f.gpu.Execute(Gx({2, kVramVaddr, 0xAABBCCDD, kVramVaddr + 0x1000, 0, 0, 0, 0x0201}));
    for (u32 i = 0; i < 0x1000; i += 4) {
        const u32 word = *reinterpret_cast<u32*>(f.host.Vram(i));
        REQUIRE(word == 0xAABBCCDD);
    }
    CHECK(*f.host.Vram(0x1000) == 0);  // nothing beyond the end
    REQUIRE(f.host.signals.size() == 1);
    CHECK(f.host.signals[0].id == Interrupt::PSC0);
    // 0x1000 bytes from VRAM to VRAM at the texture speed 5400 MiB/s: 4096 * (1e9 / (5400 * 2^20))
    CHECK(f.gpu.GetStats().last_delay_ns[0] == 723);
    // the register file: trigger cleared, finished set, address >> 3
    CHECK(f.gpu.GpuRegister(4 + 3) == 0x202);
    CHECK(f.gpu.GpuRegister(4 + 0) == 0x18000000 >> 3);
    CHECK(f.gpu.GpuRegister(4 + 1) == 0x18001000 >> 3);
    CHECK(f.gpu.GpuRegister(4 + 2) == 0xAABBCCDD);
}

TEST_CASE("null GPU: a fill of the second buffer alone signals PSC1; of both, PSC0 once") {
    NullFixture f;
    f.gpu.Execute(Gx({2, 0, 0, 0, kVramVaddr, 0x11223344, kVramVaddr + 0x1000, 0x0201u << 16}));
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::PSC1});
    CHECK(f.host.Vram(0)[0] == 0x44);

    f.host.signals.clear();
    // both set: "GSP dispatches PSC0 only", for the second unit's finish
    f.gpu.Execute(Gx({2, kVramVaddr, 0x01020304, kVramVaddr + 0x100, kLinearVaddr, 0x05060708,
                      kLinearVaddr + 0x100, 0x0201u | (0x0201u << 16)}));
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::PSC0});
    CHECK(f.host.Vram(0)[0] == 0x04);
    CHECK(f.host.Fcram(0)[0] == 0x08);
    // both units finished
    CHECK(f.gpu.GpuRegister(4 + 3) == 0x202);
    CHECK(f.gpu.GpuRegister(8 + 3) == 0x202);
}

TEST_CASE("null GPU: a fill through the registers signals nothing when its start address is 0") {
    NullFixture f;
    // MemoryFillConfig[0] at 0x1EF00010: start, end, value, control (the trigger starts it)
    f.gpu.WriteReg(0x1EF00010, 0);
    f.gpu.WriteReg(0x1EF00014, 0);
    f.gpu.WriteReg(0x1EF00018, 0x1234);
    f.gpu.WriteReg(0x1EF0001C, 0x201);
    CHECK(f.host.signals.empty());
    // "confirmed on hardware even if address_start is zero"
    CHECK(f.gpu.GpuRegister(4 + 3) == 0x202);
    // with an address it signals, and writing the trigger of the second unit signals PSC1
    f.gpu.WriteReg(0x1EF00020, kVramPaddr >> 3);
    f.gpu.WriteReg(0x1EF00024, (kVramPaddr + 0x20) >> 3);
    f.gpu.WriteReg(0x1EF00028, 0xABCD);
    f.gpu.WriteReg(0x1EF0002C, 0x001);
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::PSC1});
    CHECK(f.host.Vram(0)[0] == 0xCD);
    CHECK(f.host.Vram(0)[1] == 0xAB);
}

TEST_CASE("null GPU: DMA copies and signals DMA with the delay of its copy mode") {
    NullFixture f;
    for (u32 i = 0; i < 0x2000; ++i) {
        f.host.fcram[i] = static_cast<u8>(i * 7);
        f.host.vram[0x4000 + i] = static_cast<u8>(i * 11);
    }
    // RAM to RAM: 0x1000 bytes at 190 MiB/s
    f.gpu.Execute(Gx({0, kLinearVaddr, kLinearVaddr + 0x8000, 0x1000}));
    CHECK(std::equal(f.host.Fcram(0), f.host.Fcram(0x1000), f.host.Fcram(0x8000)));
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 20559);
    // RAM to VRAM: 0x2000 bytes at 310 MiB/s
    f.gpu.Execute(Gx({0, kLinearVaddr, kVramVaddr + 0x10000, 0x2000}));
    CHECK(std::equal(f.host.Fcram(0), f.host.Fcram(0x2000), f.host.Vram(0x10000)));
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 25201);
    // VRAM to RAM: 0x400 bytes at 380 MiB/s
    f.gpu.Execute(Gx({0, kVramVaddr + 0x4000, kLinearVaddr + 0x20000, 0x400}));
    CHECK(std::equal(f.host.Vram(0x4000), f.host.Vram(0x4400), f.host.Fcram(0x20000)));
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 2569);
    // VRAM to VRAM: 0x800 bytes at 380 MiB/s
    f.gpu.Execute(Gx({0, kVramVaddr + 0x4000, kVramVaddr + 0x20000, 0x800}));
    CHECK(std::equal(f.host.Vram(0x4000), f.host.Vram(0x4800), f.host.Vram(0x20000)));
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 5139);
    CHECK(f.host.Ids() == std::vector<Interrupt>(4, Interrupt::DMA));
    CHECK(f.gpu.GetStats().commands[0] == 4);
}

TEST_CASE("null GPU: a DMA to memory the host does not know copies nothing and still signals DMA") {
    NullFixture f;
    f.gpu.Execute(Gx({0, 0x08000000, kLinearVaddr, 0x100}));  // the application's heap
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::DMA});
    CHECK(f.Logged("DMA with an address that is not physical memory"));
}

TEST_CASE("null GPU: display transfers and texture copies draw nothing and signal PPF") {
    NullFixture f;
    std::fill(f.host.fcram.begin(), f.host.fcram.begin() + 0x4000, 0x5A);
    std::fill(f.host.vram.begin(), f.host.vram.begin() + 0x4000, 0xC3);
    // in out in_size out_size flags: 64 x 32 pixels of RGBA8 from VRAM to FCRAM
    f.gpu.Execute(Gx({3, kVramVaddr, kLinearVaddr, 64u | (32u << 16), 64u | (32u << 16), 0}));
    CHECK(f.host.fcram[0] == 0x5A);  // the destination is untouched
    CHECK(f.host.fcram[0x1FFF] == 0x5A);
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::PPF});
    // 64 * 32 * 4 = 8192 bytes, VRAM to RAM at 5400 MiB/s
    CHECK(f.gpu.GetStats().last_delay_ns[4] == 1446);
    CHECK(f.gpu.GpuRegister(0x300 + 6) == 0);  // the trigger is cleared
    CHECK(f.gpu.GpuRegister(0x300 + 0) == kVramPaddr >> 3);
    CHECK(f.gpu.GpuRegister(0x300 + 1) == kFcramPaddr >> 3);
    CHECK(f.gpu.GetStats().skipped_transfers == 1);

    // texture copy: in out size in_gap out_gap flags (bit 3: texture copy); RAM to RAM, 0x400 bytes
    // at 450 MiB/s
    f.host.signals.clear();
    f.gpu.Execute(Gx({4, kLinearVaddr + 0x1000, kLinearVaddr + 0x2000, 0x400, 0, 0, 8}));
    CHECK(f.host.fcram[0x2000] == 0x5A);
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::PPF});
    CHECK(f.gpu.GetStats().last_delay_ns[4] == 2170);
    CHECK(f.gpu.GetStats().skipped_transfers == 2);
}

TEST_CASE("null GPU: a cache flush does nothing, an unknown command is counted") {
    NullFixture f;
    f.gpu.Execute(Gx({5, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(f.host.signals.empty());
    f.gpu.Execute(Gx({0x77, 1, 2, 3}));
    CHECK(f.gpu.GetStats().unknown_commands == 1);
    CHECK(f.host.signals.empty());
    CHECK(f.Logged("Unknown command"));
}

TEST_CASE("null GPU: the delay Azahar's GPU passes is kept and handed to the host (D41); 0 on request") {
    NullFixture f;
    CHECK(f.gpu.PassDelays());
    f.gpu.Execute(Gx({0, kLinearVaddr, kLinearVaddr + 0x8000, 0x1000}));
    REQUIRE(f.host.signals.size() == 1);
    CHECK(f.host.signals[0].delay_ns == 20559);  // the host ignores it for the timing; GetPerfLog records it
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 20559);
    f.gpu.SetPassDelays(false);
    CHECK_FALSE(f.gpu.PassDelays());
    f.gpu.Execute(Gx({0, kLinearVaddr, kLinearVaddr + 0x8000, 0x1000}));
    REQUIRE(f.host.signals.size() == 2);
    CHECK(f.host.signals[1].delay_ns == 0);
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 20559);  // still computed
}

// ---- command lists ------------------------------------------------------------------------------

// A command list in FCRAM at physical 0x20001000: pairs of (value, header), padded as Azahar's
// lists are.
class ListBuilder {
public:
    // header: id bits 0-15, mask 16-19, extra words 20-30, group bit 31
    ListBuilder& Write(u32 id, u32 value, u32 mask = 0xF) {
        words_.push_back(value);
        words_.push_back(id | (mask << 16));
        return *this;
    }
    // A write with `extra` after it: the same register again for each (group false), or the
    // registers after it (group true). The data is padded to an even number of words.
    ListBuilder& WriteMany(u32 id, u32 value, std::vector<u32> extra, bool group, u32 mask = 0xF) {
        words_.push_back(value);
        words_.push_back(id | (mask << 16) | (static_cast<u32>(extra.size()) << 20) |
                         (group ? 0x80000000u : 0));
        words_.insert(words_.end(), extra.begin(), extra.end());
        if (words_.size() % 2 != 0) {
            words_.push_back(0);
        }
        return *this;
    }
    ListBuilder& Finalize(u32 value = 0x12345678) { return Write(0x10, value); }
    u32 Size() const { return static_cast<u32>(words_.size() * 4); }
    void Store(FakeHost& host, u32 offset = 0x1000) const {
        std::memcpy(host.Fcram(offset), words_.data(), words_.size() * 4);
    }

private:
    std::vector<u32> words_;
};

// Runs `list` through the GX command SubmitCmdList and returns the interrupts it signalled.
std::vector<Interrupt> RunList(NullFixture& f, const ListBuilder& list) {
    f.host.signals.clear();
    list.Store(f.host);
    f.gpu.Execute(Gx({1, kLinearVaddr + 0x1000, list.Size(), 0, 0, 0, 0, 1}));
    return f.host.Ids();
}

TEST_CASE("null GPU: a command list signals P3D for a finalize that matches irq_compare") {
    NullFixture f;
    // the default compare value is 0x12345678 (PicaCore::InitializeRegs)
    CHECK(f.gpu.GpuRegister(0x400 + 0x20) == 0x12345678);
    CHECK(f.gpu.GpuRegister(0x400 + 0x34) == 1);           // irq_autostop
    CHECK(f.gpu.GpuRegister(0x400 + 0x30) == 0xFFFFFFF0);  // irq_mask

    CHECK(RunList(f, ListBuilder().Write(0x40, 1).Finalize()) == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.gpu.GpuRegister(0x400 + 0x10) == 0x12345678);
    // the trigger is cleared and the registers hold the list
    CHECK(f.gpu.GpuRegister(0x400 + 0x23C) == 0);
    CHECK(f.gpu.GpuRegister(0x400 + 0x23A) == (kFcramPaddr + 0x1000) >> 3);
    CHECK(f.gpu.GpuRegister(0x400 + 0x238) == ListBuilder().Write(0x40, 1).Finalize().Size() >> 3);
    CHECK(f.gpu.GetStats().command_lists == 1);

    // no byte equal to the compare value's: nothing
    CHECK(RunList(f, ListBuilder().Finalize(0xAAAAAAAA)).empty());
    // one byte equal is enough ("any_byte_match")
    CHECK(RunList(f, ListBuilder().Finalize(0xFFFFFF78)) == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(RunList(f, ListBuilder().Finalize(0x12FFFFFF)) == std::vector<Interrupt>{Interrupt::P3D});
    // the bytes are compared in place: the right bytes in the wrong places do not match
    CHECK(RunList(f, ListBuilder().Finalize(0x78563412)).empty());
    // a list with no finalize signals nothing
    CHECK(RunList(f, ListBuilder().Write(0x40, 1).Write(0x41, 2)).empty());
    // The write mask: only the bytes it names are written. A write of no bytes leaves the register
    // as it was (0xAAAAAAAA here, which does not match), one byte can complete a match.
    CHECK(RunList(f, ListBuilder().Finalize(0xAAAAAAAA)).empty());
    CHECK(RunList(f, ListBuilder().Write(0x10, 0x78787878, 0x0)).empty());
    CHECK(f.gpu.GpuRegister(0x400 + 0x10) == 0xAAAAAAAA);
    CHECK(RunList(f, ListBuilder().Write(0x10, 0x00000078, 0x1)) == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.gpu.GpuRegister(0x400 + 0x10) == 0xAAAAAA78);
}

TEST_CASE("null GPU: irq_autostop ends a list at its first P3D; without it every finalize counts") {
    NullFixture f;
    CHECK(RunList(f, ListBuilder().Finalize().Write(0x40, 1).Finalize()) ==
          std::vector<Interrupt>{Interrupt::P3D});
    // the list writes irq_autostop = 0 (register 0x34), then two finalizes
    CHECK(RunList(f, ListBuilder().Write(0x34, 0).Finalize().Finalize()) ==
          std::vector<Interrupt>(2, Interrupt::P3D));
    // it stays off for the next list, as a register does
    CHECK(RunList(f, ListBuilder().Finalize().Finalize().Finalize()) ==
          std::vector<Interrupt>(3, Interrupt::P3D));
    // a different compare value, written by the list
    CHECK(RunList(f, ListBuilder().Write(0x20, 0xCAFE0000).Finalize()).empty());
    CHECK(RunList(f, ListBuilder().Finalize(0xCAFE0000)) == std::vector<Interrupt>{Interrupt::P3D});
    // and back through the MMIO window (0x1EF01000 + 4 * 0x34)
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x34, 1);
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x20, 0x12345678);
    CHECK(RunList(f, ListBuilder().Finalize().Finalize()) == std::vector<Interrupt>{Interrupt::P3D});
}

TEST_CASE("null GPU: commands with extra data keep the list's alignment") {
    NullFixture f;
    // register 0x2C0 written with three more words in a row (a shader upload): value, header and
    // three words, then a pad; the finalize after them has to be found
    const ListBuilder upload = ListBuilder().WriteMany(0x2C0, 1, {2, 3, 4}, false).Finalize();
    CHECK(RunList(f, upload) == std::vector<Interrupt>{Interrupt::P3D});
    // a grouped write covering registers 0x41..0x43, then a finalize
    CHECK(RunList(f, ListBuilder().WriteMany(0x40, 9, {1, 2, 3}, true).Finalize()) ==
          std::vector<Interrupt>{Interrupt::P3D});
    // an odd number of extra words (one) is padded
    CHECK(RunList(f, ListBuilder().WriteMany(0x40, 9, {1}, false).Finalize()) ==
          std::vector<Interrupt>{Interrupt::P3D});
}

TEST_CASE("null GPU: a finalize in a batch or a group is handled as Azahar's slow path does") {
    NullFixture f;
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x34, 0);  // no autostop, so every match counts
    // The same register written 1 + 2 times: the first write is a finalize of its own, then each of
    // the two extra values is handled with the register's final value (WriteInternalRegBatch).
    CHECK(RunList(f, ListBuilder().WriteMany(0x10, 0x12345678, {0x12345678, 0x00000078}, false)) ==
          std::vector<Interrupt>(3, Interrupt::P3D));
    // The batch stores only the last value: it does not match, so the extras are not finalizes.
    CHECK(RunList(f, ListBuilder().WriteMany(0x10, 0x12345678, {0x12345678, 0xAAAAAAAA}, false)) ==
          std::vector<Interrupt>{Interrupt::P3D});
    // A grouped write that reaches 0x10 from 0x0F: register 0x10 is the second word.
    CHECK(RunList(f, ListBuilder().WriteMany(0x0F, 0, {0x11111111, 0x12345678}, true)) ==
          std::vector<Interrupt>{});  // 0x10 gets 0x11111111, 0x11 gets the match: only 0x10 is special
    CHECK(RunList(f, ListBuilder().WriteMany(0x0F, 0, {0x12345678, 0x11111111}, true)) ==
          std::vector<Interrupt>{Interrupt::P3D});
    // with autostop on, the batch stops at the first match
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x34, 1);
    CHECK(RunList(f, ListBuilder().WriteMany(0x10, 0x12345678, {0x12345678, 0x12345678}, false)) ==
          std::vector<Interrupt>{Interrupt::P3D});
}

TEST_CASE("null GPU: a list can also be started through the command buffer registers") {
    NullFixture f;
    f.host.signals.clear();
    const ListBuilder list = ListBuilder().Finalize();
    list.Store(f.host);
    // channel 1: size at 0x239, address at 0x23B, trigger at 0x23D (registers of 0x1EF01000)
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x239, list.Size() >> 3);
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x23B, (kFcramPaddr + 0x1000) >> 3);
    CHECK(f.host.signals.empty());  // nothing starts before the trigger
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x23D, 1);
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.gpu.GpuRegister(0x400 + 0x23D) == 0);
    // channel 0 as well
    f.host.signals.clear();
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x238, list.Size() >> 3);
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x23A, (kFcramPaddr + 0x1000) >> 3);
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x23C, 1);
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::P3D});
}

// ---- jumps: writes to the command buffer trigger registers inside a list ------------------------------

constexpr u32 kListA = 0x1000;  // the list the GX command submits (RunList)
constexpr u32 kListB = 0x3000;  // the list it jumps to

// Appends the three writes that make `channel` describe a list and start it: size (bytes / 8), address
// (physical / 8), trigger (registers 0x238 + c, 0x23A + c, 0x23C + c).
ListBuilder& Jump(ListBuilder& list, u32 channel, u32 target_paddr, u32 target_size) {
    return list.Write(0x238 + channel, target_size >> 3)
        .Write(0x23A + channel, target_paddr >> 3)
        .Write(0x23C + channel, 1);
}

// Runs list A (at kListA) with list B stored at kListB.
std::vector<Interrupt> RunJump(NullFixture& f, const ListBuilder& a, const ListBuilder& b) {
    b.Store(f.host, kListB);
    return RunList(f, a);
}

TEST_CASE("null GPU: a jump through the trigger registers continues in the target list and drops the rest") {
    NullFixture f;
    const ListBuilder b = ListBuilder().Write(0x40, 7).Finalize();
    // Autostop is off, so a finalize that is read is a P3D that is signalled: the one after the jump
    // belongs to the list that was left (Azahar's CommandList::Reset drops it), the one in B counts.
    for (const u32 channel : {0u, 1u}) {
        ListBuilder a;
        a.Write(0x34, 0);
        Jump(a, channel, kFcramPaddr + kListB, b.Size());
        a.Finalize();
        CHECK(RunJump(f, a, b) == std::vector<Interrupt>{Interrupt::P3D});
        // B was read: its write to register 0x40 reached the register file
        CHECK(f.gpu.GpuRegister(0x400 + 0x40) == 7);
        f.gpu.WriteReg(0x1EF01000 + 4 * 0x40, 0);
    }
    CHECK(f.gpu.GetStats().command_list_jumps == 2);
    CHECK(f.gpu.GetStats().command_lists == 2);
    // B's own P3D with autostop on, as the first list would have it
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x34, 1);
    ListBuilder a;
    Jump(a, 1, kFcramPaddr + kListB, b.Size());
    a.Finalize();
    CHECK(RunJump(f, a, b) == std::vector<Interrupt>{Interrupt::P3D});
}

TEST_CASE("null GPU: a jump as the last command of a list still runs the target; a target of no commands ends the scan") {
    NullFixture f;
    // the last command of A is the trigger write, with nothing after it
    ListBuilder a;
    Jump(a, 0, kFcramPaddr + kListB, ListBuilder().Finalize().Size());
    CHECK(RunJump(f, a, ListBuilder().Finalize()) == std::vector<Interrupt>{Interrupt::P3D});
    // inside a list, a size of 0 is a list of no commands: nothing is signalled and the finalize
    // after the jump is not read
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x34, 0);
    ListBuilder empty_target;
    Jump(empty_target, 0, kFcramPaddr + kListB, 0);
    empty_target.Finalize();
    CHECK(RunJump(f, empty_target, ListBuilder().Finalize()).empty());
}

TEST_CASE("null GPU: a jump to a list that is not in physical memory counts as its finalize and ends the scan") {
    NullFixture f;
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x34, 0);  // the finalize after the jump would show up
    ListBuilder a;
    Jump(a, 1, kFcramPaddr + 0x200000, 0x100);  // past the 1 MiB the fake host has
    a.Finalize();
    CHECK(RunJump(f, a, ListBuilder()) == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.Logged("command list jump to 0X20200000"));
    CHECK(f.Logged("is not in physical memory"));
    CHECK(f.gpu.GetStats().command_list_jumps == 1);
}

TEST_CASE("null GPU: a chain of lists counts the P3D of each list it reaches") {
    NullFixture f;
    // A jumps to B, B writes its own registers and jumps to C, C ends with a finalize
    constexpr u32 kListC = 0x5000;
    const ListBuilder c = ListBuilder().Write(0x41, 3).Finalize();
    ListBuilder b;
    Jump(b, 1, kFcramPaddr + kListC, c.Size());
    b.Finalize();  // dropped
    c.Store(f.host, kListC);
    ListBuilder a;
    a.Write(0x34, 0);
    Jump(a, 0, kFcramPaddr + kListB, b.Size());
    CHECK(RunJump(f, a, b) == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.gpu.GpuRegister(0x400 + 0x41) == 3);
    CHECK(f.gpu.GetStats().command_list_jumps == 2);
    // With autostop on, the finalize before the jump stops the scan: the jump is not reached (B's
    // finalize would be a second P3D)
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x34, 1);
    const u64 jumps = f.gpu.GetStats().command_list_jumps;
    ListBuilder first;
    first.Finalize();
    Jump(first, 0, kFcramPaddr + kListB, b.Size());
    CHECK(RunJump(f, first, b) == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.gpu.GetStats().command_list_jumps == jumps);
}

TEST_CASE("null GPU: a jump inside a grouped write leaves the other values to be written, as Azahar's batch does") {
    NullFixture f;
    f.gpu.WriteReg(0x1EF01000 + 4 * 0x34, 0);
    constexpr u32 kListC = 0x5000;
    const ListBuilder b = ListBuilder().Write(0x41, 5).Finalize();
    const ListBuilder c = ListBuilder().Write(0x42, 9).Finalize();
    b.Store(f.host, kListB);
    c.Store(f.host, kListC);
    // Channel 0 describes B and channel 1 describes C. One grouped write then covers the registers
    // from 0x23C: the trigger of channel 0 (a jump to B) and the trigger of channel 1 (a jump to C).
    // Both only reposition the scan (CommandList::Reset): the second wins before anything of B is
    // read, and the finalize after the group belongs to a list that has been left.
    ListBuilder a;
    a.Write(0x238, b.Size() >> 3).Write(0x23A, (kFcramPaddr + kListB) >> 3);
    a.Write(0x239, c.Size() >> 3).Write(0x23B, (kFcramPaddr + kListC) >> 3);
    a.WriteMany(0x23B, (kFcramPaddr + kListC) >> 3, {1, 0x77}, true);
    a.Finalize();
    CHECK(RunList(f, a) == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.gpu.GpuRegister(0x400 + 0x41) == 0);  // B was never read
    CHECK(f.gpu.GpuRegister(0x400 + 0x42) == 9);  // C was
    CHECK(f.gpu.GpuRegister(0x400 + 0x23D) == 0x77);
    CHECK(f.gpu.GetStats().command_list_jumps == 2);
}

TEST_CASE("null GPU: a trigger written with extra data takes the data from the start of the new list, as Azahar does") {
    NullFixture f;
    // pica_core.cpp: the jump of the header's own write moves the list to the target, and the extra
    // words are then read at the list's current position, the target's first word. The register ends
    // up holding that word, and the target is read from its start after all.
    const ListBuilder b = ListBuilder().Finalize(0x12345678);
    b.Store(f.host, kListB);
    // (channel 1, because the submission clears channel 0's trigger when it ends)
    ListBuilder a;
    a.Write(0x239, b.Size() >> 3).Write(0x23B, (kFcramPaddr + kListB) >> 3);
    a.WriteMany(0x23D, 1, {0xAAAAAAAA}, false);
    CHECK(RunList(f, a) == std::vector<Interrupt>{Interrupt::P3D});
    // B's first word is the value of its finalize write: 0x12345678
    CHECK(f.gpu.GpuRegister(0x400 + 0x23D) == 0x12345678);
}

TEST_CASE("null GPU: a list that jumps to itself is cut after a bound and says so") {
    NullFixture f;
    ListBuilder a;
    const u32 self_size = 6 * 4;  // three commands: size, address, trigger
    Jump(a, 0, kFcramPaddr + kListA, self_size);
    REQUIRE(a.Size() == self_size);
    CHECK(RunList(f, a).empty());
    CHECK(f.Logged("never ends"));
    CHECK(f.gpu.GetStats().command_list_jumps > 1000);
    // the GPU still works afterwards, and the trigger of a plain list is cleared
    f.logged.clear();
    CHECK(RunList(f, ListBuilder().Finalize()) == std::vector<Interrupt>{Interrupt::P3D});
    CHECK_FALSE(f.Logged("never ends"));
}

TEST_CASE("null GPU: the registers a list writes are in the register file") {
    NullFixture f;
    CHECK(RunList(f, ListBuilder().Write(0x40, 0x1234).Write(0x2C0, 0xBEEF, 0x3).Finalize()) ==
          std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.gpu.GpuRegister(0x400 + 0x40) == 0x1234);
    CHECK(f.gpu.GpuRegister(0x400 + 0x2C0) == 0x0000BEEF);
    // a register that does not exist is a no-op
    CHECK(RunList(f, ListBuilder().Write(0x300, 5).Write(0xFFFF, 6).Finalize()) ==
          std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.gpu.GpuRegister(0x400 + 0x2FF) == 0);
}

TEST_CASE("null GPU: a list outside physical memory signals one P3D and says so") {
    NullFixture f;
    f.host.signals.clear();
    f.gpu.Execute(Gx({1, 0x08000000, 0x100, 0, 0, 0, 0, 1}));  // the heap: no physical view
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::P3D});
    CHECK(f.Logged("is not in physical memory"));
    // an empty list is nothing to run, wherever it is
    f.host.signals.clear();
    f.gpu.Execute(Gx({1, 0x08000000, 0, 0, 0, 0, 0, 1}));
    CHECK(f.host.signals.empty());
}

TEST_CASE("null GPU: registers read back what was written; buffer swaps and colour fills land in them") {
    NullFixture f;
    f.gpu.WriteReg(0x1ED02000 + 4 * 0x82, 0x55AA);  // an LCD register
    CHECK(f.gpu.ReadReg(0x1ED02000 + 4 * 0x82) == 0x55AA);
    f.gpu.WriteReg(0x1EF00000 + 4 * 0x500, 0x1234);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * 0x500) == 0x1234);
    // the framebuffer defaults of nn::gx (PicaCore::InitializeRegs)
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (0x100 + 0x1A)) == 0x181E6000);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (0x140 + 0x1A)) == 0x1848F000);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (0x100 + 0x17)) == (240u | (400u << 16)));
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (0x140 + 0x17)) == (240u | (320u << 16)));
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (0x100 + 0x24)) == 720);
    // gs.max_input_attribute_index = 1 and gs.shader_mode = VS (0xA0), at internal register 0x289
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (0x400 + 0x289)) == 0xA0000001);

    // SetBufferSwap: the addresses go through the virtual-to-physical map
    gpu::FrameBufferInfo info{1, kVramVaddr + 0x100, kLinearVaddr + 0x200, 960, 0, 1, 0};
    f.gpu.SetBufferSwap(1, info);
    const u32 bottom = 0x140;
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (bottom + 0x1B)) == kVramPaddr + 0x100);   // left 2
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (bottom + 0x26)) == kFcramPaddr + 0x200);  // right 2
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (bottom + 0x24)) == 960);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (bottom + 0x1C)) == 0);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (bottom + 0x1E)) == 1);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (bottom + 0x1A)) == 0x1848F000);  // the first buffer is as it was
    info.active_fb = 0;
    info.address_right = 0;
    f.gpu.SetBufferSwap(0, info);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (0x100 + 0x1A)) == kVramPaddr + 0x100);
    CHECK(f.gpu.ReadReg(0x1EF00000 + 4 * (0x100 + 0x25)) == 0);  // address 0 stays 0
    f.gpu.SetBufferSwap(2, info);  // there are two screens
    CHECK(f.gpu.GetStats().buffer_swaps[0] == 1);
    CHECK(f.gpu.GetStats().buffer_swaps[1] == 1);

    f.gpu.SetColorFill(0x01000000);
    CHECK(f.gpu.ReadReg(0x1ED02000 + 4 * 0x81) == 0x01000000);
    CHECK(f.gpu.ReadReg(0x1ED02000 + 4 * 0x281) == 0x01000000);

    // an address outside both windows reads 0, once in the log
    CHECK(f.gpu.ReadReg(0x1EC00000) == 0);
    CHECK(f.gpu.ReadReg(0x1EC00004) == 0);
    f.gpu.WriteReg(0x1EC00008, 5);
    int reports = 0;
    for (const std::string& line : f.logged) {
        reports += line.find("unknown GPU address") != std::string::npos ? 1 : 0;
    }
    CHECK(reports == 1);
}

// ---- DMA anywhere (D41) ---------------------------------------------------------------------------

using GuestNullFixture = BasicNullFixture<GuestFakeHost>;

void Pattern(std::vector<u8>& bytes, u32 offset, u32 count, u32 seed) {
    for (u32 i = 0; i < count; ++i) {
        bytes[offset + i] = static_cast<u8>(seed + i * 13);
    }
}

bool Same(const std::vector<u8>& a, u32 a_offset, const std::vector<u8>& b, u32 b_offset, u32 count) {
    return std::equal(a.begin() + a_offset, a.begin() + a_offset + count, b.begin() + b_offset);
}

TEST_CASE("null GPU: a DMA from the application's heap reaches the linear heap and VRAM") {
    GuestNullFixture f;
    Pattern(f.host.heap, 0, 0x2000, 3);
    // heap to the linear heap: RAM to RAM, 0x1000 bytes at 190 MiB/s
    f.gpu.Execute(Gx({0, kHeapVaddr, kLinearVaddr + 0x100, 0x1000}));
    CHECK(Same(f.host.heap, 0, f.host.fcram, 0x100, 0x1000));
    CHECK(f.host.fcram[0xFF] == 0);
    CHECK(f.host.fcram[0x1100] == 0);
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 20559);
    // heap to VRAM: RAM to VRAM, 0x800 bytes at 310 MiB/s
    f.gpu.Execute(Gx({0, kHeapVaddr + 0x1000, kVramVaddr + 0x2000, 0x800}));
    CHECK(Same(f.host.heap, 0x1000, f.host.vram, 0x2000, 0x800));
    CHECK(f.host.vram[0x2800] == 0);
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 6300);
    CHECK(f.host.Ids() == std::vector<Interrupt>(2, Interrupt::DMA));
    CHECK(f.gpu.GetStats().dma_through_guest == 2);
    CHECK(f.gpu.GetStats().dma_not_copied == 0);
    CHECK(f.logged.empty());
}

TEST_CASE("null GPU: a DMA into the application's heap, and within guest memory, copies") {
    GuestNullFixture f;
    Pattern(f.host.fcram, 0, 0x1000, 5);
    Pattern(f.host.vram, 0x4000, 0x400, 9);
    // the linear heap to the heap, VRAM to the heap (VRAM to RAM: 380 MiB/s)
    f.gpu.Execute(Gx({0, kLinearVaddr, kHeapVaddr + 0x40, 0x1000}));
    CHECK(Same(f.host.fcram, 0, f.host.heap, 0x40, 0x1000));
    f.gpu.Execute(Gx({0, kVramVaddr + 0x4000, kHeapVaddr + 0x3000, 0x400}));
    CHECK(Same(f.host.vram, 0x4000, f.host.heap, 0x3000, 0x400));
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 2569);
    // one range of guest memory to another, with no physical view at either end
    Pattern(f.host.heap, 0x8000, 0x200, 77);
    f.gpu.Execute(Gx({0, kHeapVaddr + 0x8000, kOtherVaddr + 0x10, 0x200}));
    CHECK(Same(f.host.heap, 0x8000, f.host.other, 0x10, 0x200));
    CHECK(f.host.Ids() == std::vector<Interrupt>(3, Interrupt::DMA));
    CHECK(f.logged.empty());
}

TEST_CASE("null GPU: a DMA that overlaps itself moves the bytes as memmove does") {
    GuestNullFixture f;
    Pattern(f.host.heap, 0, 0x100, 1);
    const std::vector<u8> original = f.host.heap;
    f.gpu.Execute(Gx({0, kHeapVaddr, kHeapVaddr + 0x10, 0xF0}));
    CHECK(Same(original, 0, f.host.heap, 0x10, 0xF0));
    CHECK(Same(original, 0, f.host.heap, 0, 0x10));
    f.host.heap = original;
    f.gpu.Execute(Gx({0, kHeapVaddr + 0x10, kHeapVaddr, 0xF0}));
    CHECK(Same(original, 0x10, f.host.heap, 0, 0xF0));
}

TEST_CASE("null GPU: the physical view serves a DMA first; the guest pointers only fill in what it cannot") {
    GuestNullFixture f;
    Pattern(f.host.fcram, 0, 0x400, 21);
    f.gpu.Execute(Gx({0, kLinearVaddr, kVramVaddr + 0x100, 0x400}));
    CHECK(Same(f.host.fcram, 0, f.host.vram, 0x100, 0x400));
    CHECK(f.host.guest_pointer_calls == 0);
    CHECK(f.gpu.GetStats().dma_through_guest == 0);
    // the end of the arena of the linear heap: the physical prefix is copied, as it always was
    Pattern(f.host.fcram, kArena - 0x80, 0x80, 99);
    f.gpu.Execute(Gx({0, kLinearVaddr + kArena - 0x80, kLinearVaddr + 0x2000, 0x100}));
    CHECK(Same(f.host.fcram, kArena - 0x80, f.host.fcram, 0x2000, 0x80));
    CHECK(f.Logged("leaves the memory (0x80 copied)"));
}

TEST_CASE("null GPU: a DMA to or from memory nobody maps copies nothing, says so once, and still signals DMA") {
    GuestNullFixture f;
    Pattern(f.host.heap, 0, 0x100, 4);
    Pattern(f.host.vram, 0, 0x100, 8);
    const std::vector<u8> heap = f.host.heap, vram = f.host.vram, fcram = f.host.fcram;
    // an unmapped source
    f.gpu.Execute(Gx({0, 0x30000000, kVramVaddr, 0x100}));
    CHECK(f.host.vram == vram);
    CHECK(f.host.Ids() == std::vector<Interrupt>{Interrupt::DMA});
    int reports = 0;
    for (const std::string& line : f.logged) {
        reports += line.find("DMA with an address that is not physical memory") != std::string::npos ? 1 : 0;
    }
    CHECK(reports == 1);
    // an unmapped destination
    f.gpu.Execute(Gx({0, kHeapVaddr, 0x30000000, 0x100}));
    // both
    f.gpu.Execute(Gx({0, 0x30000000, 0x34000000, 0x100}));
    // a range that starts in the heap and leaves it: not mapped as a whole, so nothing is copied
    f.gpu.Execute(Gx({0, kHeapVaddr + kArena - 0x80, kOtherVaddr, 0x100}));
    f.gpu.Execute(Gx({0, kHeapVaddr, kOtherVaddr + kArena - 0x80, 0x100}));
    CHECK(f.host.heap == heap);
    CHECK(f.host.vram == vram);
    CHECK(f.host.fcram == fcram);
    CHECK(std::all_of(f.host.other.begin(), f.host.other.end(), [](u8 b) { return b == 0; }));
    CHECK(f.host.Ids() == std::vector<Interrupt>(5, Interrupt::DMA));
    CHECK(f.gpu.GetStats().dma_not_copied == 5);
    // the delay is still Azahar's: 0x100 bytes from RAM to RAM at 190 MiB/s
    CHECK(f.gpu.GetStats().last_delay_ns[6] == 1284);
}

TEST_CASE("null GPU: a game that keeps DMAing unmapped memory does not flood the log") {
    GuestNullFixture f;
    for (u32 i = 0; i < 1000; ++i) {
        f.gpu.Execute(Gx({0, 0x30000000, kHeapVaddr, 0x100}));
    }
    CHECK(f.host.signals.size() == 1000);
    CHECK(f.gpu.GetStats().dma_not_copied == 1000);
    CHECK(f.logged.size() <= 10);
    CHECK(f.Logged("further DMAs"));
}

TEST_CASE("null GPU: every interrupt carries the delay of its work, P3D carries none") {
    NullFixture f;
    // a fill of 0x1000 bytes of VRAM: PSC0, the texture speed of VRAM to VRAM
    f.gpu.Execute(Gx({2, kVramVaddr, 0xAABBCCDD, kVramVaddr + 0x1000, 0, 0, 0, 0x0201}));
    // the second unit alone: PSC1
    f.gpu.Execute(Gx({2, 0, 0, 0, kVramVaddr, 1, kVramVaddr + 0x1000, 0x0201u << 16}));
    // a display transfer of 64 x 32 RGBA8 pixels from VRAM to FCRAM: PPF
    f.gpu.Execute(Gx({3, kVramVaddr, kLinearVaddr, 64u | (32u << 16), 64u | (32u << 16), 0}));
    // a DMA of 0x1000 bytes from VRAM to VRAM at 380 MiB/s
    f.gpu.Execute(Gx({0, kVramVaddr, kVramVaddr + 0x8000, 0x1000}));
    // a command list with one finalize
    ListBuilder list;
    list.Finalize();
    list.Store(f.host);
    f.gpu.Execute(Gx({1, kLinearVaddr + 0x1000, list.Size(), 0, 0, 0, 0, 1}));
    REQUIRE(f.host.signals.size() == 5);
    CHECK(f.host.signals[0].id == Interrupt::PSC0);
    CHECK(f.host.signals[0].delay_ns == 723);
    CHECK(f.host.signals[1].id == Interrupt::PSC1);
    CHECK(f.host.signals[1].delay_ns == 723);
    CHECK(f.host.signals[2].id == Interrupt::PPF);
    CHECK(f.host.signals[2].delay_ns == 1446);
    CHECK(f.host.signals[3].id == Interrupt::DMA);
    CHECK(f.host.signals[3].delay_ns == 10279);
    CHECK(f.host.signals[4].id == Interrupt::P3D);
    CHECK(f.host.signals[4].delay_ns == 0);
}


// ===================================================================================================
// The service, through the SVC layer
// ===================================================================================================

constexpr u32 kText = 0x00100000;
constexpr u32 kShm = 0x10100000;  // where the tests map the GSP shared memory
constexpr Result kTimeout = 0x09401BFE;
constexpr u32 kCurrentProcess = 0xFFFF8001;
constexpr u32 kDontCare = 0x10000000;

void Put16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v), b[at + 1] = static_cast<u8>(v >> 8);
}
void Put32(std::vector<u8>& b, std::size_t at, u32 v) {
    Put16(b, at, v & 0xFFFF), Put16(b, at + 2, v >> 16);
}

std::vector<u8> MakeElf() {
    std::vector<u8> b(52 + 32, 0);
    b[0] = 0x7F, b[1] = 'E', b[2] = 'L', b[3] = 'F', b[4] = 1, b[5] = 1, b[6] = 1;
    Put16(b, 16, 2), Put16(b, 18, 40), Put32(b, 20, 1), Put32(b, 24, kText);
    Put32(b, 28, 52), Put16(b, 40, 52), Put16(b, 42, 32), Put16(b, 44, 1);
    Put32(b, 52, 1);  // PT_LOAD
    Put32(b, 52 + 4, 84), Put32(b, 52 + 8, kText), Put32(b, 52 + 12, kText);
    Put32(b, 52 + 16, 0x40), Put32(b, 52 + 20, 0x40), Put32(b, 52 + 24, 5), Put32(b, 52 + 28, 0x1000);
    for (u32 i = 0; i < 0x40; ++i) {
        b.push_back(static_cast<u8>(i));
    }
    return b;
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_gsp_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string WriteElf() const {
        const std::string path = (path_ / "program.elf").string();
        const std::vector<u8> bytes = MakeElf();
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return path;
    }

private:
    std::filesystem::path path_;
};

// ---- a Gpu that records what the service tells it -------------------------------------------------------

Runtime* g_runtime = nullptr;

class RecordingGpu final : public gpu::Gpu {
public:
    explicit RecordingGpu(gpu::Host& host) : host_(host) {}

    void Execute(const gpu::GxCommand& command) override {
        executed.push_back(command);
        if (on_execute) {
            on_execute(command);
        }
    }
    void WriteReg(u32 addr, u32 value) override {
        writes.push_back({addr, value});
        regs[addr] = value;
    }
    u32 ReadReg(u32 addr) override {
        reads.push_back(addr);
        const auto it = regs.find(addr);
        return it == regs.end() ? 0 : it->second;
    }
    void SetBufferSwap(u32 screen, const gpu::FrameBufferInfo& info) override {
        swaps.push_back({screen, info});
    }
    void SetColorFill(u32 fill) override { fills.push_back(fill); }
    void FlushRegion(u32 vaddr, u32 size) override { flushes.push_back({vaddr, size}); }
    void InvalidateRegion(u32 vaddr, u32 size) override { invalidations.push_back({vaddr, size}); }
    void VBlank(u32 screen) override {
        if (screen < 2 && g_runtime != nullptr) {
            vblank_ticks[screen].push_back(g_runtime->Clock().Now());
        }
        order.push_back(screen);
    }

    gpu::Host& host_;
    std::vector<gpu::GxCommand> executed;
    std::function<void(const gpu::GxCommand&)> on_execute;
    std::vector<std::pair<u32, u32>> writes;
    std::vector<u32> reads;
    std::map<u32, u32> regs;
    std::vector<std::pair<u32, gpu::FrameBufferInfo>> swaps;
    std::vector<u32> fills;
    std::vector<std::pair<u32, u32>> flushes, invalidations;
    std::vector<u64> vblank_ticks[2];
    std::vector<u32> order;  // the screens of the VBlank calls
};

RecordingGpu* g_recording = nullptr;

MH4N_REGISTER(gpu::GpuFactory, "gsp-test", [](gpu::Host& host, const Options&) -> std::unique_ptr<gpu::Gpu> {
    auto made = std::make_unique<RecordingGpu>(host);
    g_recording = made.get();
    return made;
});

struct Fixture {
    ManualTimeSource* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    std::vector<std::string> logged;
    Fixture() {
        auto source = std::make_unique<ManualTimeSource>();
        time = source.get();
        RuntimeConfig config;
        config.time = std::move(source);
        runtime = std::make_unique<Runtime>(std::move(config));
        g_runtime = runtime.get();
        g_recording = nullptr;
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { logged.push_back(std::string(r.message)); });
    }
    ~Fixture() {
        runtime.reset();
        g_runtime = nullptr;
        g_recording = nullptr;
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    int LoggedCount(const std::string& needle) const {
        int n = 0;
        for (const std::string& line : logged) {
            n += line.find(needle) != std::string::npos ? 1 : 0;
        }
        return n;
    }
    bool Logged(const std::string& needle) const { return LoggedCount(needle) > 0; }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra, std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf()};
    args.insert(args.end(), extra.begin(), extra.end());
    f.runtime->SetMainThreadEntry([body = std::move(body)](GuestThread&) { body(); });
    return f.runtime->Boot(Parse(args));
}

struct Regs {
    std::array<u32, 16> r{};
};

Regs CallSvc(Runtime& rt, u32 number, std::initializer_list<u32> in) {
    RCtx ctx{};
    int i = 0;
    for (const u32 value : in) {
        ctx.r[i++] = value;
    }
    ctx.r[14] = kText + 0x20;
    rt.Svc().CallSvc(ctx, number, kText + 0x11);
    Regs out;
    for (int k = 0; k < 16; ++k) {
        out.r[static_cast<std::size_t>(k)] = ctx.r[k];
    }
    return out;
}

void ExitProcess(Runtime& rt) {
    CallSvc(rt, 0x03, {});
}

// The guest side of one run: the main thread's command buffer and a scratch area on its stack.
struct Guest {
    Runtime& rt;
    explicit Guest(Runtime& runtime) : rt(runtime) {}

    u32 Cmd() const { return CommandBufferAddress(*rt.MainThread()); }
    u32 Scratch(u32 offset = 0) const { return rt.MainThread()->stack_top - 0x3000 + offset; }

    void PutWords(u32 address, const std::vector<u32>& words) {
        REQUIRE(rt.Memory().WriteBlock(address, words.data(), static_cast<u32>(words.size() * 4)));
    }
    void Put(const std::vector<u32>& words, u32 first_word = 0) { PutWords(Cmd() + first_word * 4, words); }
    std::vector<u32> GetWords(u32 address, std::size_t count) {
        std::vector<u32> words(count);
        REQUIRE(rt.Memory().ReadBlock(words.data(), address, static_cast<u32>(count * 4)));
        return words;
    }
    std::vector<u32> Get(std::size_t count) { return GetWords(Cmd(), count); }
    u32 Word(u32 address) { return GetWords(address, 1)[0]; }
    void PutBytes(u32 address, const std::vector<u8>& bytes) {
        REQUIRE(rt.Memory().WriteBlock(address, bytes.data(), static_cast<u32>(bytes.size())));
    }
    std::vector<u8> GetBytes(u32 address, std::size_t count) {
        std::vector<u8> bytes(count);
        REQUIRE(rt.Memory().ReadBlock(bytes.data(), address, static_cast<u32>(count)));
        return bytes;
    }

    Result ConnectToPort(const std::string& name, Handle* handle) {
        std::vector<u8> text(name.begin(), name.end());
        text.push_back(0);
        PutBytes(Scratch(0x2000), text);
        const Regs out = CallSvc(rt, 0x2D, {0, Scratch(0x2000)});
        *handle = out.r[1];
        return out.r[0];
    }
    Result Send(Handle session) { return CallSvc(rt, 0x32, {session}).r[0]; }
    Result Close(Handle handle) { return CallSvc(rt, 0x23, {handle}).r[0]; }
    Result Wait(Handle handle, s64 ns) {
        return CallSvc(rt, 0x24,
                       {handle, static_cast<u32>(ns), static_cast<u32>(static_cast<u64>(ns) >> 32)})
            .r[0];
    }
    std::vector<u32> Request(Handle session, const std::vector<u32>& words, std::size_t reply_words,
                             Result* result = nullptr) {
        Put(words);
        const Result r = Send(session);
        if (result != nullptr) {
            *result = r;
        }
        return Get(reply_words);
    }
    Handle NewEvent() { return CallSvc(rt, 0x17, {0}).r[1]; }  // a OneShot event
    void Sleep(s64 ns) { rt.Scheduler().Sleep(ns); }
    Result MapBlock(Handle block, u32 address) {
        return CallSvc(rt, 0x1F, {block, address, 3, kDontCare}).r[0];
    }
};

std::array<u32, 2> NameWords(const std::string& name) {
    std::array<u32, 2> words{};
    std::memcpy(words.data(), name.data(), std::min<std::size_t>(name.size(), 8));
    return words;
}

// gsp::Gpu sessions, the way the game gets them.
struct Gsp {
    Guest& g;
    Handle srv = 0;
    explicit Gsp(Guest& guest) : g(guest) {
        REQUIRE(g.ConnectToPort("srv:", &srv) == kResultSuccess);
    }
    // The reply words of GetServiceHandle(name) and the handle (0 if refused).
    std::pair<std::vector<u32>, Handle> Open(const std::string& name = "gsp::Gpu") {
        const auto words = NameWords(name);
        const std::vector<u32> reply =
            g.Request(srv, {ipc::MakeHeader(0x0005, 4, 0), words[0], words[1], 8, 0}, 4);
        return {reply, reply[0] == ipc::MakeHeader(0x0005, 1, 2) ? reply[3] : 0};
    }
};

// One session of gsp::Gpu and what the game does with it.
struct Session {
    Guest& g;
    Handle handle = 0;
    Handle event = 0;
    std::vector<u32> registered;  // the reply of RegisterInterruptRelayQueue
    u32 thread = 0xFFFFFFFF;

    Session(Guest& guest, Handle session) : g(guest), handle(session) {}

    std::vector<u32> Call(const std::vector<u32>& words, std::size_t reply_words) {
        return g.Request(handle, words, reply_words);
    }
    // RegisterInterruptRelayQueue with a new event; maps the shared memory at kShm if asked.
    std::vector<u32> Register(u32 flags = 1, bool map = true) {
        event = g.NewEvent();
        registered = Call({0x00130042, flags, ipc::CopyHandleDesc(1), event}, 5);
        thread = registered[2];
        if (map && registered[0] == 0x00130082 && IsSuccess(registered[1])) {
            REQUIRE(g.MapBlock(registered[4], kShm) == kResultSuccess);
        }
        return registered;
    }
    Result AcquireRight() { return Call({0x00160042, 0, ipc::CopyHandleDesc(1), kCurrentProcess}, 2)[1]; }
    Result TryAcquireRight() { return Call({0x00150002, ipc::CopyHandleDesc(1), kCurrentProcess}, 2)[1]; }
    Result ReleaseRight() { return Call({0x00170000}, 2)[1]; }
    Result Trigger() { return Call({0x000C0000}, 2)[1]; }

    // Shared memory of the mapped block.
    gsp::InterruptRelayQueue Queue() {
        gsp::InterruptRelayQueue q;
        const std::vector<u8> bytes = g.GetBytes(kShm + 0x40 * thread, sizeof(q));
        std::memcpy(&q, bytes.data(), sizeof(q));
        return q;
    }
    // What the game's interrupt thread does: pops the oldest interrupt, or returns -1.
    int Pop() {
        gsp::InterruptRelayQueue q = Queue();
        if (q.number_interrupts == 0) {
            return -1;
        }
        const int id = q.slot[q.index];
        const u32 base = kShm + 0x40 * thread;
        g.PutBytes(base, {static_cast<u8>((q.index + 1) % gsp::InterruptRelayQueue::kMaxSlots),
                          static_cast<u8>(q.number_interrupts - 1)});
        return id;
    }
    std::vector<int> PopAll() {
        std::vector<int> ids;
        for (int id = Pop(); id >= 0; id = Pop()) {
            ids.push_back(id);
        }
        return ids;
    }
    u32 CommandBuffer() const { return kShm + 0x800 + 0x200 * thread; }
    // Writes `commands` into this thread's GX command buffer from `index` on and sets the counts.
    void PutCommands(const std::vector<gpu::GxCommand>& commands, u32 index = 0) {
        for (u32 i = 0; i < commands.size(); ++i) {
            const u32 slot = (index + i) % 0xF;
            g.PutWords(CommandBuffer() + 0x20 + 0x20 * slot,
                       std::vector<u32>(commands[i].words, commands[i].words + 8));
        }
        g.PutWords(CommandBuffer(), {index | (static_cast<u32>(commands.size()) << 8)});
    }
    u32 CommandHeader() { return g.Word(CommandBuffer()); }
    // The framebuffer update of this thread for `screen`.
    void SetFrameBuffer(u32 screen, u32 index, bool dirty, gpu::FrameBufferInfo info0,
                        gpu::FrameBufferInfo info1) {
        const u32 base = kShm + 0x200 + (2 * thread + screen) * 0x40;
        std::vector<u32> words = {static_cast<u32>(index) | (dirty ? 0x100u : 0u)};
        for (const gpu::FrameBufferInfo& info : {info0, info1}) {
            words.insert(words.end(), {info.active_fb, info.address_left, info.address_right, info.stride,
                                       info.format, info.shown_fb, info.unknown});
        }
        words.push_back(0);
        g.PutWords(base, words);
    }
    bool FrameBufferDirty(u32 screen) {
        return (g.Word(kShm + 0x200 + (2 * thread + screen) * 0x40) & 0x100) != 0;
    }
};

gpu::GxCommand FillCommand(u32 start, u32 value, u32 end, u32 control = 0x201) {
    return Gx({2, start, value, end, 0, 0, 0, control});
}

std::shared_ptr<gsp::GspState> StateOf(Fixture& f) {
    ServiceManager* manager = FindServiceManager(*f.runtime);
    REQUIRE(manager != nullptr);
    return manager->Context().State<gsp::GspState>("gsp::Gpu");
}

}  // namespace

// ---- the VBlank length ---------------------------------------------------------------------------------

TEST_CASE("gsp: the VBlank period for a refresh rate is 268111856 / hz, truncated; 0 is the default") {
    CHECK(gsp::kFrameTicks == 4481136);
    CHECK(gsp::VBlankPeriodForRate(0) == 4481136);
    CHECK(gsp::VBlankPeriodForRate(120.0) == 2234265);  // 2234265.47
    CHECK(gsp::VBlankPeriodForRate(60.0) == 4468530);    // 4468530.93
    CHECK(gsp::VBlankPeriodForRate(30.0) == 8937061);
    CHECK(gsp::VBlankPeriodForRate(59.83) == 4481227);
    CHECK(gsp::VBlankPeriodForRate(268111856.0) == 1);
    // the default is the console's own rate, about 59.83 Hz
    CHECK(268111856.0 / 4481136.0 == doctest::Approx(59.8261).epsilon(0.0001));
    // what the patch leaves open: nothing that is not a positive number, nothing that stops time
    CHECK(gsp::VBlankPeriodForRate(-5.0) == 4481136);
    CHECK(gsp::VBlankPeriodForRate(std::numeric_limits<double>::quiet_NaN()) == 4481136);
    bool clamped = false;
    CHECK(gsp::VBlankPeriodForRate(1e12, &clamped) == 1);
    CHECK(clamped);
    CHECK(gsp::VBlankPeriodForRate(std::numeric_limits<double>::infinity(), &clamped) == 1);
    CHECK(clamped);
    CHECK(gsp::VBlankPeriodForRate(1e-12, &clamped) == gsp::kMaxVBlankPeriodTicks);
    CHECK(clamped);
    gsp::VBlankPeriodForRate(60.0, &clamped);
    CHECK_FALSE(clamped);
}

// ---- the cadence on a fake clock, without a guest ------------------------------------------------------

TEST_CASE("gsp: VBlanks come every 4,481,136 ticks from the previous deadline, however late they run") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
    auto state = StateOf(f);
    auto* null_gpu = dynamic_cast<gpu::null::NullGpu*>(f.runtime->Interfaces().gpu);
    REQUIRE(null_gpu != nullptr);  // the runtime's default GPU is the null one

    const u64 start = f.runtime->Clock().Now();
    CHECK(state->VBlankPeriodTicks() == 4481136);
    CHECK(state->NextVBlankTick() == start + 4481136);
    CHECK(state->VBlankCount() == 0);

    // Irregular steps, some shorter than a period and some longer than three: time arrives in
    // lumps, as when the host stalls. The VBlanks that are due run (catching up), and the next
    // deadline is always start + (count + 1) * period: nothing accumulates.
    u64 lcg = 12345;
    u64 total_ns = 0;
    for (int step = 0; step < 4000; ++step) {
        lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
        const s64 ns = static_cast<s64>((lcg >> 33) % 70000000) + 1000;  // 1 us to 70 ms
        f.time->Advance(ns);
        total_ns += static_cast<u64>(ns);
        f.runtime->Scheduler().RunPending();
        const u64 now = f.runtime->Clock().Now();
        const u64 expected = (now - start) / 4481136;
        REQUIRE(state->VBlankCount() == expected);
        REQUIRE(state->NextVBlankTick() == start + (expected + 1) * 4481136);
    }
    CHECK(state->VBlankCount() > 3000);  // about 136 s of game time: 8000+ frames
    CHECK(null_gpu->GetStats().vblanks[0] == state->VBlankCount());
    CHECK(null_gpu->GetStats().vblanks[1] == state->VBlankCount());
    CHECK(total_ns > 100000000000ull);
}

TEST_CASE("gsp: a refresh rate changes the period the next VBlank is scheduled with") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
    auto state = StateOf(f);
    const u64 start = f.runtime->Clock().Now();
    const u64 first = start + 4481136;

    // the platform's thread asks for 120 Hz; the sink runs on the scheduler thread
    std::thread([&] { f.runtime->SetRefreshRate(120.0); }).join();
    CHECK(state->VBlankPeriodTicks() == 4481136);  // not yet
    f.runtime->Scheduler().RunPending();
    CHECK(state->VBlankPeriodTicks() == 2234265);
    CHECK(f.LoggedCount("vblank period 2234265 ticks") == 1);
    // the VBlank that is already scheduled keeps its deadline (Azahar's event is rescheduled when it
    // runs); the next one is one new period after it
    CHECK(state->NextVBlankTick() == first);

    const auto advance_to = [&](u64 tick) {
        while (f.runtime->Clock().Now() < tick) {
            f.time->Advance(static_cast<s64>(TicksToNsCeil(tick - f.runtime->Clock().Now())));
        }
        f.runtime->Scheduler().RunPending();
    };
    advance_to(first + 1);
    CHECK(state->VBlankCount() == 1);
    CHECK(state->NextVBlankTick() == first + 2234265);
    // From here, 1000 frames at 120 Hz exactly.
    advance_to(first + 1000ull * 2234265 + 1);
    CHECK(state->VBlankCount() == 1001);
    CHECK(state->NextVBlankTick() == first + 1001ull * 2234265);

    // back to the default with 0: logged, and the same value again is not
    std::thread([&] { f.runtime->SetRefreshRate(0.0); }).join();
    f.runtime->Scheduler().RunPending();
    CHECK(state->VBlankPeriodTicks() == 4481136);
    CHECK(f.LoggedCount("vblank period 4481136 ticks") == 1);
    std::thread([&] { f.runtime->SetRefreshRate(0.0); }).join();
    f.runtime->Scheduler().RunPending();
    CHECK(f.LoggedCount("vblank period 4481136 ticks") == 1);
    CHECK(f.LoggedCount("vblank period") == 2);
}

TEST_CASE("gsp: a refresh rate known before the services start sets the period after the first VBlank") {
    Fixture f;
    f.runtime->SetRefreshRate(120.0);  // before anything exists: only remembered
    REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
    auto state = StateOf(f);
    auto* null_gpu = dynamic_cast<gpu::null::NullGpu*>(f.runtime->Interfaces().gpu);
    REQUIRE(null_gpu != nullptr);
    const u64 start = f.runtime->Clock().Now();
    CHECK(state->VBlankPeriodTicks() == 2234265);
    CHECK(f.Logged("vblank period 2234265 ticks"));
    // Azahar's GPU constructor schedules the first VBlank at FRAME_TICKS; the patch's SetRefreshRate
    // only changes the reschedules (GPU::VBlankCallback).
    const u64 first = start + 4481136;
    CHECK(state->NextVBlankTick() == first);

    const auto advance_to = [&](u64 tick) {
        while (f.runtime->Clock().Now() < tick) {
            f.time->Advance(static_cast<s64>(TicksToNsCeil(tick - f.runtime->Clock().Now())));
        }
        f.runtime->Scheduler().RunPending();
    };
    advance_to(first - 1);
    CHECK(state->VBlankCount() == 0);  // nothing at the 120 Hz deadline
    advance_to(first);
    CHECK(state->VBlankCount() == 1);
    CHECK(state->NextVBlankTick() == first + 2234265);
    advance_to(first + 2234265);
    CHECK(state->VBlankCount() == 2);
    CHECK(state->NextVBlankTick() == first + 2 * 2234265);
    CHECK(null_gpu->GetStats().vblanks[0] == 2);
}

TEST_CASE("gsp: a rate that is not usable is clamped and says so") {
    Fixture f;
    REQUIRE(f.runtime->Prepare(Parse({}), {Phase::Devices, Phase::Services}));
    auto state = StateOf(f);
    state->SetRefreshRate(1e12);
    CHECK(state->VBlankPeriodTicks() == 1);
    CHECK(f.Logged("outside the usable range"));
    state->SetRefreshRate(0.0);
    CHECK(state->VBlankPeriodTicks() == 4481136);
}

// ---- through a guest -------------------------------------------------------------------------------------

TEST_CASE("gsp: the VBlank cadence does not drift over 3000 frames with the guest running") {
    Fixture f;
    TempDir dir;
    u64 start = 0;
    std::vector<u64> ticks;
    const int status = BootWith(f, dir, {"--gpu", "gsp-test", "--ms", "100000"}, [&] {
        Guest g(*f.runtime);
        start = f.runtime->Clock().Now();
        // the game: sleeps, as a thread waiting for the next frame would
        for (int i = 0; i < 3400; ++i) {  // 54 s: 3255 frames
            g.Sleep(16000000);
        }
        ticks = g_recording->vblank_ticks[0];
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(ticks.size() > 3200);
    // The first VBlank is one period after the services started; every one is on its deadline (the
    // clock moves in jumps of up to 4 ticks per nanosecond rounding, never more).
    const u64 first = ticks[0];
    CHECK(first > start);  // one period after the services started, a little before the guest
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const s64 late = static_cast<s64>(ticks[i]) - static_cast<s64>(first + i * 4481136);
        REQUIRE(late >= -8);
        REQUIRE(late <= 8);
    }
}

TEST_CASE("gsp: no VBlank interrupt is lost over a long session, at 60 and then 120 Hz") {
    Fixture f;
    TempDir dir;
    u64 pdc0 = 0, pdc1 = 0, others = 0, vblanks_at_switch = 0;
    gsp::InterruptRelayQueue last{};
    u64 count = 0;
    std::vector<u64> second_half;
    const int status = BootWith(f, dir, {"--ms", "100000000", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        auto state = StateOf(f);
        // The game's interrupt thread: wait for the event, pop everything, count. 20000 frames at the
        // console's rate, a little over 5.5 minutes of game time, then 20000 at 120 Hz.
        auto frames = [&](u32 n, u64 ns_per_loop) {
            for (u32 i = 0; i < n; ++i) {
                g.Sleep(static_cast<s64>(ns_per_loop));
                for (int id = s.Pop(); id >= 0; id = s.Pop()) {
                    pdc0 += id == 2;
                    pdc1 += id == 3;
                    others += id != 2 && id != 3;
                }
            }
        };
        frames(20000, 16713680);  // exactly one VBlank in each loop, give or take
        vblanks_at_switch = state->VBlankCount();
        f.runtime->SetRefreshRate(120.0);
        frames(1, 20000000);  // the sink runs; the pending VBlank has the old period
        second_half.push_back(state->VBlankPeriodTicks());
        const u64 before = state->VBlankCount();
        frames(20000, 8333333);  // the period of 120 Hz in game time
        count = state->VBlankCount() - before;
        last = s.Queue();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // the loops are not exactly one period long, so the count can differ from the loop count by
    // the drift between the two, but every VBlank that happened was delivered once, to both
    CHECK(pdc0 == pdc1);
    CHECK(pdc0 > 20000 + 19000);
    CHECK(others == 0);
    CHECK(last.missed_pdc0 == 0);
    CHECK(last.missed_pdc1 == 0);
    CHECK(last.error_code == 0);
    CHECK(second_half == std::vector<u64>{2234265});
    CHECK(vblanks_at_switch >= 19990);
    CHECK(vblanks_at_switch <= 20010);
    CHECK(count >= 19990);
    CHECK(count <= 20010);
}

TEST_CASE("gsp: RegisterInterruptRelayQueue answers 0x2A07 once, gives each session its thread index") {
    Fixture f;
    TempDir dir;
    std::vector<u32> first, second;
    std::pair<std::vector<u32>, Handle> fifth;
    u32 reused_thread = 99;
    std::vector<u32> unregister;
    Result map_again = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session a(g, gsp.Open().second);
        Session b(g, gsp.Open().second);
        first = a.Register(1);
        second = b.Register(0, false);
        Session c(g, gsp.Open().second);
        Session d(g, gsp.Open().second);
        fifth = gsp.Open();  // four sessions at most
        unregister = a.Call({0x00140000}, 2);
        // closing a session frees its thread index, and the lowest one is the next
        g.Close(b.handle);
        Session e(g, gsp.Open().second);
        e.Register(0, false);
        reused_thread = e.thread;
        // a second mapping of the block is refused (unsupported aliasing), the first one stays
        map_again = g.MapBlock(first[4], kShm + 0x10000);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(first.size() == 5);
    CHECK(first[0] == 0x00130082);
    CHECK(first[1] == 0x2A07);
    CHECK(first[2] == 0);                       // thread index
    CHECK(first[3] == ipc::CopyHandleDesc(1));  // the handle descriptor
    CHECK(first[4] != 0);
    CHECK(second[1] == kResultSuccess);  // only the first registration is 0x2A07
    CHECK(second[2] == 1);
    CHECK(fifth.second == 0);
    CHECK(fifth.first[1] == 0xD0401834);  // all four sessions are open
    CHECK(unregister == std::vector<u32>{0x00140040, 0});
    CHECK(reused_thread == 1);
    CHECK(map_again != kResultSuccess);
}

TEST_CASE("gsp: a request without a usable event handle is refused, not asserted") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session a(g, gsp.Open().second);
        reply = a.Call({0x00130042, 1, ipc::CopyHandleDesc(1), 0}, 2);  // the handle 0
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply[0] == 0x00130040);
    CHECK(reply[1] == 0xD8E007F7);  // kResultInvalidHandle
}

TEST_CASE("gsp: a fill in the GX queue reaches the thread's interrupt queue before the reply") {
    Fixture f;
    TempDir dir;
    std::vector<u32> trigger_reply, header_after, vram_words;
    gsp::InterruptRelayQueue queue{};
    Result event_first = 0, event_second = 0, right = 0;
    std::vector<int> popped;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        right = s.AcquireRight();
        // a fill of 0x1000 bytes of VRAM with a 32-bit value
        s.PutCommands({FillCommand(0x1F000000, 0xAABBCCDD, 0x1F001000)});
        trigger_reply = s.Call({0x000C0000}, 2);
        header_after = {s.CommandHeader()};
        vram_words = g.GetWords(0x1F000000, 4);
        queue = s.Queue();
        event_first = g.Wait(s.event, 0);   // signalled by the interrupt
        event_second = g.Wait(s.event, 0);  // and a OneShot event is clear again
        popped = s.PopAll();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(right == kResultSuccess);
    CHECK(trigger_reply == std::vector<u32>{0x000C0040, 0});
    // the command was taken: index 1, no commands left
    CHECK(header_after[0] == 0x00000001);
    CHECK(vram_words == std::vector<u32>(4, 0xAABBCCDD));
    CHECK(queue.index == 0);
    CHECK(queue.number_interrupts == 1);
    CHECK(queue.error_code == 0);
    CHECK(queue.slot[0] == 0);  // PSC0
    CHECK(event_first == kResultSuccess);
    CHECK(event_second == kTimeout);
    CHECK(popped == std::vector<int>{0});
}

TEST_CASE("gsp: the interrupt is in the queue when the request returns, also with no reply delay") {
    // Azahar queues the interrupt inside the handler. The GPU's interrupt reaches the service through
    // Kernel::Post, which runs at the next safe point: before the guest does anything else.
    Fixture f;
    TempDir dir;
    std::vector<int> popped;
    Result event_result = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        s.AcquireRight();
        FindServiceManager(*f.runtime)->SetReplyDelayNs(0);
        for (u32 round = 0; round < 3; ++round) {
            s.PutCommands({FillCommand(0x1F000000, round, 0x1F000100), Gx({0, 0x1F000000, 0x1F001000, 0x100})});
            s.Trigger();
            // both are in the queue at once: the fill's PSC0 and the DMA
            const std::vector<int> ids = s.PopAll();
            popped.insert(popped.end(), ids.begin(), ids.end());
            event_result = g.Wait(s.event, 0);
        }
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(popped == std::vector<int>{0, 6, 0, 6, 0, 6});
    CHECK(event_result == kResultSuccess);
}

TEST_CASE("gsp: the interrupt queue wraps, overflows, and PDC stops queueing at 0x20") {
    Fixture f;
    TempDir dir;
    std::vector<int> after_wrap;
    gsp::InterruptRelayQueue full{}, threshold{}, ignored{};
    u8 index_after = 0;
    const int status = BootWith(f, dir, {"--ms", "100", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        s.AcquireRight();
        auto state = StateOf(f);
        // fill the queue: 0x34 slots
        for (u32 i = 0; i < 0x34 + 5; ++i) {
            state->SignalInterrupt(i % 2 == 0 ? gpu::Interrupt::DMA : gpu::Interrupt::PPF);
        }
        full = s.Queue();
        // the game pops three, GSP queues three more: they land in the slots that came free
        s.Pop(), s.Pop(), s.Pop();
        state->SignalInterrupt(gpu::Interrupt::P3D);
        state->SignalInterrupt(gpu::Interrupt::PSC1);
        state->SignalInterrupt(gpu::Interrupt::PSC0);
        index_after = s.Queue().index;
        after_wrap = s.PopAll();
        // PDC: queued while fewer than 0x20 wait, counted as missed from there on
        for (int i = 0; i < 0x20; ++i) {
            state->SignalInterrupt(gpu::Interrupt::DMA);
        }
        state->SignalInterrupt(gpu::Interrupt::PDC0);
        state->SignalInterrupt(gpu::Interrupt::PDC1);
        state->SignalInterrupt(gpu::Interrupt::PDC1);
        threshold = s.Queue();
        s.PopAll();
        // the config byte's bit 0 tells GSP to leave the VBlanks out
        g.PutBytes(kShm + 0x40 * s.thread + 3, {1});
        state->SignalInterrupt(gpu::Interrupt::PDC0);
        state->SignalInterrupt(gpu::Interrupt::PSC0);
        ignored = s.Queue();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(full.number_interrupts == 0x34);
    CHECK(full.error_code == 1);  // "queue_full_error"
    CHECK(full.index == 0);
    CHECK(full.slot[0] == 6);  // DMA
    CHECK(full.slot[1] == 4);  // PPF
    CHECK(full.slot[0x33] == 4);
    CHECK(index_after == 3);
    // the first interrupts the game left in the queue are 3..0x33, then the three new ones
    REQUIRE(after_wrap.size() == 0x34);
    CHECK(after_wrap[0] == 4);
    CHECK(after_wrap[0x31] == 5);  // P3D
    CHECK(after_wrap[0x32] == 1);  // PSC1
    CHECK(after_wrap[0x33] == 0);  // PSC0
    CHECK(threshold.number_interrupts == 0x20);  // none of the three PDCs was queued
    CHECK(threshold.missed_pdc0 == 1);
    CHECK(threshold.missed_pdc1 == 2);
    CHECK(ignored.number_interrupts == 1);  // the PSC0 only
    CHECK(ignored.missed_pdc0 == 1);        // the ignored PDC0 is not counted either
    CHECK(ignored.slot[ignored.index] == 0);
}

TEST_CASE("gsp: PDC reaches every registered thread, the others only the thread with the right") {
    Fixture f;
    TempDir dir;
    std::vector<int> a_pdc, b_pdc, a_psc, b_psc, a_none, b_none;
    const int status = BootWith(f, dir, {"--ms", "100", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session a(g, gsp.Open().second);
        Session b(g, gsp.Open().second);
        a.Register(0);
        b.Register(0, false);
        auto state = StateOf(f);
        // nobody has the right: the others go nowhere
        state->SignalInterrupt(gpu::Interrupt::PPF);
        a_none = a.PopAll();
        b_none = b.PopAll();
        state->SignalInterrupt(gpu::Interrupt::PDC0);
        state->SignalInterrupt(gpu::Interrupt::PDC1);
        a_pdc = a.PopAll();
        b_pdc = b.PopAll();
        // thread 1 takes the right
        REQUIRE(b.AcquireRight() == kResultSuccess);
        state->SignalInterrupt(gpu::Interrupt::DMA);
        a_psc = a.PopAll();
        b_psc = b.PopAll();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(a_none.empty());
    CHECK(b_none.empty());
    CHECK(a_pdc == std::vector<int>{2, 3});
    CHECK(b_pdc == std::vector<int>{2, 3});
    CHECK(a_psc.empty());
    CHECK(b_psc == std::vector<int>{6});
}

TEST_CASE("gsp: the sink writes the queue and signals the event at once, whatever delay the GPU passed") {
    // The GPU passes Host::SignalInterrupt a delay; D25 has the service ignore it. The sink takes only
    // the interrupt, so the queue is written when it is called.
    Fixture f;
    TempDir dir;
    gsp::InterruptRelayQueue queue{};
    Result signalled = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        s.AcquireRight();
        StateOf(f)->SignalInterrupt(gpu::Interrupt::P3D);
        queue = s.Queue();
        signalled = g.Wait(s.event, 0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(queue.number_interrupts == 1);
    CHECK(queue.slot[0] == 5);
    CHECK(signalled == kResultSuccess);
}

TEST_CASE("gsp: the GX command queue runs the commands in order and honours stop and should_stop") {
    Fixture f;
    TempDir dir;
    std::vector<gpu::GxCommand> first_run, wrapped, stopped_run;
    u32 header_first = 0, header_wrapped = 0, header_stopped = 0, header_should_stop = 0;
    std::size_t executed_without_right = 99, executed_after_should_stop = 0;
    std::vector<u32> reply_without_right;
    const int status = BootWith(f, dir, {"--ms", "100", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        // no right yet: the call answers success and runs nothing
        s.PutCommands({Gx({5, 1}), Gx({5, 2})});
        reply_without_right = s.Call({0x000C0000}, 2);
        executed_without_right = g_recording->executed.size();
        s.AcquireRight();
        // three commands
        s.PutCommands({Gx({5, 1}), Gx({5, 2}), Gx({5, 3})});
        s.Trigger();
        first_run = g_recording->executed;
        header_first = s.CommandHeader();
        g_recording->executed.clear();
        // the index wraps at 15: commands in slots 14 and 0
        s.PutCommands({Gx({5, 14}), Gx({5, 15})}, 14);
        s.Trigger();
        wrapped = g_recording->executed;
        header_wrapped = s.CommandHeader();
        g_recording->executed.clear();
        // a command with its stop byte set (bits 16-23 of the first word) stops the queue after it
        s.PutCommands({Gx({5 | (1u << 16), 21}), Gx({5, 22}), Gx({5, 23})});
        s.Trigger();
        stopped_run = g_recording->executed;
        header_stopped = s.CommandHeader();
        g_recording->executed.clear();
        // a stopped queue does nothing until the game clears the status
        s.Trigger();
        executed_after_should_stop = g_recording->executed.size();
        // should_stop (bits 24-31) stops it before a command runs
        s.PutCommands({Gx({5, 31})}, 0);
        g.PutWords(s.CommandBuffer(), {(1u << 8) | (1u << 24)});
        s.Trigger();
        header_should_stop = s.CommandHeader();
        executed_after_should_stop += g_recording->executed.size();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply_without_right == std::vector<u32>{0x000C0040, 0});
    CHECK(executed_without_right == 0);
    REQUIRE(first_run.size() == 3);
    CHECK(first_run[0].words[1] == 1);
    CHECK(first_run[1].words[1] == 2);
    CHECK(first_run[2].words[1] == 3);
    CHECK(header_first == 0x00000003);  // index 3, no commands left
    REQUIRE(wrapped.size() == 2);
    CHECK(wrapped[0].words[1] == 14);
    CHECK(wrapped[1].words[1] == 15);
    CHECK(header_wrapped == 0x00000001);  // (14 + 2) % 15
    REQUIRE(stopped_run.size() == 1);
    CHECK(stopped_run[0].words[1] == 21);
    // index 1, two commands left, status 1
    CHECK(header_stopped == (1u | (2u << 8) | (1u << 16)));
    CHECK(executed_after_should_stop == 0);
    CHECK(header_should_stop == ((1u << 8) | (1u << 16) | (1u << 24)));
}

TEST_CASE("gsp: the register commands check their arguments in Azahar's order and reach the GPU") {
    Fixture f;
    TempDir dir;
    std::vector<std::pair<u32, u32>> writes;
    std::map<u32, u32> regs;
    std::vector<Result> results;
    std::vector<u32> read_reply, read_data, read_big, read_bad_addr, read_bad_size, read_capped;
    const int status = BootWith(f, dir, {"--ms", "100", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        const u32 data = g.Scratch(0x100), masks = g.Scratch(0x200), receive = g.Scratch(0x300);
        g.PutWords(data, {0x11111111, 0x22222222, 0x33333333, 0x44444444});
        g.PutWords(masks, {0x0000FFFF, 0xFFFF0000, 0, 0xFFFFFFFF});
        auto write = [&](u32 addr, u32 size) {
            return s.Call({0x00010082, addr, size, ipc::StaticBufferDesc(size, 0), data}, 2)[1];
        };
        auto write_masked = [&](u32 addr, u32 size) {
            return s.Call({0x00020084, addr, size, ipc::StaticBufferDesc(size, 0), data,
                           ipc::StaticBufferDesc(size, 1), masks}, 2)[1];
        };
        // WriteHWRegs: the registers at 0x1EF00010 (offset 0x400010) get two words
        results.push_back(write(0x400010, 8));
        // the checks, in order: address first
        results.push_back(write(0x400011, 8));        // misaligned address
        results.push_back(write(0x420000, 8));        // beyond the window
        results.push_back(write(0x420001, 0x84));     // address first, though the size is bad too
        results.push_back(write(0x400010, 0x84));     // too long
        results.push_back(write(0x400010, 0x81));     // too long first, though misaligned too
        results.push_back(write(0x400010, 6));        // misaligned size
        results.push_back(write(0x400010, 0x80));     // the longest, accepted
        // the writes with a mask start from the register's current value
        g_recording->regs[0x1EF00020] = 0xFFFFFFFF;
        g_recording->regs[0x1EF00024] = 0xFFFFFFFF;
        g_recording->regs[0x1EF00028] = 0x5A5A5A5A;
        writes.clear();
        g_recording->writes.clear();
        results.push_back(write_masked(0x400020, 12));
        writes = g_recording->writes;
        results.push_back(write_masked(0x400021, 12));  // the same checks
        results.push_back(write_masked(0x400020, 0x90));
        // ReadHWRegs
        g_recording->regs[0x1EF00030] = 0xA0A0A0A0;
        g_recording->regs[0x1EF00034] = 0xB0B0B0B0;
        g_recording->regs[0x1EF00038] = 0xC0C0C0C0;
        g.PutWords(g.Cmd() + 4 * 64, {ipc::StaticBufferDesc(0x80, 0), receive});
        g.Put({0x00040080, 0x400030, 12});
        REQUIRE(g.Send(s.handle) == kResultSuccess);
        read_reply = g.Get(4);
        read_data = g.GetWords(receive, 3);
        read_bad_addr = s.Call({0x00040080, 0x400031, 12}, 2);
        read_bad_size = s.Call({0x00040080, 0x400030, 6}, 2);
        read_capped = s.Call({0x00040080, 0x400030, 0x1000}, 4);  // capped at 0x80 bytes: accepted
        regs = g_recording->regs;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(results.size() == 11);
    CHECK(results[0] == kResultSuccess);
    CHECK(results[1] == 0xE0E02A01);
    CHECK(results[2] == 0xE0E02A01);
    CHECK(results[3] == 0xE0E02A01);
    CHECK(results[4] == 0xE0E02BEC);
    CHECK(results[5] == 0xE0E02BEC);
    CHECK(results[6] == 0xE0E02BF2);
    CHECK(results[7] == kResultSuccess);
    CHECK(results[8] == kResultSuccess);
    CHECK(results[9] == 0xE0E02A01);
    CHECK(results[10] == 0xE0E02BEC);
    // the masked write: (current & ~mask) | (value & mask)
    REQUIRE(writes.size() == 3);
    CHECK(writes[0] == std::pair<u32, u32>{0x1EF00020, 0xFFFF1111});
    CHECK(writes[1] == std::pair<u32, u32>{0x1EF00024, 0x2222FFFF});
    CHECK(writes[2] == std::pair<u32, u32>{0x1EF00028, 0x5A5A5A5A});
    CHECK(regs[0x1EF00010] == 0x11111111);
    CHECK(regs[0x1EF00014] == 0x22222222);
    // ReadHWRegs
    CHECK(read_reply[0] == 0x00040042);
    CHECK(read_reply[1] == kResultSuccess);
    CHECK(read_reply[2] == ipc::StaticBufferDesc(12, 0));
    CHECK(read_data == std::vector<u32>{0xA0A0A0A0, 0xB0B0B0B0, 0xC0C0C0C0});
    CHECK(read_bad_addr == std::vector<u32>{0x00040040, 0xE0E02A01});
    CHECK(read_bad_size == std::vector<u32>{0x00040040, 0xE0E02BF2});
    CHECK(read_capped[0] == 0x00040042);
    CHECK(read_capped[1] == kResultSuccess);
}

TEST_CASE("gsp: a register write whose data buffer is shorter than its size is refused") {
    Fixture f;
    TempDir dir;
    Result result = 0;
    std::size_t writes = 99;
    const int status = BootWith(f, dir, {"--ms", "100", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        const u32 data = g.Scratch(0x100);
        g.PutWords(data, {1, 2});
        // 16 bytes asked for, a buffer of 8
        result = s.Call({0x00010082, 0x400010, 16, ipc::StaticBufferDesc(8, 0), data}, 2)[1];
        writes = g_recording->writes.size();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(result == 0xE0E02BEC);
    CHECK(writes == 0);
}

TEST_CASE("gsp: the cache commands tell the GPU what the CPU wrote and what it will read") {
    Fixture f;
    TempDir dir;
    std::vector<std::pair<u32, u32>> flushes, invalidations;
    std::vector<std::vector<u32>> replies;
    const int status = BootWith(f, dir, {"--ms", "100", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        replies.push_back(s.Call({0x00080082, 0x14000000, 0x1000, 0, kCurrentProcess}, 2));  // FlushDataCache
        replies.push_back(s.Call({0x00090082, 0x14002000, 0x400, 0, kCurrentProcess}, 2));   // Invalidate
        replies.push_back(s.Call({0x001F0082, 0x14004000, 0x80, 0, kCurrentProcess}, 2));    // StoreDataCache
        flushes = g_recording->flushes;
        invalidations = g_recording->invalidations;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(replies[0] == std::vector<u32>{0x00080040, 0});
    CHECK(replies[1] == std::vector<u32>{0x00090040, 0});
    CHECK(replies[2] == std::vector<u32>{0x001F0040, 0});
    CHECK(flushes == std::vector<std::pair<u32, u32>>{{0x14000000, 0x1000}, {0x14004000, 0x80}});
    CHECK(invalidations == std::vector<std::pair<u32, u32>>{{0x14002000, 0x400}});
}

TEST_CASE("gsp: SetBufferSwap, SetLcdForceBlack and the simple commands") {
    Fixture f;
    TempDir dir;
    std::vector<std::vector<u32>> replies;
    std::vector<std::pair<u32, gpu::FrameBufferInfo>> swaps;
    std::vector<u32> fills;
    u8 led = 0xFF;
    const int status = BootWith(f, dir, {"--ms", "100", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        replies.push_back(s.Call({0x00050200, 1, 1, 0x1F000000, 0x1F100000, 720, 1, 1, 0}, 2));
        replies.push_back(s.Call({0x000B0040, 1}, 2));
        replies.push_back(s.Call({0x000B0040, 0}, 2));
        replies.push_back(s.Call({0x00100040, 3}, 2));  // SetAxiConfigQoSMode
        replies.push_back(s.Call({0x001E0080, 0x18, 0x19}, 2));  // SetInternalPriorities
        replies.push_back(s.Call({0x001C0040, 1}, 2));  // SetLedForceOff
        led = g.GetBytes(0x1FF81084, 1)[0];
        swaps = g_recording->swaps;
        fills = g_recording->fills;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(replies[0] == std::vector<u32>{0x00050040, 0});
    CHECK(replies[1] == std::vector<u32>{0x000B0040, 0});
    CHECK(replies[2] == std::vector<u32>{0x000B0040, 0});
    CHECK(replies[3] == std::vector<u32>{0x00100040, 0});
    CHECK(replies[4] == std::vector<u32>{0x001E0040, 0});
    CHECK(replies[5] == std::vector<u32>{0x001C0040, 0});
    REQUIRE(swaps.size() == 1);
    CHECK(swaps[0].first == 1);
    CHECK(swaps[0].second.active_fb == 1);
    CHECK(swaps[0].second.address_left == 0x1F000000);
    CHECK(swaps[0].second.address_right == 0x1F100000);
    CHECK(swaps[0].second.stride == 720);
    CHECK(swaps[0].second.format == 1);
    CHECK(swaps[0].second.shown_fb == 1);
    CHECK(fills == std::vector<u32>{0x01000000, 0});  // is_enabled is bit 24
    CHECK(led == 1);
}

TEST_CASE("gsp: the GPU right: acquire, busy, already done, release, and closing the session") {
    Fixture f;
    TempDir dir;
    std::vector<Result> r;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session a(g, gsp.Open().second);
        Session b(g, gsp.Open().second);
        a.Register(0, false);
        b.Register(0, false);
        r.push_back(a.TryAcquireRight());  // 0: success
        r.push_back(a.TryAcquireRight());  // 1: already done
        r.push_back(a.AcquireRight());     // 2: already done, blocking or not
        r.push_back(b.TryAcquireRight());  // 3: busy
        r.push_back(b.AcquireRight());     // 4: busy too (Azahar asserts)
        r.push_back(b.ReleaseRight());     // 5: success even though b has no right
        r.push_back(b.TryAcquireRight());  // 6: a still holds it
        r.push_back(a.ReleaseRight());     // 7
        r.push_back(b.AcquireRight());     // 8: b can have it now
        // a session that closes gives the right back
        g.Close(b.handle);
        Session c(g, gsp.Open().second);
        c.Register(0, false);
        r.push_back(c.TryAcquireRight());  // 9
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(r.size() == 10);
    CHECK(r[0] == kResultSuccess);
    CHECK(r[1] == 0x2BEB);
    CHECK(r[2] == 0x2BEB);
    CHECK(r[3] == 0xC8402BF0);
    CHECK(r[4] == 0xC8402BF0);
    CHECK(r[5] == kResultSuccess);
    CHECK(r[6] == 0xC8402BF0);
    CHECK(r[7] == kResultSuccess);
    CHECK(r[8] == kResultSuccess);
    CHECK(r[9] == kResultSuccess);
}

TEST_CASE("gsp: the framebuffer information is applied at VBlank when it is marked dirty") {
    Fixture f;
    TempDir dir;
    std::vector<std::pair<u32, gpu::FrameBufferInfo>> swaps_after_first, swaps_after_second,
        swaps_after_third;
    std::vector<int> ids;
    bool dirty_top_after = true, dirty_bottom_after = true;
    std::vector<u32> order;
    u64 vblank_tick = 0;
    const int status = BootWith(f, dir, {"--ms", "200", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        const gpu::FrameBufferInfo top0{0, 0x1F000000, 0x1F100000, 720, 1, 0, 0};
        const gpu::FrameBufferInfo top1{1, 0x1F200000, 0x1F300000, 960, 0, 1, 7};
        const gpu::FrameBufferInfo bottom0{0, 0x1F400000, 0, 720, 2, 0, 0};
        // top: the second info is the current one; bottom: the first
        s.SetFrameBuffer(0, 1, true, top0, top1);
        s.SetFrameBuffer(1, 0, true, bottom0, top1);
        g.Sleep(20000000);  // one VBlank (16.7 ms)
        swaps_after_first = g_recording->swaps;
        dirty_top_after = s.FrameBufferDirty(0);
        dirty_bottom_after = s.FrameBufferDirty(1);
        ids = s.PopAll();
        order = g_recording->order;
        vblank_tick = g_recording->vblank_ticks[0].empty() ? 0 : g_recording->vblank_ticks[0][0];
        // nothing dirty: the next VBlank does not swap
        g.Sleep(20000000);
        swaps_after_second = g_recording->swaps;
        // the game marks the top screen dirty again, this time with the first info current
        s.SetFrameBuffer(0, 0, true, top0, top1);
        g.Sleep(20000000);
        swaps_after_third = g_recording->swaps;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // PDC0 swaps screen 0 with the current info of the top screen, PDC1 screen 1
    REQUIRE(swaps_after_first.size() == 2);
    CHECK(swaps_after_first[0].first == 0);
    CHECK(swaps_after_first[0].second.active_fb == 1);
    CHECK(swaps_after_first[0].second.address_left == 0x1F200000);
    CHECK(swaps_after_first[0].second.stride == 960);
    CHECK(swaps_after_first[0].second.unknown == 7);
    CHECK(swaps_after_first[1].first == 1);
    CHECK(swaps_after_first[1].second.address_left == 0x1F400000);
    CHECK(swaps_after_first[1].second.format == 2);
    CHECK_FALSE(dirty_top_after);  // GSP cleared the flags
    CHECK_FALSE(dirty_bottom_after);
    CHECK(ids == std::vector<int>{2, 3});  // PDC0, PDC1
    CHECK(order == std::vector<u32>{0, 1});  // then the GPU is told the refreshes are over
    CHECK(vblank_tick > 0);
    CHECK(swaps_after_second.size() == 2);
    REQUIRE(swaps_after_third.size() == 3);
    CHECK(swaps_after_third[2].first == 0);
    CHECK(swaps_after_third[2].second.address_left == 0x1F000000);
    CHECK(swaps_after_third[2].second.format == 1);
}

TEST_CASE("gsp: ImportDisplayCaptureInfo answers with the cached framebuffer information") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply, reply_again;
    std::size_t swaps_before = 0, swaps_after = 0;
    const int status = BootWith(f, dir, {"--ms", "200", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        s.AcquireRight();
        s.SetFrameBuffer(0, 0, true, {0, 0x1F000000, 0x1F100000, 720, 1, 0, 0}, {});
        s.SetFrameBuffer(1, 0, true, {0, 0x1F400000, 0, 720, 2, 0, 0}, {});
        reply = s.Call({0x00180000}, 10);
        reply_again = s.Call({0x00180000}, 10);
        // The info was taken (the flag is clear) and the next VBlank swaps it in even so
        swaps_before = g_recording->swaps.size();
        g.Sleep(20000000);
        swaps_after = g_recording->swaps.size();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // result, then left, right, format and stride of the top screen and of the bottom one
    CHECK(reply[0] == 0x00180240);
    CHECK(reply[1] == kResultSuccess);
    CHECK(std::vector<u32>(reply.begin() + 2, reply.begin() + 6) ==
          std::vector<u32>{0x1F000000, 0x1F100000, 1, 720});
    CHECK(std::vector<u32>(reply.begin() + 6, reply.end()) ==
          std::vector<u32>{0x1F400000, 0, 2, 720});
    CHECK(reply_again == reply);  // the cache answers again
    CHECK(swaps_before == 0);
    CHECK(swaps_after == 2);  // forced for both screens
}

TEST_CASE("gsp: SaveVramSysArea and RestoreVramSysArea wait for the next bottom-screen VBlank") {
    Fixture f;
    TempDir dir;
    std::vector<u32> denied, saved, restored;
    u64 save_ticks = 0, restore_ticks = 0;
    std::vector<u8> copied_top, copied_bottom, bank3_after_restore, bank7_after_restore, bank0_after_restore;
    std::pair<u32, gpu::FrameBufferInfo> swap_top, swap_bottom;
    std::size_t swaps = 0;
    const int status = BootWith(f, dir, {"--ms", "300", "--gpu", "gsp-test"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session no_permission(g, gsp.Open().second);
        no_permission.Register(0, false);
        denied = no_permission.Call({0x00190000}, 2);
        no_permission.Call({0x00140000}, 2);
        g.Close(no_permission.handle);

        Session s(g, gsp.Open().second);
        s.Register(1);
        s.AcquireRight();
        // a top screen of RGB8 in VRAM, 240 x 400, stride 720, and a bottom one of RGB565
        std::vector<u8> top(720 * 400), bottom(480 * 320);
        for (std::size_t i = 0; i < top.size(); ++i) {
            top[i] = static_cast<u8>(i * 3 + 1);
        }
        for (std::size_t i = 0; i < bottom.size(); ++i) {
            bottom[i] = static_cast<u8>(i * 5 + 2);
        }
        g.PutBytes(0x1F000000, top);
        g.PutBytes(0x1F400000, bottom);
        s.SetFrameBuffer(0, 0, true, {0, 0x1F000000, 0, 720, 1, 0, 0}, {});
        s.SetFrameBuffer(1, 0, true, {0, 0x1F400000, 0, 480, 2, 0, 0}, {});
        // banks 3 (MASK_1) and 7 (MASK_2) of VRAM: 0x1F180000 and 0x1F380000
        g.PutBytes(0x1F180000, std::vector<u8>(16, 0xAA));
        g.PutBytes(0x1F380000, std::vector<u8>(16, 0xBB));
        g.PutBytes(0x1F000000 + 0x7F000, std::vector<u8>(16, 0xCC));  // bank 0: in neither mask
        g_recording->swaps.clear();

        const u64 before = f.runtime->Clock().Now();
        saved = s.Call({0x00190000}, 2);
        save_ticks = f.runtime->Clock().Now() - before;
        copied_top = g.GetBytes(0x1F273000, 16);
        copied_bottom = g.GetBytes(0x1F4C7800, 16);
        // the game changes both banks after the save...
        g.PutBytes(0x1F180000, std::vector<u8>(16, 0x11));
        g.PutBytes(0x1F380000, std::vector<u8>(16, 0x22));
        g.PutBytes(0x1F000000 + 0x7F000, std::vector<u8>(16, 0x33));
        swaps = g_recording->swaps.size();
        if (swaps >= 2) {
            swap_top = g_recording->swaps[0];
            swap_bottom = g_recording->swaps[1];
        }
        // ...and the restore puts the saved contents back
        const u64 before_restore = f.runtime->Clock().Now();
        restored = s.Call({0x001A0000}, 2);
        restore_ticks = f.runtime->Clock().Now() - before_restore;
        bank3_after_restore = g.GetBytes(0x1F180000, 16);
        bank7_after_restore = g.GetBytes(0x1F380000, 16);
        bank0_after_restore = g.GetBytes(0x1F000000 + 0x7F000, 16);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(denied == std::vector<u32>{0x00190040, 0xD8202A06});  // no AllowSaveVramSysArea flag
    CHECK(saved == std::vector<u32>{0x00190040, 0});
    CHECK(restored == std::vector<u32>{0x001A0040, 0});
    // the reply comes with a VBlank: within one period (plus the IPC delay), and not at once
    CHECK(save_ticks <= 4481136 + 100000);
    CHECK(restore_ticks <= 4481136 + 100000);
    // the framebuffers were copied to the save area, at 240 pixels of their own bytes per pixel
    {
        std::vector<u8> expect(16);
        for (std::size_t i = 0; i < 16; ++i) {
            expect[i] = static_cast<u8>(i * 3 + 1);
        }
        CHECK(copied_top == expect);
        std::vector<u8> expect_bottom(16);
        for (std::size_t i = 0; i < 16; ++i) {
            expect_bottom[i] = static_cast<u8>(i * 5 + 2);
        }
        CHECK(copied_bottom == expect_bottom);
    }
    // the VBlank that answered also swapped the saved framebuffers in (forced), top and bottom
    REQUIRE(swaps >= 2);
    CHECK(swap_top.first == 0);
    CHECK(swap_top.second.address_left == 0x1F273000);
    CHECK(swap_top.second.address_right == 0x1F2B9800);
    CHECK(swap_top.second.stride == 720);
    CHECK(swap_bottom.first == 1);
    CHECK(swap_bottom.second.address_left == 0x1F4C7800);
    CHECK(swap_bottom.second.stride == 480);
    // banks 3 and 7 are in the backup, bank 0 is not
    CHECK(bank3_after_restore == std::vector<u8>(16, 0xAA));
    CHECK(bank7_after_restore == std::vector<u8>(16, 0xBB));
    CHECK(bank0_after_restore == std::vector<u8>(16, 0x33));
}

TEST_CASE("gsp: the performance log keeps the VBlank times and counts them per thread") {
    Fixture f;
    TempDir dir;
    std::vector<u32> log;
    const int status = BootWith(f, dir, {"--ms", "200"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        s.Call({0x00110040, 1}, 2);  // SetPerfLogMode on
        g.Sleep(20000000);           // one VBlank
        log = s.Call({0x00120000}, 16);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // Azahar's layout: MakeBuilder(15, 0) and the 14 entries (delta and sum for each of the seven
    // interrupts) pushed raw, with no result word: words 1-14, and word 15 not written (0 here).
    // PDC0 is interrupt 2: delta and sum at words 1 + 4 and 1 + 5. Azahar records the time of a
    // frame (16,713,680 ns) once for each of the four thread indices.
    REQUIRE(log.size() == 16);
    CHECK(log[0] == 0x001203C0);
    CHECK(log[1 + 0] == 0);  // PSC0: no fill yet; also not a result code
    CHECK(log[1 + 1] == 0);
    CHECK(log[1 + 4] == 16713680);
    CHECK(log[1 + 5] == 4 * 16713680);
    CHECK(log[1 + 6] == 16713680);  // PDC1
    CHECK(log[1 + 7] == 4 * 16713680);
    CHECK(log[1 + 12] == 0);  // DMA, the seventh interrupt
    CHECK(log[1 + 13] == 0);
    CHECK(log[15] == 0);
}

// ---- N1-17b: interrupts at once, recorded delays, DMA anywhere ------------------------------------------

namespace {

// Maps the application's heap and the start of the linear heap, as the game would have them, and
// returns the guest address where the linear heap starts.
u32 MapDmaMemory(Runtime& rt) {
    GuestMemory& memory = rt.Memory();
    if (memory.LinearHeapArea() == 0) {
        memory.SetLinearHeapArea(mh4n::kLinearHeapVaddr);
    }
    const u32 linear = memory.LinearHeapArea();
    if (!memory.IsMapped(mh4n::kHeapVaddr, 0x10000)) {
        REQUIRE(IsSuccess(memory.Map(mh4n::kHeapVaddr, 0x10000, MemPerm::ReadWrite, MemState::Private)));
    }
    if (!memory.IsMapped(linear, 0x10000)) {
        REQUIRE(IsSuccess(memory.Map(linear, 0x10000, MemPerm::ReadWrite, MemState::Continuous)));
    }
    return linear;
}

std::vector<u8> Bytes(u32 count, u32 seed) {
    std::vector<u8> bytes(count);
    for (u32 i = 0; i < count; ++i) {
        bytes[i] = static_cast<u8>(seed + i * 7);
    }
    return bytes;
}

}  // namespace

TEST_CASE("gsp: an interrupt the GPU signals with a delay reaches the thread queues in the same tick") {
    // D25 and D41: Azahar's GSP, with the reference's settings, does not wait for the delay the GPU
    // passes; the host posts the interrupt to the sink at once and in the order it was signalled.
    Fixture f;
    TempDir dir;
    std::vector<int> first_thread, second_thread;
    std::vector<u32> first_missed;
    u64 tick_before = 0, tick_after = 0;
    Result first_event = 0, second_event = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session first(g, gsp.Open().second);
        Session second(g, gsp.Open().second);
        first.Register(1);
        REQUIRE(first.AcquireRight() == kResultSuccess);
        // the second session maps the block too, at another address: use its own queue through
        // the same shared memory (one block, thread index 1)
        second.event = g.NewEvent();
        second.registered = second.Call({0x00130042, 1, ipc::CopyHandleDesc(1), second.event}, 5);
        second.thread = second.registered[2];
        REQUIRE(second.thread == 1);
        gpu::Host& host = f.runtime->GpuHost();
        FindServiceManager(*f.runtime)->SetReplyDelayNs(0);  // a request takes no game time
        tick_before = f.runtime->Clock().Now();
        // 20 ms, 1 ms and 7 ms of game time on a console, and a PDC0 that goes to every thread
        host.SignalInterrupt(gpu::Interrupt::DMA, 20000000);
        host.SignalInterrupt(gpu::Interrupt::PSC0, 1000000);
        host.SignalInterrupt(gpu::Interrupt::P3D, 7000000);
        host.SignalInterrupt(gpu::Interrupt::PDC0, 5000000);
        // the next safe point: a request to the service
        first.Call({0x00110040, 0}, 2);  // SetPerfLogMode off
        tick_after = f.runtime->Clock().Now();
        first_thread = first.PopAll();
        second_thread = second.PopAll();
        first_event = g.Wait(first.event, 0);
        second_event = g.Wait(second.event, 0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // in the order they were signalled, none of them later than the others for its delay
    CHECK(first_thread == std::vector<int>{6, 0, 5, 2});
    CHECK(second_thread == std::vector<int>{2});  // PDC0 only: the right is the first session's
    CHECK(tick_after == tick_before);
    CHECK(first_event == kResultSuccess);
    CHECK(second_event == kResultSuccess);
}

TEST_CASE("gsp: the sink is called at once for a delay, from the scheduler thread, in order") {
    Fixture f;
    TempDir dir;
    std::vector<std::pair<gpu::Interrupt, u64>> seen;
    std::vector<u64> delays_outside;
    u64 outside_after = 1;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        gpu::Host& host = f.runtime->GpuHost();
        // a second sink on top of GSP's: it sees the delay the host reports during the call
        f.runtime->SetGpuInterruptSink([&](gpu::Interrupt id) {
            seen.push_back({id, host.InterruptDelayNs()});
        });
        delays_outside.push_back(host.InterruptDelayNs());
        host.SignalInterrupt(gpu::Interrupt::PPF, 123456789);
        host.SignalInterrupt(gpu::Interrupt::DMA, 5);
        host.SignalInterrupt(gpu::Interrupt::P3D, 0);
        g.Sleep(1000);  // a safe point
        outside_after = host.InterruptDelayNs();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == std::make_pair(gpu::Interrupt::PPF, u64{123456789}));
    CHECK(seen[1] == std::make_pair(gpu::Interrupt::DMA, u64{5}));
    CHECK(seen[2] == std::make_pair(gpu::Interrupt::P3D, u64{0}));
    CHECK(delays_outside == std::vector<u64>{0});
    CHECK(outside_after == 0);
}

TEST_CASE("gsp: a DMA from the application's heap to the linear heap and to VRAM copies the bytes") {
    Fixture f;
    TempDir dir;
    std::vector<u8> to_linear, to_vram, source_a, source_b;
    std::vector<int> popped;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const u32 linear = MapDmaMemory(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        REQUIRE(s.AcquireRight() == kResultSuccess);
        source_a = Bytes(0x1000, 3);
        source_b = Bytes(0x800, 91);
        g.PutBytes(mh4n::kHeapVaddr + 0x100, source_a);
        g.PutBytes(mh4n::kHeapVaddr + 0x2000, source_b);
        s.PutCommands({Gx({0, mh4n::kHeapVaddr + 0x100, linear + 0x4000, 0x1000}),
                       Gx({0, mh4n::kHeapVaddr + 0x2000, 0x1F000000 + 0x3000, 0x800})});
        REQUIRE(s.Trigger() == kResultSuccess);
        to_linear = g.GetBytes(linear + 0x4000, 0x1000);
        to_vram = g.GetBytes(0x1F000000 + 0x3000, 0x800);
        popped = s.PopAll();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(to_linear == source_a);
    CHECK(to_vram == source_b);
    CHECK(popped == std::vector<int>{6, 6});
    CHECK_FALSE(f.Logged("DMA with an address"));
    CHECK_FALSE(f.Logged("leaves the memory"));
}

TEST_CASE("gsp: a DMA into the heap, from VRAM or the linear heap, and within the heap, copies the bytes") {
    Fixture f;
    TempDir dir;
    std::vector<u8> from_linear, from_vram, within, linear_source, vram_source, heap_source;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const u32 linear = MapDmaMemory(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        REQUIRE(s.AcquireRight() == kResultSuccess);
        linear_source = Bytes(0x400, 5);
        vram_source = Bytes(0x200, 17);
        heap_source = Bytes(0x100, 40);
        g.PutBytes(linear + 0x100, linear_source);
        g.PutBytes(0x1F000000 + 0x500, vram_source);
        g.PutBytes(mh4n::kHeapVaddr + 0x6000, heap_source);
        s.PutCommands({Gx({0, linear + 0x100, mh4n::kHeapVaddr + 0x20, 0x400}),
                       Gx({0, 0x1F000000 + 0x500, mh4n::kHeapVaddr + 0x1000, 0x200}),
                       Gx({0, mh4n::kHeapVaddr + 0x6000, mh4n::kHeapVaddr + 0x7000, 0x100})});
        REQUIRE(s.Trigger() == kResultSuccess);
        from_linear = g.GetBytes(mh4n::kHeapVaddr + 0x20, 0x400);
        from_vram = g.GetBytes(mh4n::kHeapVaddr + 0x1000, 0x200);
        within = g.GetBytes(mh4n::kHeapVaddr + 0x7000, 0x100);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(from_linear == linear_source);
    CHECK(from_vram == vram_source);
    CHECK(within == heap_source);
}

TEST_CASE("gsp: a DMA with an unmapped range copies nothing, does not crash, and signals DMA") {
    Fixture f;
    TempDir dir;
    std::vector<u8> vram_after, heap_after;
    std::vector<int> popped;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const u32 linear = MapDmaMemory(*f.runtime);
        (void)linear;
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        REQUIRE(s.AcquireRight() == kResultSuccess);
        g.PutBytes(0x1F000000 + 0x100, Bytes(0x100, 1));
        g.PutBytes(mh4n::kHeapVaddr, Bytes(0x100, 2));
        // from an unmapped range to VRAM, from the heap to an unmapped range, and a range that
        // starts in the heap and runs off its end
        s.PutCommands({Gx({0, 0x0C000000, 0x1F000000 + 0x100, 0x100}),
                       Gx({0, mh4n::kHeapVaddr, 0x0D000000, 0x100}),
                       Gx({0, mh4n::kHeapVaddr + 0xFFF0, 0x1F000000 + 0x100, 0x100})});
        REQUIRE(s.Trigger() == kResultSuccess);
        vram_after = g.GetBytes(0x1F000000 + 0x100, 0x100);
        heap_after = g.GetBytes(mh4n::kHeapVaddr, 0x100);
        popped = s.PopAll();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(vram_after == Bytes(0x100, 1));
    CHECK(heap_after == Bytes(0x100, 2));
    CHECK(popped == std::vector<int>{6, 6, 6});
    CHECK(f.LoggedCount("DMA with an address that is not physical memory") == 3);
}

TEST_CASE("gsp: GetPerfLog records the delays of the GPU's interrupts, as Azahar's PerformanceRecorder does") {
    Fixture f;
    TempDir dir;
    std::vector<u32> first, second;
    std::vector<int> popped;
    const int status = BootWith(f, dir, {"--ms", "200"}, [&] {
        Guest g(*f.runtime);
        const u32 linear = MapDmaMemory(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        REQUIRE(s.AcquireRight() == kResultSuccess);
        FindServiceManager(*f.runtime)->SetReplyDelayNs(0);
        s.Call({0x00110040, 1}, 2);  // SetPerfLogMode on
        // a command list with one finalize, in VRAM
        g.PutWords(0x1F000000 + 0x8000, {0x12345678, 0x000F0010});
        s.PutCommands(
            {FillCommand(0x1F000000, 0xAABBCCDD, 0x1F001000),  // PSC0: 0x1000 bytes VRAM to VRAM
             Gx({2, 0, 0, 0, 0x1F000000, 1, 0x1F000000 + 0x1000, 0x0201u << 16}),  // PSC1, the same
             Gx({3, 0x1F000000, linear, 64u | (32u << 16), 64u | (32u << 16), 0}),  // PPF: 8192 bytes
             Gx({0, 0x1F000000, 0x1F000000 + 0x4000, 0x1000}),  // DMA: VRAM to VRAM
             Gx({1, 0x1F000000 + 0x8000, 8, 0, 0, 0, 0, 1})});  // P3D
        REQUIRE(s.Trigger() == kResultSuccess);
        first = s.Call({0x00120000}, 16);
        // a second DMA: delta is the new delay, sum has both
        s.PutCommands({Gx({0, 0x1F000000, 0x1F000000 + 0x4000, 0x800})});
        REQUIRE(s.Trigger() == kResultSuccess);
        second = s.Call({0x00120000}, 16);
        popped = s.PopAll();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // Azahar's layout: delta and sum for each interrupt, at words 1 + 2 * id: PSC0 0, PSC1 1,
    // PDC0 2, PDC1 3, PPF 4, P3D 5, DMA 6. The time of a fill, a transfer or a DMA is
    // size / speed (DelayGenerator); P3D's is made of the list's commands and triangles, which the
    // null GPU does not count: 0.
    REQUIRE(first.size() == 16);
    CHECK(first[1 + 0] == 723);
    CHECK(first[1 + 1] == 723);
    CHECK(first[1 + 2] == 723);
    CHECK(first[1 + 3] == 723);
    CHECK(first[1 + 8] == 1446);
    CHECK(first[1 + 9] == 1446);
    CHECK(first[1 + 10] == 0);
    CHECK(first[1 + 11] == 0);
    CHECK(first[1 + 12] == 10279);
    CHECK(first[1 + 13] == 10279);
    // the VBlanks are not in it yet: no frame has passed
    CHECK(first[1 + 4] == 0);
    CHECK(first[1 + 6] == 0);
    REQUIRE(second.size() == 16);
    CHECK(second[1 + 12] == 5139);
    CHECK(second[1 + 13] == 10279 + 5139);
    CHECK(second[1 + 0] == 723);  // untouched
    CHECK(popped == std::vector<int>{0, 1, 4, 6, 5, 6});
}

TEST_CASE("gsp: the performance log records the delay the sink is given, and only for a thread that gets it") {
    Fixture f;
    TempDir dir;
    std::vector<u32> log;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1);
        s.Call({0x00110040, 1}, 2);
        const std::shared_ptr<gsp::GspState> state = StateOf(f);
        // no thread has the GPU right: Azahar returns before it records anything
        state->SignalInterrupt(gpu::Interrupt::PPF, 777);
        REQUIRE(s.AcquireRight() == kResultSuccess);
        state->SignalInterrupt(gpu::Interrupt::PPF, 1000);
        state->SignalInterrupt(gpu::Interrupt::PPF, 2345);
        // the delay of a VBlank is the frame's, whatever is passed
        state->SignalInterrupt(gpu::Interrupt::PDC1, 99);
        log = s.Call({0x00120000}, 16);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(log.size() == 16);
    CHECK(log[1 + 8] == 2345);
    CHECK(log[1 + 9] == 3345);
    CHECK(log[1 + 6] == 16713680);
}

TEST_CASE("gsp: gsp::Lcd gives a session and answers its commands as unimplemented ones") {
    Fixture f;
    TempDir dir;
    std::pair<std::vector<u32>, Handle> lcd;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        lcd = gsp.Open("gsp::Lcd");
        reply = g.Request(lcd.second, {0x000F0000}, 2);  // PowerOnAllBacklights
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(lcd.second != 0);
    CHECK(reply == std::vector<u32>{0x000F0040, 0});
    CHECK(f.Logged("unimplemented gsp::Lcd"));
}

TEST_CASE("gsp: the shared memory is a block the kernel allocates, mapped with 'don't care'") {
    Fixture f;
    TempDir dir;
    Result with_permissions = 0, mapped = 0;
    std::vector<u8> contents;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Gsp gsp(g);
        Session s(g, gsp.Open().second);
        s.Register(1, false);
        // an automatically allocated block maps only with other permissions DontCare
        with_permissions = CallSvc(*f.runtime, 0x1F, {s.registered[4], kShm, 3, 3}).r[0];
        mapped = g.MapBlock(s.registered[4], kShm);
        contents = g.GetBytes(kShm, 0x1000);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(with_permissions == 0xE0E01BEE);  // kResultInvalidCombination
    CHECK(mapped == kResultSuccess);
    CHECK(contents == std::vector<u8>(0x1000, 0));
}

#endif  // recomp.h
