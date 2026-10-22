#include "display_mem/display_mem.h"

#include <algorithm>
#include <utility>

#include "mh4n/log.h"
#include "mh4n/registry.h"

#if MH4N_DISPLAY_MEM_PLATFORM
#include "platform/platform_native.h"
#endif

namespace mh4n::display_mem {

namespace {

constexpr const char* kLog = "GPU.DisplayMem";

#if MH4N_DISPLAY_MEM_PLATFORM
// The top screen's framebuffer size register (word 0x17 of the first framebuffer block, which starts
// at word 0x100 of the GPU registers): the high half is the number of columns stored, the width the
// top screen shows (400, or what the hooks make of it).
constexpr u32 kTopSizeRegister = gpu_capture::DisplayShadow::kGpuBase + 4 * (0x100 + 0x17);
#endif

}  // namespace

DisplayMemGpu::DisplayMemGpu(gpu::Host& host, std::unique_ptr<gpu::Gpu> inner)
    : host_(host), inner_(std::move(inner)) {
    // Load takes the registers as little-endian bytes: read them from the inner GPU's register file.
    std::vector<u8> gpu_regs(4 * gpu_capture::DisplayShadow::kGpuRegWords);
    std::vector<u8> lcd_regs(4 * gpu_capture::DisplayShadow::kLcdRegWords);
    const auto put = [](std::vector<u8>& bytes, u32 index, u32 value) {
        for (u32 i = 0; i < 4; ++i) {
            bytes[4 * index + i] = static_cast<u8>(value >> (8 * i));
        }
    };
    for (u32 i = 0; i < gpu_capture::DisplayShadow::kGpuRegWords; ++i) {
        put(gpu_regs, i, inner_->ReadReg(gpu_capture::DisplayShadow::kGpuBase + 4 * i));
    }
    for (u32 i = 0; i < gpu_capture::DisplayShadow::kLcdRegWords; ++i) {
        put(lcd_regs, i, inner_->ReadReg(gpu_capture::DisplayShadow::kLcdBase + 4 * i));
    }
    shadow_.Load(gpu_regs.data(), lcd_regs.data());
}

void DisplayMemGpu::Execute(const gpu::GxCommand& command) {
    inner_->Execute(command);
}

void DisplayMemGpu::WriteReg(u32 addr, u32 value) {
    inner_->WriteReg(addr, value);
    shadow_.WriteReg(addr, value);
}

u32 DisplayMemGpu::ReadReg(u32 addr) {
    return inner_->ReadReg(addr);
}

void DisplayMemGpu::SetBufferSwap(u32 screen, const gpu::FrameBufferInfo& info) {
    inner_->SetBufferSwap(screen, info);
    // GPU::VirtualToPhysicalAddress: 0 stays 0, an address in no window is kept.
    const auto physical = [this](u32 vaddr) {
        if (vaddr == 0) {
            return 0u;
        }
        const auto converted = host_.VirtualToPhysical(vaddr);
        return converted ? *converted : vaddr;
    };
    shadow_.ApplySwap(screen, info, physical(info.address_left), physical(info.address_right));
}

void DisplayMemGpu::SetColorFill(u32 fill) {
    inner_->SetColorFill(fill);
    shadow_.ApplyFill(fill);
}

void DisplayMemGpu::FlushRegion(u32 vaddr, u32 size) {
    inner_->FlushRegion(vaddr, size);
}

void DisplayMemGpu::InvalidateRegion(u32 vaddr, u32 size) {
    inner_->InvalidateRegion(vaddr, size);
}

void DisplayMemGpu::VBlank(u32 screen) {
    inner_->VBlank(screen);
    if (screen != 0 || sinks_.empty()) {
        return;
    }
    present::DisplayFrame frame;
    frame.sequence = next_sequence_++;
    if (link_ != nullptr) {
        frame.game_frame = link_->game_frame.load();
        if (link_->runtime != nullptr) {
            frame.tick = link_->runtime->Clock().Now();
        }
    }
    frame.top = BuildScreen(0);
    frame.bottom = BuildScreen(1);
    for (present::ScreenSink* sink : sinks_) {
        sink->Publish(frame);
    }
}

std::optional<present::ScreenImage> DisplayMemGpu::BuildScreen(u32 screen) {
#if MH4N_DISPLAY_MEM_PLATFORM
    if (screen == 0 && link_ != nullptr && link_->runtime != nullptr) {
        // The reference's hooks write the top framebuffer's size register; here the platform keeps
        // what they asked for.
        if (const auto platform = platform::NativePlatform::Find(*link_->runtime)) {
            const u32 rows = std::min<u32>(platform->TopScreenRows(), 0xFFFF);
            if (rows != 0) {
                const u32 old = shadow_.GpuReg((kTopSizeRegister - gpu_capture::DisplayShadow::kGpuBase) / 4);
                shadow_.WriteReg(kTopSizeRegister, (old & 0xFFFF) | (rows << 16));
            }
        }
    }
#endif
    gpu_capture::Image image;
    std::string error;
    const gpu_capture::PhysicalPointerFn memory = [this](u32 paddr, u32 size) -> const u8* {
        return host_.PhysicalPointer(paddr, size);
    };
    if (!shadow_.BuildScreen(screen, memory, image, &error)) {
        if (error != last_error_[screen]) {
            MH4N_LOG(Warning, kLog, "screen {} is not shown: {}", screen, error);
            last_error_[screen] = error;
        }
        return std::nullopt;
    }
    last_error_[screen].clear();
    present::ScreenImage result;
    result.width = image.width;
    result.height = image.height;
    auto pixels = std::make_shared<std::vector<u8>>(static_cast<std::size_t>(image.width) * image.height * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(image.width) * image.height; ++i) {
        (*pixels)[4 * i + 0] = image.rgb[3 * i + 0];
        (*pixels)[4 * i + 1] = image.rgb[3 * i + 1];
        (*pixels)[4 * i + 2] = image.rgb[3 * i + 2];
        (*pixels)[4 * i + 3] = 0xFF;
    }
    result.pixels = std::move(pixels);
    result.region = {0, 0, result.width, result.height};
    return result;
}

void DisplayMemGpu::AddScreenSink(present::ScreenSink* sink) {
    sinks_.push_back(sink);
}

void DisplayMemGpu::RemoveScreenSink(present::ScreenSink* sink) {
    std::erase(sinks_, sink);
}

void DisplayMemGpu::ReadScreen(const present::ScreenImage& screen, std::function<void(present::RgbImage)> done) {
    present::RgbImage out;
    if (screen.Valid() && screen.IsCpu()) {
        out.width = screen.width;
        out.height = screen.height;
        const std::size_t count = static_cast<std::size_t>(screen.width) * screen.height;
        out.rgb.resize(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            std::copy_n(screen.pixels->data() + 4 * i, 3, out.rgb.data() + 3 * i);
        }
    }
    done(std::move(out));
}

void DisplayMemGpu::Attach(std::shared_ptr<RuntimeLink> link) {
    link_ = std::move(link);
}

namespace {

std::unique_ptr<gpu::Gpu> MakeDisplayMem(gpu::Host& host, const Options& options) {
    const gpu::GpuFactory* null_factory = Registry<gpu::GpuFactory>::Instance().Find("null");
    if (null_factory == nullptr) {
        MH4N_LOG(Error, kLog, "the null GPU is not registered");
        return nullptr;
    }
    return std::make_unique<DisplayMemGpu>(host, (*null_factory)(host, options));
}

// The GPU factory gets no runtime: once the `devices` step has made the GPU, this step tells it
// which runtime it belongs to and follows the frame counter.
void AttachStep(Runtime& rt) {
    auto* gpu = dynamic_cast<DisplayMemGpu*>(rt.Interfaces().gpu);
    if (gpu == nullptr) {
        return;
    }
    auto link = std::make_shared<RuntimeLink>();
    link->runtime = &rt;
    rt.OnFrame([link](u32 frame, u64) { link->game_frame.store(frame); });
    gpu->Attach(std::move(link));
}

}  // namespace

MH4N_REGISTER(gpu::GpuFactory, "null-display", MakeDisplayMem);
MH4N_REGISTER(present::DisplayGpu, "null-display", {});
MH4N_REGISTER(BootStep, "display_mem.attach", BootStep{Phase::Services, AttachStep});

}  // namespace mh4n::display_mem
