// Derived from Azahar src/video_core/shader/generator/pica_fs_config.cpp (FramebufferConfig::
// FramebufferConfig, FramebufferConfig::ApplyProfile, TextureConfig::TextureConfig,
// TextureConfig::ApplyProfile, LightConfig::LightConfig, ProcTexConfig::ProcTexConfig, FSConfig::FSConfig)
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any
// later version"); the register positions come from src/video_core/pica/regs_*.h (GPL-2.0-or-later).
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_fs_config.cpp)
#include "gpu/fragment/key.h"

#include "gpu/fragment/feature.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

u32 R(const pica::PicaState& state, u32 id) {
    return state.regs[id];
}

float LutScale(u32 scale_field) {
    switch (scale_field) {
    case 0: return 1.0f;
    case 1: return 2.0f;
    case 2: return 4.0f;
    case 3: return 8.0f;
    case 6: return 0.25f;
    case 7: return 0.5f;
    default: return 0.0f;
    }
}

FramebufferConfig MakeFramebuffer(const pica::PicaState& state) {
    FramebufferConfig fb;
    const u32 fragop = R(state, freg::kFragmentOperation);
    const u32 blending = R(state, freg::kAlphaBlending);
    const u32 alpha_test = R(state, freg::kAlphaTest);

    fb.set_scissor_test_mode(static_cast<re::ScissorMode>(Bits(R(state, freg::kScissorMode), 0, 2)));
    fb.set_depthmap_enable(static_cast<re::DepthBuffering>(Bits(R(state, freg::kDepthmapEnable), 0, 1)));
    fb.set_shadow_rendering(Bits(fragop, 0, 2) == static_cast<u32>(re::FragmentOperationMode::Shadow) ? 1 : 0);
    fb.set_alpha_test_func(Bits(alpha_test, 0, 1) != 0 ? static_cast<re::CompareFunc>(Bits(alpha_test, 4, 3))
                                                        : re::CompareFunc::Always);
    fb.set_alphablend_enable(Bits(fragop, 8, 1));
    fb.set_logic_op(re::LogicOp::Copy);

    if (fb.alphablend_enable() != 0) {
        fb.requested_rgb_blend.eq = static_cast<re::BlendEquation>(Bits(blending, 0, 3));
        fb.requested_rgb_blend.src_factor = static_cast<re::BlendFactor>(Bits(blending, 16, 4));
        fb.requested_rgb_blend.dst_factor = static_cast<re::BlendFactor>(Bits(blending, 20, 4));
        fb.requested_alpha_blend.eq = static_cast<re::BlendEquation>(Bits(blending, 8, 3));
        fb.requested_alpha_blend.src_factor = static_cast<re::BlendFactor>(Bits(blending, 24, 4));
        fb.requested_alpha_blend.dst_factor = static_cast<re::BlendFactor>(Bits(blending, 28, 4));
        // The canonical rule (design 4.3): the logic op register is ignored while blending is on, and a half's
        // requested blend matters only for Min and Max (ApplyProfile reads the factors for nothing else).
        fb.requested_logic_op = re::LogicOp{};
        if (!fb.requested_rgb_blend.RequiresMinMaxEmulation()) {
            fb.requested_rgb_blend = BlendConfig{};
        }
        if (!fb.requested_alpha_blend.RequiresMinMaxEmulation()) {
            fb.requested_alpha_blend = BlendConfig{};
        }
    } else {
        fb.requested_logic_op = static_cast<re::LogicOp>(Bits(R(state, freg::kLogicOp), 0, 4));
    }
    return fb;
}

TextureConfig MakeTexture(const pica::PicaState& state) {
    TextureConfig tex;
    const u32 main_config = R(state, freg::kMainConfig);
    const u32 fog_word = R(state, freg::kFogModeAndCombiner);
    tex.set_texture0_type(static_cast<re::TextureType>(Bits(R(state, freg::kTexture0 + freg::kTextureParams), 28, 3)));
    tex.set_texture2_use_coord1(Bits(main_config, 13, 1) != 0 ? 1 : 0);
    // update_mask_rgb | update_mask_a << 4, the two nibbles of bits 8-15
    tex.set_combiner_buffer_input(Bits(fog_word, 8, 4) | (Bits(fog_word, 12, 4) << 4));
    tex.set_fog_mode(static_cast<re::FogMode>(Bits(fog_word, 0, 3)));
    tex.set_fog_flip(Bits(fog_word, 16, 1) != 0 ? 1 : 0);
    tex.set_shadow_texture_orthographic(Bits(R(state, freg::kShadowTexture), 0, 1) != 0 ? 1 : 0);

    const u32 unit_base[3] = {freg::kTexture0, freg::kTexture1, freg::kTexture2};
    for (u32 i = 0; i < 3; ++i) {
        const u32 params = R(state, unit_base[i] + freg::kTextureParams);
        tex.requested_wrap[i].s = static_cast<re::WrapMode>(Bits(params, 12, 3));
        tex.requested_wrap[i].t = static_cast<re::WrapMode>(Bits(params, 8, 3));
        tex.texture_border_color[i].set_enable_s(0);
        tex.texture_border_color[i].set_enable_t(0);
    }

    const u32 stage_base[6] = {freg::kTevStage0, freg::kTevStage1, freg::kTevStage2,
                               freg::kTevStage3, freg::kTevStage4, freg::kTevStage5};
    for (u32 i = 0; i < 6; ++i) {
        TevStageConfigRaw& stage = tex.tev_stages[i];
        stage.sources_raw = R(state, stage_base[i] + freg::kTevSources);
        stage.modifiers_raw = R(state, stage_base[i] + freg::kTevModifiers);
        stage.ops_raw = R(state, stage_base[i] + freg::kTevOps);
        stage.scales_raw = R(state, stage_base[i] + freg::kTevScales);
        if (stage.color_op() == re::TevOperation::Dot3_RGBA) {
            stage.sources_raw &= 0xFFF;
            stage.modifiers_raw &= 0xFFF;
            stage.ops_raw &= 0xF;
        }
    }
    return tex;
}

LightConfig MakeLighting(const pica::PicaState& state) {
    LightConfig light;
    if (Bits(R(state, freg::kLightingDisable), 0, 1) != 0) {
        return light;
    }

    const u32 config0 = R(state, freg::kLightingConfig0);
    const u32 config1 = R(state, freg::kLightingConfig1);
    const u32 max_light_index = Bits(R(state, freg::kLightingMaxLightIndex), 0, 3);

    light.set_enable(1);
    light.set_src_num(max_light_index + 1);
    light.set_config(static_cast<re::LightingConfig>(Bits(config0, 4, 4)));
    light.set_enable_primary_alpha(Bits(config0, 2, 1));
    light.set_enable_secondary_alpha(Bits(config0, 3, 1));
    light.set_bump_mode(static_cast<re::LightingBumpMode>(Bits(config0, 28, 2)));
    light.set_bump_selector(Bits(config0, 22, 2));
    light.set_bump_renorm(Bits(config0, 30, 1) == 0 ? 1 : 0);
    light.set_clamp_highlights(Bits(config0, 27, 1) != 0 ? 1 : 0);

    light.set_enable_shadow(Bits(config0, 0, 1) != 0 ? 1 : 0);
    if (light.enable_shadow() != 0) {
        light.set_shadow_primary(Bits(config0, 16, 1) != 0 ? 1 : 0);
        light.set_shadow_secondary(Bits(config0, 17, 1) != 0 ? 1 : 0);
        light.set_shadow_invert(Bits(config0, 18, 1) != 0 ? 1 : 0);
        light.set_shadow_alpha(Bits(config0, 19, 1) != 0 ? 1 : 0);
        light.set_shadow_selector(Bits(config0, 24, 2));
    }

    const u32 enable_slots = R(state, freg::kLightingLightEnable);
    for (u32 light_index = 0; light_index <= max_light_index; ++light_index) {
        const u32 num = Bits(enable_slots, 4 * light_index, 3);
        const u32 light_config = R(state, freg::kLight0 + freg::kLightStride * num + freg::kLightConfig);
        Light& l = light.lights[light_index];
        l.SetField(0, 3, num);
        l.SetField(3, 1, Bits(light_config, 0, 1) != 0 ? 1 : 0);   // directional
        l.SetField(4, 1, Bits(light_config, 1, 1) != 0 ? 1 : 0);   // two_sided_diffuse
        l.SetField(7, 1, Bits(light_config, 2, 1) != 0 ? 1 : 0);   // geometric_factor_0
        l.SetField(8, 1, Bits(light_config, 3, 1) != 0 ? 1 : 0);   // geometric_factor_1
        l.SetField(5, 1, Bits(config1, 24 + num, 1) == 0 ? 1 : 0);  // dist_atten_enable: !IsDistAttenDisabled
        l.SetField(6, 1, Bits(config1, 8 + num, 1) == 0 ? 1 : 0);   // spot_atten_enable: !IsSpotAttenDisabled
        l.SetField(9, 1, Bits(config1, num, 1) == 0 ? 1 : 0);       // shadow_enable: !IsShadowDisabled
    }

    const u32 abs_input = R(state, freg::kLightingAbsLutInput);
    const u32 lut_input = R(state, freg::kLightingLutInput);
    const u32 lut_scale = R(state, freg::kLightingLutScale);

    // (the LUT config, its disable bit in config1 (or none), the abs-input disable bit, the input field, the scale field)
    auto fill = [&](LutConfig& lut, int disable_bit, u32 abs_bit, u32 field) {
        lut.set_enable(disable_bit < 0 ? 1 : (Bits(config1, static_cast<u32>(disable_bit), 1) == 0 ? 1 : 0));
        if (lut.enable() != 0) {
            lut.set_abs_input(Bits(abs_input, abs_bit, 1) == 0 ? 1 : 0);
            lut.set_type(static_cast<re::LightingLutInput>(Bits(lut_input, field, 3)));
            lut.SetScale(LutScale(Bits(lut_scale, field, 3)));
        }
    };
    fill(light.lut_d0, 16, 1, 0);
    fill(light.lut_d1, 17, 5, 4);
    // lut_sp: "a dummy field due to lack of the corresponding register": always enabled
    fill(light.lut_sp, -1, 9, 8);
    fill(light.lut_fr, 19, 13, 12);
    fill(light.lut_rr, 20, 25, 24);
    fill(light.lut_rg, 21, 21, 20);
    fill(light.lut_rb, 22, 17, 16);
    return light;
}

ProcTexConfig MakeProcTex(const pica::PicaState& state) {
    ProcTexConfig pt;
    const u32 main_config = R(state, freg::kMainConfig);
    if (Bits(main_config, 10, 1) == 0) {
        return pt;
    }
    const u32 proctex = R(state, freg::kProcTex);
    const u32 lut = R(state, freg::kProcTexLut);
    const u32 offsets = R(state, freg::kProcTexLutOffset);

    pt.set_enable(1);
    pt.set_coord(Bits(main_config, 8, 2));
    pt.set_u_clamp(static_cast<re::ProcTexClamp>(Bits(proctex, 0, 3)));
    pt.set_v_clamp(static_cast<re::ProcTexClamp>(Bits(proctex, 3, 3)));
    pt.set_color_combiner(static_cast<re::ProcTexCombiner>(Bits(proctex, 6, 4)));
    pt.set_alpha_combiner(static_cast<re::ProcTexCombiner>(Bits(proctex, 10, 4)));
    pt.set_separate_alpha(Bits(proctex, 14, 1));
    pt.set_noise_enable(Bits(proctex, 15, 1));
    pt.set_u_shift(static_cast<re::ProcTexShift>(Bits(proctex, 16, 2)));
    pt.set_v_shift(static_cast<re::ProcTexShift>(Bits(proctex, 18, 2)));
    pt.lut_width = static_cast<s32>(Bits(lut, 11, 8));
    pt.lut_offset0 = static_cast<s32>(Bits(offsets, 0, 8));
    pt.lut_offset1 = static_cast<s32>(Bits(offsets, 8, 8));
    pt.lut_offset2 = static_cast<s32>(Bits(offsets, 16, 8));
    pt.lut_offset3 = static_cast<s32>(Bits(offsets, 24, 8));
    pt.lod_min = static_cast<u16>(Bits(lut, 3, 4));
    pt.lod_max = static_cast<u16>(Bits(lut, 7, 4));
    pt.set_lut_filter(static_cast<re::ProcTexFilter>(Bits(lut, 0, 3)));
    return pt;
}

} // namespace

FragmentKey MakeKey(const pica::PicaState& state) {
    return MakeKey(state, FeatureRegistry::Global());
}

FragmentKey MakeKey(const pica::PicaState& state, const FeatureRegistry& features) {
    FragmentKey key;
    key.framebuffer = MakeFramebuffer(state);
    key.texture = MakeTexture(state);
    key.lighting = MakeLighting(state);
    key.proctex = MakeProcTex(state);
    for (const FeatureModule* feature : features.All()) {
        feature->Key(state, key, ReservedWords{key.user.raw, key.gas.raw});
    }
    return key;
}

void FramebufferConfig::ApplyProfile(const DeviceProfile& profile) {
    // Emulate the logic op in the shader if needed and not supported.
    if (!profile.logic_op && alphablend_enable() == 0) {
        set_logic_op(requested_logic_op);
    }

    if (alphablend_enable() == 0) {
        return;
    }

    // Check if we don't need blend min/max emulation. (The profile is always a Vulkan one.)
    if (profile.has_blend_minmax_factor) {
        requested_rgb_blend.SetMinMaxEmulationDisabled();
        requested_alpha_blend.SetMinMaxEmulationDisabled();
    } else {
        // VK_BLEND_OP_MIN/MAX ignore the blend factors, unlike the PICA hardware.
        if (requested_rgb_blend.UsesNoOpFactors()) {
            requested_rgb_blend.SetMinMaxEmulationDisabled();
        }
        if (requested_alpha_blend.UsesNoOpFactors()) {
            requested_alpha_blend.SetMinMaxEmulationDisabled();
        }
    }
}

void TextureConfig::ApplyProfile(const DeviceProfile& profile) {
    // Emulate the custom border colour if needed and not supported.
    if (profile.custom_border_color) {
        return;
    }
    for (u32 i = 0; i < 3; ++i) {
        texture_border_color[i].set_enable_s(requested_wrap[i].s == re::WrapMode::ClampToBorder ? 1 : 0);
        texture_border_color[i].set_enable_t(requested_wrap[i].t == re::WrapMode::ClampToBorder ? 1 : 0);
    }
}

void FragmentKey::ApplyProfile(const DeviceProfile& profile) {
    framebuffer.ApplyProfile(profile);
    texture.ApplyProfile(profile);
}

u64 HashKey(const FragmentKey& key) {
    u32 words[kFragmentKeyWords];
    std::memcpy(words, &key, sizeof(words));
    u64 h = HashCombine(0x4D48344E46534Bull, kFragmentKeyWords);
    for (u32 w : words) {
        h = HashCombine(h, w);
    }
    return h;
}

void Needs::Merge(const Needs& other) {
    shadow_write_pass |= other.shadow_write_pass;
    dst_copy |= other.dst_copy;
    fixup_gs |= other.fixup_gs;
    lut_lf |= other.lut_lf;
    lut_rg |= other.lut_rg;
    lut_rgba |= other.lut_rgba;
    texture_units = texture_units > other.texture_units ? texture_units : other.texture_units;
    ordered_rmw |= other.ordered_rmw;
}

Needs CoreNeedsOf(const FragmentKey& key, const DeviceProfile& profile) {
    Needs needs;
    const FramebufferConfig& fb = key.framebuffer;
    needs.shadow_write_pass = fb.shadow_rendering() != 0;
    needs.fixup_gs = key.lighting.enable() != 0;

    if (fb.shadow_rendering() == 0) {
        // A logic op the profile emulates (blending off) reads the destination for eleven of the 16.
        if (!profile.logic_op && fb.alphablend_enable() == 0 && LogicOpReadsDestination(fb.requested_logic_op)) {
            needs.dst_copy = true;
        }
        // A min/max half that the profile emulates (both factors not One: hardware MIN and MAX ignore them).
        if (fb.alphablend_enable() != 0) {
            const bool rgb = fb.requested_rgb_blend.RequiresMinMaxEmulation() &&
                             (profile.has_blend_minmax_factor ? false : !fb.requested_rgb_blend.UsesNoOpFactors());
            const bool alpha = fb.requested_alpha_blend.RequiresMinMaxEmulation() &&
                               (profile.has_blend_minmax_factor ? false : !fb.requested_alpha_blend.UsesNoOpFactors());
            needs.dst_copy = needs.dst_copy || rgb || alpha;
        }
    }
    return needs;
}

Needs NeedsOf(const FragmentKey& key, const DeviceProfile& profile) {
    return NeedsOf(key, profile, FeatureRegistry::Global());
}

Needs NeedsOf(const FragmentKey& key, const DeviceProfile& profile, const FeatureRegistry& features) {
    Needs needs = CoreNeedsOf(key, profile);
    for (const FeatureModule* feature : features.All()) {
        needs.Merge(feature->NeedsOf(key, profile));
    }
    return needs;
}

} // namespace mh4n::gpu::fragment
