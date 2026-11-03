// Derived from Azahar src/video_core/renderer_software/sw_rasterizer.cpp (RasterizerSoftware::WriteFog and the computation of
// the depth it takes: the depth scale and offset, the W-buffer factor, the clamp), src/video_core/pica/pica_core.h
// (Fog::LutEntry: the entry's bit fields and the division by 2047), src/video_core/pica/regs_texturing.h (the fog mode, flip,
// colour and table-offset registers, and the TEV stage words), src/video_core/pica/regs_framebuffer.h,
// src/video_core/pica/regs_rasterizer.h, src/video_core/pica/regs_lighting.h and src/video_core/pica/regs_internal.h (the
// positions of the output-merger, depth-range and lighting-disable registers the base state writes),
// src/video_core/rasterizer_accelerated.cpp and src/video_core/pica/dirty_regs.h (the fog colour uniform, bytes over 255, and the
// register that dirties it) and src/video_core/renderer_vulkan/vk_rasterizer.cpp (SyncAndUploadLUTsLF: the fog table's
// {value, difference} pairs in lut_lf) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp says
// "Licensed under GPLv3 or any later version"; every other file named here says "Licensed under GPLv2 or any later version").
// The rig below (Tri, Vertex, Rig, the register helpers) is adapted from N2-08's TEV test in this directory (GPL-3.0-or-later, see
// its header for what it takes from Azahar and for the BSD-3-Clause notice of the instruction encoder, which this file does not use).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, rasterizer_accelerated.cpp, vk_rasterizer.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h, regs_internal.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
// Copyright 2017 Citra Emulator Project (regs_lighting.h)
// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project (dirty_regs.h)
//
// Fog and gas (N2-10), on whatever Vulkan device the machine has (MH4N_VULKAN_DEVICE; a case that needs one skips with a message
// without it and fails under MH4N_REQUIRE_VULKAN=1).
//
// FOG. The expected pixels are computed here by a CPU port of the SOFTWARE renderer's WriteFog (u8 colours, a float fog index, the
// table's value + difference * fraction, the result truncated to a byte) from the depth the software renderer computes (z over w
// times the depth scale plus the offset, times w in W-buffering, clamped to [0, 1]). The generator follows Azahar's GLSL (float
// arithmetic, the colour rounded to a byte afterwards), so the two agree within one step of 8 bits (truncation against rounding).
//
// GAS. Azahar draws no gas. The expected values are the ones docs/research/gas.md works out by hand from 3dbrew's register
// descriptions and the labelled steps of fog/fog.cpp (D1 to D6, S1 to S6); they are written below as literals (the "by hand"
// numbers) and, separately, by a small CPU model of the same steps for the cases that vary a value. The 16-bit densities are
// chosen so that every sum is exact in binary16: the rounding of an inexact sum is open (gas.md). Every draw is also rendered in
// the ubershader and must give the same pixels as the specialised shader.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gpu/cpuvertex/cpu_vertex.h"
#include "gpu/cpuvertex/triangle_batch.h"
#include "gpu/fbops/fixed_function.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/fog/fog_layout.h"
#include "gpu/fragment/generator.h"
#include "gpu/fragment/key.h"
#include "gpu/fragment/profile.h"
#include "gpu/fragment/state.h"
#include "gpu/fragment/tables.h"
#include "gpu/fragment/uniforms.h"
#include "gpu/native/bindings.h"
#include "gpu/native/compose.h"
#include "gpu/native/default_provider.h"
#include "gpu/native/pipeline_builder.h"
#include "gpu/pica/state.h"
#include "support/vulkan_device.h"

using namespace mh4n;
using namespace mh4n::gpu::fragment;
namespace re = mh4n::gpu::fragment::regenum;
namespace native = mh4n::gpu::native;
namespace cpuvertex = mh4n::gpu::cpuvertex;
namespace fbops = mh4n::gpu::fbops;
namespace pica = mh4n::gpu::pica;
namespace rhi = mh4n::gpu::rhi;
namespace test = mh4n::test;

namespace {

constexpr u32 kSize = 8;  // the targets are 8x8

// ---- registers --------------------------------------------------------------------------------------------------------

constexpr u32 kRegDepthRange = 0x4D, kRegDepthNear = 0x4E, kRegDepthmap = 0x6D;
constexpr u32 kRegTev0 = 0xC0;
constexpr u32 kRegFogMode = 0xE0, kRegFogColor = 0xE1;
constexpr u32 kRegGasAttenuation = 0xE4, kRegGasAccMax = 0xE5;
constexpr u32 kRegOutputMerger = 0x100, kRegLogicOp = 0x102, kRegDepthColorMask = 0x107, kRegAllowColor = 0x113, kRegAllowDepth = 0x115;
constexpr u32 kRegGasLightXY = 0x120, kRegGasLightZ = 0x121, kRegGasLightZColor = 0x122, kRegGasDeltaZ = 0x126;

constexpr u32 kFogModeNone = 0, kFogModeFog = 5, kFogModeGas = 7;
constexpr u32 kOperationGas = 1;

// Float24 (1 sign, 7 exponent, 16 mantissa bits) of a normal float.
u32 F24(float f) {
    if (f == 0.0f) return 0;
    u32 bits;
    std::memcpy(&bits, &f, 4);
    const u32 sign = bits >> 31;
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 63;
    return (sign << 23) | (static_cast<u32>(exponent) << 16) | ((bits >> 7) & 0xFFFF);
}

// A PICA state with stage 0 = the primary colour (the vertex colour), the other stages pass-through, colour writes on, no depth,
// no blending (logic op Copy), no lighting, Z-buffering with a depth scale of -1: a triangle at PICA z gets the depth -z.
std::unique_ptr<pica::PicaState> BaseState() {
    auto state = std::make_unique<pica::PicaState>();
    state->regs.fill(0);
    state->dirty.SetAll();
    // stage 0: colour and alpha source 0 (the primary colour), Replace; the other sources Previous (15)
    state->regs[kRegTev0 + 0] = 0x0 | (15u << 4) | (15u << 8) | (0x0u << 16) | (15u << 20) | (15u << 24);
    state->regs[kRegTev0 + 1] = 0;
    state->regs[kRegTev0 + 2] = 0;
    for (u32 base : {0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u}) {
        state->regs[base + 0] = 15u | (15u << 4) | (15u << 8) | (15u << 16) | (15u << 20) | (15u << 24);
        state->regs[base + 1] = 0;
        state->regs[base + 2] = 0;
    }
    state->regs[kRegLogicOp] = static_cast<u32>(re::LogicOp::Copy);
    state->regs[kRegDepthColorMask] = 0xF00;  // red, green, blue, alpha writes
    state->regs[kRegAllowColor] = 0xF;
    state->regs[kRegAllowDepth] = 0;
    state->regs[0x1C6] = 1;  // lighting disabled
    state->regs[kRegDepthRange] = F24(-1.0f);
    state->regs[kRegDepthNear] = 0;
    state->regs[kRegDepthmap] = 1;  // Z-buffering
    return state;
}

// ---- the rig (adapted from gpu_tev_test.cpp) ------------------------------------------------------------------------------

using cpuvertex::OutputVertex;

OutputVertex Vertex(float x, float y, float z, float w, std::array<float, 4> color) {
    OutputVertex v{};
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = z; v.pos[3] = w;
    v.quat[0] = 0; v.quat[1] = 0; v.quat[2] = 0; v.quat[3] = 1;
    for (int i = 0; i < 4; ++i) v.color[i] = color[i];
    v.tc0_w = 1.0f;
    return v;
}

float B(int byte) { return static_cast<float>(byte) / 255.0f; }

struct Tri {
    OutputVertex v[3];
};
// One triangle that covers the whole target, with a constant colour (bytes), a PICA z and a w.
Tri FullTriangle(std::array<int, 4> color, float z = -0.5f, float w = 1.0f) {
    const std::array<float, 4> c = {B(color[0]), B(color[1]), B(color[2]), B(color[3])};
    return Tri{{Vertex(-1 * w, -1 * w, z * w, w, c), Vertex(3 * w, -1 * w, z * w, w, c), Vertex(-1 * w, 3 * w, z * w, w, c)}};
}

native::VertexInputKey InputKey() {
    const rhi::VertexLayout layout = cpuvertex::MakeBatchVertexLayout();
    native::VertexInputKey key;
    key.binding_count = static_cast<u8>(layout.bindings.size());
    for (size_t i = 0; i < layout.bindings.size(); ++i) key.bindings[i].stride = layout.bindings[i].stride;
    key.attribute_count = static_cast<u8>(layout.attributes.size());
    for (size_t i = 0; i < layout.attributes.size(); ++i) {
        key.attributes[i] = {static_cast<u8>(layout.attributes[i].location), static_cast<u8>(layout.attributes[i].binding),
                             layout.attributes[i].format, layout.attributes[i].offset};
    }
    return key;
}

// One draw: the registers it runs with, and the triangle.
struct DrawSpec {
    std::shared_ptr<pica::PicaState> state;
    Tri tri = FullTriangle({255, 255, 255, 255});
};

// A draw with the base state changed by `edit`.
template <typename Edit>
DrawSpec Draw(Tri tri, Edit&& edit) {
    std::shared_ptr<pica::PicaState> state = BaseState();
    edit(*state);
    DrawSpec spec;
    spec.state = std::move(state);
    spec.tri = tri;
    return spec;
}

struct Rig {
    std::shared_ptr<rhi::Backend> backend;
    DeviceProfile profile;
    std::unique_ptr<native::PipelineBuilder> builder;
    std::unique_ptr<native::DefaultPipelineProvider> provider;
    std::unique_ptr<native::DrawComposer> composer;
    std::unique_ptr<FragmentUniformRing> uniforms;
    std::unique_ptr<TableRings> tables;
    std::unique_ptr<rhi::Ring> vertex_ring, vs_ring;
    std::shared_ptr<rhi::Image> color;
    native::DrawResult last;
    u64 dst_copies = 0;
    u64 draws = 0;

    Rig(std::shared_ptr<rhi::Backend> b, bool force_uber) : backend(std::move(b)) {
        profile = ProfileOf(backend->Caps());
        builder = std::make_unique<native::PipelineBuilder>(backend, FeatureRegistry::Global());
        provider = std::make_unique<native::DefaultPipelineProvider>(
            [this](const native::PipelineKey& key) { return builder->Build(key); }, native::DefaultProviderOptions{force_uber});
        auto made = native::DrawComposer::Create(backend, *provider);
        REQUIRE_MESSAGE(made.ok(), made.error());
        composer = std::move(*made);
        auto ring = FragmentUniformRing::Create(*backend);
        REQUIRE_MESSAGE(ring.ok(), ring.error());
        uniforms = std::move(*ring);
        auto table_rings = TableRings::Create(*backend);
        REQUIRE_MESSAGE(table_rings.ok(), table_rings.error());
        tables = std::move(*table_rings);
        auto vr = backend->CreateRing(1 << 20, rhi::BufferUsage::Vertex, "fog vertices");
        auto sr = backend->CreateRing(1 << 20, rhi::BufferUsage::Uniform, "fog vs uniforms");
        REQUIRE(vr.ok());
        REQUIRE(sr.ok());
        vertex_ring = std::move(*vr);
        vs_ring = std::move(*sr);
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = desc.height = kSize;
        desc.name = "fog colour";
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        color = *image;
    }

    // Clears the target to `clear` (bytes), runs the draws in one recorder through FragmentState (the real path: the key from the
    // registers, the uniforms, the tables), and returns the colour pixels. One FragmentState for the whole call, so the second
    // draw sees what the first left in the block.
    std::vector<u8> Render(const std::vector<DrawSpec>& specs, std::array<int, 4> clear = {0, 0, 0, 0}) {
        rhi::Recorder rec = backend->BeginRecording();
        composer->BeginRecording(rec);
        uniforms->BeginRecording();
        FragmentState fragment(profile, FeatureRegistry::Global());
        bool first = true;
        for (const DrawSpec& spec : specs) {
            auto prepared = fragment.Prepare(*spec.state, {}, tables.get());
            REQUIRE_MESSAGE(prepared.ok(), prepared.error());
            auto alloc = uniforms->Write(fragment.block(), prepared->block_changed);
            REQUIRE_MESSAGE(alloc.ok(), alloc.error());

            cpuvertex::TriangleBatch batch;
            batch.AddTriangle(spec.tri.v[0], spec.tri.v[1], spec.tri.v[2]);
            auto streamed = cpuvertex::UploadBatch(*vertex_ring, batch);
            REQUIRE_MESSAGE(streamed.ok(), streamed.error());
            auto vs = vs_ring->Map(2048, std::max<u64>(256, backend->Caps().uniform_buffer_offset_alignment));
            REQUIRE_MESSAGE(vs.ok(), vs.error());
            std::memset(vs->data.data(), 0, 2048);
            REQUIRE(vs_ring->Commit(2048).ok());

            rhi::TargetPassDesc pass;
            pass.color = color.get();
            pass.area = rhi::Rect{0, 0, kSize, kSize};
            const fbops::DynamicState dynamic = fbops::MakeDynamicState(*spec.state);
            if (first) {
                rhi::TargetPassDesc clear_pass = pass;
                clear_pass.color_load = rhi::LoadOp::Clear;
                clear_pass.clear_color = {B(clear[0]), B(clear[1]), B(clear[2]), B(clear[3])};
                clear_pass.depth_load = rhi::LoadOp::Clear;
                clear_pass.clear_depth = 1.0f;
                composer->BeginTargetPass(rec, clear_pass);
                fbops::ApplyDynamicState(rec, dynamic, false);
                first = false;
            }
            const FragmentKey key = fragment.key();
            native::DrawDesc d;
            d.vertex.kind = native::VertexStageKind::Trivial;
            d.vertex.input = InputKey();
            d.fragment = &key;
            d.needs = fragment.needs();
            d.state = fbops::MakeFixedFunctionState(*spec.state);
            d.color_format = rhi::Format::Rgba8Unorm;
            d.depth_format = rhi::Format::Undefined;
            d.target = pass;
            d.vs_pica_uniforms = native::UniformRange{vs_ring->GetBuffer().get(), vs->offset, 1616};
            d.vs_uniforms = native::UniformRange{vs_ring->GetBuffer().get(), vs->offset + 1792, 32};
            d.fragment_uniforms = *alloc;
            d.tables = tables.get();
            const rhi::VertexBufferBinding binding{vertex_ring->GetBuffer().get(), streamed->offset};
            d.vertex_buffers = std::span<const rhi::VertexBufferBinding>(&binding, 1);
            d.count = streamed->vertex_count;
            d.viewport = rhi::Viewport{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
            d.scissor = rhi::Rect{0, 0, kSize, kSize};
            auto result = composer->Draw(rec, d);
            REQUIRE_MESSAGE(result.ok(), result.error());
            last = *result;
            dst_copies += result->dst_copied ? 1 : 0;
            ++draws;
        }
        backend->WaitIdle(backend->Submit(std::move(rec)));
        auto pixels = backend->ReadImage(*color, 0, 0, kSize, kSize);
        REQUIRE_MESSAGE(pixels.ok(), pixels.error());
        return *pixels;
    }
};

std::array<int, 4> Pixel(const std::vector<u8>& pixels, u32 x, u32 y) {
    const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
    return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
}

// True when every pixel is within `tolerance` of `want` (bytes) in every channel.
bool AllNear(const std::vector<u8>& pixels, std::array<int, 4> want, int tolerance, std::string* why = nullptr) {
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const std::array<int, 4> p = Pixel(pixels, x, y);
            for (int c = 0; c < 4; ++c) {
                if (std::abs(p[c] - want[c]) > tolerance) {
                    if (why != nullptr) {
                        *why = "pixel (" + std::to_string(x) + "," + std::to_string(y) + ") channel " + std::to_string(c) + " is " +
                               std::to_string(p[c]) + ", expected " + std::to_string(want[c]) + " +- " + std::to_string(tolerance);
                    }
                    return false;
                }
            }
        }
    }
    return true;
}

// The specialised shader and the ubershader are one source in two forms: their pixels agree within one step per channel (they are
// identical on lavapipe).
bool SameWithin(const std::vector<u8>& a, const std::vector<u8>& b, int tolerance = 1) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > tolerance) return false;
    }
    return true;
}

// ---- fog: the software renderer's formulas ----------------------------------------------------------------------------------

// A fog table entry as the register delivers it: Fog::LutEntry, difference bits 0-12 (signed), value bits 13-23.
u32 FogEntry(int value, int difference) {
    return (static_cast<u32>(value) << 13) | (static_cast<u32>(difference) & 0x1FFF);
}

// The table of the cases: value[i] = (53 i) mod 2048, the difference the step to the next value (the last entry 100), so the factor
// is continuous in the depth, rises and falls, and leaves [0, 1] for the clamp to act.
std::array<u32, 128> FogTable(int stride) {
    std::array<u32, 128> table{};
    for (int i = 0; i < 128; ++i) {
        const int value = (stride * i) % 2048;
        const int next = (stride * (i + 1)) % 2048;
        table[i] = FogEntry(value, i == 127 ? 100 : next - value);
    }
    return table;
}

// sw_rasterizer.cpp's depth: z over w times the depth range plus the near plane, times w with W-buffering, clamped.
float SwDepth(float z_over_w, float w, float scale, float offset, bool w_buffering) {
    float depth = z_over_w * scale + offset;
    if (w_buffering) depth *= w;
    return std::clamp(depth, 0.0f, 1.0f);
}

// sw_rasterizer.cpp's WriteFog on one pixel (the rgb bytes; alpha is untouched).
std::array<int, 4> SwFog(std::array<int, 4> pixel, const std::array<u32, 128>& table, std::array<int, 3> fog_color, bool flip,
                         float depth) {
    const float fog_index = flip ? (1.0f - depth) * 128.0f : depth * 128.0f;
    const float fog_i = std::clamp(std::floor(fog_index), 0.0f, 127.0f);
    const float fog_f = fog_index - fog_i;
    const u32 entry = table[static_cast<u32>(fog_i)];
    const int difference = (static_cast<int>((entry & 0x1FFF) << 19)) >> 19;  // 13 bits, signed
    float fog_factor = static_cast<float>((entry >> 13) & 0x7FF) / 2047.0f + (static_cast<float>(difference) / 2047.0f) * fog_f;
    fog_factor = std::clamp(fog_factor, 0.0f, 1.0f);
    std::array<int, 4> out = pixel;
    for (int i = 0; i < 3; ++i) {
        out[i] = static_cast<u8>(fog_factor * static_cast<float>(pixel[i]) + (1.0f - fog_factor) * static_cast<float>(fog_color[i]));
    }
    return out;
}

struct FogCase {
    const char* name;
    float z;           // PICA z of the triangle, in [-1, 0]
    float w;
    bool flip;
    bool w_buffering;
};

DrawSpec FogDraw(const FogCase& c, const std::array<u32, 128>& table, std::array<int, 3> fog_color, std::array<int, 4> pixel) {
    return Draw(FullTriangle(pixel, c.z, c.w), [&](pica::PicaState& s) {
        s.regs[kRegFogMode] = kFogModeFog | (c.flip ? (1u << 16) : 0u);
        s.regs[kRegFogColor] = static_cast<u32>(fog_color[0]) | (static_cast<u32>(fog_color[1]) << 8) | (static_cast<u32>(fog_color[2]) << 16);
        s.regs[kRegDepthmap] = c.w_buffering ? 0 : 1;
        for (u32 i = 0; i < 128; ++i) s.fog.lut[i] = table[i];
        s.fog.lut_dirty = true;
    });
}

}  // namespace

// ---- without a device ---------------------------------------------------------------------------------------------------

TEST_CASE("fog/the_gas_key_word_is_canonical_and_the_stubs_are_gone") {
    auto key_of = [](auto&& edit) {
        auto state = BaseState();
        edit(*state);
        return MakeKey(*state);
    };
    // nothing gas: the word is zero, whatever the other gas registers hold
    CHECK(key_of([](pica::PicaState&) {}).gas.raw == 0);
    CHECK(key_of([](pica::PicaState& s) {
              s.regs[kRegGasLightZColor] = 0x1FF;
              s.regs[kRegGasAccMax] = 0x4400;
              s.regs[kRegGasAttenuation] = 0x3C00;
          }).gas.raw == 0);
    // fog mode Fog: zero
    CHECK(key_of([](pica::PicaState& s) { s.regs[kRegFogMode] = kFogModeFog; }).gas.raw == 0);
    // the fragment operation mode Gas: the density pass (bit 0)
    CHECK(key_of([](pica::PicaState& s) { s.regs[kRegOutputMerger] = kOperationGas; }).gas.raw == fog::kKeyDensityPass);
    // the light-factor input is read only by the shading pass
    CHECK(key_of([](pica::PicaState& s) { s.regs[kRegGasLightZColor] = 0x100; }).gas.raw == 0);
    CHECK(key_of([](pica::PicaState& s) { s.regs[kRegFogMode] = kFogModeGas; }).gas.raw == 0);
    CHECK(key_of([](pica::PicaState& s) {
              s.regs[kRegFogMode] = kFogModeGas;
              s.regs[kRegGasLightZColor] = 0x100;
          }).gas.raw == fog::kKeyLightFactorInput);
    CHECK(key_of([](pica::PicaState& s) {
              s.regs[kRegFogMode] = kFogModeGas;
              s.regs[kRegGasLightZColor] = 0x0FF;  // only the line-of-sight byte: not the LUT input
          }).gas.raw == 0);
    // both modes: the density pass wins and the word holds its bit alone
    CHECK(key_of([](pica::PicaState& s) {
              s.regs[kRegOutputMerger] = kOperationGas;
              s.regs[kRegFogMode] = kFogModeGas;
              s.regs[kRegGasLightZColor] = 0x100;
          }).gas.raw == fog::kKeyDensityPass);

    // no feature is a stub any more: neither fog nor gas is reported
    std::array<const char*, 8> names{};
    FragmentKey fog;
    fog.texture.set_fog_mode(re::FogMode::Fog);
    CHECK(StubbedFeatures(fog, FeatureRegistry::Global(), names) == 0);
    fog.texture.set_fog_mode(re::FogMode::Gas);
    CHECK(StubbedFeatures(fog, FeatureRegistry::Global(), names) == 0);
    fog.gas.raw = fog::kKeyDensityPass;
    CHECK(StubbedFeatures(fog, FeatureRegistry::Global(), names) == 0);
}

TEST_CASE("fog/the_needs_are_the_table_for_fog_and_the_feedback_copy_for_both_gas_passes") {
    const DeviceProfile profile = DeviceProfile{};
    auto needs_of = [&](auto&& edit) {
        auto state = BaseState();
        edit(*state);
        return NeedsOf(MakeKey(*state), profile);
    };
    const Needs none = needs_of([](pica::PicaState&) {});
    CHECK_FALSE(none.dst_copy);
    CHECK_FALSE(none.lut_lf);
    CHECK_FALSE(none.ordered_rmw);
    const Needs fog = needs_of([](pica::PicaState& s) { s.regs[kRegFogMode] = kFogModeFog; });
    CHECK(fog.lut_lf);
    CHECK_FALSE(fog.dst_copy);
    const Needs shading = needs_of([](pica::PicaState& s) { s.regs[kRegFogMode] = kFogModeGas; });
    CHECK(shading.dst_copy);
    CHECK_FALSE(shading.lut_lf);
    const Needs density = needs_of([](pica::PicaState& s) { s.regs[kRegOutputMerger] = kOperationGas; });
    CHECK(density.dst_copy);
    // the ordered read-modify-write is not asked for (gas.md: the limitation is open)
    CHECK_FALSE(shading.ordered_rmw);
    CHECK_FALSE(density.ordered_rmw);
}

TEST_CASE("fog/the_uniforms_follow_the_registers_and_the_gas_region_holds_the_documented_layout") {
    auto state = BaseState();
    state->regs[kRegFogColor] = 0x00C0'80'40;  // r 0x40, g 0x80, b 0xC0
    state->regs[kRegGasLightXY] = 0x00112233;
    state->regs[kRegGasLightZ] = 0x00445566;
    state->regs[kRegGasLightZColor] = 0x100 | 0x7F;
    state->regs[0x123] = 5;  // GAS_LUT_INDEX
    state->regs[0x125] = 0x4400'3C00;
    state->regs[kRegGasDeltaZ] = 0x0300'0100;
    state->regs[kRegFogMode] = kFogModeGas | 8;
    state->regs[kRegGasAttenuation] = 0x3800;
    state->regs[kRegGasAccMax] = 0x4400;
    for (u32 i = 0; i < pica::Gas::kEntries; ++i) state->gas.lut[i] = 0x00010203u * (i + 1);

    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    UniformDirty dirty;
    dirty.all = true;
    FillUniforms(*state, block, dirty, {});
    CHECK(dirty.changed);

    // the fog colour: bytes over 255 (SyncDrawUniforms)
    CHECK(block.uniforms.fog_color.x == doctest::Approx(64.0f / 255.0f));
    CHECK(block.uniforms.fog_color.y == doctest::Approx(128.0f / 255.0f));
    CHECK(block.uniforms.fog_color.z == doctest::Approx(192.0f / 255.0f));

    // the gas registers in the words fog_layout.h gives them
    CHECK(block.gas.regs[fog::kRegLightXY] == 0x00112233);
    CHECK(block.gas.regs[fog::kRegLightZ] == 0x00445566);
    CHECK(block.gas.regs[fog::kRegLightZColor] == 0x17F);
    CHECK(block.gas.regs[fog::kRegLutIndex] == 5);
    CHECK(block.gas.regs[fog::kRegAccMaxFeedback] == 0x4400'3C00);
    CHECK(block.gas.regs[fog::kRegDeltaZDepth] == 0x0300'0100);
    CHECK(block.gas.regs[fog::kRegFogMode] == (kFogModeGas | 8));
    CHECK(block.gas.regs[fog::kRegAttenuation] == 0x3800);
    CHECK(block.gas.regs[fog::kRegAccMax] == 0x4400);
    for (u32 i = fog::kRegWordsUsed; i < 16; ++i) CHECK(block.gas.regs[i] == 0);
    for (u32 i = 0; i < 16; ++i) CHECK(block.gas.table[i] == 0x00010203u * (i + 1));

    // nothing dirty: nothing written, nothing marked
    state->ClearDirty();
    UniformDirty clean;
    FillUniforms(*state, block, clean, {});
    CHECK_FALSE(clean.changed);

    // a gas register changes: the region follows and the block is marked; the same value written again marks nothing
    state->regs[kRegGasAccMax] = 0x4800;
    state->dirty.Set(kRegGasAccMax);
    UniformDirty changed;
    FillUniforms(*state, block, changed, {});
    CHECK(changed.changed);
    CHECK(block.gas.regs[fog::kRegAccMax] == 0x4800);
    state->ClearDirty();
    state->dirty.Set(kRegGasAccMax);
    UniformDirty same;
    FillUniforms(*state, block, same, {});
    CHECK_FALSE(same.changed);

    // a table word arrives (the dirty flag of N2-38): the region follows
    state->ClearDirty();
    state->gas.lut[9] = 0x00ABCDEF;
    state->gas.table_dirty = true;
    UniformDirty table;
    FillUniforms(*state, block, table, {});
    CHECK(table.changed);
    CHECK(block.gas.table[9] == 0x00ABCDEF);

    // the fog colour register changes
    state->ClearDirty();
    state->regs[kRegFogColor] = 0x00FF'00'00;
    state->dirty.Set(kRegFogColor);
    UniformDirty colour;
    FillUniforms(*state, block, colour, {});
    CHECK(colour.changed);
    CHECK(block.uniforms.fog_color.x == doctest::Approx(0.0f));
    CHECK(block.uniforms.fog_color.z == doctest::Approx(1.0f));
}

TEST_CASE("fog/the_shaders_of_every_mode_compile_in_both_forms_and_a_key_without_gas_carries_no_gas_helper") {
    DeviceProfile profile;
    profile.logic_op = true;
    auto key_with = [](auto&& edit) {
        auto state = BaseState();
        edit(*state);
        return MakeKey(*state);
    };
    const FragmentKey plain = key_with([](pica::PicaState&) {});
    const FragmentKey fog = key_with([](pica::PicaState& s) { s.regs[kRegFogMode] = kFogModeFog | (1u << 16); });
    const FragmentKey shading = key_with([](pica::PicaState& s) { s.regs[kRegFogMode] = kFogModeGas; });
    const FragmentKey light = key_with([](pica::PicaState& s) {
        s.regs[kRegFogMode] = kFogModeGas;
        s.regs[kRegGasLightZColor] = 0x100;
    });
    const FragmentKey density = key_with([](pica::PicaState& s) { s.regs[kRegOutputMerger] = kOperationGas; });
    for (const FragmentKey* key : {&plain, &fog, &shading, &light, &density}) {
        const Generated g = GenerateSpecialised(*key, profile, ShaderPass::Normal);
        CHECK_MESSAGE(g.ok(), g.error);
        const Generated shadow = GenerateSpecialised(*key, profile, ShaderPass::ShadowWrite);
        CHECK_MESSAGE(shadow.ok(), shadow.error);
    }
    const Generated uber = GenerateUber(ShaderPass::Normal, profile);
    CHECK_MESSAGE(uber.ok(), uber.error);
    CHECK(uber.glsl.find("mh4n_gas_color") != std::string::npos);
    CHECK(uber.glsl.find("texture_buffer_lut_lf, int(fog_i)") != std::string::npos);

    // the specialised shader of a key that uses no gas does not carry the gas helpers, and one that does, does
    CHECK(GenerateSpecialisedGlsl(plain, profile, ShaderPass::Normal).find("mh4n_gas_color") == std::string::npos);
    CHECK(GenerateSpecialisedGlsl(fog, profile, ShaderPass::Normal).find("mh4n_gas_color") == std::string::npos);
    CHECK(GenerateSpecialisedGlsl(shading, profile, ShaderPass::Normal).find("mh4n_gas_color(gas_t)") != std::string::npos);
    CHECK(GenerateSpecialisedGlsl(density, profile, ShaderPass::Normal).find("packHalf2x16") != std::string::npos);
    // the specialised shader of the plain key reads no fog table
    CHECK(GenerateSpecialisedGlsl(plain, profile, ShaderPass::Normal).find("fog_lut_offset)") == std::string::npos);
    CHECK(GenerateSpecialisedGlsl(fog, profile, ShaderPass::Normal).find("(1.0 - fog_depth) * 128.0") != std::string::npos);
}

// ---- fog on a device -----------------------------------------------------------------------------------------------------

TEST_CASE("fog/depth_fog_with_its_table_matches_the_software_renderers_formulas_in_both_modes") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    const std::array<u32, 128> table = FogTable(53);
    const std::array<int, 3> fog_color = {200, 40, 90};
    const std::array<int, 4> pixel = {20, 120, 220, 77};

    const std::vector<FogCase> cases = {
        {"depth 0.5, index 64.0", -0.5f, 1.0f, false, false},
        {"depth 0.3, index 38.4", -0.3f, 1.0f, false, false},
        {"depth 0.9, flipped", -0.9f, 1.0f, true, false},
        {"depth 0.25, flipped", -0.25f, 1.0f, true, false},
        {"depth 0.0", 0.0f, 1.0f, false, false},
        {"depth 1.0, index 128: the last segment at its end", -1.0f, 1.0f, false, false},
        {"depth 1.0, flipped: index 0", -1.0f, 1.0f, true, false},
        {"W-buffering, w 2: depth 0.1 * 2", -0.1f, 2.0f, false, true},
        {"W-buffering clamps at 1: depth 0.7 * 2", -0.7f, 2.0f, false, true},
        {"W-buffering, flipped, w 4: depth 0.05 * 4", -0.05f, 4.0f, true, true},
        {"W-buffering, flipped, w 4: depth 0.3 * 4 clamped", -0.3f, 4.0f, true, true},
        {"Z-buffering ignores w", -0.6f, 3.0f, false, false},
    };
    for (const FogCase& c : cases) {
        INFO(c.name);
        const float depth = SwDepth(c.z, c.w, -1.0f, 0.0f, c.w_buffering);
        // the depth is -z, so z over w is c.z for the triangle of FullTriangle, scale -1: SwDepth takes the PICA z over w
        const std::array<int, 4> want = SwFog(pixel, table, fog_color, c.flip, depth);
        const DrawSpec spec = FogDraw(c, table, fog_color, pixel);
        std::string why;
        const std::vector<u8> a = spec_rig.Render({spec});
        const std::vector<u8> b = uber_rig.Render({spec});
        CHECK_MESSAGE(AllNear(a, want, 1, &why), why);
        CHECK_MESSAGE(AllNear(b, want, 1, &why), why);
        CHECK(SameWithin(a, b));
        CHECK(Pixel(a, 3, 3)[3] == 77);  // the alpha is not fogged
        CHECK_FALSE(spec_rig.last.dst_copied);
    }
}

TEST_CASE("fog/fog_mode_none_leaves_the_pixel_and_the_table_is_uploaded_again_when_it_changes") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    const std::array<int, 3> fog_color = {10, 250, 10};
    const std::array<int, 4> pixel = {100, 100, 100, 255};
    for (Rig* rig : {&spec_rig, &uber_rig}) {
        std::string why;
        // fog mode None with a fog table and colour present: the pixel as the TEV made it
        DrawSpec none = FogDraw({"", -0.5f, 1.0f, false, false}, FogTable(53), fog_color, pixel);
        none.state->regs[kRegFogMode] = kFogModeNone;
        CHECK_MESSAGE(AllNear(rig->Render({none}), pixel, 0, &why), why);

        // each table alone, then two draws in one recording: the second draw uploads its own table (the table is dirty) and the
        // pixels are the second table's
        const std::array<u32, 128> a = FogTable(53), b = FogTable(211);
        const FogCase c{"", -0.3f, 1.0f, false, false};
        const float depth = SwDepth(c.z, 1.0f, -1.0f, 0.0f, false);
        const std::array<int, 4> want_a = SwFog(pixel, a, fog_color, false, depth);
        const std::array<int, 4> want_b = SwFog(pixel, b, fog_color, false, depth);
        CHECK(want_a != want_b);  // the two tables must give different pixels, or the case proves nothing
        CHECK_MESSAGE(AllNear(rig->Render({FogDraw(c, a, fog_color, pixel)}), want_a, 1, &why), why);
        CHECK_MESSAGE(AllNear(rig->Render({FogDraw(c, b, fog_color, pixel)}), want_b, 1, &why), why);
        CHECK_MESSAGE(AllNear(rig->Render({FogDraw(c, a, fog_color, pixel), FogDraw(c, b, fog_color, pixel)}), want_b, 1, &why), why);
        // another fog colour in the second draw of one recording
        DrawSpec second = FogDraw(c, b, {0, 0, 0}, pixel);
        const std::array<int, 4> want_black = SwFog(pixel, b, {0, 0, 0}, false, depth);
        CHECK_MESSAGE(AllNear(rig->Render({FogDraw(c, b, fog_color, pixel), second}), want_black, 1, &why), why);
    }
}

// ---- gas -----------------------------------------------------------------------------------------------------------------

namespace {

// float1.5.10 as IEEE binary16 [unverified, fog.cpp D5]; exact conversions for the values the cases use (normal numbers and zero).
float HalfToFloat(u32 bits) {
    const u32 sign = (bits >> 15) & 1, exponent = (bits >> 10) & 31, mantissa = bits & 1023;
    float magnitude;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<float>(mantissa), -24);
    } else {
        magnitude = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
    }
    return sign != 0 ? -magnitude : magnitude;
}
u32 FloatToHalfExact(float value) {  // the value must be representable: the check is part of the model
    if (value == 0.0f) return 0;
    int exponent;
    const float fraction = std::frexp(std::fabs(value), &exponent);  // in [0.5, 1)
    const int e = exponent - 1 + 15;
    const float scaled = (fraction * 2.0f - 1.0f) * 1024.0f;
    REQUIRE(e > 0);
    REQUIRE(e < 31);
    REQUIRE(scaled == std::floor(scaled));
    return (value < 0 ? 0x8000u : 0u) | (static_cast<u32>(e) << 10) | static_cast<u32>(scaled);
}

// The bytes of a density: blue is the high byte and alpha the low byte.
u32 DensityBits(const std::array<int, 4>& pixel) { return (static_cast<u32>(pixel[2]) << 8) | static_cast<u32>(pixel[3]); }

// The density pass (fog.cpp D1 to D6) on one pixel: `fragment` is the TEV output, `dest` the colour buffer before the draw.
std::array<int, 4> ModelDensity(std::array<int, 4> fragment, std::array<int, 4> dest) {
    const float sum = HalfToFloat(DensityBits(fragment)) + HalfToFloat(DensityBits(dest));
    const u32 bits = FloatToHalfExact(sum);
    return {fragment[0], fragment[1], static_cast<int>(bits >> 8), static_cast<int>(bits & 255)};
}

// The gas table of the cases (entry words as the data port received them: bytes red, green, blue from bit 0; entries 0-7 are
// signed differences, entries 8-15 unsigned colours).
u32 Rgb(int r, int g, int b) { return (static_cast<u32>(r) & 255) | ((static_cast<u32>(g) & 255) << 8) | ((static_cast<u32>(b) & 255) << 16); }
std::array<u32, 16> GasTable() {
    std::array<u32, 16> t{};
    t[3] = Rgb(20, -10, 0);       // difference of segment 3
    t[7] = Rgb(10, -10, 100);     // difference of segment 7
    t[8 + 0] = Rgb(1, 2, 3);      // value of segment 0
    t[8 + 2] = Rgb(40, 50, 60);   // value of segment 2
    t[8 + 3] = Rgb(100, 200, 50); // value of segment 3
    t[8 + 7] = Rgb(250, 5, 0);    // value of segment 7
    return t;
}

constexpr u32 kAccMax4 = 0x4400;  // 4.0
constexpr int kShadingAlpha = 77;

DrawSpec DensityDraw(std::array<int, 4> fragment) {
    return Draw(FullTriangle(fragment), [](pica::PicaState& s) { s.regs[kRegOutputMerger] = kOperationGas; });
}

DrawSpec ShadingDraw(u32 acc_max, bool light_factor_input = false) {
    return Draw(FullTriangle({9, 9, 9, kShadingAlpha}), [&](pica::PicaState& s) {
        s.regs[kRegFogMode] = kFogModeGas;
        s.regs[kRegGasAccMax] = acc_max;
        if (light_factor_input) s.regs[kRegGasLightZColor] = 0x100;
        const std::array<u32, 16> table = GasTable();
        for (u32 i = 0; i < 16; ++i) s.gas.lut[i] = table[i];
        s.gas.table_dirty = true;
    });
}

}  // namespace

TEST_CASE("gas/the_model_helpers_agree_with_the_binary16_values_gas_md_uses") {
    CHECK(HalfToFloat(0x3C00) == 1.0f);
    CHECK(HalfToFloat(0x3800) == 0.5f);
    CHECK(HalfToFloat(0x3400) == 0.25f);
    CHECK(HalfToFloat(0x3E00) == 1.5f);
    CHECK(HalfToFloat(0x3F00) == 1.75f);
    CHECK(HalfToFloat(0x4400) == 4.0f);
    CHECK(HalfToFloat(0x3C80) == 1.125f);
    CHECK(HalfToFloat(0x3480) == 0.28125f);
    CHECK(HalfToFloat(0x3DA0) == 1.40625f);
    for (u32 bits : {0x3C00u, 0x3F00u, 0x3DA0u, 0x4400u, 0x0400u}) CHECK(FloatToHalfExact(HalfToFloat(bits)) == bits);
}

TEST_CASE("gas/the_density_pass_accumulates_the_blue_and_alpha_bytes_by_hand_computed_values") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    for (Rig* rig : {&spec_rig, &uber_rig}) {
        std::string why;
        // gas.md, G1: three draws on a buffer cleared to 0. Densities 1.0 (0x3C00), 0.5 (0x3800), 0.25 (0x3400).
        const DrawSpec d1 = DensityDraw({10, 20, 0x3C, 0x00});
        const DrawSpec d2 = DensityDraw({30, 40, 0x38, 0x00});
        const DrawSpec d3 = DensityDraw({50, 60, 0x34, 0x00});
        CHECK_MESSAGE(AllNear(rig->Render({d1}), {10, 20, 0x3C, 0x00}, 0, &why), why);              // 0 + 1.0 = 1.0
        CHECK_MESSAGE(AllNear(rig->Render({d1, d2}), {30, 40, 0x3E, 0x00}, 0, &why), why);          // 1.0 + 0.5 = 1.5 = 0x3E00
        CHECK_MESSAGE(AllNear(rig->Render({d1, d2, d3}), {50, 60, 0x3F, 0x00}, 0, &why), why);      // 1.5 + 0.25 = 1.75 = 0x3F00
        CHECK(rig->last.dst_copied);

        // G2: a density with a fractional part in the alpha byte: 1.125 (0x3C80) + 0.28125 (0x3480) = 1.40625 = 0x3DA0
        CHECK_MESSAGE(AllNear(rig->Render({DensityDraw({1, 2, 0x34, 0x80})}, {7, 8, 0x3C, 0x80}), {1, 2, 0x3D, 0xA0}, 0, &why), why);

        // G3: zero and zero stay zero; red and green are the TEV output's
        CHECK_MESSAGE(AllNear(rig->Render({DensityDraw({3, 4, 0, 0})}, {9, 9, 0, 0}), {3, 4, 0, 0}, 0, &why), why);

        // the CPU model of the same steps agrees on the cases that vary a value
        for (const std::array<int, 4>& dest : {std::array<int, 4>{0, 0, 0x3C, 0x00}, {5, 5, 0x34, 0x80}, {1, 1, 0x40, 0x00}}) {
            for (const std::array<int, 4>& fragment : {std::array<int, 4>{70, 80, 0x38, 0x00}, {71, 81, 0x3C, 0x40}, {72, 82, 0x20, 0x00}}) {
                CHECK_MESSAGE(AllNear(rig->Render({DensityDraw(fragment)}, dest), ModelDensity(fragment, dest), 0, &why), why);
            }
        }
    }
    // the two forms agree draw by draw
    const std::vector<DrawSpec> three = {DensityDraw({10, 20, 0x3C, 0x00}), DensityDraw({30, 40, 0x38, 0x00}), DensityDraw({50, 60, 0x34, 0x00})};
    CHECK(SameWithin(spec_rig.Render(three), uber_rig.Render(three), 0));
}

TEST_CASE("gas/the_shading_pass_reads_the_density_normalises_it_by_accmax_and_looks_the_table_up") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    struct Case {
        const char* name;
        std::array<int, 4> dest;  // the colour buffer before the shading draw: the density in blue (high) and alpha (low)
        u32 acc_max;
        bool light_factor;
        std::array<int, 4> want;
    };
    // gas.md, H1 to H9 (the table is GasTable(); ACCMAX 4.0 = 0x4400 unless stated; the output alpha is the TEV's, 77)
    const std::vector<Case> cases = {
        {"H1 density 1.75: t = 0.4375, position 3.5, segment 3, fraction 0.5", {1, 1, 0x3F, 0x00}, kAccMax4, false, {110, 195, 50, kShadingAlpha}},
        {"H2 density 0: t = 0, segment 0", {5, 5, 0x00, 0x00}, kAccMax4, false, {1, 2, 3, kShadingAlpha}},
        {"H3 density 8.0 above ACCMAX: t = 1, segment 7 fraction 1, clamped", {1, 1, 0x48, 0x00}, kAccMax4, false, {255, 0, 100, kShadingAlpha}},
        {"H4 density equal to ACCMAX: t = 1", {1, 1, 0x44, 0x00}, kAccMax4, false, {255, 0, 100, kShadingAlpha}},
        {"H5 negative density (bits 0xBC00) compares above ACCMAX: t = 1", {1, 1, 0xBC, 0x00}, kAccMax4, false, {255, 0, 100, kShadingAlpha}},
        {"H6 light-factor input: t = 1 (unverified placeholder)", {1, 1, 0x3F, 0x00}, kAccMax4, true, {255, 0, 100, kShadingAlpha}},
        {"H7 density 1.0: t = 0.25, position 2.0 exactly, segment 2", {1, 1, 0x3C, 0x00}, kAccMax4, false, {40, 50, 60, kShadingAlpha}},
        {"H8 ACCMAX 0 and a density above it: t = 1", {1, 1, 0x3C, 0x00}, 0x0000, false, {255, 0, 100, kShadingAlpha}},
        {"H9 ACCMAX NaN (0x7E00): t = 0", {1, 1, 0x3C, 0x00}, 0x7E00, false, {1, 2, 3, kShadingAlpha}},
        {"H10 density +infinity (0x7C00) at or above ACCMAX: t = 1", {1, 1, 0x7C, 0x00}, kAccMax4, false, {255, 0, 100, kShadingAlpha}},
        {"H11 density 0.001 (0x1000 = 2^-11): t = 2^-13, position 2^-10: segment 0 plus 2^-10 of a zero difference", {1, 1, 0x10, 0x00}, kAccMax4, false, {1, 2, 3, kShadingAlpha}},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        std::string why;
        const DrawSpec shading = ShadingDraw(c.acc_max, c.light_factor);
        const std::vector<u8> a = spec_rig.Render({shading}, c.dest);
        const std::vector<u8> b = uber_rig.Render({shading}, c.dest);
        CHECK_MESSAGE(AllNear(a, c.want, 0, &why), why);
        CHECK_MESSAGE(AllNear(b, c.want, 0, &why), why);
        CHECK(SameWithin(a, b, 0));
        CHECK(spec_rig.last.dst_copied);
    }

    // gas.md, G1 then H1 in one recording: three density draws, then the shading draw reads their sum
    for (Rig* rig : {&spec_rig, &uber_rig}) {
        std::string why;
        const std::vector<DrawSpec> frame = {DensityDraw({10, 20, 0x3C, 0x00}), DensityDraw({30, 40, 0x38, 0x00}),
                                             DensityDraw({50, 60, 0x34, 0x00}), ShadingDraw(kAccMax4)};
        CHECK_MESSAGE(AllNear(rig->Render(frame), {110, 195, 50, kShadingAlpha}, 0, &why), why);
    }
}

TEST_CASE("gas/a_draw_without_gas_is_unchanged_and_asks_for_no_feedback_copy") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig spec_rig(backend, false), uber_rig(backend, true);
    for (Rig* rig : {&spec_rig, &uber_rig}) {
        std::string why;
        // gas registers set, but neither mode on: the TEV output is written as it is
        DrawSpec plain = Draw(FullTriangle({11, 22, 33, 44}), [](pica::PicaState& s) {
            s.regs[kRegGasAccMax] = kAccMax4;
            s.regs[kRegGasLightZColor] = 0x100;
            s.regs[kRegGasAttenuation] = 0x3C00;
            s.regs[kRegGasLightXY] = 0x00FFFFFF;
            const std::array<u32, 16> table = GasTable();
            for (u32 i = 0; i < 16; ++i) s.gas.lut[i] = table[i];
        });
        CHECK_MESSAGE(AllNear(rig->Render({plain}, {99, 99, 99, 99}), {11, 22, 33, 44}, 0, &why), why);
        CHECK_FALSE(rig->last.dst_copied);
    }
}
