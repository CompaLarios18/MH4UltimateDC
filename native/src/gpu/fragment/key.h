// Derived from Azahar src/video_core/shader/generator/pica_fs_config.h and pica_fs_config.cpp
// (FSConfig, FramebufferConfig, BlendConfig, TextureConfig, TextureBorder, TevStageConfigRaw,
// Light, LutConfig, LightConfig, ProcTexConfig, UserConfig and the StructHash scheme) and
// profile.h (Profile) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6. pica_fs_config.h and
// pica_fs_config.cpp say "Licensed under GPLv3 or any later version", profile.h says "Licensed under
// GPLv2 or any later version": this file is GPL-3.0-or-later.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_fs_config.h,
//   pica_fs_config.cpp, profile.h)
//
// The fragment key (N2-33; docs/native/gpu-fragment-design.md 4.3 and 4.8; Azahar's `FSConfig`).
//
// `FragmentKey` is the REQUESTED fragment configuration of a draw: what the registers ask for, with
// no device profile applied (the generator applies a `DeviceProfile` to a copy, `ApplyProfile`, never
// to a stored key). It is Azahar's `FSConfig` field for field (framebuffer, texture, lighting,
// procedural texture), plus two words Azahar's key lacks:
//   `user`  the "user" word (the HD pack's custom normal map, N2-17/N2-18);
//   `gas`   the reserved gas word, owned by `fog/` (N2-10): the gas registers are not in Azahar's key.
// Every structure is plain data with no padding (`std::has_unique_object_representations_v`), saved
// in lists (N2-16) and compared with memcmp; each has a `StructHash()` in the manner of Azahar's, so
// a change to a layout is seen by the list's header.
//
// Final names (the design called them proposals): `FragmentKey`, `MakeKey`, `DeviceProfile`,
// `ApplyProfile`, `Needs`, `NeedsOf`, `kFragmentKeyStructHash`, `HashKey`.
//
// The bit fields of Azahar's `BitField<first, count, T>` members are the `raw` word of each structure
// with the same first bit and width; each has a getter and a setter of the same name as Azahar's
// member (`alpha_test_func()`, `set_alpha_test_func(v)`).
//
// Threads: `MakeKey` reads `PicaState`, which is valid only during `pica::Backend::Draw`: it runs on
// the thread that owns the command processor.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <functional>
#include <type_traits>

#include "gpu/fragment/regs.h"
#include "gpu/fragment/struct_hash.h"
#include "mh4n/types.h"

namespace mh4n::gpu::pica {
struct PicaState;
}

namespace mh4n::gpu::fragment {

static_assert(std::endian::native == std::endian::little, "the key's words are hashed and stored little-endian");

namespace re = regenum;

// A bit field of a raw word: getter and setter generators.
#define MH4N_BITFIELD(name, type, first, count)                                                          \
    constexpr type name() const { return static_cast<type>(Bits(raw, first, count)); }                   \
    constexpr void set_##name(type v) { raw = SetBits(raw, first, count, static_cast<u32>(v)); }

// ---- the framebuffer part ----------------------------------------------------------------------

struct BlendConfig {
    re::BlendEquation eq{};
    re::BlendFactor src_factor{};
    re::BlendFactor dst_factor{};

    // The equation of a half whose min/max emulation is not needed (ApplyProfile): "-1", Azahar's marker.
    static constexpr re::BlendEquation kMinMaxEmulationDisabled = static_cast<re::BlendEquation>(0xFFFFFFFFu);

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = BlendConfig;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(eq), MH4N_FIELD_HASH(src_factor),
                           MH4N_FIELD_HASH(dst_factor));
    }

    constexpr void SetMinMaxEmulationDisabled() { eq = kMinMaxEmulationDisabled; }
    constexpr bool RequiresMinMaxEmulation() const {
        return eq == re::BlendEquation::Min || eq == re::BlendEquation::Max;
    }
    constexpr bool UsesNoOpFactors() const {
        return src_factor == re::BlendFactor::One && dst_factor == re::BlendFactor::One;
    }
};
static_assert(std::has_unique_object_representations_v<BlendConfig>);

struct DeviceProfile;

struct FramebufferConfig {
    // bits: alpha_test_func 0-2, scissor_test_mode 3-4, depthmap_enable 5, logic_op 6-9, shadow_rendering 10,
    // alphablend_enable 11
    u32 raw{};
    BlendConfig requested_rgb_blend{};
    BlendConfig requested_alpha_blend{};
    re::LogicOp requested_logic_op{};

    MH4N_BITFIELD(alpha_test_func, re::CompareFunc, 0, 3)
    MH4N_BITFIELD(scissor_test_mode, re::ScissorMode, 3, 2)
    MH4N_BITFIELD(depthmap_enable, re::DepthBuffering, 5, 1)
    MH4N_BITFIELD(logic_op, re::LogicOp, 6, 4)
    MH4N_BITFIELD(shadow_rendering, u32, 10, 1)
    MH4N_BITFIELD(alphablend_enable, u32, 11, 1)

    void ApplyProfile(const DeviceProfile& profile);

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = FramebufferConfig;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(requested_rgb_blend),
                           MH4N_FIELD_HASH(requested_alpha_blend), MH4N_FIELD_HASH(requested_logic_op),
                           BlendConfig::StructHash());
    }
};
static_assert(std::has_unique_object_representations_v<FramebufferConfig>);

// ---- the texture part ----------------------------------------------------------------------------

struct TevStageConfigRaw {
    u32 sources_raw{};
    u32 modifiers_raw{};
    u32 ops_raw{};
    u32 scales_raw{};

    // The fields, as Azahar's TevStageConfig reads them.
    constexpr re::TevOperation color_op() const { return static_cast<re::TevOperation>(Bits(ops_raw, 0, 4)); }
    constexpr re::TevOperation alpha_op() const { return static_cast<re::TevOperation>(Bits(ops_raw, 16, 4)); }
    constexpr u32 color_source(u32 i) const { return Bits(sources_raw, 4 * i, 4); }   // i = 0..2
    constexpr u32 alpha_source(u32 i) const { return Bits(sources_raw, 16 + 4 * i, 4); }
    constexpr u32 color_scale() const { return Bits(scales_raw, 0, 2); }
    constexpr u32 alpha_scale() const { return Bits(scales_raw, 16, 2); }

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = TevStageConfigRaw;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(sources_raw), MH4N_FIELD_HASH(modifiers_raw),
                           MH4N_FIELD_HASH(ops_raw), MH4N_FIELD_HASH(scales_raw));
    }
};
static_assert(std::has_unique_object_representations_v<TevStageConfigRaw>);

struct TextureBorder {
    u32 raw{};  // enable_s bit 0, enable_t bit 1
    MH4N_BITFIELD(enable_s, u32, 0, 1)
    MH4N_BITFIELD(enable_t, u32, 1, 1)

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = TextureBorder;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw));
    }
};
static_assert(std::has_unique_object_representations_v<TextureBorder>);

struct TextureConfig {
    // texture0_type 0-2, texture2_use_coord1 3, combiner_buffer_input 4-11, fog_mode 12-14, fog_flip 15,
    // shadow_texture_orthographic 16
    u32 raw{};
    std::array<TextureBorder, 3> texture_border_color{};
    std::array<TevStageConfigRaw, 6> tev_stages{};

    struct TextureWrap {
        re::WrapMode s{};
        re::WrapMode t{};

        static consteval u64 StructHash() {
            constexpr u64 STRUCT_VERSION = 0;
            using T = TextureWrap;
            return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(s), MH4N_FIELD_HASH(t));
        }
    };
    std::array<TextureWrap, 3> requested_wrap{};

    MH4N_BITFIELD(texture0_type, re::TextureType, 0, 3)
    MH4N_BITFIELD(texture2_use_coord1, u32, 3, 1)
    MH4N_BITFIELD(combiner_buffer_input, u32, 4, 8)
    MH4N_BITFIELD(fog_mode, re::FogMode, 12, 3)
    MH4N_BITFIELD(fog_flip, u32, 15, 1)
    MH4N_BITFIELD(shadow_texture_orthographic, u32, 16, 1)

    void ApplyProfile(const DeviceProfile& profile);

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = TextureConfig;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(texture_border_color),
                           MH4N_FIELD_HASH(tev_stages), MH4N_FIELD_HASH(requested_wrap), TextureBorder::StructHash(),
                           TevStageConfigRaw::StructHash(), TextureWrap::StructHash());
    }
};
static_assert(std::has_unique_object_representations_v<TextureConfig>);

// ---- the lighting part -------------------------------------------------------------------------

struct Light {
    u16 raw{};  // num 0-2, directional 3, two_sided_diffuse 4, dist_atten_enable 5, spot_atten_enable 6,
                // geometric_factor_0 7, geometric_factor_1 8, shadow_enable 9
    constexpr u32 Field(u32 first, u32 count) const { return Bits(raw, first, count); }
    constexpr void SetField(u32 first, u32 count, u32 v) { raw = static_cast<u16>(SetBits(raw, first, count, v)); }
    constexpr u32 num() const { return Field(0, 3); }
    constexpr u32 directional() const { return Field(3, 1); }
    constexpr u32 two_sided_diffuse() const { return Field(4, 1); }
    constexpr u32 dist_atten_enable() const { return Field(5, 1); }
    constexpr u32 spot_atten_enable() const { return Field(6, 1); }
    constexpr u32 geometric_factor_0() const { return Field(7, 1); }
    constexpr u32 geometric_factor_1() const { return Field(8, 1); }
    constexpr u32 shadow_enable() const { return Field(9, 1); }

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = Light;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw));
    }
};
static_assert(std::has_unique_object_representations_v<Light>);

struct LutConfig {
    u32 raw{};  // enable 0, abs_input 1, type 2-4
    // Needed for std::has_unique_object_representations_v
    u32 scale_bits{};

    MH4N_BITFIELD(enable, u32, 0, 1)
    MH4N_BITFIELD(abs_input, u32, 1, 1)
    MH4N_BITFIELD(type, re::LightingLutInput, 2, 3)
    float GetScale() const { return std::bit_cast<float>(scale_bits); }
    void SetScale(float value) { scale_bits = std::bit_cast<u32>(value); }

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = LutConfig;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(scale_bits));
    }
};
static_assert(std::has_unique_object_representations_v<LutConfig>);

struct LightConfig {
    // enable 0, src_num 1-4, bump_mode 5-6, bump_selector 7-8, bump_renorm 9, clamp_highlights 10, config 11-14,
    // enable_primary_alpha 15, enable_secondary_alpha 16, enable_shadow 17, shadow_primary 18,
    // shadow_secondary 19, shadow_invert 20, shadow_alpha 21, shadow_selector 22-23
    u32 raw{};
    LutConfig lut_d0{};
    LutConfig lut_d1{};
    LutConfig lut_sp{};
    LutConfig lut_fr{};
    LutConfig lut_rr{};
    LutConfig lut_rg{};
    LutConfig lut_rb{};
    std::array<Light, 8> lights{};

    MH4N_BITFIELD(enable, u32, 0, 1)
    MH4N_BITFIELD(src_num, u32, 1, 4)
    MH4N_BITFIELD(bump_mode, re::LightingBumpMode, 5, 2)
    MH4N_BITFIELD(bump_selector, u32, 7, 2)
    MH4N_BITFIELD(bump_renorm, u32, 9, 1)
    MH4N_BITFIELD(clamp_highlights, u32, 10, 1)
    MH4N_BITFIELD(config, re::LightingConfig, 11, 4)
    MH4N_BITFIELD(enable_primary_alpha, u32, 15, 1)
    MH4N_BITFIELD(enable_secondary_alpha, u32, 16, 1)
    MH4N_BITFIELD(enable_shadow, u32, 17, 1)
    MH4N_BITFIELD(shadow_primary, u32, 18, 1)
    MH4N_BITFIELD(shadow_secondary, u32, 19, 1)
    MH4N_BITFIELD(shadow_invert, u32, 20, 1)
    MH4N_BITFIELD(shadow_alpha, u32, 21, 1)
    MH4N_BITFIELD(shadow_selector, u32, 22, 2)

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = LightConfig;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(lut_d0),
                           MH4N_FIELD_HASH(lut_d1), MH4N_FIELD_HASH(lut_sp), MH4N_FIELD_HASH(lut_fr),
                           MH4N_FIELD_HASH(lut_rr), MH4N_FIELD_HASH(lut_rg), MH4N_FIELD_HASH(lut_rb),
                           MH4N_FIELD_HASH(lights), LutConfig::StructHash(), Light::StructHash());
    }
};
static_assert(std::has_unique_object_representations_v<LightConfig>);

// ---- the procedural texture part -----------------------------------------------------------------

struct ProcTexConfig {
    // enable 0, coord 1-2, u_clamp 3-5, v_clamp 6-8, color_combiner 9-12, alpha_combiner 13-16, lut_filter 17-19,
    // separate_alpha 20, noise_enable 21, u_shift 22-23, v_shift 24-25
    u32 raw{};
    s32 lut_width{};
    s32 lut_offset0{};
    s32 lut_offset1{};
    s32 lut_offset2{};
    s32 lut_offset3{};
    u16 lod_min{};
    u16 lod_max{};

    MH4N_BITFIELD(enable, u32, 0, 1)
    MH4N_BITFIELD(coord, u32, 1, 2)
    MH4N_BITFIELD(u_clamp, re::ProcTexClamp, 3, 3)
    MH4N_BITFIELD(v_clamp, re::ProcTexClamp, 6, 3)
    MH4N_BITFIELD(color_combiner, re::ProcTexCombiner, 9, 4)
    MH4N_BITFIELD(alpha_combiner, re::ProcTexCombiner, 13, 4)
    MH4N_BITFIELD(lut_filter, re::ProcTexFilter, 17, 3)
    MH4N_BITFIELD(separate_alpha, u32, 20, 1)
    MH4N_BITFIELD(noise_enable, u32, 21, 1)
    MH4N_BITFIELD(u_shift, re::ProcTexShift, 22, 2)
    MH4N_BITFIELD(v_shift, re::ProcTexShift, 24, 2)

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = ProcTexConfig;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(raw), MH4N_FIELD_HASH(lut_width),
                           MH4N_FIELD_HASH(lut_offset0), MH4N_FIELD_HASH(lut_offset1), MH4N_FIELD_HASH(lut_offset2),
                           MH4N_FIELD_HASH(lut_offset3), MH4N_FIELD_HASH(lod_min), MH4N_FIELD_HASH(lod_max));
    }
};
static_assert(std::has_unique_object_representations_v<ProcTexConfig>);

// ---- the two words Azahar's key lacks ------------------------------------------------------------

// Azahar's UserConfig (kept outside its cache: IsCacheable). Bit 0: use_custom_normal. Zero for now.
struct UserConfig {
    u32 raw{};
    MH4N_BITFIELD(use_custom_normal, u32, 0, 1)
};
static_assert(std::has_unique_object_representations_v<UserConfig>);

// The reserved gas word, owned by `fog/` (N2-10): its feature fills it in its `Key` hook and nobody else
// reads or writes it. Zero while the feature is a stub.
struct GasConfig {
    u32 raw{};
};
static_assert(std::has_unique_object_representations_v<GasConfig>);

// ---- the key ---------------------------------------------------------------------------------------

struct FragmentKey {
    FramebufferConfig framebuffer;
    TextureConfig texture;
    LightConfig lighting;
    ProcTexConfig proctex;
    UserConfig user;
    GasConfig gas;

    // Azahar's FSConfig::TevStageUpdatesCombinerBufferColor / Alpha.
    constexpr bool TevStageUpdatesCombinerBufferColor(u32 stage_index) const {
        return (stage_index < 4) && (texture.combiner_buffer_input() & (1u << stage_index));
    }
    constexpr bool TevStageUpdatesCombinerBufferAlpha(u32 stage_index) const {
        return (stage_index < 4) && ((texture.combiner_buffer_input() >> 4) & (1u << stage_index));
    }

    // Azahar's UsesSpirvIncompatibleConfig, kept for N2-08's reports: ShadowCube, shadow rendering and a
    // min/max half. Nothing here needs it (there is one generator).
    bool UsesSpirvIncompatibleConfig() const {
        return texture.texture0_type() == re::TextureType::ShadowCube || framebuffer.shadow_rendering() != 0 ||
               framebuffer.requested_rgb_blend.RequiresMinMaxEmulation() ||
               framebuffer.requested_alpha_blend.RequiresMinMaxEmulation();
    }

    void ApplyProfile(const DeviceProfile& profile);

    bool operator==(const FragmentKey& other) const { return std::memcmp(this, &other, sizeof(FragmentKey)) == 0; }

    static consteval u64 StructHash() {
        constexpr u64 STRUCT_VERSION = 0;
        using T = FragmentKey;
        return HashCombine(STRUCT_VERSION, MH4N_LAYOUT_HASH, MH4N_FIELD_HASH(framebuffer), MH4N_FIELD_HASH(texture),
                           MH4N_FIELD_HASH(lighting), MH4N_FIELD_HASH(proctex), MH4N_FIELD_HASH(user),
                           MH4N_FIELD_HASH(gas), FramebufferConfig::StructHash(), TextureConfig::StructHash(),
                           LightConfig::StructHash(), ProcTexConfig::StructHash());
    }
};
static_assert(std::has_unique_object_representations_v<FragmentKey>);
static_assert(std::is_trivially_copyable_v<FragmentKey>);
static_assert(std::is_standard_layout_v<FragmentKey>);

// Azahar's 272-byte FSConfig (32 + 136 + 76 + 28) plus the user and gas words: 280 bytes, 70 words. The design
// document's figure (an inference when it was written) holds.
inline constexpr size_t kFragmentKeyBytes = 280;
inline constexpr size_t kFragmentKeyWords = kFragmentKeyBytes / sizeof(u32);
static_assert(sizeof(FragmentKey) == kFragmentKeyBytes);
static_assert(sizeof(FramebufferConfig) == 32 && sizeof(TextureConfig) == 136 && sizeof(LightConfig) == 76 &&
              sizeof(ProcTexConfig) == 28);
static_assert(sizeof(FragmentKey) % sizeof(u32) == 0);

// The version of the whole key: lists store it in their header (N2-16).
inline constexpr u64 kFragmentKeyStructHash = FragmentKey::StructHash();

// A stable 64-bit hash of the key's words (the same on every run and build).
u64 HashKey(const FragmentKey& key);

// ---- the device profile ----------------------------------------------------------------------------

// What the generator needs to know about the device (Azahar's `Profile`, reduced). Built from
// `rhi::Capabilities` after the mask by `ProfileOf` (gpu/fragment/profile.h).
struct DeviceProfile {
    bool logic_op = false;             // hardware logic ops (with blending off)
    bool custom_border_color = false;  // samplers with any border colour
    bool interlock = false;            // fragment shader pixel interlock (the shadow write)
    bool geometry_shader = false;      // the fix-up stage can be a geometry shader
    // False on every device with the vendored backend: Vulkan's MIN and MAX ignore the blend factors,
    // the PICA's do not (design 5.3).
    bool has_blend_minmax_factor = false;

    bool operator==(const DeviceProfile&) const = default;
};

// ---- MakeKey -----------------------------------------------------------------------------------------

class FeatureRegistry;

// Azahar's FSConfig::FSConfig(regs), whole: every sub-struct, from the registers of `state`, in the
// canonical form of design 4.3: `requested_logic_op` is zero when `alphablend_enable` is set, and a half's
// requested blend (equation and factors) is zero unless its equation is Min or Max. The registered
// features' `Key` hooks then fill the reserved words (the registry given, the global one by default).
FragmentKey MakeKey(const pica::PicaState& state);
FragmentKey MakeKey(const pica::PicaState& state, const FeatureRegistry& features);

// ---- Needs -------------------------------------------------------------------------------------------

// What a draw with this key needs from the renderer (design 4.7).
struct Needs {
    bool shadow_write_pass = false;  // a pass with no attachments, a storage image, and the interlock or the CAS loop
    bool dst_copy = false;           // FeedbackCopy before the draw: an emulated logic op that reads the destination,
                                     // or a min/max emulation
    bool fixup_gs = false;           // lighting is on: the quaternion fix-up (acted on only with a translated vertex stage)
    bool lut_lf = false;             // the texel buffers to bind (the layout binds all three always)
    bool lut_rg = false;
    bool lut_rgba = false;
    u8 texture_units = 0;            // units with a texture or sampler to bind
    bool ordered_rmw = false;        // reserved for gas (N2-10)

    bool operator==(const Needs&) const = default;
    // ORs `other` into this (texture_units: the larger).
    void Merge(const Needs& other);
};

// Core's part: `shadow_write_pass` for the fragment operation mode Shadow; `dst_copy` for a logic op that
// the profile emulates and that reads the destination (eleven of the 16) and for a min/max half that the
// profile emulates; `fixup_gs` when lighting is on. ORed with each registered feature's.
Needs CoreNeedsOf(const FragmentKey& key, const DeviceProfile& profile);
Needs NeedsOf(const FragmentKey& key, const DeviceProfile& profile);
Needs NeedsOf(const FragmentKey& key, const DeviceProfile& profile, const FeatureRegistry& features);

// True for the eleven logic ops that read the destination colour (And, AndReverse, Invert, Nand, Or, Nor,
// Xor, Equiv, AndInverted, OrReverse, OrInverted); the other five (Clear, Copy, Set, CopyInverted, NoOp) do not.
constexpr bool LogicOpReadsDestination(re::LogicOp op) {
    switch (op) {
    case re::LogicOp::Clear:
    case re::LogicOp::Copy:
    case re::LogicOp::Set:
    case re::LogicOp::CopyInverted:
    case re::LogicOp::NoOp:
        return false;
    default:
        return true;
    }
}

} // namespace mh4n::gpu::fragment

namespace std {
template <>
struct hash<mh4n::gpu::fragment::FragmentKey> {
    size_t operator()(const mh4n::gpu::fragment::FragmentKey& key) const noexcept {
        return static_cast<size_t>(mh4n::gpu::fragment::HashKey(key));
    }
};
} // namespace std

#undef MH4N_BITFIELD
