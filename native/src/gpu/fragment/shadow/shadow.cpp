// Derived from Azahar src/video_core/renderer_software/sw_framebuffer.cpp (DecodeD24S8Shadow, EncodeD24X8Shadow,
// EncodeX24S8Shadow and Framebuffer::DrawShadowMapPixel: the shadow texel's bytes and the write rule),
// src/video_core/renderer_software/sw_rasterizer.cpp (the shadow branch of the fragment loop: the depth and the green
// intensity it passes; RasterizerSoftware::TextureColor: the Shadow2D coordinate, the texel index, the border test and the
// compare), src/video_core/renderer_software/sw_clipper.cpp (ConvertCubeCoord), src/video_core/renderer_software/
// sw_texturing.cpp (GetWrappedTexCoord), src/video_core/rasterizer_accelerated.cpp (the shadow uniforms of
// SyncDrawUniforms), src/video_core/pica_types.h (Float::FromRaw and Float::operator*: how a half is read and Pica's
// infinity times zero), src/video_core/pica/regs_framebuffer.h and regs_texturing.h (the fields of 0x130 and 0x8B) and
// src/video_core/shader/generator/glsl_fs_shader_gen.cpp (WriteShadow: the interlock section and the compare-and-swap loop;
// DefineShadowHelpers: EncodeShadow and UpdateShadow; the names and signatures of shadowTexture and shadowTextureCube) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: glsl_fs_shader_gen.cpp says "Licensed under GPLv3 or any later
// version"; sw_framebuffer.cpp, sw_rasterizer.cpp, sw_clipper.cpp, sw_texturing.cpp, rasterizer_accelerated.cpp, pica_types.h,
// regs_framebuffer.h and regs_texturing.h say "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp, rasterizer_accelerated.cpp)
// Copyright 2017 Citra Emulator Project (sw_framebuffer.cpp, sw_texturing.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp, pica_types.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (sw_clipper.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
//
// The `shadow/` feature (N2-12): the shadow write (fragment operation mode Shadow, the ShadowWrite pass) and the two shadow
// lookups (unit 0 Shadow2D and ShadowCube), as the SOFTWARE renderer does them (the phase's oracle), with Azahar's GLSL as the
// reference for the means: the R32_UINT storage view, the interlock section, the compare-and-swap loop, a texel fetch and a
// compare written by hand (no compare sampler).
//
// THE TEXEL. Read through the R32_UINT view, a texel is the word (depth << 8) | intensity (EncodeShadow): an RGBA8 texel decoded
// from guest bytes b0..b3 (red = b3) is b3 | b2 << 8 | b1 << 16 | b0 << 24, so the depth is b0 << 16 | b1 << 8 | b2 and the
// intensity b3, which is DecodeD24S8Shadow and TextureColor's z_ref and density. A border colour is turned into the same word
// (red the intensity; alpha, blue and green the depth's high, middle and low bytes) and compared like a texel.
//
// THE WRITE (DrawShadowMapPixel, per texel; ShadowWrite pass only): d = the fragment's depth as a colour draw computes it (core's
// `depth`), clamped to [0, 1], times 16777215.0, truncated; s = the TEV output's green as a byte, taken at the AlphaTest point
// (after core's alpha test, before fog/'s FogGas). If d >= ref_z nothing changes; else s = 0 writes the depth and keeps the
// intensity, s > 0 keeps the depth and writes min(ref_s, s'), s' = s / (c + l * d / ref_z) clamped to [0, 255] and truncated.
// The shader does no float arithmetic or comparison on a value that may be infinite or a NaN (a Vulkan compiler may assume there
// is none): c and l are classified by their bits. In this order: c infinite, or d > 0 and l infinite: s' = 0; else l * x is 0
// when d = 0 (Pica's infinity times zero, and a finite l times +0) and l * (d / ref_z) otherwise, the denominator is finite, and
// s' = min(s / den, 255) when den > 0, 255 when den = 0 (the intensity is kept), 0 when den < 0.
// Ordering: with the `interlock` capability the read-modify-write sits between beginInvocationInterlock and
// endInvocationInterlock (pixel_interlock_ordered, glslang's default); without it, Azahar's imageAtomicCompSwap loop, atomic but
// not primitive-ordered, guarded by !gl_HelperInvocation (an atomic in a helper invocation returns an undefined value). The
// loop is exact only when every fragment on a texel within one pass has s = 0, or every one has s > 0.
//
// THE LOOKUPS (TextureColor's shadow branch). Shadow2D: (u, v) = uv, divided by w unless the orthographic bit is set; z = |w|,
// taken before the division; s = int(u * width) and t = int(v * height), truncated toward zero; per axis, ClampToBorder takes
// the border below 0 or at the size and above, ClampToBorder2 at the size and above, any other index goes through
// GetWrappedTexCoord as written (its negative-side repeat is `uint(val) % size`); ONE texel, no filtering, fetched with no flip
// (the upload already reverses the rows, which is the software renderer's `t = height - 1 - wrap(t)`). The compare:
// z_int = int(min(z, 1) * 16777215) - shadow_texture_bias, signed and not clamped at 0; lit when the texel's depth >= z_int:
// (i, i, i, i) / 255 with i the texel's intensity; otherwise 0. ShadowCube: the face and the face coordinates are
// ConvertCubeCoord's and the compare depth is the major coordinate's magnitude (the orthographic bit is not read); from there
// Shadow2D's rules on the face's size. The face is read through the cube class (tex0_cube, the RGBA8 cube's sampled view, with
// samp0, nearest): the direction of the chosen texel's centre on the chosen face, at level 0, and the word rebuilt from the four
// channels by round(c * 255).
//
// WHERE THIS DIFFERS FROM AZAHAR'S GLSL (glsl_fs_shader_gen.cpp), on purpose:
//   - the lookups read ONE texel and apply the unit's wrap modes and border colour, where Azahar's GLSL mixes four taps, lights a
//     tap outside the map, shadows on equality (`p.x <= z`) and clamps z at 0; ShadowCube follows the software renderer, not
//     Azahar's Vulkan code;
//   - UpdateShadow computes s' as the software renderer does (s / (c + l * x), x = d / ref_z, with Pica's infinity times zero and
//     a clamp to [0, 255]), where Azahar's computes `l * d / ref` and does not clamp;
//   - s is the green ROUNDED to a byte (core's byteround of the colour output), where Azahar's GLSL truncates
//     (`uint(combiner_output.g * float(0xFF))`); and s is taken before fog/'s FogGas point, as the software renderer ignores fog in
//     shadow mode, where Azahar's GLSL takes it after WriteFog (and in gas mode returns before WriteShadow, writing nothing);
//   - the storage image is declared `coherent` (core's declaration), where Azahar's is not: the interlock path loads what an
//     earlier fragment stored;
//   - the compare-and-swap loop is guarded by !gl_HelperInvocation (Azahar's is not).
// WHERE THIS DIFFERS FROM THE SOFTWARE RENDERER, known (none of them is in the captures):
//   - core's alpha test (and its Never discard) runs before the tail; the software renderer skips the alpha test in shadow mode;
//   - an enabled unit 0 with address 0 gives (0, 0, 0, 1) in the software renderer before any compare; here it gives
//     (0, 0, 0, 0) (no word gives that through the compare, and the key holds no address);
//   - s' for three pairs of halves: a NaN half (the software renderer casts the NaN to u8, which C++ leaves undefined; here
//     s' = 0), c and l * x infinite with opposite signs (a NaN again; here s' = 0), and a denominator of -0 (c is 0x8000 and l * x a
//     negative zero: the software renderer's -infinity gives 0, this shader 255);
//   - GLSL's division is not correctly rounded and fp32 products near 2^24 are truncated, so a d, z_int, s' or texel index within
//     an ulp of a whole number can differ by one (the tests keep their values away from those edges).
#include <cstring>

#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"
#include "gpu/fragment/regs.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

namespace kw = keyword;

// Pica::f16::FromRaw(half).ToFloat32() (pica_types.h): a normal half exactly, a denormal renormalised exactly, zero with its
// sign, an infinity or a NaN kept (the mantissa moved to the top of fp32's).
float HalfToFloat(u32 half) {
    const u32 sign = (half >> 15) & 1u;
    s32 exponent = static_cast<s32>((half >> 10) & 0x1Fu);
    u32 mantissa = half & 0x3FFu;
    u32 bits;
    if (exponent == 0 && mantissa == 0) {
        bits = sign << 31;
    } else if (exponent == 0) {
        exponent = 127 - 15 + 1;
        while ((mantissa & 0x400u) == 0) {
            --exponent;
            mantissa <<= 1;
        }
        mantissa &= 0x3FFu;
        bits = (sign << 31) | (static_cast<u32>(exponent) << 23) | (mantissa << 13);
    } else if (exponent == 0x1F) {
        bits = (sign << 31) | (0xFFu << 23) | (mantissa << 13);
    } else {
        bits = (sign << 31) | (static_cast<u32>(exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool SameBits(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// The helpers of the lookups (both passes): GetWrappedTexCoord, TextureColor's border test, the border colour as a texel, the
// compare.
constexpr const char* kLookupHelpers = R"(
int mh4n_shadow_wrap(uint mode, int val, int size) {
    if (mode == 0u || (mode == 4u && val >= 0)) return clamp(val, 0, size - 1);
    if (mode == 1u) return val;
    if (mode == 3u) {
        uint coord = uint(val) % uint(2 * size);
        if (coord >= uint(size)) coord = uint(2 * size) - 1u - coord;
        return int(coord);
    }
    if (mode == 2u || mode == 4u || mode == 5u || mode == 6u || mode == 7u) return int(uint(val) % uint(size));
    return 0;
}
bool mh4n_shadow_border(uint mode, int val, int size) {
    return (mode == 1u && (val < 0 || val >= size)) || (mode == 5u && val >= size);
}
uint mh4n_shadow_border_word() {
    uvec4 b = uvec4(round(tex_border_color[0] * 255.0));
    return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
}
vec4 mh4n_shadow_compare(uint texel, float z) {
    int z_int = int(min(z, 1.0) * 16777215.0) - shadow_texture_bias;
    if (int(texel >> 8) >= z_int) return vec4(float(texel & 0xFFu) / 255.0);
    return vec4(0.0);
}
)";

// The helpers of the write (ShadowWrite pass only): the bit classification of c and l, s' and UpdateShadow.
constexpr const char* kWriteHelpers = R"(
bool mh4n_shadow_inf(float v) { return (floatBitsToUint(v) & 0x7FFFFFFFu) == 0x7F800000u; }
bool mh4n_shadow_nan(float v) { return (floatBitsToUint(v) & 0x7FFFFFFFu) > 0x7F800000u; }
uint mh4n_shadow_intensity(uint s, uint d, uint ref_z) {
    float c = shadow_bias_constant;
    float l = shadow_bias_linear;
    if (mh4n_shadow_nan(c) || mh4n_shadow_nan(l) || mh4n_shadow_inf(c) || (d > 0u && mh4n_shadow_inf(l))) return 0u;
    float lx = 0.0;
    if (d > 0u) lx = l * (float(d) / float(ref_z));
    float den = c + lx;
    if (den > 0.0) return uint(min(float(s) / den, 255.0));
    return den == 0.0 ? 255u : 0u;
}
uint EncodeShadow(uvec2 pixel) {
    return (pixel.x << 8) | pixel.y;
}
uint UpdateShadow(uint pixel, uint d, uint s) {
    uvec2 ref = DecodeShadow(pixel);
    if (d < ref.x) {
        if (s == 0u) {
            ref.x = d;
        } else {
            ref.y = min(ref.y, mh4n_shadow_intensity(s, d, ref.x));
        }
    }
    return EncodeShadow(ref);
}
)";

constexpr const char* kTailInterlock = R"(beginInvocationInterlock();
imageStore(shadow_buffer, mh4n_shadow_coord, uvec4(UpdateShadow(imageLoad(shadow_buffer, mh4n_shadow_coord).x, mh4n_shadow_d, mh4n_shadow_s)));
endInvocationInterlock();
)";

constexpr const char* kTailCompareAndSwap = R"(if (!gl_HelperInvocation) {
    uint mh4n_shadow_old = imageLoad(shadow_buffer, mh4n_shadow_coord).x;
    uint mh4n_shadow_seen;
    do {
        mh4n_shadow_seen = mh4n_shadow_old;
        mh4n_shadow_old = imageAtomicCompSwap(shadow_buffer, mh4n_shadow_coord, mh4n_shadow_seen,
                                              UpdateShadow(mh4n_shadow_seen, mh4n_shadow_d, mh4n_shadow_s));
    } while (mh4n_shadow_old != mh4n_shadow_seen);
}
)";

// Unit 0's requested wrap mode of an axis (0 = s, 1 = t), as the mode sees it.
std::string WrapOf(const Emitter& e, u32 axis) {
    return e.Field(kw::kTexWrap + axis, 0, 32).glsl;
}

// The border test, the wrapped fetch through `fetch` (a GLSL statement that sets `texel` from `ws` and `wt`) and the compare;
// `size`, `s`, `t` and `z` are locals of the function.
void EmitSelectAndCompare(Emitter& e, const char* fetch) {
    e.Line("uint wrap_s = {};", WrapOf(e, 0));
    e.Line("uint wrap_t = {};", WrapOf(e, 1));
    e.Raw("uint texel;\n"
          "if (mh4n_shadow_border(wrap_s, s, size.x) || mh4n_shadow_border(wrap_t, t, size.y)) {\n"
          "    texel = mh4n_shadow_border_word();\n"
          "} else {\n"
          "    int ws = mh4n_shadow_wrap(wrap_s, s, size.x);\n"
          "    int wt = mh4n_shadow_wrap(wrap_t, t, size.y);\n");
    e.Raw(fetch);
    e.Raw("}\n"
          "return mh4n_shadow_compare(texel, z);\n"
          "}\n");
}

// shadowTexture: unit 0 as Shadow2D.
void EmitShadowTexture(Emitter& e) {
    e.Raw("vec4 shadowTexture(vec2 uv, float w) {\n"
          "float z = abs(w);\n");
    e.If(e.Eq(e.Field(kw::kTexRaw, 16, 1), 0), [&] { e.Raw("uv /= w;\n"); });
    e.Raw("ivec2 size = textureSize(tex0_shadow, 0);\n"
          "int s = int(uv.x * float(size.x));\n"
          "int t = int(uv.y * float(size.y));\n");
    EmitSelectAndCompare(e, "    texel = texelFetch(tex0_shadow, ivec2(ws, wt), 0).r;\n");
}

// shadowTextureCube: unit 0 as ShadowCube. ConvertCubeCoord's face table; the texel is read through the cube class at the
// direction of its centre (the inverse of the face's row of the table).
void EmitShadowTextureCube(Emitter& e) {
    e.Raw("vec4 shadowTextureCube(vec2 uv, float w) {\n"
          "vec3 c = vec3(uv, w);\n"
          "vec3 a = abs(c);\n"
          "uint face;\n"
          "float x;\n"
          "float y;\n"
          "float major;\n"
          "if (a.x > a.y && a.x > a.z) {\n"
          "    if (c.x > 0.0) { face = 0u; y = -c.y; } else { face = 1u; y = c.y; }\n"
          "    x = -c.z;\n"
          "    major = c.x;\n"
          "} else if (a.y > a.z) {\n"
          "    if (c.y > 0.0) { face = 2u; x = c.x; } else { face = 3u; x = -c.x; }\n"
          "    y = c.z;\n"
          "    major = c.y;\n"
          "} else {\n"
          "    if (c.z > 0.0) { face = 4u; y = -c.y; } else { face = 5u; y = c.y; }\n"
          "    x = c.x;\n"
          "    major = c.z;\n"
          "}\n"
          "float z = abs(major);\n"
          "ivec2 size = textureSize(tex0_cube, 0);\n"
          "int s = int((x / major * 0.5 + 0.5) * float(size.x));\n"
          "int t = int((y / major * 0.5 + 0.5) * float(size.y));\n");
    EmitSelectAndCompare(e,
                         "    vec2 p = (vec2(ws, wt) + vec2(0.5)) / vec2(size) * 2.0 - vec2(1.0);\n"
                         "    vec3 dir;\n"
                         "    if (face == 0u) dir = vec3(1.0, -p.y, -p.x);\n"
                         "    else if (face == 1u) dir = vec3(-1.0, -p.y, p.x);\n"
                         "    else if (face == 2u) dir = vec3(p.x, 1.0, p.y);\n"
                         "    else if (face == 3u) dir = vec3(p.x, -1.0, -p.y);\n"
                         "    else if (face == 4u) dir = vec3(p.x, -p.y, 1.0);\n"
                         "    else dir = vec3(-p.x, -p.y, -1.0);\n"
                         "    uvec4 b = uvec4(round(textureLod(samplerCube(tex0_cube, samp0), dir, 0.0) * 255.0));\n"
                         "    texel = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);\n");
}

class ShadowFeature final : public FeatureModule {
public:
    const char* Name() const override { return "shadow"; }

    void Emit(Point point, EmitContext& ctx) const override {
        if (ctx.emitter == nullptr) {
            return;
        }
        Emitter& e = *ctx.emitter;
        // Branch on the pass, never on `shadow_rendering` or ctx.key (all zero in the ubershader).
        const bool write = e.pass() == ShaderPass::ShadowWrite;
        switch (point) {
        case Point::Declarations:
            e.Raw(kLookupHelpers);
            EmitShadowTexture(e);
            EmitShadowTextureCube(e);
            if (write) {
                e.Raw(kWriteHelpers);
            }
            break;
        case Point::AlphaTest:
            // s, before fog/'s FogGas point (which changes combiner_output), as core's colour output rounds it.
            if (write) {
                e.Raw("uint mh4n_shadow_s = uint(round(combiner_output.g * 255.0));\n");
            }
            break;
        case Point::Output:
            // The tail, at the top level of main: the interlock calls may not be in flow control or after a return.
            if (write) {
                e.Raw("uint mh4n_shadow_d = uint(clamp(depth, 0.0, 1.0) * 16777215.0);\n"
                      "ivec2 mh4n_shadow_coord = ivec2(gl_FragCoord.xy);\n");
                e.Raw(e.profile().interlock ? kTailInterlock : kTailCompareAndSwap);
            }
            break;
        default:
            break;
        }
    }

    // rasterizer_accelerated.cpp's "Sync shadow uniforms" (dirty.CheckShadow: 0x130 and 0x8B).
    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        if (!dirty.all && !state.dirty.Test(freg::kFramebufferShadow) && !state.dirty.Test(freg::kShadowTexture)) {
            return;
        }
        const u32 bias_word = state.regs[freg::kFramebufferShadow];
        const float constant = HalfToFloat(bias_word & 0xFFFFu);
        const float linear = HalfToFloat(bias_word >> 16);
        // regs.texturing.shadow.bias << 1: bits 1-23 (bit 0 is the orthographic bit).
        const int texture_bias = static_cast<int>(state.regs[freg::kShadowTexture] & 0x00FFFFFEu);
        FragmentUniforms& u = block.uniforms;
        if (dirty.all || !SameBits(u.shadow_bias_constant, constant) || !SameBits(u.shadow_bias_linear, linear) ||
            u.shadow_texture_bias != texture_bias) {
            u.shadow_bias_constant = constant;
            u.shadow_bias_linear = linear;
            u.shadow_texture_bias = texture_bias;
            dirty.Mark();
        }
    }

    // Core sets `Needs::shadow_write_pass`; nothing is left behind a stub.
    const char* Stubbed(const FragmentKey&) const override { return nullptr; }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(ShadowFeature)
}  // namespace mh4n::gpu::fragment
