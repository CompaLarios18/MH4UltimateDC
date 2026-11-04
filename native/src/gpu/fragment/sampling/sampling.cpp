// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (DefineTexUnitSampler: sampleTexUnit0 to
// sampleTexUnit3, the LOD lookups, the border emulation, the procedural slot's routing), src/video_core/rasterizer_accelerated.cpp
// (the tex_lod_bias and tex_border_color uniforms), src/video_core/rasterizer_cache/sampler_params.h and rasterizer_cache.h
// (SamplerParams, GetSampler), src/video_core/renderer_vulkan/vk_texture_runtime.cpp (Sampler::Sampler), pica_to_vk.h (the wrap
// and filter tables), src/video_core/pica/regs_texturing.h (the register fields) and src/video_core/renderer_software/
// sw_rasterizer.cpp and sw_texturing.cpp (TextureColor's border test and GetWrappedTexCoord: what the wrap modes 4 to 7 do) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: glsl_fs_shader_gen.cpp says "Licensed under GPLv3 or any later
// version"; the others say "GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp, vk_texture_runtime.cpp,
//   pica_to_vk.h, rasterizer_accelerated.cpp)
// Copyright 2023 Citra Emulator Project (sampler_params.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_cache.h, sw_rasterizer.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017 Citra Emulator Project (sw_texturing.cpp)
//
// The `sampling/` feature (N2-23): the four texture units of the fragment stage.
//
//   sampleTexUnit0 .. 2   a lookup in the unit's texture with the unit's sampler: unit 0 by its type (Texture2D with Azahar's
//                         explicit level of detail and the unit's bias, Projection2D with textureProj, TextureCube with the
//                         direction (texcoord0, texcoord0_w), Shadow2D and ShadowCube sent to shadow/'s lookups, Disabled
//                         vec4(0.0)); unit 1 and unit 2 as Texture2D, unit 2 on texcoord1 where the key says so
//   sampleTexUnit3        the procedural slot: ProcTex() where the key enables the procedural texture, else vec4(0.0)
//
// What differs from Azahar's text, on purpose, to give the software renderer's pixels (the oracle of this phase):
//   - the border test is `< 0 || >= 1` on the UNTRUNCATED coordinate, not Azahar's `< 0 || > 1`. It is the software renderer's
//     `s < 0 || s >= width` (on the texel index it truncates toward zero) only for a coordinate that is a whole number of
//     texels or beyond: at exactly 1.0 (`s == width`) it is the border, at exactly 0.0 it is texel 0. For a coordinate in
//     (-1/width, 0) the software renderer truncates to texel 0 and this shader, like the samplers of Azahar's accelerated
//     renderers (which floor), flags the border (mode 1) or wraps to the last texel (Repeat, Mirror, and the negative side of
//     modes 4 and 5). It reads the PROJECTED coordinate for Projection2D (the software renderer divides first) and is not
//     applied to the cube types (their face coordinates are always inside) or to Shadow2D (shadow/'s lookup tests the border
//     itself, on the projected texel index, and compares the border colour as a texel, on both border-colour paths);
//   - the wrap modes 4 (ClampToEdge2) and 5 (ClampToBorder2) are the software renderer's: a negative coordinate repeats, a
//     positive one clamps (4) or gives the border colour (5). Azahar's samplers cannot tell them from ClampToEdge and
//     ClampToBorder (a TODO in pica_to_vk.h); a sampler cannot say it either, so they are shader code here. The negative-side
//     repeat of 4 and 5 is applied to Texture2D (units 0 to 2) only, not to Projection2D or the cube types; mode 5's
//     positive-side border test is applied to every type but the cubes and Shadow2D, Projection2D included;
//   - a unit 0 type the PICA does not define (6 and 7) is sampled as Texture2D with the unprojected coordinate, as
//     sw_rasterizer.cpp's TextureColor does after its UNIMPLEMENTED() and as Azahar's DefineTexUnitSampler falls back;
//   - the border colour uniform of a unit that is off is transparent black and that of an enabled unit with address 0 opaque black
//     (TextureColor returns those before its border test; Azahar's uniform is the register's colour whatever the unit);
//   - the level of detail is computed before any early return (derivatives stay in uniform control flow), and the lookup is
//     made before the border test.
// The key is complete (N2-33); this feature reads the requested wrap modes and the texture-0 type from it and the device
// profile from the emitter, and writes only `tex_lod_bias` and `tex_border_color`.
#include "gpu/fragment/sampling/sampling.h"

#include <cstring>

#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"
#include "gpu/fragment/regs.h"

namespace mh4n::gpu::fragment {
namespace {

namespace freg_local {
inline constexpr u32 kTextureAddress = 4;  // the fifth word of a unit's five (address in bits 0-27, in units of 8 bytes)
}

using sampling::kClampToBorder;
using sampling::kClampToBorder2;
using sampling::kClampToEdge2;

// TextureConfig::TextureType.
constexpr u32 kTexture2D = 0, kTextureCube = 1, kShadow2D = 2, kProjection2D = 3, kShadowCube = 4, kDisabled = 5;

// The wrap mode of a unit's axis (0 = s, 1 = t): requested_wrap[unit] is two words, s then t.
Value WrapOf(const Emitter& e, u32 unit, u32 axis) {
    return e.Field(keyword::kTexWrap + 2 * unit + axis, 0, 32);
}

const char* Lane(u32 axis) { return axis == 0 ? "x" : "y"; }

// The wrap modes 4 and 5 repeat a negative coordinate (and the sampler clamps the positive one): `tcw` is the coordinate the
// lookup uses, `tc` the one the interpolators gave.
void EmitNegativeRepeat(Emitter& e, u32 unit) {
    for (u32 axis = 0; axis < 2; ++axis) {
        const Value wrap = WrapOf(e, unit, axis);
        e.If(e.Or(e.Eq(wrap, kClampToEdge2), e.Eq(wrap, kClampToBorder2)),
             [&] { e.Line("if (tc.{0} < 0.0) tcw.{0} = fract(tc.{0});", Lane(axis)); });
    }
}

// The border colour: emulated for ClampToBorder where the device has no custom border colour; always for ClampToBorder2
// (positive side only: the negative side repeated above). `tcb` is the coordinate to test.
void EmitBorderTests(Emitter& e, u32 unit) {
    for (u32 axis = 0; axis < 2; ++axis) {
        const Value wrap = WrapOf(e, unit, axis);
        const Cond emulate = e.profile().custom_border_color ? Emitter::False() : e.Eq(wrap, kClampToBorder);
        e.If(emulate, [&] { e.Line("if (tcb.{0} < 0.0 || tcb.{0} >= 1.0) return tex_border_color[{1}];", Lane(axis), unit); });
        e.If(e.Eq(wrap, kClampToBorder2),
             [&] { e.Line("if (tcb.{0} >= 1.0) return tex_border_color[{1}];", Lane(axis), unit); });
    }
}

// The coordinate pair a unit reads: unit 2 reads texcoord1 where the key's texture2_use_coord1 says so.
std::string CoordOf(const Emitter& e, u32 unit) {
    if (unit != 2) {
        return fmt::format("texcoord{}", unit);
    }
    const Cond use1 = e.Eq(e.Field(keyword::kTexRaw, 3, 1), 1);
    if (use1.known) {
        return use1.value ? "texcoord1" : "texcoord2";
    }
    return fmt::format("({} ? texcoord1 : texcoord2)", use1.glsl);
}

// The lookup of a plain 2D unit (units 1 and 2, and unit 0 as Texture2D): Azahar's `textureLod(texN, texcoordM, getLod(texcoordM
// * vec2(textureSize(texN, 0))) + tex_lod_bias[N])` with the separate texture and sampler objects.
void EmitLod2D(Emitter& e, u32 unit, const char* result) {
    e.Line("vec2 tcw = tc;");
    EmitNegativeRepeat(e, unit);
    e.Line("float lod = getLod(tc * vec2(textureSize(tex{0}, 0))) + tex_lod_bias[{0}];", unit);
    e.Line("{} = textureLod(sampler2D(tex{}, samp{}), tcw, lod);", result, unit, unit);
}

void EmitUnit(Emitter& e, u32 unit) {
    e.Line("vec4 sampleTexUnit{}() {{", unit);

    if (unit == 3) {
        // The procedural slot (glsl_fs_shader_gen.cpp: `if (config.proctex.enable) return ProcTex(); else return vec4(0.0)`).
        e.If(e.Eq(e.Field(keyword::kProcTexRaw, 0, 1), 1), [&] { e.Raw("return ProcTex();\n"); },
             [&] { e.Raw("return vec4(0.0);\n"); });
        if (!e.specialised()) {
            e.Raw("return vec4(0.0);\n");
        }
        e.Raw("}\n");
        return;
    }

    if (unit == 0) {
        const Value type = e.Field(keyword::kTexRaw, 0, 3);
        if (e.specialised() && type.literal == kDisabled) {
            e.Raw("return vec4(0.0);\n}\n");
            return;
        }
        e.If(e.Eq(type, kDisabled), [&] { e.Raw("return vec4(0.0);\n"); });
        e.Raw("vec2 tc = texcoord0;\nvec2 tcb = tc;\n");
        e.If(e.Eq(type, kProjection2D), [&] { e.Raw("tcb = tc / texcoord0_w;\n"); });
        e.Raw("vec4 result = vec4(0.0);\n");
        e.Switch(
            type,
            {
                {kTexture2D, [&] { EmitLod2D(e, 0, "result"); }},
                // TODO (wwylele) in Azahar: the exact LOD formula for a projection texture; the implicit one.
                {kProjection2D, [&] { e.Raw("result = textureProj(sampler2D(tex0, samp0), vec3(tc, texcoord0_w));\n"); }},
                {kTextureCube, [&] { e.Raw("result = texture(samplerCube(tex0_cube, samp0), vec3(texcoord0, texcoord0_w));\n"); }},
                {kShadow2D, [&] { e.Raw("result = shadowTexture(texcoord0, texcoord0_w);\n"); }},
                {kShadowCube, [&] { e.Raw("result = shadowTextureCube(texcoord0, texcoord0_w);\n"); }},
            },
            // a type the PICA does not define (6, 7): the software renderer logs and samples it as a 2D texture with the unprojected
            // coordinate (only Disabled skips the unit), and so does Azahar's generator (`texture(tex0, texcoord0)`)
            [&] { EmitLod2D(e, 0, "result"); });
        // The cube types' face coordinates are inside by construction: no border. Shadow2D's border is shadow/'s (the projected
        // texel index, the border colour compared as a texel, on both border-colour paths): none here either.
        e.If(e.And(e.And(e.Ne(type, kTextureCube), e.Ne(type, kShadowCube)), e.Ne(type, kShadow2D)), [&] { EmitBorderTests(e, 0); });
        e.Raw("return result;\n}\n");
        return;
    }

    // Units 1 and 2: Texture2D.
    e.Line("vec2 tc = {};", CoordOf(e, unit));
    e.Raw("vec2 tcb = tc;\nvec4 result;\n");
    EmitLod2D(e, unit, "result");
    EmitBorderTests(e, unit);
    e.Raw("return result;\n}\n");
}

class Sampling final : public FeatureModule {
public:
    const char* Name() const override { return "sampling"; }

    void Emit(Point point, EmitContext& ctx) const override {
        if (point != Point::Declarations || ctx.emitter == nullptr) {
            return;
        }
        for (u32 unit = 0; unit < 4; ++unit) {
            EmitUnit(*ctx.emitter, unit);
        }
    }

    // rasterizer_accelerated.cpp's "Sync texture unit uniforms" (dirty.CheckTexUnits: border colour and LOD of each unit).
    // One difference, for the software renderer's pixels: the border colour of a unit that is off is transparent black and that
    // of an enabled unit with address 0 is opaque black (TextureColor returns those before it looks at the border), so the
    // shader's border emulation, which cannot know a unit is off, returns them too.
    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        const u32 base[3] = {freg::kTexture0, freg::kTexture1, freg::kTexture2};
        const bool main_dirty = state.dirty.Test(freg::kMainConfig);
        for (u32 unit = 0; unit < 3; ++unit) {
            const u32 border_reg = base[unit] + freg::kTextureBorderColor;
            const u32 lod_reg = base[unit] + freg::kTextureLod;
            const u32 address_reg = base[unit] + freg_local::kTextureAddress;
            if (!dirty.all && !main_dirty && !state.dirty.Test(border_reg) && !state.dirty.Test(lod_reg) &&
                !state.dirty.Test(address_reg)) {
                continue;
            }
            const float bias = sampling::LodBiasOf(state.regs[lod_reg]);
            const bool enabled = Bits(state.regs[freg::kMainConfig], unit, 1) != 0;
            const bool no_address = Bits(state.regs[address_reg], 0, 28) == 0;
            const Vec4f border = !enabled ? Vec4f{0.0f, 0.0f, 0.0f, 0.0f}
                                          : (no_address ? Vec4f{0.0f, 0.0f, 0.0f, 1.0f} : ColorRGBA8(state.regs[border_reg]));
            float* bias_lane = &block.uniforms.tex_lod_bias.x + unit;
            Vec4f& slot = block.uniforms.tex_border_color[unit];
            if (*bias_lane != bias || std::memcmp(&slot, &border, sizeof(Vec4f)) != 0 || dirty.all) {
                *bias_lane = bias;
                slot = border;
                dirty.Mark();
            }
        }
    }

    // The TEV sources only: BindTextures also binds the units that lighting reads (bump and shadow selectors); nothing uses
    // `texture_units` yet.
    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        Needs needs;
        for (u32 unit = 0; unit < 3; ++unit) {
            if (TevUsesSource(key, 3 + unit)) {
                needs.texture_units = static_cast<u8>(unit + 1);
            }
        }
        return needs;
    }

    // Nothing is left behind a stub here: every unit is sampled. The shadow lookups (shadow/) and the procedural texture
    // (proctex/) report themselves.
    const char* Stubbed(const FragmentKey&) const override { return nullptr; }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(Sampling)

// ---- the host side (sampling.h) -----------------------------------------------------------------------------------------------

namespace sampling {

SamplerParams SamplerParamsOf(const u32* regs, u32 unit) {
    const u32 base = unit == 0 ? freg::kTexture0 : (unit == 1 ? freg::kTexture1 : freg::kTexture2);
    const u32 params = regs[base + freg::kTextureParams];
    const u32 lod = regs[base + freg::kTextureLod];
    SamplerParams out;
    out.mag_filter = Bits(params, 1, 1);
    out.min_filter = Bits(params, 2, 1);
    out.mip_filter = Bits(params, 24, 1);
    out.wrap_t = Bits(params, 8, 3);
    out.wrap_s = Bits(params, 12, 3);
    out.border_color = regs[base + freg::kTextureBorderColor];
    out.lod_max = Bits(lod, 16, 4);
    out.lod_min = Bits(lod, 24, 4);
    return out;
}

float LodBiasOf(u32 lod_word) {
    const s32 bias = static_cast<s32>(Bits(lod_word, 0, 13) << 19) >> 19;
    return static_cast<float>(bias) / 256.0f;
}

rhi::SamplerDesc SamplerDescOf(const SamplerParams& params, const rhi::Capabilities& caps) {
    auto filter = [](u32 f) { return f != 0 ? rhi::Filter::Linear : rhi::Filter::Nearest; };
    // pica_to_vk.h's WrapMode table, with the two differences this feature owns (see the top of the file): the modes 4 and 5
    // are clamp-to-edge samplers (the shader repeats a negative coordinate and, for 5, tests the border), and mode 1 is
    // clamp-to-edge where the device cannot take a border colour (the shader tests it).
    auto address = [&](u32 wrap) {
        switch (wrap) {
        case kClampToBorder: return caps.custom_border_color ? rhi::AddressMode::ClampToBorder : rhi::AddressMode::ClampToEdge;
        case kRepeat:
        case kRepeat2:
        case kRepeat3: return rhi::AddressMode::Repeat;
        case kMirroredRepeat: return rhi::AddressMode::MirroredRepeat;
        default: return rhi::AddressMode::ClampToEdge;  // 0, 4, 5, and anything else
        }
    };
    rhi::SamplerDesc desc;
    desc.mag_filter = filter(params.mag_filter);
    desc.min_filter = filter(params.min_filter);
    desc.mip_filter = filter(params.mip_filter);
    desc.address_u = address(params.wrap_s);
    desc.address_v = address(params.wrap_t);
    desc.address_w = rhi::AddressMode::ClampToEdge;
    if (caps.custom_border_color && (params.wrap_s == kClampToBorder || params.wrap_t == kClampToBorder)) {
        const u32 c = params.border_color;
        desc.custom_border_color = std::array<float, 4>{static_cast<float>(c & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                                                        static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                                                        static_cast<float>((c >> 24) & 0xFF) / 255.0f};
    }
    desc.min_lod = static_cast<float>(params.lod_min);
    desc.max_lod = static_cast<float>(params.lod_max < params.lod_min ? params.lod_min : params.lod_max);
    desc.mip_lod_bias = 0.0f;  // the bias is a uniform added in the shader (tex_lod_bias)
    desc.max_anisotropy = 1.0f;
    return desc;
}

rhi::Result<std::shared_ptr<rhi::Sampler>> SamplerCache::Get(const SamplerParams& wanted) {
    // Two parameter sets that make the same sampler are one key: the border colour matters only to a ClampToBorder axis of a
    // device that takes one.
    SamplerParams key = wanted;
    const rhi::Capabilities& caps = backend_->Caps();
    if (!caps.custom_border_color || (key.wrap_s != kClampToBorder && key.wrap_t != kClampToBorder)) {
        key.border_color = 0;
    }
    if (key.lod_max < key.lod_min) {
        key.lod_max = key.lod_min;
    }
    const auto it = samplers_.find(key);
    if (it != samplers_.end()) {
        ++hits_;
        return it->second;
    }
    rhi::Result<std::shared_ptr<rhi::Sampler>> made = backend_->CreateSampler(SamplerDescOf(key, caps));
    if (!made.ok()) {
        return rhi::Error{made.error()};
    }
    ++creates_;
    samplers_.emplace(key, *made);
    return *made;
}

rhi::Result<void> UploadRgba8(rhi::Recorder& rec, rhi::Ring& staging, rhi::Image& image, u32 mip, u32 layer, u32 width, u32 height,
                              std::span<const u8> rgba) {
    const u64 row = static_cast<u64>(width) * 4;
    const u64 size = row * height;
    if (width == 0 || height == 0 || rgba.size() < size) {
        return rhi::Error{"UploadRgba8: the texel data is smaller than width * height * 4"};
    }
    rhi::Result<rhi::RingAllocation> alloc = staging.Map(size, 16);
    if (!alloc.ok()) {
        return rhi::Error{alloc.error()};
    }
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(alloc->data.data() + y * row, rgba.data() + static_cast<u64>(height - 1 - y) * row, row);
    }
    staging.GetBuffer()->Flush(alloc->offset, size);
    rhi::Result<void> committed = staging.Commit(size);
    if (!committed.ok()) {
        return committed;
    }
    rhi::BufferImageRegion region;
    region.buffer_offset = alloc->offset;
    region.mip_level = mip;
    region.layer = layer;
    region.image_rect = rhi::Rect{0, 0, width, height};
    region.aspect = rhi::ImageAspect::Color;
    rec.CopyBufferToImage(*staging.GetBuffer(), image, std::span<const rhi::BufferImageRegion>(&region, 1));
    return rhi::Result<void>{};
}

}  // namespace sampling
}  // namespace mh4n::gpu::fragment
