// Derived from Azahar src/video_core/renderer_software/sw_proctex.cpp (ProcTex, LookupLUT, NoiseRand1D, NoiseRand2D, NoiseCoef,
// GetShiftOffset, ClampCoord, CombineAndMap: the oracle below is a CPU port of them), src/video_core/pica/regs_texturing.h (the
// procedural texture registers and the TEV stage words), src/video_core/pica/pica_core.h (ProcTex::ValueEntry, ColorEntry and
// ColorDifferenceEntry: the entry layouts and the divisions by 4095 and 255), src/video_core/pica_types.h (Float::FromRaw: how a
// half is read), src/video_core/rasterizer_accelerated.cpp and src/video_core/pica/dirty_regs.h (the noise, phase, amplitude and bias
// uniforms and the six registers that dirty them), src/video_core/renderer_vulkan/vk_rasterizer.cpp (SyncAndUploadLUTs: the
// five tables, their order and sizes) and src/common/vector_math.h (BilinearInterp) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: vk_rasterizer.cpp says "Licensed under GPLv3 or any later version"; sw_proctex.cpp, regs_texturing.h,
// pica_core.h, pica_types.h, rasterizer_accelerated.cpp, dirty_regs.h and vector_math.h each say "Licensed under GPLv2 or any later
// version"); the shader generator's text (glsl_fs_shader_gen.cpp, "Licensed under GPLv3 or any later version") is the oracle of the
// mipmap levels, which the software renderer lacks. The rig below (Tri, Vertex, Rig, the register helpers) is adapted from N2-10's
// fog test in this directory (GPL-3.0-or-later; its header names what it takes from Azahar).
// Copyright 2017 Citra Emulator Project (sw_proctex.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, rasterizer_accelerated.cpp, vk_rasterizer.cpp,
//   glsl_fs_shader_gen.cpp)
// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project (dirty_regs.h)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (vector_math.h)
//
// The procedural texture (N2-11), on whatever Vulkan device the machine has (MH4N_VULKAN_DEVICE; a case that needs one skips with a
// message without it and fails under MH4N_REQUIRE_VULKAN=1).
//
// WHERE THE EXPECTED PIXELS COME FROM. `ProcTexModel` below (with its helpers) is a CPU port of the SOFTWARE renderer's ProcTex (float arithmetic, u8
// colours truncated, the colour table looked up with the offset added before the interpolation). Each draw feeds the texture
// coordinates the rasterizer interpolates (a triangle over the whole 8x8 target, so an affine map of the pixel centre) through it, then
// through the TEV layout of the draw. Choices, where the software renderer and Azahar's GLSL (the shader's text, which the game was
// checked against) differ:
//   - the GLSL's ProcTexLookupLUT clamps its result to [0, 1], the software LookupLUT does not: the shader keeps the GLSL. The synthetic
//     tables keep value + difference inside [0, 4095] for every entry, so a lookup never leaves [0, 1] and the choice shows nowhere;
//   - the software renderer truncates its u8 colour, the shader rounds when it writes the colour: every comparison has a tolerance of
//     one step (the fog test's rule), and the layouts that invert a channel (255 - c) keep it;
//   - the software renderer implements no mipmap ("TODO(wwylele): implement mipmap"): its filters 2 to 5 give the level-0 result of the
//     linear / nearest filter. The LOD and the four mipmap filters therefore follow Azahar's GLSL (DefineProcTexSampler): lod =
//     log2(|width * bias| * (duv.x + duv.y)), zero when the bias is zero, clamped to [max(0, lod_min), min(7, lod_max)]; the nearest
//     level is round(lod), the linear pair int(lod) and int(lod) + 1 mixed by fract(lod); a level reads width >> level entries from
//     the level's offset (offsets 0 to 3 from the register, 4 to 7 hardcoded). The model (`GlslColor`) is written from that text and
//     takes the derivatives of the affine coordinates analytically.
// Every one of the 64 pixels of every draw is compared (on lavapipe the interpolated coordinates and the table lookups agree with the
// float model closely enough that no pixel lands on the wrong side of a rounding or table-index boundary; a device whose interpolation
// differs by an ulp could flip one at a discontinuity). Every draw is rendered in the specialised mode and
// in the ubershader, each of them must match the oracle, and they must equal each other (the whole image).
//
// The ubershader runs the procedural texture once per fragment (N2-43): the draws below read Texture3 at a TEV stage other than 0 and
// through alpha operands as well (`Tev`), so that a sample that the ubershader lost would show. A CPU case counts the call sites of the
// heavy helpers in the ubershader's text; another checks that the ubershader rig really ran the fallback.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gpu/cpuvertex/cpu_vertex.h"
#include "gpu/cpuvertex/triangle_batch.h"
#include "gpu/fbops/fixed_function.h"
#include "gpu/fragment/feature.h"
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
constexpr u32 kRegMainConfig = 0x80;
constexpr u32 kRegProcTex = 0xA8, kRegNoiseU = 0xA9, kRegNoiseV = 0xAA, kRegNoiseFrequency = 0xAB, kRegLut = 0xAC, kRegLutOffset = 0xAD;
constexpr u32 kTevStageBase[6] = {0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8};
constexpr u32 kRegLogicOp = 0x102, kRegDepthColorMask = 0x107, kRegAllowColor = 0x113, kRegAllowDepth = 0x115;

// Float24 (1 sign, 7 exponent, 16 mantissa bits) of a normal float.
u32 F24(float f) {
    if (f == 0.0f) return 0;
    u32 bits;
    std::memcpy(&bits, &f, 4);
    const u32 sign = bits >> 31;
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 63;
    return (sign << 23) | (static_cast<u32>(exponent) << 16) | ((bits >> 7) & 0xFFFF);
}

// The TEV source values of Azahar's TevStageConfig::Source.
constexpr u32 kPrimary = 0, kTexture3 = 6, kPrevious = 15;

// A pass-through stage: every source Previous, Replace, scale 1.
void PassThrough(pica::PicaState& s, u32 stage) {
    const u32 base = kTevStageBase[stage];
    s.regs[base + 0] = kPrevious | (kPrevious << 4) | (kPrevious << 8) | (kPrevious << 16) | (kPrevious << 20) | (kPrevious << 24);
    s.regs[base + 1] = 0;
    s.regs[base + 2] = 0;
    s.regs[base + 4] = 0;
}

// A PICA state with stage 0 = the primary colour (the vertex colour), the other stages pass-through, colour writes on, no depth, no
// blending (logic op Copy), no lighting, Z-buffering.
std::unique_ptr<pica::PicaState> BaseState() {
    auto state = std::make_unique<pica::PicaState>();
    state->regs.fill(0);
    state->dirty.SetAll();
    state->regs[kTevStageBase[0] + 0] = kPrimary | (kPrevious << 4) | (kPrevious << 8) | (kPrimary << 16) | (kPrevious << 20) | (kPrevious << 24);
    state->regs[kTevStageBase[0] + 1] = 0;
    state->regs[kTevStageBase[0] + 2] = 0;
    for (u32 stage = 1; stage < 6; ++stage) PassThrough(*state, stage);
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

// ---- the rig (adapted from gpu_fog_test.cpp) -----------------------------------------------------------------------------

using cpuvertex::OutputVertex;

float B(int byte) { return static_cast<float>(byte) / 255.0f; }

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

// An affine map of the target: texcoord = (u0 + ux s + uy t, v0 + vx s + vy t), s and t running from 0 to 1 over the target.
struct Map {
    float u0 = 0, ux = 0, uy = 0, v0 = 0, vx = 0, vy = 0;
};

// The one triangle that covers the whole target (NDC (-1,-1), (3,-1), (-1,3)) with the three texcoord sets the maps give.
struct Tri {
    OutputVertex v[3];
};
Tri MapTriangle(const Map maps[3], std::array<int, 4> color) {
    Tri tri{};
    const float xs[3] = {-1, 3, -1}, ys[3] = {-1, -1, 3};
    for (int i = 0; i < 3; ++i) {
        OutputVertex& v = tri.v[i];
        v.pos[0] = xs[i]; v.pos[1] = ys[i]; v.pos[2] = -0.5f; v.pos[3] = 1.0f;
        v.quat[3] = 1.0f;
        for (int c = 0; c < 4; ++c) v.color[c] = B(color[c]);
        v.tc0_w = 1.0f;
        // vertex 0 is (s, t) = (0, 0), vertex 1 (2, 0), vertex 2 (0, 2)
        const float s = i == 1 ? 2.0f : 0.0f, t = i == 2 ? 2.0f : 0.0f;
        v.tc0[0] = maps[0].u0 + maps[0].ux * s + maps[0].uy * t;
        v.tc0[1] = maps[0].v0 + maps[0].vx * s + maps[0].vy * t;
        v.tc1[0] = maps[1].u0 + maps[1].ux * s + maps[1].uy * t;
        v.tc1[1] = maps[1].v0 + maps[1].vx * s + maps[1].vy * t;
        v.tc2[0] = maps[2].u0 + maps[2].ux * s + maps[2].uy * t;
        v.tc2[1] = maps[2].v0 + maps[2].vx * s + maps[2].vy * t;
    }
    return tri;
}

struct DrawSpec {
    std::shared_ptr<pica::PicaState> state;
    Tri tri;
};

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
    FragmentBlock last_block{};
    FragmentKey last_key{};
    bool last_fallback = false;  // the draw ran with the ubershader fallback

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
        auto vr = backend->CreateRing(1 << 20, rhi::BufferUsage::Vertex, "proctex vertices");
        auto sr = backend->CreateRing(1 << 20, rhi::BufferUsage::Uniform, "proctex vs uniforms");
        REQUIRE(vr.ok());
        REQUIRE(sr.ok());
        vertex_ring = std::move(*vr);
        vs_ring = std::move(*sr);
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = desc.height = kSize;
        desc.name = "proctex colour";
        auto image = backend->CreateImage(desc);
        REQUIRE_MESSAGE(image.ok(), image.error());
        color = *image;
    }

    // Clears the target to zero, runs the draw in one recorder through FragmentState (the real path: the key from the registers, the
    // uniforms, the tables), and returns the colour pixels.
    std::vector<u8> Render(const DrawSpec& spec) {
        rhi::Recorder rec = backend->BeginRecording();
        composer->BeginRecording(rec);
        uniforms->BeginRecording();
        FragmentState fragment(profile, FeatureRegistry::Global());
        auto prepared = fragment.Prepare(*spec.state, {}, tables.get());
        REQUIRE_MESSAGE(prepared.ok(), prepared.error());
        auto alloc = uniforms->Write(fragment.block(), prepared->block_changed);
        REQUIRE_MESSAGE(alloc.ok(), alloc.error());
        last_block = fragment.block();
        last_key = fragment.key();

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
        rhi::TargetPassDesc clear_pass = pass;
        clear_pass.color_load = rhi::LoadOp::Clear;
        clear_pass.clear_color = {0, 0, 0, 0};
        clear_pass.depth_load = rhi::LoadOp::Clear;
        clear_pass.clear_depth = 1.0f;
        composer->BeginTargetPass(rec, clear_pass);
        fbops::ApplyDynamicState(rec, dynamic, false);
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
        last_fallback = result->fallback;
        backend->WaitIdle(backend->Submit(std::move(rec)));
        auto pixels = backend->ReadImage(*color, 0, 0, kSize, kSize);
        REQUIRE_MESSAGE(pixels.ok(), pixels.error());
        return *pixels;
    }
};

// The two rigs of the GPU cases are built once (the ubershader compiles once), by the first GPU case, and released by the last.
struct Rigs {
    std::shared_ptr<rhi::Backend> backend;
    std::unique_ptr<Rig> specialised, uber;
};
std::unique_ptr<Rigs>& SharedRigs() {
    static std::unique_ptr<Rigs> rigs;
    return rigs;
}
Rigs* RequireRigs() {
    std::unique_ptr<Rigs>& rigs = SharedRigs();
    if (!rigs) {
        auto backend = test::RequireBackend();
        if (!backend) return nullptr;
        rigs = std::make_unique<Rigs>();
        rigs->backend = backend;
        rigs->specialised = std::make_unique<Rig>(backend, false);
        rigs->uber = std::make_unique<Rig>(backend, true);
    }
    return rigs.get();
}

// ---- the procedural texture's registers and tables ------------------------------------------------------------------------

// Everything the registers and tables say. The defaults: ToEdge both ways, the U combiner, no noise, no shift, the Linear filter, a
// 255-entry table at offset 0, no bias.
struct Proc {
    u32 u_clamp = 1, v_clamp = 1;
    u32 color_combiner = 0, alpha_combiner = 0, separate_alpha = 0;
    u32 noise = 0, u_shift = 0, v_shift = 0;
    u32 coord = 0;
    s32 amp_u = 0, amp_v = 0;        // fixed 1.3.12 (the register's s16)
    u32 phase_u = 0, phase_v = 0;    // half-precision raw
    u32 freq_u = 0, freq_v = 0;      // half-precision raw
    u32 bias_raw = 0;                // half-precision raw
    u32 filter = 1, lod_min = 0, lod_max = 7;
    u32 width = 255;  // the register's field is 8 bits wide
    u32 offset[4] = {0, 0, 0, 0};
    std::array<u32, 128> noise_table{}, color_map{}, alpha_map{};
    std::array<u32, 256> color_table{}, color_diff{};
};

// A ValueEntry as the register delivers it: value bits 0-11, difference bits 12-23 (signed).
u32 ValueEntry(int value, int difference) { return static_cast<u32>(value) | ((static_cast<u32>(difference) & 0xFFF) << 12); }
// value = start + step * i, difference = step: every entry and every entry plus its difference stays inside [0, 4095].
std::array<u32, 128> Ramp(int start, int step) {
    std::array<u32, 128> table{};
    for (int i = 0; i < 128; ++i) table[i] = ValueEntry(start + step * i, step);
    return table;
}

// The colour table: bytes r = 3 i mod 256, g = 255 - i, b = a tent (2 i, then back down), a = 128 + i / 2; each difference entry is half
// the step to the next entry (ColorDifferenceEntry), so value + fraction * 2 * entry stays between the two neighbours.
void FillColorTables(Proc& p) {
    auto bytes = [](int i) {
        const int r = (3 * i) % 256, g = 255 - i, b = i < 128 ? 2 * i : 2 * (255 - i), a = 128 + i / 2;
        return std::array<int, 4>{r, g, b, a};
    };
    for (int i = 0; i < 256; ++i) {
        const std::array<int, 4> now = bytes(i), next = bytes(std::min(i + 1, 255));
        u32 value = 0, diff = 0;
        for (int c = 0; c < 4; ++c) {
            value |= static_cast<u32>(now[c]) << (8 * c);
            diff |= (static_cast<u32>((next[c] - now[c]) / 2) & 0xFF) << (8 * c);
        }
        p.color_table[i] = value;
        p.color_diff[i] = diff;
    }
}

Proc DefaultProc() {
    Proc p;
    p.noise_table = Ramp(100, 28);
    p.color_map = Ramp(0, 31);
    p.alpha_map = Ramp(4000, -30);
    FillColorTables(p);
    return p;
}

constexpr u32 kHalfOne = 0x3C00, kHalfTwo = 0x4000, kHalfHalf = 0x3800, kHalfThreeQuarters = 0x3A00, kHalfThreeEighths = 0x3600,
              kHalfQuarter = 0x3400, kHalfEighth = 0x3000, kHalfOneAndHalf = 0x3E00;

// Writes the registers and tables of `p` into a state.
void Apply(pica::PicaState& s, const Proc& p) {
    s.regs[kRegMainConfig] = (1u << 10) | (p.coord << 8);  // texture 3 enabled, its coordinates
    s.regs[kRegProcTex] = p.u_clamp | (p.v_clamp << 3) | (p.color_combiner << 6) | (p.alpha_combiner << 10) | (p.separate_alpha << 14) |
                          (p.noise << 15) | (p.u_shift << 16) | (p.v_shift << 18) | ((p.bias_raw & 0xFF) << 20);
    s.regs[kRegNoiseU] = (static_cast<u32>(p.amp_u) & 0xFFFF) | (p.phase_u << 16);
    s.regs[kRegNoiseV] = (static_cast<u32>(p.amp_v) & 0xFFFF) | (p.phase_v << 16);
    s.regs[kRegNoiseFrequency] = p.freq_u | (p.freq_v << 16);
    s.regs[kRegLut] = p.filter | (p.lod_min << 3) | (p.lod_max << 7) | (p.width << 11) | (((p.bias_raw >> 8) & 0xFF) << 19);
    s.regs[kRegLutOffset] = p.offset[0] | (p.offset[1] << 8) | (p.offset[2] << 16) | (p.offset[3] << 24);
    s.proctex.noise_table = p.noise_table;
    s.proctex.color_map_table = p.color_map;
    s.proctex.alpha_map_table = p.alpha_map;
    s.proctex.color_table = p.color_table;
    s.proctex.color_diff_table = p.color_diff;
    s.proctex.table_dirty = pica::ProcTex::kAllDirty;
}

// ---- the TEV layouts: where the draw reads Texture3 -----------------------------------------------------------------------

// Texture3 is read at a stage other than 0, through colour and alpha operands, so a sample that is lost shows (stage 0 reads the
// primary colour, the others are pass-through unless named).
enum class Tev { Replace1, Replace3, Split24, Inverted5 };

const char* Name(Tev t) {
    switch (t) {
        case Tev::Replace1: return "stage 1: colour and alpha from Texture3";
        case Tev::Replace3: return "stage 3: colour and alpha from Texture3";
        case Tev::Split24: return "stage 2 colour from Texture3, stage 4 alpha from Texture3 (one minus alpha)";
        case Tev::Inverted5: return "stage 5: one minus Texture3's colour, Texture3's red as alpha";
    }
    return "";
}

void ApplyTev(pica::PicaState& s, Tev t) {
    auto replace = [&](u32 stage, u32 color_source, u32 alpha_source, u32 modifiers) {
        const u32 base = kTevStageBase[stage];
        s.regs[base + 0] = color_source | (kPrevious << 4) | (kPrevious << 8) | (alpha_source << 16) | (kPrevious << 20) | (kPrevious << 24);
        s.regs[base + 1] = modifiers;
        s.regs[base + 2] = 0;  // Replace both
    };
    switch (t) {
        case Tev::Replace1: replace(1, kTexture3, kTexture3, 0); break;
        case Tev::Replace3: replace(3, kTexture3, kTexture3, 0); break;
        case Tev::Split24:
            replace(2, kTexture3, kPrimary, 0);
            replace(4, kPrevious, kTexture3, 1u << 12);  // alpha modifier 1: one minus source alpha
            break;
        case Tev::Inverted5: replace(5, kTexture3, kTexture3, 1u | (2u << 12)); break;  // colour: one minus source colour; alpha: source red
    }
}

// The pixel (bytes as floats) that layout `t` makes of the procedural texture's colour.
std::array<float, 4> ModelTev(Tev t, std::array<float, 4> tex) {
    switch (t) {
        case Tev::Replace1:
        case Tev::Replace3: return tex;
        case Tev::Split24: return {tex[0], tex[1], tex[2], 255.0f - tex[3]};
        case Tev::Inverted5: return {255.0f - tex[0], 255.0f - tex[1], 255.0f - tex[2], tex[0]};
    }
    return tex;
}

// ---- the software renderer's ProcTex, ported -------------------------------------------------------------------------------

float HalfToFloat(u32 half) {
    const u32 sign = (half >> 15) & 1u;
    const int exponent = static_cast<int>((half >> 10) & 0x1Fu);
    const u32 mantissa = half & 0x3FFu;
    float magnitude;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<float>(mantissa), -24);
    } else {
        magnitude = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, exponent - 15);
    }
    return sign != 0 ? -magnitude : magnitude;
}

float ValueOf(u32 entry) { return static_cast<float>(entry & 0xFFF) / 4095.0f; }
float DiffOf(u32 entry) { return static_cast<float>((static_cast<s32>(((entry >> 12) & 0xFFF) << 20)) >> 20) / 4095.0f; }

float LookupLUT(const std::array<u32, 128>& lut, float coord) {
    coord *= 128;
    const int index_int = std::min(static_cast<int>(coord), 127);
    const float frac = coord - index_int;
    return ValueOf(lut[index_int]) + frac * DiffOf(lut[index_int]);
}

unsigned NoiseRand1D(unsigned v) {
    static constexpr std::array<unsigned, 16> table{{0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11}};
    return ((v % 9 + 2) * 3 & 0xF) ^ table[(v / 9) & 0xF];
}

float NoiseRand2D(unsigned x, unsigned y) {
    static constexpr std::array<unsigned, 16> table{{10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14}};
    unsigned u2 = NoiseRand1D(x);
    unsigned v2 = NoiseRand1D(y);
    v2 += ((u2 & 3) == 1) ? 4 : 0;
    v2 ^= (u2 & 1) * 6;
    v2 += 10 + u2;
    v2 &= 0xF;
    v2 ^= table[u2];
    return -1.0f + v2 * 2.0f / 15.0f;
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

float NoiseCoef(float u, float v, const Proc& p) {
    const float freq_u = HalfToFloat(p.freq_u), freq_v = HalfToFloat(p.freq_v);
    const float phase_u = HalfToFloat(p.phase_u), phase_v = HalfToFloat(p.phase_v);
    const float x = 9 * freq_u * std::abs(u + phase_u);
    const float y = 9 * freq_v * std::abs(v + phase_v);
    const int x_int = static_cast<int>(x);
    const int y_int = static_cast<int>(y);
    const float x_frac = x - x_int;
    const float y_frac = y - y_int;
    const float g0 = NoiseRand2D(x_int, y_int) * (x_frac + y_frac);
    const float g1 = NoiseRand2D(x_int + 1, y_int) * (x_frac + y_frac - 1);
    const float g2 = NoiseRand2D(x_int, y_int + 1) * (x_frac + y_frac - 1);
    const float g3 = NoiseRand2D(x_int + 1, y_int + 1) * (x_frac + y_frac - 2);
    const float x_noise = LookupLUT(p.noise_table, x_frac);
    const float y_noise = LookupLUT(p.noise_table, y_frac);
    return Lerp(Lerp(g0, g1, x_noise), Lerp(g2, g3, x_noise), y_noise);
}

float GetShiftOffset(float v, u32 mode, u32 clamp_mode) {
    const float offset = (clamp_mode == 3) ? 1.0f : 0.5f;
    switch (mode) {
        case 0: return 0;
        case 1: return offset * (((int)v / 2) % 2);
        case 2: return offset * ((((int)v + 1) / 2) % 2);
        default: return 0;
    }
}

void ClampCoord(float& coord, u32 mode) {
    switch (mode) {
        case 0:
            if (coord > 1.0f) coord = 0.0f;
            break;
        case 1: coord = std::min(coord, 1.0f); break;
        case 2: coord = coord - std::floor(coord); break;
        case 3: {
            int integer = static_cast<int>(coord);
            float frac = coord - integer;
            coord = (integer % 2) == 0 ? frac : (1.0f - frac);
            break;
        }
        case 4: coord = coord <= 0.5f ? 0.0f : 1.0f; break;
        default: coord = std::min(coord, 1.0f); break;
    }
}

float CombineAndMap(float u, float v, u32 combiner, const std::array<u32, 128>& map_table) {
    float f;
    switch (combiner) {
        case 0: f = u; break;
        case 1: f = u * u; break;
        case 2: f = v; break;
        case 3: f = v * v; break;
        case 4: f = (u + v) * 0.5f; break;
        case 5: f = (u * u + v * v) * 0.5f; break;
        case 6: f = std::min(std::sqrt(u * u + v * v), 1.0f); break;
        case 7: f = std::min(u, v); break;
        case 8: f = std::max(u, v); break;
        case 9: f = std::min(((u + v) * 0.5f + std::sqrt(u * u + v * v)) * 0.5f, 1.0f); break;
        default: f = 0.0f; break;
    }
    return LookupLUT(map_table, f);
}

std::array<float, 4> ColorBytes(const Proc& p, int index) {
    const u32 c = p.color_table[index];
    return {static_cast<float>(c & 255), static_cast<float>((c >> 8) & 255), static_cast<float>((c >> 16) & 255),
            static_cast<float>((c >> 24) & 255)};
}
std::array<float, 4> DiffBytes(const Proc& p, int index) {  // twice the stored half difference
    const u32 d = p.color_diff[index];
    std::array<float, 4> out{};
    for (int c = 0; c < 4; ++c) out[c] = static_cast<float>((static_cast<s32>(((d >> (8 * c)) & 255) << 24) >> 24) * 2);
    return out;
}

// Azahar's GLSL, for the mipmap filters (the software renderer has none): `SampleProcTexColor(lut_coord, level)` in the 0 to 255 range.
std::array<float, 4> GlslColorAtLevel(const Proc& p, float lut_coord, int level) {
    const int lut_width = static_cast<int>(p.width) >> level;
    const int offsets[8] = {static_cast<int>(p.offset[0]), static_cast<int>(p.offset[1]), static_cast<int>(p.offset[2]),
                            static_cast<int>(p.offset[3]), 0xF0, 0xF8, 0xFC, 0xFE};
    const int offset = offsets[level];
    lut_coord *= static_cast<float>(lut_width - 1);
    const bool linear = p.filter == 1 || p.filter == 3 || p.filter == 5;
    if (linear) {
        const int index = static_cast<int>(lut_coord) + offset;
        const float frac = lut_coord - std::floor(lut_coord);
        const std::array<float, 4> value = ColorBytes(p, index), diff = DiffBytes(p, index);
        return {value[0] + frac * diff[0], value[1] + frac * diff[1], value[2] + frac * diff[2], value[3] + frac * diff[3]};
    }
    lut_coord += static_cast<float>(offset);
    return ColorBytes(p, static_cast<int>(std::round(lut_coord)));
}

// The GLSL's level of detail of the draw (the derivatives of the affine coordinates are the map's coefficients over the target).
float GlslLod(const Proc& p, const Map& m) {
    const float dx = std::max(std::abs(m.ux), std::abs(m.uy)) / static_cast<float>(kSize);
    const float dy = std::max(std::abs(m.vx), std::abs(m.vy)) / static_cast<float>(kSize);
    const float bias = HalfToFloat(p.bias_raw);
    float lod = std::log2(std::abs(static_cast<float>(p.width) * bias) * (dx + dy));
    if (bias == 0.0f) lod = 0.0f;
    return std::clamp(lod, std::max(0.0f, static_cast<float>(p.lod_min)), std::min(7.0f, static_cast<float>(p.lod_max)));
}

// SwRenderer::ProcTex on (u, v), with the level of detail `lod` for the GLSL-only mipmap filters. The result is the colour in bytes
// (the software renderer's u8 values as floats, or the GLSL model's unrounded ones for the mipmap filters).
std::array<float, 4> ProcTexModel(const Proc& p, float u, float v, float lod) {
    u = std::abs(u);
    v = std::abs(v);
    const float u_shift = GetShiftOffset(v, p.u_shift, p.u_clamp);
    const float v_shift = GetShiftOffset(u, p.v_shift, p.v_clamp);
    if (p.noise != 0) {
        const float noise = NoiseCoef(u, v, p);
        u += noise * static_cast<float>(p.amp_u) / 4095.0f;
        v += noise * static_cast<float>(p.amp_v) / 4095.0f;
        u = std::abs(u);
        v = std::abs(v);
    }
    u += u_shift;
    v += v_shift;
    ClampCoord(u, p.u_clamp);
    ClampCoord(v, p.v_clamp);
    const float lut_coord = CombineAndMap(u, v, p.color_combiner, p.color_map);

    std::array<float, 4> color;
    if (p.filter >= 2) {
        // GLSL: the nearest level, or the two levels and their mix
        if (p.filter == 2 || p.filter == 3) {
            color = GlslColorAtLevel(p, lut_coord, static_cast<int>(std::round(lod)));
        } else {
            const int lod_i = static_cast<int>(lod);
            const float lod_f = lod - std::floor(lod);
            const std::array<float, 4> a = GlslColorAtLevel(p, lut_coord, lod_i);
            const std::array<float, 4> b = GlslColorAtLevel(p, lut_coord, std::min(lod_i + 1, 7));
            for (int c = 0; c < 4; ++c) color[c] = a[c] * (1.0f - lod_f) + b[c] * lod_f;
        }
    } else {
        // the software renderer: the offset is added before the interpolation, the colour truncated to a byte
        const float index = static_cast<float>(p.offset[0]) + (lut_coord * static_cast<float>(p.width - 1));
        if (p.filter == 1) {
            const int index_int = static_cast<int>(index);
            const float frac = index - index_int;
            const std::array<float, 4> value = ColorBytes(p, index_int), diff = DiffBytes(p, index_int);
            for (int c = 0; c < 4; ++c) color[c] = static_cast<float>(static_cast<u8>(value[c] + frac * diff[c]));
        } else {
            color = ColorBytes(p, static_cast<int>(std::round(index)));
        }
    }
    if (p.separate_alpha != 0) {
        // the alpha channel skips the colour LUT and uses the output of CombineAndMap directly
        const float final_alpha = CombineAndMap(u, v, p.alpha_combiner, p.alpha_map);
        color[3] = p.filter >= 2 ? final_alpha * 255.0f : static_cast<float>(static_cast<u8>(final_alpha * 255));
    }
    return color;
}

// ---- the cases ---------------------------------------------------------------------------------------------------------------

struct Case {
    std::string name;
    Proc proc;
    Map maps[3];
    Tev tev = Tev::Replace1;
};

// The rasterizer's pixel centres, in the order of the readback: s = (x + 0.5) / 8; t from the same row index. (kFlipT is set by the first
// run of the rig: Vulkan's framebuffer has y down while the vertices' y is up.)
constexpr bool kFlipT = false;

std::array<float, 2> MapAt(const Map& m, u32 x, u32 y) {
    const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSize);
    float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSize);
    if (kFlipT) t = 1.0f - t;
    return {m.u0 + m.ux * s + m.uy * t, m.v0 + m.vx * s + m.vy * t};
}

std::array<float, 4> CasePixel(const Case& c, u32 x, u32 y, float du = 0.0f, float dv = 0.0f) {
    const Map& m = c.maps[c.proc.coord];
    const std::array<float, 2> uv = MapAt(m, x, y);
    return ModelTev(c.tev, ProcTexModel(c.proc, uv[0] + du, uv[1] + dv, GlslLod(c.proc, m)));
}

DrawSpec MakeDraw(const Case& c) {
    std::shared_ptr<pica::PicaState> state = BaseState();
    Apply(*state, c.proc);
    ApplyTev(*state, c.tev);
    DrawSpec spec;
    spec.state = state;
    spec.tri = MapTriangle(c.maps, {90, 60, 30, 200});
    return spec;
}

// Compares every pixel with the model: each channel within `tolerance`. `why` names the first mismatch.
void CompareToModel(const std::vector<u8>& pixels, const Case& c, int tolerance, std::string& why) {
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const std::array<float, 4> want = CasePixel(c, x, y);
            for (int ch = 0; ch < 4; ++ch) {
                const float got = pixels[(static_cast<size_t>(y) * kSize + x) * 4 + ch];
                if (std::abs(got - want[ch]) > static_cast<float>(tolerance) + 0.01f && why.empty()) {
                    why = c.name + ": pixel (" + std::to_string(x) + "," + std::to_string(y) + ") channel " + std::to_string(ch) + " is " +
                          std::to_string(static_cast<int>(got)) + ", expected " + std::to_string(want[ch]) + " +- " + std::to_string(tolerance);
                }
            }
        }
    }
}

bool ReportOn() { return std::getenv("MH4N_PROCTEX_REPORT") != nullptr; }

// Renders every case in the specialised mode and in the ubershader and checks each against the model and the two against each other.
void RunCases(const std::vector<Case>& cases) {
    Rigs* rigs = RequireRigs();
    if (rigs == nullptr) return;
    for (const Case& c : cases) {
        INFO(c.name << " (" << Name(c.tev) << ")");
        const DrawSpec spec = MakeDraw(c);
        const std::vector<u8> a = rigs->specialised->Render(spec);
        const std::vector<u8> b = rigs->uber->Render(spec);
        CHECK_MESSAGE(!rigs->specialised->last_fallback, c.name << ": the specialised rig used the ubershader");
        CHECK_MESSAGE(rigs->uber->last_fallback, c.name << ": the forced rig did not use the ubershader");
        std::string why_a, why_b;
        CompareToModel(a, c, 1, why_a);
        CompareToModel(b, c, 1, why_b);
        CHECK_MESSAGE(why_a.empty(), "specialised: " << why_a);
        CHECK_MESSAGE(why_b.empty(), "ubershader: " << why_b);
        // the same draw in the two modes: the whole image
        int worst = 0;
        for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
        CHECK_MESSAGE(worst <= 1, c.name << ": the specialised shader and the ubershader differ by " << worst);
        // the draw must show something: not all pixels equal the cleared target
        bool any = false;
        for (u8 byte : a) any = any || byte != 0;
        CHECK_MESSAGE(any, c.name << ": the draw wrote nothing");
        if (ReportOn()) std::printf("proctex %-70s worst spec-vs-uber %d\n", c.name.c_str(), worst);
    }
}

// ---- the maps ---------------------------------------------------------------------------------------------------------------

// Three coordinate sets that differ, so a wrong `coord` shows. `a` is the one the case reads (coord 0), the others are unrelated.
Map MapOf(float u0, float ux, float uy, float v0, float vx, float vy) { return Map{u0, ux, uy, v0, vx, vy}; }

Case MakeCase(const std::string& name, Proc proc, Map used, Tev tev = Tev::Replace1) {
    Case c;
    c.name = name;
    c.proc = proc;
    // the coordinate sets the case does not read
    const Map other0 = MapOf(0.9f, -0.3f, 0.2f, 0.15f, 0.1f, 0.45f);
    const Map other1 = MapOf(0.05f, 0.6f, 0.25f, 0.7f, -0.2f, 0.3f);
    const Map other2 = MapOf(0.4f, 0.1f, 0.55f, 0.3f, 0.45f, -0.1f);
    c.maps[0] = other0;
    c.maps[1] = other1;
    c.maps[2] = other2;
    c.maps[proc.coord] = used;
    c.tev = tev;
    return c;
}

constexpr Tev kTevRotation[4] = {Tev::Replace1, Tev::Replace3, Tev::Split24, Tev::Inverted5};

// The cases of each group (every one a distinct key: a specialised pipeline each).
std::vector<Case> ClampCases() {
    // u and v run from 0.07 and 0.11 to 2.4 and 2.2: past 1 (ToZero, ToEdge, Pulse at 0.5) and past 2 (the mirror's odd and even)
    const Map m = MapOf(0.07f, 1.1f, 1.23f, 0.11f, 0.9f, 1.2f);
    std::vector<Case> out;
    for (u32 i = 0; i < 5; ++i) {
        Proc p = DefaultProc();
        p.u_clamp = i;
        p.v_clamp = (i + 2) % 5;
        p.color_combiner = 4;  // Add: reads both coordinates
        out.push_back(MakeCase("clamp u=" + std::to_string(p.u_clamp) + " v=" + std::to_string(p.v_clamp), p, m, kTevRotation[i % 4]));
    }
    return out;
}

std::vector<Case> ShiftCases() {
    // v runs to 5.9 (the u shift reads uv.y: odd on [2, 4), even on [1, 3) and [5, 7)), u to 5.4
    const Map m = MapOf(0.13f, 2.4f, 3.0f, 0.21f, 3.1f, 2.4f);
    struct S { u32 u_shift, v_shift, u_clamp, v_clamp; };
    const S shifts[] = {
        {1, 2, 3, 2},  // odd on u with MirroredRepeat (offset 1.0), even on v with SymmetricalRepeat (0.5)
        {2, 1, 2, 3},  // even on u with SymmetricalRepeat (0.5), odd on v with MirroredRepeat (1.0)
        {1, 1, 1, 0},  // odd both, ToEdge and ToZero (0.5)
        {2, 2, 4, 1},  // even both, Pulse and ToEdge (0.5)
        {0, 2, 3, 3},  // none on u, even on v with MirroredRepeat (1.0)
    };
    std::vector<Case> out;
    int i = 0;
    for (const S& s : shifts) {
        Proc p = DefaultProc();
        p.u_shift = s.u_shift;
        p.v_shift = s.v_shift;
        p.u_clamp = s.u_clamp;
        p.v_clamp = s.v_clamp;
        p.color_combiner = 8;  // Max
        out.push_back(MakeCase("shift u=" + std::to_string(s.u_shift) + "/clamp " + std::to_string(s.u_clamp) + " v=" +
                                   std::to_string(s.v_shift) + "/clamp " + std::to_string(s.v_clamp),
                               p, m, kTevRotation[(i++ + 1) % 4]));
    }
    return out;
}

std::vector<Case> CombinerCases() {
    const Map m = MapOf(0.03f, 0.82f, 0.31f, 0.06f, 0.27f, 0.88f);
    std::vector<Case> out;
    for (u32 i = 0; i < 10; ++i) {
        Proc p = DefaultProc();
        p.color_combiner = i;
        p.alpha_combiner = (i + 4) % 10;
        p.separate_alpha = 1;
        out.push_back(MakeCase("combiner colour=" + std::to_string(p.color_combiner) + " alpha=" + std::to_string(p.alpha_combiner) +
                                   " (separate alpha)",
                               p, m, kTevRotation[i % 4]));
    }
    Proc p = DefaultProc();
    p.color_combiner = 9;  // the alpha is the colour table's, not the alpha map's
    out.push_back(MakeCase("combiner colour=9, alpha from the colour table", p, m, Tev::Split24));
    return out;
}

std::vector<Case> FilterCases() {
    std::vector<Case> out;
    // the non-mipmap filters: width 64, offset 5 (the nearest filter's rounding is far from a boundary for most pixels)
    {
        Proc p = DefaultProc();
        p.filter = 0;
        p.width = 64;
        p.offset[0] = 5;
        p.color_combiner = 4;
        out.push_back(MakeCase("filter 0 Nearest, width 64, offset 5", p, MapOf(0.02f, 0.9f, 0.05f, 0.04f, 0.1f, 0.8f), Tev::Replace3));
        p.filter = 1;
        out.push_back(MakeCase("filter 1 Linear, width 64, offset 5", p, MapOf(0.02f, 0.9f, 0.05f, 0.04f, 0.1f, 0.8f), Tev::Replace1));
    }
    // the mipmap filters (GLSL only): width 64 with the levels at 0, 64, 96, 112 (widths 64, 32, 16, 8). The map's slope sets the lod:
    // lod = log2(64 * 1.0 * 8 * (max|ux, uy| / 8 + ...)) -> see GlslLod; the levels are printed with MH4N_PROCTEX_REPORT
    struct Mip { u32 filter; float scale; u32 lod_min, lod_max; u32 bias; const char* what; };
    const Mip mips[] = {
        {2, 0.307f, 0, 7, kHalfOne, "NearestMipmapNearest, lod ~1.3 -> level 1"},
        {3, 0.574f, 0, 7, kHalfOne, "LinearMipmapNearest, lod ~2.2 -> level 2"},
        {4, 0.1895f, 0, 7, kHalfOne, "NearestMipmapLinear, lod ~0.6: levels 0 and 1"},
        {5, 0.405f, 0, 7, kHalfOne, "LinearMipmapLinear, lod ~1.7: levels 1 and 2"},
        {5, 0.405f, 0, 7, 0, "LinearMipmapLinear with a zero bias: lod 0"},
        {3, 0.574f, 0, 1, kHalfOne, "LinearMipmapNearest clamped by lod_max 1"},
        {3, 0.0813f, 2, 7, kHalfOne, "LinearMipmapNearest lifted by lod_min 2 (lod ~0.4)"},
        {4, 0.3f, 0, 7, kHalfThreeQuarters, "NearestMipmapLinear with a bias of 0.75: lod ~0.9"},
    };
    for (const Mip& m : mips) {
        Proc p = DefaultProc();
        p.filter = m.filter;
        p.width = 64;
        p.offset[0] = 0;
        p.offset[1] = 64;
        p.offset[2] = 96;
        p.offset[3] = 112;
        p.lod_min = m.lod_min;
        p.lod_max = m.lod_max;
        p.bias_raw = m.bias;
        p.color_combiner = 4;
        // ux + uy... the lod depends on max(|ux|, |uy|) + max(|vx|, |vy|) = scale * 1.5 / ... (see GlslLod); u0 and v0 keep the maps inside [0, 1]
        const float s = m.scale;
        out.push_back(MakeCase(std::string("filter ") + m.what, p, MapOf(0.1f, s * 0.7f, s * 0.2f, 0.15f, s * 0.3f, s * 0.9f), Tev::Replace1));
    }
    return out;
}

std::vector<Case> CoordCases() {
    std::vector<Case> out;
    for (u32 i = 0; i < 3; ++i) {
        Proc p = DefaultProc();
        p.coord = i;
        p.color_combiner = 5;  // Add2
        out.push_back(MakeCase("coordinates texcoord" + std::to_string(i), p, MapOf(0.1f, 0.77f, 0.35f, 0.2f, 0.4f, 0.69f),
                               kTevRotation[(i + 2) % 4]));
    }
    return out;
}

std::vector<Case> LutCases() {
    const Map m = MapOf(0.05f, 0.9f, 0.08f, 0.1f, 0.12f, 0.85f);
    std::vector<Case> out;
    struct W { u32 width, offset; u32 filter; };
    for (const W& w : {W{17, 5, 1}, W{100, 100, 1}, W{2, 254, 1}, W{33, 200, 0}}) {
        Proc p = DefaultProc();
        p.width = w.width;
        p.offset[0] = w.offset;
        p.filter = w.filter;
        p.color_combiner = 4;
        out.push_back(MakeCase("lut width " + std::to_string(w.width) + " offset " + std::to_string(w.offset) + " filter " +
                                   std::to_string(w.filter),
                               p, m, kTevRotation[w.width % 4]));
    }
    return out;
}

std::vector<Case> NoiseCases() {
    const Map m = MapOf(0.11f, 0.83f, 0.17f, 0.07f, 0.21f, 0.91f);
    std::vector<Case> out;
    {
        Proc p = DefaultProc();
        p.noise = 1;
        p.freq_u = kHalfOne;
        p.freq_v = kHalfOne;
        p.amp_u = 1024;
        p.amp_v = 1024;
        p.color_combiner = 4;
        out.push_back(MakeCase("noise f=(1, 1) a=(+0.25, +0.25) p=0", p, m, Tev::Replace1));
    }
    {
        Proc p = DefaultProc();
        p.noise = 1;
        p.freq_u = kHalfTwo;
        p.freq_v = kHalfThreeQuarters;
        p.amp_u = -2048;
        p.amp_v = 512;
        p.phase_u = kHalfThreeEighths;
        p.phase_v = 0;
        p.color_combiner = 4;
        out.push_back(MakeCase("noise f=(2, 0.75) a=(-0.5, +0.125) p=(0.375, 0)", p, m, Tev::Replace3));
    }
    {
        Proc p = DefaultProc();
        p.noise = 1;
        p.freq_u = kHalfHalf;
        p.freq_v = kHalfOneAndHalf;
        p.amp_u = 0;
        p.amp_v = -1500;
        p.phase_u = kHalfQuarter;
        p.phase_v = kHalfEighth;
        p.color_combiner = 2;  // V: only the v noise shows
        out.push_back(MakeCase("noise f=(0.5, 1.5) a=(0, -0.37) p=(0.25, 0.125), V combiner", p, m, Tev::Split24));
    }
    {
        // noise with a shift (taken before the noise), a clamp and a separate alpha
        Proc p = DefaultProc();
        p.noise = 1;
        p.freq_u = kHalfOne;
        p.freq_v = kHalfTwo;
        p.amp_u = 2000;
        p.amp_v = -700;
        p.phase_u = kHalfEighth;
        p.phase_v = kHalfQuarter;
        p.u_shift = 1;
        p.v_shift = 2;
        p.u_clamp = 3;
        p.v_clamp = 2;
        p.color_combiner = 5;
        p.alpha_combiner = 6;
        p.separate_alpha = 1;
        out.push_back(MakeCase("noise with shifts, mirrored and repeated clamps and a separate alpha", p,
                               MapOf(0.2f, 2.3f, 0.4f, 0.1f, 0.6f, 3.1f), Tev::Inverted5));
    }
    {
        // noise off while its registers and table hold values: no effect
        Proc p = DefaultProc();
        p.noise = 0;
        p.freq_u = kHalfOne;
        p.freq_v = kHalfOne;
        p.amp_u = 3000;
        p.amp_v = 3000;
        p.color_combiner = 4;
        out.push_back(MakeCase("noise off with noise registers set", p, m, Tev::Replace1));
    }
    return out;
}

// ---- without a device ----------------------------------------------------------------------------------------------------

const DeviceProfile kFullProfile = [] {
    DeviceProfile p;
    p.logic_op = true;
    p.custom_border_color = true;
    p.interlock = true;
    p.geometry_shader = true;
    return p;
}();
const DeviceProfile kLeanProfile = DeviceProfile{};

bool IsIdentifierChar(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; }

// The calls of the function `name` in `text`: the occurrences `name(` that are not a prototype or a definition (a return type before).
size_t CountCalls(const std::string& text, const std::string& name) {
    size_t count = 0;
    const std::string needle = name + "(";
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        if (at > 0 && IsIdentifierChar(text[at - 1])) continue;
        auto preceded_by = [&](const char* word) {
            const size_t n = std::strlen(word);
            return at >= n && text.compare(at - n, n, word) == 0;
        };
        if (preceded_by("float ") || preceded_by("vec4 ") || preceded_by("int ") || preceded_by("vec2 ")) continue;
        ++count;
    }
    return count;
}

// The text of the function that starts with `header` (up to its closing brace), or empty.
std::string BodyOf(const std::string& text, const std::string& header) {
    const size_t start = text.find(header);
    if (start == std::string::npos) return {};
    size_t at = text.find('{', start);
    int depth = 0;
    for (size_t i = at; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        if (text[i] == '}' && --depth == 0) return text.substr(start, i - start + 1);
    }
    return {};
}

}  // namespace

TEST_CASE("proctex/the_feature_is_real_and_needs_the_two_texel_buffers_only_while_it_is_on") {
    const FeatureModule* feature = FeatureRegistry::Global().Find("proctex");
    REQUIRE(feature != nullptr);
    FragmentKey off;
    FragmentKey on;
    on.proctex.set_enable(1);
    CHECK(feature->Stubbed(off) == nullptr);
    CHECK(feature->Stubbed(on) == nullptr);
    std::array<const char*, 8> names{};
    CHECK(StubbedFeatures(on, FeatureRegistry::Global(), names) == 0);
    CHECK_FALSE(NeedsOf(off, DeviceProfile{}).lut_rg);
    CHECK_FALSE(NeedsOf(off, DeviceProfile{}).lut_rgba);
    CHECK(NeedsOf(on, DeviceProfile{}).lut_rg);
    CHECK(NeedsOf(on, DeviceProfile{}).lut_rgba);
    CHECK_FALSE(NeedsOf(on, DeviceProfile{}).lut_lf);
    // the key follows the registers (N2-33's MakeKey)
    Proc p = DefaultProc();
    p.coord = 2;
    p.u_clamp = 3;
    p.v_clamp = 4;
    p.color_combiner = 9;
    p.alpha_combiner = 6;
    p.separate_alpha = 1;
    p.noise = 1;
    p.u_shift = 2;
    p.v_shift = 1;
    p.filter = 5;
    p.width = 77;
    p.offset[0] = 1; p.offset[1] = 2; p.offset[2] = 3; p.offset[3] = 4;
    p.lod_min = 3;
    p.lod_max = 6;
    auto state = BaseState();
    Apply(*state, p);
    const FragmentKey key = MakeKey(*state);
    CHECK(key.proctex.enable() == 1);
    CHECK(key.proctex.coord() == 2);
    CHECK(static_cast<u32>(key.proctex.u_clamp()) == 3);
    CHECK(static_cast<u32>(key.proctex.v_clamp()) == 4);
    CHECK(static_cast<u32>(key.proctex.color_combiner()) == 9);
    CHECK(static_cast<u32>(key.proctex.alpha_combiner()) == 6);
    CHECK(key.proctex.separate_alpha() == 1);
    CHECK(key.proctex.noise_enable() == 1);
    CHECK(static_cast<u32>(key.proctex.u_shift()) == 2);
    CHECK(static_cast<u32>(key.proctex.v_shift()) == 1);
    CHECK(static_cast<u32>(key.proctex.lut_filter()) == 5);
    CHECK(key.proctex.lut_width == 77);
    CHECK(key.proctex.lut_offset0 == 1);
    CHECK(key.proctex.lut_offset3 == 4);
    CHECK(key.proctex.lod_min == 3);
    CHECK(key.proctex.lod_max == 6);
}

TEST_CASE("proctex/the_uniforms_follow_the_registers_and_are_written_only_when_one_of_the_six_registers_is_dirty") {
    Proc p = DefaultProc();
    p.freq_u = kHalfTwo;
    p.freq_v = kHalfThreeQuarters;
    p.amp_u = 1024;
    p.amp_v = -2048;          // a negative amplitude: the register's s16
    p.phase_u = kHalfThreeEighths;
    p.phase_v = kHalfEighth;
    p.bias_raw = 0x4200;      // 3.0: bias_low is the proctex register's bits 20-27 (0x00), bias_high the lut register's bits 19-26 (0x42)
    auto state = BaseState();
    Apply(*state, p);

    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    UniformDirty dirty;
    dirty.all = true;
    FillUniforms(*state, block, dirty, {});
    CHECK(dirty.changed);
    CHECK(block.uniforms.proctex_noise_f.x == doctest::Approx(2.0f));
    CHECK(block.uniforms.proctex_noise_f.y == doctest::Approx(0.75f));
    CHECK(block.uniforms.proctex_noise_a.x == doctest::Approx(1024.0f / 4095.0f));
    CHECK(block.uniforms.proctex_noise_a.y == doctest::Approx(-2048.0f / 4095.0f));
    CHECK(block.uniforms.proctex_noise_p.x == doctest::Approx(0.375f));
    CHECK(block.uniforms.proctex_noise_p.y == doctest::Approx(0.125f));
    CHECK(block.uniforms.proctex_bias == doctest::Approx(3.0f));

    // the bias's low byte comes from the proctex register
    p.bias_raw = 0x3C01;  // low byte 0x01 (the proctex register's bits 20-27), high byte 0x3C (the lut register's bits 19-26)
    state = BaseState();
    Apply(*state, p);
    FragmentBlock other;
    std::memset(&other, 0, sizeof(other));
    UniformDirty all;
    all.all = true;
    FillUniforms(*state, other, all, {});
    CHECK(other.uniforms.proctex_bias == doctest::Approx(HalfToFloat(0x3C01)));

    // nothing dirty: nothing written, nothing marked
    state->ClearDirty();
    state->regs[kRegNoiseFrequency] = kHalfOne | (kHalfOne << 16);  // changed behind the flags' back
    UniformDirty clean;
    FillUniforms(*state, block, clean, {});
    CHECK_FALSE(clean.changed);
    CHECK(block.uniforms.proctex_noise_f.x == doctest::Approx(2.0f));

    // each of the six registers (0xA8 to 0xAD) dirties them; one outside does not
    for (u32 id = kRegProcTex; id <= kRegLutOffset; ++id) {
        state->ClearDirty();
        state->dirty.Set(id);
        UniformDirty one;
        FillUniforms(*state, block, one, {});
        // the first one that runs sees the new frequency and writes it; the later ones write the same value and mark nothing
        if (id == kRegProcTex) {
            CHECK(one.changed);
            CHECK(block.uniforms.proctex_noise_f.x == doctest::Approx(1.0f));
        } else {
            CHECK_FALSE(one.changed);
        }
    }
    state->ClearDirty();
    state->dirty.Set(kRegLutOffset + 1);
    state->regs[kRegNoiseFrequency] = kHalfTwo | (kHalfTwo << 16);
    UniformDirty outside;
    FillUniforms(*state, block, outside, {});
    CHECK_FALSE(outside.changed);
    CHECK(block.uniforms.proctex_noise_f.x == doctest::Approx(1.0f));
}

TEST_CASE("proctex/the_ubershader_calls_each_heavy_helper_as_often_as_azahars_text_does_for_one_configuration") {
    const std::pair<const DeviceProfile*, const char*> profiles[] = {{&kFullProfile, "kFullProfile"}, {&kLeanProfile, "kLeanProfile"}};
    const std::pair<ShaderPass, const char*> passes[] = {{ShaderPass::Normal, "Normal"}, {ShaderPass::ShadowWrite, "ShadowWrite"}};
    for (const auto& [profile, profile_name] : profiles) {
        for (const auto& [pass, pass_name] : passes) {
            const std::string where = std::string("the ") + pass_name + " pass, " + profile_name;
            const Generated uber = GenerateUber(pass, *profile);
            REQUIRE_MESSAGE(uber.ok(), where << ": " << uber.error);
            const std::string& g = uber.glsl;
            if (ReportOn()) std::printf("proctex ubershader %s: %zu SPIR-V words\n", where.c_str(), uber.spirv.size());

            const std::string proctex = BodyOf(g, "vec4 ProcTex() {");
            const std::string noise = BodyOf(g, "float ProcTexNoiseCoef(vec2 x) {");
            const std::string unit3 = BodyOf(g, "vec4 sampleTexUnit3() {");
            REQUIRE_MESSAGE(!proctex.empty(), where << ": no definition of ProcTex");
            REQUIRE_MESSAGE(!noise.empty(), where << ": no definition of ProcTexNoiseCoef");
            REQUIRE_MESSAGE(!unit3.empty(), where << ": no definition of sampleTexUnit3");

            // ProcTex: one call, the one in sampleTexUnit3 (the vacuous cases fail: a name that is not found at all has a count of 0)
            CHECK_MESSAGE(CountCalls(g, "ProcTex") == 1, where << ": ProcTex() is called " << CountCalls(g, "ProcTex") << " times, expected 1");
            CHECK_MESSAGE(CountCalls(unit3, "ProcTex") == 1, where << ": the call is not in sampleTexUnit3");
            // ProcTex's own body
            CHECK_MESSAGE(CountCalls(proctex, "ProcTexLookupLUT") == 2, where << ": ProcTexLookupLUT in ProcTex: " << CountCalls(proctex, "ProcTexLookupLUT"));
            CHECK_MESSAGE(proctex.find("ProcTexLookupLUT(proctex_color_map_offset") != std::string::npos, where << ": no colour-map lookup");
            CHECK_MESSAGE(proctex.find("ProcTexLookupLUT(proctex_alpha_map_offset") != std::string::npos, where << ": no alpha-map lookup");
            CHECK_MESSAGE(CountCalls(proctex, "ProcTexNoiseCoef") == 1, where << ": ProcTexNoiseCoef in ProcTex: " << CountCalls(proctex, "ProcTexNoiseCoef"));
            const size_t colors = CountCalls(g, "SampleProcTexColor");
            CHECK_MESSAGE(colors >= 1, where << ": SampleProcTexColor is never called");
            CHECK_MESSAGE(colors <= 2, where << ": SampleProcTexColor has " << colors << " call sites, expected at most 2");
            CHECK_MESSAGE(CountCalls(proctex, "SampleProcTexColor") == colors, where << ": a SampleProcTexColor call outside ProcTex");
            // the noise: four random lookups and the two noise-table lookups, once
            CHECK_MESSAGE(CountCalls(noise, "ProcTexNoiseRand2D") == 4, where << ": ProcTexNoiseRand2D in the noise: " << CountCalls(noise, "ProcTexNoiseRand2D"));
            CHECK_MESSAGE(CountCalls(noise, "ProcTexLookupLUT") == 2, where << ": ProcTexLookupLUT in the noise: " << CountCalls(noise, "ProcTexLookupLUT"));
            // and nowhere else in the text
            CHECK_MESSAGE(CountCalls(g, "ProcTexNoiseCoef") == 1, where << ": ProcTexNoiseCoef in the whole text: " << CountCalls(g, "ProcTexNoiseCoef"));
            CHECK_MESSAGE(CountCalls(g, "ProcTexNoiseRand2D") == 4, where << ": ProcTexNoiseRand2D in the whole text: " << CountCalls(g, "ProcTexNoiseRand2D"));
            CHECK_MESSAGE(CountCalls(g, "ProcTexLookupLUT") == 4, where << ": ProcTexLookupLUT in the whole text: " << CountCalls(g, "ProcTexLookupLUT"));
            // the ubershader reads the key only through cfg inside ProcTex (no literal of a specialised key)
            CHECK_MESSAGE(proctex.find("cfg[") != std::string::npos, where << ": ProcTex reads no cfg");
        }
    }
}

TEST_CASE("proctex/the_specialised_text_is_azahars_for_the_key_and_every_case_compiles_in_both_modes") {
    // a key without the procedural texture: ProcTex is defined and trivial; no helper is carried
    {
        auto state = BaseState();
        const FragmentKey key = MakeKey(*state);
        const std::string glsl = GenerateSpecialisedGlsl(key, kLeanProfile, ShaderPass::Normal);
        CHECK(glsl.find("vec4 ProcTex() { return vec4(0.0); }") != std::string::npos);
        CHECK(glsl.find("SampleProcTexColor") == std::string::npos);
        CHECK(glsl.find("ProcTexNoiseCoef") == std::string::npos);
    }
    // a key with it: Azahar's text for its configuration. No noise: no noise helper; separate alpha: the alpha-map lookup
    {
        Proc p = DefaultProc();
        p.u_clamp = 3;
        p.u_shift = 1;
        p.color_combiner = 6;
        auto state = BaseState();
        Apply(*state, p);
        ApplyTev(*state, Tev::Replace1);
        const std::string glsl = GenerateSpecialisedGlsl(MakeKey(*state), kLeanProfile, ShaderPass::Normal);
        CHECK(glsl.find("ProcTexNoiseCoef") == std::string::npos);
        CHECK(glsl.find("proctex_alpha_map_offset, ") == std::string::npos);
        CHECK(glsl.find("ProcTexLookupLUT(proctex_color_map_offset, min(sqrt(u * u + v * v), 1.0))") != std::string::npos);
        CHECK(glsl.find("0.5 * float((int(uv.y) / 2) % 2)") == std::string::npos);  // the u shift's clamp is Mirrored: offset 1.0
        CHECK(glsl.find("1.0 * float((int(uv.y) / 2) % 2)") != std::string::npos);
        CHECK(glsl.find("u = int(u) % 2 == 0 ? fract(u) : 1.0 - fract(u);") != std::string::npos);
        CHECK(glsl.find("vec4 final_color = SampleProcTexColor(lut_coord, lod_i);") != std::string::npos);
        CHECK(CountCalls(glsl, "SampleProcTexColor") == 1);
        Proc q = p;
        q.noise = 1;
        q.separate_alpha = 1;
        q.alpha_combiner = 7;
        q.filter = 5;
        auto state2 = BaseState();
        Apply(*state2, q);
        ApplyTev(*state2, Tev::Replace1);
        const std::string noisy = GenerateSpecialisedGlsl(MakeKey(*state2), kLeanProfile, ShaderPass::Normal);
        CHECK(noisy.find("uv += proctex_noise_a * ProcTexNoiseCoef(uv);") != std::string::npos);
        CHECK(noisy.find("ProcTexLookupLUT(proctex_alpha_map_offset, min(u, v))") != std::string::npos);
        CHECK(CountCalls(noisy, "SampleProcTexColor") == 2);  // the two linear-mipmap levels
    }
    // every case of the GPU groups compiles (the specialised shader of its key, and the ubershader once)
    std::vector<Case> all;
    for (auto&& group : {ClampCases(), ShiftCases(), CombinerCases(), FilterCases(), CoordCases(), LutCases(), NoiseCases()}) {
        all.insert(all.end(), group.begin(), group.end());
    }
    for (const Case& c : all) {
        auto state = BaseState();
        Apply(*state, c.proc);
        ApplyTev(*state, c.tev);
        const Generated g = GenerateSpecialised(MakeKey(*state), kLeanProfile, ShaderPass::Normal);
        CHECK_MESSAGE(g.ok(), c.name << ": " << g.error);
    }
    CHECK(GenerateUber(ShaderPass::Normal, kLeanProfile).ok());
}

// ---- on a device -----------------------------------------------------------------------------------------------------------

TEST_CASE("proctex/every_clamp_mode_matches_the_software_renderers_formulas_in_both_modes") {
    RunCases(ClampCases());
}

TEST_CASE("proctex/every_shift_mode_with_both_offsets_matches_in_both_modes") {
    RunCases(ShiftCases());
}

TEST_CASE("proctex/every_combiner_for_the_colour_and_for_the_separate_alpha_matches_in_both_modes") {
    RunCases(CombinerCases());
}

TEST_CASE("proctex/every_filter_matches_the_software_renderer_and_the_glsls_mipmap_levels_in_both_modes") {
    RunCases(FilterCases());
}

TEST_CASE("proctex/the_coordinate_choice_the_lut_width_and_offset_and_the_noise_uniforms_match_in_both_modes") {
    RunCases(CoordCases());
    RunCases(LutCases());
    RunCases(NoiseCases());
}

TEST_CASE("proctex/upload_tables_uploads_exactly_the_tables_need_names_and_nothing_else") {
    Rigs* rigs = RequireRigs();
    if (rigs == nullptr) return;
    rhi::Backend& backend = *rigs->backend;
    auto expect_value_table = [&](TableRings& rings, u64 byte_offset, const std::array<u32, 128>& table) {
        std::vector<u8> bytes(table.size() * sizeof(Vec2f));
        REQUIRE(backend.ReadBuffer(rings.Buffer(TableRing::General), byte_offset, std::span<u8>(bytes)).ok());
        for (size_t i = 0; i < table.size(); ++i) {
            Vec2f entry;
            std::memcpy(&entry, bytes.data() + i * sizeof(Vec2f), sizeof(entry));
            REQUIRE(entry.x == doctest::Approx(ValueOf(table[i])));
            REQUIRE(entry.y == doctest::Approx(DiffOf(table[i])));
        }
    };
    auto expect_color_table = [&](TableRings& rings, u64 byte_offset, const std::array<u32, 256>& table, bool difference, const Proc& p) {
        std::vector<u8> bytes(table.size() * sizeof(Vec4f));
        REQUIRE(backend.ReadBuffer(rings.Buffer(TableRing::General), byte_offset, std::span<u8>(bytes)).ok());
        for (size_t i = 0; i < table.size(); ++i) {
            Vec4f entry;
            std::memcpy(&entry, bytes.data() + i * sizeof(Vec4f), sizeof(entry));
            const std::array<float, 4> want = difference ? DiffBytes(p, static_cast<int>(i)) : ColorBytes(p, static_cast<int>(i));
            REQUIRE(entry.x == doctest::Approx(want[0] / 255.0f));
            REQUIRE(entry.y == doctest::Approx(want[1] / 255.0f));
            REQUIRE(entry.z == doctest::Approx(want[2] / 255.0f));
            REQUIRE(entry.w == doctest::Approx(want[3] / 255.0f));
        }
    };
    const Proc p = DefaultProc();
    constexpr int kSentinel = -12345;
    auto sentinel_block = [&] {
        FragmentBlock block;
        std::memset(&block, 0, sizeof(block));
        block.uniforms.proctex_noise_lut_offset = block.uniforms.proctex_color_map_offset = block.uniforms.proctex_alpha_map_offset =
            block.uniforms.proctex_lut_offset = block.uniforms.proctex_diff_lut_offset = kSentinel;
        return block;
    };
    auto state = BaseState();
    Apply(*state, p);

    // a `need` that names one table uploads that one and changes only its offset, whatever the state's flags say
    for (u32 table : {0u, 2u, 3u, 4u, 5u}) {
        auto rings = TableRings::Create(backend);
        REQUIRE_MESSAGE(rings.ok(), rings.error());
        state->proctex.table_dirty = 0;  // the flags say nothing changed
        FragmentBlock block = sentinel_block();
        UniformDirty dirty;
        TableDirty need;
        need.proctex = static_cast<u8>(1u << table);
        rhi::Recorder rec = backend.BeginRecording();
        auto summary = UploadTables(*state, **rings, block, dirty, need, FeatureRegistry::Global());
        REQUIRE_MESSAGE(summary.ok(), summary.error());
        backend.WaitIdle(backend.Submit(std::move(rec)));
        CHECK(summary->uploaded);
        CHECK(dirty.changed);
        const int offsets[6] = {block.uniforms.proctex_noise_lut_offset, kSentinel, block.uniforms.proctex_color_map_offset,
                                block.uniforms.proctex_alpha_map_offset, block.uniforms.proctex_lut_offset,
                                block.uniforms.proctex_diff_lut_offset};
        for (u32 other : {0u, 2u, 3u, 4u, 5u}) {
            if (other == table) {
                CHECK_MESSAGE(offsets[other] != kSentinel, "table " << table << " was not given an offset");
            } else {
                CHECK_MESSAGE(offsets[other] == kSentinel, "table " << table << " changed the offset of table " << other);
            }
        }
        // the table's contents sit at the offset (the first slice of a fresh ring starts at 0)
        CHECK(offsets[table] == 0);
        if (table == 0) expect_value_table(**rings, 0, p.noise_table);
        if (table == 2) expect_value_table(**rings, 0, p.color_map);
        if (table == 3) expect_value_table(**rings, 0, p.alpha_map);
        if (table == 4) expect_color_table(**rings, 0, p.color_table, false, p);
        if (table == 5) expect_color_table(**rings, 0, p.color_diff, true, p);
    }

    // a `need` of nothing uploads nothing and opens no session, even when every flag of the state is set
    {
        auto rings = TableRings::Create(backend);
        REQUIRE(rings.ok());
        state->proctex.table_dirty = pica::ProcTex::kAllDirty;
        FragmentBlock block = sentinel_block();
        UniformDirty dirty;
        rhi::Recorder rec = backend.BeginRecording();
        auto summary = UploadTables(*state, **rings, block, dirty, TableDirty{}, FeatureRegistry::Global());
        REQUIRE(summary.ok());
        backend.WaitIdle(backend.Submit(std::move(rec)));
        CHECK_FALSE(summary->uploaded);
        CHECK_FALSE(dirty.changed);
        CHECK(block.uniforms.proctex_noise_lut_offset == kSentinel);
        CHECK(block.uniforms.proctex_lut_offset == kSentinel);
    }

    // every bit: all five tables in Azahar's layout, 11,264 bytes (kGeneralRingMaxUpload), in a fresh ring
    {
        auto rings = TableRings::Create(backend);
        REQUIRE(rings.ok());
        state->proctex.table_dirty = 0;
        FragmentBlock block = sentinel_block();
        UniformDirty dirty;
        TableDirty need;
        need.proctex = pica::ProcTex::kAllDirty;
        rhi::Recorder rec = backend.BeginRecording();
        auto summary = UploadTables(*state, **rings, block, dirty, need, FeatureRegistry::Global());
        REQUIRE(summary.ok());
        backend.WaitIdle(backend.Submit(std::move(rec)));
        CHECK(summary->uploaded);
        CHECK(block.uniforms.proctex_noise_lut_offset == 0);
        CHECK(block.uniforms.proctex_color_map_offset == 128);   // 1,024 / 8
        CHECK(block.uniforms.proctex_alpha_map_offset == 256);
        CHECK(block.uniforms.proctex_lut_offset == 192);         // 3,072 / 16
        CHECK(block.uniforms.proctex_diff_lut_offset == 192 + 256);
        const u64 end = static_cast<u64>(block.uniforms.proctex_diff_lut_offset) * 16 + 256 * 16;
        CHECK(end == kGeneralRingMaxUpload);
        CHECK(end == 11264);
        expect_value_table(**rings, 0, p.noise_table);
        expect_value_table(**rings, 1024, p.color_map);
        expect_value_table(**rings, 2048, p.alpha_map);
        expect_color_table(**rings, 3072, p.color_table, false, p);
        expect_color_table(**rings, 3072 + 4096, p.color_diff, true, p);

        // and again with the same `need`: uploaded again (no "already uploaded" state), where the ring puts it
        FragmentBlock again = sentinel_block();
        UniformDirty dirty_again;
        rhi::Recorder rec2 = backend.BeginRecording();
        auto second = UploadTables(*state, **rings, again, dirty_again, need, FeatureRegistry::Global());
        REQUIRE(second.ok());
        backend.WaitIdle(backend.Submit(std::move(rec2)));
        CHECK(second->uploaded);
        CHECK(dirty_again.changed);
        CHECK(again.uniforms.proctex_noise_lut_offset != kSentinel);
        CHECK(again.uniforms.proctex_diff_lut_offset != kSentinel);
        // (the first recording has completed, so the ring may hand out the same space again: the offsets need not differ)
        const u64 first = static_cast<u64>(again.uniforms.proctex_noise_lut_offset) * 8;
        expect_value_table(**rings, first, p.noise_table);
        expect_color_table(**rings, static_cast<u64>(again.uniforms.proctex_diff_lut_offset) * 16, p.color_diff, true, p);
    }
}

TEST_CASE("proctex/zz_release_the_shared_rigs") {
    SharedRigs().reset();
    CHECK(true);
}
