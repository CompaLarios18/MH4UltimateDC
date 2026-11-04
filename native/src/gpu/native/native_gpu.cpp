// Derived from Azahar src/video_core/rasterizer_cache/framebuffer_base.h (FramebufferHelper), src/video_core/renderer_vulkan/
// vk_rasterizer.cpp (Draw; SyncTextureUnits: which texture each unit binds, the null surfaces for a unit that is off),
// src/video_core/pica/regs_framebuffer.h, regs_rasterizer.h, regs_texturing.h, src/video_core/gpu.cpp and
// src/video_core/renderer_software/sw_framebuffer.cpp (Framebuffer::DrawShadowMapPixel: a shadow map is at the colour buffer's
// address, 4 bytes a texel whatever the colour format) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// vk_rasterizer.cpp says "Licensed under GPLv3 or any later version"; the others say "Licensed under GPLv2 or any later
// version"): the viewport, scissor and draw rectangle of a draw (mirrored when the framebuffer is flipped), the surfaces a draw
// uses (no depth surface in shadow mode), the register positions of the framebuffer block. The rest (the targets, the shadow
// targets, the write-back, the recorder and ring management, the counters) is ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (vk_rasterizer.cpp, gpu.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (framebuffer_base.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017 Citra Emulator Project (sw_framebuffer.cpp)
//
// `--gpu native` (N2-08; the shadow targets, the shadow-write draw and unit 0's shadow binding N2-12's). See native_gpu.h for the
// order of a draw and what the render targets are.
#include "gpu/native/native_gpu.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <map>
#include <optional>
#include <tuple>

#include <fmt/format.h>

#include "gpu/backend/state/state.h"
#include "gpu/cpuvertex/cpu_vertex.h"
#include "gpu/cpuvertex/primitive_assembly.h"
#include "gpu/cpuvertex/triangle_batch.h"
#include "gpu/fbops/fixed_function.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"
#include "gpu/fragment/profile.h"
#include "gpu/fragment/sampling/sampling.h"
#include "gpu/fragment/state.h"
#include "gpu/fragment/tables.h"
#include "gpu/gx/engine.h"
#include "gpu/native/compose.h"
#include "gpu/native/default_provider.h"
#include "gpu/native/pipeline_builder.h"
#include "gpu/native/vertex_input.h"
#include "gpu/pica/command_processor.h"
#include "gpu/shader/vertex_shader.h"
#include "gpu/texture/cache.h"
#include "gpu/texture/decode.h"
#include "gpu/texture/texture_regs.h"
#include "gpu/vertex/vertex_loader.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"

namespace mh4n::gpu::native {
namespace {

constexpr const char* kLog = "NativeGpu";

constexpr u32 kVaddrLcd = 0x1ED02000;
constexpr u32 kLcdRegs = 0x400;

// RegsInternal: FramebufferRegs at 0x100, its framebuffer block at 0x110; RasterizerRegs at 0x40.
constexpr u32 kRegViewportSizeX = 0x41;
constexpr u32 kRegViewportSizeY = 0x43;
constexpr u32 kRegScissorXY1 = 0x66;
constexpr u32 kRegScissorXY2 = 0x67;
constexpr u32 kRegViewportCorner = 0x68;
constexpr u32 kRegDepthFormat = 0x116;
constexpr u32 kRegColorFormat = 0x117;
constexpr u32 kRegDepthAddress = 0x11C;
constexpr u32 kRegColorAddress = 0x11D;
constexpr u32 kRegFramebufferSize = 0x11E;

// The vertex-stage rings; the recording is submitted before they run out (see EnsureRoom).
constexpr u64 kVertexRingBytes = 16ull << 20;
constexpr u64 kVsUniformRingBytes = 2ull << 20;
constexpr u64 kStagingRingBytes = 8ull << 20;
// The decoded textures kept as images (a texture is level 0 only until N2-15); past this the ones the current recording does not
// use are released.
constexpr u64 kTextureBudgetBytes = 768ull << 20;
constexpr u32 kMaxDrawsPerRecording = 256;
constexpr u32 kVsUniformAllocation = 2048;  // PicaUniformData (1,616) padded to 1,792, then VertexUniformData (32)

enum class ColorFormat : u32 { RGBA8 = 0, RGB8 = 1, RGB5A1 = 2, RGB565 = 3, RGBA4 = 4 };
enum class DepthFormat : u32 { D16 = 0, D24 = 2, D24S8 = 3 };

u32 ColorBytesPerPixel(u32 format) {
    switch (static_cast<ColorFormat>(format)) {
    case ColorFormat::RGBA8: return 4;
    case ColorFormat::RGB8: return 3;
    default: return 2;
    }
}

u32 DepthBytesPerPixel(u32 format) {
    switch (static_cast<DepthFormat>(format)) {
    case DepthFormat::D16: return 2;
    case DepthFormat::D24: return 3;
    default: return 4;
    }
}

rhi::PicaFormat ToPicaFormat(u32 color_format) {
    switch (static_cast<ColorFormat>(color_format)) {
    case ColorFormat::RGB8: return rhi::PicaFormat::Rgb8;
    case ColorFormat::RGB5A1: return rhi::PicaFormat::Rgb5A1;
    case ColorFormat::RGB565: return rhi::PicaFormat::Rgb565;
    case ColorFormat::RGBA4: return rhi::PicaFormat::Rgba4;
    default: return rhi::PicaFormat::Rgba8;
    }
}

rhi::PicaFormat ToPicaDepthFormat(u32 depth_format) {
    switch (static_cast<DepthFormat>(depth_format)) {
    case DepthFormat::D16: return rhi::PicaFormat::D16;
    case DepthFormat::D24: return rhi::PicaFormat::D24;
    default: return rhi::PicaFormat::D24S8;
    }
}

// The logical format of the depth image a PICA depth format is drawn into (the backend's ChooseFormat picks the device's).
rhi::Format DepthImageFormat(u32 depth_format) {
    return static_cast<DepthFormat>(depth_format) == DepthFormat::D16 ? rhi::Format::D16Unorm : rhi::Format::D24S8;
}

struct Rect4 {
    s32 left = 0, top = 0, right = 0, bottom = 0;  // Azahar's Common::Rectangle: top >= bottom (y up)
};

struct DrawGeometry {
    rhi::Viewport viewport;
    rhi::Rect scissor;
    bool empty = false;
    fragment::UniformContext context;
};

// FramebufferHelper's constructor at scale 1.
DrawGeometry MakeGeometry(const pica::PicaState& state, u32 width, u32 height, bool flipped) {
    const auto& regs = state.regs;
    DrawGeometry out;
    const u32 corner = regs[kRegViewportCorner];
    auto signed10 = [](u32 v) { return static_cast<s32>(v << 22) >> 22; };
    const s32 corner_x = signed10(pica::BitsOf(corner, 0, 10));
    const s32 corner_y = signed10(pica::BitsOf(corner, 16, 10));
    const s32 size_x = static_cast<s32>(pica::Float24ToFloat(pica::BitsOf(regs[kRegViewportSizeX], 0, 24)) * 2);
    const s32 size_y = static_cast<s32>(pica::Float24ToFloat(pica::BitsOf(regs[kRegViewportSizeY], 0, 24)) * 2);
    // GetViewportRect: left, top = corner.y + height, right, bottom = corner.y
    Rect4 viewport{corner_x, corner_y + size_y, corner_x + size_x, corner_y};
    const s32 h = static_cast<s32>(height);
    if (flipped) {  // VerticalMirror(height)
        viewport = Rect4{viewport.left, h - viewport.bottom, viewport.right, h - viewport.top};
    }
    const s32 w = static_cast<s32>(width);
    Rect4 draw;
    draw.left = std::clamp(viewport.left, 0, w);
    draw.top = std::clamp(viewport.top, 0, h);
    draw.right = std::clamp(viewport.right, 0, w);
    draw.bottom = std::clamp(viewport.bottom, 0, h);
    out.viewport.x = static_cast<float>(viewport.left);
    out.viewport.y = static_cast<float>(viewport.bottom);
    out.viewport.width = static_cast<float>(std::abs(viewport.right - viewport.left));
    out.viewport.height = static_cast<float>(std::abs(viewport.bottom - viewport.top));
    out.viewport.min_depth = 0.0f;
    out.viewport.max_depth = 1.0f;
    out.scissor = rhi::Rect{draw.left, draw.bottom, static_cast<u32>(std::abs(draw.right - draw.left)),
                            static_cast<u32>(std::abs(draw.top - draw.bottom))};
    out.empty = out.scissor.width == 0 || out.scissor.height == 0;

    // The scissor uniform: x1, y2, x2, y1 of FramebufferHelper::Scissor() (left, top, right, bottom).
    const u32 xy1 = regs[kRegScissorXY1];
    const u32 xy2 = regs[kRegScissorXY2];
    Rect4 scissor{static_cast<s32>(pica::BitsOf(xy1, 0, 10)), static_cast<s32>(pica::BitsOf(xy2, 16, 10)) + 1,
                  static_cast<s32>(pica::BitsOf(xy2, 0, 10)) + 1, static_cast<s32>(pica::BitsOf(xy1, 16, 10))};
    if (flipped) {
        scissor = Rect4{scissor.left, h - scissor.bottom, scissor.right, h - scissor.top};
    }
    out.context.framebuffer_scale = 1;
    out.context.scissor = fragment::UniformContext::Scissor{scissor.left, scissor.bottom, scissor.right, scissor.top};
    return out;
}

// ---- the render targets --------------------------------------------------------------------------------------------

struct Target {
    std::shared_ptr<rhi::Image> image;
    bool depth = false;
    bool shadow = false;  // a shadow map (N2-12): RGBA8 made with shadow_source, written in the shadow-write pass
    u32 paddr = 0;
    u32 format = 0;  // the PICA's colour or depth format
    u32 width = 0, height = 0;
    u32 bytes = 0;
    bool gpu_dirty = false;  // drawn into since the last write-back
    bool stale = true;       // guest memory may be newer than the image (and has never been loaded)

    bool Overlaps(u32 begin, u32 end) const { return paddr < end && begin < paddr + bytes; }
};

// ---- the textures (N2-23) ------------------------------------------------------------------------------------------------

// A texture image is keyed by what the cache is keyed by: a 2D texture by one cache key, a cube by its six faces. A Shadow2D
// texture (N2-12) is another image of the same memory, made with shadow_source (its R32_UINT view is what the lookup reads).
struct TexImageKey {
    bool cube = false;
    bool shadow = false;
    std::array<texture::CacheKey, 6> faces{};
    friend bool operator<(const TexImageKey& a, const TexImageKey& b) {
        return std::tie(a.cube, a.shadow, a.faces) < std::tie(b.cube, b.shadow, b.faces);
    }
};
struct TexImage {
    std::shared_ptr<rhi::Image> image;
    std::array<u32, 6> generation{};  // of the cache's decoded face the image holds
    u64 bytes = 0;
    u64 stamp = 0;  // the recording that last bound it
};

// ---- the GPU --------------------------------------------------------------------------------------------------------

class Impl final : public NativeGpu, public pica::Backend {
public:
    Impl(Host& host, const NativeOptions& options)
        : host_(host), options_(options), gx_(host), processor_(host, *this), registry_(&fragment::FeatureRegistry::Global()) {
        processor_.SetRegisterWriteLog(true);
    }

    ~Impl() override {
        if (renderer_ready_) {
            FlushRecording();
            backend_->WaitIdle();
        }
    }

    // ---- Gpu ----
    void Execute(const GxCommand& command) override {
        if ((command.words[0] & 0xFF) == 1) {  // SubmitCmdList
            const u32 vaddr = command.words[1];
            const u32 paddr = vaddr == 0 ? 0 : host_.VirtualToPhysical(vaddr).value_or(vaddr);
            processor_.SubmitGxCmdList(paddr, command.words[2]);
            FlushRecording();
            return;
        }
        // Fills, transfers, copies and DMA are the CPU engine's: the memory must be what the GPU drew, and the targets
        // must be loaded again afterwards.
        WriteBackAll();
        gx_.Execute(command);
        MarkAllStale();
        if (texture_cache_) {
            texture_cache_->OnGpuWrite(0, 0xFFFFFFFFu);  // a fill, transfer or copy may have written any texture's memory
        }
    }

    void WriteReg(u32 addr, u32 value) override {
        const u32 window = addr & 0xFFFFF000;
        if (window == pica::kGpuMmioBase || window == pica::kGpuMmioBase + 0x1000) {
            const u32 index = (addr - pica::kGpuMmioBase) / 4;
            if (index >= pica::kExternalRegs) {
                if (index - pica::kExternalRegs < pica::kNumRegs) {
                    processor_.WriteHardwareRegister(index - pica::kExternalRegs, value);
                }
                return;
            }
        }
        gx_.WriteReg(addr, value);
    }

    u32 ReadReg(u32 addr) override {
        const u32 window = addr & 0xFFFFF000;
        if (window == pica::kGpuMmioBase || window == pica::kGpuMmioBase + 0x1000) {
            const u32 index = (addr - pica::kGpuMmioBase) / 4;
            if (index >= pica::kExternalRegs) {
                return index - pica::kExternalRegs < pica::kNumRegs ? processor_.state().regs[index - pica::kExternalRegs] : 0;
            }
        }
        return gx_.ReadReg(addr);
    }

    void SetBufferSwap(u32 screen, const FrameBufferInfo& info) override { gx_.SetBufferSwap(screen, info); }
    void SetColorFill(u32 fill) override { gx_.SetColorFill(fill); }

    void FlushRegion(u32 vaddr, u32 size) override {
        // The CPU wrote this range: a target that covers it is out of date.
        if (!renderer_ready_) {
            return;
        }
        if (const std::optional<u32> paddr = host_.VirtualToPhysical(vaddr)) {
            WriteBackOverlapping(*paddr, size, /*stale=*/true);
            texture_cache_->OnFlush(*paddr, size);
        } else {
            WriteBackAll();
            MarkAllStale();
            texture_cache_->OnFlush(0, 0xFFFFFFFFu);
        }
    }

    void InvalidateRegion(u32 vaddr, u32 size) override {
        // The CPU is about to read this range: what the GPU drew there goes to memory first.
        if (!renderer_ready_) {
            return;
        }
        if (const std::optional<u32> paddr = host_.VirtualToPhysical(vaddr)) {
            WriteBackOverlapping(*paddr, size, /*stale=*/false);
            texture_cache_->OnInvalidate(*paddr, size);
        } else {
            WriteBackAll();
            texture_cache_->OnInvalidate(0, 0xFFFFFFFFu);
        }
    }

    void VBlank(u32) override {}

    // ---- GpuDebug ----
    void LoadInitState(const InitState& init) override {
        processor_.LoadInitState(init);
        // The GPU register block (the external registers) belongs to the GX engine; the three trigger words are written
        // with their trigger bits cleared so that loading a state starts no fill or transfer. Only its first
        // pica::kExternalRegs words are external registers: the words from there on are internal registers 0 to 0x31,
        // which the command processor loaded above (and MMIO from word 0x400 on goes to the processor, not to the engine).
        const auto gpu_regs = init.Block(InitBlock::GpuRegs);
        if (gpu_regs.size() == pica::kGpuRegsBlockWords * 4) {
            for (size_t i = 0; i < std::min<size_t>(gpu_regs.size() / 4, pica::kExternalRegs); ++i) {
                u32 word;
                std::memcpy(&word, gpu_regs.data() + 4 * i, 4);
                if (i == gx::kRegFillControl0 || i == gx::kRegFillControl1 || i == gx::kRegTransferTrigger) {
                    word &= ~1u;
                }
                gx_.WriteReg(pica::kGpuMmioBase + 4 * static_cast<u32>(i), word);
            }
        }
        const auto lcd_regs = init.Block(InitBlock::LcdRegs);
        if (lcd_regs.size() == kLcdRegs * 4) {
            for (size_t i = 0; i < kLcdRegs; ++i) {
                u32 word;
                std::memcpy(&word, lcd_regs.data() + 4 * i, 4);
                gx_.WriteReg(kVaddrLcd + 4 * static_cast<u32>(i), word);
            }
        }
    }

    std::optional<std::vector<RegisterWrite>> TakeRegisterWrites() override { return processor_.TakeRegisterWrites(); }

    void WriteBackColorBuffers() override { WriteBackAll(); }

    std::vector<Counter> TakeFrameCounters() override {
        std::vector<Counter> out = PeekCounters();
        frame_ = FrameCounters{};
        return out;
    }

    std::vector<Counter> PeekCounters() const override {
        std::vector<Counter> out;
        auto add = [&](const std::string& name, u64 value) { out.push_back(Counter{name, value}); };
        add("native_draws", frame_.draws);
        add("native_draws_dropped", frame_.dropped);
        add("cpu_path_draws", frame_.cpu_path_draws);
        add("cpu_path_unhandled", frame_.cpu_path_unhandled);
        add("cpu_path_load_failed", frame_.cpu_path_load_failed);
        add("translate_failures", frame_.translate_failures);
        add(vertex::kCounterRestartWidened, frame_.restart_widened);
        add("pipeline_sync_builds", frame_.sync_builds);
        add("pipeline_forced_uber", frame_.forced_uber);
        add("stubbed:fixup", frame_.stubbed_fixup);
        add("dst_copies", frame_.dst_copies);
        add("fallback_draws", frame_.fallback_draws);
        add("stubbed:blend_plan", frame_.stubbed_blend_plan);
        add("texture_binds", frame_.texture_binds);
        add("texture_uploads", frame_.texture_uploads);
        add("texture_unavailable", frame_.texture_unavailable);
        add("sampler_creates", frame_.sampler_creates);
        add("texture_levels_missing", frame_.texture_levels_missing);
        add("shadow_write_draws", frame_.shadow_write_draws);
        add("shadow_lookups", frame_.shadow_lookups);
        add("shadow_unavailable", frame_.shadow_unavailable);
        for (const char* name : {"sampling", "lighting", "fog", "proctex", "shadow", "gas"}) {
            const auto it = frame_.stubbed.find(std::string_view(name));
            add(std::string("stubbed:") + name, it == frame_.stubbed.end() ? 0 : it->second);
        }
        return out;
    }

    std::optional<u64> TakeFrameGpuTimeNs() override {
        if (!renderer_ready_ || !frame_marked_) {
            return std::nullopt;
        }
        EnsureRecording();
        rec_->MarkFrame(rhi::FrameMark::End);
        frame_marked_ = false;
        FlushRecording();
        backend_->WaitIdle();
        return backend_->FrameGpuTimeNs();
    }

    // ---- pica::Backend ----
    void Draw(const pica::DrawCall& draw) override {
        if (!EnsureRenderer()) {
            ++frame_.dropped;
            return;
        }
        DrawImpl(draw);
    }

private:
    struct FrameCounters {
        u64 draws = 0, dropped = 0;
        u64 cpu_path_draws = 0, cpu_path_unhandled = 0, cpu_path_load_failed = 0;
        u64 translate_failures = 0, restart_widened = 0;
        u64 sync_builds = 0, forced_uber = 0;
        u64 stubbed_fixup = 0, dst_copies = 0, fallback_draws = 0, stubbed_blend_plan = 0;
        u64 texture_binds = 0, texture_uploads = 0, texture_unavailable = 0, sampler_creates = 0, texture_levels_missing = 0;
        // N2-12: draws recorded in the shadow-write pass; draws whose unit 0 is Shadow2D or ShadowCube and that bound a decoded
        // map; shadow-mode draws dropped because the shadow target could not be made (also counted in `dropped`)
        u64 shadow_write_draws = 0, shadow_lookups = 0, shadow_unavailable = 0;
        std::map<std::string, u64, std::less<>> stubbed;
    };

    // ---- the renderer, made on first use ----
    bool EnsureRenderer() {
        if (renderer_ready_) {
            return true;
        }
        if (renderer_failed_) {
            return false;
        }
        renderer_failed_ = true;
        rhi::Result<std::shared_ptr<rhi::Instance>> instance = rhi::CreateInstance();
        if (!instance.ok()) {
            MH4N_LOG(Error, kLog, "no Vulkan instance: {}", instance.error());
            return false;
        }
        instance_ = *instance;
        rhi::Result<std::shared_ptr<rhi::Backend>> backend = rhi::CreateBackend(instance_);
        if (!backend.ok()) {
            MH4N_LOG(Error, kLog, "no Vulkan device: {}", backend.error());
            return false;
        }
        backend_ = *backend;
        const rhi::Capabilities& caps = backend_->Caps();
        MH4N_LOG(Info, kLog, "device: {} ({})", caps.device_name, caps.driver_name);
        profile_ = fragment::ProfileOf(caps);

        builder_ = std::make_unique<PipelineBuilder>(backend_, *registry_);
        provider_ = std::make_unique<DefaultPipelineProvider>(
            [this](const PipelineKey& key) { return builder_->Build(key); }, DefaultProviderOptions{options_.force_ubershader});
        rhi::Result<std::unique_ptr<DrawComposer>> composer = DrawComposer::Create(backend_, *provider_);
        if (!composer.ok()) {
            MH4N_LOG(Error, kLog, "composer: {}", composer.error());
            return false;
        }
        composer_ = std::move(*composer);
        rhi::Result<std::unique_ptr<fragment::FragmentUniformRing>> uniforms = fragment::FragmentUniformRing::Create(*backend_);
        rhi::Result<std::unique_ptr<fragment::TableRings>> tables = fragment::TableRings::Create(*backend_);
        rhi::Result<std::unique_ptr<rhi::Ring>> vertex_ring =
            backend_->CreateRing(kVertexRingBytes, rhi::BufferUsage::Vertex | rhi::BufferUsage::Index, "native vertices");
        rhi::Result<std::unique_ptr<rhi::Ring>> vs_ring =
            backend_->CreateRing(kVsUniformRingBytes, rhi::BufferUsage::Uniform, "native vertex-stage uniforms");
        rhi::Result<std::unique_ptr<rhi::Ring>> staging =
            backend_->CreateRing(kStagingRingBytes, rhi::BufferUsage::Transfer, "native staging");
        if (!uniforms.ok() || !tables.ok() || !vertex_ring.ok() || !vs_ring.ok() || !staging.ok()) {
            MH4N_LOG(Error, kLog, "rings: {} {} {} {} {}", uniforms.error(), tables.error(), vertex_ring.error(), vs_ring.error(),
                     staging.error());
            return false;
        }
        uniform_ring_ = std::move(*uniforms);
        tables_ = std::move(*tables);
        vertex_ring_ = std::move(*vertex_ring);
        vs_ring_ = std::move(*vs_ring);
        staging_ring_ = std::move(*staging);
        fragment_state_ = std::make_unique<fragment::FragmentState>(profile_, *registry_);
        texture_cache_ = std::make_unique<texture::TextureCache>(host_);
        samplers_ = std::make_unique<fragment::sampling::SamplerCache>(backend_);

        vertex::VertexCaps vertex_caps;
        vertex_caps.triangle_fans = caps.triangle_fans;
        vertex_caps.index_uint8 = caps.index_uint8;
        vertex_caps.apple = caps.apple;
        vertex_caps.stride_alignment = caps.stride_alignment;
        vertex_caps.primitive_restart_always_on = caps.primitive_restart_always_on;
        loader_ = std::make_unique<vertex::VertexLoader>(vertex_caps);

        renderer_failed_ = false;
        renderer_ready_ = true;
        return true;
    }

    void EnsureRecording() {
        if (rec_) {
            return;
        }
        rec_.emplace(backend_->BeginRecording());
        if (!frame_marked_) {
            rec_->MarkFrame(rhi::FrameMark::Begin);
            frame_marked_ = true;
        }
        composer_->BeginRecording(*rec_);
        uniform_ring_->BeginRecording();
        ++recording_serial_;
        TrimTextures();
        draws_in_recording_ = 0;
        vertex_bytes_in_recording_ = 0;
        dynamic_applied_ = false;
    }

    void FlushRecording() {
        if (!rec_) {
            return;
        }
        composer_->EndPass(*rec_);
        last_submit_ = backend_->Submit(std::move(*rec_));
        rec_.reset();
    }

    // Submits the recording when the rings that hold its data would run out.
    void EnsureRoom(u64 vertex_bytes) {
        if (rec_ && (draws_in_recording_ >= kMaxDrawsPerRecording ||
                     vertex_bytes_in_recording_ + 2 * vertex_bytes > kVertexRingBytes / 2)) {
            FlushRecording();
        }
        EnsureRecording();
    }

    // ---- targets ----
    // A shadow target (N2-12) is keyed apart from a colour target of the same range: a colour target has no storage view and a
    // shadow source is not an attachment (rhi.h), so the two are two images, and UseTarget's overlap rule carries the contents
    // between them through guest memory. Its format is always RGBA8 (PICA format 0).
    Target* FindTarget(bool depth, u32 paddr, u32 format, u32 width, u32 height, bool shadow = false) {
        for (auto& t : targets_) {
            if (t->depth == depth && t->shadow == shadow && t->paddr == paddr && t->format == format && t->width == width &&
                t->height == height) {
                return t.get();
            }
        }
        return nullptr;
    }

    Target* GetTarget(bool depth, u32 paddr, u32 format, u32 width, u32 height, bool shadow = false) {
        if (Target* found = FindTarget(depth, paddr, format, width, height, shadow)) {
            return found;
        }
        rhi::ImageDesc desc;
        desc.format = depth ? DepthImageFormat(format) : rhi::Format::Rgba8Unorm;
        desc.width = width;
        desc.height = height;
        desc.shadow_source = shadow;
        desc.name = depth ? "pica depth buffer" : (shadow ? "pica shadow map" : "pica colour buffer");
        rhi::Result<std::shared_ptr<rhi::Image>> image = backend_->CreateImage(desc);
        if (!image.ok()) {
            MH4N_LOG(Error, kLog, "render target {}x{}: {}", width, height, image.error());
            return nullptr;
        }
        auto target = std::make_unique<Target>();
        target->image = *image;
        target->depth = depth;
        target->shadow = shadow;
        target->paddr = paddr;
        target->format = format;
        target->width = width;
        target->height = height;
        target->bytes = width * height * (depth ? DepthBytesPerPixel(format) : ColorBytesPerPixel(format));
        targets_.push_back(std::move(target));
        return targets_.back().get();
    }

    u8* GuestMemory(const Target& t) { return host_.PhysicalPointer(t.paddr, t.bytes); }

    // Loads a target from guest memory (colour) or starts it at 1.0 (depth). Outside a pass.
    void LoadTarget(Target& t) {
        EnsureRecording();
        composer_->EndPass(*rec_);
        t.stale = false;
        if (t.depth) {
            rhi::ClearValue clear;
            clear.depth = 1.0f;
            clear.stencil = 0;
            rec_->ClearImage(*t.image, clear);
            return;
        }
        std::vector<u8> rgba;
        const u8* memory = GuestMemory(t);
        const bool decoded = memory != nullptr && t.width % 8 == 0 && t.height % 8 == 0 &&
                             texture::DecodeToRgba8(memory, t.bytes, t.width, t.height, static_cast<texture::Format>(t.format), rgba);
        if (!decoded) {
            rhi::ClearValue clear;
            clear.color = {0.0f, 0.0f, 0.0f, 0.0f};
            rec_->ClearImage(*t.image, clear);
            return;
        }
        rhi::Result<rhi::RingAllocation> alloc = staging_ring_->Map(rgba.size(), 16);
        if (!alloc.ok()) {
            FlushRecording();
            EnsureRecording();
            alloc = staging_ring_->Map(rgba.size(), 16);
        }
        if (!alloc.ok()) {
            MH4N_LOG(Error, kLog, "staging for a render target: {}", alloc.error());
            return;
        }
        // The decoder keeps the memory's order (guest row g is row g of `rgba`); the write-back reads guest row g from image row
        // height - 1 - g (readback.comp: Azahar's DrawPixel puts window row y into row height - y of the buffer, the size register
        // holding height - 1). The load puts the rows where the write-back reads them, so a target no draw touches comes back
        // unchanged.
        const size_t row_bytes = static_cast<size_t>(t.width) * 4;
        for (u32 g = 0; g < t.height; ++g) {
            std::memcpy(alloc->data.data() + static_cast<size_t>(t.height - 1 - g) * row_bytes,
                        rgba.data() + static_cast<size_t>(g) * row_bytes, row_bytes);
        }
        staging_ring_->GetBuffer()->Flush(alloc->offset, rgba.size());
        if (!staging_ring_->Commit(rgba.size()).ok()) {
            return;
        }
        rhi::BufferImageRegion region;
        region.buffer_offset = alloc->offset;
        region.image_rect = rhi::Rect{0, 0, t.width, t.height};
        region.aspect = rhi::ImageAspect::Color;
        rec_->CopyBufferToImage(*staging_ring_->GetBuffer(), *t.image, std::span<const rhi::BufferImageRegion>(&region, 1));
    }

    void WriteBack(Target& t) {
        if (!t.gpu_dirty) {
            return;
        }
        t.gpu_dirty = false;
        FlushRecording();
        u8* memory = GuestMemory(t);
        if (memory == nullptr) {
            return;
        }
        const rhi::PicaFormat format = t.depth ? ToPicaDepthFormat(t.format) : ToPicaFormat(t.format);
        rhi::Result<void> r = backend_->ReadbackToGuest(*t.image, rhi::Rect{0, 0, t.width, t.height}, format,
                                                        std::span<u8>(memory, t.bytes));
        if (!r.ok()) {
            MH4N_LOG(Error, kLog, "write-back of a render target: {}", r.error());
        }
        texture_cache_->OnGpuWrite(t.paddr, t.bytes);  // a texture drawn into is out of date in the cache
    }

    void WriteBackAll() {
        if (!renderer_ready_) {
            return;
        }
        for (auto& t : targets_) {
            WriteBack(*t);
        }
    }

    void WriteBackOverlapping(u32 paddr, u32 size, bool stale) {
        for (auto& t : targets_) {
            if (t->Overlaps(paddr, paddr + size)) {
                WriteBack(*t);
                if (stale) {
                    t->stale = true;
                }
            }
        }
    }

    void MarkAllStale() {
        for (auto& t : targets_) {
            t->stale = true;
        }
    }

    // The target a draw uses: written back and stale-marked where another target overlaps, loaded when stale.
    Target* UseTarget(bool depth, u32 paddr, u32 format, u32 width, u32 height, bool shadow = false) {
        Target* t = GetTarget(depth, paddr, format, width, height, shadow);
        if (t == nullptr) {
            return nullptr;
        }
        for (auto& other : targets_) {
            if (other.get() != t && other->Overlaps(t->paddr, t->paddr + t->bytes)) {
                WriteBack(*other);
                other->stale = true;
            }
        }
        if (t->stale) {
            LoadTarget(*t);
        }
        return t;
    }

    // ---- textures (N2-23) ----
    // Which texture units the draw reads: a TEV stage that is not a pass-through, or lighting's bump map or shadow map.
    std::array<bool, 3> UnitsRead(const fragment::FragmentKey& key) const {
        std::array<bool, 3> used{fragment::TevUsesSource(key, 3), fragment::TevUsesSource(key, 4), fragment::TevUsesSource(key, 5)};
        if (key.lighting.enable() != 0) {
            if (key.lighting.bump_mode() != fragment::regenum::LightingBumpMode::None && key.lighting.bump_selector() < 3) {
                used[key.lighting.bump_selector()] = true;
            }
            if (key.lighting.enable_shadow() != 0 && key.lighting.shadow_selector() < 3) {
                used[key.lighting.shadow_selector()] = true;
            }
        }
        return used;
    }

    // The 1x1 images a unit holds when it has nothing to show (Azahar binds a null surface to a unit that is off; the software
    // renderer gives transparent black for it and opaque black for an enabled unit with address 0).
    void EnsureNullTextures() {
        if (null_2d_ == nullptr) {
            rhi::ImageDesc desc;
            desc.format = rhi::Format::Rgba8Unorm;
            desc.width = desc.height = 1;
            desc.name = "null texture";
            auto a = backend_->CreateImage(desc);
            desc.name = "null opaque-black texture";
            auto b = backend_->CreateImage(desc);
            desc.name = "null cube texture";
            desc.cube = true;
            desc.layers = 6;
            auto c = backend_->CreateImage(desc);
            // N2-12: a Shadow2D unit with nothing to show reads word 0 through the R32_UINT view (null_2d_ has no such view)
            desc.name = "null shadow texture";
            desc.cube = false;
            desc.layers = 1;
            desc.shadow_source = true;
            auto s = backend_->CreateImage(desc);
            if (!a.ok() || !b.ok() || !c.ok() || !s.ok()) {
                MH4N_LOG(Error, kLog, "null textures: {} {} {} {}", a.error(), b.error(), c.error(), s.error());
                return;
            }
            null_2d_ = *a;
            null_black_2d_ = *b;
            null_cube_ = *c;
            null_shadow_ = *s;
            null_cleared_ = false;
        }
        if (!null_cleared_) {
            EnsureRecording();
            composer_->EndPass(*rec_);
            rhi::ClearValue clear;
            rec_->ClearImage(*null_2d_, clear);
            rec_->ClearImage(*null_shadow_, clear);
            rec_->ClearImage(*null_cube_, clear, 0, 0);
            for (u32 layer = 1; layer < 6; ++layer) {
                rec_->ClearImage(*null_cube_, clear, 0, layer);
            }
            clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
            rec_->ClearImage(*null_black_2d_, clear);
            null_cleared_ = true;
        }
    }

    // Releases the images the current recording does not use when the budget is exceeded (called when a recording begins, with
    // everything earlier submitted: the GPU may still be reading them, so the backend keeps them until it has passed that).
    void TrimTextures() {
        if (binding_textures_ || texture_bytes_ <= kTextureBudgetBytes) {  // not while a draw is still collecting its images
            return;
        }
        for (auto it = texture_images_.begin(); it != texture_images_.end();) {
            if (it->second.stamp != recording_serial_) {
                texture_bytes_ -= it->second.bytes;
                backend_->Retire(it->second.image, last_submit_);
                it = texture_images_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Uploads one decoded face into `image` (submitting the recording once when the staging ring is full). False on failure.
    bool UploadFace(rhi::Image& image, u32 layer, u32 width, u32 height, const std::vector<u8>& rgba) {
        EnsureRecording();
        composer_->EndPass(*rec_);
        rhi::Result<void> r = fragment::sampling::UploadRgba8(*rec_, *staging_ring_, image, 0, layer, width, height, rgba);
        if (!r.ok()) {
            FlushRecording();
            EnsureRecording();
            r = fragment::sampling::UploadRgba8(*rec_, *staging_ring_, image, 0, layer, width, height, rgba);
        }
        if (!r.ok()) {
            MH4N_LOG(Error, kLog, "texture upload {}x{}: {}", width, height, r.error());
            return false;
        }
        ++frame_.texture_uploads;
        return true;
    }

    // The image of a texture (one 2D face or the six of a cube), decoded by the cache and uploaded when new or changed.
    // Null when the memory cannot be read or decoded. `shadow`: a Shadow2D texture, an image made with shadow_source (the RGBA8 of
    // the texture's format, read through R32_UINT, is the software renderer's z_ref and density for any format).
    rhi::Image* TextureImage(const std::vector<const texture::TextureRef*>& faces, bool shadow = false) {
        TexImageKey key;
        key.cube = faces.size() == 6;
        key.shadow = shadow;
        const texture::TextureRef& first = *faces[0];
        for (size_t i = 0; i < faces.size(); ++i) {
            const texture::TextureRef& f = *faces[i];
            if (f.width != first.width || f.height != first.height || f.format != first.format) {
                return nullptr;  // a cube whose faces differ
            }
            key.faces[i] = texture::CacheKey{f.paddr, f.format, f.width, f.height};
            // A render target that holds this memory is written back first (until the surface cache, N2-15, samples it directly).
            WriteBackOverlapping(f.paddr, static_cast<u32>(texture::EncodedBytes(f.format, f.width, f.height)), /*stale=*/false);
        }
        std::array<const texture::CachedTexture*, 6> decoded{};
        for (size_t i = 0; i < faces.size(); ++i) {
            decoded[i] = texture_cache_->Get(key.faces[i]);
            if (decoded[i] == nullptr) {
                return nullptr;
            }
        }
        auto it = texture_images_.find(key);
        if (it == texture_images_.end()) {
            rhi::ImageDesc desc;
            desc.format = rhi::Format::Rgba8Unorm;
            desc.width = first.width;
            desc.height = first.height;
            desc.cube = key.cube;
            desc.layers = key.cube ? 6 : 1;
            desc.shadow_source = key.shadow;
            desc.name = key.cube ? "pica cube texture" : (key.shadow ? "pica shadow texture" : "pica texture");
            rhi::Result<std::shared_ptr<rhi::Image>> image = backend_->CreateImage(desc);
            if (!image.ok()) {
                MH4N_LOG(Error, kLog, "texture {}x{}: {}", first.width, first.height, image.error());
                return nullptr;
            }
            TexImage entry;
            entry.image = *image;
            entry.bytes = static_cast<u64>(first.width) * first.height * 4 * faces.size();
            texture_bytes_ += entry.bytes;
            it = texture_images_.emplace(key, std::move(entry)).first;
        }
        TexImage& entry = it->second;
        entry.stamp = recording_serial_;
        for (size_t i = 0; i < faces.size(); ++i) {
            if (entry.generation[i] == decoded[i]->generation) {
                continue;
            }
            if (!UploadFace(*entry.image, static_cast<u32>(i), first.width, first.height, decoded[i]->rgba8)) {
                return nullptr;
            }
            entry.generation[i] = decoded[i]->generation;
            // Uploading may have submitted the recording: the entry stays valid (the map is not touched), but the stamp of this
            // recording is the new one.
            entry.stamp = recording_serial_;
        }
        return entry.image.get();
    }

    // The binding of one unit (design 4.8: a texture and a sampler per unit; vk_rasterizer.cpp's SyncTextureUnits).
    TextureBinding BindUnit(const pica::PicaState& state, u32 unit, const std::vector<texture::TextureRef>& refs) {
        using texture::Type;
        const auto& regs = state.regs;
        const u32 config[3] = {texture::kTexture0, texture::kTexture1, texture::kTexture2};
        const u32 params = regs[config[unit] + 2];
        const Type type = unit == 0 ? static_cast<Type>(pica::BitsOf(params, 28, 3)) : Type::Texture2D;
        const bool shadow_2d = type == Type::Shadow2D;
        const bool shadow_cube = type == Type::ShadowCube;
        const bool cube = type == Type::TextureCube || shadow_cube;
        TextureBinding binding;
        if (type == Type::Disabled) {
            return binding;  // a Disabled unit 0 never reads its texture
        }
        EnsureNullTextures();
        // A shadow unit with nothing to show reads word 0 (depth 0, intensity 0: transparent black whatever the compare), never the
        // composer's white dummy (lit): Shadow2D from a 1x1 shadow source, ShadowCube from the null cube.
        auto null_texture = [&](bool opaque) {
            binding.image = shadow_2d ? null_shadow_.get() : (cube ? null_cube_.get() : (opaque ? null_black_2d_.get() : null_2d_.get()));
            binding.view = shadow_2d ? rhi::ImageView::ShadowStorage : rhi::ImageView::Sampled;
            return binding;
        };
        if (((regs[texture::kMainConfig] >> unit) & 1) == 0) {
            return null_texture(false);  // the unit is off: the software renderer's transparent black
        }
        if (pica::BitsOf(regs[config[unit] + 4], 0, 28) == 0) {
            return null_texture(true);  // an enabled unit with address 0: opaque black
        }
        std::vector<const texture::TextureRef*> faces;
        for (const texture::TextureRef& ref : refs) {
            if (ref.unit == unit) {
                faces.push_back(&ref);
            }
        }
        // A unit 0 type the PICA does not define (6, 7) is sampled as Texture2D (sampling.cpp), but TexturesOfDraw leaves it out:
        // describe the 2D texture from the registers the same way.
        texture::TextureRef undefined_type;
        if (faces.empty() && unit == 0 && (type == static_cast<Type>(6) || type == static_cast<Type>(7))) {
            const u32 size_word = regs[config[0] + 1];
            undefined_type.unit = 0;
            undefined_type.width = pica::BitsOf(size_word, 16, 11);
            undefined_type.height = pica::BitsOf(size_word, 0, 11);
            undefined_type.paddr = pica::BitsOf(regs[config[0] + 4], 0, 28) * 8;
            const u32 format_value = pica::BitsOf(regs[texture::kTexture0Format], 0, 4);
            undefined_type.format = static_cast<texture::Format>(format_value);
            if (texture::IsValidFormat(format_value) && texture::IsValidSize(undefined_type.width, undefined_type.height)) {
                faces.push_back(&undefined_type);
            }
        }
        // The shadow types (N2-12): TexturesOfDraw leaves them out; describe them from the registers, a Shadow2D as one face, a
        // ShadowCube as the six faces TexturesOfDraw gives a TextureCube (face 0 at the unit's address, faces 1 to 5 with the low
        // 22 bits from 0x086 to 0x08A).
        std::array<texture::TextureRef, 6> shadow_faces{};
        if (faces.empty() && unit == 0 && (shadow_2d || shadow_cube)) {
            const u32 size_word = regs[config[0] + 1];
            const u32 address = pica::BitsOf(regs[config[0] + 4], 0, 28);
            const u32 format_value = pica::BitsOf(regs[texture::kTexture0Format], 0, 4);
            const u32 width = pica::BitsOf(size_word, 16, 11), height = pica::BitsOf(size_word, 0, 11);
            if (texture::IsValidFormat(format_value) && texture::IsValidSize(width, height)) {
                for (u32 face = 0; face < (shadow_cube ? 6u : 1u); ++face) {
                    texture::TextureRef& ref = shadow_faces[face];
                    ref.unit = 0;
                    ref.face = face;
                    ref.cube = shadow_cube;
                    ref.width = width;
                    ref.height = height;
                    ref.format = static_cast<texture::Format>(format_value);
                    u32 face_address = address;
                    if (face != 0) {
                        face_address = (address & ~0x3FFFFFu) | pica::BitsOf(regs[texture::kCubeAddress + face - 1], 0, 22);
                    }
                    ref.paddr = face_address * 8;
                    faces.push_back(&ref);
                }
            }
        }
        if (faces.empty() || (cube && faces.size() != 6)) {
            ++frame_.texture_unavailable;
            return null_texture(false);
        }
        rhi::Image* image = TextureImage(faces, shadow_2d);
        if (image == nullptr) {
            ++frame_.texture_unavailable;
            return null_texture(false);
        }
        if (shadow_2d || shadow_cube) {
            // Shadow2D: the image's R32_UINT view (the composer forces it), no sampler (a texel fetch); ShadowCube: the plain RGBA8
            // cube a TextureCube of the same faces uses, its sampled view with the composer's nearest sampler. Level 0 only, as the
            // software renderer reads.
            ++frame_.texture_binds;
            binding.image = image;
            binding.view = shadow_2d ? rhi::ImageView::ShadowStorage : rhi::ImageView::Sampled;
            return binding;
        }
        rhi::Result<std::shared_ptr<rhi::Sampler>> sampler = samplers_->Get(fragment::sampling::SamplerParamsOf(state, unit));
        if (!sampler.ok()) {
            MH4N_LOG(Error, kLog, "sampler: {}", sampler.error());
            ++frame_.texture_unavailable;
            return null_texture(false);
        }
        frame_.sampler_creates += samplers_->creates() - seen_sampler_creates_;
        seen_sampler_creates_ = samplers_->creates();
        ++frame_.texture_binds;
        // Azahar gives a texture lod.max_level + 1 levels from guest memory; this binds level 0 only.
        if (pica::BitsOf(regs[config[unit] + 3], 16, 4) > 0) {
            levels_missing_in_draw_ = true;
        }
        binding.image = image;
        binding.view = rhi::ImageView::Sampled;
        binding.sampler = sampler->get();
        return binding;
    }

    void BindTextures(const pica::PicaState& state, const fragment::FragmentKey& key, DrawDesc& desc) {
        const std::array<bool, 3> used = UnitsRead(key);
        if (!used[0] && !used[1] && !used[2]) {
            return;
        }
        EnsureRecording();
        binding_textures_ = true;
        levels_missing_in_draw_ = false;
        const std::vector<texture::TextureRef> refs = texture::TexturesOfDraw(state.regs.data());
        for (u32 unit = 0; unit < 3; ++unit) {
            if (used[unit]) {
                desc.units[unit] = BindUnit(state, unit, refs);
            }
        }
        binding_textures_ = false;
        if (levels_missing_in_draw_) {
            ++frame_.texture_levels_missing;
        }
    }

    // ---- a draw ----
    rhi::Result<DrawResult> DrawImpl(const pica::DrawCall& draw) {
        const pica::PicaState& state = *draw.state;
        const auto& regs = state.regs;
        ++frame_.draws;

        // The primitive assembler applies the draw's register side effects first, whatever happens to the draw.
        assembler_.ApplyRegisterWrites(state);
        EnsureRoom(0);

        // The surfaces the draw names (their registers), and the viewport, scissor and the scissor uniform from them.
        const u32 color_address = pica::BitsOf(regs[kRegColorAddress], 0, 28) * 8;
        const u32 depth_address = pica::BitsOf(regs[kRegDepthAddress], 0, 28) * 8;
        const u32 size_word = regs[kRegFramebufferSize];
        const u32 width = pica::BitsOf(size_word, 0, 11);
        const u32 height = pica::BitsOf(size_word, 12, 10) + 1;
        const bool flipped = pica::BitsOf(size_word, 24, 1) == 0;
        const u32 color_format = pica::BitsOf(regs[kRegColorFormat], 16, 3);
        const u32 depth_format = pica::BitsOf(regs[kRegDepthFormat], 0, 2);
        const DrawGeometry geometry = MakeGeometry(state, width, height, flipped);

        bool runner_saw = false;
        auto drop = [&](const char* why) -> rhi::Result<DrawResult> {
            ++frame_.dropped;
            if (!runner_saw) {
                runner_.NoteDraw(draw);
                runner_saw = true;
            }
            return rhi::Error{why};
        };

        // The fragment state first, for every draw: the dirty flags the processor clears after this draw are read here.
        rhi::Result<fragment::FragmentState::Prepared> prepared = fragment_state_->Prepare(state, geometry.context, tables_.get());
        if (!prepared.ok()) {
            MH4N_LOG(Error, kLog, "fragment state: {}", prepared.error());
            return drop("the fragment state could not be prepared");
        }
        const fragment::FragmentKey key = fragment_state_->key();
        const fragment::Needs needs = fragment_state_->needs();
        CountStubs(key);

        // A draw in fragment operation mode Shadow (N2-12) draws in the shadow-write pass into the shadow target at the colour
        // buffer's address; it has no depth target (MakeFixedFunctionState makes its output state inert; Azahar's Vulkan renderer
        // uses no depth buffer in shadow mode either).
        const bool shadow_mode = needs.shadow_write_pass;
        rhi::FixedFunctionState ff = fbops::MakeFixedFunctionState(state);
        const bool wants_depth = !shadow_mode && (ff.depth_test || ff.depth_write || ff.stencil_test);
        if (width == 0 || height == 0 || (color_address == 0 && !(depth_address != 0 && wants_depth))) {
            return drop("no surface");
        }

        // ---- the textures the draw reads (before any vertex data is committed: binding one can submit the recording) ----
        DrawDesc desc;
        BindTextures(state, key, desc);

        // ---- the vertices ----
        bool use_cpu = cpuvertex::NeedsCpuPath(draw, assembler_.IsEmpty());
        vertex::LoadedDraw loaded;
        shader::VertexShaderConfig vs_config;
        if (!use_cpu) {
            loaded = loader_->Load(host_, draw);
            frame_.restart_widened += loader_->counters().restart_widened - seen_restart_widened_;
            seen_restart_widened_ = loader_->counters().restart_widened;
            if (loaded.status != vertex::LoadStatus::Ok) {
                return drop("the vertex data could not be loaded");
            }
            shader::ConfigOptions options;
            options.use_clip_planes = true;
            options.geometry_stage = DecideFixup(needs.fixup_gs, VertexStageKind::Translated, backend_->Caps().geometry_shader,
                                                 FixupGeneratorRegistry::Global().Has()) == FixupDecision::Applied;
            vs_config = shader::MakeVertexShaderConfig(state, &loaded, options);
            shader::TranslateResult result = vs_cache_.Get(state.vs, vs_config);
            if (!result.ok()) {
                // Azahar falls back to the software path when AccelerateDrawBatch fails (a program that does not translate).
                ++frame_.translate_failures;
                MH4N_LOG(Warning, kLog, "a vertex program was not translated ({}): CPU path", result.error);
                use_cpu = true;
            } else {
                builder_->AddVertexStage(vs_config.Hash(), result.shader);
            }
        }

        const rhi::Capabilities& caps = backend_->Caps();
        std::vector<rhi::VertexBufferBinding> vertex_buffers;
        u32 count = 0;
        s32 base_vertex = 0;
        const rhi::Buffer* index_buffer = nullptr;
        u64 index_offset = 0;
        rhi::IndexType index_type = rhi::IndexType::U16;
        rhi::Topology topology = rhi::Topology::TriangleList;

        if (use_cpu) {
            const cpuvertex::RunStatus status = runner_.Run(host_, draw, cpuvertex::RunMode::Full);
            runner_saw = true;
            const cpuvertex::Counters& c = runner_.counters();
            frame_.cpu_path_draws += c.draws - seen_cpu_.draws;
            frame_.cpu_path_unhandled += c.unhandled - seen_cpu_.unhandled;
            frame_.cpu_path_load_failed += c.load_failed - seen_cpu_.load_failed;
            seen_cpu_ = c;
            if (status != cpuvertex::RunStatus::Ok) {
                return drop("the CPU path did not run the draw");
            }
            batch_.Clear();
            assembler_.SubmitStream(runner_.output(), [&](const cpuvertex::OutputVertex& a, const cpuvertex::OutputVertex& b,
                                                          const cpuvertex::OutputVertex& v) { batch_.AddTriangle(a, b, v); });
            if (batch_.Empty()) {
                return drop("the CPU path made no triangle");
            }
            EnsureRoom(batch_.VertexCount() * cpuvertex::kHardwareVertexStride);
            rhi::Result<cpuvertex::StreamedBatch> streamed = cpuvertex::UploadBatch(*vertex_ring_, batch_);
            if (!streamed.ok() || streamed->vertex_count == 0) {
                return drop("the triangle batch could not be uploaded");
            }
            vertex_bytes_in_recording_ += batch_.VertexCount() * cpuvertex::kHardwareVertexStride;
            vertex_buffers.push_back(rhi::VertexBufferBinding{vertex_ring_->GetBuffer().get(), streamed->offset});
            count = streamed->vertex_count;
            desc.vertex.kind = VertexStageKind::Trivial;
            desc.vertex.program = 0;
            const rhi::VertexLayout layout = cpuvertex::MakeBatchVertexLayout();
            desc.vertex.input = InputKeyOf(layout);
        } else {
            runner_.NoteDraw(draw);
            runner_saw = true;
            u64 bytes = loaded.index_data.size();
            for (const vertex::VertexBuffer& b : loaded.buffers) {
                bytes += b.data.size() + 16;
            }
            EnsureRoom(bytes);
            for (const vertex::VertexBuffer& b : loaded.buffers) {
                rhi::Result<rhi::RingAllocation> alloc = vertex_ring_->Map(std::max<u64>(b.data.size(), 16), 16);
                if (!alloc.ok()) {
                    return drop("a vertex buffer does not fit the ring");
                }
                std::memcpy(alloc->data.data(), b.data.data(), b.data.size());
                vertex_ring_->GetBuffer()->Flush(alloc->offset, b.data.size());
                if (!vertex_ring_->Commit(std::max<u64>(b.data.size(), 16)).ok()) {
                    return drop("a vertex buffer could not be committed");
                }
                vertex_buffers.push_back(rhi::VertexBufferBinding{vertex_ring_->GetBuffer().get(), alloc->offset});
            }
            if (loaded.index_type != vertex::IndexType::None) {
                rhi::Result<rhi::RingAllocation> alloc = vertex_ring_->Map(std::max<u64>(loaded.index_data.size(), 16), 16);
                if (!alloc.ok()) {
                    return drop("an index buffer does not fit the ring");
                }
                std::memcpy(alloc->data.data(), loaded.index_data.data(), loaded.index_data.size());
                vertex_ring_->GetBuffer()->Flush(alloc->offset, loaded.index_data.size());
                if (!vertex_ring_->Commit(std::max<u64>(loaded.index_data.size(), 16)).ok()) {
                    return drop("an index buffer could not be committed");
                }
                index_buffer = vertex_ring_->GetBuffer().get();
                index_offset = alloc->offset;
                index_type = loaded.index_type == vertex::IndexType::U8    ? rhi::IndexType::U8
                             : loaded.index_type == vertex::IndexType::U32 ? rhi::IndexType::U32
                                                                           : rhi::IndexType::U16;
            }
            vertex_bytes_in_recording_ += bytes;
            count = loaded.count;
            base_vertex = loaded.base_vertex;
            topology = ToRhiTopology(loaded.topology);
            desc.vertex.kind = VertexStageKind::Translated;
            desc.vertex.program = vs_config.Hash();
            desc.vertex.input = MakeVertexInput(loaded);
            if (count == 0) {
                return drop("nothing to draw");
            }
        }

        // ---- the targets ----
        Target* color = nullptr;
        Target* depth = nullptr;
        Target* shadow = nullptr;
        if (shadow_mode) {
            // DrawShadowMapPixel: the colour buffer's address, the size of 0x11E, 4 bytes a texel whatever 0x117 says. Window row y
            // is image row y (the same MakeGeometry as a colour draw), which the write-back puts at guest row h - 1 - y.
            shadow = UseTarget(false, color_address, static_cast<u32>(ColorFormat::RGBA8), width, height, /*shadow=*/true);
            if (shadow == nullptr) {
                ++frame_.shadow_unavailable;
                return drop("the shadow target could not be made");
            }
            shadow->gpu_dirty = true;
        } else {
            color = color_address != 0 ? UseTarget(false, color_address, color_format, width, height) : nullptr;
            depth = depth_address != 0 && wants_depth ? UseTarget(true, depth_address, depth_format, width, height) : nullptr;
            if (color == nullptr && depth == nullptr) {
                return drop("no render target");
            }
        }
        if (color != nullptr) color->gpu_dirty = true;
        if (depth != nullptr) depth->gpu_dirty = true;
        if (depth == nullptr) {
            ff.depth_test = ff.depth_write = ff.stencil_test = false;
        }
        if (color == nullptr) {
            ff.blend_enabled = false;
            ff.logic_op_enabled = false;
        }

        if (geometry.empty) {
            return drop("empty draw rectangle");
        }
        EnsureRecording();

        rhi::Result<fragment::FragmentUniformRing::Allocation> block = uniform_ring_->Write(fragment_state_->block(), prepared->block_changed);
        if (!block.ok()) {
            FlushRecording();
            EnsureRecording();
            block = uniform_ring_->Write(fragment_state_->block(), true);
            if (!block.ok()) {
                return drop("the fragment uniforms do not fit the ring");
            }
        }

        // The vertex-stage uniforms: PicaUniformData then VertexUniformData, one ring allocation.
        rhi::Result<rhi::RingAllocation> vs_alloc = vs_ring_->Map(kVsUniformAllocation, std::max<u64>(caps.uniform_buffer_offset_alignment, 256));
        if (!vs_alloc.ok()) {
            return drop("the vertex uniforms do not fit the ring");
        }
        {
            std::memset(vs_alloc->data.data(), 0, kVsUniformAllocation);
            const shader::PicaUniformData pica_data = shader::MakePicaUniformData(state.vs);
            const shader::VertexUniformData vs_data = shader::MakeVertexUniformData(state, flipped);
            std::memcpy(vs_alloc->data.data(), &pica_data, sizeof(pica_data));
            std::memcpy(vs_alloc->data.data() + 1792, &vs_data, sizeof(vs_data));
            vs_ring_->GetBuffer()->Flush(vs_alloc->offset, kVsUniformAllocation);
            if (!vs_ring_->Commit(kVsUniformAllocation).ok()) {
                return drop("the vertex uniforms could not be committed");
            }
        }

        // ---- the dynamic state ----
        rhi::Result<rhi::state::PipelinePlan> plan = rhi::state::PlanPipeline(ff, caps);
        if (!plan.ok()) {
            return drop("the fixed-function state has no plan");
        }
        if (plan->rgb_uses_blend_rgb) {
            // The colour half of the blend that the plan moves into the shader although it is not a Min or Max half
            // (design: the fragment key holds only the Min/Max halves): not generated. Counted; the pipeline draws without it.
            ++frame_.stubbed_blend_plan;
        }
        fbops::DynamicState dynamic = fbops::MakeDynamicState(state);
        if (!dynamic_applied_ || !(dynamic == last_dynamic_) || plan->constants_from_alpha != last_constants_from_alpha_) {
            fbops::ApplyDynamicState(*rec_, dynamic, plan->constants_from_alpha);
            last_dynamic_ = dynamic;
            last_constants_from_alpha_ = plan->constants_from_alpha;
            dynamic_applied_ = true;
        }

        // ---- the draw ----
        desc.fragment = &key;
        desc.needs = needs;
        desc.state = ff;
        desc.topology = use_cpu ? rhi::Topology::TriangleList : topology;
        desc.color_format = color != nullptr ? rhi::Format::Rgba8Unorm : rhi::Format::Undefined;
        desc.depth_format = depth != nullptr ? DepthImageFormat(depth_format) : rhi::Format::Undefined;
        desc.target.color = color != nullptr ? color->image.get() : nullptr;
        desc.target.depth = depth != nullptr ? depth->image.get() : nullptr;
        desc.target.area = rhi::Rect{0, 0, width, height};
        desc.shadow_target = shadow != nullptr ? shadow->image.get() : nullptr;  // the composer picks the pass and the key
        desc.vs_pica_uniforms = UniformRange{vs_ring_->GetBuffer().get(), vs_alloc->offset, sizeof(shader::PicaUniformData)};
        desc.vs_uniforms = UniformRange{vs_ring_->GetBuffer().get(), vs_alloc->offset + 1792, sizeof(shader::VertexUniformData)};
        desc.fragment_uniforms = *block;
        desc.tables = tables_.get();
        desc.vertex_buffers = vertex_buffers;
        desc.index_buffer = index_buffer;
        desc.index_offset = index_offset;
        desc.index_type = index_type;
        desc.count = count;
        desc.base_vertex = base_vertex;
        desc.viewport = geometry.viewport;
        desc.scissor = geometry.scissor;

        EnsureRecording();
        rhi::Result<DrawResult> result = composer_->Draw(*rec_, desc);
        CountComposer();
        if (!result.ok()) {
            MH4N_LOG(Error, kLog, "draw: {}", result.error());
            ++frame_.dropped;
            return result;
        }
        ++draws_in_recording_;
        if (shadow_mode) ++frame_.shadow_write_draws;
        // N2-12: a shadow lookup is a Shadow2D or ShadowCube unit 0 bound to a decoded map (not the 1x1 word-0 image, not the null
        // cube)
        const auto unit0_type = static_cast<texture::Type>(pica::BitsOf(regs[texture::kTexture0 + 2], 28, 3));
        const rhi::Image* unit0_image = desc.units[0].image;
        const bool shadow_lookup = (unit0_type == texture::Type::Shadow2D || unit0_type == texture::Type::ShadowCube) &&
                                   unit0_image != nullptr && unit0_image != null_shadow_.get() && unit0_image != null_cube_.get();
        if (shadow_lookup) ++frame_.shadow_lookups;
        return result;
    }

    void CountStubs(const fragment::FragmentKey& key) {
        std::array<const char*, 8> names{};
        const size_t n = fragment::StubbedFeatures(key, *registry_, names);
        for (size_t i = 0; i < n; ++i) {
            ++frame_.stubbed[names[i]];
        }
    }

    void CountComposer() {
        // The composer's and the provider's counters since the last draw.
        for (const FrameCounter& c : composer_->TakeFrameCounters()) {
            if (c.name == "stubbed:fixup") frame_.stubbed_fixup += c.value;
            else if (c.name == "dst_copies") frame_.dst_copies += c.value;
            else if (c.name == "fallback_draws") frame_.fallback_draws += c.value;
        }
        for (const FrameCounter& c : provider_->TakeFrameCounters()) {
            if (c.name == "pipeline_sync_builds") frame_.sync_builds += c.value;
            else if (c.name == "pipeline_forced_uber") frame_.forced_uber += c.value;
        }
    }

    static VertexInputKey InputKeyOf(const rhi::VertexLayout& layout) {
        VertexInputKey key;
        key.binding_count = static_cast<u8>(layout.bindings.size());
        for (size_t i = 0; i < layout.bindings.size(); ++i) {
            key.bindings[i].stride = layout.bindings[i].stride;
            key.bindings[i].per_instance = layout.bindings[i].per_instance ? 1 : 0;
        }
        key.attribute_count = static_cast<u8>(layout.attributes.size());
        for (size_t i = 0; i < layout.attributes.size(); ++i) {
            key.attributes[i].location = static_cast<u8>(layout.attributes[i].location);
            key.attributes[i].binding = static_cast<u8>(layout.attributes[i].binding);
            key.attributes[i].format = layout.attributes[i].format;
            key.attributes[i].offset = layout.attributes[i].offset;
        }
        return key;
    }

    Host& host_;
    NativeOptions options_;
    gx::GxEngine gx_;
    pica::CommandProcessor processor_;
    const fragment::FeatureRegistry* registry_;

    // the renderer (made on the first draw, on the thread that calls Execute)
    bool renderer_ready_ = false;
    bool renderer_failed_ = false;
    std::shared_ptr<rhi::Instance> instance_;
    std::shared_ptr<rhi::Backend> backend_;
    fragment::DeviceProfile profile_;
    std::unique_ptr<PipelineBuilder> builder_;
    std::unique_ptr<DefaultPipelineProvider> provider_;
    std::unique_ptr<DrawComposer> composer_;
    std::unique_ptr<fragment::FragmentUniformRing> uniform_ring_;
    std::unique_ptr<fragment::TableRings> tables_;
    std::unique_ptr<rhi::Ring> vertex_ring_, vs_ring_, staging_ring_;
    std::unique_ptr<fragment::FragmentState> fragment_state_;
    std::unique_ptr<vertex::VertexLoader> loader_;
    shader::VertexShaderCache vs_cache_;
    cpuvertex::CpuVertexRunner runner_;
    cpuvertex::PrimitiveAssembler assembler_;
    cpuvertex::TriangleBatch batch_;
    std::vector<std::unique_ptr<Target>> targets_;

    // textures (N2-23): the decoded texture cache of N2-14, the images made from it, the sampler objects
    std::unique_ptr<texture::TextureCache> texture_cache_;
    std::unique_ptr<fragment::sampling::SamplerCache> samplers_;
    std::map<TexImageKey, TexImage> texture_images_;
    u64 texture_bytes_ = 0;
    u64 recording_serial_ = 0;
    rhi::TimelineValue last_submit_ = 0;
    std::shared_ptr<rhi::Image> null_2d_, null_black_2d_, null_cube_;  // transparent black, opaque black, transparent black cube
    std::shared_ptr<rhi::Image> null_shadow_;  // N2-12: a 1x1 shadow source holding word 0
    bool null_cleared_ = false;
    bool binding_textures_ = false;
    bool levels_missing_in_draw_ = false;
    u64 seen_sampler_creates_ = 0;

    // the recording
    std::optional<rhi::Recorder> rec_;
    bool frame_marked_ = false;
    u32 draws_in_recording_ = 0;
    u64 vertex_bytes_in_recording_ = 0;
    bool dynamic_applied_ = false;
    fbops::DynamicState last_dynamic_{};
    bool last_constants_from_alpha_ = false;

    // counters
    FrameCounters frame_;
    u64 seen_restart_widened_ = 0;
    cpuvertex::Counters seen_cpu_{};
};

}  // namespace

std::unique_ptr<NativeGpu> MakeNativeGpu(Host& host, const NativeOptions& options) {
    return std::make_unique<Impl>(host, options);
}

MH4N_OPTION("capture-player", "--force-ubershader", "",
            "--gpu native: every draw takes the ubershader (with the GPU time per frame, the forced-ubershader timing of D5)");

MH4N_REGISTER(GpuFactory, "native", [](Host& host, const Options& options) -> std::unique_ptr<Gpu> {
    NativeOptions native;
    native.force_ubershader = options.Has("--force-ubershader");
    return MakeNativeGpu(host, native);
});

}  // namespace mh4n::gpu::native
