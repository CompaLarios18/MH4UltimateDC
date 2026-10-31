// Derived from Azahar src/video_core/shader/generator/pica_fs_config.h and pica_fs_config.cpp (the constructors
// of the sub-structures, worked out by hand for each synthetic register set below), shader_uniforms.h
// (FSUniformData: the offset of every member), profile.h and src/video_core/pica/regs_internal.h,
// regs_framebuffer.h, regs_texturing.h, regs_lighting.h and regs_rasterizer.h (the register positions the tests
// write) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: pica_fs_config.h and pica_fs_config.cpp say
// "Licensed under GPLv3 or any later version"; the others say "GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_fs_config.h, pica_fs_config.cpp, profile.h)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (shader_uniforms.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_framebuffer.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017 Citra Emulator Project (regs_lighting.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
//
// The fragment key, the uniforms, the feature interface and the pipeline key (N2-33), with no device.
// Every expected value is worked out by hand from Azahar's constructors (the comments show the arithmetic); none is
// computed with the code under test. The stable hashes at the end are constants written down after a first run: they
// pin the hash functions across builds (the splitmix64 finalizer itself is checked against its published value).
#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <string>

#include "gpu/fragment/feature.h"
#include "gpu/fragment/key.h"
#include "gpu/fragment/profile.h"
#include "gpu/fragment/state.h"
#include "gpu/fragment/tables.h"
#include "gpu/fragment/uniforms.h"
#include "gpu/native/pipeline_key.h"
#include "gpu/native/vertex_input.h"
#include "gpu/pica/state.h"

using namespace mh4n;
using namespace mh4n::gpu::fragment;
namespace re = mh4n::gpu::fragment::regenum;
namespace native = mh4n::gpu::native;
namespace rhi = mh4n::gpu::rhi;
namespace pica = mh4n::gpu::pica;

namespace {

// A PicaState with every register zero and the lighting disabled (reg 0x1C6 bit 0), nothing dirty.
std::unique_ptr<pica::PicaState> NewState() {
    auto state = std::make_unique<pica::PicaState>();
    state->regs.fill(0);
    state->regs[freg::kLightingDisable] = 1;
    state->dirty.Reset();
    return state;
}

FragmentKey KeyOfState(const pica::PicaState& state) {
    FeatureRegistry empty;
    return MakeKey(state, empty);
}

// ---- features for the registry tests ----

class FogFeature final : public FeatureModule {
public:
    const char* Name() const override { return "fog"; }
    void Key(const pica::PicaState& state, const FragmentKey&, ReservedWords words) const override {
        words.gas = state.regs[freg::kGasFirst] + 1;  // the gas word follows the first gas register
    }
    Needs NeedsOf(const FragmentKey&, const DeviceProfile&) const override {
        Needs n;
        n.lut_lf = true;
        n.texture_units = 1;
        return n;
    }
    const char* Stubbed(const FragmentKey& key) const override { return key.texture.fog_mode() == re::FogMode::Gas ? "gas" : nullptr; }
    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        if (dirty.all || state.dirty.Test(freg::kFogColor)) {
            block.uniforms.fog_color = {1.0f, 2.0f, 3.0f};
            dirty.Mark();
        }
    }
};

class SamplingFeature final : public FeatureModule {
public:
    const char* Name() const override { return "sampling"; }
    Needs NeedsOf(const FragmentKey&, const DeviceProfile&) const override {
        Needs n;
        n.texture_units = 3;
        n.lut_rgba = true;
        return n;
    }
};

class NotAFeature final : public FeatureModule {
public:
    const char* Name() const override { return "weather"; }
};

}  // namespace

// ---- the size ---------------------------------------------------------------------------------------------------------

TEST_CASE("key/the_size_and_the_layout_are_asserted") {
    // Azahar's FSConfig: FramebufferConfig 32 + TextureConfig 136 + LightConfig 76 + ProcTexConfig 28 = 272, and the user
    // word and the gas word: 280 bytes, 70 words. The design's figure holds.
    static_assert(sizeof(FramebufferConfig) == 4 + 12 + 12 + 4);
    static_assert(sizeof(TextureConfig) == 4 + 3 * 4 + 6 * 16 + 3 * 8);
    static_assert(sizeof(LightConfig) == 4 + 7 * 8 + 8 * 2);
    static_assert(sizeof(ProcTexConfig) == 4 + 5 * 4 + 2 * 2);
    static_assert(sizeof(FramebufferConfig) + sizeof(TextureConfig) + sizeof(LightConfig) + sizeof(ProcTexConfig) == 272);
    static_assert(sizeof(FragmentKey) == 280);
    static_assert(sizeof(FragmentKey) / sizeof(u32) == 70);
    static_assert(std::has_unique_object_representations_v<FragmentKey>);
    CHECK(kFragmentKeyBytes == 280);
    CHECK(kFragmentKeyWords == 70);
}

TEST_CASE("key/a_struct_hash_changes_when_a_member_is_added") {
    // Compile-time check on copies of a structure: the same members give the same hash; a member added, a member
    // moved and a version bump each give another.
    struct A {
        u32 raw;
        u16 lod_min;
        u16 lod_max;
        static consteval u64 StructHash() {
            constexpr u64 STRUCT_VERSION = 0;
            using T = A;
            return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(lod_min),
                               MH4N_FIELD_HASH(lod_max));
        }
    };
    struct APlus {
        u32 raw;
        u16 lod_min;
        u16 lod_max;
        u32 added;
        static consteval u64 StructHash() {
            constexpr u64 STRUCT_VERSION = 0;
            using T = APlus;
            return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(lod_min),
                               MH4N_FIELD_HASH(lod_max), MH4N_FIELD_HASH(added));
        }
    };
    struct AMoved {
        u16 lod_min;
        u16 lod_max;
        u32 raw;
        static consteval u64 StructHash() {
            constexpr u64 STRUCT_VERSION = 0;
            using T = AMoved;
            return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(lod_min),
                               MH4N_FIELD_HASH(lod_max));
        }
    };
    struct AVersion {
        u32 raw;
        u16 lod_min;
        u16 lod_max;
        static consteval u64 StructHash() {
            constexpr u64 STRUCT_VERSION = 1;
            using T = AVersion;
            return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(lod_min),
                               MH4N_FIELD_HASH(lod_max));
        }
    };
    static_assert(A::StructHash() == A::StructHash());
    static_assert(A::StructHash() != APlus::StructHash());
    static_assert(A::StructHash() != AMoved::StructHash());
    static_assert(A::StructHash() != AVersion::StructHash());
    // and the real ones differ from each other and are the nested combination
    static_assert(FramebufferConfig::StructHash() != TextureConfig::StructHash());
    static_assert(FragmentKey::StructHash() == kFragmentKeyStructHash);
    static_assert(kFragmentKeyStructHash != LightConfig::StructHash());
    CHECK(kFragmentKeyStructHash == FragmentKey::StructHash());
}

TEST_CASE("key/the_hash_mixer_is_splitmix64") {
    // The published first output of splitmix64 with seed 0 is the finalizer applied to the golden-ratio increment.
    CHECK(HashMix(0x9E3779B97F4A7C15ull) == 0xE220A8397B1DCDAFull);
    CHECK(HashMix(0) == 0);
    CHECK(HashCombine(1, 2) != HashCombine(2, 1));
    CHECK(HashCombine(1, 2, 3) == HashCombine(HashCombine(1, 2), 3));
}

// ---- MakeKey --------------------------------------------------------------------------------------------------------

TEST_CASE("key/zero_registers_give_Azahars_defaults") {
    auto state = NewState();
    const FragmentKey key = KeyOfState(*state);
    // framebuffer: the alpha test is off, so the function is Always (1, bits 0-2); the logic_op field is Copy (3,
    // bits 6-9: 3 << 6 = 192); everything else zero. raw = 1 + 192 = 193.
    CHECK(key.framebuffer.raw == 193);
    CHECK(key.framebuffer.alpha_test_func() == re::CompareFunc::Always);
    CHECK(key.framebuffer.logic_op() == re::LogicOp::Copy);
    CHECK(key.framebuffer.requested_logic_op == re::LogicOp::Clear);  // the register holds 0
    CHECK(key.framebuffer.alphablend_enable() == 0);
    // texture, lighting off, procedural texture off, the two reserved words
    CHECK(key.texture.raw == 0);
    CHECK(key.lighting.raw == 0);
    CHECK(key.proctex.raw == 0);
    CHECK(key.user.raw == 0);
    CHECK(key.gas.raw == 0);
    for (const TevStageConfigRaw& stage : key.texture.tev_stages) CHECK(stage.sources_raw == 0);
}

TEST_CASE("key/the_framebuffer_part") {
    auto state = NewState();
    auto& r = state->regs;
    r[freg::kScissorMode] = 3;                          // Include
    r[freg::kDepthmapEnable] = 1;                       // ZBuffering
    r[freg::kAlphaTest] = 1 | (6u << 4) | (0x80u << 8);  // enabled, GreaterThan, ref 0x80 (the ref is a uniform, not in the key)
    r[freg::kFragmentOperation] = 0;
    r[freg::kLogicOp] = 7;  // Invert, blending off: kept as requested
    FragmentKey key = KeyOfState(*state);
    // raw: alpha_test_func 6 | scissor 3 << 3 = 24 | depthmap 1 << 5 = 32 | logic_op Copy 3 << 6 = 192 = 6 + 24 + 32 + 192
    CHECK(key.framebuffer.raw == 254);
    CHECK(key.framebuffer.scissor_test_mode() == re::ScissorMode::Include);
    CHECK(key.framebuffer.depthmap_enable() == re::DepthBuffering::ZBuffering);
    CHECK(key.framebuffer.requested_logic_op == re::LogicOp::Invert);
    CHECK(key.framebuffer.shadow_rendering() == 0);

    r[freg::kFragmentOperation] = 3;  // the shadow mode
    key = KeyOfState(*state);
    CHECK(key.framebuffer.shadow_rendering() == 1);
    CHECK(key.framebuffer.raw == (254u | (1u << 10)));
}

TEST_CASE("key/the_canonical_rule") {
    auto state = NewState();
    auto& r = state->regs;
    r[freg::kFragmentOperation] = 1u << 8;  // blending on
    // Add / Add with SourceAlpha, OneMinusSourceAlpha and One, Zero; a stale logic-op register
    r[freg::kAlphaBlending] = 0 | (0u << 8) | (6u << 16) | (7u << 20) | (1u << 24) | (0u << 28);
    r[freg::kLogicOp] = 9;
    const FragmentKey a = KeyOfState(*state);
    // blending on: the requested logic op is zero, and both Add halves are zero (equation Add is 0, factors zeroed)
    CHECK(a.framebuffer.requested_logic_op == re::LogicOp::Clear);
    CHECK(a.framebuffer.requested_rgb_blend.eq == re::BlendEquation::Add);
    CHECK(a.framebuffer.requested_rgb_blend.src_factor == re::BlendFactor::Zero);
    CHECK(a.framebuffer.requested_rgb_blend.dst_factor == re::BlendFactor::Zero);
    CHECK(a.framebuffer.requested_alpha_blend.src_factor == re::BlendFactor::Zero);
    CHECK(a.framebuffer.alphablend_enable() == 1);
    // raw: Always 1 | alphablend 1 << 11 = 2048 | Copy 192 = 2241
    CHECK(a.framebuffer.raw == 2241);

    // another draw with other Add factors and another logic op: the same key
    r[freg::kAlphaBlending] = 0 | (0u << 8) | (2u << 16) | (3u << 20) | (6u << 24) | (7u << 28);
    r[freg::kLogicOp] = 12;
    const FragmentKey b = KeyOfState(*state);
    CHECK(a == b);
    CHECK(HashKey(a) == HashKey(b));

    // Subtract and ReverseSubtract halves are zeroed too: only Min and Max matter
    r[freg::kAlphaBlending] = 1 | (2u << 8) | (6u << 16) | (7u << 20) | (6u << 24) | (7u << 28);
    CHECK(KeyOfState(*state) == a);

    // a Min half keeps its equation and factors: rgb Min (3) with DestColor (4) and SourceColor (2)
    r[freg::kAlphaBlending] = 3 | (0u << 8) | (4u << 16) | (2u << 20) | (6u << 24) | (7u << 28);
    const FragmentKey min_a = KeyOfState(*state);
    CHECK(min_a.framebuffer.requested_rgb_blend.eq == re::BlendEquation::Min);
    CHECK(min_a.framebuffer.requested_rgb_blend.src_factor == re::BlendFactor::DestColor);
    CHECK(min_a.framebuffer.requested_rgb_blend.dst_factor == re::BlendFactor::SourceColor);
    CHECK(min_a.framebuffer.requested_alpha_blend.src_factor == re::BlendFactor::Zero);  // the Add half is still zero
    CHECK_FALSE(min_a == a);
    // and other factors on the Min half make another key
    r[freg::kAlphaBlending] = 3 | (0u << 8) | (1u << 16) | (1u << 20) | (6u << 24) | (7u << 28);
    const FragmentKey min_b = KeyOfState(*state);
    CHECK_FALSE(min_a == min_b);
    // a Max alpha half
    r[freg::kAlphaBlending] = 0 | (4u << 8) | (6u << 16) | (7u << 20) | (5u << 24) | (3u << 28);
    const FragmentKey max_a = KeyOfState(*state);
    CHECK(max_a.framebuffer.requested_alpha_blend.eq == re::BlendEquation::Max);
    CHECK(max_a.framebuffer.requested_alpha_blend.src_factor == re::BlendFactor::OneMinusDestColor);
    CHECK(max_a.framebuffer.requested_alpha_blend.dst_factor == re::BlendFactor::OneMinusSourceColor);

    // with blending off the logic op is kept (this is the only case where it matters), factors are not read
    r[freg::kFragmentOperation] = 0;
    r[freg::kAlphaBlending] = 0x77777777u;
    r[freg::kLogicOp] = 12;
    const FragmentKey logic = KeyOfState(*state);
    CHECK(logic.framebuffer.requested_logic_op == re::LogicOp::Equiv);
    CHECK(logic.framebuffer.requested_rgb_blend.src_factor == re::BlendFactor::Zero);
}

TEST_CASE("key/the_texture_part") {
    auto state = NewState();
    auto& r = state->regs;
    r[freg::kMainConfig] = 1u << 13;                                          // texture2_use_coord1
    r[freg::kTexture0 + freg::kTextureParams] = (2u << 28) | (2u << 12) | (1u << 8);  // Shadow2D, wrap_s Repeat, wrap_t ClampToBorder
    r[freg::kTexture1 + freg::kTextureParams] = (3u << 12) | (5u << 8);      // MirroredRepeat, ClampToBorder2 (bit 28+ ignored: unit 1)
    r[freg::kTexture2 + freg::kTextureParams] = (5u << 12) | (4u << 28);      // wrap_s ClampToBorder2 (needs all 3 bits); the type field is only read for unit 0
    r[freg::kShadowTexture] = 1;                                              // orthographic
    r[freg::kFogModeAndCombiner] = 5 | (0x5u << 8) | (0x3u << 12) | (1u << 16);  // Fog, rgb mask 0101, a mask 0011, flip
    const FragmentKey key = KeyOfState(*state);
    CHECK(key.texture.texture0_type() == re::TextureType::Shadow2D);
    CHECK(key.texture.texture2_use_coord1() == 1);
    CHECK(key.texture.shadow_texture_orthographic() == 1);
    CHECK(key.texture.fog_mode() == re::FogMode::Fog);
    CHECK(key.texture.fog_flip() == 1);
    // combiner_buffer_input = rgb mask | alpha mask << 4 = 0x5 | 0x30 = 0x35
    CHECK(key.texture.combiner_buffer_input() == 0x35);
    CHECK(key.TevStageUpdatesCombinerBufferColor(0));
    CHECK_FALSE(key.TevStageUpdatesCombinerBufferColor(1));
    CHECK(key.TevStageUpdatesCombinerBufferColor(2));
    CHECK(key.TevStageUpdatesCombinerBufferAlpha(0));
    CHECK(key.TevStageUpdatesCombinerBufferAlpha(1));
    CHECK_FALSE(key.TevStageUpdatesCombinerBufferAlpha(2));
    CHECK_FALSE(key.TevStageUpdatesCombinerBufferColor(4));  // only stages 0-3 write the buffer
    CHECK(key.texture.requested_wrap[0].s == re::WrapMode::Repeat);
    CHECK(key.texture.requested_wrap[0].t == re::WrapMode::ClampToBorder);
    CHECK(key.texture.requested_wrap[1].s == re::WrapMode::MirroredRepeat);
    CHECK(key.texture.requested_wrap[1].t == re::WrapMode::ClampToBorder2);
    CHECK(key.texture.requested_wrap[2].s == re::WrapMode::ClampToBorder2);
    CHECK(key.texture.requested_wrap[2].t == re::WrapMode::ClampToEdge);
    // raw: type 2 | use_coord1 1 << 3 = 8 | combiner 0x35 << 4 = 848 | fog_mode 5 << 12 = 20480 | flip 1 << 15 = 32768 | ortho 1 << 16 = 65536
    CHECK(key.texture.raw == 2u + 8u + 848u + 20480u + 32768u + 65536u);
    // the border enable bits are the profile's, not the key's
    CHECK(key.texture.texture_border_color[0].raw == 0);
}

TEST_CASE("key/the_tev_stages_and_the_Dot3_RGBA_masking") {
    auto state = NewState();
    auto& r = state->regs;
    const u32 bases[6] = {0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8};
    for (u32 i = 0; i < 6; ++i) {
        r[bases[i] + 0] = 0x00112233u + i;   // sources
        r[bases[i] + 1] = 0x00445566u + i;   // modifiers
        r[bases[i] + 2] = 0x00010002u;       // ops: alpha Modulate (1), colour Add (2)
        r[bases[i] + 3] = 0xAABBCCDDu;       // const colour (a uniform, not in the key)
        r[bases[i] + 4] = 0x00010002u;       // scales
    }
    // stage 2 has the colour operation Dot3_RGBA (7): its sources, modifiers and ops are masked to the low bits
    r[bases[2] + 0] = 0xFFFFFFFFu;
    r[bases[2] + 1] = 0xFFFFFFFFu;
    r[bases[2] + 2] = 0x000500F7u;  // colour op 7 (Dot3_RGBA); bits 4-7 and the alpha op (16-19) must be cleared
    const FragmentKey key = KeyOfState(*state);
    for (u32 i = 0; i < 6; ++i) {
        if (i == 2) continue;
        CHECK(key.texture.tev_stages[i].sources_raw == 0x00112233u + i);
        CHECK(key.texture.tev_stages[i].modifiers_raw == 0x00445566u + i);
        CHECK(key.texture.tev_stages[i].ops_raw == 0x00010002u);
        CHECK(key.texture.tev_stages[i].scales_raw == 0x00010002u);
        CHECK(key.texture.tev_stages[i].color_op() == re::TevOperation::Add);
        CHECK(key.texture.tev_stages[i].alpha_op() == re::TevOperation::Modulate);
    }
    CHECK(key.texture.tev_stages[2].sources_raw == 0xFFFu);
    CHECK(key.texture.tev_stages[2].modifiers_raw == 0xFFFu);
    CHECK(key.texture.tev_stages[2].ops_raw == 0x7u);  // 0x000500F7 & 0xF
    CHECK(key.texture.tev_stages[2].scales_raw == 0x00010002u);  // not masked
}

TEST_CASE("key/the_lighting_part") {
    auto state = NewState();
    auto& r = state->regs;
    r[freg::kLightingDisable] = 0;
    r[freg::kLightingMaxLightIndex] = 1;  // two lights
    // slots: light 3 then light 5
    r[freg::kLightingLightEnable] = 3 | (5u << 4);
    r[freg::kLight0 + freg::kLightStride * 3 + freg::kLightConfig] = 1 | 8;   // directional, geometric_factor_1
    r[freg::kLight0 + freg::kLightStride * 5 + freg::kLightConfig] = 2;       // two_sided_diffuse
    // config1: disable_lut_d1 (bit 17), the dummy bit 18 (real games set it, Azahar ignores it: lut_sp stays enabled), rr (20), rg (21), rb (22); disable_dist_atten of light 3 (24 + 3);
    // disable_spot_atten of light 5 (8 + 5)
    r[freg::kLightingConfig1] = (1u << 17) | (1u << 18) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 27) | (1u << 13);
    // config0: enable_shadow (0), enable_primary_alpha (2), config 1 (4-7), shadow_primary (16), bump_selector 1 (22-23),
    // shadow_selector 2 (24-25), clamp_highlights (27), bump_mode NormalMap (28-29), disable_bump_renorm (30)
    r[freg::kLightingConfig0] = 1u | (1u << 2) | (1u << 4) | (1u << 16) | (1u << 22) | (2u << 24) | (1u << 27) | (1u << 28) | (1u << 30);
    r[freg::kLightingAbsLutInput] = 1u << 1;  // disable_d0: d0 uses the signed input
    r[freg::kLightingLutInput] = 2u | (4u << 8) | (1u << 12);         // d0 NV, sp SP, fr VH
    r[freg::kLightingLutScale] = 1u | (7u << 8) | (6u << 12);          // d0 Scale2, sp Scale1_2, fr Scale1_4
    const FragmentKey key = KeyOfState(*state);
    const LightConfig& l = key.lighting;
    CHECK(l.enable() == 1);
    CHECK(l.src_num() == 2);
    CHECK(l.config() == re::LightingConfig::Config1);
    CHECK(l.enable_primary_alpha() == 1);
    CHECK(l.enable_secondary_alpha() == 0);
    CHECK(l.bump_mode() == re::LightingBumpMode::NormalMap);
    CHECK(l.bump_selector() == 1);
    CHECK(l.bump_renorm() == 0);  // disable_bump_renorm is set
    CHECK(l.clamp_highlights() == 1);
    CHECK(l.enable_shadow() == 1);
    CHECK(l.shadow_primary() == 1);
    CHECK(l.shadow_secondary() == 0);
    CHECK(l.shadow_selector() == 2);
    // raw: 1 + src_num 2 << 1 = 4 + bump_mode 1 << 5 = 32 + bump_selector 1 << 7 = 128 + clamp 1 << 10 = 1024 + config 1 << 11 = 2048
    //      + primary_alpha 1 << 15 = 32768 + enable_shadow 1 << 17 = 131072 + shadow_primary 1 << 18 = 262144 + selector 2 << 22 = 8388608
    CHECK(l.raw == 8817829);
    // light 0 is light 3: num 3 | directional 1 << 3 = 8 | dist_atten disabled (bit 27) so 0 | spot_atten on 1 << 6 = 64 |
    // geometric_factor_1 1 << 8 = 256 | shadow on 1 << 9 = 512
    CHECK(l.lights[0].raw == 3 + 8 + 64 + 256 + 512);
    CHECK(l.lights[0].num() == 3);
    CHECK(l.lights[0].dist_atten_enable() == 0);
    // light 1 is light 5: num 5 | two_sided 1 << 4 = 16 | dist_atten on 1 << 5 = 32 | spot disabled (bit 13) so 0 | shadow on 512
    CHECK(l.lights[1].raw == 5 + 16 + 32 + 512);
    CHECK(l.lights[2].raw == 0);
    // LUTs: d0 enabled, signed input (abs_input 0), NV (2), scale 2
    CHECK(l.lut_d0.raw == (1u | (0u << 1) | (2u << 2)));
    CHECK(l.lut_d0.GetScale() == 2.0f);
    CHECK(l.lut_d0.scale_bits == 0x40000000u);
    CHECK(l.lut_d1.raw == 0);  // disabled: nothing else is read
    CHECK(l.lut_d1.scale_bits == 0);
    // sp is always enabled ("a dummy field"): abs_input 1, type SP (4), scale 0.5
    CHECK(l.lut_sp.raw == (1u | (1u << 1) | (4u << 2)));
    CHECK(l.lut_sp.GetScale() == 0.5f);
    CHECK(l.lut_fr.raw == (1u | (1u << 1) | (1u << 2)));
    CHECK(l.lut_fr.scale_bits == 0x3E800000u);  // 0.25
    CHECK(l.lut_rr.raw == 0);
    CHECK(l.lut_rg.raw == 0);
    CHECK(l.lut_rb.raw == 0);

    // the disable bit empties it all
    r[freg::kLightingDisable] = 1;
    CHECK(KeyOfState(*state).lighting.raw == 0);
    CHECK(KeyOfState(*state).lighting.lights[0].raw == 0);
}

TEST_CASE("key/the_procedural_texture_part") {
    auto state = NewState();
    auto& r = state->regs;
    r[freg::kProcTex] = 4u | (1u << 3) | (8u << 6) | (9u << 10) | (1u << 14) | (1u << 15) | (1u << 16) | (2u << 18);
    r[freg::kProcTexLut] = 5u | (2u << 3) | (9u << 7) | (200u << 11);  // LinearMipmapLinear, lod 2..9, width 200
    r[freg::kProcTexLutOffset] = 10u | (20u << 8) | (30u << 16) | (40u << 24);
    // disabled (texture3_enable clear): everything stays zero whatever the other registers hold
    CHECK(KeyOfState(*state).proctex.raw == 0);
    CHECK(KeyOfState(*state).proctex.lut_width == 0);

    r[freg::kMainConfig] = (1u << 10) | (2u << 8);  // texture3_enable, coordinates 2
    const ProcTexConfig& p = KeyOfState(*state).proctex;
    CHECK(p.enable() == 1);
    CHECK(p.coord() == 2);
    CHECK(p.u_clamp() == re::ProcTexClamp::Pulse);
    CHECK(p.v_clamp() == re::ProcTexClamp::ToEdge);
    CHECK(p.color_combiner() == re::ProcTexCombiner::Max);
    CHECK(p.alpha_combiner() == re::ProcTexCombiner::RMax);
    CHECK(p.separate_alpha() == 1);
    CHECK(p.noise_enable() == 1);
    CHECK(p.u_shift() == re::ProcTexShift::Odd);
    CHECK(p.v_shift() == re::ProcTexShift::Even);
    CHECK(p.lut_filter() == re::ProcTexFilter::LinearMipmapLinear);
    CHECK(p.lut_width == 200);
    CHECK(p.lut_offset0 == 10);
    CHECK(p.lut_offset1 == 20);
    CHECK(p.lut_offset2 == 30);
    CHECK(p.lut_offset3 == 40);
    CHECK(p.lod_min == 2);
    CHECK(p.lod_max == 9);
    // raw: enable 1 + coord 2 << 1 = 4 + u_clamp 4 << 3 = 32 + v_clamp 1 << 6 = 64 + color 8 << 9 = 4096 + alpha 9 << 13 = 73728
    //      + filter 5 << 17 = 655360 + separate_alpha 1 << 20 + noise 1 << 21 + u_shift 1 << 22 + v_shift 2 << 24
    CHECK(p.raw == 1u + 4u + 32u + 64u + 4096u + 73728u + 655360u + (1u << 20) + (1u << 21) + (1u << 22) + (2u << 24));
}

TEST_CASE("key/the_registers_that_are_not_in_the_key_do_not_change_it") {
    auto state = NewState();
    const FragmentKey base = KeyOfState(*state);
    auto& r = state->regs;
    // uniforms: constant colours, the alpha reference, the blend colour, the depth range, lod bias, border colours, fog
    // colour, the lights' colours and positions
    r[0xC3] = 0x11223344;
    r[freg::kBlendConst] = 0x55667788;
    r[freg::kAlphaTest] = 0x0000FF00;  // ref only (disabled)
    r[freg::kViewportDepthRange] = 0x3F0000;
    r[freg::kTexture0 + freg::kTextureLod] = 0x1234;
    r[freg::kTexture0 + freg::kTextureBorderColor] = 0xFFFFFFFF;
    r[freg::kFogColor] = 0x123456;
    r[freg::kLight0 + freg::kLightDiffuse] = 0xDEADBEEF;
    CHECK(KeyOfState(*state) == base);
}

// ---- the profile ------------------------------------------------------------------------------------------------------

TEST_CASE("profile/ApplyProfile_sets_the_emulation_bits_as_Azahars") {
    auto state = NewState();
    auto& r = state->regs;
    // no blending, an Invert logic op, two clamp-to-border waps
    r[freg::kFragmentOperation] = 0;
    r[freg::kLogicOp] = 7;
    r[freg::kTexture0 + freg::kTextureParams] = (1u << 12) | (2u << 8);  // wrap_s ClampToBorder, wrap_t Repeat
    r[freg::kTexture2 + freg::kTextureParams] = (2u << 12) | (1u << 8);  // wrap_t ClampToBorder
    const FragmentKey requested = KeyOfState(*state);

    // the PC: hardware logic op and custom border colours
    FragmentKey pc = requested;
    DeviceProfile pc_profile;
    pc_profile.logic_op = true;
    pc_profile.custom_border_color = true;
    pc.ApplyProfile(pc_profile);
    CHECK(pc.framebuffer.logic_op() == re::LogicOp::Copy);  // the hardware does it: the shader writes the colour as it is
    CHECK(pc.texture.texture_border_color[0].raw == 0);
    CHECK(pc == requested);

    // MoltenVK: no logic op, no custom border colour
    FragmentKey mac = requested;
    DeviceProfile mac_profile;
    mac.ApplyProfile(mac_profile);
    CHECK(mac.framebuffer.logic_op() == re::LogicOp::Invert);  // emulated in the shader
    CHECK(mac.texture.texture_border_color[0].enable_s() == 1);
    CHECK(mac.texture.texture_border_color[0].enable_t() == 0);
    CHECK(mac.texture.texture_border_color[1].raw == 0);
    CHECK(mac.texture.texture_border_color[2].enable_s() == 0);
    CHECK(mac.texture.texture_border_color[2].enable_t() == 1);
    // the stored key is untouched
    CHECK(requested.framebuffer.logic_op() == re::LogicOp::Copy);
    CHECK(requested.texture.texture_border_color[0].raw == 0);

    // KosmicKrisp: hardware logic op, no custom border colour
    FragmentKey kk = requested;
    DeviceProfile kk_profile;
    kk_profile.logic_op = true;
    kk.ApplyProfile(kk_profile);
    CHECK(kk.framebuffer.logic_op() == re::LogicOp::Copy);
    CHECK(kk.texture.texture_border_color[0].enable_s() == 1);

    // with blending on, the logic op is never emulated, even with no hardware op
    r[freg::kFragmentOperation] = 1u << 8;
    FragmentKey blended = KeyOfState(*state);
    blended.ApplyProfile(mac_profile);
    CHECK(blended.framebuffer.logic_op() == re::LogicOp::Copy);
}

TEST_CASE("profile/the_device_profile_follows_the_capabilities_after_the_mask") {
    rhi::Capabilities caps;
    caps.logic_op = true;
    caps.custom_border_color = false;
    caps.interlock = true;
    caps.geometry_shader = false;
    DeviceProfile p = ProfileOf(caps);
    CHECK(p.logic_op);
    CHECK_FALSE(p.custom_border_color);
    CHECK(p.interlock);
    CHECK_FALSE(p.geometry_shader);
    CHECK_FALSE(p.has_blend_minmax_factor);  // false on every device
    caps.logic_op = false;
    caps.custom_border_color = true;
    caps.interlock = false;
    caps.geometry_shader = true;
    p = ProfileOf(caps);
    CHECK_FALSE(p.logic_op);
    CHECK(p.custom_border_color);
    CHECK_FALSE(p.interlock);
    CHECK(p.geometry_shader);
    CHECK_FALSE(p.has_blend_minmax_factor);
}

TEST_CASE("profile/min_max_halves") {
    // A hand-built key in Azahar's (non-canonical) form: rgb Min with One/One, alpha Max with SrcAlpha/DstAlpha, and an
    // Add half with One/One.
    FragmentKey key;
    key.framebuffer.set_alphablend_enable(1);
    key.framebuffer.requested_rgb_blend = {re::BlendEquation::Min, re::BlendFactor::One, re::BlendFactor::One};
    key.framebuffer.requested_alpha_blend = {re::BlendEquation::Max, re::BlendFactor::SourceAlpha, re::BlendFactor::DestAlpha};
    DeviceProfile vulkan;  // has_blend_minmax_factor false
    FragmentKey applied = key;
    applied.ApplyProfile(vulkan);
    // Vulkan's MIN/MAX ignore the factors: with both factors One the hardware op is right (emulation disabled, the
    // marker -1); the other half needs the emulation
    CHECK(applied.framebuffer.requested_rgb_blend.eq == BlendConfig::kMinMaxEmulationDisabled);
    CHECK_FALSE(applied.framebuffer.requested_rgb_blend.RequiresMinMaxEmulation());
    CHECK(applied.framebuffer.requested_alpha_blend.eq == re::BlendEquation::Max);
    CHECK(applied.framebuffer.requested_alpha_blend.RequiresMinMaxEmulation());
    CHECK(applied.framebuffer.requested_alpha_blend.src_factor == re::BlendFactor::SourceAlpha);

    // a device whose min/max honours the factors disables both
    DeviceProfile honours;
    honours.has_blend_minmax_factor = true;
    FragmentKey both = key;
    both.ApplyProfile(honours);
    CHECK(both.framebuffer.requested_rgb_blend.eq == BlendConfig::kMinMaxEmulationDisabled);
    CHECK(both.framebuffer.requested_alpha_blend.eq == BlendConfig::kMinMaxEmulationDisabled);

    // blending off: the halves are not touched
    FragmentKey off = key;
    off.framebuffer.set_alphablend_enable(0);
    off.ApplyProfile(vulkan);
    CHECK(off.framebuffer.requested_rgb_blend.eq == re::BlendEquation::Min);
}

// ---- Needs ------------------------------------------------------------------------------------------------------------

TEST_CASE("needs/core_cases") {
    FeatureRegistry none;
    DeviceProfile hardware;  // the PC
    hardware.logic_op = true;
    DeviceProfile software;  // MoltenVK: no hardware logic op
    FragmentKey key;
    key.framebuffer.set_logic_op(re::LogicOp::Copy);

    CHECK(NeedsOf(key, software, none) == Needs{});

    // the shadow mode: the write pass
    FragmentKey shadow = key;
    shadow.framebuffer.set_shadow_rendering(1);
    CHECK(NeedsOf(shadow, hardware, none).shadow_write_pass);
    CHECK_FALSE(NeedsOf(key, hardware, none).shadow_write_pass);

    // lighting on: the fix-up
    FragmentKey lit = key;
    lit.lighting.set_enable(1);
    CHECK(NeedsOf(lit, hardware, none).fixup_gs);
    CHECK_FALSE(NeedsOf(key, hardware, none).fixup_gs);

    // a logic op, blending off, no hardware op: eleven of the 16 read the destination
    int reading = 0;
    for (u32 op = 0; op < 16; ++op) {
        FragmentKey k = key;
        k.framebuffer.requested_logic_op = static_cast<re::LogicOp>(op);
        const bool copy = NeedsOf(k, software, none).dst_copy;
        reading += copy ? 1 : 0;
        CHECK(copy == LogicOpReadsDestination(static_cast<re::LogicOp>(op)));
        CHECK_FALSE(NeedsOf(k, hardware, none).dst_copy);  // the hardware does it
    }
    CHECK(reading == 11);
    // the five that do not: Clear, Copy, Set, CopyInverted, NoOp
    for (re::LogicOp op : {re::LogicOp::Clear, re::LogicOp::Copy, re::LogicOp::Set, re::LogicOp::CopyInverted, re::LogicOp::NoOp}) {
        FragmentKey k = key;
        k.framebuffer.requested_logic_op = op;
        CHECK_FALSE(NeedsOf(k, software, none).dst_copy);
    }
    // blending on: the logic op register is ignored
    FragmentKey blend = key;
    blend.framebuffer.set_alphablend_enable(1);
    blend.framebuffer.requested_logic_op = re::LogicOp::Xor;
    CHECK_FALSE(NeedsOf(blend, software, none).dst_copy);
    // the shadow mode draws no colour
    FragmentKey shadow_logic = key;
    shadow_logic.framebuffer.set_shadow_rendering(1);
    shadow_logic.framebuffer.requested_logic_op = re::LogicOp::Xor;
    CHECK_FALSE(NeedsOf(shadow_logic, software, none).dst_copy);

    // min/max: a Min half with factors other than One/One needs the copy; with One/One the hardware op does
    FragmentKey minmax = key;
    minmax.framebuffer.set_alphablend_enable(1);
    minmax.framebuffer.requested_rgb_blend = {re::BlendEquation::Min, re::BlendFactor::SourceAlpha, re::BlendFactor::One};
    CHECK(NeedsOf(minmax, hardware, none).dst_copy);
    minmax.framebuffer.requested_rgb_blend = {re::BlendEquation::Min, re::BlendFactor::One, re::BlendFactor::One};
    CHECK_FALSE(NeedsOf(minmax, hardware, none).dst_copy);
    minmax.framebuffer.requested_alpha_blend = {re::BlendEquation::Max, re::BlendFactor::DestAlpha, re::BlendFactor::Zero};
    CHECK(NeedsOf(minmax, hardware, none).dst_copy);
    // a device whose min/max honours the factors never needs it
    DeviceProfile honours = hardware;
    honours.has_blend_minmax_factor = true;
    CHECK_FALSE(NeedsOf(minmax, honours, none).dst_copy);
}

TEST_CASE("needs/the_features_are_ored_in") {
    FogFeature fog;
    SamplingFeature sampling;
    FeatureRegistry registry;
    CHECK(registry.Add(fog));
    CHECK(registry.Add(sampling));
    FragmentKey lit;
    lit.lighting.set_enable(1);
    DeviceProfile profile;
    const Needs needs = NeedsOf(lit, profile, registry);
    CHECK(needs.fixup_gs);   // core's
    CHECK(needs.lut_lf);     // fog's
    CHECK(needs.lut_rgba);   // sampling's
    CHECK_FALSE(needs.lut_rg);
    CHECK(needs.texture_units == 3);  // the larger of 1 and 3
}

// ---- the registry -----------------------------------------------------------------------------------------------------

TEST_CASE("feature/the_registry_holds_the_five_names_in_a_fixed_order") {
    FogFeature fog;
    SamplingFeature sampling;
    NotAFeature weather;
    FeatureRegistry registry;
    CHECK_FALSE(registry.Add(weather));  // not one of the five
    CHECK(registry.Add(fog));
    CHECK(registry.Add(sampling));
    CHECK_FALSE(registry.Add(fog));  // taken
    REQUIRE(registry.All().size() == 2);
    CHECK(std::string(registry.All()[0]->Name()) == "sampling");  // the order of kFeatureNames, not of registration
    CHECK(std::string(registry.All()[1]->Name()) == "fog");
    CHECK(registry.Find("fog") == &fog);
    CHECK(registry.Find("lighting") == nullptr);
    CHECK(kFeatureNames.size() == 5);
    registry.Clear();
    CHECK(registry.All().empty());
}

TEST_CASE("feature/the_key_hook_fills_the_reserved_words_only") {
    auto state = NewState();
    state->regs[freg::kGasFirst] = 41;
    FogFeature fog;
    FeatureRegistry registry;
    registry.Add(fog);
    const FragmentKey with = MakeKey(*state, registry);
    CHECK(with.gas.raw == 42);
    CHECK(with.user.raw == 0);
    FeatureRegistry empty;
    FragmentKey without = MakeKey(*state, empty);
    CHECK(without.gas.raw == 0);
    // everything but the gas word is the same
    without.gas.raw = 42;
    CHECK(without == with);
    // the stub report
    const char* names[5];
    CHECK(StubbedFeatures(with, registry, names) == 0);
    FragmentKey gas = with;
    gas.texture.set_fog_mode(re::FogMode::Gas);
    REQUIRE(StubbedFeatures(gas, registry, names) == 1);
    CHECK(std::string(names[0]) == "gas");
}

// ---- the uniforms -----------------------------------------------------------------------------------------------------

TEST_CASE("uniforms/the_offsets_equal_Azahars") {
    // FSUniformData worked out from its declaration: 0x530 = 1,328 bytes. (Vec3f members are 12 bytes at 16-byte alignment.)
    CHECK(sizeof(FragmentUniforms) == 0x530);
    CHECK(sizeof(LightSrc) == 112);
    struct Row {
        const char* name;
        size_t offset;
        size_t want;
    };
    const Row rows[] = {
        {"framebuffer_scale", offsetof(FragmentUniforms, framebuffer_scale), 0},
        {"alphatest_ref", offsetof(FragmentUniforms, alphatest_ref), 4},
        {"depth_scale", offsetof(FragmentUniforms, depth_scale), 8},
        {"depth_offset", offsetof(FragmentUniforms, depth_offset), 12},
        {"shadow_bias_constant", offsetof(FragmentUniforms, shadow_bias_constant), 16},
        {"shadow_bias_linear", offsetof(FragmentUniforms, shadow_bias_linear), 20},
        {"scissor_x1", offsetof(FragmentUniforms, scissor_x1), 24},
        {"scissor_y1", offsetof(FragmentUniforms, scissor_y1), 28},
        {"scissor_x2", offsetof(FragmentUniforms, scissor_x2), 32},
        {"scissor_y2", offsetof(FragmentUniforms, scissor_y2), 36},
        {"fog_lut_offset", offsetof(FragmentUniforms, fog_lut_offset), 40},
        {"proctex_noise_lut_offset", offsetof(FragmentUniforms, proctex_noise_lut_offset), 44},
        {"proctex_color_map_offset", offsetof(FragmentUniforms, proctex_color_map_offset), 48},
        {"proctex_alpha_map_offset", offsetof(FragmentUniforms, proctex_alpha_map_offset), 52},
        {"proctex_lut_offset", offsetof(FragmentUniforms, proctex_lut_offset), 56},
        {"proctex_diff_lut_offset", offsetof(FragmentUniforms, proctex_diff_lut_offset), 60},
        {"proctex_bias", offsetof(FragmentUniforms, proctex_bias), 64},
        {"shadow_texture_bias", offsetof(FragmentUniforms, shadow_texture_bias), 68},
        {"lighting_lut_offset", offsetof(FragmentUniforms, lighting_lut_offset), 80},     // 18 + 2 words of padding to 16
        {"fog_color", offsetof(FragmentUniforms, fog_color), 176},                        // 80 + 6 * 16
        {"proctex_noise_f", offsetof(FragmentUniforms, proctex_noise_f), 192},            // 188 up to 8
        {"proctex_noise_a", offsetof(FragmentUniforms, proctex_noise_a), 200},
        {"proctex_noise_p", offsetof(FragmentUniforms, proctex_noise_p), 208},
        {"lighting_global_ambient", offsetof(FragmentUniforms, lighting_global_ambient), 224},  // 216 up to 16
        {"light_src", offsetof(FragmentUniforms, light_src), 240},                        // 236 up to 16
        {"const_color", offsetof(FragmentUniforms, const_color), 1136},                   // 240 + 8 * 112
        {"tev_combiner_buffer_color", offsetof(FragmentUniforms, tev_combiner_buffer_color), 1232},
        {"tex_lod_bias", offsetof(FragmentUniforms, tex_lod_bias), 1248},
        {"tex_border_color", offsetof(FragmentUniforms, tex_border_color), 1264},         // 1260 up to 16
        {"blend_color", offsetof(FragmentUniforms, blend_color), 1312},
    };
    for (const Row& row : rows) {
        INFO(row.name);
        CHECK(row.offset == row.want);
    }
    // inside LightSrc: six vectors of 12 bytes at 16-byte alignment, then two floats right after the last, padded to 112
    CHECK(offsetof(LightSrc, specular_1) == 16);
    CHECK(offsetof(LightSrc, diffuse) == 32);
    CHECK(offsetof(LightSrc, ambient) == 48);
    CHECK(offsetof(LightSrc, position) == 64);
    CHECK(offsetof(LightSrc, spot_direction) == 80);
    CHECK(offsetof(LightSrc, dist_atten_bias) == 92);   // spot_direction is 12 bytes at 80: the float follows it
    CHECK(offsetof(LightSrc, dist_atten_scale) == 96);
}

TEST_CASE("uniforms/the_block_and_the_allocation") {
    // uniforms 1328, the key (280 bytes) at 1328 padded to 1616, the gas region (16 + 16 words) at 1616: 1744 bytes,
    // allocated in a multiple of 256: 7 * 256 = 1792
    CHECK(offsetof(FragmentBlock, cfg) == 1328);
    CHECK(offsetof(FragmentBlock, gas) == 1616);
    CHECK(sizeof(GasUniforms) == 128);
    CHECK(sizeof(FragmentBlock) == 1744);
    CHECK(kFragmentBlockBytes == 1744);
    CHECK(kFragmentBlockAllocation == 1792);
    CHECK(kFragmentBlockAllocation % 256 == 0);
}

TEST_CASE("uniforms/core_members_are_written_only_when_their_registers_are_dirty") {
    auto state = NewState();
    auto& r = state->regs;
    FragmentBlock block;
    std::memset(&block, 0, sizeof(block));
    FeatureRegistry none;

    r[0xC3] = 0x80FF4000u;  // stage 0 constant: r 0x00, g 0x40, b 0xFF, a 0x80
    r[0xFB] = 0xFF000000u;  // stage 5: alpha only
    r[freg::kTevCombinerBufferColor] = 0x000000FFu;
    r[freg::kBlendConst] = 0xFF800000u;
    r[freg::kAlphaTest] = 1u | (6u << 4) | (0x20u << 8);
    r[freg::kViewportDepthRange] = 0x400000;      // float24: sign 0, exponent 0x40 (= 64, bias 63), mantissa 0 = 2.0
    r[freg::kViewportDepthNearPlane] = 0x3F0000;  // exponent 63 = 1.0
    r[freg::kScissorXY1] = 2u | (3u << 16);
    r[freg::kScissorXY2] = 9u | (11u << 16);

    // nothing dirty: nothing but the scissor (which depends on the context, and is compared) is written
    UniformDirty dirty;
    FillUniforms(*state, block, dirty, {}, none);
    CHECK(dirty.changed);  // the scissor rectangle changed from zero
    CHECK(block.uniforms.const_color[0].w == 0.0f);
    CHECK(block.uniforms.alphatest_ref == 0);
    CHECK(block.uniforms.depth_scale == 0.0f);
    // the scissor at scale 1: x1 2, y1 3, x2 + 1 = 10, y2 + 1 = 12
    CHECK(block.uniforms.scissor_x1 == 2);
    CHECK(block.uniforms.scissor_y1 == 3);
    CHECK(block.uniforms.scissor_x2 == 10);
    CHECK(block.uniforms.scissor_y2 == 12);
    CHECK(block.uniforms.framebuffer_scale == 1);
    // and a second call with nothing dirty and the same context writes nothing
    dirty = {};
    FillUniforms(*state, block, dirty, {}, none);
    CHECK_FALSE(dirty.changed);

    // dirty bits: each group on its own
    dirty = {};
    state->dirty.Set(0xC3);
    FillUniforms(*state, block, dirty, {}, none);
    CHECK(dirty.changed);
    CHECK(block.uniforms.const_color[0].x == 0.0f);
    CHECK(block.uniforms.const_color[0].y == 64.0f / 255.0f);
    CHECK(block.uniforms.const_color[0].z == 1.0f);
    CHECK(block.uniforms.const_color[0].w == 128.0f / 255.0f);
    CHECK(block.uniforms.const_color[5].w == 1.0f);  // all six are read together
    CHECK(block.uniforms.tev_combiner_buffer_color.x == 1.0f);
    CHECK(block.uniforms.alphatest_ref == 0);        // its group was not dirty
    CHECK(block.uniforms.depth_scale == 0.0f);

    state->dirty.Reset();
    dirty = {};
    state->dirty.Set(freg::kAlphaTest);
    FillUniforms(*state, block, dirty, {}, none);
    CHECK(dirty.changed);
    CHECK(block.uniforms.alphatest_ref == 0x20);
    // 0xFF800000: r 0x00, g 0x00, b 0x80, a 0xFF
    CHECK(block.uniforms.blend_color.x == 0.0f);
    CHECK(block.uniforms.blend_color.y == 0.0f);
    CHECK(block.uniforms.blend_color.z == 128.0f / 255.0f);
    CHECK(block.uniforms.blend_color.w == 1.0f);

    state->dirty.Reset();
    dirty = {};
    state->dirty.Set(freg::kViewportDepthNearPlane);
    FillUniforms(*state, block, dirty, {}, none);
    CHECK(dirty.changed);
    CHECK(block.uniforms.depth_scale == 2.0f);
    CHECK(block.uniforms.depth_offset == 1.0f);

    // the context: a scale and the surface cache's own rectangle
    state->dirty.Reset();
    dirty = {};
    UniformContext context;
    context.framebuffer_scale = 3;
    FillUniforms(*state, block, dirty, context, none);
    CHECK(dirty.changed);
    CHECK(block.uniforms.framebuffer_scale == 3);
    CHECK(block.uniforms.scissor_x1 == 6);    // 2 * 3
    CHECK(block.uniforms.scissor_y2 == 36);   // (11 + 1) * 3
    dirty = {};
    context.scissor = UniformContext::Scissor{100, 200, 300, 400};
    FillUniforms(*state, block, dirty, context, none);
    CHECK(dirty.changed);
    CHECK(block.uniforms.scissor_x1 == 100);
    CHECK(block.uniforms.scissor_y1 == 200);
    CHECK(block.uniforms.scissor_x2 == 300);
    CHECK(block.uniforms.scissor_y2 == 400);

    // a feature's members: only when its register is dirty
    FogFeature fog;
    FeatureRegistry registry;
    registry.Add(fog);
    state->dirty.Reset();
    dirty = {};
    FillUniforms(*state, block, dirty, context, registry);
    CHECK_FALSE(dirty.changed);
    CHECK(block.uniforms.fog_color.x == 0.0f);
    state->dirty.Set(freg::kFogColor);
    FillUniforms(*state, block, dirty, context, registry);
    CHECK(dirty.changed);
    CHECK(block.uniforms.fog_color.z == 3.0f);
}

TEST_CASE("uniforms/the_first_block_is_written_whole_whatever_the_dirty_bits_say") {
    // a FragmentState made after the processor's first draws sees no dirty bit; its first block must still be right
    auto state = NewState();
    state->dirty.Reset();
    state->regs[0xC3] = 0xFF302010u;
    state->regs[freg::kAlphaTest] = 0x2000u;  // ref 0x20
    FogFeature fog;
    FeatureRegistry registry;
    registry.Add(fog);
    FragmentState fragment(DeviceProfile{}, registry);
    REQUIRE(fragment.Prepare(*state, {}, nullptr).ok());
    CHECK(fragment.block().uniforms.const_color[0].z == 48.0f / 255.0f);
    CHECK(fragment.block().uniforms.alphatest_ref == 0x20);
    CHECK(fragment.block().uniforms.fog_color.y == 2.0f);  // the feature's hook honours `all` too
    // and only the first
    state->regs[0xC3] = 0;
    UniformDirty dirty;
    CHECK_FALSE(dirty.all);
    REQUIRE(fragment.Prepare(*state, {}, nullptr).ok());
    CHECK(fragment.block().uniforms.const_color[0].z == 48.0f / 255.0f);
}

TEST_CASE("uniforms/the_key_is_stored_in_cfg_and_a_draw_with_no_dirty_state_changes_nothing") {
    auto state = NewState();
    state->dirty.SetAll();  // the first draw
    FeatureRegistry none;
    FragmentState fragment(DeviceProfile{}, none);
    UniformContext context;
    rhi::Result<FragmentState::Prepared> first = fragment.Prepare(*state, context, nullptr);
    REQUIRE(first.ok());
    CHECK(first->key_changed);
    CHECK(first->block_changed);
    CHECK(fragment.key_extractions() == 1);
    CHECK(fragment.block().cfg == KeyOfState(*state));
    // cfg is the key itself, as words: the first word is the framebuffer part's raw word (193 for these registers)
    u32 word0 = 0;
    std::memcpy(&word0, &fragment.block().cfg, 4);
    CHECK(word0 == 193);

    // the processor cleared the dirty flags after the draw: the next draw has nothing dirty
    state->dirty.Reset();
    rhi::Result<FragmentState::Prepared> second = fragment.Prepare(*state, context, nullptr);
    REQUIRE(second.ok());
    CHECK_FALSE(second->key_changed);
    CHECK_FALSE(second->block_changed);  // the previous allocation and binding set are reused
    CHECK(fragment.key_extractions() == 1);  // and the key was not extracted again
    CHECK(fragment.draws() == 2);

    // a dirty register that is not a fragment register (a vertex attribute register) does not extract the key either
    state->dirty.Set(0x200);
    CHECK_FALSE(fragment.Prepare(*state, context, nullptr)->block_changed);
    CHECK(fragment.key_extractions() == 1);

    // a dirty fragment register that changes the key
    state->regs[freg::kDepthmapEnable] = 1;
    state->dirty.Reset();
    state->dirty.Set(freg::kDepthmapEnable);
    rhi::Result<FragmentState::Prepared> third = fragment.Prepare(*state, context, nullptr);
    REQUIRE(third.ok());
    CHECK(third->key_changed);
    CHECK(third->block_changed);
    CHECK(fragment.key_extractions() == 2);
    CHECK(fragment.key().framebuffer.depthmap_enable() == re::DepthBuffering::ZBuffering);

    // a dirty fragment register that leaves the key as it was (a constant colour): extracted, key equal, the uniforms written
    state->regs[0xC3] = 0x11111111;
    state->dirty.Reset();
    state->dirty.Set(0xC3);
    rhi::Result<FragmentState::Prepared> fourth = fragment.Prepare(*state, context, nullptr);
    REQUIRE(fourth.ok());
    CHECK_FALSE(fourth->key_changed);
    CHECK(fourth->block_changed);
}

// ---- the pipeline key -------------------------------------------------------------------------------------------------

namespace {

native::PipelineKey SampleKey() {
    native::PipelineKey key;
    key.vertex.kind = native::VertexStageKind::Translated;
    key.vertex.program = 0x1122334455667788ull;
    key.vertex.input.binding_count = 2;
    key.vertex.input.bindings[0] = {16, 0};
    key.vertex.input.bindings[1] = {16, 1};
    key.vertex.input.attribute_count = 2;
    key.vertex.input.attributes[0] = {0, 0, rhi::Format::Rgba32Float, 0};
    key.vertex.input.attributes[1] = {1, 0, rhi::Format::Rg16Sint, 12};
    key.fixup = native::FixupStage::None;
    key.fragment.kind = native::FragmentKind::Specialised;
    key.fragment.key.framebuffer.set_alpha_test_func(re::CompareFunc::GreaterThan);
    key.state.blend_enabled = true;
    key.state.src_color = rhi::BlendFactor::SrcAlpha;
    key.state.dst_color = rhi::BlendFactor::OneMinusSrcAlpha;
    key.state.depth_test = true;
    key.state.depth_compare = rhi::CompareOp::LessEqual;
    key.state.depth_bias_slope = 1.5f;
    key.topology = rhi::Topology::TriangleStrip;
    key.color_format = rhi::Format::Rgba8Unorm;
    key.depth_format = rhi::Format::D24S8;
    return key;
}

}  // namespace

TEST_CASE("pipeline_key/equal_keys_hash_equal_and_every_field_matters") {
    const native::PipelineKey a = SampleKey();
    native::PipelineKey b = SampleKey();
    CHECK(a == b);
    CHECK(a.Hash() == b.Hash());
    CHECK(std::hash<FragmentKey>()(a.fragment.key) == std::hash<FragmentKey>()(b.fragment.key));

    auto differs = [&](auto&& change) {
        native::PipelineKey c = SampleKey();
        change(c);
        return !(a == c) && a.Hash() != c.Hash();
    };
    CHECK(differs([](native::PipelineKey& k) { k.pass = native::PassKind::ShadowWrite; }));
    CHECK(differs([](native::PipelineKey& k) { k.vertex.program += 1; }));
    CHECK(differs([](native::PipelineKey& k) { k.vertex.kind = native::VertexStageKind::Trivial; }));
    CHECK(differs([](native::PipelineKey& k) { k.vertex.input.bindings[1].stride = 32; }));
    CHECK(differs([](native::PipelineKey& k) { k.vertex.input.attributes[1].offset = 8; }));
    CHECK(differs([](native::PipelineKey& k) { k.vertex.input.attributes[1].format = rhi::Format::Rg32Float; }));
    CHECK(differs([](native::PipelineKey& k) { k.fixup = native::FixupStage::FixedGeometry; }));
    CHECK(differs([](native::PipelineKey& k) { k.fragment.key.lighting.set_enable(1); }));
    CHECK(differs([](native::PipelineKey& k) { k.fragment.kind = native::FragmentKind::Uber; }));
    CHECK(differs([](native::PipelineKey& k) { k.state.blend_enabled = false; }));
    CHECK(differs([](native::PipelineKey& k) { k.state.src_color = rhi::BlendFactor::DstAlpha; }));
    CHECK(differs([](native::PipelineKey& k) { k.state.color_write_mask = 7; }));
    CHECK(differs([](native::PipelineKey& k) { k.state.logic_op_enabled = true; }));
    CHECK(differs([](native::PipelineKey& k) { k.state.stencil_front.pass = rhi::StencilOp::Replace; }));
    CHECK(differs([](native::PipelineKey& k) { k.state.stencil_back.compare = rhi::CompareOp::Never; }));
    CHECK(differs([](native::PipelineKey& k) { k.state.cull = rhi::CullMode::Back; }));
    CHECK(differs([](native::PipelineKey& k) { k.state.depth_bias_slope = 2.5f; }));
    CHECK(differs([](native::PipelineKey& k) { k.topology = rhi::Topology::TriangleList; }));
    CHECK(differs([](native::PipelineKey& k) { k.color_format = rhi::Format::Rgb565; }));
    CHECK(differs([](native::PipelineKey& k) { k.depth_format = rhi::Format::D16Unorm; }));
    // the fragment key of an ubershader key is not part of it
    native::PipelineKey u1 = a.AsFallback(), u2 = a;
    u2.fragment.key.lighting.set_enable(1);
    u2 = u2.AsFallback();
    CHECK(u1 == u2);
    CHECK(u1.Hash() == u2.Hash());
}

TEST_CASE("pipeline_key/the_fallback_keeps_everything_but_the_fragment") {
    const native::PipelineKey a = SampleKey();
    const native::PipelineKey f = a.AsFallback();
    CHECK(f.IsFallback());
    CHECK_FALSE(a.IsFallback());
    CHECK(f.fragment.uber == native::UberModule::Color);
    CHECK(f.vertex == a.vertex);
    CHECK(f.fixup == a.fixup);
    CHECK(native::SameFixedFunctionState(f.state, a.state));
    CHECK(f.topology == a.topology);
    CHECK(f.color_format == a.color_format);
    CHECK(f.depth_format == a.depth_format);
    CHECK(f.pass == a.pass);
    native::PipelineKey shadow = a;
    shadow.pass = native::PassKind::ShadowWrite;
    CHECK(shadow.AsFallback().fragment.uber == native::UberModule::ShadowWrite);
    CHECK(f.AsFallback() == f);
}

TEST_CASE("pipeline_key/the_hashes_are_stable_constants") {
    // Written down after a first run; they must be the same on every run and every build (lists store them).
    const native::PipelineKey a = SampleKey();
    CHECK(HashKey(KeyOfState(*NewState())) == 0x3825C6F24EC238DCull);
    CHECK(a.Hash() == 0xC93AC51DD230FE21ull);
    CHECK(native::kPipelineKeyVersion == 0x1F602CC0C61F3C38ull);
    CHECK(kFragmentKeyStructHash == 0x8CE909FC30D36458ull);

    // A word with its top bit set must reach the fold: tev_stages[5].sources_raw is word 32 of the key (8 words of
    // framebuffer, the texture part's raw word and three border words, then six stages of four words: 12 + 5 * 4).
    // The constant is the fold computed apart from the code under test (splitmix64's finalizer, seed
    // HashCombine(0x4D48344E46534B, 70), one HashCombine per word) for the words {193, 0 ..., 0xFFFFFFFF at 32, 0 ...}.
    FragmentKey high = KeyOfState(*NewState());
    high.texture.tev_stages[5].sources_raw = 0xFFFFFFFFu;
    u32 words[kFragmentKeyWords];
    std::memcpy(words, &high, sizeof(words));
    CHECK(words[0] == 193);
    CHECK(words[32] == 0xFFFFFFFFu);
    CHECK(HashKey(high) == 0x9CE8EA9CD738A4D0ull);
}

TEST_CASE("pipeline_key/the_vertex_input_layout") {
    native::VertexInputKey input = SampleKey().vertex.input;
    const rhi::VertexLayout layout = input.ToLayout();
    REQUIRE(layout.bindings.size() == 2);
    CHECK(layout.bindings[1].binding == 1);
    CHECK(layout.bindings[1].stride == 16);
    CHECK(layout.bindings[1].per_instance);  // binding 1 of the sample key is the per-instance constant buffer
    REQUIRE(layout.attributes.size() == 2);
    CHECK(layout.attributes[1].location == 1);
    CHECK(layout.attributes[1].format == rhi::Format::Rg16Sint);
    CHECK(layout.attributes[1].offset == 12);
}

TEST_CASE("pipeline_key/the_vertex_input_from_N2_05s_description") {
    namespace vertex = mh4n::gpu::vertex;
    vertex::LoadedDraw draw;
    draw.buffers.resize(2);
    draw.buffers[0].stride = 24;
    draw.buffers[1].stride = 16;
    draw.buffers[1].per_instance = true;  // the constant buffer is the last binding
    for (u32 i = 0; i < 16; ++i) {
        draw.attributes[i].location = i;
        draw.attributes[i].binding = 1;  // no loader: the constant buffer
        draw.attributes[i].offset = 0;
        draw.attributes[i].format = vertex::VertexFormat::R32G32B32A32_FLOAT;
    }
    draw.attributes[0].binding = 0;
    draw.attributes[0].format = vertex::VertexFormat::R32G32B32_FLOAT;
    draw.attributes[1].binding = 0;
    draw.attributes[1].offset = 12;
    draw.attributes[1].format = vertex::VertexFormat::R16G16_SINT;
    const native::VertexInputKey key = native::MakeVertexInput(draw);
    CHECK(key.binding_count == 2);
    CHECK(key.bindings[0].stride == 24);
    CHECK(key.bindings[1].per_instance == 1);
    CHECK(key.attribute_count == 16);
    CHECK(key.attributes[0].format == rhi::Format::Rgb32Float);
    CHECK(key.attributes[1].format == rhi::Format::Rg16Sint);
    CHECK(key.attributes[1].offset == 12);
    CHECK(key.attributes[2].binding == 1);
    CHECK(key.attributes[15].location == 15);

    // every format has a host format
    using F = vertex::VertexFormat;
    CHECK(native::ToRhiFormat(F::R8_SINT) == rhi::Format::R8Sint);
    CHECK(native::ToRhiFormat(F::R8G8_SINT) == rhi::Format::Rg8Sint);
    CHECK(native::ToRhiFormat(F::R8G8B8A8_SINT) == rhi::Format::Rgba8Sint);
    CHECK(native::ToRhiFormat(F::R8_UINT) == rhi::Format::R8Uint);
    CHECK(native::ToRhiFormat(F::R8G8_UINT) == rhi::Format::Rg8Uint);
    CHECK(native::ToRhiFormat(F::R8G8B8A8_UINT) == rhi::Format::Rgba8Uint);
    CHECK(native::ToRhiFormat(F::R16_SINT) == rhi::Format::R16Sint);
    CHECK(native::ToRhiFormat(F::R16G16_SINT) == rhi::Format::Rg16Sint);
    CHECK(native::ToRhiFormat(F::R16G16B16A16_SINT) == rhi::Format::Rgba16Sint);
    CHECK(native::ToRhiFormat(F::R32_FLOAT) == rhi::Format::R32Float);
    CHECK(native::ToRhiFormat(F::R32G32_FLOAT) == rhi::Format::Rg32Float);
    CHECK(native::ToRhiFormat(F::R32G32B32_FLOAT) == rhi::Format::Rgb32Float);
    CHECK(native::ToRhiFormat(F::R32G32B32A32_FLOAT) == rhi::Format::Rgba32Float);
    CHECK(native::ToRhiTopology(vertex::Topology::TriangleFan) == rhi::Topology::TriangleFan);
    CHECK(native::ToRhiTopology(vertex::Topology::Shader) == rhi::Topology::TriangleList);
}
