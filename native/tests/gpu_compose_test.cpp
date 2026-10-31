// The composition's slots (N2-33), on whatever Vulkan device the machine has (lavapipe in the cloud and in Linux CI):
// a draw through each slot with hand-written shaders (native/tests/shaders/gpu_compose, built with tools/native/
// spirv_inc.py into gpu_compose_shaders.inc) and the expected pixels worked out by hand; the default pipeline provider
// (the build on the first miss, counted once; `force_ubershader`); the fragment uniform ring, the lookup-table rings and
// the binding sets that are reused. A case that needs a device skips with a message without one, and fails under
// MH4N_REQUIRE_VULKAN=1.
//
// Every shader draws a full-screen triangle into a 4x4 target; the pixel values below are the shader's arithmetic.
#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gpu/fragment/feature.h"
#include "gpu/fragment/key.h"
#include "gpu/fragment/profile.h"
#include "gpu/fragment/state.h"
#include "gpu/fragment/tables.h"
#include "gpu/fragment/uniforms.h"
#include "gpu/native/compose.h"
#include "gpu/native/default_provider.h"
#include "gpu/pica/state.h"
#include "support/vulkan_device.h"

#include "gpu_compose_shaders.inc"

using namespace mh4n;
using namespace mh4n::gpu::fragment;
namespace re = mh4n::gpu::fragment::regenum;
namespace native = mh4n::gpu::native;
namespace rhi = mh4n::gpu::rhi;
namespace pica = mh4n::gpu::pica;
namespace test = mh4n::test;

namespace {

template <size_t N>
rhi::ShaderCode Spv(const uint32_t (&words)[N]) {
    rhi::ShaderCode code;
    code.spirv = std::span<const u32>(words, N);
    return code;
}

// Which fragment shader a key selects (through the user word of the key, which nothing else uses here).
enum Selector : u32 { kBlock = 1, kLuts = 2, kDst = 3, kShadowWrite = 4, kColorIn = 5, kTextures = 6 };

std::unique_ptr<pica::PicaState> NewState() {
    auto state = std::make_unique<pica::PicaState>();
    state->regs.fill(0);
    state->regs[freg::kLightingDisable] = 1;
    state->dirty.SetAll();
    return state;
}

std::shared_ptr<rhi::Image> MakeImage(rhi::Backend& backend, u32 width, u32 height, bool shadow = false) {
    rhi::ImageDesc desc;
    desc.format = rhi::Format::Rgba8Unorm;
    desc.width = width;
    desc.height = height;
    desc.shadow_source = shadow;
    desc.name = "compose test image";
    rhi::Result<std::shared_ptr<rhi::Image>> image = backend.CreateImage(desc);
    REQUIRE_MESSAGE(image.ok(), image.error());
    return *image;
}

rhi::ClearValue Rgba(float r, float g, float b, float a) {
    rhi::ClearValue value;
    value.color = {r, g, b, a};
    return value;
}

void Run(rhi::Backend& backend, rhi::Recorder&& rec) {
    backend.WaitIdle(backend.Submit(std::move(rec)));
}

bool AllPixels(rhi::Backend& backend, const rhi::Image& image, u32 w, u32 h, std::array<u8, 4> want) {
    rhi::Result<std::vector<u8>> bytes = backend.ReadImage(image, 0, 0, w, h);
    REQUIRE_MESSAGE(bytes.ok(), bytes.error());
    for (u32 i = 0; i < w * h; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            if ((*bytes)[i * 4 + c] != want[c]) {
                return false;
            }
        }
    }
    return true;
}

// The test features: fill the tables with known values and keep the offsets in the uniforms.
struct TableLog {
    std::vector<TableDirty> needs;
};

class LightingTables final : public FeatureModule {
public:
    explicit LightingTables(TableLog* log) : log_(log) {}
    const char* Name() const override { return "lighting"; }
    void UploadTables(TableUpload& upload) const override {
        log_->needs.push_back(upload.need);
        if (upload.need.lighting == 0) return;
        // 24 tables of 256 entries of two floats: 49,152 bytes, zero (the test reads the fog table that follows)
        for (u32 i = 0; i < kNumLightingSamplers; ++i) {
            rhi::Result<TableSlice> slice = upload.rings.Take(TableRing::Lf, 8ull * 256);
            REQUIRE(slice.ok());
            std::memset(slice->data.data(), 0, slice->data.size());
            s32* offsets = reinterpret_cast<s32*>(&upload.block.uniforms.lighting_lut_offset[i / 4]);
            offsets[i % 4] = slice->ElementOffset(8);
        }
        upload.dirty.Mark();
    }

private:
    TableLog* log_;
};

class FogTable final : public FeatureModule {
public:
    const char* Name() const override { return "fog"; }
    void UploadTables(TableUpload& upload) const override {
        if (!upload.need.fog) return;
        rhi::Result<TableSlice> slice = upload.rings.Take(TableRing::Lf, 8ull * 128);
        REQUIRE(slice.ok());
        float* f = reinterpret_cast<float*>(slice->data.data());
        for (u32 i = 0; i < 128; ++i) {
            f[2 * i] = static_cast<float>(10 + i) / 255.0f;
            f[2 * i + 1] = static_cast<float>(20 + i) / 255.0f;
        }
        upload.block.uniforms.fog_lut_offset = slice->ElementOffset(8);
        upload.dirty.Mark();
    }
};

class ProcTexTables final : public FeatureModule {
public:
    const char* Name() const override { return "proctex"; }
    void UploadTables(TableUpload& upload) const override {
        if (upload.need.proctex == 0) return;
        auto value_table = [&](float base_x, float base_y, int& offset) {
            rhi::Result<TableSlice> slice = upload.rings.Take(TableRing::General, 8ull * 128);
            REQUIRE(slice.ok());
            float* f = reinterpret_cast<float*>(slice->data.data());
            for (u32 i = 0; i < 128; ++i) {
                f[2 * i] = (base_x + static_cast<float>(i)) / 255.0f;
                f[2 * i + 1] = (base_y + static_cast<float>(i)) / 255.0f;
            }
            offset = slice->ElementOffset(8);
        };
        value_table(30, 40, upload.block.uniforms.proctex_noise_lut_offset);
        value_table(31, 41, upload.block.uniforms.proctex_color_map_offset);
        value_table(32, 42, upload.block.uniforms.proctex_alpha_map_offset);
        auto color_table = [&](int& offset) {
            rhi::Result<TableSlice> slice = upload.rings.Take(TableRing::General, 16ull * 256);
            REQUIRE(slice.ok());
            float* f = reinterpret_cast<float*>(slice->data.data());
            for (u32 i = 0; i < 256; ++i) {
                f[4 * i + 0] = static_cast<float>(50 + (i % 100)) / 255.0f;
                f[4 * i + 1] = static_cast<float>(60 + (i % 100)) / 255.0f;
                f[4 * i + 2] = static_cast<float>(70 + (i % 100)) / 255.0f;
                f[4 * i + 3] = static_cast<float>(80 + (i % 100)) / 255.0f;
            }
            offset = slice->ElementOffset(16);
        };
        color_table(upload.block.uniforms.proctex_lut_offset);
        color_table(upload.block.uniforms.proctex_diff_lut_offset);
        upload.dirty.Mark();
    }
};

// Everything one case needs.
struct Rig {
    std::shared_ptr<rhi::Backend> backend;
    std::vector<native::PipelineKey> built;
    native::FixupGeneratorRegistry fixup;
    std::unique_ptr<native::DefaultPipelineProvider> provider;
    std::unique_ptr<native::DrawComposer> composer;
    std::unique_ptr<FragmentUniformRing> uniforms;
    FeatureRegistry features;
    DeviceProfile profile;
    std::unique_ptr<FragmentState> fragment;

    explicit Rig(std::shared_ptr<rhi::Backend> b, bool force_ubershader = false) : backend(std::move(b)) {
        profile = ProfileOf(backend->Caps());
        provider = std::make_unique<native::DefaultPipelineProvider>(
            [this](const native::PipelineKey& key) { return Build(key); }, native::DefaultProviderOptions{force_ubershader});
        rhi::Result<std::unique_ptr<native::DrawComposer>> made = native::DrawComposer::Create(backend, *provider);
        REQUIRE_MESSAGE(made.ok(), made.error());
        composer = std::move(*made);
        composer->SetFixupRegistry(&fixup);
        rhi::Result<std::unique_ptr<FragmentUniformRing>> ring = FragmentUniformRing::Create(*backend);
        REQUIRE_MESSAGE(ring.ok(), ring.error());
        uniforms = std::move(*ring);
        fragment = std::make_unique<FragmentState>(profile, features);
    }

    // The builder the default provider calls on a miss: the test's hand-written shaders, with the parts N2-33 owns from
    // BaseDescOf.
    rhi::Result<std::shared_ptr<rhi::Pipeline>> Build(const native::PipelineKey& key) {
        built.push_back(key);
        rhi::GraphicsPipelineDesc desc = native::BaseDescOf(key, *backend);
        desc.vertex = Spv(spv_vert_red);
        if (key.IsFallback()) {
            desc.fragment = Spv(spv_frag_uber);
        } else {
            switch (key.fragment.key.user.raw) {
            case kBlock: desc.fragment = Spv(spv_frag_block); break;
            case kLuts: desc.fragment = Spv(spv_frag_luts); break;
            case kDst: desc.fragment = Spv(spv_frag_dst); break;
            case kShadowWrite: desc.fragment = Spv(spv_frag_shadow_write); break;
            case kColorIn: desc.fragment = Spv(spv_frag_color_in); break;
            case kTextures: desc.fragment = Spv(spv_frag_textures); break;
            default: return rhi::Error{"the test has no shader for this key"};
            }
        }
        if (key.fixup == native::FixupStage::FixedGeometry) {
            desc.geometry = Spv(spv_geom_pass);
        }
        return backend->CreateGraphicsPipeline(desc);
    }

    // One draw description for the current fragment state, selecting a shader.
    native::DrawDesc Desc(const FragmentKey& key, rhi::Image* target) {
        native::DrawDesc d;
        d.vertex.kind = native::VertexStageKind::Translated;
        d.vertex.program = 1;
        d.fragment = &key;
        d.needs = fragment->needs();
        d.target.color = target;
        d.target.area = rhi::Rect{0, 0, target != nullptr ? target->Width() : 0, target != nullptr ? target->Height() : 0};
        d.color_format = rhi::Format::Rgba8Unorm;
        d.count = 3;
        return d;
    }
};

rhi::TargetPassDesc ClearPass(rhi::Image* image, float r, float g, float b, float a) {
    rhi::TargetPassDesc pass;
    pass.color = image;
    pass.color_load = rhi::LoadOp::Clear;
    pass.clear_color = {r, g, b, a};
    pass.area = rhi::Rect{0, 0, image->Width(), image->Height()};
    return pass;
}

}  // namespace

// ---- the normal pass, the uniform block and the tables ----------------------------------------------------------------

TEST_CASE("compose/a_normal_draw_reads_the_fragment_block_and_the_key") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend);
    auto state = NewState();
    state->regs[0xC3] = 0xFF302010u;  // stage 0 constant: r 0x10, g 0x20, b 0x30, a 0xFF
    auto target = MakeImage(*backend, 4, 4);
    // the vertex stage's uniform buffer, given by the caller (set 0, binding 1): the shader does not read it
    rhi::Result<std::unique_ptr<rhi::Ring>> vs_ring = backend->CreateRing(4096, rhi::BufferUsage::Uniform, "test vs uniforms");
    REQUIRE(vs_ring.ok());

    rhi::Recorder rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
    REQUIRE(prepared.ok());
    rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
    REQUIRE_MESSAGE(alloc.ok(), alloc.error());
    CHECK(alloc->offset % 256 == 0);
    rhi::Result<rhi::RingAllocation> vs = (*vs_ring)->Map(32, backend->Caps().uniform_buffer_offset_alignment);
    REQUIRE(vs.ok());
    std::memset(vs->data.data(), 0, 32);
    REQUIRE((*vs_ring)->Commit(32).ok());

    FragmentKey key = rig.fragment->key();
    key.user.raw = kBlock;
    native::DrawDesc d = rig.Desc(key, target.get());
    d.fragment_uniforms = *alloc;
    d.vs_uniforms = native::UniformRange{(*vs_ring)->GetBuffer().get(), vs->offset, 32};
    rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 0, 0, 0, 0));
    rhi::Result<native::DrawResult> result = rig.composer->Draw(rec, d);
    REQUIRE_MESSAGE(result.ok(), result.error());
    CHECK_FALSE(result->fallback);
    Run(*backend, std::move(rec));

    // const_color[0] = (16, 32, 48) / 255 and the alpha is the low byte of the key's first word: the framebuffer part's raw
    // word of zero registers is 193 (Always = 1, Copy = 3 << 6)
    CHECK(AllPixels(*backend, *target, 4, 4, {16, 32, 48, 193}));
    CHECK(rig.built.size() == 1);
    CHECK(rig.built[0].pass == native::PassKind::Normal);
}

TEST_CASE("compose/texel_buffers_are_read_at_the_offsets_the_uniforms_keep_and_a_wrap_uploads_everything_again") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend);
    TableLog log;
    LightingTables lighting(&log);
    FogTable fog;
    ProcTexTables proctex;
    REQUIRE(rig.features.Add(lighting));
    REQUIRE(rig.features.Add(fog));
    REQUIRE(rig.features.Add(proctex));
    // rings that hold two worst-case uploads
    TableRingsDesc ring_desc;
    ring_desc.lf_bytes = 2 * kLfRingMaxUpload;
    ring_desc.general_bytes = 2 * kGeneralRingMaxUpload;
    rhi::Result<std::unique_ptr<TableRings>> rings = TableRings::Create(*backend, ring_desc);
    REQUIRE_MESSAGE(rings.ok(), rings.error());
    auto state = NewState();
    auto target = MakeImage(*backend, 4, 4);

    // One draw: the state's table flags as given, a recorder of its own or the one given.
    auto draw = [&](rhi::Recorder& rec) {
        rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, rings->get());
        REQUIRE_MESSAGE(prepared.ok(), prepared.error());
        CHECK(prepared->tables_uploaded);
        rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
        REQUIRE(alloc.ok());
        FragmentKey key = rig.fragment->key();
        key.user.raw = kLuts;
        native::DrawDesc d = rig.Desc(key, target.get());
        d.fragment_uniforms = *alloc;
        d.tables = rings->get();
        rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 0, 0, 0, 0));
        rhi::Result<native::DrawResult> result = rig.composer->Draw(rec, d);
        REQUIRE_MESSAGE(result.ok(), result.error());
    };
    // Reads the target after the recorder ran. The fog table's entry 0 at the offset: x = 10; the noise table's entry 1: y = 40 + 1;
    // the colour table's entry 2: z = 70 + 2 and w = 80 + 2.
    const std::array<u8, 4> want = {10, 41, 72, 82};

    // First recorder: a full upload, then a draw that finds one lighting table dirty (the rings have room for a second session).
    state->lighting.lut_dirty = pica::Lighting::kAllDirty;
    state->fog.lut_dirty = true;
    state->proctex.table_dirty = pica::ProcTex::kAllDirty;
    rhi::Recorder rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    draw(rec);
    // the lighting tables come first in the ring (24 x 2,048 bytes = 6,144 elements of 8 bytes), the colour table after three
    // value tables (3 x 1,024 bytes = 192 elements of 16 bytes): the offsets are not zero
    CHECK(rig.fragment->block().uniforms.fog_lut_offset == 6144);
    CHECK(rig.fragment->block().uniforms.proctex_noise_lut_offset == 0);
    CHECK(rig.fragment->block().uniforms.proctex_color_map_offset == 128);  // 1,024 / 8
    CHECK(rig.fragment->block().uniforms.proctex_alpha_map_offset == 256);
    CHECK(rig.fragment->block().uniforms.proctex_lut_offset == 192);        // 3,072 / 16
    CHECK(rig.fragment->block().uniforms.proctex_diff_lut_offset == 192 + 256);
    CHECK(rig.fragment->block().uniforms.lighting_lut_offset[1].y == 256 * 5);  // table 5, 2,048 bytes each = 256 elements
    state->dirty.Reset();
    state->lighting.lut_dirty = 1u;
    state->fog.lut_dirty = false;
    state->proctex.table_dirty = 0;
    draw(rec);
    // the second session started after the first (50,176 bytes = 6,272 elements): table 0 moved there, and the fog table, not
    // uploaded again, is still where it was
    CHECK(rig.fragment->block().uniforms.lighting_lut_offset[0].x == 6272);
    CHECK(rig.fragment->block().uniforms.fog_lut_offset == 6144);
    Run(*backend, std::move(rec));
    CHECK(AllPixels(*backend, *target, 4, 4, want));
    CHECK(rings->get()->Wraps(TableRing::Lf) == 0);

    // Second recorder, the ring empty again: its next session starts at the beginning, which is a wrap, so the whole ring is
    // uploaded again although only one table is dirty
    rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    state->dirty.Reset();
    state->lighting.lut_dirty = 1u;
    draw(rec);
    Run(*backend, std::move(rec));
    CHECK(AllPixels(*backend, *target, 4, 4, want));
    CHECK(rings->get()->Wraps(TableRing::Lf) == 1);
    CHECK(rings->get()->Wraps(TableRing::General) == 0);  // never begun: nothing of the procedural tables was dirty
    CHECK(rig.fragment->block().uniforms.fog_lut_offset == 6144);
    CHECK(rig.fragment->block().uniforms.lighting_lut_offset[0].x == 0);

    REQUIRE(log.needs.size() == 3);
    CHECK(log.needs[0].lighting == pica::Lighting::kAllDirty);
    CHECK(log.needs[0].fog);
    CHECK(log.needs[0].proctex == pica::ProcTex::kAllDirty);
    CHECK(log.needs[1].lighting == 1u);
    CHECK_FALSE(log.needs[1].fog);
    CHECK(log.needs[1].proctex == 0);
    CHECK(log.needs[2].lighting == pica::Lighting::kAllDirty);  // widened by the wrap
    CHECK(log.needs[2].fog);
    CHECK(log.needs[2].proctex == 0);
}

TEST_CASE("tables/a_wrapped_ring_makes_every_table_of_it_dirty_again") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    TableLog log;
    LightingTables lighting(&log);
    FogTable fog;
    FeatureRegistry features;
    features.Add(lighting);
    features.Add(fog);
    TableRingsDesc ring_desc;
    ring_desc.lf_bytes = 2 * kLfRingMaxUpload;
    rhi::Result<std::unique_ptr<TableRings>> rings = TableRings::Create(*backend, ring_desc);
    REQUIRE(rings.ok());
    auto state = NewState();
    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    state->lighting.lut_dirty = 1u << 3;  // always a single dirty table
    state->fog.lut_dirty = false;
    state->proctex.table_dirty = 0;

    // two sessions in one recorder fit the ring; a third, in a later recorder, restarts at the beginning
    rhi::Recorder rec = backend->BeginRecording();
    for (int i = 0; i < 2; ++i) {
        UniformDirty dirty;
        rhi::Result<UploadSummary> summary = UploadTables(*state, **rings, block, dirty, features);
        REQUIRE_MESSAGE(summary.ok(), summary.error());
        CHECK(summary->uploaded);
        CHECK_FALSE(summary->lf_wrapped);
        CHECK_FALSE(summary->general_wrapped);
        CHECK(dirty.changed);
    }
    Run(*backend, std::move(rec));
    rec = backend->BeginRecording();
    UniformDirty dirty;
    rhi::Result<UploadSummary> summary = UploadTables(*state, **rings, block, dirty, features);
    REQUIRE_MESSAGE(summary.ok(), summary.error());
    CHECK(summary->lf_wrapped);
    CHECK_FALSE(summary->general_wrapped);
    Run(*backend, std::move(rec));
    REQUIRE(log.needs.size() == 3);
    CHECK(log.needs[0].lighting == (1u << 3));
    CHECK(log.needs[1].lighting == (1u << 3));
    CHECK(log.needs[2].lighting == pica::Lighting::kAllDirty);
    CHECK(log.needs[2].fog);

    // nothing dirty: no session at all
    state->lighting.lut_dirty = 0;
    rec = backend->BeginRecording();
    rhi::Result<UploadSummary> idle = UploadTables(*state, **rings, block, dirty, features);
    REQUIRE(idle.ok());
    CHECK_FALSE(idle->uploaded);
    CHECK(log.needs.size() == 3);
    Run(*backend, std::move(rec));
}

// ---- the shadow-write pass and the pass switches ------------------------------------------------------------------------

TEST_CASE("compose/a_shadow_write_pass_stores_into_the_r32_uint_view_and_passes_switch_both_ways") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend);
    auto state = NewState();
    auto target = MakeImage(*backend, 4, 4);
    auto shadow = MakeImage(*backend, 4, 4, true);

    rhi::Recorder rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    rec.ClearImage(*shadow, Rgba(0, 0, 0, 0));
    rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
    REQUIRE(prepared.ok());
    rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
    REQUIRE(alloc.ok());

    // normal, shadow write, normal
    FragmentKey normal_key = rig.fragment->key();
    normal_key.user.raw = kColorIn;
    FragmentKey shadow_key = rig.fragment->key();
    shadow_key.user.raw = kShadowWrite;

    native::DrawDesc normal = rig.Desc(normal_key, target.get());
    normal.fragment_uniforms = *alloc;
    native::DrawDesc write = rig.Desc(shadow_key, nullptr);
    write.fragment_uniforms = *alloc;
    write.needs.shadow_write_pass = true;
    write.shadow_target = shadow.get();
    write.color_format = rhi::Format::Undefined;

    rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 0, 0, 0, 0));
    REQUIRE(rig.composer->Draw(rec, normal).ok());
    rhi::Result<native::DrawResult> r2 = rig.composer->Draw(rec, write);
    REQUIRE_MESSAGE(r2.ok(), r2.error());
    REQUIRE(rig.composer->Draw(rec, write).ok());  // a second draw continues the same pass and reuses the sets
    rhi::Result<native::DrawResult> r4 = rig.composer->Draw(rec, normal);
    REQUIRE_MESSAGE(r4.ok(), r4.error());
    Run(*backend, std::move(rec));

    CHECK(AllPixels(*backend, *target, 4, 4, {255, 0, 0, 255}));
    // every R32_UINT texel is 7: the RGBA8 bytes 07 00 00 00 (little endian)
    CHECK(AllPixels(*backend, *shadow, 4, 4, {7, 0, 0, 0}));
    CHECK(rig.composer->counters().pass_begins == 3);  // target, no attachments, target
    // the two keys: Normal and ShadowWrite, the second with no attachment formats
    REQUIRE(rig.built.size() == 2);
    CHECK(rig.built[0].pass == native::PassKind::Normal);
    CHECK(rig.built[1].pass == native::PassKind::ShadowWrite);
    CHECK(rig.built[1].color_format == rhi::Format::Undefined);
}

// ---- the destination copy ---------------------------------------------------------------------------------------------------

TEST_CASE("compose/a_draw_after_the_destination_copy_reads_tex_color") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend);
    auto state = NewState();
    auto target = MakeImage(*backend, 4, 4);

    rhi::Recorder rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
    REQUIRE(prepared.ok());
    rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
    REQUIRE(alloc.ok());
    FragmentKey key = rig.fragment->key();
    key.user.raw = kDst;
    native::DrawDesc d = rig.Desc(key, target.get());
    d.fragment_uniforms = *alloc;
    d.needs.dst_copy = true;
    rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 1, 0, 0, 1));  // red
    rhi::Result<native::DrawResult> result = rig.composer->Draw(rec, d);
    REQUIRE_MESSAGE(result.ok(), result.error());
    CHECK(result->dst_copied);
    Run(*backend, std::move(rec));
    // the shader wrote the destination (red) plus (0, 0.4, 0, 0): green 0.4 * 255 = 102
    CHECK(AllPixels(*backend, *target, 4, 4, {255, 102, 0, 255}));
    CHECK(rig.composer->counters().dst_copies == 1);
}

// ---- the fix-up stage -------------------------------------------------------------------------------------------------------

TEST_CASE("compose/the_fixup_stage_is_in_the_pipeline_only_when_it_is_needed_possible_and_generated") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    if (!backend->Caps().geometry_shader) {
        test::SkipCase("the device has no geometry shader");
        return;
    }
    Rig rig(backend);
    auto state = NewState();
    auto target = MakeImage(*backend, 4, 4);

    auto draw = [&](bool needs_fixup, native::VertexStageKind kind, bool generator, native::DrawResult* out, std::string* error) {
        rig.fixup.Set(generator ? native::FixupGenerator([](const native::VertexStageOutputs&) { return std::vector<u32>{1}; })
                                : native::FixupGenerator{});
        rhi::Recorder rec = backend->BeginRecording();
        rig.composer->BeginRecording(rec);
        rig.uniforms->BeginRecording();
        rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
        REQUIRE(prepared.ok());
        rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
        REQUIRE(alloc.ok());
        FragmentKey key = rig.fragment->key();
        key.user.raw = kColorIn;
        native::DrawDesc d = rig.Desc(key, target.get());
        d.fragment_uniforms = *alloc;
        d.needs.fixup_gs = needs_fixup;
        d.vertex.kind = kind;
        rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 0, 0, 0, 0));
        rhi::Result<native::DrawResult> result = rig.composer->Draw(rec, d);
        if (!result.ok()) *error = result.error();
        else *out = *result;
        Run(*backend, std::move(rec));
    };

    native::DrawResult result;
    std::string error;
    // needed, translated vertex stage, geometry shaders available, generator registered: the stage is in the pipeline
    draw(true, native::VertexStageKind::Translated, true, &result, &error);
    REQUIRE_MESSAGE(error.empty(), error);
    CHECK(result.fixup);
    CHECK_FALSE(result.fixup_stubbed);
    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].fixup == native::FixupStage::FixedGeometry);
    CHECK(AllPixels(*backend, *target, 4, 4, {0, 255, 0, 255}));  // the pass-through stage wrote green

    // the same draw with the field None: the vertex stage's red
    draw(false, native::VertexStageKind::Translated, true, &result, &error);
    REQUIRE_MESSAGE(error.empty(), error);
    CHECK_FALSE(result.fixup);
    CHECK_FALSE(result.fixup_stubbed);
    REQUIRE(rig.built.size() == 2);
    CHECK(rig.built[1].fixup == native::FixupStage::None);
    CHECK(AllPixels(*backend, *target, 4, 4, {255, 0, 0, 255}));

    // needed but no generator is registered: the field stays None and the draw is counted stubbed:fixup
    draw(true, native::VertexStageKind::Translated, false, &result, &error);
    REQUIRE_MESSAGE(error.empty(), error);
    CHECK_FALSE(result.fixup);
    CHECK(result.fixup_stubbed);
    CHECK(rig.composer->counters().stubbed_fixup == 1);
    CHECK(rig.built.size() == 2);  // the key is the same as the second one: a hit
    CHECK(AllPixels(*backend, *target, 4, 4, {255, 0, 0, 255}));
    std::vector<native::FrameCounter> counters = rig.composer->TakeFrameCounters();
    bool seen = false;
    for (const native::FrameCounter& c : counters) {
        if (c.name == "stubbed:fixup") {
            seen = true;
            CHECK(c.value == 1);
        }
    }
    CHECK(seen);
    for (const native::FrameCounter& c : rig.composer->TakeFrameCounters()) CHECK(c.value == 0);  // reading resets

    // a trivial vertex stage (the CPU path's batches) has nothing to fix up
    draw(true, native::VertexStageKind::Trivial, true, &result, &error);
    REQUIRE_MESSAGE(error.empty(), error);
    CHECK_FALSE(result.fixup);
    CHECK_FALSE(result.fixup_stubbed);
}

TEST_CASE("compose/the_fixup_rule_on_synthetic_values") {
    // The M2's route (no geometry shader) cannot be run on lavapipe through the mask until N2-34's bit exists, so the rule is
    // checked on values: wanted by lighting, a translated vertex stage, a device with geometry shaders, a generator.
    using native::DecideFixup;
    using native::FixupDecision;
    using native::VertexStageKind;
    const VertexStageKind translated = VertexStageKind::Translated, trivial = VertexStageKind::Trivial;
    CHECK(DecideFixup(true, translated, true, true) == FixupDecision::Applied);
    CHECK(DecideFixup(true, translated, true, false) == FixupDecision::Stubbed);   // counted stubbed:fixup
    CHECK(DecideFixup(true, translated, false, true) == FixupDecision::None);      // the M2: no fix-up
    CHECK(DecideFixup(true, translated, false, false) == FixupDecision::None);
    CHECK(DecideFixup(true, trivial, true, true) == FixupDecision::None);          // the CPU path does the fix-up
    CHECK(DecideFixup(false, translated, true, true) == FixupDecision::None);      // lighting is off
}

// ---- the textures -----------------------------------------------------------------------------------------------------------

TEST_CASE("compose/unit_zero_goes_to_the_class_its_type_names_and_the_other_classes_are_dummies") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend);
    auto state = NewState();
    auto target = MakeImage(*backend, 4, 4);
    auto t0 = MakeImage(*backend, 1, 1), t1 = MakeImage(*backend, 1, 1), t2 = MakeImage(*backend, 1, 1);
    auto shadow = MakeImage(*backend, 1, 1, true);

    rhi::Recorder rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    rec.ClearImage(*t0, Rgba(0.2f, 0, 0, 1));
    rec.ClearImage(*t1, Rgba(0.4f, 0, 0, 1));
    rec.ClearImage(*t2, Rgba(0.6f, 0, 0, 1));
    rec.ClearImage(*shadow, Rgba(1, 1, 1, 1));
    rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
    REQUIRE(prepared.ok());
    rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
    REQUIRE(alloc.ok());

    FragmentKey key2d = rig.fragment->key();
    key2d.user.raw = kTextures;
    native::DrawDesc d = rig.Desc(key2d, target.get());
    d.fragment_uniforms = *alloc;
    d.units[0].image = t0.get();
    d.units[1].image = t1.get();
    d.units[2].image = t2.get();
    rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 0, 0, 0, 0));
    REQUIRE(rig.composer->Draw(rec, d).ok());
    Run(*backend, std::move(rec));
    // a = 0.2 * 255 = 51, b = 102, c = 153; the flag is 1 when the cube and shadow dummies are bound (white)
    CHECK(AllPixels(*backend, *target, 4, 4, {51, 102, 153, 255}));

    // the same units with unit 0 a Shadow2D texture: the 2D class is now the white dummy
    rhi::Recorder rec2 = backend->BeginRecording();
    rig.composer->BeginRecording(rec2);
    rig.uniforms->BeginRecording();
    FragmentKey key_shadow = key2d;
    key_shadow.texture.set_texture0_type(re::TextureType::Shadow2D);
    native::DrawDesc s = rig.Desc(key_shadow, target.get());
    s.fragment_uniforms = *alloc;
    s.units[0].image = shadow.get();
    s.units[1].image = t1.get();
    s.units[2].image = t2.get();
    rig.composer->BeginTargetPass(rec2, ClearPass(target.get(), 0, 0, 0, 0));
    rhi::Result<native::DrawResult> shadow_result = rig.composer->Draw(rec2, s);
    REQUIRE_MESSAGE(shadow_result.ok(), shadow_result.error());
    Run(*backend, std::move(rec2));
    CHECK(AllPixels(*backend, *target, 4, 4, {255, 102, 153, 255}));
}

// ---- the uniform ring and the binding sets ----------------------------------------------------------------------------------

TEST_CASE("compose/a_draw_with_no_dirty_fragment_state_reuses_the_allocation_and_the_binding_sets") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend);
    auto state = NewState();
    state->regs[0xC3] = 0xFF302010u;  // const_color[0] = (16, 32, 48)
    auto target = MakeImage(*backend, 4, 4);

    rhi::Recorder rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 0, 0, 0, 0));

    u64 first_offset = 0;
    for (int draw = 0; draw < 3; ++draw) {
        if (draw > 0) state->dirty.Reset();  // the processor cleared the flags after the previous draw
        rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
        REQUIRE(prepared.ok());
        CHECK(prepared->block_changed == (draw == 0));
        rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
        REQUIRE(alloc.ok());
        CHECK(alloc->reused == (draw > 0));
        if (draw == 0) first_offset = alloc->offset;
        CHECK(alloc->offset == first_offset);
        FragmentKey key = rig.fragment->key();
        key.user.raw = kBlock;
        native::DrawDesc d = rig.Desc(key, target.get());
        d.fragment_uniforms = *alloc;
        rhi::Result<native::DrawResult> result = rig.composer->Draw(rec, d);
        REQUIRE_MESSAGE(result.ok(), result.error());
        CHECK(result->set0_reused == (draw > 0));
        CHECK(result->set1_reused == (draw > 0));
        CHECK(result->set2_reused == (draw > 0));
    }
    CHECK(rig.uniforms->allocations() == 1);
    CHECK(rig.uniforms->reuses() == 2);
    CHECK(rig.composer->counters().sets_acquired == 3);
    CHECK(rig.composer->counters().sets_reused == 6);

    // The other direction: a fourth draw in the same recording after a dirty constant colour with a new value. The block is
    // written again to another place, set 0 (which holds its offset) is acquired again, sets 1 and 2 (textures, shadow image)
    // are still the same.
    state->regs[0xC3] = 0xFF605040u;  // r 0x40, g 0x50, b 0x60
    state->dirty.Reset();
    state->dirty.Set(0xC3);
    rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
    REQUIRE(prepared.ok());
    CHECK(prepared->block_changed);
    rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
    REQUIRE(alloc.ok());
    CHECK_FALSE(alloc->reused);
    CHECK(alloc->offset != first_offset);
    FragmentKey key = rig.fragment->key();
    key.user.raw = kBlock;
    native::DrawDesc d = rig.Desc(key, target.get());
    d.fragment_uniforms = *alloc;
    rhi::Result<native::DrawResult> result = rig.composer->Draw(rec, d);
    REQUIRE_MESSAGE(result.ok(), result.error());
    CHECK_FALSE(result->set0_reused);
    CHECK(result->set1_reused);
    CHECK(result->set2_reused);
    CHECK(rig.uniforms->allocations() == 2);
    CHECK(rig.composer->counters().sets_acquired == 4);
    Run(*backend, std::move(rec));
    // the last draw's colour: the new constant, and the key's alpha byte
    CHECK(AllPixels(*backend, *target, 4, 4, {64, 80, 96, 193}));

    // a new recording forgets them: the space committed to an earlier recorder is not offered again
    rhi::Recorder rec2 = backend->BeginRecording();
    rig.composer->BeginRecording(rec2);
    rig.uniforms->BeginRecording();
    state->dirty.Reset();
    prepared = rig.fragment->Prepare(*state, {}, nullptr);
    REQUIRE(prepared.ok());
    CHECK_FALSE(prepared->block_changed);
    rhi::Result<FragmentUniformRing::Allocation> again = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
    REQUIRE(again.ok());
    CHECK_FALSE(again->reused);
    Run(*backend, std::move(rec2));
}

TEST_CASE("compose/a_texture_change_between_two_draws_acquires_set_1_only") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend);
    auto state = NewState();
    auto target = MakeImage(*backend, 4, 4);
    auto t0 = MakeImage(*backend, 1, 1), t1 = MakeImage(*backend, 1, 1), t1b = MakeImage(*backend, 1, 1), t2 = MakeImage(*backend, 1, 1);

    rhi::Recorder rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    rec.ClearImage(*t0, Rgba(0.2f, 0, 0, 1));
    rec.ClearImage(*t1, Rgba(0.4f, 0, 0, 1));
    rec.ClearImage(*t1b, Rgba(0.8f, 0, 0, 1));
    rec.ClearImage(*t2, Rgba(0.6f, 0, 0, 1));
    rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
    REQUIRE(prepared.ok());
    rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
    REQUIRE(alloc.ok());
    FragmentKey key = rig.fragment->key();
    key.user.raw = kTextures;
    native::DrawDesc d = rig.Desc(key, target.get());
    d.fragment_uniforms = *alloc;
    d.units[0].image = t0.get();
    d.units[1].image = t1.get();
    d.units[2].image = t2.get();
    rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 0, 0, 0, 0));
    rhi::Result<native::DrawResult> first = rig.composer->Draw(rec, d);
    REQUIRE_MESSAGE(first.ok(), first.error());
    CHECK_FALSE(first->set1_reused);
    // the same uniforms, another texture on unit 1: set 1 is acquired again, sets 0 and 2 are bound again as they were
    d.units[1].image = t1b.get();
    rhi::Result<native::DrawResult> second = rig.composer->Draw(rec, d);
    REQUIRE_MESSAGE(second.ok(), second.error());
    CHECK(second->set0_reused);
    CHECK_FALSE(second->set1_reused);
    CHECK(second->set2_reused);
    Run(*backend, std::move(rec));
    // unit 1 is the second draw's: 0.8 * 255 = 204; the others as before (51, 153)
    CHECK(AllPixels(*backend, *target, 4, 4, {51, 204, 153, 255}));
}

TEST_CASE("compose/a_full_uniform_ring_inside_one_recording_is_an_error_and_not_a_hang") {
    // Inside one recording every change of the fragment state takes another block of the ring, and space committed to a
    // recorder that has not been submitted cannot be waited for: a ring of two allocations fails the third write with
    // the ring's message (a 1 MiB ring holds about 585 changes).
    auto backend = test::RequireBackend();
    if (!backend) return;
    rhi::Result<std::unique_ptr<FragmentUniformRing>> made = FragmentUniformRing::Create(*backend, 2 * kFragmentBlockAllocation);
    REQUIRE_MESSAGE(made.ok(), made.error());
    FragmentUniformRing& ring = **made;
    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    rhi::Recorder rec = backend->BeginRecording();
    ring.BeginRecording();
    rhi::Result<FragmentUniformRing::Allocation> a = ring.Write(block, true);
    rhi::Result<FragmentUniformRing::Allocation> b = ring.Write(block, true);
    REQUIRE_MESSAGE(a.ok(), a.error());
    REQUIRE_MESSAGE(b.ok(), b.error());
    CHECK(a->offset == 0);
    CHECK(b->offset == kFragmentBlockAllocation);
    rhi::Result<FragmentUniformRing::Allocation> c = ring.Write(block, true);
    CHECK_FALSE(c.ok());
    CHECK(c.error().find("not been submitted") != std::string::npos);
    // an unchanged block still reuses the last allocation; after the recorder ran, the ring has room again
    CHECK(ring.Write(block, false).ok());
    Run(*backend, std::move(rec));
    rec = backend->BeginRecording();
    ring.BeginRecording();
    CHECK(ring.Write(block, true).ok());
    Run(*backend, std::move(rec));
}

// ---- the default provider -----------------------------------------------------------------------------------------------------

TEST_CASE("provider/the_default_provider_builds_on_the_first_miss_and_not_on_the_second") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend);
    FragmentKey fk;
    fk.user.raw = kColorIn;
    native::PipelineKey key;
    key.fragment.key = fk;
    key.color_format = rhi::Format::Rgba8Unorm;
    key.vertex.program = 1;

    rhi::Result<native::PipelineHandle> first = rig.provider->Lookup(key);
    REQUIRE_MESSAGE(first.ok(), first.error());
    CHECK(first->pipeline != nullptr);
    CHECK_FALSE(first->fallback);
    CHECK(rig.built.size() == 1);
    CHECK(rig.provider->Counters().sync_builds == 1);
    rhi::Result<native::PipelineHandle> second = rig.provider->Lookup(key);
    REQUIRE(second.ok());
    CHECK(second->pipeline == first->pipeline);
    CHECK(rig.built.size() == 1);  // not built again
    CHECK(rig.provider->Counters().sync_builds == 1);
    CHECK(rig.provider->Counters().hits == 1);
    CHECK(rig.provider->Counters().lookups == 2);

    // another key is another build
    native::PipelineKey other = key;
    other.state.cull = rhi::CullMode::Back;
    REQUIRE(rig.provider->Lookup(other).ok());
    CHECK(rig.built.size() == 2);
    CHECK(rig.provider->size() == 2);

    // the frame counters: pipeline_sync_builds 2, pipeline_forced_uber 0, and reading resets them
    std::vector<native::FrameCounter> counters = rig.provider->TakeFrameCounters();
    REQUIRE(counters.size() == 2);
    CHECK(counters[0].name == "pipeline_sync_builds");
    CHECK(counters[0].value == 2);
    CHECK(counters[1].name == "pipeline_forced_uber");
    CHECK(counters[1].value == 0);
    CHECK(rig.provider->TakeFrameCounters()[0].value == 0);
    CHECK(rig.provider->Counters().sync_builds == 2);  // the totals stay

    // a miss the composition reports itself
    rig.provider->ReportMiss(key, "the generator refused");
    CHECK(rig.provider->Counters().reported_misses == 1);

    // a builder that fails: an error, counted, and nothing kept
    native::DefaultPipelineProvider failing([](const native::PipelineKey&) -> rhi::Result<std::shared_ptr<rhi::Pipeline>> {
        return rhi::Error{"no shader"};
    });
    CHECK_FALSE(failing.Lookup(key).ok());
    CHECK(failing.Counters().build_failures == 1);
    CHECK(failing.size() == 0);
}

TEST_CASE("provider/force_ubershader_returns_the_fallback_for_every_draw") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    Rig rig(backend, /*force_ubershader=*/true);
    auto state = NewState();
    auto target = MakeImage(*backend, 4, 4);

    rhi::Recorder rec = backend->BeginRecording();
    rig.composer->BeginRecording(rec);
    rig.uniforms->BeginRecording();
    rhi::Result<FragmentState::Prepared> prepared = rig.fragment->Prepare(*state, {}, nullptr);
    REQUIRE(prepared.ok());
    rhi::Result<FragmentUniformRing::Allocation> alloc = rig.uniforms->Write(rig.fragment->block(), prepared->block_changed);
    REQUIRE(alloc.ok());
    rig.composer->BeginTargetPass(rec, ClearPass(target.get(), 0, 0, 0, 0));
    // two draws with different fragment keys: both take the one fallback pipeline
    for (u32 selector : {u32(kColorIn), u32(kBlock)}) {
        FragmentKey key = rig.fragment->key();
        key.user.raw = selector;
        native::DrawDesc d = rig.Desc(key, target.get());
        d.fragment_uniforms = *alloc;
        rhi::Result<native::DrawResult> result = rig.composer->Draw(rec, d);
        REQUIRE_MESSAGE(result.ok(), result.error());
        CHECK(result->fallback);
    }
    Run(*backend, std::move(rec));
    CHECK(AllPixels(*backend, *target, 4, 4, {0, 0, 255, 255}));  // the test's ubershader is blue
    // the fallback was built once (synchronously, counted), the specialised pipelines never
    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].IsFallback());
    CHECK(rig.built[0].fragment.uber == native::UberModule::Color);
    CHECK(rig.provider->Counters().forced_uber == 2);
    CHECK(rig.provider->Counters().sync_builds == 1);
    CHECK(rig.composer->counters().fallback_draws == 2);
    std::vector<native::FrameCounter> counters = rig.provider->TakeFrameCounters();
    CHECK(counters[1].name == "pipeline_forced_uber");
    CHECK(counters[1].value == 2);

    // switching it off answers with the key's own pipeline
    rig.provider->SetForceUbershader(false);
    FragmentKey key = rig.fragment->key();
    key.user.raw = kColorIn;
    native::DrawDesc d = rig.Desc(key, target.get());
    rhi::Result<native::PipelineHandle> handle = rig.provider->Lookup(rig.composer->KeyOf(d));
    REQUIRE(handle.ok());
    CHECK_FALSE(handle->fallback);
    CHECK(rig.built.size() == 2);
}

TEST_CASE("compose/the_base_description_and_the_one_layout") {
    auto backend = test::RequireBackend();
    if (!backend) return;
    native::PipelineKey key;
    key.vertex.input.binding_count = 1;
    key.vertex.input.bindings[0] = {32, 0};
    key.vertex.input.attribute_count = 1;
    key.vertex.input.attributes[0] = {0, 0, rhi::Format::Rgba32Float, 0};
    key.topology = rhi::Topology::TriangleStrip;
    key.color_format = rhi::Format::Rgba8Unorm;
    key.depth_format = rhi::Format::D24S8;
    key.state.depth_test = true;
    rhi::GraphicsPipelineDesc desc = native::BaseDescOf(key, *backend);
    CHECK(desc.topology == rhi::Topology::TriangleStrip);
    CHECK(desc.color_format == rhi::Format::Rgba8Unorm);
    // the host depth format is what ChooseFormat says (D32S8 where the device has no D24S8)
    CHECK(desc.depth_format == backend->ChooseFormat(rhi::Format::D24S8, rhi::FormatUse::DepthTarget).format);
    CHECK(desc.state.depth_test);
    REQUIRE(desc.vertex_layout.bindings.size() == 1);
    CHECK(desc.vertex_layout.bindings[0].stride == 32);
    REQUIRE(desc.vertex_layout.attributes.size() == 1);
    REQUIRE(desc.sets.size() == 3);
    CHECK(desc.sets[0].slots.size() == 6);   // three uniform buffers and three texel buffers
    CHECK(desc.sets[1].slots.size() == 9);   // textures, samplers, tex_color
    CHECK(desc.sets[2].slots.size() == 1);   // the shadow storage image
    CHECK(desc.sets[0].slots[2].type == rhi::BindingType::UniformBuffer);
    CHECK(desc.sets[0].slots[5].type == rhi::BindingType::TexelBuffer);
    CHECK(desc.sets[1].slots[3].type == rhi::BindingType::Sampler);
    CHECK(desc.sets[2].slots[0].type == rhi::BindingType::StorageImage);
    // and a shadow-write key has no attachment formats
    key.pass = native::PassKind::ShadowWrite;
    desc = native::BaseDescOf(key, *backend);
    CHECK(desc.color_format == rhi::Format::Undefined);
    CHECK(desc.depth_format == rhi::Format::Undefined);
}
