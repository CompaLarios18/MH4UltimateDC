// Derived from Azahar src/video_core/pica/pica_core.h, regs_external.h, regs_internal.h and
// regs_texturing.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the word
// positions of the registers GPU::WriteReg and the command processor act on (the fill, display
// transfer and command-buffer triggers, the draw triggers, the fog mode).
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, regs_external.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
#include "gpu_capture/player.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <ostream>
#include <set>
#include <sstream>

#include <fmt/format.h>

#include "gpu_capture/capture_host.h"
#include "gpu_capture/check.h"
#include "gpu_capture/mgpu.h"

namespace mh4n::gpu_capture {

MH4N_OPTION("capture-player", "--gpu", "NAME", "the Gpu to replay into (see --list)");
MH4N_OPTION("capture-player", "--isolated", "",
            "apply each command's GPU_PAGES after checking it, so every command starts from the "
            "reference inputs");
MH4N_OPTION("capture-player", "--chained", "",
            "only compare GPU_PAGES, so errors build up as in the game (the default)");
MH4N_OPTION("capture-player", "--judge", "KINDS",
            "what is judged: fill,transfer,copy,dma,cmdlist,frames,cacheflush, all or none "
            "(default fill,transfer,copy,dma,cmdlist,frames)");
MH4N_OPTION("capture-player", "--check", "NAME", "turn a registered check on (repeatable; see --list)");
MH4N_OPTION("capture-player", "--skip-gas-frames", "",
            "report but do not judge frames whose command lists use fog mode 7 (gas)");
MH4N_OPTION("capture-player", "--fail-fast", "", "stop at the first failure");
MH4N_OPTION("capture-player", "--frames-dir", "DIR",
            "the reference PPMs <n>_top.ppm and <n>_bottom.ppm (default: <capture>.frames)");
MH4N_OPTION("capture-player", "--dump-frames", "DIR",
            "write the native screens and difference images of every frame as PPM");
MH4N_OPTION("capture-player", "--list", "", "list the registered Gpus, wrappers, checks and options");
MH4N_OPTION("capture-player", "--help", "", "print this help and exit");

namespace {

using Clock = std::chrono::steady_clock;

constexpr u32 kRegFogMode = 0x0E0;
constexpr u32 kRegTriggerDraw = 0x22E;
constexpr u32 kRegTriggerDrawIndexed = 0x22F;

// Word indices of the GPU registers that start work when written (Pica::PicaCore::Regs).
constexpr u32 kGpuRegFillTrigger[2] = {7, 11};
constexpr u32 kGpuRegTransferFlags = 0x304;
constexpr u32 kGpuRegTransferTrigger = 0x306;
constexpr u32 kGpuRegCmdListTrigger[2] = {0x432 + 0x23C, 0x432 + 0x23D};

enum class Kind { None, Dma, CmdList, Fill, Transfer, Copy, CacheFlush };

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Dma:
        return "dma";
    case Kind::CmdList:
        return "cmdlist";
    case Kind::Fill:
        return "fill";
    case Kind::Transfer:
        return "transfer";
    case Kind::Copy:
        return "copy";
    case Kind::CacheFlush:
        return "cacheflush";
    case Kind::None:
        break;
    }
    return "none";
}

u32 JudgeBit(Kind kind) {
    switch (kind) {
    case Kind::Dma:
        return kJudgeDma;
    case Kind::CmdList:
        return kJudgeCmdList;
    case Kind::Fill:
        return kJudgeFill;
    case Kind::Transfer:
        return kJudgeTransfer;
    case Kind::Copy:
        return kJudgeCopy;
    case Kind::CacheFlush:
        return kJudgeCacheFlush;
    case Kind::None:
        break;
    }
    return 0;
}

const char* InterruptName(u32 id) {
    static const char* const names[] = {"PSC0", "PSC1", "PDC0", "PDC1", "PPF", "P3D", "DMA"};
    return id < 7 ? names[id] : "unknown";
}

std::string FormatDb(double psnr) {
    return std::isinf(psnr) ? "inf" : fmt::format("{:.2f}", psnr);
}

struct TailRecord {
    size_t index;
    Record record;
};

class Player {
public:
    Player(const PlayerConfig& config, const Options& options) : config_(config), options_(options) {}

    PlayerResult Run();

private:
    // ---- setup --------------------------------------------------------------------------------
    bool Setup();
    void ResolveFramesDir();

    // ---- records ------------------------------------------------------------------------------
    bool ReadNext(Record& out, size_t& index);
    void Fatal(size_t index, const std::string& message);
    void Fail(size_t index, const Record& record, std::string message);
    void FailAt(size_t index, u32 frame, std::string name, std::string message);
    void Deliver(size_t index, const Record& record);

    void HandleInit(size_t index, const Record& record);
    void HandleMem(size_t index, const Record& record);
    void HandleCpuPages(size_t index, const Record& record);
    void HandleFlush(size_t index, const Record& record);
    void HandleFrame(size_t index, const Record& record);
    void HandleStandaloneIrq(size_t index, const Record& record);
    void RunGroup(size_t index, const Record& anchor);

    // ---- judging ------------------------------------------------------------------------------
    Kind Classify(const Record& anchor);
    std::string JudgeExact(const EventReference& reference, Kind kind);
    std::string JudgeCommandList(const EventReference& reference, const std::vector<ColorBufferConfig>& draws,
                                 const ColorBufferConfig& final_config);
    void ApplyPicaWrites(const std::vector<gpu::RegisterWrite>& writes,
                         std::vector<ColorBufferConfig>* draw_targets);
    void RefillSignals();
    void CompareInterrupt(size_t index, const Record& record, u32 id, bool judged);
    void CheckLeftoverInterrupts(size_t index, u32 frame);

    ScreenResult JudgeScreen(u32 screen, u32 frame, const char* suffix, Image* native_out);

    bool Stopped() const {
        return !result_.error.empty() || (config_.fail_fast && !result_.failures.empty());
    }

    struct ScopedTimer {
        explicit ScopedTimer(u64& total) : total_(total), start_(Clock::now()) {}
        ~ScopedTimer() {
            total_ += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count());
        }
        u64& total_;
        Clock::time_point start_;
    };

    const PlayerConfig& config_;
    const Options& options_;
    PlayerResult result_;

    Reader reader_;
    bool has_pending_ = false;
    Record pending_;
    size_t pending_index_ = 0;
    size_t records_read_ = 0;
    u32 last_frame_ = 0;

    CaptureHost host_;
    std::unique_ptr<gpu::Gpu> gpu_;
    gpu::GpuDebug* debug_ = nullptr;
    std::vector<std::pair<std::string, std::unique_ptr<CaptureCheck>>> checks_;

    DisplayShadow display_;
    std::array<u32, kNumPicaRegs> pica_{};
    bool have_init_ = false;

    std::string frames_dir_;
    std::deque<SignalledInterrupt> signals_;
    bool last_judged_ = true;

    u64 cpu_ns_ = 0;
    bool gas_ = false;
    u32 command_failures_ = 0;
    u32 frame_check_failures_ = 0;
    std::set<std::string> check_columns_seen_;
};

// ---- setup ------------------------------------------------------------------------------------

void Player::ResolveFramesDir() {
    namespace fs = std::filesystem;
    if (!config_.frames_dir.empty()) {
        frames_dir_ = config_.frames_dir;
        return;
    }
    const fs::path capture(config_.capture_path);
    std::error_code ec;
    for (const fs::path& candidate :
         {fs::path(config_.capture_path + ".frames"),
          fs::path(capture).replace_extension(".frames")}) {
        if (fs::is_directory(candidate, ec)) {
            frames_dir_ = candidate.string();
            return;
        }
    }
}

bool Player::Setup() {
    std::string error;
    if (!reader_.Open(config_.capture_path, &error)) {
        result_.error = fmt::format("{}: {}", config_.capture_path, error);
        return false;
    }
    ResolveFramesDir();

    const auto& factories = Registry<gpu::GpuFactory>::Instance();
    const gpu::GpuFactory* factory = factories.Find(config_.gpu_name);
    if (factory == nullptr) {
        std::string names;
        for (const auto& entry : factories.All()) {
            names += (names.empty() ? "" : ", ") + entry.first;
        }
        result_.error = fmt::format("unknown gpu '{}' (registered: {})", config_.gpu_name,
                                    names.empty() ? "none" : names);
        return false;
    }
    gpu_ = (*factory)(host_, options_);
    if (gpu_ == nullptr) {
        result_.error = fmt::format("gpu '{}' could not be created", config_.gpu_name);
        return false;
    }
    for (const auto& entry : Registry<gpu::GpuWrapper>::Instance().All()) {
        if (options_.Has("--" + entry.first)) {
            gpu_ = entry.second(std::move(gpu_), host_, options_);
            if (gpu_ == nullptr) {
                result_.error = fmt::format("wrapper '{}' did not return a gpu", entry.first);
                return false;
            }
        }
    }
    debug_ = dynamic_cast<gpu::GpuDebug*>(gpu_.get());

    CheckContext context{*gpu_, debug_, host_, options_};
    for (const std::string& name : config_.checks) {
        const CheckFactory* check_factory = Registry<CheckFactory>::Instance().Find(name);
        if (check_factory == nullptr) {
            std::string names;
            for (const auto& entry : Registry<CheckFactory>::Instance().All()) {
                names += (names.empty() ? "" : ", ") + entry.first;
            }
            result_.error = fmt::format("unknown check '{}' (registered: {})", name,
                                        names.empty() ? "none" : names);
            return false;
        }
        auto check = (*check_factory)(options_);
        if (check == nullptr) {
            result_.error = fmt::format("check '{}' could not be created", name);
            return false;
        }
        std::string start_error;
        if (!check->Start(context, &start_error)) {
            result_.error = fmt::format("check '{}': {}", name, start_error);
            return false;
        }
        for (const std::string& column : check->Columns()) {
            if (check_columns_seen_.insert(column).second) {
                result_.check_columns.push_back(column);
            }
        }
        checks_.emplace_back(name, std::move(check));
    }
    if (!config_.dump_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(config_.dump_dir, ec);
        if (ec) {
            result_.error = fmt::format("cannot create {}: {}", config_.dump_dir, ec.message());
            return false;
        }
    }
    return true;
}

// ---- reading ----------------------------------------------------------------------------------

void Player::Fatal(size_t index, const std::string& message) {
    if (result_.error.empty()) {
        result_.error = fmt::format("record {}: {}", index, message);
    }
}

void Player::FailAt(size_t index, u32 frame, std::string name, std::string message) {
    result_.failures.push_back({index, frame, std::move(name), std::move(message)});
    ++command_failures_;
}

void Player::Fail(size_t index, const Record& record, std::string message) {
    std::string name = record.name();
    if (record.type == kGx && !record.payload.empty()) {
        name += std::string(" ") + GxName(record.payload[0]);
    }
    FailAt(index, record.frame, std::move(name), std::move(message));
}

bool Player::ReadNext(Record& out, size_t& index) {
    if (has_pending_) {
        out = std::move(pending_);
        index = pending_index_;
        has_pending_ = false;
        return true;
    }
    std::string error;
    index = reader_.next_index();
    switch (reader_.Next(out, &error)) {
    case Reader::Status::End:
        return false;
    case Reader::Status::Error:
        Fatal(index, error);
        return false;
    case Reader::Status::Record:
        break;
    }
    const std::string problem = CheckRecord(out);
    if (!problem.empty()) {
        Fatal(index, problem);
        return false;
    }
    if (index == 0 && out.type != kInit) {
        Fatal(index, "the first record must be INIT");
        return false;
    }
    if (index > 0 && out.type == kInit) {
        Fatal(index, "INIT is only allowed as the first record");
        return false;
    }
    if (index > 0 && out.frame < last_frame_) {
        Fatal(index, fmt::format("frame {} goes back from {}", out.frame, last_frame_));
        return false;
    }
    last_frame_ = out.frame;
    ++records_read_;
    return true;
}

void Player::Deliver(size_t index, const Record& record) {
    for (auto& [name, check] : checks_) {
        const std::string message = check->OnRecord(index, record);
        if (!message.empty()) {
            Fail(index, record, fmt::format("[check {}] {}", name, message));
        }
    }
}

// ---- simple records -----------------------------------------------------------------------------

void Player::HandleInit(size_t index, const Record& record) {
    gpu::InitState state;
    std::string error;
    if (!MakeInitState(record, state, &error)) {
        Fatal(index, error);
        return;
    }
    display_.Load(state.Block(gpu::InitBlock::GpuRegs).data(),
                  state.Block(gpu::InitBlock::LcdRegs).data());
    const auto regs = state.Block(gpu::InitBlock::PicaRegs);
    for (u32 i = 0; i < kNumPicaRegs; ++i) {
        const u8* p = regs.data() + 4 * i;
        pica_[i] = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                   (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
    }
    have_init_ = true;
    if (debug_ != nullptr) {
        ScopedTimer timer(cpu_ns_);
        debug_->LoadInitState(state);
    }
    Deliver(index, record);
}

void Player::HandleMem(size_t index, const Record& record) {
    MemData mem;
    std::string error;
    if (!ParseMem(record, mem, &error)) {
        Fatal(index, error);
        return;
    }
    if (!host_.WriteBoth(mem.address, mem.data.data(), static_cast<u32>(mem.data.size()))) {
        Fatal(index, fmt::format("MEM at 0x{:08x} ({} bytes) is not inside FCRAM, VRAM or DSP RAM",
                                 mem.address, mem.data.size()));
        return;
    }
    Deliver(index, record);
}

void Player::HandleCpuPages(size_t index, const Record& record) {
    std::vector<PageData> pages;
    std::string error;
    if (!ParsePages(record, pages, &error)) {
        Fatal(index, error);
        return;
    }
    for (const PageData& page : pages) {
        if (!host_.WriteBoth(page.address, page.data.data(), kPageSize)) {
            Fatal(index, fmt::format("CPU_PAGES page 0x{:08x} is not inside FCRAM, VRAM or DSP RAM",
                                     page.address));
            return;
        }
    }
    Deliver(index, record);
}

void Player::HandleFlush(size_t index, const Record& record) {
    FlushData flush;
    std::string error;
    if (!ParseFlush(record, flush, &error)) {
        Fatal(index, error);
        return;
    }
    {
        ScopedTimer timer(cpu_ns_);
        if (flush.kind == kInvalidateDataCache) {
            gpu_->InvalidateRegion(flush.address, flush.size);
        } else {
            gpu_->FlushRegion(flush.address, flush.size); // kinds 1, 3 and 4
        }
    }
    Deliver(index, record);
}

// ---- interrupts ---------------------------------------------------------------------------------

void Player::RefillSignals() {
    for (const SignalledInterrupt& s : host_.TakeSignalled()) {
        signals_.push_back(s);
    }
}

void Player::CompareInterrupt(size_t index, const Record& record, u32 id, bool judged) {
    RefillSignals();
    if (signals_.empty()) {
        if (judged) {
            Fail(index, record,
                 fmt::format("the capture signals {} here; the Gpu signalled nothing",
                             InterruptName(id)));
        }
        return;
    }
    const u32 got = static_cast<u32>(signals_.front().id);
    signals_.pop_front();
    if (judged && got != id) {
        Fail(index, record,
             fmt::format("the capture signals {}; the Gpu signalled {}", InterruptName(id),
                         InterruptName(got)));
    }
}

void Player::CheckLeftoverInterrupts(size_t index, u32 frame) {
    RefillSignals();
    if (!signals_.empty() && last_judged_) {
        FailAt(index, frame, "interrupts",
               fmt::format("the Gpu signalled {} interrupt(s) the capture does not have "
                           "(first: {})",
                           signals_.size(), InterruptName(static_cast<u32>(signals_.front().id))));
    }
    signals_.clear();
}

void Player::HandleStandaloneIrq(size_t index, const Record& record) {
    IrqData irq;
    std::string error;
    if (!ParseIrq(record, irq, &error)) {
        Fatal(index, error);
        return;
    }
    if (irq.id == 2 || irq.id == 3) { // PDC0 and PDC1 are VBlanks
        ScopedTimer timer(cpu_ns_);
        gpu_->VBlank(irq.id - 2);
    } else {
        CompareInterrupt(index, record, irq.id, last_judged_);
    }
    Deliver(index, record);
}

// ---- registers ----------------------------------------------------------------------------------

void Player::ApplyPicaWrites(const std::vector<gpu::RegisterWrite>& writes,
                             std::vector<ColorBufferConfig>* draw_targets) {
    for (const gpu::RegisterWrite& w : writes) {
        const u32 m = ExpandMask(w.mask);
        pica_[w.reg] = (pica_[w.reg] & ~m) | (w.value & m);
        if (w.reg == kRegFogMode && (w.mask & 1) && (w.value & 7) == 7) {
            gas_ = true;
        }
        if (w.reg == kRegTriggerDraw || w.reg == kRegTriggerDrawIndexed) {
            if ((pica_[kRegFogMode] & 7) == 7) {
                gas_ = true;
            }
            if (draw_targets != nullptr) {
                const ColorBufferConfig config = ColorBufferFromRegisters(pica_.data());
                if (std::find(draw_targets->begin(), draw_targets->end(), config) ==
                    draw_targets->end()) {
                    draw_targets->push_back(config);
                }
            }
        }
    }
}

// ---- judging ------------------------------------------------------------------------------------

Kind Player::Classify(const Record& anchor) {
    if (anchor.type == kGx) {
        switch (anchor.payload[0]) {
        case kGxRequestDma:
            return Kind::Dma;
        case kGxSubmitCmdList:
            return Kind::CmdList;
        case kGxMemoryFill:
            return Kind::Fill;
        case kGxDisplayTransfer:
            return Kind::Transfer;
        case kGxTextureCopy:
            return Kind::Copy;
        case kGxCacheFlush:
            return Kind::CacheFlush;
        default:
            return Kind::None;
        }
    }
    if (anchor.type == kMmio) {
        // GPU::WriteReg starts work when one of these registers is written.
        MmioData mmio;
        if (!ParseMmio(anchor, mmio, nullptr)) {
            return Kind::None;
        }
        const u32 page = mmio.address & 0xFFFFF000;
        if (page != DisplayShadow::kGpuBase && page != DisplayShadow::kGpuBase + 0x1000) {
            return Kind::None;
        }
        const u32 index = (mmio.address - DisplayShadow::kGpuBase) / 4;
        // The work starts only when the trigger bit is set (GPU::MemoryFill, MemoryTransfer and
        // SubmitCmdList return early otherwise).
        if ((index == kGpuRegFillTrigger[0] || index == kGpuRegFillTrigger[1]) &&
            (mmio.value & 1) != 0) {
            return Kind::Fill;
        }
        if (index == kGpuRegTransferTrigger && (mmio.value & 1) != 0) {
            return (display_.GpuReg(kGpuRegTransferFlags) & (1u << 3)) ? Kind::Copy : Kind::Transfer;
        }
        if ((index == kGpuRegCmdListTrigger[0] || index == kGpuRegCmdListTrigger[1]) &&
            mmio.value != 0) {
            return Kind::CmdList;
        }
    }
    return Kind::None;
}

// Bit-exact: every page the capture's GPU wrote must hold the same bytes in the Gpu's memory.
std::string Player::JudgeExact(const EventReference& reference, Kind kind) {
    u32 differing = 0;
    u32 first_page = 0;
    u32 first_offset = 0;
    for (const PageData& page : reference.pages) {
        const u8* mine = host_.PhysicalPointer(page.address, kPageSize);
        if (mine == nullptr || std::memcmp(mine, page.data.data(), kPageSize) == 0) {
            continue;
        }
        if (differing == 0) {
            first_page = page.address;
            while (first_offset < kPageSize && mine[first_offset] == page.data[first_offset]) {
                ++first_offset;
            }
        }
        ++differing;
    }
    if (differing == 0) {
        return {};
    }
    return fmt::format("{}: {} of {} pages written by the capture's GPU differ; first at "
                       "0x{:08x} (byte {} of the page)",
                       KindName(kind), differing, reference.pages.size(), first_page, first_offset);
}

// Draws: the colour buffers go through the image thresholds.
std::string Player::JudgeCommandList(const EventReference& reference,
                                     const std::vector<ColorBufferConfig>& draws,
                                     const ColorBufferConfig& final_config) {
    // Every colour buffer a draw targeted is judged; the final one too when the capture's GPU
    // wrote into it.
    std::vector<ColorBufferConfig> targets = draws;
    auto overlaps = [](const ColorBufferConfig& config, u32 page_address) {
        return page_address < config.address + config.ByteSize() &&
               page_address + kPageSize > config.address;
    };
    if (final_config.valid() &&
        std::find(targets.begin(), targets.end(), final_config) == targets.end()) {
        for (const PageData& page : reference.pages) {
            if (overlaps(final_config, page.address)) {
                targets.push_back(final_config);
                break;
            }
        }
    }
    std::string message;
    std::vector<ColorBufferConfig> judged;
    for (const ColorBufferConfig& config : targets) {
        if (!config.valid()) {
            continue;
        }
        const u32 size = config.ByteSize();
        const u8* mine = host_.PhysicalPointer(config.address, size);
        const u8* reference_bytes = host_.ReferencePointer(config.address, size);
        if (mine == nullptr || reference_bytes == nullptr) {
            continue;
        }
        judged.push_back(config);
        const Image expected = DecodeColorBuffer(reference_bytes, config);
        const Image actual = DecodeColorBuffer(mine, config);
        const ImageMetrics metrics = CompareImages(expected, actual);
        if (!metrics.Passes()) {
            if (!message.empty()) {
                message += "; ";
            }
            message += fmt::format(
                "colour buffer 0x{:08x} ({}x{}, format {}): PSNR {} dB (needs {:.0f}), worst 16x16 "
                "block at ({}, {}) has mean error {:.2f}/255 (allowed {:.0f})",
                config.address, config.width, config.height, config.format, FormatDb(metrics.psnr),
                kMinPsnrDb, metrics.worst_block_x, metrics.worst_block_y, metrics.worst_block,
                kMaxBlockError);
        }
    }
    for (const PageData& page : reference.pages) {
        const bool inside = std::any_of(judged.begin(), judged.end(), [&](const auto& config) {
            return overlaps(config, page.address);
        });
        if (!inside) {
            ++result_.unjudged_pages;
        }
    }
    return message;
}

// ---- events -------------------------------------------------------------------------------------

void Player::RunGroup(size_t index, const Record& anchor) {
    // The records that belong to the event: PICA_REG, GPU_PAGES, IRQ and FLUSH, up to the next
    // record of any other kind.
    std::vector<TailRecord> tail;
    Record next;
    size_t next_index = 0;
    while (ReadNext(next, next_index)) {
        if (next.type == kPicaReg || next.type == kGpuPages || next.type == kIrq ||
            next.type == kFlush) {
            tail.push_back({next_index, std::move(next)});
            next = Record();
        } else {
            pending_ = std::move(next);
            pending_index_ = next_index;
            has_pending_ = true;
            break;
        }
    }
    if (!result_.error.empty()) {
        return;
    }

    const bool is_cmdlist_gx = anchor.type == kGx && anchor.payload[0] == kGxSubmitCmdList;
    for (size_t i = 0; i < tail.size(); ++i) {
        if (tail[i].record.type == kPicaReg && !(is_cmdlist_gx && i == 0)) {
            Fatal(tail[i].index, "PICA_REG does not follow a command-list GX");
            return;
        }
    }
    if (is_cmdlist_gx && (tail.empty() || tail[0].record.type != kPicaReg)) {
        Fatal(index, "command-list GX is not followed by PICA_REG");
        return;
    }

    // What the capture says this event produced.
    EventReference reference;
    std::string error;
    for (const TailRecord& t : tail) {
        if (t.record.type == kPicaReg) {
            if (!ParsePicaReg(t.record, reference.registers, &error)) {
                Fatal(t.index, error);
                return;
            }
        } else if (t.record.type == kGpuPages) {
            std::vector<PageData> pages;
            if (!ParsePages(t.record, pages, &error)) {
                Fatal(t.index, error);
                return;
            }
            reference.pages.insert(reference.pages.end(), pages.begin(), pages.end());
        } else if (t.record.type == kIrq) {
            IrqData irq;
            if (!ParseIrq(t.record, irq, &error)) {
                Fatal(t.index, error);
                return;
            }
            if (irq.id != 2 && irq.id != 3) {
                reference.interrupts.push_back(irq);
            }
        }
    }

    const Kind kind = Classify(anchor);
    const bool judged = kind == Kind::None || (config_.judge & JudgeBit(kind)) != 0;

    CheckLeftoverInterrupts(index, anchor.frame);
    last_judged_ = judged;

    // ---- run it ----
    host_.SetCurrentEvent(&reference);
    {
        ScopedTimer timer(cpu_ns_);
        switch (anchor.type) {
        case kGx: {
            gpu::GxCommand command;
            ParseGx(anchor, command, nullptr);
            gpu_->Execute(command);
            break;
        }
        case kMmio: {
            MmioData mmio;
            ParseMmio(anchor, mmio, nullptr);
            gpu_->WriteReg(mmio.address, mmio.value);
            break;
        }
        case kSwap: {
            SwapData swap;
            ParseSwap(anchor, swap, nullptr);
            gpu_->SetBufferSwap(swap.screen, swap.info);
            break;
        }
        case kFill: {
            u32 word = 0;
            ParseFill(anchor, word, nullptr);
            gpu_->SetColorFill(word);
            break;
        }
        default:
            break;
        }
    }
    host_.SetCurrentEvent(nullptr);

    // The player's own copy of the display registers.
    if (anchor.type == kMmio) {
        MmioData mmio;
        ParseMmio(anchor, mmio, nullptr);
        display_.WriteReg(mmio.address, mmio.value);
    } else if (anchor.type == kSwap) {
        SwapData swap;
        ParseSwap(anchor, swap, nullptr);
        display_.ApplySwap(swap.screen, swap.info,
                           host_.VirtualToPhysical(swap.info.address_left).value_or(0),
                           host_.VirtualToPhysical(swap.info.address_right).value_or(0));
    } else if (anchor.type == kFill) {
        u32 word = 0;
        ParseFill(anchor, word, nullptr);
        display_.ApplyFill(word);
    }

    // The capture's memory after the event.
    for (const PageData& page : reference.pages) {
        if (!host_.ApplyReferencePage(page)) {
            Fatal(index, fmt::format("GPU_PAGES page 0x{:08x} is not inside FCRAM, VRAM or DSP RAM",
                                     page.address));
            return;
        }
    }

    // The register file, and the colour buffers the command list drew into.
    std::vector<ColorBufferConfig> draw_targets;
    ColorBufferConfig final_config;
    if (!reference.registers.empty() || kind == Kind::CmdList) {
        ApplyPicaWrites(reference.registers, &draw_targets);
        final_config = ColorBufferFromRegisters(pica_.data());
    }

    // ---- judge it ----
    if (kind != Kind::None) {
        if (judged) {
            ++result_.judged[KindName(kind)];
            std::string message;
            if (kind == Kind::CmdList) {
                if (debug_ != nullptr) {
                    ScopedTimer timer(cpu_ns_);
                    debug_->WriteBackColorBuffers();
                }
                message = JudgeCommandList(reference, draw_targets, final_config);
            } else {
                message = JudgeExact(reference, kind);
            }
            if (!message.empty()) {
                Fail(index, anchor, std::move(message));
            }
        } else {
            ++result_.skipped[KindName(kind)];
        }
    }
    if (!judged || config_.isolated || kind == Kind::None) {
        // Not judged: the capture's output stands in for the Gpu's. Isolated: the next command
        // starts from the reference inputs.
        for (const PageData& page : reference.pages) {
            host_.ApplyPageToGpu(page);
        }
    }

    // ---- the rest of the group, in file order ----
    Deliver(index, anchor);
    for (const TailRecord& t : tail) {
        if (t.record.type == kIrq) {
            IrqData irq;
            ParseIrq(t.record, irq, nullptr);
            if (irq.id == 2 || irq.id == 3) {
                ScopedTimer timer(cpu_ns_);
                gpu_->VBlank(irq.id - 2);
            } else {
                CompareInterrupt(t.index, t.record, irq.id, judged);
            }
        } else if (t.record.type == kFlush) {
            HandleFlush(t.index, t.record);
            continue; // HandleFlush delivers it
        }
        Deliver(t.index, t.record);
    }
}

// ---- frames -------------------------------------------------------------------------------------

ScreenResult Player::JudgeScreen(u32 screen, u32 frame, const char* suffix, Image* native_out) {
    ScreenResult out;
    Image native;
    std::string error;
    const PhysicalPointerFn memory = [this](u32 address, u32 size) -> const u8* {
        return host_.PhysicalPointer(address, size);
    };
    if (!display_.BuildScreen(screen, memory, native, &error)) {
        out.note = error;
        return out;
    }
    *native_out = native;

    if (!frames_dir_.empty()) {
        const std::string path = fmt::format("{}/{}_{}.ppm", frames_dir_, frame, suffix);
        Image reference;
        if (!std::filesystem::exists(path)) {
            out.note = "no reference image";
        } else if (!ReadPpm(path, reference, &error)) {
            out.note = error;
        } else {
            out.metrics = CompareImages(reference, native);
            out.compared = out.metrics.comparable;
            if (!out.compared) {
                out.note = fmt::format("size {}x{} differs from the reference's {}x{}", native.width,
                                       native.height, reference.width, reference.height);
            }
            if (!config_.dump_dir.empty() && out.compared) {
                WritePpm(fmt::format("{}/{}_{}_diff.ppm", config_.dump_dir, frame, suffix),
                         DifferenceImage(reference, native), nullptr);
            }
        }
    } else {
        out.note = "no reference image";
    }
    return out;
}

void Player::HandleFrame(size_t index, const Record& record) {
    CheckLeftoverInterrupts(index, record.frame);

    FrameRow row;
    row.frame = record.frame;
    row.record = index;
    if (debug_ != nullptr) {
        {
            ScopedTimer timer(cpu_ns_);
            debug_->WriteBackColorBuffers();
        }
        row.counters = debug_->TakeFrameCounters();
        row.gpu_ns = debug_->TakeFrameGpuTimeNs();
    }
    row.cpu_ns = cpu_ns_;
    cpu_ns_ = 0;
    row.gas = gas_;
    gas_ = false;

    const bool frames_selected = (config_.judge & kJudgeFrames) != 0;
    row.judged = frames_selected && !(row.gas && config_.skip_gas_frames);

    Image top, bottom;
    row.top = JudgeScreen(0, record.frame, "top", &top);
    row.bottom = JudgeScreen(1, record.frame, "bottom", &bottom);
    if (!config_.dump_dir.empty()) {
        if (!top.empty()) {
            WritePpm(fmt::format("{}/{}_top.ppm", config_.dump_dir, record.frame), top, nullptr);
        }
        if (!bottom.empty()) {
            WritePpm(fmt::format("{}/{}_bottom.ppm", config_.dump_dir, record.frame), bottom, nullptr);
        }
    }

    bool images_ok = true;
    if (row.judged) {
        for (const auto& [name, screen] : {std::pair<const char*, const ScreenResult&>{"top", row.top},
                                           std::pair<const char*, const ScreenResult&>{"bottom", row.bottom}}) {
            if (!screen.compared) {
                images_ok = false;
                FailAt(index, record.frame, "FRAME",
                       fmt::format("{} screen not compared: {}", name,
                                   screen.note.empty() ? "no image" : screen.note));
            } else if (!screen.metrics.Passes()) {
                images_ok = false;
                FailAt(index, record.frame, "FRAME",
                       fmt::format("{} screen: PSNR {} dB (needs {:.0f}), worst 16x16 block at "
                                   "({}, {}) has mean error {:.2f}/255 (allowed {:.0f})",
                                   name, FormatDb(screen.metrics.psnr), kMinPsnrDb,
                                   screen.metrics.worst_block_x, screen.metrics.worst_block_y,
                                   screen.metrics.worst_block, kMaxBlockError));
            }
        }
    }

    // Checks: their columns and failures.
    FrameInfo info{record.frame, index, row.gas};
    for (auto& [name, check] : checks_) {
        FrameColumns columns;
        const std::string message = check->OnFrame(info, columns);
        for (const auto& cell : columns.cells()) {
            row.columns.push_back(cell);
        }
        if (!message.empty()) {
            ++frame_check_failures_;
            FailAt(index, record.frame, "FRAME", fmt::format("[check {}] {}", name, message));
        }
    }
    Deliver(index, record);

    row.command_failures = command_failures_;
    row.passed = images_ok && command_failures_ == 0;
    command_failures_ = 0;
    result_.frames.push_back(std::move(row));
}

// ---- the run ------------------------------------------------------------------------------------

PlayerResult Player::Run() {
    if (!Setup()) {
        return std::move(result_);
    }
    Record record;
    size_t index = 0;
    while (!Stopped() && ReadNext(record, index)) {
        switch (record.type) {
        case kInit:
            HandleInit(index, record);
            break;
        case kMem:
            HandleMem(index, record);
            break;
        case kCpuPages:
            HandleCpuPages(index, record);
            break;
        case kGx:
        case kMmio:
        case kSwap:
        case kFill:
            RunGroup(index, record);
            break;
        case kFlush:
            HandleFlush(index, record);
            break;
        case kIrq:
            HandleStandaloneIrq(index, record);
            break;
        case kFrame:
            HandleFrame(index, record);
            break;
        case kGpuPages: {
            // Pages with no event before them: the capture's GPU wrote them, so they stand.
            std::vector<PageData> pages;
            std::string error;
            if (!ParsePages(record, pages, &error)) {
                Fatal(index, error);
                break;
            }
            for (const PageData& page : pages) {
                host_.ApplyReferencePage(page);
                host_.ApplyPageToGpu(page);
            }
            Deliver(index, record);
            break;
        }
        case kPicaReg:
            Fatal(index, "PICA_REG does not follow a command-list GX");
            break;
        default:
            break;
        }
    }
    if (result_.error.empty()) {
        if (!have_init_) {
            result_.error = "the capture has no records";
        } else {
            CheckLeftoverInterrupts(records_read_ == 0 ? 0 : records_read_ - 1, last_frame_);
            for (auto& [name, check] : checks_) {
                const std::string message = check->Finish();
                if (!message.empty()) {
                    FailAt(records_read_ == 0 ? 0 : records_read_ - 1, last_frame_, "end",
                           fmt::format("[check {}] {}", name, message));
                }
            }
        }
    }
    result_.records = records_read_;
    return std::move(result_);
}

} // namespace

// ---- public -------------------------------------------------------------------------------------

namespace {
struct JudgeName {
    const char* name;
    u32 bit;
};
// The --judge names, in the order the error text lists them (all and none are handled apart).
constexpr JudgeName kJudgeKinds[] = {
    {"fill", kJudgeFill},         {"transfer", kJudgeTransfer},   {"copy", kJudgeCopy},
    {"dma", kJudgeDma},           {"cmdlist", kJudgeCmdList},     {"frames", kJudgeFrames},
    {"cacheflush", kJudgeCacheFlush},
};
} // namespace

bool ParseJudge(const std::string& text, u32& mask, std::string* error) {
    mask = 0;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (item.empty() || item == "none") {
            continue;
        }
        if (item == "all") {
            mask |= kDefaultJudge | kJudgeCacheFlush;
            continue;
        }
        const auto* kind = std::find_if(std::begin(kJudgeKinds), std::end(kJudgeKinds),
                                        [&](const JudgeName& k) { return item == k.name; });
        if (kind == std::end(kJudgeKinds)) {
            if (error != nullptr) {
                std::string names;
                for (const JudgeName& k : kJudgeKinds) {
                    names += k.name;
                    names += ", ";
                }
                *error = fmt::format("--judge: unknown kind '{}' ({}all, none)", item, names);
            }
            return false;
        }
        mask |= kind->bit;
    }
    return true;
}

bool ConfigFromOptions(const Options& options, PlayerConfig& config, std::string* error) {
    if (options.positional().size() != 1) {
        *error = "give exactly one capture file";
        return false;
    }
    config.capture_path = options.positional()[0];
    const auto gpu_name = options.Get("--gpu");
    if (!gpu_name || gpu_name->empty()) {
        *error = "--gpu NAME is required (see --list)";
        return false;
    }
    config.gpu_name = *gpu_name;
    if (options.Has("--isolated") && options.Has("--chained")) {
        *error = "--isolated and --chained exclude each other";
        return false;
    }
    config.isolated = options.Has("--isolated");
    if (const auto judge = options.Get("--judge")) {
        if (!ParseJudge(*judge, config.judge, error)) {
            return false;
        }
    }
    for (const std::string& value : options.All("--check")) {
        std::stringstream stream(value);
        std::string item;
        while (std::getline(stream, item, ',')) {
            if (!item.empty()) {
                config.checks.push_back(item);
            }
        }
    }
    config.skip_gas_frames = options.Has("--skip-gas-frames");
    config.fail_fast = options.Has("--fail-fast");
    config.frames_dir = options.GetString("--frames-dir");
    config.dump_dir = options.GetString("--dump-frames");
    return true;
}

PlayerResult RunPlayer(const PlayerConfig& config, const Options& options) {
    Player player(config, options);
    return player.Run();
}

std::string ListRegistered() {
    std::string out = "gpus:\n";
    for (const auto& entry : Registry<gpu::GpuFactory>::Instance().All()) {
        out += fmt::format("  {}\n", entry.first);
    }
    out += "wrappers (turned on by the flag of the same name):\n";
    for (const auto& entry : Registry<gpu::GpuWrapper>::Instance().All()) {
        out += fmt::format("  {}  (--{})\n", entry.first, entry.first);
    }
    out += "checks (--check NAME):\n";
    for (const auto& entry : Registry<CheckFactory>::Instance().All()) {
        out += fmt::format("  {}\n", entry.first);
    }
    out += "options:\n" + Usage("capture-player");
    return out;
}

std::string FormatReport(const PlayerResult& result) {
    std::string out;
    // Columns: the fixed ones, the counters (in the order they first appear), the checks'.
    std::vector<std::string> counter_names;
    for (const FrameRow& row : result.frames) {
        for (const gpu::Counter& counter : row.counters) {
            if (std::find(counter_names.begin(), counter_names.end(), counter.name) ==
                counter_names.end()) {
                counter_names.push_back(counter.name);
            }
        }
    }
    std::vector<std::string> column_names = result.check_columns;
    for (const FrameRow& row : result.frames) {
        for (const auto& cell : row.columns) {
            if (std::find(column_names.begin(), column_names.end(), cell.first) ==
                column_names.end()) {
                column_names.push_back(cell.first);
            }
        }
    }

    std::vector<std::vector<std::string>> table;
    std::vector<std::string> header = {"frame", "record", "result", "top_psnr", "top_blk",
                                       "bot_psnr", "bot_blk", "cpu_ms", "gpu_ms"};
    header.insert(header.end(), counter_names.begin(), counter_names.end());
    header.insert(header.end(), column_names.begin(), column_names.end());
    table.push_back(header);
    auto psnr_cell = [](const ScreenResult& s) {
        return s.compared ? FormatDb(s.metrics.psnr) : std::string("-");
    };
    auto block_cell = [](const ScreenResult& s) {
        return s.compared ? fmt::format("{:.2f}", s.metrics.worst_block) : std::string("-");
    };
    for (const FrameRow& row : result.frames) {
        std::string verdict = "ok";
        if (!row.passed) {
            verdict = "FAIL";
        } else if (row.gas && !row.judged && (row.top.compared || row.bottom.compared)) {
            verdict = "gas-skipped";
        } else if (!row.judged) {
            verdict = "not-judged";
        }
        std::vector<std::string> cells = {
            std::to_string(row.frame),
            std::to_string(row.record),
            verdict,
            psnr_cell(row.top),
            block_cell(row.top),
            psnr_cell(row.bottom),
            block_cell(row.bottom),
            fmt::format("{:.3f}", static_cast<double>(row.cpu_ns) / 1e6),
            row.gpu_ns ? fmt::format("{:.3f}", static_cast<double>(*row.gpu_ns) / 1e6) : "-"};
        for (const std::string& name : counter_names) {
            std::string cell = "-";
            for (const gpu::Counter& counter : row.counters) {
                if (counter.name == name) {
                    cell = std::to_string(counter.value);
                }
            }
            cells.push_back(cell);
        }
        for (const std::string& name : column_names) {
            std::string cell = "-";
            for (const auto& c : row.columns) {
                if (c.first == name) {
                    cell = c.second;
                }
            }
            cells.push_back(cell);
        }
        table.push_back(std::move(cells));
    }
    std::vector<size_t> widths(header.size(), 0);
    for (const auto& line : table) {
        for (size_t i = 0; i < line.size(); ++i) {
            widths[i] = std::max(widths[i], line[i].size());
        }
    }
    if (!result.frames.empty()) {
        for (const auto& line : table) {
            std::string text;
            for (size_t i = 0; i < line.size(); ++i) {
                text += fmt::format("{:>{}}{}", line[i], widths[i], i + 1 < line.size() ? "  " : "");
            }
            out += text + "\n";
        }
    }

    if (!result.failures.empty()) {
        out += "failures:\n";
        const size_t shown = std::min<size_t>(result.failures.size(), 50);
        for (size_t i = 0; i < shown; ++i) {
            const Failure& f = result.failures[i];
            out += fmt::format("  record {} (frame {}, {}): {}\n", f.record, f.frame,
                               f.record_name, f.message);
        }
        if (result.failures.size() > shown) {
            out += fmt::format("  ... and {} more\n", result.failures.size() - shown);
        }
    }
    auto counts = [](const std::map<std::string, u32>& map) {
        std::string text;
        for (const auto& [kind, count] : map) {
            text += fmt::format("{}{}={}", text.empty() ? "" : " ", kind, count);
        }
        return text.empty() ? std::string("none") : text;
    };
    out += fmt::format("records: {}  frames: {}\n", result.records, result.frames.size());
    out += fmt::format("commands judged: {}\n", counts(result.judged));
    out += fmt::format("commands skipped (not in --judge): {}\n", counts(result.skipped));
    if (result.unjudged_pages != 0) {
        out += fmt::format("command-list pages outside any judged colour buffer: {}\n",
                           result.unjudged_pages);
    }
    if (!result.error.empty()) {
        out += fmt::format("error: {}\n", result.error);
    }
    out += fmt::format("result: {}\n", result.error.empty()
                                           ? (result.failures.empty()
                                                  ? "PASS"
                                                  : fmt::format("FAIL ({} failure(s), first at record {})",
                                                                result.failures.size(),
                                                                result.failures.front().record))
                                           : "ERROR");
    return out;
}

int RunCapturePlayer(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const Options options = ParseOptions("capture-player", args);
    if (!options.ok()) {
        err << "error: " << options.error() << "\noptions:\n" << Usage("capture-player");
        return 2;
    }
    if (options.Has("--help")) {
        out << "usage: mh4n_capture_player [options] CAPTURE.mgpu\noptions:\n"
            << Usage("capture-player");
        return 0;
    }
    if (options.Has("--list")) {
        out << ListRegistered();
        return 0;
    }
    PlayerConfig config;
    std::string error;
    if (!ConfigFromOptions(options, config, &error)) {
        err << "error: " << error << "\n";
        return 2;
    }
    const PlayerResult result = RunPlayer(config, options);
    out << FormatReport(result);
    if (!result.error.empty()) {
        err << "error: " << result.error << "\n";
    }
    return result.ExitStatus();
}

} // namespace mh4n::gpu_capture
