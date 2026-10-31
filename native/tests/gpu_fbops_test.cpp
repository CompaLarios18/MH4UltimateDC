// Derived from Azahar src/video_core/renderer_software/sw_framebuffer.cpp, sw_rasterizer.cpp,
// src/video_core/pica/regs_framebuffer.h, regs_rasterizer.h, regs_internal.h, src/video_core/renderer_vulkan/pica_to_vk.h
// and vk_rasterizer.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp says "Licensed
// under GPLv3 or any later version"; the others say "GPLv2 or any later version"): the expected pixels are the
// software renderer's formulas (the blend factors and equations, the 16 logic ops, the depth and stencil tests and
// the stencil actions, the colour write mask: PixelColor, EvaluateBlendEquation, LogicOp, DoDepthStencilTest,
// PerformStencilAction), the register positions and bit fields come from the register structures, and the expected
// Vulkan state of the register tests restates the rules of pica_to_vk.h and of SyncDrawState. The rest is ours.
// Copyright 2017 Citra Emulator Project (sw_framebuffer.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h, regs_internal.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_to_vk.h, vk_rasterizer.cpp)
//
// Framebuffer operations (N2-13): blending, logic ops, colour write masks, depth and stencil, as rhi.h's
// FixedFunctionState and the dynamic-state calls do them, and the GLSL the shader path embeds.
//
// Every draw is described the way the PICA does (a `Merger`: the output merger's registers), turned into registers,
// into the requested FixedFunctionState (fbops::MakeFixedFunctionState), into the device's pipeline (state::PlanPipeline)
// and drawn on a target through the real backend; the expected pixels are computed by the CPU model below, which is the
// software renderer's code. Blending is float on the GPU and integer (truncating) in the software renderer, so a
// blended channel may differ by 1; everything else (logic ops, masks, depth and stencil outcomes, stencil values) is exact.
//
// Run on whatever Vulkan device the machine has (lavapipe in the cloud and in Linux CI); a case that needs a device skips
// with a message without one, and fails under MH4N_REQUIRE_VULKAN=1. The cases that depend on a capability build their
// own masked backends, so each path runs whatever MH4N_RHI_MASK says; the runs to make are
//   MH4N_REQUIRE_VULKAN=1 ctest --test-dir build/native -R '^gpu_fbops_test$'                       (the plain run)
//   MH4N_RHI_MASK=logic_op                    (the shader's logic ops: MoltenVK's path)
//   MH4N_RHI_MASK=d24s8                       (D32S8: the PC's path)
//   MH4N_RHI_MASK=logic_op,d24s8
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gpu/backend/state/state.h"
#include "gpu/fbops/fixed_function.h"
#include "gpu/fbops/output_stage.h"
#include "gpu/pica/state.h"
#include "support/vulkan_device.h"

#include "gpu_fbops_shaders.inc"

using mh4n::s32;
using mh4n::u16;
using mh4n::u32;
using mh4n::u64;
using mh4n::u8;
namespace test = mh4n::test;
using namespace mh4n::gpu::rhi;
namespace pica = mh4n::gpu::pica;
namespace fbops = mh4n::gpu::fbops;

namespace {

// ---- backend helpers ------------------------------------------------------------------------------------------------

template <size_t N>
ShaderCode Spv(const uint32_t (&words)[N]) {
    ShaderCode code;
    code.spirv = std::span<const u32>(words, N);
    return code;
}

std::shared_ptr<Backend> MakeBackend(std::initializer_list<Cap> masked = {}) {
    BackendOptions options;
    for (Cap cap : masked) options.mask = options.mask.With(cap);
    return test::RequireBackend(options);
}

std::shared_ptr<Image> MakeImage(Backend& backend, Format format, u32 width, u32 height) {
    ImageDesc desc;
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.name = "fbops image";
    Result<std::shared_ptr<Image>> image = backend.CreateImage(desc);
    REQUIRE_MESSAGE(image.ok(), image.error());
    return *image;
}

std::shared_ptr<Buffer> MakeReadbackBuffer(Backend& backend, u64 size) {
    BufferDesc desc;
    desc.size = size;
    desc.heap = Heap::Readback;
    desc.usage = BufferUsage::Transfer;
    desc.name = "fbops readback";
    Result<std::shared_ptr<Buffer>> buffer = backend.CreateBuffer(desc);
    REQUIRE_MESSAGE(buffer.ok(), buffer.error());
    return *buffer;
}

// One aspect of a whole image, tightly packed, through CopyImageToBuffer on a recorder of its own.
std::vector<u8> ReadAspect(Backend& backend, Image& image, u32 width, u32 height, u32 bytes_per_texel, ImageAspect aspect) {
    const u64 size = u64(width) * height * bytes_per_texel;
    std::shared_ptr<Buffer> buffer = MakeReadbackBuffer(backend, size);
    Recorder rec = backend.BeginRecording();
    BufferImageRegion region;
    region.image_rect = Rect{0, 0, width, height};
    region.aspect = aspect;
    rec.CopyImageToBuffer(image, *buffer, std::span<const BufferImageRegion>(&region, 1));
    const TimelineValue value = backend.Submit(std::move(rec));
    std::vector<u8> bytes(size);
    Result<void> read = backend.ReadBuffer(*buffer, 0, bytes, value);
    REQUIRE_MESSAGE(read.ok(), read.error());
    return bytes;
}

u32 Word(const std::vector<u8>& bytes, size_t index, u32 size) {
    u32 word = 0;
    for (u32 i = 0; i < size; ++i) word |= u32(bytes[index * size + i]) << (8 * i);
    return word;
}

u32 FloatBits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

const BindingSetDesc kSampledAndSampler{{{0, BindingType::SampledImage}, {1, BindingType::Sampler}}};

// ---- the PICA's enumerators (regs_framebuffer.h) ----------------------------------------------------------------------

namespace eq {
constexpr u32 Add = 0, Subtract = 1, ReverseSubtract = 2, Min = 3, Max = 4;
}
namespace fac {
constexpr u32 Zero = 0, One = 1, SrcColor = 2, OneMinusSrcColor = 3, DstColor = 4, OneMinusDstColor = 5, SrcAlpha = 6,
              OneMinusSrcAlpha = 7, DstAlpha = 8, OneMinusDstAlpha = 9, ConstColor = 10, OneMinusConstColor = 11,
              ConstAlpha = 12, OneMinusConstAlpha = 13, SrcAlphaSat = 14;
constexpr u32 kCount = 15;
}
namespace cmp {
constexpr u32 Never = 0, Always = 1, Equal = 2, NotEqual = 3, Less = 4, LessEqual = 5, Greater = 6, GreaterEqual = 7;
}
namespace act {
constexpr u32 Keep = 0, Zero = 1, Replace = 2, Increment = 3, Decrement = 4, Invert = 5, IncrementWrap = 6, DecrementWrap = 7;
}
namespace lop {
constexpr u32 Clear = 0, And = 1, AndReverse = 2, Copy = 3, Set = 4, CopyInverted = 5, NoOp = 6, Invert = 7, Nand = 8, Or = 9,
              Nor = 10, Xor = 11, Equiv = 12, AndInverted = 13, OrReverse = 14, OrInverted = 15;
}

// ---- the output merger as a draw describes it, and the registers it makes ---------------------------------------------

struct Merger {
    bool shadow = false;
    bool blend = false;
    u32 eq_rgb = eq::Add, eq_a = eq::Add;
    u32 src_rgb = fac::One, dst_rgb = fac::Zero, src_a = fac::One, dst_a = fac::Zero;
    u32 logic_op = lop::Copy;
    std::array<u8, 4> constant{0, 0, 0, 0};  // the blend_const register: r, g, b, a
    u32 write_mask = 15;                     // the output merger's red/green/blue/alpha enable bits
    bool allow_color = true;                 // the framebuffer's allow_color_write
    bool depth_test = false;
    u32 depth_func = cmp::Always;
    bool depth_write = false;
    bool allow_depth_stencil = true;  // the framebuffer's allow_depth_stencil_write
    u32 depth_format = 3;             // D24S8
    bool stencil = false;
    u32 stencil_func = cmp::Always;
    u32 reference = 0, input_mask = 0xFF, stencil_write_mask = 0xFF;
    u32 stencil_fail = act::Keep, stencil_depth_fail = act::Keep, stencil_depth_pass = act::Keep;
    u32 cull = 0;
    bool flipped = true;  // the framebuffer's flip bit is clear
};

std::unique_ptr<pica::PicaState> StateOf(const Merger& m) {
    auto state = std::make_unique<pica::PicaState>();
    auto& regs = state->regs;
    regs[0x40] = m.cull;
    regs[0x100] = (m.shadow ? 3u : 0u) | (m.blend ? 1u << 8 : 0u);
    regs[0x101] = m.eq_rgb | (m.eq_a << 8) | (m.src_rgb << 16) | (m.dst_rgb << 20) | (m.src_a << 24) | (m.dst_a << 28);
    regs[0x102] = m.logic_op;
    regs[0x103] = u32(m.constant[0]) | (u32(m.constant[1]) << 8) | (u32(m.constant[2]) << 16) | (u32(m.constant[3]) << 24);
    regs[0x105] = (m.stencil ? 1u : 0u) | (m.stencil_func << 4) | (m.stencil_write_mask << 8) | (m.reference << 16) |
                  (m.input_mask << 24);
    regs[0x106] = m.stencil_fail | (m.stencil_depth_fail << 4) | (m.stencil_depth_pass << 8);
    regs[0x107] = (m.depth_test ? 1u : 0u) | (m.depth_func << 4) | (m.write_mask << 8) | (m.depth_write ? 1u << 12 : 0u);
    regs[0x113] = m.allow_color ? 0xFu : 0u;
    regs[0x115] = m.allow_depth_stencil ? 3u : 0u;
    regs[0x116] = m.depth_format;
    regs[0x11E] = m.flipped ? 0u : 1u << 24;
    return state;
}

// ---- the software renderer's formulas (sw_rasterizer.cpp, sw_framebuffer.cpp), on integers ------------------------------

using Rgba = std::array<int, 4>;

int SwFactor(u32 channel, u32 factor, const Rgba& src, const Rgba& dst, const std::array<u8, 4>& cc) {
    switch (factor) {
    case fac::Zero: return 0;
    case fac::One: return 255;
    case fac::SrcColor: return src[channel];
    case fac::OneMinusSrcColor: return 255 - src[channel];
    case fac::DstColor: return dst[channel];
    case fac::OneMinusDstColor: return 255 - dst[channel];
    case fac::SrcAlpha: return src[3];
    case fac::OneMinusSrcAlpha: return 255 - src[3];
    case fac::DstAlpha: return dst[3];
    case fac::OneMinusDstAlpha: return 255 - dst[3];
    case fac::ConstColor: return cc[channel];
    case fac::OneMinusConstColor: return 255 - cc[channel];
    case fac::ConstAlpha: return cc[3];
    case fac::OneMinusConstAlpha: return 255 - cc[3];
    case fac::SrcAlphaSat: return channel == 3 ? 255 : std::min(src[3], 255 - dst[3]);
    }
    return src[channel];
}

int SwEquation(u32 equation, int src_product, int dst_product) {
    int result = 0;
    switch (equation) {
    case eq::Add: result = (src_product + dst_product) / 255; break;
    case eq::Subtract: result = (src_product - dst_product) / 255; break;
    case eq::ReverseSubtract: result = (dst_product - src_product) / 255; break;
    case eq::Min: result = std::min(src_product, dst_product) / 255; break;
    case eq::Max: result = std::max(src_product, dst_product) / 255; break;
    }
    return std::clamp(result, 0, 255);
}

int SwLogicOp(u32 op, int src, int dst) {
    const int s = src & 255, d = dst & 255;
    switch (op) {
    case lop::Clear: return 0;
    case lop::And: return s & d;
    case lop::AndReverse: return s & ~d & 255;
    case lop::Copy: return s;
    case lop::Set: return 255;
    case lop::CopyInverted: return ~s & 255;
    case lop::NoOp: return d;
    case lop::Invert: return ~d & 255;
    case lop::Nand: return ~(s & d) & 255;
    case lop::Or: return s | d;
    case lop::Nor: return ~(s | d) & 255;
    case lop::Xor: return s ^ d;
    case lop::Equiv: return ~(s ^ d) & 255;
    case lop::AndInverted: return ~s & d & 255;
    case lop::OrReverse: return (s | ~d) & 255;
    case lop::OrInverted: return (~s | d) & 255;
    }
    return s;
}

// PixelColor: the colour a fragment leaves in the framebuffer where the depth and stencil tests passed.
Rgba SwPixelColor(const Merger& m, const Rgba& src, const Rgba& dst) {
    Rgba out = src;
    if (m.blend) {
        for (u32 channel = 0; channel < 3; ++channel) {
            out[channel] = SwEquation(m.eq_rgb, src[channel] * SwFactor(channel, m.src_rgb, src, dst, m.constant),
                                      dst[channel] * SwFactor(channel, m.dst_rgb, src, dst, m.constant));
        }
        out[3] = SwEquation(m.eq_a, src[3] * SwFactor(3, m.src_a, src, dst, m.constant),
                            dst[3] * SwFactor(3, m.dst_a, src, dst, m.constant));
    } else {
        for (u32 channel = 0; channel < 4; ++channel) out[channel] = SwLogicOp(m.logic_op, src[channel], dst[channel]);
    }
    Rgba result;
    for (u32 channel = 0; channel < 4; ++channel) result[channel] = ((m.write_mask >> channel) & 1) ? out[channel] : dst[channel];
    return result;
}

bool SwCompare(u32 func, u32 left, u32 right) {
    switch (func) {
    case cmp::Never: return false;
    case cmp::Always: return true;
    case cmp::Equal: return left == right;
    case cmp::NotEqual: return left != right;
    case cmp::Less: return left < right;
    case cmp::LessEqual: return left <= right;
    case cmp::Greater: return left > right;
    case cmp::GreaterEqual: return left >= right;
    }
    return false;
}

u8 SwStencilAction(u32 action, u8 old_stencil, u8 ref) {
    switch (action) {
    case act::Keep: return old_stencil;
    case act::Zero: return 0;
    case act::Replace: return ref;
    case act::Increment: return std::min<u8>(old_stencil, 254) + 1;
    case act::Decrement: return std::max<u8>(old_stencil, 1) - 1;
    case act::Invert: return static_cast<u8>(~old_stencil);
    case act::IncrementWrap: return static_cast<u8>(old_stencil + 1);
    case act::DecrementWrap: return static_cast<u8>(old_stencil - 1);
    }
    return 0;
}

struct Pixel {
    Rgba color{0, 0, 0, 0};
    u32 depth = 0;    // 24 bits
    u32 stencil = 0;  // 8 bits
};

// DoDepthStencilTest and the colour write after it, for one fragment of the draw at the PICA depth
// z_over_w * scale + offset (WriteDepth), clamped as the software renderer clamps it.
void SwFragment(const Merger& m, const Rgba& src, float z_over_w, float scale, float offset, Pixel& px) {
    float depth = z_over_w * scale + offset;
    depth = std::clamp(depth, 0.0f, 1.0f);
    const u32 z = static_cast<u32>(depth * 0xFFFFFF);
    const u8 old_stencil = static_cast<u8>(px.stencil);
    const bool stencil_on = m.stencil && m.depth_format == 3;
    const auto update_stencil = [&](u32 action) {
        const u8 new_stencil = SwStencilAction(action, old_stencil, static_cast<u8>(m.reference));
        if (m.allow_depth_stencil) {
            px.stencil = (new_stencil & m.stencil_write_mask) | (old_stencil & ~m.stencil_write_mask & 0xFF);
        }
    };
    if (stencil_on) {
        const u8 dest = old_stencil & m.input_mask;
        const u8 ref = m.reference & m.input_mask;
        if (!SwCompare(m.stencil_func, ref, dest)) {
            update_stencil(m.stencil_fail);
            return;
        }
    }
    if (m.depth_test && !SwCompare(m.depth_func, z, px.depth)) {
        if (stencil_on) update_stencil(m.stencil_depth_fail);
        return;
    }
    if (m.allow_depth_stencil && m.depth_write) px.depth = z;
    if (stencil_on) update_stencil(m.stencil_depth_pass);
    const Rgba result = SwPixelColor(m, src, px.color);
    if (m.allow_color) px.color = result;
}

// ---- drawing a case on the backend, and the same case in the model -------------------------------------------------------

constexpr u32 kGrid = 32;  // targets are 32 x 32; a case owns one pixel

struct DrawCase {
    Merger m;
    Rgba src{0, 0, 0, 255};  // the fragment colour (a byte per channel)
    float z = -0.5f;         // the PICA's clip z over w of the draw
    float scale = 0.5f, offset = 0.75f;  // viewport_depth_range and viewport_depth_near_plane
    u32 x = 0, y = 0;
    std::string label;
};

DrawCase At(DrawCase c, u32 index) {
    c.x = index % kGrid;
    c.y = index / kGrid;
    return c;
}

std::string KeyOf(const GraphicsPipelineDesc& desc, int kind) {
    const FixedFunctionState& s = desc.state;
    std::ostringstream key;
    key << kind << '|' << s.blend_enabled << ',' << int(s.src_color) << ',' << int(s.dst_color) << ',' << int(s.color_op) << ','
        << int(s.src_alpha) << ',' << int(s.dst_alpha) << ',' << int(s.alpha_op) << ',' << int(s.color_write_mask) << '|'
        << s.logic_op_enabled << ',' << int(s.logic_op) << '|' << s.depth_test << s.depth_write << int(s.depth_compare) << '|'
        << s.stencil_test << int(s.stencil_front.fail) << int(s.stencil_front.pass) << int(s.stencil_front.depth_fail)
        << int(s.stencil_front.compare) << '|' << int(s.cull) << int(s.front_face) << '|';
    for (const SpecConstant& c : desc.specialization) key << c.id << '=' << c.value << ';';
    return key.str();
}

struct Harness {
    std::shared_ptr<Backend> backend;
    bool with_depth = false;
    std::shared_ptr<Image> color;
    std::shared_ptr<Image> depth;
    std::shared_ptr<Sampler> sampler;
    std::unique_ptr<BindingArena> arena;
    std::map<std::string, std::shared_ptr<Pipeline>> pipelines;
    std::vector<Pixel> model = std::vector<Pixel>(kGrid * kGrid);
    std::vector<state::PipelinePlan> plans;  // the plan of every draw, in order

    Harness(std::shared_ptr<Backend> b, bool depth_target) : backend(std::move(b)), with_depth(depth_target) {
        color = MakeImage(*backend, Format::Rgba8Unorm, kGrid, kGrid);
        if (with_depth) depth = MakeImage(*backend, Format::D24S8, kGrid, kGrid);
        SamplerDesc sd;
        sd.min_filter = sd.mag_filter = sd.mip_filter = Filter::Nearest;
        Result<std::shared_ptr<Sampler>> made = backend->CreateSampler(sd);
        REQUIRE_MESSAGE(made.ok(), made.error());
        sampler = *made;
        arena = std::make_unique<BindingArena>(backend);
    }

    // Starts a recorder with a pass that clears the target (colour as bytes, depth 0..1, stencil) and the model alike.
    Recorder Begin(const Rgba& clear_color, float clear_depth, u32 clear_stencil) {
        Pixel clear;
        clear.color = clear_color;
        clear.depth = static_cast<u32>(clear_depth * 0xFFFFFF);
        clear.stencil = clear_stencil;
        std::fill(model.begin(), model.end(), clear);
        plans.clear();
        TargetPassDesc pass;
        pass.color = color.get();
        pass.depth = with_depth ? depth.get() : nullptr;
        pass.color_load = LoadOp::Clear;
        pass.depth_load = with_depth ? LoadOp::Clear : LoadOp::Load;
        for (u32 i = 0; i < 4; ++i) pass.clear_color[i] = float(clear_color[i]) / 255.0f;
        pass.clear_depth = clear_depth;
        pass.clear_stencil = clear_stencil;
        pass.area = Rect{0, 0, kGrid, kGrid};
        Recorder rec = backend->BeginRecording();
        rec.BeginTargetPass(pass);
        return rec;
    }

    // Draws the case into its pixel (the scissor) and runs it through the model.
    void Draw(Recorder& rec, const DrawCase& c, BindingSet** set) {
        const std::unique_ptr<pica::PicaState> registers = StateOf(c.m);
        const FixedFunctionState requested = fbops::MakeFixedFunctionState(*registers);
        Result<state::PipelinePlan> planned = state::PlanPipeline(requested, backend->Caps());
        REQUIRE_MESSAGE(planned.ok(), c.label << ": " << planned.error());
        const state::PipelinePlan& plan = *planned;
        plans.push_back(plan);

        GraphicsPipelineDesc desc;
        desc.vertex = Spv(spv_fs_vert);
        desc.color_format = color->GetFormat();
        desc.depth_format = with_depth ? depth->GetFormat() : Format::Undefined;
        desc.state = plan.built;
        desc.name = "fbops";
        std::vector<SpecConstant>& spec = desc.specialization;
        spec = {{0, FloatBits(c.z)}, {5, FloatBits(c.scale)}, {6, FloatBits(c.offset)}};
        for (u32 i = 0; i < 4; ++i) spec.push_back({1 + i, FloatBits(float(c.src[i]) / 255.0f)});
        int kind = 0;
        bool needs_destination = false;
        if (plan.rgb_in_shader || plan.alpha_in_shader) {
            kind = 1;
            needs_destination = true;
            desc.fragment = Spv(spv_minmax_frag);
            // the generator's composition (minmax.frag): mh4n_minmax_blend with the requested equation for a Min or Max
            // half the plan moved and 0 for the others, then mh4n_blend_rgb from the original source for the colour half
            // the plan sends here although it is not Min or Max
            const bool rgb_minmax = plan.rgb_in_shader && (c.m.eq_rgb == eq::Min || c.m.eq_rgb == eq::Max);
            spec.push_back({7, rgb_minmax ? c.m.eq_rgb : 0});
            spec.push_back({8, c.m.src_rgb});
            spec.push_back({9, c.m.dst_rgb});
            spec.push_back({10, plan.alpha_in_shader ? c.m.eq_a : 0});
            spec.push_back({11, c.m.src_a});
            spec.push_back({12, c.m.dst_a});
            for (u32 i = 0; i < 4; ++i) spec.push_back({13 + i, FloatBits(float(c.m.constant[i]) / 255.0f)});
            spec.push_back({17, c.m.eq_rgb});
            spec.push_back({18, plan.rgb_uses_blend_rgb ? 1u : 0u});
        } else if (plan.shader_logic_op) {
            kind = plan.needs_destination_copy ? 2 : 3;
            needs_destination = plan.needs_destination_copy;
            desc.fragment = needs_destination ? Spv(spv_logic_frag) : Spv(spv_logic_nodst_frag);
            spec.push_back({7, c.m.logic_op});
        } else {
            desc.fragment = Spv(spv_draw_frag);
        }
        if (needs_destination) desc.sets = {kSampledAndSampler};

        std::shared_ptr<Pipeline>& pipeline = pipelines[KeyOf(desc, kind)];
        if (pipeline == nullptr) {
            Result<std::shared_ptr<Pipeline>> made = backend->CreateGraphicsPipeline(desc);
            REQUIRE_MESSAGE(made.ok(), c.label << ": " << made.error());
            pipeline = *made;
        }

        fbops::ApplyDynamicState(rec, fbops::MakeDynamicState(*registers), plan.constants_from_alpha);
        rec.SetPipeline(*pipeline);
        if (needs_destination) {
            // the destination under the pixel is the target as it is now: a fresh copy for every draw that reads it
            Image& twin = rec.FeedbackCopy(*color);
            if (*set == nullptr) {
                Result<BindingSet*> acquired = arena->Acquire(kSampledAndSampler);
                REQUIRE_MESSAGE(acquired.ok(), acquired.error());
                REQUIRE((*acquired)->SetSampledImage(0, twin, ImageView::Sampled).ok());
                REQUIRE((*acquired)->SetSampler(1, *sampler).ok());
                *set = *acquired;
            }
            rec.Bind(**set, 0);
        }
        rec.SetScissor(Rect{static_cast<s32>(c.x), static_cast<s32>(c.y), 1, 1});
        rec.Draw(3);

        SwFragment(c.m, c.src, c.z, c.scale, c.offset, model[c.y * kGrid + c.x]);
    }

    // Draws the generator's blend composition (minmax.frag) with the given constants, blending off, at a pixel; the
    // destination is the target as it is.
    void DrawShader(Recorder& rec, const Rgba& src, const std::vector<SpecConstant>& extra, u32 x, u32 y, BindingSet** set) {
        GraphicsPipelineDesc desc;
        desc.vertex = Spv(spv_fs_vert);
        desc.fragment = Spv(spv_minmax_frag);
        desc.color_format = color->GetFormat();
        desc.depth_format = with_depth ? depth->GetFormat() : Format::Undefined;
        desc.sets = {kSampledAndSampler};
        desc.name = "fbops raw";
        desc.specialization = {{0, FloatBits(-0.5f)}};
        for (u32 i = 0; i < 4; ++i) desc.specialization.push_back({1 + i, FloatBits(float(src[i]) / 255.0f)});
        desc.specialization.insert(desc.specialization.end(), extra.begin(), extra.end());
        Result<std::shared_ptr<Pipeline>> made = backend->CreateGraphicsPipeline(desc);
        REQUIRE_MESSAGE(made.ok(), made.error());
        raw_pipelines.push_back(*made);
        fbops::ApplyDynamicState(rec, fbops::DynamicState{});
        rec.SetPipeline(**made);
        Image& twin = rec.FeedbackCopy(*color);
        if (*set == nullptr) {
            Result<BindingSet*> acquired = arena->Acquire(kSampledAndSampler);
            REQUIRE_MESSAGE(acquired.ok(), acquired.error());
            REQUIRE((*acquired)->SetSampledImage(0, twin, ImageView::Sampled).ok());
            REQUIRE((*acquired)->SetSampler(1, *sampler).ok());
            *set = *acquired;
        }
        rec.Bind(**set, 0);
        rec.SetScissor(Rect{static_cast<s32>(x), static_cast<s32>(y), 1, 1});
        rec.Draw(3);
    }
    std::vector<std::shared_ptr<Pipeline>> raw_pipelines;

    struct Result3 {
        std::vector<u8> color, depth, stencil;
    };

    Result3 Finish(Recorder&& rec) {
        backend->WaitIdle(backend->Submit(std::move(rec)));
        Result3 out;
        out.color = ReadAspect(*backend, *color, kGrid, kGrid, 4, ImageAspect::Color);
        if (with_depth) {
            out.depth = ReadAspect(*backend, *depth, kGrid, kGrid, 4, ImageAspect::Depth);
            out.stencil = ReadAspect(*backend, *depth, kGrid, kGrid, 1, ImageAspect::Stencil);
        }
        return out;
    }

    // The 24-bit depth the target holds at a pixel.
    u32 DepthAt(const Result3& r, u32 index) const {
        const u32 word = Word(r.depth, index, 4);
        if (depth->GetFormat() == Format::D24S8) return word & 0xFFFFFFu;
        float f;
        std::memcpy(&f, &word, sizeof(f));
        return static_cast<u32>(std::lround(f * 0xFFFFFF));
    }
};

// Runs the draws, in order, on a target cleared to the given values, and compares every pixel a case touched with the model
// (colour within `color_tolerance`, depth within 1: the software renderer truncates and the device rounds; stencil exactly).
// `check_depth` and `check_stencil` say which planes the case is about.
void RunCases(Harness& h, const std::vector<DrawCase>& cases, const Rgba& clear_color, float clear_depth, u32 clear_stencil,
              int color_tolerance, bool check_color = true, bool check_depth = false, bool check_stencil = false) {
    REQUIRE(cases.size() <= kGrid * kGrid);
    Recorder rec = h.Begin(clear_color, clear_depth, clear_stencil);
    BindingSet* set = nullptr;
    std::set<u32> touched;
    for (const DrawCase& c : cases) {
        h.Draw(rec, c, &set);
        touched.insert(c.y * kGrid + c.x);
    }
    const Harness::Result3 got = h.Finish(std::move(rec));
    for (const u32 index : touched) {
        const Pixel& want = h.model[index];
        INFO("pixel " << index << " (" << index % kGrid << "," << index / kGrid << ")");
        if (check_color) {
            for (u32 channel = 0; channel < 4; ++channel) {
                const int have = got.color[index * 4 + channel];
                INFO("channel " << channel << " got " << have << " want " << want.color[channel]);
                CHECK(std::abs(have - want.color[channel]) <= color_tolerance);
            }
        }
        if (check_depth) {
            const u32 have = h.DepthAt(got, index);
            INFO("depth got " << have << " want " << want.depth);
            // the model truncates to 24 bits and the device rounds: the model's value or the next
            CHECK((have == want.depth || have == want.depth + 1));
        }
        if (check_stencil) {
            INFO("stencil got " << int(got.stencil[index]) << " want " << want.stencil);
            CHECK(got.stencil[index] == want.stencil);
        }
    }
    // the cases that were drawn name their own label on failure
    if (!cases.empty()) CHECK_MESSAGE(h.plans.size() == cases.size(), "one plan per draw");
}

}  // namespace

// ================================================================================================================
// The requested form: registers to FixedFunctionState, no device
// ================================================================================================================

TEST_CASE("registers/blend_equations_factors_logic_ops_and_masks_map_as_azahar_does") {
    // pica_to_vk.h's tables, in the PICA's order, written out again here
    const BlendOp ops[5] = {BlendOp::Add, BlendOp::Subtract, BlendOp::ReverseSubtract, BlendOp::Min, BlendOp::Max};
    const BlendFactor factors[15] = {
        BlendFactor::Zero, BlendFactor::One, BlendFactor::SrcColor, BlendFactor::OneMinusSrcColor, BlendFactor::DstColor,
        BlendFactor::OneMinusDstColor, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendFactor::DstAlpha,
        BlendFactor::OneMinusDstAlpha, BlendFactor::ConstantColor, BlendFactor::OneMinusConstantColor,
        BlendFactor::ConstantAlpha, BlendFactor::OneMinusConstantAlpha, BlendFactor::SrcAlphaSaturate};
    for (u32 e = 0; e < 5; ++e) {
        for (u32 f = 0; f < 15; ++f) {
            Merger m;
            m.blend = true;
            m.eq_rgb = e;
            m.eq_a = 4 - e;
            m.src_rgb = f;
            m.dst_rgb = 14 - f;
            m.src_a = (f + 3) % 15;
            m.dst_a = (f + 7) % 15;
            const FixedFunctionState s = fbops::MakeFixedFunctionState(*StateOf(m));
            CHECK(s.blend_enabled);
            CHECK(s.color_op == ops[e]);
            CHECK(s.alpha_op == ops[4 - e]);
            CHECK(s.src_color == factors[f]);
            CHECK(s.dst_color == factors[14 - f]);
            CHECK(s.src_alpha == factors[(f + 3) % 15]);
            CHECK(s.dst_alpha == factors[(f + 7) % 15]);
            CHECK_FALSE(s.logic_op_enabled);
        }
    }
    // blending off: the equations and factors are the defaults whatever the registers hold, the logic op is requested
    // unless it is Copy
    const LogicOp logic[16] = {LogicOp::Clear, LogicOp::And, LogicOp::AndReverse, LogicOp::Copy, LogicOp::Set,
                               LogicOp::CopyInverted, LogicOp::NoOp, LogicOp::Invert, LogicOp::Nand, LogicOp::Or,
                               LogicOp::Nor, LogicOp::Xor, LogicOp::Equiv, LogicOp::AndInverted, LogicOp::OrReverse,
                               LogicOp::OrInverted};
    for (u32 op = 0; op < 16; ++op) {
        Merger m;
        m.eq_rgb = eq::Max;
        m.src_rgb = fac::DstColor;
        m.logic_op = op;
        const FixedFunctionState s = fbops::MakeFixedFunctionState(*StateOf(m));
        CHECK_FALSE(s.blend_enabled);
        CHECK(s.color_op == BlendOp::Add);
        CHECK(s.src_color == BlendFactor::One);
        CHECK(s.logic_op_enabled == (op != lop::Copy));
        CHECK(s.logic_op == (op != lop::Copy ? logic[op] : FixedFunctionState{}.logic_op));
        // the same op with blending on is not requested
        m.blend = true;
        CHECK_FALSE(fbops::MakeFixedFunctionState(*StateOf(m)).logic_op_enabled);
    }
    // the colour write mask: the four bits (r = 1, g = 2, b = 4, a = 8), 0 when the framebuffer's colour write is off
    for (u32 mask = 0; mask < 16; ++mask) {
        Merger m;
        m.write_mask = mask;
        CHECK(fbops::MakeFixedFunctionState(*StateOf(m)).color_write_mask == mask);
        m.allow_color = false;
        CHECK(fbops::MakeFixedFunctionState(*StateOf(m)).color_write_mask == 0);
    }
    // an out-of-range equation is Add and an out-of-range factor One (pica_to_vk.h)
    Merger m;
    m.blend = true;
    m.eq_rgb = 7;
    m.src_rgb = 15;
    const FixedFunctionState s = fbops::MakeFixedFunctionState(*StateOf(m));
    CHECK(s.color_op == BlendOp::Add);
    CHECK(s.src_color == BlendFactor::One);
}

TEST_CASE("registers/depth_stencil_cull_and_the_dynamic_state_come_from_the_registers") {
    const CompareOp compare[8] = {CompareOp::Never, CompareOp::Always, CompareOp::Equal, CompareOp::NotEqual,
                                  CompareOp::Less, CompareOp::LessEqual, CompareOp::Greater, CompareOp::GreaterEqual};
    const StencilOp actions[8] = {StencilOp::Keep, StencilOp::Zero, StencilOp::Replace, StencilOp::IncrementClamp,
                                  StencilOp::DecrementClamp, StencilOp::Invert, StencilOp::IncrementWrap,
                                  StencilOp::DecrementWrap};
    for (u32 f = 0; f < 8; ++f) {
        Merger m;
        m.depth_test = true;
        m.depth_func = f;
        m.depth_write = true;
        FixedFunctionState s = fbops::MakeFixedFunctionState(*StateOf(m));
        CHECK(s.depth_test);
        CHECK(s.depth_compare == compare[f]);
        CHECK(s.depth_write);
        m = Merger{};
        m.stencil = true;
        m.stencil_func = f;
        m.stencil_fail = f;
        m.stencil_depth_fail = (f + 1) % 8;
        m.stencil_depth_pass = (f + 2) % 8;
        s = fbops::MakeFixedFunctionState(*StateOf(m));
        CHECK(s.stencil_test);
        for (const StencilFaceState& face : {s.stencil_front, s.stencil_back}) {
            CHECK(face.compare == compare[f]);
            CHECK(face.fail == actions[f]);
            CHECK(face.depth_fail == actions[(f + 1) % 8]);
            CHECK(face.pass == actions[(f + 2) % 8]);
        }
    }
    // depth: the write alone turns the test on with Always (Vulkan writes depth only with the test on); neither leaves
    // the defaults; the framebuffer's depth/stencil write gates the write
    Merger write_only;
    write_only.depth_write = true;
    write_only.depth_func = cmp::Greater;  // not used: the test is off
    FixedFunctionState s = fbops::MakeFixedFunctionState(*StateOf(write_only));
    CHECK(s.depth_test);
    CHECK(s.depth_compare == CompareOp::Always);
    CHECK(s.depth_write);
    write_only.allow_depth_stencil = false;
    s = fbops::MakeFixedFunctionState(*StateOf(write_only));
    CHECK(s.depth_test);
    CHECK_FALSE(s.depth_write);
    s = fbops::MakeFixedFunctionState(*StateOf(Merger{}));
    CHECK_FALSE(s.depth_test);
    CHECK_FALSE(s.depth_write);
    CHECK(s.depth_compare == FixedFunctionState{}.depth_compare);
    // stencil: only with a D24S8 depth format (3); D16 (0) and D24 (2) have none
    for (const u32 format : {0u, 2u, 3u}) {
        Merger m;
        m.stencil = true;
        m.depth_format = format;
        CHECK(fbops::MakeFixedFunctionState(*StateOf(m)).stencil_test == (format == 3));
    }
    // cull and front face: the keep-one-winding modes cull the back (the front when the framebuffer is flipped is
    // Azahar's flip_viewport: the flip bit CLEAR), keep-counter-clockwise swaps the front face
    struct CullCase {
        u32 mode;
        bool flipped;
        CullMode cull;
        FrontFace face;
    };
    const CullCase cull_cases[] = {
        {0, true, CullMode::None, FrontFace::CounterClockwise},  {3, false, CullMode::None, FrontFace::CounterClockwise},
        {1, true, CullMode::Front, FrontFace::CounterClockwise}, {1, false, CullMode::Back, FrontFace::CounterClockwise},
        {2, true, CullMode::Front, FrontFace::Clockwise},        {2, false, CullMode::Back, FrontFace::Clockwise},
    };
    for (const CullCase& c : cull_cases) {
        Merger m;
        m.cull = c.mode;
        m.flipped = c.flipped;
        const FixedFunctionState out = fbops::MakeFixedFunctionState(*StateOf(m));
        INFO("cull mode " << c.mode << " flipped " << c.flipped);
        CHECK(out.cull == c.cull);
        CHECK(out.front_face == c.face);
        CHECK_FALSE(out.depth_bias_enabled);
    }
    // the dynamic state: the constant as 0..1 floats, the stencil reference and compare mask, the write mask unless the
    // framebuffer's depth/stencil write is off
    Merger d;
    d.constant = {255, 0, 51, 102};
    d.reference = 0xA5;
    d.input_mask = 0x3C;
    d.stencil_write_mask = 0x0F;
    fbops::DynamicState dyn = fbops::MakeDynamicState(*StateOf(d));
    CHECK(dyn.blend_constants == std::array<float, 4>{1.0f, 0.0f, 51.0f / 255.0f, 102.0f / 255.0f});
    CHECK(dyn.stencil_reference == 0xA5);
    CHECK(dyn.stencil_compare_mask == 0x3C);
    CHECK(dyn.stencil_write_mask == 0x0F);
    d.allow_depth_stencil = false;
    dyn = fbops::MakeDynamicState(*StateOf(d));
    CHECK(dyn.stencil_write_mask == 0);
    CHECK(dyn.stencil_reference == 0xA5);
}

TEST_CASE("registers/shadow_mode_is_inert_and_keeps_only_the_cull_state") {
    Merger m;
    m.shadow = true;
    m.blend = true;
    m.depth_test = m.depth_write = true;
    m.stencil = true;
    m.cull = 2;
    const FixedFunctionState s = fbops::MakeFixedFunctionState(*StateOf(m));
    CHECK_FALSE(s.blend_enabled);
    CHECK_FALSE(s.logic_op_enabled);
    CHECK_FALSE(s.depth_test);
    CHECK_FALSE(s.depth_write);
    CHECK_FALSE(s.stencil_test);
    CHECK(s.cull == CullMode::Front);
    CHECK(s.front_face == FrontFace::Clockwise);
}

// ================================================================================================================
// The device profile: PlanPipeline on synthetic capabilities, no device
// ================================================================================================================

namespace {

Capabilities CapsWith(bool logic_op) {
    Capabilities caps;
    caps.logic_op = logic_op;
    return caps;
}

FixedFunctionState Requested(const Merger& m) {
    return fbops::MakeFixedFunctionState(*StateOf(m));
}

}  // namespace

TEST_CASE("plan/a_logic_op_is_the_hardwares_with_the_bit_and_the_shaders_without") {
    const std::set<u32> needs_destination = {lop::And, lop::AndReverse, lop::Invert, lop::Nand, lop::Or, lop::Nor,
                                              lop::Xor, lop::Equiv, lop::AndInverted, lop::OrReverse, lop::OrInverted};
    CHECK(needs_destination.size() == 11);
    for (u32 op = 0; op < 16; ++op) {
        Merger m;
        m.logic_op = op;
        m.write_mask = 0xB;
        const FixedFunctionState requested = Requested(m);
        INFO("op " << op);
        CHECK(state::LogicOpNeedsDestination(requested.logic_op_enabled ? requested.logic_op : LogicOp::Copy) ==
              (needs_destination.count(op) != 0));

        Result<state::PipelinePlan> hardware = state::PlanPipeline(requested, CapsWith(true));
        REQUIRE_MESSAGE(hardware.ok(), hardware.error());
        CHECK_FALSE(hardware->shader_logic_op);
        CHECK_FALSE(hardware->needs_destination_copy);
        CHECK(hardware->built.logic_op_enabled == (op != lop::Copy));
        CHECK(hardware->built.color_write_mask == 0xB);

        Result<state::PipelinePlan> emulated = state::PlanPipeline(requested, CapsWith(false));
        REQUIRE_MESSAGE(emulated.ok(), emulated.error());
        CHECK_FALSE(emulated->built.logic_op_enabled);
        CHECK_FALSE(emulated->built.blend_enabled);
        const bool in_shader = op != lop::Copy && op != lop::NoOp;
        CHECK(emulated->shader_logic_op == in_shader);
        CHECK(emulated->needs_destination_copy == (needs_destination.count(op) != 0));
        // NoOp is the colour write mask 0 (the depth write stays); every other op keeps the requested mask
        CHECK(emulated->built.color_write_mask == (op == lop::NoOp ? 0 : 0xB));
    }
    // with blending on the logic op is not requested, and a request for both is refused
    Merger blend;
    blend.blend = true;
    blend.logic_op = lop::Xor;
    Result<state::PipelinePlan> plan = state::PlanPipeline(Requested(blend), CapsWith(false));
    REQUIRE(plan.ok());
    CHECK_FALSE(plan->shader_logic_op);
    FixedFunctionState both = Requested(blend);
    both.logic_op_enabled = true;
    both.logic_op = LogicOp::Xor;
    CHECK_FALSE(state::PlanPipeline(both, CapsWith(true)).ok());
}

TEST_CASE("plan/a_min_or_max_half_goes_to_the_shader_unless_both_factors_are_one") {
    const std::vector<std::pair<u32, u32>> factor_pairs = {{fac::One, fac::One},   {fac::One, fac::Zero},
                                                           {fac::Zero, fac::One},  {fac::SrcAlpha, fac::One},
                                                           {fac::One, fac::DstColor}, {fac::ConstColor, fac::OneMinusConstAlpha}};
    for (const u32 equation : {eq::Min, eq::Max}) {
        for (const auto& [src, dst] : factor_pairs) {
            Merger m;
            m.blend = true;
            m.eq_rgb = m.eq_a = equation;
            m.src_rgb = src;
            m.dst_rgb = dst;
            m.src_a = fac::One;
            m.dst_a = fac::One;
            INFO("equation " << equation << " factors " << src << "," << dst);
            Result<state::PipelinePlan> plan = state::PlanPipeline(Requested(m), CapsWith(true));
            REQUIRE_MESSAGE(plan.ok(), plan.error());
            const bool shader = !(src == fac::One && dst == fac::One);
            CHECK(plan->rgb_in_shader == shader);
            CHECK_FALSE(plan->rgb_uses_blend_rgb);  // a Min or Max colour half is mh4n_minmax_blend's
            CHECK_FALSE(plan->alpha_in_shader);  // its factors are both One: the hardware's
            CHECK(plan->needs_destination_copy == shader);
            if (shader) {
                CHECK(plan->built.color_op == BlendOp::Add);
                CHECK(plan->built.src_color == BlendFactor::One);
                CHECK(plan->built.dst_color == BlendFactor::Zero);
            } else {
                CHECK(plan->built.color_op == (equation == eq::Min ? BlendOp::Min : BlendOp::Max));
            }
            CHECK(plan->built.alpha_op == (equation == eq::Min ? BlendOp::Min : BlendOp::Max));
            // the other equations never go to the shader
            Merger add = m;
            add.eq_rgb = add.eq_a = eq::Add;
            plan = state::PlanPipeline(Requested(add), CapsWith(true));
            if (src == fac::ConstColor && dst == fac::OneMinusConstAlpha) {
                REQUIRE(plan.ok());  // one of each constant pair in the colour half: the shader's (the next case)
                CHECK(plan->rgb_in_shader);
                continue;
            }
            REQUIRE_MESSAGE(plan.ok(), plan.error());
            CHECK_FALSE(plan->rgb_in_shader);
            CHECK_FALSE(plan->needs_destination_copy);
            CHECK(plan->built.src_color == Requested(add).src_color);
        }
    }
    // the alpha half alone
    Merger m;
    Result<state::PipelinePlan> plan = Error{"unset"};
    m.blend = true;
    m.eq_a = eq::Max;
    m.src_a = fac::SrcAlpha;
    m.dst_a = fac::One;
    plan = state::PlanPipeline(Requested(m), CapsWith(true));
    REQUIRE(plan.ok());
    CHECK_FALSE(plan->rgb_in_shader);
    CHECK(plan->alpha_in_shader);
    CHECK(plan->needs_destination_copy);
    CHECK(plan->built.src_alpha == BlendFactor::One);
    CHECK(plan->built.dst_alpha == BlendFactor::Zero);
    CHECK(plan->built.alpha_op == BlendOp::Add);
    // the capability does not matter: the device's blend minimum ignores the factors on every Vulkan device
    CHECK(state::PlanPipeline(Requested(m), CapsWith(false))->alpha_in_shader);
    // the alpha half is the shader's and the colour half's factors read the source alpha, which the shader replaced: the
    // colour half goes to the shader as well, whatever its equation; factors that do not read it stay in the hardware
    m.src_rgb = fac::SrcAlpha;
    m.dst_rgb = fac::OneMinusSrcAlpha;
    plan = state::PlanPipeline(Requested(m), CapsWith(true));
    REQUIRE(plan.ok());
    CHECK(plan->rgb_in_shader);
    CHECK(plan->alpha_in_shader);
    CHECK(plan->built.src_color == BlendFactor::One);
    CHECK(plan->built.dst_color == BlendFactor::Zero);
    CHECK(plan->built.color_op == BlendOp::Add);
    CHECK(plan->rgb_uses_blend_rgb);
    // each source-alpha factor alone, in the source slot and in the destination slot, moves the colour half
    for (const u32 factor : {fac::SrcAlpha, fac::OneMinusSrcAlpha, fac::SrcAlphaSat}) {
        for (const bool source_slot : {true, false}) {
            m.src_rgb = source_slot ? factor : fac::DstColor;
            m.dst_rgb = source_slot ? fac::Zero : factor;
            plan = state::PlanPipeline(Requested(m), CapsWith(true));
            REQUIRE(plan.ok());
            INFO("factor " << factor << " in the " << (source_slot ? "source" : "destination") << " slot");
            CHECK(plan->rgb_in_shader);
            CHECK(plan->rgb_uses_blend_rgb);
        }
    }
    m.src_rgb = fac::DstColor;
    m.dst_rgb = fac::ConstColor;
    plan = state::PlanPipeline(Requested(m), CapsWith(true));
    REQUIRE(plan.ok());
    CHECK_FALSE(plan->rgb_in_shader);
    CHECK_FALSE(plan->rgb_uses_blend_rgb);
    CHECK(plan->alpha_in_shader);
    CHECK(plan->built.src_color == BlendFactor::DstColor);
}

TEST_CASE("plan/constant_alpha_factors_are_rewritten_where_the_vendored_backend_can_express_them") {
    // the alpha half reads the constant's alpha through either pair
    Merger m;
    m.blend = true;
    m.src_rgb = fac::ConstColor;
    m.dst_rgb = fac::OneMinusConstColor;
    m.src_a = fac::ConstAlpha;
    m.dst_a = fac::OneMinusConstAlpha;
    Result<state::PipelinePlan> plan = state::PlanPipeline(Requested(m), CapsWith(true));
    REQUIRE(plan.ok());
    CHECK(plan->built.src_color == BlendFactor::ConstantColor);
    CHECK(plan->built.dst_color == BlendFactor::OneMinusConstantColor);
    CHECK(plan->built.src_alpha == BlendFactor::ConstantColor);
    CHECK(plan->built.dst_alpha == BlendFactor::OneMinusConstantColor);
    CHECK_FALSE(plan->constants_from_alpha);
    const std::array<float, 4> constants = {0.1f, 0.2f, 0.3f, 0.4f};
    CHECK(state::EffectiveBlendConstants(*plan, constants) == constants);
    // the colour half on the alpha pair: the colour pair with the constants (a, a, a, a)
    m.src_rgb = fac::ConstAlpha;
    m.dst_rgb = fac::OneMinusConstAlpha;
    plan = state::PlanPipeline(Requested(m), CapsWith(true));
    REQUIRE(plan.ok());
    CHECK(plan->built.src_color == BlendFactor::ConstantColor);
    CHECK(plan->built.dst_color == BlendFactor::OneMinusConstantColor);
    CHECK(plan->constants_from_alpha);
    CHECK(state::EffectiveBlendConstants(*plan, constants) == std::array<float, 4>{0.4f, 0.4f, 0.4f, 0.4f});
    // one slot of each pair in the colour half cannot be expressed with one constant colour: the shader computes the
    // half, ONE, ZERO, ADD in the pipeline, the constants as requested
    for (const u32 color_pair : {fac::ConstColor, fac::OneMinusConstColor}) {
        for (const u32 alpha_pair : {fac::ConstAlpha, fac::OneMinusConstAlpha}) {
            for (const bool color_first : {true, false}) {
                m.eq_rgb = eq::Subtract;
                m.src_rgb = color_first ? color_pair : alpha_pair;
                m.dst_rgb = color_first ? alpha_pair : color_pair;
                plan = state::PlanPipeline(Requested(m), CapsWith(true));
                REQUIRE_MESSAGE(plan.ok(), plan.error());
                CHECK(plan->rgb_in_shader);
                CHECK(plan->rgb_uses_blend_rgb);
                CHECK_FALSE(plan->alpha_in_shader);
                CHECK(plan->needs_destination_copy);
                CHECK_FALSE(plan->constants_from_alpha);
                CHECK(plan->built.color_op == BlendOp::Add);
                CHECK(plan->built.src_color == BlendFactor::One);
                CHECK(plan->built.dst_color == BlendFactor::Zero);
                CHECK(state::EffectiveBlendConstants(*plan, constants) == constants);
            }
        }
    }
    m.eq_rgb = eq::Add;
    m.src_rgb = fac::ConstColor;
    m.dst_rgb = fac::OneMinusConstAlpha;
    // blending off: nothing to plan; a Min half that held one is the shader's anyway
    m.blend = false;
    CHECK_FALSE(state::PlanPipeline(Requested(m), CapsWith(true))->rgb_in_shader);
    m.blend = true;
    m.eq_rgb = eq::Min;
    plan = state::PlanPipeline(Requested(m), CapsWith(true));
    REQUIRE_MESSAGE(plan.ok(), plan.error());
    CHECK(plan->rgb_in_shader);
    CHECK_FALSE(plan->constants_from_alpha);
}

// ================================================================================================================
// The GLSL text
// ================================================================================================================

TEST_CASE("glsl/the_text_the_product_embeds_is_the_text_of_the_file_and_of_the_test_spirv") {
    const std::string_view text = fbops::OutputStageGlsl();
    REQUIRE(text.size() > 1000);
    CHECK(text.find("vec4 mh4n_logic_op(uint op, vec4 src, vec4 dst)") != std::string_view::npos);
    CHECK(text.find("vec4 mh4n_minmax_blend(uint eq_rgb, uint src_rgb, uint dst_rgb, uint eq_a, uint src_a, uint dst_a, vec4 src,") !=
          std::string_view::npos);
    CHECK(text.find("\n#version") == std::string_view::npos);  // no directive: the generator's shader has its own
    CHECK(text.substr(0, 8) != "#version");

    // 1. the embedded text is the file's, byte for byte (the build generates the source from the file, this pins it)
    const std::filesystem::path file = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "gpu" / "fbops" / "output_stage.glsl";
    std::ifstream in(file, std::ios::binary);
    if (in) {
        std::stringstream contents;
        contents << in.rdbuf();
        CHECK(contents.str() == std::string(text));
    } else {
        test::SkipCase("output_stage.glsl is not at " + file.string() + " (the tree moved since the test was built)");
    }

    // 2. the committed test SPIR-V were built from this text: glslang's -g puts the text of every included file into the
    // module (OpSource), so the text must be inside each array that includes it. Editing output_stage.glsl without
    // running tools/native/spirv_inc.py again makes this fail.
    const auto contains = [&](const uint32_t* words, size_t count) {
        const std::string_view bytes(reinterpret_cast<const char*>(words), count * sizeof(uint32_t));
        return bytes.find(text) != std::string_view::npos;
    };
    CHECK(contains(spv_logic_frag, std::size(spv_logic_frag)));
    CHECK(contains(spv_logic_nodst_frag, std::size(spv_logic_nodst_frag)));
    CHECK(contains(spv_minmax_frag, std::size(spv_minmax_frag)));
    // and the check can fail: a text with one character changed is not in them
    std::string changed(text);
    changed[changed.size() / 2] ^= 1;
    const std::string_view bytes(reinterpret_cast<const char*>(spv_minmax_frag), sizeof(spv_minmax_frag));
    CHECK(bytes.find(changed) == std::string_view::npos);
}

// ================================================================================================================
// Blending on the device
// ================================================================================================================

namespace {

const Rgba kSource{40, 90, 160, 200};
const Rgba kDestination{100, 50, 30, 70};
const std::array<u8, 4> kConstant{200, 100, 50, 150};

DrawCase BlendCase(u32 e_rgb, u32 e_a, u32 s_rgb, u32 d_rgb, u32 s_a, u32 d_a, std::string label) {
    DrawCase c;
    c.m.blend = true;
    c.m.eq_rgb = e_rgb;
    c.m.eq_a = e_a;
    c.m.src_rgb = s_rgb;
    c.m.dst_rgb = d_rgb;
    c.m.src_a = s_a;
    c.m.dst_a = d_a;
    c.m.constant = kConstant;
    c.src = kSource;
    c.label = std::move(label);
    return c;
}

}  // namespace

TEST_CASE("blend/every_equation_pair_with_hardware_and_shader_factors") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    Harness h(backend, false);
    std::vector<DrawCase> cases;
    // factors that keep Min and Max in the hardware (both One), and factors that send them to the shader
    for (u32 e_rgb = 0; e_rgb < 5; ++e_rgb) {
        for (u32 e_a = 0; e_a < 5; ++e_a) {
            cases.push_back(BlendCase(e_rgb, e_a, fac::One, fac::One, fac::One, fac::One, "ones"));
            cases.push_back(BlendCase(e_rgb, e_a, fac::SrcAlpha, fac::OneMinusSrcAlpha, fac::DstColor, fac::OneMinusDstAlpha,
                                      "mixed"));
        }
    }
    REQUIRE(cases.size() == 50);
    for (u32 i = 0; i < cases.size(); ++i) cases[i] = At(cases[i], i);
    RunCases(h, cases, kDestination, 1.0f, 0, 1);
    // the shader path really ran for the Min and Max cases with factors, and the hardware for the others
    u32 in_shader = 0;
    for (u32 i = 0; i < cases.size(); ++i) {
        const bool want = ((cases[i].m.eq_rgb >= 3 || cases[i].m.eq_a >= 3) && cases[i].label == "mixed");
        if (h.plans[i].needs_destination_copy) ++in_shader;
        CHECK(h.plans[i].needs_destination_copy == want);
    }
    CHECK(in_shader == 5 * 5 - 3 * 3);
}

TEST_CASE("blend/every_factor_in_every_slot") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    Harness h(backend, false);
    std::vector<DrawCase> cases;
    // one factor in one slot, the other slot of its half Zero (so the sum shows the factor alone, with no clamping), the
    // other half One / Zero; Add everywhere
    for (u32 slot = 0; slot < 4; ++slot) {
        for (u32 f = 0; f < fac::kCount; ++f) {
            u32 s_rgb = fac::One, d_rgb = fac::Zero, s_a = fac::One, d_a = fac::Zero;
            switch (slot) {
            case 0: s_rgb = f; d_rgb = fac::Zero; break;
            case 1: s_rgb = fac::Zero; d_rgb = f; break;
            case 2: s_a = f; d_a = fac::Zero; break;
            case 3: s_a = fac::Zero; d_a = f; break;
            }
            cases.push_back(BlendCase(eq::Add, eq::Add, s_rgb, d_rgb, s_a, d_a, "slot " + std::to_string(slot) + " factor " + std::to_string(f)));
        }
    }
    REQUIRE(cases.size() == 60);
    for (u32 i = 0; i < cases.size(); ++i) cases[i] = At(cases[i], i);
    RunCases(h, cases, kDestination, 1.0f, 0, 1);
    // the constant-alpha factors of the colour half used the rewrite
    u32 rewritten = 0;
    for (const state::PipelinePlan& plan : h.plans) rewritten += plan.constants_from_alpha ? 1 : 0;
    CHECK(rewritten == 4);  // slots 0 and 1, factors 12 and 13

    // the cases tell the factors apart: the model gives a different pixel for each factor of a slot, ConstantColor against
    // ConstantAlpha included (a test that passed for any factor would pass for a wrong table)
    std::set<std::array<int, 4>> seen;
    for (u32 f = 0; f < fac::kCount; ++f) {
        const DrawCase c = BlendCase(eq::Add, eq::Add, f, fac::Zero, fac::One, fac::Zero, "");
        const Rgba px = SwPixelColor(c.m, kSource, kDestination);
        seen.insert({px[0], px[1], px[2], px[3]});
    }
    CHECK(seen.size() >= 13);
}

TEST_CASE("blend/a_colour_half_with_both_constant_pairs_is_the_shaders") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::LogicOp, Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        Harness h(backend, false);
        std::vector<DrawCase> cases;
        const std::array<std::array<u8, 4>, 3> constants = {{{200, 100, 50, 150}, {10, 240, 128, 64}, {255, 30, 90, 220}}};
        u32 index = 0;
        for (const u32 equation : {eq::Add, eq::Subtract, eq::ReverseSubtract}) {
            for (const u32 color_pair : {fac::ConstColor, fac::OneMinusConstColor}) {
                for (const u32 alpha_pair : {fac::ConstAlpha, fac::OneMinusConstAlpha}) {
                    for (const bool color_first : {true, false}) {
                        for (const auto& constant : constants) {
                            DrawCase c = BlendCase(equation, eq::Add, color_first ? color_pair : alpha_pair,
                                                   color_first ? alpha_pair : color_pair, fac::ConstAlpha, fac::OneMinusConstColor, "mixed");
                            c.m.constant = constant;
                            cases.push_back(At(c, index++));
                        }
                    }
                }
            }
        }
        REQUIRE(cases.size() == 72);
        RunCases(h, cases, kDestination, 1.0f, 0, 1);
        for (const state::PipelinePlan& plan : h.plans) {
            CHECK(plan.rgb_in_shader);
            CHECK_FALSE(plan.alpha_in_shader);
            CHECK_FALSE(plan.constants_from_alpha);
            CHECK(plan.needs_destination_copy);
        }
        // the constants matter and the pairs differ: the model gives several different pixels for one combination
        std::set<std::array<int, 4>> seen;
        for (u32 i = 0; i < 3; ++i) seen.insert(h.model[i].color);
        CHECK(seen.size() == 3);
        CHECK(h.model[0].color != h.model[6].color);
    }
}

TEST_CASE("blend/the_constant_colour_comes_from_set_blend_constants_and_changes_without_a_new_pipeline") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    Harness h(backend, false);
    std::vector<DrawCase> cases;
    // one pipeline (ConstantColor / OneMinusConstantColor), four constants: the draws differ only in the dynamic state
    const std::array<std::array<u8, 4>, 4> constants = {{{0, 0, 0, 0}, {255, 255, 255, 255}, {200, 100, 50, 150}, {10, 240, 128, 64}}};
    for (u32 i = 0; i < 4; ++i) {
        DrawCase c = BlendCase(eq::Add, eq::Add, fac::ConstColor, fac::OneMinusConstColor, fac::ConstAlpha, fac::OneMinusConstAlpha, "");
        c.m.constant = constants[i];
        cases.push_back(At(c, i));
    }
    RunCases(h, cases, kDestination, 1.0f, 0, 1);
    CHECK(h.pipelines.size() == 1);  // the constant is not in the pipeline
    // the constants matter: the four pixels differ in the model
    std::set<std::array<int, 4>> seen;
    for (u32 i = 0; i < 4; ++i) seen.insert(h.model[i].color);
    CHECK(seen.size() == 4);
}

TEST_CASE("blend/colour_write_masks_in_blending_and_in_the_logic_op") {
    for (const bool xor_op : {false, true}) {
        for (const bool emulate : {false, true}) {
            if (!xor_op && emulate) continue;
            std::shared_ptr<Backend> backend = emulate ? MakeBackend({Cap::LogicOp}) : MakeBackend();
            if (!backend) return;
            Harness h(backend, false);
            std::vector<DrawCase> cases;
            for (u32 mask = 0; mask < 16; ++mask) {
                DrawCase c = BlendCase(eq::Add, eq::Add, fac::SrcAlpha, fac::OneMinusSrcAlpha, fac::One, fac::One, "mask");
                if (xor_op) {
                    c.m.blend = false;
                    c.m.logic_op = lop::Xor;
                }
                c.m.write_mask = mask;
                cases.push_back(At(c, mask));
            }
            // the framebuffer's colour write off: nothing is written
            DrawCase off = cases[0];
            off.m.write_mask = 15;
            off.m.allow_color = false;
            cases.push_back(At(off, 16));
            INFO("xor " << xor_op << " emulated " << emulate);
            RunCases(h, cases, kDestination, 1.0f, 0, 1);
            // the mask was a pipeline parameter: every mask has a pixel of the right kind (a masked channel keeps the destination)
            for (u32 mask = 0; mask < 16; ++mask) {
                for (u32 channel = 0; channel < 4; ++channel) {
                    if (!((mask >> channel) & 1)) CHECK(h.model[mask].color[channel] == kDestination[channel]);
                }
            }
            CHECK(h.model[16].color == kDestination);
        }
    }
}

TEST_CASE("blend/min_and_max_in_hardware_with_one_factors_and_in_the_shader_otherwise") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::LogicOp, Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        Harness h(backend, false);
        std::vector<DrawCase> cases;
        const std::vector<std::pair<u32, u32>> rgb_pairs = {{fac::One, fac::One},      {fac::SrcAlpha, fac::One},
                                                            {fac::One, fac::DstColor}, {fac::SrcColor, fac::OneMinusSrcColor},
                                                            {fac::ConstColor, fac::OneMinusConstColor}, {fac::SrcAlphaSat, fac::DstAlpha},
                                                            {fac::Zero, fac::One}};
        const std::vector<std::pair<u32, u32>> alpha_pairs = {{fac::One, fac::One}, {fac::SrcAlpha, fac::OneMinusDstAlpha},
                                                              {fac::ConstAlpha, fac::DstColor}};
        for (const u32 e_rgb : {eq::Min, eq::Max}) {
            for (const auto& rgb : rgb_pairs) {
                for (const u32 e_a : {eq::Min, eq::Max, eq::Add}) {
                    for (const auto& alpha : alpha_pairs) {
                        cases.push_back(BlendCase(e_rgb, e_a, rgb.first, rgb.second, alpha.first, alpha.second, "minmax"));
                    }
                }
            }
        }
        REQUIRE(cases.size() == 126);
        for (u32 i = 0; i < cases.size(); ++i) cases[i] = At(cases[i], i);
        RunCases(h, cases, kDestination, 1.0f, 0, 1);
        u32 hardware = 0, shader = 0;
        for (u32 i = 0; i < cases.size(); ++i) {
            const state::PipelinePlan& plan = h.plans[i];
            const bool rgb_one = cases[i].m.src_rgb == fac::One && cases[i].m.dst_rgb == fac::One;
            const bool alpha_min_max = cases[i].m.eq_a != eq::Add;
            const bool alpha_one = cases[i].m.src_a == fac::One && cases[i].m.dst_a == fac::One;
            CHECK(plan.rgb_in_shader == !rgb_one);
            CHECK(plan.alpha_in_shader == (alpha_min_max && !alpha_one));
            (plan.needs_destination_copy ? shader : hardware)++;
        }
        CHECK(hardware > 0);
        CHECK(shader > hardware);
    }
}

namespace {
// Larger than the tolerance of 1 that the blend checks allow, or the control could not tell the two apart.
int MaxDifference(const Rgba& a, const Rgba& b) {
    int most = 0;
    for (u32 i = 0; i < 4; ++i) most = std::max(most, std::abs(a[i] - b[i]));
    return most;
}
}  // namespace

TEST_CASE("controls/the_cases_tell_a_wrong_minmax_or_constant_apart") {
    // Min of source * SrcAlpha and destination * SrcAlpha: the shader's answer. A pipeline that handed the equation to the
    // hardware (Vulkan's MIN ignores the factors) would give min(source, destination) instead; the case must tell them apart.
    DrawCase c = BlendCase(eq::Min, eq::Min, fac::SrcAlpha, fac::SrcAlpha, fac::SrcAlpha, fac::SrcAlpha, "");
    const Rgba right = SwPixelColor(c.m, kSource, kDestination);
    const Rgba ignoring_factors = {std::min(kSource[0], kDestination[0]), std::min(kSource[1], kDestination[1]),
                                   std::min(kSource[2], kDestination[2]), std::min(kSource[3], kDestination[3])};
    int different = 0;
    for (u32 i = 0; i < 4; ++i) different += std::abs(right[i] - ignoring_factors[i]) > 2 ? 1 : 0;
    CHECK(different >= 3);
    // the constant: the draw with constants (200, 100, 50, 150) is not the draw with zero constants
    DrawCase k = BlendCase(eq::Add, eq::Add, fac::ConstColor, fac::Zero, fac::ConstAlpha, fac::Zero, "");
    DrawCase zero = k;
    zero.m.constant = {0, 0, 0, 0};
    CHECK(MaxDifference(SwPixelColor(k.m, kSource, kDestination), SwPixelColor(zero.m, kSource, kDestination)) > 1);
    // and the colour half on ConstantAlpha is not the same as on ConstantColor
    DrawCase a = BlendCase(eq::Add, eq::Add, fac::ConstAlpha, fac::Zero, fac::One, fac::Zero, "");
    DrawCase b = BlendCase(eq::Add, eq::Add, fac::ConstColor, fac::Zero, fac::One, fac::Zero, "");
    CHECK(MaxDifference(SwPixelColor(a.m, kSource, kDestination), SwPixelColor(b.m, kSource, kDestination)) > 1);
}

// ================================================================================================================
// Logic ops on the device
// ================================================================================================================

TEST_CASE("logic_ops/all_16_in_hardware_and_through_the_shader_over_the_feedback_copy") {
    const Rgba dst{0x3C, 0x66, 0xF0, 0xA5};
    const Rgba src{0xF0, 0xCC, 0xAA, 0x3C};
    // the case can tell the ops apart: the model gives 16 different pixels
    std::set<std::array<int, 4>> distinct;
    for (u32 op = 0; op < 16; ++op) {
        Merger m;
        m.logic_op = op;
        distinct.insert(SwPixelColor(m, src, dst));
    }
    CHECK(distinct.size() == 16);

    for (const bool emulate : {false, true}) {
        std::shared_ptr<Backend> backend = emulate ? MakeBackend({Cap::LogicOp}) : MakeBackend();
        if (!backend) return;
        INFO("emulated " << emulate << " logic_op bit " << backend->Caps().logic_op);
        if (emulate) CHECK_FALSE(backend->Caps().logic_op);
        Harness h(backend, false);
        std::vector<DrawCase> cases;
        for (u32 op = 0; op < 16; ++op) {
            DrawCase c;
            c.m.logic_op = op;
            c.src = src;
            c.label = "op " + std::to_string(op);
            cases.push_back(At(c, op));
        }
        RunCases(h, cases, dst, 1.0f, 0, 0);  // exact
        // which path each op took
        const bool hardware = backend->Caps().logic_op;
        for (u32 op = 0; op < 16; ++op) {
            const state::PipelinePlan& plan = h.plans[op];
            INFO("op " << op);
            if (hardware) {
                CHECK_FALSE(plan.shader_logic_op);
                CHECK(plan.built.logic_op_enabled == (op != lop::Copy));
            } else {
                CHECK(plan.shader_logic_op == (op != lop::Copy && op != lop::NoOp));
                CHECK_FALSE(plan.built.logic_op_enabled);
                CHECK(plan.needs_destination_copy == (std::set<u32>{1, 2, 7, 8, 9, 10, 11, 12, 13, 14, 15}.count(op) != 0));
            }
        }
    }
}

TEST_CASE("logic_ops/no_op_discards_the_colour_and_keeps_the_depth_write") {
    for (const bool emulate : {false, true}) {
        std::shared_ptr<Backend> backend = emulate ? MakeBackend({Cap::LogicOp}) : MakeBackend();
        if (!backend) return;
        Harness h(backend, true);
        DrawCase c;
        c.m.logic_op = lop::NoOp;
        c.m.depth_test = true;
        c.m.depth_func = cmp::Less;
        c.m.depth_write = true;
        c.src = {255, 0, 0, 255};
        c.z = -0.25f;  // depth 0.625 against the cleared 1.0
        std::vector<DrawCase> cases = {At(c, 0)};
        DrawCase copy = c;  // the control: Copy writes the colour
        copy.m.logic_op = lop::Copy;
        cases.push_back(At(copy, 1));
        INFO("emulated " << emulate);
        RunCases(h, cases, {10, 20, 30, 40}, 1.0f, 0, 0, true, true, false);
        // NoOp kept the destination, wrote the depth; Copy wrote the colour
        CHECK(h.model[0].color == Rgba{10, 20, 30, 40});
        CHECK(h.model[0].depth == static_cast<u32>(0.625f * 0xFFFFFF));
        CHECK(h.model[1].color == Rgba{255, 0, 0, 255});
        if (emulate) CHECK(h.plans[0].built.color_write_mask == 0);
    }
}

// ================================================================================================================
// Depth and stencil on the device
// ================================================================================================================

TEST_CASE("depth/every_compare_op_with_the_ranges_and_offsets_and_the_write") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        Harness h(backend, true);
        INFO("d24s8 masked " << masked << " stored " << static_cast<int>(h.depth->GetFormat()));
        CHECK(h.depth->GetFormat() == (masked || !backend->Caps().d24s8 ? Format::D32S8 : Format::D24S8));
        // the PICA depth is z_over_w * scale + offset; the buffer holds 0.5. With (0.5, 0.75): z = -1, -0.5, 0 give 0.25,
        // 0.5, 0.75; with (1, 1): z = -0.75, -0.5, -0.25 give the same three (a different range and offset, the same depths)
        struct Range {
            float scale, offset;
            float z[3];
        };
        const Range ranges[2] = {{0.5f, 0.75f, {-1.0f, -0.5f, 0.0f}}, {1.0f, 1.0f, {-0.75f, -0.5f, -0.25f}}};
        std::vector<DrawCase> cases;
        u32 index = 0;
        for (const bool write : {false, true}) {
            for (u32 func = 0; func < 8; ++func) {
                for (const Range& range : ranges) {
                    for (u32 k = 0; k < 3; ++k) {
                        DrawCase c;
                        c.m.depth_test = true;
                        c.m.depth_func = func;
                        c.m.depth_write = write;
                        c.src = {255, 0, 0, 255};
                        c.z = range.z[k];
                        c.scale = range.scale;
                        c.offset = range.offset;
                        c.label = "func " + std::to_string(func);
                        cases.push_back(At(c, index++));
                    }
                }
            }
        }
        REQUIRE(cases.size() == 96);
        RunCases(h, cases, {0, 0, 0, 0}, 0.5f, 0, 0, true, true, false);
        // the colour shows pass or fail: for each function the three depths give the pattern the function has
        const bool expected_pass[8][3] = {{0, 0, 0}, {1, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {0, 0, 1}, {0, 1, 1}};
        for (u32 func = 0; func < 8; ++func) {
            for (u32 k = 0; k < 3; ++k) {
                const u32 at = (func * 2) * 3 + k;  // write off, first range
                CHECK((h.model[at].color[0] == 255) == expected_pass[func][k]);
            }
        }
    }
}

TEST_CASE("depth/the_write_without_the_test_the_framebuffers_write_off_and_the_clamp") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        Harness h(backend, true);
        std::vector<DrawCase> cases;
        u32 index = 0;
        const auto add = [&](DrawCase c) { cases.push_back(At(c, index++)); };
        // test off, write on: the depth is written whatever the buffer holds (Always); the depth function is not used
        for (const float z : {-1.0f, -0.5f, 0.0f}) {
            DrawCase c;
            c.m.depth_write = true;
            c.m.depth_func = cmp::Never;
            c.src = {255, 0, 0, 255};
            c.z = z;
            add(c);
        }
        // test on, write on, but the framebuffer's depth/stencil write is off: the colour is drawn, the depth stays
        DrawCase off;
        off.m.depth_test = true;
        off.m.depth_func = cmp::Always;
        off.m.depth_write = true;
        off.m.allow_depth_stencil = false;
        off.src = {255, 0, 0, 255};
        off.z = -1.0f;
        add(off);
        // test and write both off: nothing about the depth
        DrawCase none;
        none.src = {0, 255, 0, 255};
        none.z = -1.0f;
        add(none);
        // the clamp: depth above 1 and below 0 are 1 and 0 (the software renderer clamps)
        DrawCase high;
        high.m.depth_write = true;
        high.src = {255, 0, 0, 255};
        high.z = 0.0f;
        high.scale = 0.5f;
        high.offset = 1.25f;
        add(high);
        DrawCase low = high;
        low.z = -1.0f;
        low.offset = 0.25f;
        add(low);
        RunCases(h, cases, {0, 0, 0, 0}, 0.5f, 0, 0, true, true, false);
        CHECK(h.model[3].depth == static_cast<u32>(0.5f * 0xFFFFFF));  // unchanged by the draw with the write off
        CHECK(h.model[5].depth == 0xFFFFFF);
        CHECK(h.model[6].depth == 0);
    }
}

TEST_CASE("stencil/every_compare_with_reference_and_masks") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        Harness h(backend, true);
        // (old stencil value, reference, compare mask, write mask) triples: the compare is (reference & mask) OP (old & mask)
        struct Triple {
            u32 old_value, reference, compare_mask, write_mask;
        };
        const Triple triples[] = {{0x5A, 0x5A, 0xFF, 0xFF}, {0x5A, 0x3C, 0xFF, 0xFF}, {0x5A, 0xA5, 0xFF, 0xFF},
                                  {0x5A, 0x1A, 0x0F, 0xFF}, {0x5A, 0xFA, 0x0F, 0x0F}, {0xFF, 0x00, 0xF0, 0xF0},
                                  {0x00, 0xFF, 0x0F, 0x3C}};
        std::vector<DrawCase> cases;
        u32 index = 0;
        // 1. set the old values: Always, Replace everywhere, colour and depth untouched
        std::vector<u32> pixel_of;
        for (u32 func = 0; func < 8; ++func) {
            for (const Triple& t : triples) {
                DrawCase init;
                init.m.stencil = true;
                init.m.stencil_func = cmp::Always;
                init.m.stencil_fail = init.m.stencil_depth_fail = init.m.stencil_depth_pass = act::Replace;
                init.m.reference = t.old_value;
                init.m.allow_color = false;
                init.label = "init";
                cases.push_back(At(init, index));
                ++index;
            }
        }
        // 2. the test draws: pass replaces with the reference, fail zeroes (under the write mask), the depth passes
        index = 0;
        for (u32 func = 0; func < 8; ++func) {
            for (const Triple& t : triples) {
                DrawCase c;
                c.m.stencil = true;
                c.m.stencil_func = func;
                c.m.reference = t.reference;
                c.m.input_mask = t.compare_mask;
                c.m.stencil_write_mask = t.write_mask;
                c.m.stencil_fail = act::Zero;
                c.m.stencil_depth_fail = act::Invert;
                c.m.stencil_depth_pass = act::Replace;
                c.m.depth_test = true;
                c.m.depth_func = cmp::Less;
                c.src = {255, 0, 0, 255};
                c.z = -0.5f;  // depth 0.5 against the cleared 1.0
                c.label = "func " + std::to_string(func);
                cases.push_back(At(c, index));
                ++index;
            }
        }
        INFO("d24s8 masked " << masked);
        // the stencil values: every pixel was drawn twice (init, test); RunCases's touched set is the 56 pixels
        RunCases(h, cases, {0, 0, 0, 0}, 1.0f, 0, 0, true, true, true);
        // the cases tell the compares apart: across a function's triples both outcomes occur for some function, and the
        // functions differ from each other
        std::set<u32> outcomes_seen;
        for (u32 func = 0; func < 8; ++func) {
            u32 pattern = 0;
            for (u32 i = 0; i < std::size(triples); ++i) pattern |= (h.model[func * std::size(triples) + i].color[0] == 255 ? 1u : 0u) << i;
            outcomes_seen.insert(pattern);
        }
        CHECK(outcomes_seen.size() >= 6);
    }
}

TEST_CASE("stencil/every_action_in_every_outcome") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        Harness h(backend, true);
        const u32 olds[] = {0x00, 0x7F, 0xFF};
        std::vector<DrawCase> cases;
        u32 index = 0;
        // 1. the old values, one pixel per (rotation, outcome, old)
        for (u32 rotation = 0; rotation < 8; ++rotation) {
            for (u32 outcome = 0; outcome < 3; ++outcome) {
                for (const u32 old_value : olds) {
                    DrawCase init;
                    init.m.stencil = true;
                    init.m.stencil_fail = init.m.stencil_depth_fail = init.m.stencil_depth_pass = act::Replace;
                    init.m.reference = old_value;
                    init.m.allow_color = false;
                    init.label = "init";
                    cases.push_back(At(init, index++));
                }
            }
        }
        // 2. the test draws. The three actions of a pipeline are different (rotation, rotation + 1, rotation + 2), so over
        // the eight rotations each action is seen in each slot. Outcomes: 0 the stencil test fails (Equal against a
        // reference that differs), 1 the stencil passes and the depth fails, 2 both pass. The reference is a third value
        // so that Replace is visible.
        index = 0;
        for (u32 rotation = 0; rotation < 8; ++rotation) {
            for (u32 outcome = 0; outcome < 3; ++outcome) {
                for (const u32 old_value : olds) {
                    DrawCase c;
                    c.m.stencil = true;
                    c.m.stencil_func = cmp::Equal;
                    c.m.reference = outcome == 0 ? (old_value ^ 0x01) : old_value;
                    c.m.stencil_fail = rotation;
                    c.m.stencil_depth_fail = (rotation + 1) % 8;
                    c.m.stencil_depth_pass = (rotation + 2) % 8;
                    c.m.depth_test = true;
                    c.m.depth_func = cmp::Less;
                    c.src = {255, 0, 0, 255};
                    c.z = outcome == 1 ? 0.0f : -0.5f;  // depth 0.75 or 0.5 (z * 0.5 + 0.75), against the 0.6 of the clear below
                    c.label = "rotation " + std::to_string(rotation) + " outcome " + std::to_string(outcome);
                    cases.push_back(At(c, index++));
                }
            }
        }
        INFO("d24s8 masked " << masked);
        // depth buffer cleared to 0.6: z = -0.5 (0.5) passes Less, z = 0 (0.75) fails
        RunCases(h, cases, {0, 0, 0, 0}, 0.6f, 0, 0, true, true, true);
        // the stencil outcomes happened: spot checks from the model that the three slots did different things
        u32 changed = 0;
        for (u32 i = 0; i < 72; ++i) changed += h.model[i].stencil != olds[i % 3] ? 1 : 0;
        CHECK(changed > 30);
    }
}

TEST_CASE("stencil/disabled_leaves_the_buffer_and_the_write_mask_register_zero_protects_it") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    Harness h(backend, true);
    std::vector<DrawCase> cases;
    u32 index = 0;
    // stencil off, with actions that would change it: the buffer keeps its clear value
    DrawCase off;
    off.m.stencil = false;
    off.m.stencil_fail = off.m.stencil_depth_fail = off.m.stencil_depth_pass = act::Zero;
    off.src = {255, 0, 0, 255};
    cases.push_back(At(off, index++));
    // stencil on but the framebuffer's depth/stencil write off: the write mask is 0
    DrawCase protect = off;
    protect.m.stencil = true;
    protect.m.allow_depth_stencil = false;
    cases.push_back(At(protect, index++));
    // a stencil write mask of 0
    DrawCase zero_mask = off;
    zero_mask.m.stencil = true;
    zero_mask.m.stencil_write_mask = 0;
    cases.push_back(At(zero_mask, index++));
    // the control: stencil on and writing zeroes the pixel
    DrawCase writes = off;
    writes.m.stencil = true;
    cases.push_back(At(writes, index++));
    RunCases(h, cases, {0, 0, 0, 0}, 1.0f, 0x6B, 0, true, true, true);
    CHECK(h.model[0].stencil == 0x6B);
    CHECK(h.model[1].stencil == 0x6B);
    CHECK(h.model[2].stencil == 0x6B);
    CHECK(h.model[3].stencil == 0);
}

// ================================================================================================================
// The dynamic state is set per recorder and does not make pipelines
// ================================================================================================================

TEST_CASE("state/dynamic_state_changes_between_draws_of_one_pipeline") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    Harness h(backend, true);
    std::vector<DrawCase> cases;
    // one pipeline (stencil Equal, Replace on pass, Keep otherwise, blend with the constant): the reference, the masks and
    // the constant come from the registers of each draw
    u32 index = 0;
    for (u32 reference : {0x10u, 0x20u, 0x30u}) {
        DrawCase init;
        init.m.stencil = true;
        init.m.stencil_fail = init.m.stencil_depth_fail = init.m.stencil_depth_pass = act::Replace;
        init.m.reference = 0x20;
        init.m.allow_color = false;
        cases.push_back(At(init, index));
        DrawCase c = BlendCase(eq::Add, eq::Add, fac::ConstColor, fac::OneMinusConstColor, fac::One, fac::Zero, "");
        c.m.stencil = true;
        c.m.stencil_func = cmp::Equal;
        c.m.reference = reference;
        c.m.stencil_depth_pass = act::Zero;
        c.m.constant = {u8(reference * 4), u8(reference * 2), u8(reference), 255};
        cases.push_back(At(c, index));
        ++index;
    }
    const size_t pipelines_before = h.pipelines.size();
    RunCases(h, cases, kDestination, 1.0f, 0, 1, true, true, true);
    CHECK(pipelines_before == 0);
    CHECK(h.pipelines.size() == 2);  // the init pipeline and the one test pipeline: three draws, one pipeline each
    // only the middle draw passed the stencil test (reference 0x20 against the 0x20 the pixel holds)
    CHECK(h.model[0].stencil == 0x20);
    CHECK(h.model[1].stencil == 0);
    CHECK(h.model[2].stencil == 0x20);
}

// ================================================================================================================
// The two blend functions of the GLSL, on their contracts
// ================================================================================================================

namespace {

// The constants of minmax.frag: mh4n_minmax_blend's equations and factors, then mh4n_blend_rgb's equation and whether it runs.
std::vector<SpecConstant> BlendSpec(u32 eq_rgb, u32 src_rgb, u32 dst_rgb, u32 eq_a, u32 src_a, u32 dst_a, u32 blend_rgb_eq = 0,
                                    bool use_blend_rgb = false, std::array<u8, 4> blend_color = {0, 0, 0, 0}) {
    std::vector<SpecConstant> spec = {{7, eq_rgb}, {8, src_rgb}, {9, dst_rgb}, {10, eq_a}, {11, src_a}, {12, dst_a},
                                      {17, blend_rgb_eq}, {18, use_blend_rgb ? 1u : 0u}};
    for (u32 i = 0; i < 4; ++i) spec.push_back({13 + i, FloatBits(float(blend_color[i]) / 255.0f)});
    return spec;
}

}  // namespace

TEST_CASE("glsl/a_half_that_is_not_min_or_max_returns_the_source_and_blend_rgb_is_the_whole_colour_half") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    Harness h(backend, false);
    const Rgba src{40, 90, 160, 200}, dst{100, 50, 30, 70};
    struct Raw {
        std::vector<SpecConstant> spec;
        Rgba want;
        int tolerance;
        const char* what;
    };
    const u32 none = 0xFFFFFFFFu;
    const std::vector<Raw> raws = {
        // mh4n_minmax_blend: the canonical zeros of a key (equation 0, factors 0) give the source back, not black
        {BlendSpec(0, 0, 0, 0, 0, 0), src, 0, "equation 0, factors 0"},
        {BlendSpec(1, 0, 0, 2, 0, 0), src, 0, "Subtract and ReverseSubtract are not its halves"},
        {BlendSpec(5, 0, 0, none, 0, 0), src, 0, "equations past the enumeration"},
        // the control: Min of nothing is black (what a function that computed every half would give for those zeros)
        {BlendSpec(3, 0, 0, 3, 0, 0), {0, 0, 0, 0}, 0, "Min with Zero factors computes"},
        {BlendSpec(4, 1, 1, 0, 0, 0), {100, 90, 160, 200}, 1, "Max colour half, alpha half left alone"},
        // mh4n_blend_rgb: the whole colour half, the alpha from mh4n_minmax_blend (left alone here)
        {BlendSpec(0, 1, 1, 0, 0, 0, 0, true), {140, 140, 190, 200}, 1, "Add One One"},
        {BlendSpec(0, 1, 1, 0, 0, 0, 2, true), {60, 0, 0, 200}, 1, "ReverseSubtract One One"},
        {BlendSpec(0, 1, 1, 0, 0, 0, 3, true), {40, 50, 30, 200}, 1, "Min One One"},
        {BlendSpec(0, 1, 1, 0, 0, 0, 7, true), src, 0, "another equation gives the source"},
        {BlendSpec(0, 1, 1, 0, 0, 0, none, true), src, 0, "no equation gives the source"},
        // the ordering: the alpha half is Min of source alpha * source alpha and the destination (70), and the colour half
        // (source * SrcAlpha) must still read the ORIGINAL source alpha 200: 40 * 200 / 255 and so on
        {BlendSpec(0, 6, 0, 3, 6, 1, 0, true), {31, 71, 125, 70}, 1, "the colour half reads the original source alpha"},
    };
    std::vector<DrawCase> unused;
    Recorder rec = h.Begin(dst, 1.0f, 0);
    BindingSet* set = nullptr;
    for (u32 i = 0; i < raws.size(); ++i) h.DrawShader(rec, src, raws[i].spec, i, 0, &set);
    const Harness::Result3 got = h.Finish(std::move(rec));
    for (u32 i = 0; i < raws.size(); ++i) {
        for (u32 channel = 0; channel < 4; ++channel) {
            INFO(raws[i].what << " channel " << channel << " got " << int(got.color[i * 4 + channel]));
            CHECK(std::abs(int(got.color[i * 4 + channel]) - raws[i].want[channel]) <= raws[i].tolerance);
        }
    }
}

TEST_CASE("blend/source_alpha_factors_in_the_colour_half_follow_an_alpha_half_in_the_shader") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::LogicOp, Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        Harness h(backend, false);
        std::vector<DrawCase> cases;
        u32 index = 0;
        // SrcAlpha, OneMinusSrcAlpha and SrcAlphaSaturate each alone in the source slot and in the destination slot (the
        // other slot Zero or DstColor), colour half Add (and Subtract or ReverseSubtract with the other slot Zero), alpha
        // half Min or Max with factors that are not both One, so that the alpha the blender would see (70 or 156) is not the
        // source alpha (200)
        for (const u32 factor : {fac::SrcAlpha, fac::OneMinusSrcAlpha, fac::SrcAlphaSat}) {
            for (const bool in_source_slot : {true, false}) {
                for (const u32 alpha_eq : {eq::Min, eq::Max}) {
                    struct Variant {
                        u32 equation, other;
                    };
                    const Variant variants[] = {{eq::Add, fac::Zero},
                                                {eq::Add, fac::DstColor},
                                                {in_source_slot ? eq::Subtract : eq::ReverseSubtract, fac::Zero}};
                    for (const Variant& v : variants) {
                        DrawCase c = BlendCase(v.equation, alpha_eq, in_source_slot ? factor : v.other,
                                               in_source_slot ? v.other : factor, fac::SrcAlpha, fac::One, "source alpha");
                        cases.push_back(At(c, index++));
                    }
                }
            }
        }
        // the controls: colour factors that do not read the source alpha stay in hardware beside the shader's alpha half
        const std::pair<u32, u32> unrelated[] = {{fac::DstColor, fac::Zero}, {fac::ConstColor, fac::DstAlpha},
                                                 {fac::Zero, fac::OneMinusDstColor}, {fac::DstAlpha, fac::OneMinusConstColor}};
        const u32 first_control = index;
        for (const auto& [src_factor, dst_factor] : unrelated) {
            cases.push_back(At(BlendCase(eq::Add, eq::Max, src_factor, dst_factor, fac::SrcAlpha, fac::One, "unrelated"), index++));
        }
        REQUIRE(cases.size() == 36 + 4);
        RunCases(h, cases, kDestination, 1.0f, 0, 1);
        for (u32 i = 0; i < cases.size(); ++i) {
            const state::PipelinePlan& plan = h.plans[i];
            INFO("case " << i);
            CHECK(plan.alpha_in_shader);
            CHECK(plan.rgb_in_shader == (i < first_control));
            CHECK(plan.rgb_uses_blend_rgb == (i < first_control));
            CHECK(plan.needs_destination_copy);
        }
        // the cases are sensitive: the model with the blender's view (the alpha the shader replaced) gives other pixels
        // for the factors that read the source alpha
        for (const u32 factor : {fac::SrcAlpha, fac::OneMinusSrcAlpha, fac::SrcAlphaSat}) {
            DrawCase right = BlendCase(eq::Add, eq::Max, factor, fac::Zero, fac::SrcAlpha, fac::One, "");
            Rgba replaced = kSource;
            replaced[3] = SwPixelColor(right.m, kSource, kDestination)[3];
            CHECK(SwPixelColor(right.m, kSource, kDestination) != SwPixelColor(right.m, replaced, kDestination));
        }
    }
}
