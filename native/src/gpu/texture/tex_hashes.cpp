// Derived from Azahar src/video_core/gpu.cpp (GPU::Execute for the command list submission,
// GPU::VirtualToPhysicalAddress's treatment of an address it does not know) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; the file says "Licensed under GPLv2
// or any later version"). The same rules as our pica_gpu module, which this file repeats for the
// GX and MMIO records it replays.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp)
#include "gpu/texture/tex_hashes.h"

#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include "gpu/pica/command_processor.h"
#include "gpu/texture/hash.h"
#include "gpu/texture/texture_regs.h"
#include "gpu_capture/check.h"
#include "mh4n/log.h"

namespace mh4n::gpu::texture {

namespace {
MH4N_OPTION("capture-player", "--tex-hashes-out", "FILE",
            "where --check tex-hashes writes its list (default: <capture>.tex-hashes.txt)");
} // namespace

std::string FormatTexHashes(const std::vector<TextureHashLine>& new_mode,
                            const std::vector<TextureHashLine>& old_mode) {
    std::string text = "# mh4n tex-hashes 1\n";
    const auto section = [&text](const char* header, const std::vector<TextureHashLine>& lines) {
        text += header;
        text += '\n';
        for (const TextureHashLine& line : lines) {
            text += fmt::format("{:016X} {}x{} {}\n", line.hash, line.width, line.height,
                                static_cast<u32>(line.format));
        }
    };
    section("# use_new_hash=1", new_mode);
    section("# use_new_hash=0", old_mode);
    return text;
}

namespace {

using gpu_capture::CaptureCheck;
using gpu_capture::CheckContext;
using gpu_capture::FrameColumns;
using gpu_capture::FrameInfo;
using gpu_capture::Record;

constexpr u32 kGxSubmitCmdList = 1;

class TexHashesCheck final : public CaptureCheck, public pica::Backend {
public:
    explicit TexHashesCheck(const Options& options) {
        out_path_ = options.GetString("--tex-hashes-out");
        if (out_path_.empty() && !options.positional().empty()) {
            out_path_ = options.positional()[0] + ".tex-hashes.txt";
        }
    }

    bool Start(CheckContext& context, std::string* error) override {
        host_ = &context.host;
        processor_ = std::make_unique<pica::CommandProcessor>(*host_, *this);
        if (out_path_.empty()) {
            *error = "tex-hashes: no output file (give --tex-hashes-out FILE)";
            return false;
        }
        return true;
    }

    std::vector<std::string> Columns() const override {
        return {"tex_new"};
    }

    std::string OnRecord(size_t, const Record& record) override {
        std::string error;
        switch (record.type) {
        case gpu_capture::kInit: {
            gpu::InitState init;
            if (!gpu_capture::MakeInitState(record, init, &error)) {
                return "tex-hashes: " + error;
            }
            processor_->LoadInitState(init);
            break;
        }
        case gpu_capture::kGx: {
            gpu::GxCommand command;
            if (!gpu_capture::ParseGx(record, command, &error)) {
                return "tex-hashes: " + error;
            }
            if ((command.words[0] & 0xFF) == kGxSubmitCmdList) {
                // GPU::VirtualToPhysicalAddress: 0 stays 0, an unknown address is taken as physical
                const u32 vaddr = command.words[1];
                const u32 paddr = vaddr == 0 ? 0 : host_->VirtualToPhysical(vaddr).value_or(vaddr);
                processor_->SubmitGxCmdList(paddr, command.words[2]);
            }
            break;
        }
        case gpu_capture::kMmio: {
            gpu_capture::MmioData mmio;
            if (!gpu_capture::ParseMmio(record, mmio, &error)) {
                return "tex-hashes: " + error;
            }
            const u32 window = mmio.address & 0xFFFFF000;
            if (window == pica::kGpuMmioBase || window == pica::kGpuMmioBase + 0x1000) {
                const u32 index = (mmio.address - pica::kGpuMmioBase) / 4;
                if (index >= pica::kExternalRegs && index - pica::kExternalRegs < pica::kNumRegs) {
                    processor_->WriteHardwareRegister(index - pica::kExternalRegs, mmio.value);
                }
            }
            break;
        }
        default:
            break;
        }
        return {};
    }

    std::string OnFrame(const FrameInfo&, FrameColumns& columns) override {
        columns.Set("tex_new", std::to_string(new_this_frame_));
        new_this_frame_ = 0;
        return {};
    }

    std::string Finish() override {
        std::ofstream file(out_path_, std::ios::binary | std::ios::trunc);
        if (!file) {
            return "tex-hashes: cannot write " + out_path_;
        }
        const std::string text = FormatTexHashes(new_lines_, old_lines_);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            return "tex-hashes: cannot write " + out_path_;
        }
        MH4N_LOG(Info, "TexHashes", "{} textures ({} draws, {} unreadable) written to {}",
                 new_lines_.size(), draws_, unreadable_, out_path_);
        return {};
    }

    // pica::Backend
    void Draw(const pica::DrawCall& draw) override {
        ++draws_;
        for (const TextureRef& ref : TexturesOfDraw(draw.state->regs.data())) {
            const std::size_t bytes = EncodedBytes(ref.format, ref.width, ref.height);
            const u8* memory = host_->PhysicalPointer(ref.paddr, static_cast<u32>(bytes));
            if (memory == nullptr) {
                ++unreadable_;
                continue;
            }
            const auto hashes = ComputeCustomHashes(memory, bytes, ref.width, ref.height, ref.format);
            if (!hashes) {
                ++unreadable_;
                continue;
            }
            const TextureHashLine new_line{hashes->new_hash, ref.width, ref.height, ref.format};
            const TextureHashLine old_line{hashes->old_hash, ref.width, ref.height, ref.format};
            if (seen_new_.insert(Key(new_line)).second) {
                new_lines_.push_back(new_line);
                ++new_this_frame_;
            }
            if (seen_old_.insert(Key(old_line)).second) {
                old_lines_.push_back(old_line);
            }
        }
    }

private:
    using LineKey = std::tuple<u64, u32, u32, u32>;
    static LineKey Key(const TextureHashLine& line) {
        return {line.hash, line.width, line.height, static_cast<u32>(line.format)};
    }

    std::string out_path_;
    gpu_capture::CaptureHost* host_ = nullptr;
    std::unique_ptr<pica::CommandProcessor> processor_;
    std::vector<TextureHashLine> new_lines_, old_lines_;
    std::set<LineKey> seen_new_, seen_old_;
    u64 draws_ = 0, unreadable_ = 0;
    u32 new_this_frame_ = 0;
};

} // namespace

MH4N_REGISTER(gpu_capture::CheckFactory, "tex-hashes", [](const Options& options) {
    return std::unique_ptr<gpu_capture::CaptureCheck>(std::make_unique<TexHashesCheck>(options));
});

} // namespace mh4n::gpu::texture
