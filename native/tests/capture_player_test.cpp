// Derived from Azahar src/video_core/gpu.cpp, src/common/color.h,
// src/video_core/renderer_software/renderer_software.cpp, src/video_core/pica/regs_external.h,
// regs_framebuffer.h and pica_core.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of those files says "GPLv2 or any later version"): the expected
// values, worked out from GPU::VirtualToPhysicalAddress, the colour decoders and the software
// renderer's loop over a displayed screen (including its off-by-one column), and the word
// positions of the display, colour-buffer, fill and transfer registers the test writes.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, regs_external.h,
//   pica_core.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h)
// Copyright 2014 Citra Emulator Project (color.h)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (renderer_software.cpp)
//
// mh4n_gpu_capture (N2-02): the MGPU reader, the capture host, the image thresholds and the
// capture player, with captures made by a C++ writer in this file from synthetic data.
//
// The writer mirrors tools/native/gcap.py (Writer). The first test also runs `gcap.py check` on a
// capture this writer produced, when python3 and the script are there. The expected screens are
// worked out in this file from the algorithm of Azahar's software renderer (see image.cpp), not
// by calling the code under test.
#include <doctest/doctest.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gpu_capture/capture_host.h"
#include "gpu_capture/check.h"
#include "gpu_capture/image.h"
#include "gpu_capture/mgpu.h"
#include "gpu_capture/player.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

#if defined(_WIN32)
#include <process.h>
#define MH4N_GETPID _getpid
#else
#include <unistd.h>
#define MH4N_GETPID getpid
#endif

using namespace mh4n;
using namespace mh4n::gpu_capture;
namespace fs = std::filesystem;

// ===============================================================================================
// Things registered from this directory (not from gpu_capture), as another WP would.
// ===============================================================================================

namespace {

struct TestState {
    std::vector<std::string> log;           // what the "test-log" Gpu was asked, in order
    std::vector<u32> probe;                 // "test-corrupt": the byte at probe_address at each Execute
    u32 probe_address = 0;
    int irq_mode = 0;                       // "test-irq": 0 as the capture, 1 wrong id, 2 none, 3 extra
    int regs_mode = 0;                      // "test-regs": 0 as decoded, 1 other value, 2 short, 3 long, 4 unsupported
    int block_mode = 0;                     // "test-block": 0 none, 1 a 16x16 block off by 10, 2 one wrong pixel
    u32 block_address = 0;                  // the colour buffer (RGBA8, 50 tiles wide) it damages
    bool corrupt_first_fill = false;
    u32 test_opt_seen = 0;
    int check_records = 0;
};
TestState g;

// Forwards everything to another Gpu (and its GpuDebug); the test Gpus override what they change.
class Forward : public gpu::Gpu, public gpu::GpuDebug {
public:
    explicit Forward(std::unique_ptr<gpu::Gpu> inner)
        : inner_(std::move(inner)), inner_debug_(dynamic_cast<gpu::GpuDebug*>(inner_.get())) {}

    void Execute(const gpu::GxCommand& c) override { inner_->Execute(c); }
    void WriteReg(u32 a, u32 v) override { inner_->WriteReg(a, v); }
    u32 ReadReg(u32 a) override { return inner_->ReadReg(a); }
    void SetBufferSwap(u32 s, const gpu::FrameBufferInfo& i) override { inner_->SetBufferSwap(s, i); }
    void SetColorFill(u32 f) override { inner_->SetColorFill(f); }
    void FlushRegion(u32 a, u32 s) override { inner_->FlushRegion(a, s); }
    void InvalidateRegion(u32 a, u32 s) override { inner_->InvalidateRegion(a, s); }
    void VBlank(u32 s) override { inner_->VBlank(s); }

    void LoadInitState(const gpu::InitState& s) override {
        if (inner_debug_) inner_debug_->LoadInitState(s);
    }
    std::optional<std::vector<gpu::RegisterWrite>> TakeRegisterWrites() override {
        return inner_debug_ ? inner_debug_->TakeRegisterWrites() : std::nullopt;
    }
    void WriteBackColorBuffers() override {
        if (inner_debug_) inner_debug_->WriteBackColorBuffers();
    }
    std::vector<gpu::Counter> TakeFrameCounters() override {
        return inner_debug_ ? inner_debug_->TakeFrameCounters() : std::vector<gpu::Counter>{};
    }
    std::optional<u64> TakeFrameGpuTimeNs() override {
        return inner_debug_ ? inner_debug_->TakeFrameGpuTimeNs() : std::nullopt;
    }

protected:
    std::unique_ptr<gpu::Gpu> inner_;
    gpu::GpuDebug* inner_debug_;
};

std::unique_ptr<gpu::Gpu> MakeReplay(gpu::Host& host, const Options& options) {
    const gpu::GpuFactory* factory = Registry<gpu::GpuFactory>::Instance().Find("replay");
    return (*factory)(host, options);
}

// "test-log": records every call it gets, then replays the capture.
class LogGpu final : public Forward {
public:
    using Forward::Forward;
    void Execute(const gpu::GxCommand& c) override {
        g.log.push_back("execute:" + std::to_string(c.words[0] & 0xFF));
        Forward::Execute(c);
    }
    void WriteReg(u32 a, u32 v) override {
        g.log.push_back(fmt::format("write_reg:{:x}={:x}", a, v));
        Forward::WriteReg(a, v);
    }
    void SetBufferSwap(u32 s, const gpu::FrameBufferInfo& i) override {
        g.log.push_back(fmt::format("swap:{}:{:x}:{}", s, i.address_left, i.stride));
        Forward::SetBufferSwap(s, i);
    }
    void SetColorFill(u32 f) override {
        g.log.push_back(fmt::format("fill:{:x}", f));
        Forward::SetColorFill(f);
    }
    void FlushRegion(u32 a, u32 s) override {
        g.log.push_back(fmt::format("flush:{:x}:{:x}", a, s));
        Forward::FlushRegion(a, s);
    }
    void InvalidateRegion(u32 a, u32 s) override {
        g.log.push_back(fmt::format("invalidate:{:x}:{:x}", a, s));
        Forward::InvalidateRegion(a, s);
    }
    void VBlank(u32 s) override {
        g.log.push_back("vblank:" + std::to_string(s));
        Forward::VBlank(s);
    }
    void LoadInitState(const gpu::InitState& s) override {
        // Every block is there, with the size the format gives it, and carries the marker the
        // test put at its start.
        bool ok = true;
        for (u32 kind = 1; kind <= gpu::kInitBlockCount; ++kind) {
            const auto block = s.Block(static_cast<gpu::InitBlock>(kind));
            ok = ok && block.size() == InitBlockSize(kind) && block[0] == kind && block[1] == 0xA5;
        }
        g.log.push_back(ok ? "init:ok" : "init:BAD");
        Forward::LoadInitState(s);
    }
};
MH4N_REGISTER(gpu::GpuFactory, "test-log", [](gpu::Host& host, const Options& options) {
    return std::unique_ptr<gpu::Gpu>(std::make_unique<LogGpu>(MakeReplay(host, options)));
});

// "test-corrupt": replays, but damages the first memory fill's first page; records the byte it
// finds at `probe_address` when each command starts.
class CorruptGpu final : public Forward {
public:
    CorruptGpu(std::unique_ptr<gpu::Gpu> inner, gpu::Host& host)
        : Forward(std::move(inner)), host_(host) {}
    void Execute(const gpu::GxCommand& c) override {
        g.probe.push_back(host_.PhysicalPointer(g.probe_address, 1)[0]);
        Forward::Execute(c);
        if (g.corrupt_first_fill && (c.words[0] & 0xFF) == kGxMemoryFill && !done_) {
            host_.PhysicalPointer(g.probe_address, 1)[0] ^= 0xFF;
            done_ = true;
        }
    }

private:
    gpu::Host& host_;
    bool done_ = false;
};
MH4N_REGISTER(gpu::GpuFactory, "test-corrupt", [](gpu::Host& host, const Options& options) {
    return std::unique_ptr<gpu::Gpu>(std::make_unique<CorruptGpu>(MakeReplay(host, options), host));
});

// "test-block": replays, then damages the colour buffer at block_address after each command list.
class BlockGpu final : public Forward {
public:
    BlockGpu(std::unique_ptr<gpu::Gpu> inner, gpu::Host& host)
        : Forward(std::move(inner)), host_(host) {}
    void Execute(const gpu::GxCommand& c) override {
        Forward::Execute(c);
        if ((c.words[0] & 0xFF) != kGxSubmitCmdList || g.block_mode == 0) {
            return;
        }
        u8* buffer = host_.PhysicalPointer(g.block_address, 400 * 240 * 4);
        if (g.block_mode == 1) {
            // The tiles of the 16x16 block at (0, 0): 0, 1, 50 and 51 (50 tiles per row).
            for (u32 tile : {0u, 1u, 50u, 51u}) {
                for (u32 m = 0; m < 64; ++m) {
                    for (u32 c2 = 1; c2 <= 3; ++c2) {
                        buffer[tile * 256 + m * 4 + c2] += 10;
                    }
                }
            }
        } else {
            for (u32 c2 = 1; c2 <= 3; ++c2) {
                buffer[c2] = 255; // pixel (0, 0); the capture has 0 there
            }
        }
    }

private:
    gpu::Host& host_;
};
MH4N_REGISTER(gpu::GpuFactory, "test-block", [](gpu::Host& host, const Options& options) {
    return std::unique_ptr<gpu::Gpu>(std::make_unique<BlockGpu>(MakeReplay(host, options), host));
});

// "test-irq": replays the pages itself and signals the capture's interrupts wrongly.
class IrqGpu final : public gpu::Gpu {
public:
    explicit IrqGpu(CaptureHost& host) : host_(host) {}
    void Execute(const gpu::GxCommand&) override { Go(); }
    void WriteReg(u32, u32) override {}
    u32 ReadReg(u32) override { return 0; }
    void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}

private:
    void Go() {
        const EventReference* e = host_.CurrentEvent();
        for (const PageData& page : e->pages) {
            std::memcpy(host_.PhysicalPointer(page.address, kPageSize), page.data.data(), kPageSize);
        }
        for (const IrqData& irq : e->interrupts) {
            if (g.irq_mode == 0) {
                host_.SignalInterrupt(static_cast<gpu::Interrupt>(irq.id), irq.delay_ns);
            } else if (g.irq_mode == 1) {
                host_.SignalInterrupt(gpu::Interrupt::DMA, irq.delay_ns);
            } else if (g.irq_mode == 3) {
                host_.SignalInterrupt(static_cast<gpu::Interrupt>(irq.id), irq.delay_ns);
                host_.SignalInterrupt(gpu::Interrupt::PSC1, irq.delay_ns);
            }
        }
    }
    CaptureHost& host_;
};
MH4N_REGISTER(gpu::GpuFactory, "test-irq", [](gpu::Host& host, const Options&) {
    return std::unique_ptr<gpu::Gpu>(std::make_unique<IrqGpu>(dynamic_cast<CaptureHost&>(host)));
});

// "test-regs": replays, and reports the register writes wrongly.
class RegsGpu final : public Forward {
public:
    using Forward::Forward;
    std::optional<std::vector<gpu::RegisterWrite>> TakeRegisterWrites() override {
        auto writes = Forward::TakeRegisterWrites();
        if (!writes) return writes;
        switch (g.regs_mode) {
        case 1:
            if (!writes->empty()) (*writes)[0].value ^= 1;
            break;
        case 2:
            if (!writes->empty()) writes->pop_back();
            break;
        case 3:
            if (!writes->empty()) writes->push_back({0x100, 0xF, 0});
            break;
        case 4:
            return std::nullopt;
        default:
            break;
        }
        return writes;
    }
};
MH4N_REGISTER(gpu::GpuFactory, "test-regs", [](gpu::Host& host, const Options& options) {
    return std::unique_ptr<gpu::Gpu>(std::make_unique<RegsGpu>(MakeReplay(host, options)));
});

// "test-counter": replays and reports counters and a device time each frame.
class CounterGpu final : public Forward {
public:
    using Forward::Forward;
    std::vector<gpu::Counter> TakeFrameCounters() override {
        return {{"ubershader_draws", 3}, {"hd_textures_ready", 12}};
    }
    std::optional<u64> TakeFrameGpuTimeNs() override { return 1500000; }
};
MH4N_REGISTER(gpu::GpuFactory, "test-counter", [](gpu::Host& host, const Options& options) {
    return std::unique_ptr<gpu::Gpu>(std::make_unique<CounterGpu>(MakeReplay(host, options)));
});

// A wrapper, turned on by --test-wrap: adds a counter of its own and forwards the rest.
class WrapGpu final : public Forward {
public:
    using Forward::Forward;
    std::vector<gpu::Counter> TakeFrameCounters() override {
        auto counters = Forward::TakeFrameCounters();
        counters.push_back({"wrapper_marker", 7});
        return counters;
    }
};
MH4N_OPTION("capture-player", "--test-wrap", "", "test wrapper (capture_player_test)");
MH4N_REGISTER(gpu::GpuWrapper, "test-wrap",
              [](std::unique_ptr<gpu::Gpu> inner, gpu::Host&, const Options&) {
                  return std::unique_ptr<gpu::Gpu>(std::make_unique<WrapGpu>(std::move(inner)));
              });

// A check with a column, and an option of its own.
MH4N_OPTION("capture-player", "--test-opt", "N", "test option (capture_player_test)");
class TestCheck final : public CaptureCheck {
public:
    explicit TestCheck(u32 option) : option_(option) {}
    std::vector<std::string> Columns() const override { return {"my_column"}; }
    std::string OnRecord(size_t, const Record&) override {
        ++g.check_records;
        return {};
    }
    std::string OnFrame(const FrameInfo& frame, FrameColumns& columns) override {
        columns.Set("my_column", fmt::format("opt{}_f{}", option_, frame.frame));
        return {};
    }

private:
    u32 option_;
};
MH4N_REGISTER(CheckFactory, "test-check", [](const Options& options) {
    g.test_opt_seen = static_cast<u32>(options.GetUInt("--test-opt", 5));
    return std::unique_ptr<CaptureCheck>(std::make_unique<TestCheck>(g.test_opt_seen));
});

} // namespace

// ===============================================================================================
// The capture writer (mirrors tools/native/gcap.py's Writer) and the scenario helpers.
// ===============================================================================================

namespace {

using Bytes = std::vector<u8>;

void Put16(Bytes& b, u32 v) {
    b.push_back(static_cast<u8>(v));
    b.push_back(static_cast<u8>(v >> 8));
}
void Put32(Bytes& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void Put64(Bytes& b, u64 v) {
    Put32(b, static_cast<u32>(v));
    Put32(b, static_cast<u32>(v >> 32));
}

constexpr u32 kTopCols = 400, kBottomCols = 320, kRows = 240;
constexpr u32 kTopFb = 0x18000000, kBottomFb = 0x18060000; // physical, in VRAM
constexpr u32 kColorBuffer = 0x20100000;                    // 400x240 RGBA8, 94 pages
constexpr u32 kColorBufferPages = 94;

class W {
public:
    W() {
        out.insert(out.end(), {'M', 'G', 'P', 'U'});
        Put32(out, 1);
    }

    size_t Rec(u32 type, const Bytes& payload) {
        Put32(out, type);
        Put32(out, frame);
        Put64(out, ticks += 1000);
        Put32(out, static_cast<u32>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        return count++;
    }

    // INIT with zero blocks, each starting with {kind, 0xA5} so a test can tell them apart.
    size_t Init(const std::map<u32, Bytes>& overrides = {}) {
        Bytes payload;
        Put32(payload, 11);
        for (u32 kind = 1; kind <= 11; ++kind) {
            Bytes block(InitBlockSize(kind), 0);
            block[0] = static_cast<u8>(kind);
            block[1] = 0xA5;
            auto it = overrides.find(kind);
            if (it != overrides.end()) block = it->second;
            Put32(payload, kind);
            Put32(payload, static_cast<u32>(block.size()));
            payload.insert(payload.end(), block.begin(), block.end());
        }
        return Rec(kInit, payload);
    }

    size_t Mem(u32 addr, const Bytes& data) {
        Bytes p;
        Put32(p, addr);
        Put32(p, static_cast<u32>(data.size()));
        p.insert(p.end(), data.begin(), data.end());
        return Rec(kMem, p);
    }

    static Bytes Pages(const std::vector<std::pair<u32, Bytes>>& pages) {
        Bytes p;
        Put32(p, static_cast<u32>(pages.size()));
        for (const auto& [addr, data] : pages) {
            Put32(p, addr);
            p.insert(p.end(), data.begin(), data.end());
        }
        return p;
    }
    size_t CpuPages(const std::vector<std::pair<u32, Bytes>>& pages) { return Rec(kCpuPages, Pages(pages)); }
    size_t GpuPages(const std::vector<std::pair<u32, Bytes>>& pages) { return Rec(kGpuPages, Pages(pages)); }

    size_t Gx(std::array<u32, 8> words) {
        Bytes p;
        for (u32 w : words) Put32(p, w);
        return Rec(kGx, p);
    }
    size_t PicaReg(const std::vector<std::array<u32, 3>>& writes) { // {reg, mask, value}
        Bytes p;
        Put32(p, static_cast<u32>(writes.size()));
        for (const auto& w : writes) {
            Put16(p, w[0]);
            p.push_back(static_cast<u8>(w[1]));
            p.push_back(0);
            Put32(p, w[2]);
        }
        return Rec(kPicaReg, p);
    }
    size_t Mmio(u32 addr, u32 value) {
        Bytes p;
        Put32(p, addr);
        Put32(p, value);
        return Rec(kMmio, p);
    }
    size_t Swap(u32 screen, std::array<u32, 7> info) {
        Bytes p;
        Put32(p, screen);
        for (u32 w : info) Put32(p, w);
        return Rec(kSwap, p);
    }
    size_t Fill(u32 word) {
        Bytes p;
        Put32(p, word);
        return Rec(kFill, p);
    }
    size_t Irq(u32 id, u64 delay = 1000) {
        Bytes p;
        Put32(p, id);
        Put64(p, delay);
        return Rec(kIrq, p);
    }
    size_t Flush(u32 kind, u32 vaddr, u32 size) {
        Bytes p;
        Put32(p, kind);
        Put32(p, vaddr);
        Put32(p, size);
        return Rec(kFlush, p);
    }
    size_t FrameMarker(u32 number) {
        frame = number;
        Bytes p;
        Put32(p, number);
        return Rec(kFrame, p);
    }

    bool Save(const fs::path& path) const {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<bool>(f);
    }

    Bytes out;
    u32 frame = 0;
    u64 ticks = 0;
    size_t count = 0;
};

Bytes Pattern(size_t size, u32 seed) { // bytes below 200, so a +10 never wraps
    Bytes b(size);
    for (size_t i = 0; i < size; ++i) b[i] = static_cast<u8>((i * 7 + seed * 13 + (i >> 8)) % 200);
    return b;
}

void SetWord(Bytes& block, u32 index, u32 value) {
    for (int i = 0; i < 4; ++i) block[index * 4 + i] = static_cast<u8>(value >> (8 * i));
}

// The GPU_REGS block with the two framebuffers configured (RGBA8, 240 pixels per column).
Bytes ScreenRegs() {
    Bytes regs(InitBlockSize(1), 0);
    regs[0] = 1;
    regs[1] = 0xA5;
    // word 0 and 1 hold the marker; the display registers are far from them.
    const u32 tops[2] = {0x100, 0x140};
    const u32 cols[2] = {kTopCols, kBottomCols};
    const u32 addrs[2] = {kTopFb, kBottomFb};
    for (int s = 0; s < 2; ++s) {
        SetWord(regs, tops[s] + 0x17, (cols[s] << 16) | kRows);
        SetWord(regs, tops[s] + 0x1A, addrs[s]);
        SetWord(regs, tops[s] + 0x1C, 0);        // RGBA8
        SetWord(regs, tops[s] + 0x1E, 0);        // first buffer
        SetWord(regs, tops[s] + 0x24, kRows * 4);
    }
    return regs;
}

// The memory a screen holds, then what the software renderer shows of it (worked out here).
struct World {
    Bytes top, bottom; // the framebuffers' bytes
};

World MakeWorld() {
    return {Pattern(kTopCols * kRows * 4, 1), Pattern(kBottomCols * kRows * 4, 2)};
}

Image ModelScreen(const Bytes& fb, u32 cols) {
    Image image;
    image.width = cols;
    image.height = kRows;
    image.rgb.assign(static_cast<size_t>(cols) * kRows * 3, 0);
    for (u32 x = 0; x < kRows; ++x) {
        for (u32 y = 0; y < cols; ++y) {
            const size_t index = static_cast<size_t>(y) * kRows + kRows - x; // Azahar's quirk
            if ((index + 1) * 4 > fb.size()) continue;                       // past the buffer: black
            const u8* px = &fb[index * 4];
            u8* out = &image.rgb[(static_cast<size_t>(x) * cols + y) * 3];
            out[0] = px[3];
            out[1] = px[2];
            out[2] = px[1];
        }
    }
    return image;
}

// INIT, the framebuffers in VRAM, a zeroed megabyte or two of FCRAM.
size_t BeginScene(W& w, const World& world) {
    const size_t init = w.Init({{1, ScreenRegs()}});
    Bytes vram(0xB0000, 0);
    std::copy(world.top.begin(), world.top.end(), vram.begin());
    std::copy(world.bottom.begin(), world.bottom.end(), vram.begin() + 0x60000);
    w.Mem(kTopFb, vram);
    w.Mem(0x20000000, Bytes(0x200000, 0));
    return init;
}

struct Indexes {
    size_t fill = 0, transfer = 0, draw = 0, frame = 0;
};

void AddFill(W& w, Indexes& at) {
    at.fill = w.Gx({kGxMemoryFill, 0x14000000, 0xAB, 0x14001000, 0, 0, 0, 0x201});
    w.GpuPages({{0x20000000, Bytes(kPageSize, 0xAB)}});
    w.Irq(0); // PSC0
}

void AddTransfer(W& w, Indexes& at) {
    at.transfer = w.Gx({kGxDisplayTransfer, 0x14010000, 0x14020000, 0, 0, 0x1000, 0, 0});
    w.GpuPages({{0x20020000, Pattern(kPageSize, 5)}, {0x20021000, Pattern(kPageSize, 6)}});
    w.Irq(4); // PPF
}

std::vector<std::pair<u32, Bytes>> ColorBufferPages() {
    std::vector<std::pair<u32, Bytes>> pages;
    for (u32 i = 0; i < kColorBufferPages; ++i) {
        Bytes page = Pattern(kPageSize, 10 + i);
        if (i == 0) std::fill(page.begin(), page.begin() + 4, u8{0}); // pixel (0, 0) is black
        pages.emplace_back(kColorBuffer + i * kPageSize, page);
    }
    return pages;
}

std::vector<std::array<u32, 3>> DrawRegisters(u32 fog_mode = 0) {
    std::vector<std::array<u32, 3>> writes = {
        {0x117, 0xF, 0},                                  // RGBA8
        {0x11D, 0xF, kColorBuffer >> 3},                  // address / 8
        {0x11E, 0xF, kTopCols | ((kRows - 1) << 12)},     // 400 x 240
    };
    if (fog_mode != 0) writes.push_back({0x0E0, 0x1, fog_mode});
    writes.push_back({0x22E, 0xF, 1});                    // draw
    return writes;
}

void AddDraw(W& w, Indexes& at, u32 fog_mode = 0) {
    at.draw = w.Gx({kGxSubmitCmdList, 0x14100000, 0x400, 0, 0, 0, 0, 1});
    w.PicaReg(DrawRegisters(fog_mode));
    w.GpuPages(ColorBufferPages());
    w.Irq(5); // P3D
}

void WritePpmFile(const fs::path& path, const Image& image) {
    std::string error;
    REQUIRE_MESSAGE(WritePpm(path.string(), image, &error), error);
}

// A scratch directory per test case.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                fmt::format("mh4n_capture_player_test_{}_{}", MH4N_GETPID(), counter++);
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        if (std::getenv("MH4N_TEST_KEEP") != nullptr) {
            return; // leave the captures there to look at (they are synthetic)
        }
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }
    fs::path File(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

struct Built {
    std::string path;
    Indexes at;
};

// Frame image files for `world`: the screens as the software renderer shows them.
void WriteReferenceFrames(const TempDir& dir, const std::string& capture, u32 frame, const World& world,
                          const std::function<void(Image&)>& damage_top = {}) {
    const fs::path frames = fs::path(capture + ".frames");
    fs::create_directories(frames);
    Image top = ModelScreen(world.top, kTopCols);
    if (damage_top) damage_top(top);
    WritePpmFile(frames / fmt::format("{}_top.ppm", frame), top);
    WritePpmFile(frames / fmt::format("{}_bottom.ppm", frame), ModelScreen(world.bottom, kBottomCols));
    (void)dir;
}

PlayerResult Run(const std::string& capture, std::vector<std::string> args) {
    args.push_back(capture);
    const Options options = ParseOptions("capture-player", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    PlayerConfig config;
    std::string error;
    REQUIRE_MESSAGE(ConfigFromOptions(options, config, &error), error);
    return RunPlayer(config, options);
}

std::string Describe(const PlayerResult& r) {
    return FormatReport(r);
}

// The full scene: fill, transfer, a draw, flushes, VBlanks, one frame; with its reference images.
Built FullCapture(const TempDir& dir, const std::function<void(Image&)>& damage_top = {},
                  u32 fog_mode = 0) {
    Built built;
    built.path = dir.File("full.mgpu").string();
    const World world = MakeWorld();
    W w;
    BeginScene(w, world);
    w.Fill(0);
    AddFill(w, built.at);
    w.Irq(2); // PDC0
    AddTransfer(w, built.at);
    w.Swap(0, {0, 0x1F000000, 0, kRows * 4, 0, 0, 0}); // virtual VRAM address
    w.Irq(3);                                           // PDC1
    w.Flush(kFlushDataCache, 0x14000000, 0x1000);
    AddDraw(w, built.at, fog_mode);
    w.CpuPages({{0x20030000, Pattern(kPageSize, 9)}});
    built.at.frame = w.FrameMarker(1);
    REQUIRE(w.Save(built.path));
    WriteReferenceFrames(dir, built.path, 1, world, damage_top);
    return built;
}

} // namespace

// ===============================================================================================
// The reader
// ===============================================================================================

TEST_CASE("the reader reads what the writer wrote, and gcap.py accepts it") {
    TempDir dir;
    const Built built = FullCapture(dir);

    Reader reader;
    std::string error;
    REQUIRE_MESSAGE(reader.Open(built.path, &error), error);
    Record record;
    std::vector<u32> types;
    while (reader.Next(record, &error) == Reader::Status::Record) {
        CHECK(CheckRecord(record).empty());
        types.push_back(record.type);
    }
    CHECK(error.empty());
    // INIT, MEM x2, FILL, GX, GPU_PAGES, IRQ, IRQ, GX, GPU_PAGES, IRQ, SWAP, IRQ, FLUSH, GX,
    // PICA_REG, GPU_PAGES, IRQ, CPU_PAGES, FRAME
    const std::vector<u32> expected = {kInit, kMem, kMem, kFill, kGx, kGpuPages, kIrq, kIrq, kGx,
                                       kGpuPages, kIrq, kSwap, kIrq, kFlush, kGx, kPicaReg,
                                       kGpuPages, kIrq, kCpuPages, kFrame};
    CHECK(types == expected);

#if !defined(_WIN32)
    // The reference implementation of the format must accept the same file.
    const fs::path gcap = fs::path(__FILE__).parent_path() / ".." / ".." / "tools" / "native" / "gcap.py";
    if (fs::exists(gcap) && std::system("python3 --version > /dev/null 2>&1") == 0) {
        const std::string command =
            fmt::format("python3 \"{}\" check \"{}\" > /dev/null 2>&1", gcap.string(), built.path);
        CHECK_MESSAGE(std::system(command.c_str()) == 0, "gcap.py check rejected the capture");
    } else {
        MESSAGE("python3 or tools/native/gcap.py not found; the cross-check was skipped");
    }
#endif
}

TEST_CASE("the reader refuses what gcap.py refuses") {
    TempDir dir;
    std::string error;
    {
        Reader reader;
        CHECK_FALSE(reader.Open(dir.File("missing.mgpu").string(), &error));
        CHECK(error.find("cannot open") != std::string::npos);
    }
    {
        std::ofstream(dir.File("magic.mgpu"), std::ios::binary).write("NOPE\1\0\0\0", 8);
        Reader reader;
        CHECK_FALSE(reader.Open(dir.File("magic.mgpu").string(), &error));
        CHECK(error.find("magic") != std::string::npos);
    }
    {
        W w;
        w.out[4] = 2; // version 2
        REQUIRE(w.Save(dir.File("version.mgpu")));
        Reader reader;
        CHECK_FALSE(reader.Open(dir.File("version.mgpu").string(), &error));
        CHECK(error.find("version 2") != std::string::npos);
    }
    {
        W w;
        w.Init();
        w.out.resize(w.out.size() - 10); // damaged payload
        REQUIRE(w.Save(dir.File("cut.mgpu")));
        Reader reader;
        REQUIRE(reader.Open(dir.File("cut.mgpu").string(), &error));
        Record record;
        CHECK(reader.Next(record, &error) == Reader::Status::Error);
        CHECK(error.find("truncated") != std::string::npos);
    }
    {
        W w;
        w.Init();
        w.out.resize(w.out.size() - w.out.size() + 8 + 7); // part of a record header
        REQUIRE(w.Save(dir.File("cut2.mgpu")));
        Reader reader;
        REQUIRE(reader.Open(dir.File("cut2.mgpu").string(), &error));
        Record record;
        CHECK(reader.Next(record, &error) == Reader::Status::Error);
    }
    // Payload rules (gcap.py check_record).
    {
        Record record;
        record.type = kGx;
        record.payload.assign(31, 0);
        CHECK_FALSE(CheckRecord(record).empty());
        record = Record();
        record.type = 13;
        CHECK_FALSE(CheckRecord(record).empty());
        record = Record();
        record.type = kFrame;
        record.frame = 5;
        Put32(record.payload, 6);
        CHECK_FALSE(CheckRecord(record).empty()); // payload differs from the header's frame
        record = Record();
        record.type = kFlush;
        record.payload = {9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK_FALSE(CheckRecord(record).empty()); // unknown kind
        record = Record();
        record.type = kPicaReg;
        Put32(record.payload, 1);
        Put16(record.payload, 0x300); // out of range
        record.payload.push_back(0xF);
        record.payload.push_back(0);
        Put32(record.payload, 0);
        CHECK_FALSE(CheckRecord(record).empty());
        record = Record();
        record.type = kCpuPages;
        Put32(record.payload, 1);
        Put32(record.payload, 0x20000010); // not page aligned
        record.payload.resize(record.payload.size() + kPageSize);
        CHECK_FALSE(CheckRecord(record).empty());
    }
    // An INIT with a block kind twice, or one missing.
    {
        Record init;
        init.type = kInit;
        Put32(init.payload, 10);
        for (u32 kind = 1; kind <= 11; ++kind) {
            if (kind == 5) continue;
            Put32(init.payload, kind);
            Put32(init.payload, InitBlockSize(kind));
            init.payload.resize(init.payload.size() + InitBlockSize(kind));
        }
        CHECK(CheckRecord(init).find("blocks") != std::string::npos);
    }
}

// ===============================================================================================
// The capture host
// ===============================================================================================

TEST_CASE("the capture host: memory areas, virtual addresses, interrupts") {
    CaptureHost host;
    CHECK(host.PhysicalPointer(kFcramPaddr, 0x1000) != nullptr);
    CHECK(host.PhysicalPointer(kVramPaddr + kVramSize - 4, 4) != nullptr);
    CHECK(host.PhysicalPointer(kVramPaddr + kVramSize - 4, 8) == nullptr); // past the area
    CHECK(host.PhysicalPointer(kDspRamPaddr, kDspRamSize) != nullptr);
    CHECK(host.PhysicalPointer(0x10000000, 4) == nullptr);
    CHECK(host.PhysicalPointer(0xFFFFFFFC, 8) == nullptr); // no wrap-around

    // Azahar's VirtualToPhysicalAddress
    CHECK(host.VirtualToPhysical(0x1F000010) == 0x18000010u);
    CHECK(host.VirtualToPhysical(0x14000040) == 0x20000040u);
    CHECK(host.VirtualToPhysical(0x30000040) == 0x20000040u);
    CHECK(host.VirtualToPhysical(0x20000040) == 0x20000040u); // already physical
    CHECK_FALSE(host.VirtualToPhysical(0).has_value());
    CHECK_FALSE(host.VirtualToPhysical(0x08000000).has_value());

    // MEM goes to both copies, a GPU page only to the reference until it is applied.
    const Bytes data = Pattern(64, 3);
    REQUIRE(host.WriteBoth(0x20000100, data.data(), 64));
    CHECK(std::memcmp(host.PhysicalPointer(0x20000100, 64), data.data(), 64) == 0);
    CHECK(std::memcmp(host.ReferencePointer(0x20000100, 64), data.data(), 64) == 0);
    CHECK_FALSE(host.WriteBoth(0x10000000, data.data(), 64));
    Bytes page = Pattern(kPageSize, 4);
    PageData page_data{0x20000000, std::span<const u8, kPageSize>(page.data(), kPageSize)};
    REQUIRE(host.ApplyReferencePage(page_data));
    CHECK(std::memcmp(host.ReferencePointer(0x20000000, kPageSize), page.data(), kPageSize) == 0);
    CHECK(std::memcmp(host.PhysicalPointer(0x20000000, kPageSize), page.data(), kPageSize) != 0);
    REQUIRE(host.ApplyPageToGpu(page_data));
    CHECK(std::memcmp(host.PhysicalPointer(0x20000000, kPageSize), page.data(), kPageSize) == 0);

    // Interrupts, and Post runs at once.
    host.SignalInterrupt(gpu::Interrupt::P3D, 5);
    host.SignalInterrupt(gpu::Interrupt::PPF, 6);
    bool ran = false;
    host.Post([&] { ran = true; });
    CHECK(ran);
    const auto signalled = host.TakeSignalled();
    REQUIRE(signalled.size() == 2);
    CHECK(signalled[0].id == gpu::Interrupt::P3D);
    CHECK(signalled[1].delay_ns == 6);
    CHECK(host.TakeSignalled().empty());
}

// ===============================================================================================
// Images, formats and thresholds
// ===============================================================================================

namespace {
Image Flat(u32 w, u32 h, u8 value) {
    Image image;
    image.width = w;
    image.height = h;
    image.rgb.assign(static_cast<size_t>(w) * h * 3, value);
    return image;
}
void Offset(Image& image, u32 x0, u32 y0, u32 size, int delta) {
    for (u32 y = y0; y < y0 + size; ++y)
        for (u32 x = x0; x < x0 + size; ++x)
            for (int c = 0; c < 3; ++c) image.rgb[(static_cast<size_t>(y) * image.width + x) * 3 + c] += delta;
}
} // namespace

TEST_CASE("thresholds: a block off by 10 fails the block rule while PSNR stays above 40 dB") {
    const Image reference = Flat(400, 240, 100);
    Image native = reference;
    Offset(native, 32, 48, 16, 10);
    const ImageMetrics m = CompareImages(reference, native);
    REQUIRE(m.comparable);
    // MSE = 100 * 256 / 96000, so PSNR = 10 log10(255^2 / 0.2667) = 53.87 dB
    CHECK(m.psnr == doctest::Approx(53.87).epsilon(0.001));
    CHECK(m.psnr >= kMinPsnrDb);
    CHECK(m.worst_block == doctest::Approx(10.0));
    CHECK(m.worst_block_x == 32);
    CHECK(m.worst_block_y == 48);
    CHECK_FALSE(m.Passes());
}

TEST_CASE("thresholds: one fully wrong pixel passes both") {
    Image reference = Flat(400, 240, 0);
    Image native = reference;
    for (int c = 0; c < 3; ++c) native.rgb[(static_cast<size_t>(100) * 400 + 200) * 3 + c] = 255;
    const ImageMetrics m = CompareImages(reference, native);
    // MSE = 255^2 / 96000 = 0.677, PSNR = 49.8 dB; the block mean is 255 / 256 = 0.996
    CHECK(m.psnr == doctest::Approx(49.8).epsilon(0.002));
    CHECK(m.worst_block == doctest::Approx(255.0 / 256.0));
    CHECK(m.Passes());
}

TEST_CASE("thresholds: the limits themselves") {
    const Image reference = Flat(400, 240, 100);
    {
        Image native = reference; // exactly 8 on every channel of one block: allowed
        Offset(native, 0, 0, 16, 8);
        CHECK(CompareImages(reference, native).worst_block == doctest::Approx(8.0));
        CHECK(CompareImages(reference, native).Passes());
        Offset(native, 0, 0, 16, 1); // 9
        CHECK_FALSE(CompareImages(reference, native).Passes());
    }
    {
        Image native = reference; // identical: infinite PSNR
        const ImageMetrics m = CompareImages(reference, native);
        CHECK(std::isinf(m.psnr));
        CHECK(m.worst_block == 0.0);
        CHECK(m.Passes());
    }
    {
        Image native = reference; // everything off by 4: no block above 8, but PSNR is 36 dB
        Offset(native, 0, 0, 16, 0);
        for (auto& v : native.rgb) v += 4;
        const ImageMetrics m = CompareImages(reference, native);
        CHECK(m.psnr == doctest::Approx(36.09).epsilon(0.001));
        CHECK_FALSE(m.Passes());
    }
    {
        const ImageMetrics m = CompareImages(Flat(10, 10, 0), Flat(11, 10, 0)); // sizes differ
        CHECK_FALSE(m.comparable);
        CHECK_FALSE(m.Passes());
    }
    {
        // A partial block at the edge averages over the pixels it has: 4x4 pixels off by 9.
        Image a = Flat(20, 20, 50), b = a;
        Offset(b, 16, 16, 4, 9);
        CHECK(CompareImages(a, b).worst_block == doctest::Approx(9.0));
    }
}

TEST_CASE("display formats decode as Azahar's common/color.h") {
    const u8 rgba8[4] = {0xFF, 0x22, 0x33, 0x44}; // A B G R in memory
    CHECK(DecodeDisplayPixel(kRGBA8, rgba8).r == 0x44);
    CHECK(DecodeDisplayPixel(kRGBA8, rgba8).g == 0x33);
    CHECK(DecodeDisplayPixel(kRGBA8, rgba8).b == 0x22);
    const u8 rgb8[3] = {0x11, 0x22, 0x33}; // B G R
    CHECK(DecodeDisplayPixel(kRGB8, rgb8).r == 0x33);
    CHECK(DecodeDisplayPixel(kRGB8, rgb8).b == 0x11);
    const u8 rgb565[2] = {0x00, 0xF8}; // 0xF800: red
    CHECK(DecodeDisplayPixel(kRGB565, rgb565).r == 255);
    CHECK(DecodeDisplayPixel(kRGB565, rgb565).g == 0);
    const u8 green565[2] = {0xE0, 0x07}; // 0x07E0
    CHECK(DecodeDisplayPixel(kRGB565, green565).g == 255);
    const u8 rgb5a1[2] = {0x3E, 0x00}; // 0x003E: blue 31, alpha 0
    CHECK(DecodeDisplayPixel(kRGB5A1, rgb5a1).b == 255);
    CHECK(DecodeDisplayPixel(kRGB5A1, rgb5a1).r == 0);
    const u8 rgba4[2] = {0x0F, 0xA0}; // 0xA00F: red 0xA -> 0xAA
    CHECK(DecodeDisplayPixel(kRGBA4, rgba4).r == 0xAA);
    CHECK(DecodeDisplayPixel(kRGBA4, rgba4).g == 0);
    CHECK(BytesPerPixel(kRGBA8) == 4);
    CHECK(BytesPerPixel(kRGB8) == 3);
    CHECK(BytesPerPixel(kRGB565) == 2);
    CHECK(BytesPerPixel(7) == 0);
}

TEST_CASE("colour buffers: registers, 8x8 Morton tiles and the colour formats") {
    std::array<u32, kNumPicaRegs> regs{};
    regs[0x117] = 3u << 16;                          // RGB565
    regs[0x11D] = 0x20100000u >> 3;
    regs[0x11E] = 400 | (239u << 12) | (1u << 24);
    const ColorBufferConfig c = ColorBufferFromRegisters(regs.data());
    CHECK(c.address == 0x20100000u);
    CHECK(c.format == 3);
    CHECK(c.width == 400);
    CHECK(c.height == 240);
    CHECK(c.ByteSize() == 50u * 30u * 64u * 2u);

    // 16x8 RGBA8: two tiles. Pixel (x, y) of tile 0 is at ((x&1) | (y&1)<<1 | (x&2)<<1 | ...) * 4.
    ColorBufferConfig config;
    config.address = 0x20000000;
    config.format = 0;
    config.width = 16;
    config.height = 8;
    Bytes data(config.ByteSize(), 0);
    auto put = [&](size_t offset, u8 r) { data[offset + 3] = r; };
    put(0 * 4, 1);     // (0, 0)
    put(1 * 4, 2);     // (1, 0)
    put(2 * 4, 3);     // (0, 1)
    put(4 * 4, 4);     // (2, 0)
    put(8 * 4, 5);     // (0, 2)
    put(16 * 4, 6);    // (4, 0)
    put(32 * 4, 7);    // (0, 4)
    put(63 * 4, 8);    // (7, 7)
    put(256 + 0, 9);   // tile 1: (8, 0)
    put(256 + 5 * 4, 10); // tile 1: x = 1+4 = 5? bits: m = 5 -> x0 = 1, y0 = 0, x1 = 1 -> (3, 0): column 8 + 3
    const Image image = DecodeColorBuffer(data.data(), config);
    auto red = [&](u32 x, u32 y) { return image.rgb[(static_cast<size_t>(y) * 16 + x) * 3]; };
    CHECK(red(0, 0) == 1);
    CHECK(red(1, 0) == 2);
    CHECK(red(0, 1) == 3);
    CHECK(red(2, 0) == 4);
    CHECK(red(0, 2) == 5);
    CHECK(red(4, 0) == 6);
    CHECK(red(0, 4) == 7);
    CHECK(red(7, 7) == 8);
    CHECK(red(8, 0) == 9);
    CHECK(red(8 + 3, 0) == 10); // m = 5: x bits 0 and 1 set -> 3, y 0
}

TEST_CASE("the displayed screen: orientation, Azahar's off-by-one column, colour fill") {
    DisplayShadow shadow;
    std::vector<u8> zeros_gpu(DisplayShadow::kGpuRegWords * 4, 0), zeros_lcd(DisplayShadow::kLcdRegWords * 4, 0);
    shadow.Load(zeros_gpu.data(), zeros_lcd.data());
    // 3 columns of 2 pixels, RGBA8: stride 8 bytes. MMIO sets the size (height field = columns).
    shadow.WriteReg(DisplayShadow::kGpuBase + 0x400 + 0x17 * 4, (3u << 16) | 2u);
    gpu::FrameBufferInfo info{0, 0x18000000, 0, 8, 0, 0, 0};
    shadow.ApplySwap(0, info, 0x18000000, 0);

    // pixels 0..6: the 7th is the one Azahar reads past the last column
    std::vector<u8> memory(7 * 4);
    for (int i = 0; i < 7; ++i) {
        memory[i * 4 + 3] = static_cast<u8>(10 + i); // R
        memory[i * 4 + 2] = static_cast<u8>(20 + i); // G
        memory[i * 4 + 1] = static_cast<u8>(30 + i); // B
    }
    auto fn = [&](u32 address, u32 size) -> const u8* {
        const u32 base = 0x18000000;
        if (address < base || address - base + size > memory.size()) return nullptr;
        return memory.data() + (address - base);
    };
    Image image;
    std::string error;
    REQUIRE_MESSAGE(shadow.BuildScreen(0, fn, image, &error), error);
    CHECK(image.width == 3);  // the columns stored
    CHECK(image.height == 2); // the pixels per column
    // output (row x, column y) is memory pixel y * 2 + 2 - x
    const int expect[2][3] = {{2, 4, 6}, {1, 3, 5}};
    for (u32 x = 0; x < 2; ++x) {
        for (u32 y = 0; y < 3; ++y) {
            const u8* p = &image.rgb[(static_cast<size_t>(x) * 3 + y) * 3];
            CHECK(p[0] == 10 + expect[x][y]);
            CHECK(p[1] == 20 + expect[x][y]);
            CHECK(p[2] == 30 + expect[x][y]);
        }
    }
    // Memory that ends at the last column: the pixel past it is black.
    memory.resize(6 * 4);
    REQUIRE(shadow.BuildScreen(0, fn, image, &error));
    CHECK(image.rgb[(0 * 3 + 2) * 3] == 0);
    CHECK(image.rgb[(1 * 3 + 2) * 3] == 15);

    // A screen with its colour fill on is that colour, whatever the memory holds.
    shadow.ApplyFill((1u << 24) | (0x30u << 16) | (0x20u << 8) | 0x10u);
    REQUIRE(shadow.BuildScreen(0, fn, image, &error));
    for (size_t i = 0; i < image.rgb.size(); i += 3) {
        CHECK(image.rgb[i] == 0x10);
        CHECK(image.rgb[i + 1] == 0x20);
        CHECK(image.rgb[i + 2] == 0x30);
    }
    // The second buffer is used when active_fb is not 0.
    shadow.ApplyFill(0);
    info.active_fb = 1;
    info.shown_fb = 1;
    shadow.ApplySwap(0, info, 0x18000020, 0);
    CHECK_FALSE(shadow.BuildScreen(0, fn, image, &error)); // 0x18000020 is outside this memory
    CHECK(error.find("not in memory") != std::string::npos);
}

TEST_CASE("PPM files round-trip and refuse what they cannot read") {
    TempDir dir;
    Image image = Flat(5, 3, 7);
    image.rgb[4] = 99;
    std::string error;
    REQUIRE(WritePpm(dir.File("a.ppm").string(), image, &error));
    Image back;
    REQUIRE_MESSAGE(ReadPpm(dir.File("a.ppm").string(), back, &error), error);
    CHECK(back.width == 5);
    CHECK(back.height == 3);
    CHECK(back.rgb == image.rgb);
    {
        std::ofstream f(dir.File("comment.ppm"), std::ios::binary);
        f << "P6\n# a comment\n2 1\n255\n";
        f.write("\1\2\3\4\5\6", 6);
    }
    REQUIRE(ReadPpm(dir.File("comment.ppm").string(), back, &error));
    CHECK(back.rgb == Bytes({1, 2, 3, 4, 5, 6}));
    {
        std::ofstream f(dir.File("short.ppm"), std::ios::binary);
        f << "P6\n2 2\n255\n12345";
    }
    CHECK_FALSE(ReadPpm(dir.File("short.ppm").string(), back, &error));
    {
        std::ofstream f(dir.File("p3.ppm"), std::ios::binary);
        f << "P3\n1 1\n255\n0 0 0\n";
    }
    CHECK_FALSE(ReadPpm(dir.File("p3.ppm").string(), back, &error));
    CHECK_FALSE(ReadPpm(dir.File("none.ppm").string(), back, &error));
}

// ===============================================================================================
// The player
// ===============================================================================================

TEST_CASE("replay passes the whole scene, with the frame images and --check registers") {
    TempDir dir;
    const Built built = FullCapture(dir);
    const PlayerResult r = Run(built.path, {"--gpu", "replay", "--check", "registers"});
    INFO(Describe(r));
    CHECK(r.error.empty());
    CHECK(r.failures.empty());
    CHECK(r.ExitStatus() == 0);
    CHECK(r.records == 20);
    CHECK(r.judged.at("fill") == 1);
    CHECK(r.judged.at("transfer") == 1);
    CHECK(r.judged.at("cmdlist") == 1);
    REQUIRE(r.frames.size() == 1);
    const FrameRow& row = r.frames[0];
    CHECK(row.frame == 1);
    CHECK(row.judged);
    CHECK(row.passed);
    CHECK(std::isinf(row.top.metrics.psnr));
    CHECK(std::isinf(row.bottom.metrics.psnr));
    CHECK(row.top.metrics.worst_block == 0.0);
    CHECK(row.cpu_ns > 0);
    CHECK_FALSE(row.gpu_ns.has_value());
    const std::string text = FormatReport(r);
    CHECK(text.find("result: PASS") != std::string::npos);
    CHECK(text.find("cpu_ms") != std::string::npos);
    CHECK(text.find("reg_writes") != std::string::npos);
    CHECK(text.find("psnr") != std::string::npos);
}

TEST_CASE("none fails at the first draw, and passes when nothing is judged") {
    TempDir dir;
    // Only a draw (no fill before it), so the first failure is the draw itself.
    Built built;
    built.path = dir.File("draw.mgpu").string();
    const World world = MakeWorld();
    W w;
    BeginScene(w, world);
    w.Flush(kFlushDataCache, 0x14000000, 0x1000);
    AddDraw(w, built.at);
    REQUIRE(w.Save(built.path));

    const PlayerResult r = Run(built.path, {"--gpu", "none"});
    INFO(Describe(r));
    REQUIRE(r.error.empty());
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().record == built.at.draw);
    CHECK(r.failures.front().record_name == "GX SUBMIT_CMDLIST");
    CHECK(r.failures.front().message.find("colour buffer 0x20100000") != std::string::npos);
    CHECK(r.ExitStatus() == 1);

    const PlayerResult none = Run(built.path, {"--gpu", "none", "--judge", "none"});
    INFO(Describe(none));
    CHECK(none.error.empty());
    CHECK(none.failures.empty());
    CHECK(none.ExitStatus() == 0);
    CHECK(none.skipped.at("cmdlist") == 1);
    CHECK(none.judged.empty());

    // With a fill and a transfer in the scene `none` fails at the fill, bit for bit.
    const Built full = FullCapture(dir);
    const PlayerResult f = Run(full.path, {"--gpu", "none"});
    REQUIRE_FALSE(f.failures.empty());
    CHECK(f.failures.front().record == full.at.fill);
    CHECK(f.failures.front().message.find("1 of 1 pages") != std::string::npos);
    // ... and `--judge cmdlist` skips them and fails at the draw.
    const PlayerResult c = Run(full.path, {"--gpu", "none", "--judge", "cmdlist"});
    REQUIRE_FALSE(c.failures.empty());
    CHECK(c.failures.front().record == full.at.draw);
    CHECK(c.skipped.at("fill") == 1);
    CHECK(c.skipped.at("transfer") == 1);
}

TEST_CASE("a colour-buffer block off by 10 fails the block rule; one wrong pixel passes") {
    TempDir dir;
    const Built built = FullCapture(dir);
    g.block_address = kColorBuffer;

    g.block_mode = 1;
    PlayerResult r = Run(built.path, {"--gpu", "test-block", "--judge", "cmdlist"});
    INFO(Describe(r));
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().record == built.at.draw);
    CHECK(r.failures.front().message.find("block at (0, 0)") != std::string::npos);
    CHECK(r.failures.front().message.find("53.87") != std::string::npos); // PSNR stays above 40

    g.block_mode = 2;
    r = Run(built.path, {"--gpu", "test-block", "--judge", "cmdlist"});
    INFO(Describe(r));
    CHECK(r.error.empty());
    CHECK(r.failures.empty());
    g.block_mode = 0;
}

TEST_CASE("frames: the same thresholds on the displayed screens") {
    TempDir dir;
    {
        // A block off by 10 in the reference top screen.
        const Built built = FullCapture(dir, [](Image& top) { Offset(top, 16, 32, 16, 10); });
        const PlayerResult r = Run(built.path, {"--gpu", "replay"});
        INFO(Describe(r));
        REQUIRE(r.frames.size() == 1);
        CHECK_FALSE(r.frames[0].passed);
        CHECK(r.frames[0].top.metrics.psnr >= kMinPsnrDb);
        CHECK(r.frames[0].top.metrics.worst_block == doctest::Approx(10.0).epsilon(0.1)); // pattern clamps
        REQUIRE_FALSE(r.failures.empty());
        CHECK(r.failures.front().record == built.at.frame);
        CHECK(r.failures.front().record_name == "FRAME");
        CHECK(r.failures.front().message.find("top screen") != std::string::npos);
        // Not judging frames reports them without failing.
        const PlayerResult unjudged = Run(built.path, {"--gpu", "replay", "--judge", "fill,transfer,copy,dma,cmdlist"});
        CHECK(unjudged.failures.empty());
        REQUIRE(unjudged.frames.size() == 1);
        CHECK_FALSE(unjudged.frames[0].judged);
        CHECK_FALSE(unjudged.frames[0].top.metrics.Passes());
        CHECK(FormatReport(unjudged).find("not-judged") != std::string::npos);
    }
    {
        // One badly wrong pixel (every channel inverted) passes both thresholds.
        const Built built = FullCapture(dir, [](Image& top) {
            for (int c = 0; c < 3; ++c) top.rgb[(static_cast<size_t>(5) * 400 + 77) * 3 + c] ^= 0xFF;
        });
        const PlayerResult r = Run(built.path, {"--gpu", "replay"});
        INFO(Describe(r));
        CHECK(r.failures.empty());
        REQUIRE(r.frames.size() == 1);
        CHECK(r.frames[0].passed);
        CHECK(r.frames[0].top.metrics.psnr >= kMinPsnrDb);
        CHECK(r.frames[0].top.metrics.psnr < 80);   // it was measured
        CHECK(r.frames[0].top.metrics.worst_block > 0.0);
        CHECK(r.frames[0].top.metrics.worst_block < 2.0);
    }
    {
        // No reference images: judged frames fail.
        const Built built = FullCapture(dir);
        fs::remove_all(built.path + ".frames");
        const PlayerResult r = Run(built.path, {"--gpu", "replay"});
        REQUIRE_FALSE(r.failures.empty());
        CHECK(r.failures.front().message.find("no reference image") != std::string::npos);
        const PlayerResult ok = Run(built.path, {"--gpu", "replay", "--judge", "all", "--frames-dir", (dir.path() / "elsewhere").string()});
        CHECK_FALSE(ok.failures.empty());
    }
}

TEST_CASE("gas frames are reported and, with --skip-gas-frames, not judged") {
    TempDir dir;
    const Built built =
        FullCapture(dir, [](Image& top) { Offset(top, 16, 32, 16, 10); }, /*fog_mode=*/7);
    PlayerResult r = Run(built.path, {"--gpu", "replay"});
    REQUIRE(r.frames.size() == 1);
    CHECK(r.frames[0].gas);
    CHECK_FALSE(r.frames[0].passed); // judged
    r = Run(built.path, {"--gpu", "replay", "--skip-gas-frames"});
    INFO(Describe(r));
    CHECK(r.failures.empty());
    REQUIRE(r.frames.size() == 1);
    CHECK(r.frames[0].gas);
    CHECK_FALSE(r.frames[0].judged);
    CHECK(r.frames[0].passed);
    CHECK(r.frames[0].top.compared); // still measured
    CHECK(FormatReport(r).find("gas-skipped") != std::string::npos);
}

TEST_CASE("the INIT state reaches the Gpu before the first command, and every call is forwarded") {
    TempDir dir;
    Built built;
    built.path = dir.File("calls.mgpu").string();
    W w;
    w.Init({{1, ScreenRegs()}});
    w.Mem(kTopFb, Pattern(0x2000, 1));
    w.Mem(0x20000000, Bytes(0x10000, 0));
    w.Fill(0x01000000 | 0x336699);
    w.Mmio(0x1EF00404, 0x1234);
    w.Swap(1, {1, 0x1F000000, 0x1F100000, 720, 1, 1, 0});
    w.Gx({kGxCacheFlush, 0, 0, 0, 0, 0, 0, 0});
    w.Irq(2); // PDC0 -> VBlank(0)
    w.Irq(3); // PDC1 -> VBlank(1)
    w.Flush(kFlushDataCache, 0x14000000, 0x100);
    w.Flush(kInvalidateDataCache, 0x14000100, 0x200);
    w.Flush(kStoreDataCache, 0x14000200, 0x300);
    w.Flush(kCacheFlushRegion, 0x14000300, 0x400);
    w.FrameMarker(2);
    REQUIRE(w.Save(built.path));

    g.log.clear();
    const PlayerResult r = Run(built.path, {"--gpu", "test-log", "--judge", "none"});
    INFO(Describe(r));
    CHECK(r.error.empty());
    const std::vector<std::string> expected = {
        "init:ok",
        "fill:1336699",
        "write_reg:1ef00404=1234",
        "swap:1:1f000000:720",
        "execute:5",
        "vblank:0",
        "vblank:1",
        "flush:14000000:100",     // kind 1: FlushRegion
        "invalidate:14000100:200", // kind 2: InvalidateRegion
        "flush:14000200:300",     // kind 3: FlushRegion
        "flush:14000300:400",     // kind 4: FlushRegion
    };
    CHECK(g.log == expected);
}

TEST_CASE("CPU_PAGES and GPU_PAGES reach the Gpu's memory as the modes say") {
    TempDir dir;
    // MEM, a CPU page, a fill that writes a page, then a second command. The "test-corrupt" Gpu
    // damages the fill's page and logs the byte it sees at each command.
    Built built;
    built.path = dir.File("modes.mgpu").string();
    W w;
    w.Init();
    w.Mem(0x20000000, Bytes(0x10000, 0x11));
    w.CpuPages({{0x20001000, Bytes(kPageSize, 0x22)}});
    w.Gx({kGxMemoryFill, 0x14000000, 0, 0x14001000, 0, 0, 0, 0});
    w.GpuPages({{0x20000000, Bytes(kPageSize, 0xAB)}});
    w.Irq(0);
    w.Gx({kGxCacheFlush, 0, 0, 0, 0, 0, 0, 0});
    w.Gx({kGxCacheFlush, 0, 0, 0, 0, 0, 0, 0});
    REQUIRE(w.Save(built.path));

    g.probe_address = 0x20000000;
    g.corrupt_first_fill = true;

    // Chained (the default): the fill fails and the damage stays for the next commands.
    g.probe.clear();
    PlayerResult r = Run(built.path, {"--gpu", "test-corrupt"});
    INFO(Describe(r));
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().message.find("1 of 1 pages") != std::string::npos);
    REQUIRE(g.probe.size() == 3);
    CHECK(g.probe[0] == 0x11);        // MEM
    CHECK(g.probe[1] == (0xABu ^ 0xFFu)); // the Gpu's own (wrong) output
    CHECK(g.probe[2] == (0xABu ^ 0xFFu));

    // Isolated: the reference page replaces the Gpu's output once the command is judged.
    g.probe.clear();
    r = Run(built.path, {"--gpu", "test-corrupt", "--isolated"});
    REQUIRE_FALSE(r.failures.empty());
    REQUIRE(g.probe.size() == 3);
    CHECK(g.probe[1] == 0xAB);
    CHECK(g.probe[2] == 0xAB);

    // A kind that is not judged: its GPU_PAGES stand in for the Gpu's output, in either mode.
    g.probe.clear();
    r = Run(built.path, {"--gpu", "test-corrupt", "--judge", "none"});
    CHECK(r.failures.empty());
    CHECK(r.skipped.at("fill") == 1);
    REQUIRE(g.probe.size() == 3);
    CHECK(g.probe[1] == 0xAB);

    // --chained is accepted, and excludes --isolated.
    r = Run(built.path, {"--gpu", "test-corrupt", "--chained"});
    CHECK_FALSE(r.failures.empty());
    g.corrupt_first_fill = false;
}

TEST_CASE("interrupts: PDC records are VBlanks; the others must be signalled, in order") {
    TempDir dir;
    Built built;
    built.path = dir.File("irq.mgpu").string();
    W w;
    w.Init();
    w.Mem(0x20000000, Bytes(0x10000, 0));
    w.Gx({kGxMemoryFill, 0x14000000, 0, 0x14001000, 0, 0, 0, 0});
    w.GpuPages({{0x20000000, Bytes(kPageSize, 1)}});
    const size_t psc0 = w.Irq(0);
    w.Irq(2);
    w.Gx({kGxDisplayTransfer, 0, 0, 0, 0, 0, 0, 0});
    w.GpuPages({{0x20001000, Bytes(kPageSize, 2)}});
    w.Irq(4);
    REQUIRE(w.Save(built.path));

    g.irq_mode = 0;
    PlayerResult r = Run(built.path, {"--gpu", "test-irq"});
    INFO(Describe(r));
    CHECK(r.failures.empty());

    g.irq_mode = 1; // the wrong id
    r = Run(built.path, {"--gpu", "test-irq"});
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().record == psc0);
    CHECK(r.failures.front().message.find("capture signals PSC0; the Gpu signalled DMA") != std::string::npos);

    g.irq_mode = 2; // none
    r = Run(built.path, {"--gpu", "test-irq"});
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().record == psc0);
    CHECK(r.failures.front().message.find("signalled nothing") != std::string::npos);
    r = Run(built.path, {"--gpu", "test-irq", "--judge", "none"}); // not judged: not compared
    CHECK(r.failures.empty());

    g.irq_mode = 3; // one more than the capture has
    r = Run(built.path, {"--gpu", "test-irq"});
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().message.find("does not have") != std::string::npos);
    g.irq_mode = 0;
}

TEST_CASE("an MMIO write that starts work is judged as the command it starts") {
    TempDir dir;
    Built built;
    built.path = dir.File("mmio.mgpu").string();
    W w;
    w.Init();
    w.Mem(0x20000000, Bytes(0x10000, 0));
    w.Mmio(0x1EF00C10, 1u << 3);   // display transfer flags: texture copy
    w.Mmio(0x1EF00C18, 0);         // the trigger written with 0 starts nothing
    const size_t start = w.Mmio(0x1EF00C18, 1);
    w.GpuPages({{0x20000000, Bytes(kPageSize, 0x5A)}});
    w.Irq(4);
    const size_t fill = w.Mmio(0x1EF0001C, 1); // memory fill 0 control: trigger
    w.GpuPages({{0x20001000, Bytes(kPageSize, 0x6B)}});
    w.Irq(0);
    w.Mmio(0x1EF00404, 5);         // an ordinary register write
    REQUIRE(w.Save(built.path));

    PlayerResult r = Run(built.path, {"--gpu", "replay"});
    INFO(Describe(r));
    CHECK(r.failures.empty());
    CHECK(r.judged.at("copy") == 1);
    CHECK(r.judged.at("fill") == 1);

    r = Run(built.path, {"--gpu", "none"});
    auto failure_at = [&](size_t record) -> const Failure* {
        for (const Failure& f : r.failures) {
            if (f.record == record) return &f;
        }
        return nullptr;
    };
    REQUIRE(failure_at(start) != nullptr);
    CHECK(failure_at(start)->record_name == "MMIO");
    CHECK(failure_at(start)->message.find("copy:") == 0);
    REQUIRE(failure_at(fill) != nullptr);
    CHECK(failure_at(fill)->message.find("fill:") == 0);
    CHECK(r.failures.front().record == start);

    r = Run(built.path, {"--gpu", "none", "--judge", "fill"});
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().record == fill);
    CHECK(r.skipped.at("copy") == 1);
}

TEST_CASE("--check registers compares the decoded writes with PICA_REG") {
    TempDir dir;
    const Built built = FullCapture(dir);
    const std::vector<std::string> base = {"--gpu", "test-regs", "--judge", "none", "--check", "registers"};

    g.regs_mode = 0;
    PlayerResult r = Run(built.path, base);
    INFO(Describe(r));
    CHECK(r.failures.empty());
    CHECK(r.error.empty());

    g.regs_mode = 1; // a value differs
    r = Run(built.path, base);
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().message.find("[check registers] write 0 of 4 differs") != std::string::npos);
    CHECK(r.failures.front().record == built.at.draw + 1); // the PICA_REG record

    g.regs_mode = 2; // one write short
    r = Run(built.path, base);
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().message.find("decoded 3") != std::string::npos);

    g.regs_mode = 3; // one too many
    r = Run(built.path, base);
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().message.find("decoded 5") != std::string::npos);

    g.regs_mode = 4; // the Gpu does not decode
    r = Run(built.path, base);
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures.front().message.find("does not decode") != std::string::npos);
    g.regs_mode = 0;

    // A Gpu without GpuDebug cannot be checked: the run stops with an error.
    r = Run(built.path, {"--gpu", "none", "--check", "registers"});
    CHECK_FALSE(r.error.empty());
    CHECK(r.ExitStatus() == 2);
    CHECK(r.error.find("GpuDebug") != std::string::npos);
}

TEST_CASE("a Gpu, a wrapper, a check, an option and a counter from another directory are found and shown") {
    TempDir dir;
    const Built built = FullCapture(dir);

    // --list shows the registered pieces.
    const std::string listing = ListRegistered();
    for (const char* name : {"test-counter", "test-log", "replay", "none", "test-wrap", "test-check",
                             "registers", "--test-wrap", "--test-opt", "--judge"}) {
        CHECK_MESSAGE(listing.find(name) != std::string::npos, name);
    }

    g.check_records = 0;
    const PlayerResult r = Run(built.path, {"--gpu", "test-counter", "--test-wrap", "--check", "test-check",
                                            "--test-opt", "42"});
    INFO(Describe(r));
    CHECK(r.error.empty());
    CHECK(r.failures.empty());
    CHECK(g.test_opt_seen == 42);
    CHECK(g.check_records == 20); // the check saw every record
    REQUIRE(r.frames.size() == 1);
    const FrameRow& row = r.frames[0];
    REQUIRE(row.gpu_ns.has_value());
    CHECK(*row.gpu_ns == 1500000);
    // The Gpu's counters and the wrapper's, in that order.
    REQUIRE(row.counters.size() == 3);
    CHECK(row.counters[0].name == "ubershader_draws");
    CHECK(row.counters[2].name == "wrapper_marker");
    CHECK(row.counters[2].value == 7);
    REQUIRE(row.columns.size() == 1);
    CHECK(row.columns[0].second == "opt42_f1");
    const std::string text = FormatReport(r);
    CHECK(text.find("ubershader_draws") != std::string::npos);
    CHECK(text.find("hd_textures_ready") != std::string::npos);
    CHECK(text.find("wrapper_marker") != std::string::npos);
    CHECK(text.find("my_column") != std::string::npos);
    CHECK(text.find("opt42_f1") != std::string::npos);
    CHECK(text.find("1.500") != std::string::npos); // gpu_ms

    // Without the wrapper flag the wrapper is not applied.
    const PlayerResult plain = Run(built.path, {"--gpu", "test-counter"});
    REQUIRE(plain.frames.size() == 1);
    CHECK(plain.frames[0].counters.size() == 2);
}

TEST_CASE("--dump-frames writes the native screens and the difference images") {
    TempDir dir;
    const Built built = FullCapture(dir, [](Image& top) { Offset(top, 16, 32, 16, 10); });
    const fs::path out = dir.File("dump");
    const PlayerResult r = Run(built.path, {"--gpu", "replay", "--dump-frames", out.string()});
    CHECK_FALSE(r.failures.empty());
    Image top, bottom, diff;
    std::string error;
    REQUIRE_MESSAGE(ReadPpm((out / "1_top.ppm").string(), top, &error), error);
    REQUIRE_MESSAGE(ReadPpm((out / "1_bottom.ppm").string(), bottom, &error), error);
    REQUIRE_MESSAGE(ReadPpm((out / "1_top_diff.ppm").string(), diff, &error), error);
    CHECK(top.width == 400);
    CHECK(top.height == 240);
    CHECK(bottom.width == 320);
    // The native image is what the model says; the difference is 40 (10 x 4) inside the block.
    CHECK(top.rgb == ModelScreen(MakeWorld().top, kTopCols).rgb);
    CHECK(diff.rgb[(static_cast<size_t>(32) * 400 + 16) * 3] == 40);
    CHECK(diff.rgb[0] == 0);
}

TEST_CASE("setup errors and the command line") {
    TempDir dir;
    const Built built = FullCapture(dir);
    PlayerResult r = Run(built.path, {"--gpu", "no-such-gpu"});
    CHECK(r.ExitStatus() == 2);
    CHECK(r.error.find("unknown gpu 'no-such-gpu'") != std::string::npos);
    CHECK(r.error.find("replay") != std::string::npos);
    r = Run(built.path, {"--gpu", "replay", "--check", "no-such-check"});
    CHECK(r.ExitStatus() == 2);
    CHECK(r.error.find("unknown check") != std::string::npos);

    // A capture that is damaged part-way reports the record, and the records before were played.
    W w;
    w.Init();
    w.Gx({kGxCacheFlush, 0, 0, 0, 0, 0, 0, 0});
    w.out.resize(w.out.size() - 5);
    REQUIRE(w.Save(dir.File("cut.mgpu")));
    r = Run(dir.File("cut.mgpu").string(), {"--gpu", "replay"});
    CHECK(r.ExitStatus() == 2);
    CHECK(r.error.find("record 1") != std::string::npos);

    // The ordering rules of gcap.py.
    {
        W v;
        v.Gx({kGxCacheFlush, 0, 0, 0, 0, 0, 0, 0});
        REQUIRE(v.Save(dir.File("first.mgpu")));
        r = Run(dir.File("first.mgpu").string(), {"--gpu", "replay"});
        CHECK(r.error.find("first record must be INIT") != std::string::npos);
    }
    {
        W v;
        v.Init();
        v.Gx({kGxSubmitCmdList, 0, 0, 0, 0, 0, 0, 0}); // no PICA_REG after it
        v.FrameMarker(1);
        REQUIRE(v.Save(dir.File("nopica.mgpu")));
        r = Run(dir.File("nopica.mgpu").string(), {"--gpu", "replay"});
        CHECK(r.error.find("not followed by PICA_REG") != std::string::npos);
    }
    {
        W v;
        v.Init();
        v.PicaReg({});
        REQUIRE(v.Save(dir.File("pica.mgpu")));
        r = Run(dir.File("pica.mgpu").string(), {"--gpu", "replay"});
        CHECK(r.error.find("PICA_REG") != std::string::npos);
    }
    {
        W v;
        v.Init();
        v.FrameMarker(5);
        v.frame = 4;
        v.Irq(0);
        REQUIRE(v.Save(dir.File("back.mgpu")));
        r = Run(dir.File("back.mgpu").string(), {"--gpu", "replay"});
        CHECK(r.error.find("goes back") != std::string::npos);
    }
    {
        W v;
        v.Init();
        v.Mem(0x10000000, Bytes(16, 0)); // not memory the 3DS GPU can see
        REQUIRE(v.Save(dir.File("badmem.mgpu")));
        r = Run(dir.File("badmem.mgpu").string(), {"--gpu", "replay"});
        CHECK(r.error.find("not inside") != std::string::npos);
    }
}

TEST_CASE("--judge and the command line") {
    u32 mask = 0;
    std::string error;
    CHECK(ParseJudge("fill,dma", mask, &error));
    CHECK(mask == (kJudgeFill | kJudgeDma));
    CHECK(ParseJudge("none", mask, &error));
    CHECK(mask == 0);
    CHECK(ParseJudge("all", mask, &error));
    CHECK((mask & kJudgeCacheFlush) != 0);
    CHECK((mask & kJudgeFrames) != 0);
    CHECK_FALSE(ParseJudge("fill,bogus", mask, &error));
    CHECK(error.find("bogus") != std::string::npos);

    PlayerConfig config;
    CHECK(config.judge == kDefaultJudge);
    CHECK((kDefaultJudge & kJudgeCacheFlush) == 0);
    for (const char* kind : {"fill", "transfer", "copy", "dma", "cmdlist", "frames"}) {
        u32 one = 0;
        REQUIRE(ParseJudge(kind, one, &error));
        CHECK((kDefaultJudge & one) == one);
    }

    auto parse = [&](std::vector<std::string> args) {
        const Options options = ParseOptions("capture-player", args);
        PlayerConfig c;
        std::string e;
        const bool ok = options.ok() && ConfigFromOptions(options, c, &e);
        return std::make_pair(ok, e);
    };
    CHECK_FALSE(parse({"x.mgpu"}).first);                                    // --gpu is required
    CHECK_FALSE(parse({"--gpu", "replay"}).first);                           // no capture
    CHECK_FALSE(parse({"--gpu", "replay", "a", "b"}).first);                 // two captures
    CHECK_FALSE(parse({"--gpu", "replay", "--isolated", "--chained", "a"}).first);
    CHECK_FALSE(parse({"--gpu", "replay", "--judge", "nope", "a"}).first);
    CHECK_FALSE(parse({"--gpu", "replay", "--no-such-option", "a"}).first);
    CHECK(parse({"--gpu", "replay", "--check", "registers,test-check", "a"}).first);
}

TEST_CASE("the program: exit statuses and output") {
    TempDir dir;
    const Built built = FullCapture(dir);
    std::ostringstream out, err;
    CHECK(RunCapturePlayer({"--gpu", "replay", built.path}, out, err) == 0);
    CHECK(out.str().find("result: PASS") != std::string::npos);
    CHECK(err.str().empty());

    out.str("");
    err.str("");
    CHECK(RunCapturePlayer({"--gpu", "none", built.path}, out, err) == 1);
    CHECK(out.str().find("FAIL") != std::string::npos);
    CHECK(out.str().find("failures:") != std::string::npos);

    out.str("");
    err.str("");
    CHECK(RunCapturePlayer({"--gpu", "replay", dir.File("missing.mgpu").string()}, out, err) == 2);
    CHECK(err.str().find("cannot open") != std::string::npos);

    out.str("");
    err.str("");
    CHECK(RunCapturePlayer({"--bogus"}, out, err) == 2);
    CHECK(err.str().find("options:") != std::string::npos);
    out.str("");
    CHECK(RunCapturePlayer({"--help"}, out, err) == 0);
    CHECK(out.str().find("--judge") != std::string::npos);
    out.str("");
    CHECK(RunCapturePlayer({"--list"}, out, err) == 0);
    CHECK(out.str().find("gpus:") != std::string::npos);

    // --fail-fast stops at the first failure: the frame is never reached.
    out.str("");
    CHECK(RunCapturePlayer({"--gpu", "none", "--fail-fast", built.path}, out, err) == 1);
    CHECK(out.str().find("frames: 0") != std::string::npos);
}
