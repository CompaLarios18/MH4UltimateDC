// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version"); the TEV stage constructs (IsPassThroughTevStage, WriteTevStage, AppendColorModifier, AppendAlphaModifier, AppendColorCombiner, AppendAlphaCombiner, GetSource), the alpha test (WriteAlphaTestCondition) and the lighting constructs (WriteLighting) are taken in simplified form, with the spike's own names for the uniforms and the sources
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
//
// The skeleton of uber_color and its two forms (see shaders.h) for the N2-35 spike. The emitter of option 1
// (LiteralSource and the Emit* functions) follows Azahar's FragmentModule: the pass-through test of a TEV stage,
// the copy "combiner_buffer = next_combiner_buffer" and the update bits of the combiner buffer (the copy sits
// before the stage here and after it in Azahar: the spike measures compile cost, not agreement with the 3DS), the
// colour and alpha operation expressions (Dot3_RGBA takes its alpha from the colour result), and the lighting
// steps (light vector from the position and the view, half vector, one- or two-sided dot product, clamp of the
// highlights, the geometric factor, the LUT inputs and the attenuations). It simplifies them (no byte rounding
// per stage, no force_source3 rule for stage 0, no shadow, no custom normal, no tangent map, Lerp written as an
// expansion) and is not a copy of the whole file. The variable names combiner_buffer and next_combiner_buffer are
// Azahar's; the other names of the emitted GLSL are the spike's. The ubershader form (kUberBody) is the spike's own: the same
// expressions behind switches on key fields. The textures, the LUT fetch, the quaternion helper and the uniform
// layout are the spike's, from the PICA hardware description and glslang's public API.
#include "shaders.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

namespace spike {

namespace {

using std::string;
using std::to_string;

// ---- shared by both forms: nothing here reads a key field -------------------------------------------------------
const char* const kPrologue = R"GLSL(#version 450
layout(location = 0) in vec4 primary_color;
layout(location = 1) in vec2 texcoord0;
layout(location = 2) in vec2 texcoord1;
layout(location = 3) in vec2 texcoord2;
layout(location = 4) in vec4 normquat;
layout(location = 5) in vec3 view;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0, std140) uniform FragUniforms {
    vec4 tev_const[6];
    vec4 tev_cbuf;
    vec4 global_ambient;
    vec4 light_pos[3];
    vec4 light_spot[3];
    vec4 light_diffuse[3];
    vec4 light_specular0[3];
    vec4 light_specular1[3];
    vec4 light_ambient[3];
    ivec4 lut_offset[2];
    vec4 tex_color[4];
    vec4 params; // x: alpha reference (0..255), y: depth scale, z: depth offset, w: distance scale
} u;
layout(set = 0, binding = 2) uniform samplerBuffer lut_lf;

// Stand-ins for the texture units (the skeleton has no sampling): arithmetic on the texture coordinates.
vec4 tex_value_0() { return fract(u.tex_color[0] + vec4(texcoord0, texcoord0.x * texcoord0.y, 0.25)); }
vec4 tex_value_1() { return fract(u.tex_color[1] + vec4(texcoord1, texcoord1.x * texcoord1.y, 0.5)); }
vec4 tex_value_2() { return fract(u.tex_color[2] + vec4(texcoord2, texcoord2.x * texcoord2.y, 0.75)); }
vec4 tex_value_3() { return fract(u.tex_color[3] + vec4(texcoord2.yx, texcoord0)); }

vec3 quat_rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
vec3 surf_normal(vec3 t) { return normalize(quat_rotate(normalize(normquat), t)); }

// Two texel fetches and a lerp, as the spec asks (table `id`, input in 0..1).
float lut_sample(uint id, float x01) {
    float fi = clamp(x01, 0.0, 1.0) * 255.0;
    int i0 = int(fi);
    float fr = fi - float(i0);
    int i1 = min(i0 + 1, 255);
    int base = u.lut_offset[id >> 2u][id & 3u] + int(id) * 256;
    float a = texelFetch(lut_lf, base + i0).r;
    float b = texelFetch(lut_lf, base + i1).r;
    return mix(a, b, fr);
}
)GLSL";

// ---- option 1b: the ubershader source ---------------------------------------------------------------------------
const char* const kUberBody = R"GLSL(
vec4 g_pfc;
vec4 g_sfc;
vec4 g_cbuf;
vec4 g_next;
vec4 g_last;

uint f_light_flags(uint i) {
    switch (i) {
        case 0u: return f_light_flags_0();
        case 1u: return f_light_flags_1();
        default: return f_light_flags_2();
    }
}

vec4 tev_source(uint s, uint stage) {
    switch (s) {
        case 0u: return primary_color;
        case 1u: return g_pfc;
        case 2u: return g_sfc;
        case 3u: return tex_value_0();
        case 4u: return tex_value_1();
        case 5u: return tex_value_2();
        case 6u: return tex_value_3();
        case 13u: return g_cbuf;
        case 14u: return u.tev_const[stage];
        default: return g_last;
    }
}

vec3 mod_color(vec4 v, uint m) {
    switch (m) {
        case 1u: return vec3(1.0) - v.rgb;
        case 2u: return vec3(v.a);
        case 3u: return vec3(1.0 - v.a);
        case 4u: return vec3(v.r);
        case 5u: return vec3(1.0 - v.r);
        case 8u: return vec3(v.g);
        case 9u: return vec3(1.0 - v.g);
        case 12u: return vec3(v.b);
        case 13u: return vec3(1.0 - v.b);
        default: return v.rgb;
    }
}

float mod_alpha(vec4 v, uint m) {
    switch (m) {
        case 1u: return 1.0 - v.a;
        case 2u: return v.r;
        case 3u: return 1.0 - v.r;
        case 4u: return v.g;
        case 5u: return 1.0 - v.g;
        case 6u: return v.b;
        case 7u: return 1.0 - v.b;
        default: return v.a;
    }
}

vec3 color_op(uint op, vec3 a, vec3 b, vec3 c) {
    switch (op) {
        case 1u: return a * b;
        case 2u: return a + b;
        case 3u: return a + b - vec3(0.5);
        case 4u: return a * c + b * (vec3(1.0) - c);
        case 5u: return a - b;
        case 6u:
        case 7u: return vec3(dot(a - vec3(0.5), b - vec3(0.5)) * 4.0);
        case 8u: return a * b + c;
        case 9u: return min(a + b, vec3(1.0)) * c;
        default: return a;
    }
}

float alpha_op(uint op, float a, float b, float c) {
    switch (op) {
        case 1u: return a * b;
        case 2u: return a + b;
        case 3u: return a + b - 0.5;
        case 4u: return a * c + b * (1.0 - c);
        case 5u: return a - b;
        case 8u: return a * b + c;
        case 9u: return min(a + b, 1.0) * c;
        default: return a;
    }
}

float scale_of(uint s) { return (s == 1u) ? 2.0 : ((s == 2u) ? 4.0 : 1.0); }

void run_tev_stage(uint stage, uint sw, uint mw, uint ow, uint cw) {
    vec3 c0 = mod_color(tev_source(sw & 15u, stage), mw & 15u);
    vec3 c1 = mod_color(tev_source((sw >> 4u) & 15u, stage), (mw >> 4u) & 15u);
    vec3 c2 = mod_color(tev_source((sw >> 8u) & 15u, stage), (mw >> 8u) & 15u);
    float a0 = mod_alpha(tev_source((sw >> 12u) & 15u, stage), (mw >> 12u) & 15u);
    float a1 = mod_alpha(tev_source((sw >> 16u) & 15u, stage), (mw >> 16u) & 15u);
    float a2 = mod_alpha(tev_source((sw >> 20u) & 15u, stage), (mw >> 20u) & 15u);
    uint cop = ow & 15u;
    vec3 cr = color_op(cop, c0, c1, c2);
    float ar = alpha_op((ow >> 4u) & 15u, a0, a1, a2);
    if (cop == 7u) {
        ar = cr.r;
    }
    g_last = vec4(clamp(cr * scale_of(cw & 3u), vec3(0.0), vec3(1.0)),
                  clamp(ar * scale_of((cw >> 2u) & 3u), 0.0, 1.0));
}

float lut_input(uint sel, float nh, float vh, float nv, float ln, float spv, float cpv) {
    switch (sel) {
        case 0u: return nh;
        case 1u: return vh;
        case 2u: return nv;
        case 3u: return ln;
        case 4u: return spv;
        default: return cpv;
    }
}

float lut_term(uint id, uint cw, float nh, float vh, float nv, float ln, float spv, float cpv) {
    if ((cw & 1u) == 0u) {
        return 1.0;
    }
    float x = lut_input((cw >> 2u) & 7u, nh, vh, nv, ln, spv, cpv);
    float x01 = ((cw & 2u) != 0u) ? abs(x) : (x * 0.5 + 0.5);
    return lut_sample(id, x01) * float(1u << ((cw >> 5u) & 3u));
}

float da_term(uint cw, float dist) {
    if ((cw & 1u) == 0u) {
        return 1.0;
    }
    return lut_sample(7u, clamp(dist * u.params.w, 0.0, 1.0)) * float(1u << ((cw >> 5u) & 3u));
}

void light_i(uint i, vec3 N, vec3 V, inout vec3 diff, inout vec3 spec) {
    uint fl = f_light_flags(i);
    vec3 Lp = u.light_pos[i].xyz + view;
    float dist = length(Lp);
    vec3 L = Lp / dist;
    vec3 H = normalize(L + V);
    float nl_raw = dot(N, L);
    float ln = ((fl & 1u) != 0u) ? abs(nl_raw) : max(nl_raw, 0.0);
    float nh = dot(N, H);
    float vh = dot(V, H);
    float nv = dot(N, V);
    float spv = dot(-L, u.light_spot[i].xyz);
    float cpv = H.x;
    float d0 = lut_term(0u, f_lut_cfg_0(), nh, vh, nv, ln, spv, cpv);
    float d1 = lut_term(1u, f_lut_cfg_1(), nh, vh, nv, ln, spv, cpv);
    float sp = lut_term(2u, f_lut_cfg_2(), nh, vh, nv, ln, spv, cpv);
    float fr = lut_term(3u, f_lut_cfg_3(), nh, vh, nv, ln, spv, cpv);
    float rr = lut_term(4u, f_lut_cfg_4(), nh, vh, nv, ln, spv, cpv);
    float rg = ((f_lut_cfg_5() & 1u) != 0u) ? lut_term(5u, f_lut_cfg_5(), nh, vh, nv, ln, spv, cpv) : rr;
    float rb = ((f_lut_cfg_6() & 1u) != 0u) ? lut_term(6u, f_lut_cfg_6(), nh, vh, nv, ln, spv, cpv) : rr;
    float da = da_term(f_lut_cfg_7(), dist);
    vec3 s0;
    vec3 s1;
    switch (f_light_config()) {
        case 0u:
            s0 = vec3(d0);
            s1 = vec3(rr, rg, rb);
            break;
        case 1u:
            s0 = vec3(d0 * d1);
            s1 = vec3(rr, rg, rb) * fr;
            break;
        default:
            s0 = vec3(0.0);
            s1 = vec3(0.0);
            break;
    }
    float att = 1.0;
    if ((fl & 4u) != 0u) {
        att *= sp;
    }
    if ((fl & 8u) != 0u) {
        att *= da;
    }
    float geo = 1.0;
    if ((fl & 2u) != 0u) {
        float hh = dot(H, H);
        geo = (hh == 0.0) ? 0.0 : min(nh / hh, 1.0);
    }
    vec3 sp_i = (u.light_specular0[i].xyz * s0 + u.light_specular1[i].xyz * s1) * (geo * att);
    if (f_light_clamp() != 0u) {
        sp_i *= (nl_raw > 0.0) ? 1.0 : 0.0;
    }
    diff += u.light_ambient[i].xyz + u.light_diffuse[i].xyz * (ln * att);
    spec += sp_i;
}

void lighting() {
    vec3 V = normalize(view);
    vec3 N;
    switch (f_light_bump()) {
        case 1u: {
            vec4 t = tex_value_0();
            N = surf_normal(normalize(vec3(t.xy * 2.0 - 1.0, t.z + 0.25)));
            break;
        }
        default:
            N = surf_normal(vec3(0.0, 0.0, 1.0));
            break;
    }
    vec3 diff = vec3(0.0);
    vec3 spec = vec3(0.0);
    for (uint i = 0u; i < f_light_count(); ++i) {
        light_i(i, N, V, diff, spec);
    }
    g_pfc = vec4(clamp(diff + u.global_ambient.xyz, vec3(0.0), vec3(1.0)), primary_color.a);
    g_sfc = vec4(clamp(spec, vec3(0.0), vec3(1.0)), 1.0);
}

void main() {
    g_cbuf = vec4(0.0);
    g_next = u.tev_cbuf;
    g_last = vec4(0.0);
    g_pfc = primary_color;
    g_sfc = vec4(0.0);
    if (f_lit() != 0u) {
        lighting();
    }
)GLSL";

const char* const kUberTail = R"GLSL(
    vec4 rounded = round(g_last * 255.0) / 255.0;
    int a8 = int(rounded.a * 255.0 + 0.5);
    int ref8 = int(u.params.x);
    switch (f_alpha_test_func()) {
        case 0u: discard;
        case 2u: if (!(a8 == ref8)) { discard; } break;
        case 3u: if (!(a8 != ref8)) { discard; } break;
        case 4u: if (!(a8 < ref8)) { discard; } break;
        case 5u: if (!(a8 <= ref8)) { discard; } break;
        case 6u: if (!(a8 > ref8)) { discard; } break;
        case 7u: if (!(a8 >= ref8)) { discard; } break;
        default: break;
    }
    frag_color = rounded;
    float dz;
    if (f_depth_mode() == 0u) {
        dz = gl_FragCoord.z;
    } else {
        dz = clamp(gl_FragCoord.z * gl_FragCoord.w * 0.5, 0.0, 1.0);
    }
    gl_FragDepth = clamp(dz * u.params.y + u.params.z, 0.0, 1.0);
}
)GLSL";

// ---- option 1: the literal emitter ------------------------------------------------------------------------------
string SrcExpr(uint32_t s, uint32_t stage) {
    switch (s) {
    case kSrcPrimaryColor: return "primary_color";
    case kSrcPrimaryFragmentColor: return "pfc";
    case kSrcSecondaryFragmentColor: return "sfc";
    case kSrcTexture0: return "tex_value_0()";
    case kSrcTexture1: return "tex_value_1()";
    case kSrcTexture2: return "tex_value_2()";
    case kSrcTexture3: return "tex_value_3()";
    case kSrcPreviousBuffer: return "combiner_buffer";
    case kSrcConstant: return "u.tev_const[" + to_string(stage) + "]";
    default: return "last_tex_env_out";
    }
}

string ColorMod(uint32_t m, const string& x) {
    switch (m) {
    case 1: return "vec3(1.0) - " + x + ".rgb";
    case 2: return "vec3(" + x + ".a)";
    case 3: return "vec3(1.0 - " + x + ".a)";
    case 4: return "vec3(" + x + ".r)";
    case 5: return "vec3(1.0 - " + x + ".r)";
    case 8: return "vec3(" + x + ".g)";
    case 9: return "vec3(1.0 - " + x + ".g)";
    case 12: return "vec3(" + x + ".b)";
    case 13: return "vec3(1.0 - " + x + ".b)";
    default: return x + ".rgb";
    }
}

string AlphaMod(uint32_t m, const string& x) {
    switch (m) {
    case 1: return "1.0 - " + x + ".a";
    case 2: return x + ".r";
    case 3: return "1.0 - " + x + ".r";
    case 4: return x + ".g";
    case 5: return "1.0 - " + x + ".g";
    case 6: return x + ".b";
    case 7: return "1.0 - " + x + ".b";
    default: return x + ".a";
    }
}

uint32_t Arity(uint32_t op, bool color) {
    switch (op) {
    case kOpModulate:
    case kOpAdd:
    case kOpAddSigned:
    case kOpSubtract:
        return 2;
    case kOpLerp:
    case kOpMultiplyThenAdd:
    case kOpAddThenMultiply:
        return 3;
    case kOpDot3Rgb:
    case kOpDot3Rgba:
        return color ? 2 : 1;
    default:
        return 1;
    }
}

string ColorOpExpr(uint32_t op) {
    switch (op) {
    case kOpModulate: return "c0 * c1";
    case kOpAdd: return "c0 + c1";
    case kOpAddSigned: return "c0 + c1 - vec3(0.5)";
    case kOpLerp: return "c0 * c2 + c1 * (vec3(1.0) - c2)";
    case kOpSubtract: return "c0 - c1";
    case kOpDot3Rgb:
    case kOpDot3Rgba: return "vec3(dot(c0 - vec3(0.5), c1 - vec3(0.5)) * 4.0)";
    case kOpMultiplyThenAdd: return "c0 * c1 + c2";
    case kOpAddThenMultiply: return "min(c0 + c1, vec3(1.0)) * c2";
    default: return "c0";
    }
}

string AlphaOpExpr(uint32_t op) {
    switch (op) {
    case kOpModulate: return "a0 * a1";
    case kOpAdd: return "a0 + a1";
    case kOpAddSigned: return "a0 + a1 - 0.5";
    case kOpLerp: return "a0 * a2 + a1 * (1.0 - a2)";
    case kOpSubtract: return "a0 - a1";
    case kOpMultiplyThenAdd: return "a0 * a1 + a2";
    case kOpAddThenMultiply: return "min(a0 + a1, 1.0) * a2";
    default: return "a0";
    }
}

const char* ScaleLiteral(uint32_t s) { return s == 1 ? "2.0" : (s == 2 ? "4.0" : "1.0"); }

void EmitStage(string& o, const Key& key, uint32_t k) {
    const uint32_t sw = key.w[f_tev_src_0 + k];
    const uint32_t mw = key.w[f_tev_mod_0 + k];
    const uint32_t ow = key.w[f_tev_op_0 + k];
    const uint32_t cw = key.w[f_tev_scale_0 + k];
    const uint32_t cop = ow & 15u;
    const uint32_t aop = (ow >> 4u) & 15u;
    // As Azahar's IsPassThroughTevStage: Previous, Replace and scale 1 in both halves emit nothing.
    const bool pass_through = (sw & 15u) == kSrcPrevious && (mw & 15u) == 0 && cop == kOpReplace &&
                              ((sw >> 12u) & 15u) == kSrcPrevious && ((mw >> 12u) & 15u) == 0 &&
                              aop == kOpReplace && (cw & 3u) == 0 && ((cw >> 2u) & 3u) == 0;
    o += "    combiner_buffer = next_combiner_buffer;\n";
    if (!pass_through) {
        o += "    { // TEV stage " + to_string(k) + "\n";
        const uint32_t nc = Arity(cop, true);
        for (uint32_t i = 0; i < nc; ++i) {
            o += "        vec3 c" + to_string(i) + " = " +
                 ColorMod((mw >> (4 * i)) & 15u, SrcExpr((sw >> (4 * i)) & 15u, k)) + ";\n";
        }
        o += "        vec3 cr = " + ColorOpExpr(cop) + ";\n";
        if (cop == kOpDot3Rgba) {
            o += "        float ar = cr.r;\n";
        } else {
            const uint32_t na = Arity(aop, false);
            for (uint32_t i = 0; i < na; ++i) {
                o += "        float a" + to_string(i) + " = " +
                     AlphaMod((mw >> (12 + 4 * i)) & 15u, SrcExpr((sw >> (12 + 4 * i)) & 15u, k)) + ";\n";
            }
            o += "        float ar = " + AlphaOpExpr(aop) + ";\n";
        }
        o += string("        last_tex_env_out = vec4(clamp(cr * ") + ScaleLiteral(cw & 3u) +
             ", vec3(0.0), vec3(1.0)), clamp(ar * " + ScaleLiteral((cw >> 2u) & 3u) + ", 0.0, 1.0));\n";
        o += "    }\n";
    }
    const uint32_t upd = key.w[f_tev_upd];
    if (((upd >> k) & 1u) != 0) {
        o += "    next_combiner_buffer.rgb = last_tex_env_out.rgb;\n";
    }
    if (((upd >> (8 + k)) & 1u) != 0) {
        o += "    next_combiner_buffer.a = last_tex_env_out.a;\n";
    }
}

const char* const kInputNames[] = {"nh", "vh", "nv", "ln", "spv", "cpv"};

// The expression of an enabled LUT term: the same operations as lut_term in the ubershader, with the key's values.
string LutExpr(uint32_t id, uint32_t cw) {
    const uint32_t sel = (cw >> 2u) & 7u;
    const string in = kInputNames[sel < 5 ? sel : 5];
    const string x01 = (cw & 2u) != 0 ? "abs(" + in + ")" : in + " * 0.5 + 0.5";
    return "lut_sample(" + to_string(id) + "u, " + x01 + ") * " + to_string(1u << ((cw >> 5u) & 3u)) + ".0";
}

void EmitTerm(string& o, const char* name, uint32_t id, uint32_t cw) {
    o += string("            float ") + name + " = " + (((cw & 1u) != 0) ? LutExpr(id, cw) : string("1.0")) + ";\n";
}

void EmitLight(string& o, const Key& key, uint32_t i) {
    const string n = to_string(i);
    const uint32_t fl = key.w[f_light_flags_0 + i];
    const uint32_t config = key.w[f_light_config];
    auto cfg = [&](uint32_t lut) { return key.w[f_lut_cfg_0 + lut]; };
    o += "        { // light " + n + "\n";
    o += "            vec3 Lp = u.light_pos[" + n + "].xyz + view;\n";
    o += "            float dist = length(Lp);\n";
    o += "            vec3 L = Lp / dist;\n";
    o += "            vec3 H = normalize(L + V);\n";
    o += "            float nl_raw = dot(N, L);\n";
    o += (fl & 1u) != 0 ? "            float ln = abs(nl_raw);\n" : "            float ln = max(nl_raw, 0.0);\n";
    o += "            float nh = dot(N, H);\n";
    o += "            float vh = dot(V, H);\n";
    o += "            float nv = dot(N, V);\n";
    o += "            float spv = dot(-L, u.light_spot[" + n + "].xyz);\n";
    o += "            float cpv = H.x;\n";
    if (config == 0 || config == 1) {
        EmitTerm(o, "d0", kD0, cfg(kD0));
        if (config == 1) {
            EmitTerm(o, "d1", kD1, cfg(kD1));
            EmitTerm(o, "fr", kFR, cfg(kFR));
        }
        EmitTerm(o, "rr", kRR, cfg(kRR));
        if ((cfg(kRG) & 1u) != 0) {
            EmitTerm(o, "rg", kRG, cfg(kRG));
        } else {
            o += "            float rg = rr;\n";
        }
        if ((cfg(kRB) & 1u) != 0) {
            EmitTerm(o, "rb", kRB, cfg(kRB));
        } else {
            o += "            float rb = rr;\n";
        }
    }
    if ((fl & 4u) != 0) {
        EmitTerm(o, "sp", kSP, cfg(kSP));
    }
    if ((fl & 8u) != 0) {
        const uint32_t cw = cfg(kDA);
        if ((cw & 1u) != 0) {
            o += "            float da = lut_sample(7u, clamp(dist * u.params.w, 0.0, 1.0)) * " +
                 to_string(1u << ((cw >> 5u) & 3u)) + ".0;\n";
        } else {
            o += "            float da = 1.0;\n";
        }
    }
    if (config == 0) {
        o += "            vec3 s0 = vec3(d0);\n            vec3 s1 = vec3(rr, rg, rb);\n";
    } else if (config == 1) {
        o += "            vec3 s0 = vec3(d0 * d1);\n            vec3 s1 = vec3(rr, rg, rb) * fr;\n";
    } else {
        o += "            vec3 s0 = vec3(0.0);\n            vec3 s1 = vec3(0.0);\n";
    }
    o += "            float att = 1.0;\n";
    if ((fl & 4u) != 0) {
        o += "            att *= sp;\n";
    }
    if ((fl & 8u) != 0) {
        o += "            att *= da;\n";
    }
    o += "            float geo = 1.0;\n";
    if ((fl & 2u) != 0) {
        o += "            { float hh = dot(H, H); geo = (hh == 0.0) ? 0.0 : min(nh / hh, 1.0); }\n";
    }
    o += "            vec3 sp_i = (u.light_specular0[" + n + "].xyz * s0 + u.light_specular1[" + n +
         "].xyz * s1) * (geo * att);\n";
    if (key.w[f_light_clamp] != 0) {
        o += "            sp_i *= (nl_raw > 0.0) ? 1.0 : 0.0;\n";
    }
    o += "            diff += u.light_ambient[" + n + "].xyz + u.light_diffuse[" + n + "].xyz * (ln * att);\n";
    o += "            spec += sp_i;\n";
    o += "        }\n";
}

void EmitLighting(string& o, const Key& key) {
    o += "    { // lighting, " + to_string(key.w[f_light_count]) + " light(s)\n";
    o += "        vec3 V = normalize(view);\n";
    if (key.w[f_light_bump] == 1) {
        o += "        vec4 t = tex_value_0();\n";
        o += "        vec3 N = surf_normal(normalize(vec3(t.xy * 2.0 - 1.0, t.z + 0.25)));\n";
    } else {
        o += "        vec3 N = surf_normal(vec3(0.0, 0.0, 1.0));\n";
    }
    o += "        vec3 diff = vec3(0.0);\n        vec3 spec = vec3(0.0);\n";
    for (uint32_t i = 0; i < key.w[f_light_count]; ++i) {
        EmitLight(o, key, i);
    }
    o += "        pfc = vec4(clamp(diff + u.global_ambient.xyz, vec3(0.0), vec3(1.0)), primary_color.a);\n";
    o += "        sfc = vec4(clamp(spec, vec3(0.0), vec3(1.0)), 1.0);\n";
    o += "    }\n";
}

// ---- glslang -----------------------------------------------------------------------------------------------------
std::once_flag g_init_once;
double g_init_ms = 0.0;

} // namespace

string VertexSource() {
    return R"GLSL(#version 450
layout(location = 0) out vec4 primary_color;
layout(location = 1) out vec2 texcoord0;
layout(location = 2) out vec2 texcoord1;
layout(location = 3) out vec2 texcoord2;
layout(location = 4) out vec4 normquat;
layout(location = 5) out vec3 view;
void main() {
    // One full-screen triangle per instance; the instance index moves the colour and the depth a little.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    float inst = float(gl_InstanceIndex);
    gl_Position = vec4(p * 2.0 - 1.0, 0.05 + inst * 0.04, 1.0);
    primary_color = vec4(0.2 + p.x * 0.4, 0.3 + p.y * 0.35, 0.5 + inst * 0.02, 0.85 - inst * 0.01);
    texcoord0 = p;
    texcoord1 = vec2(p.y * 0.75 + 0.1, p.x * 0.5 + 0.2);
    texcoord2 = p * 0.5 + vec2(0.05, 0.1);
    normquat = vec4(0.1 + p.x * 0.15, 0.05 - p.y * 0.1, 0.1 + inst * 0.005, 0.9);
    view = vec3(p - vec2(0.5), 1.0);
}
)GLSL";
}

string WarmupFragmentSource() {
    string o = kPrologue;
    o += "void main() {\n    frag_color = vec4(primary_color.rgb, 1.0) * u.tex_color[0];\n";
    o += "    gl_FragDepth = gl_FragCoord.z;\n}\n";
    return o;
}

string UberSource() {
    string o = kPrologue;
    o += "\nlayout(set = 0, binding = 1, std140) uniform CfgBlock { uvec4 w[" + to_string(kBlockVec4s) + "]; } cfg;\n";
    o += "layout(constant_id = 0) const uint spec_uber = 0u;\n";
    for (uint32_t f = 0; f < kFieldCount; ++f) {
        const string name = FieldName(f);
        o += "layout(constant_id = " + to_string(f + 1) + ") const uint sc_" + name + " = 0u;\n";
        o += "uint f_" + name + "() {\n";
        o += "    if (spec_uber != 0u) {\n";
        o += "        return cfg.w[" + to_string(f >> 2) + "][" + to_string(f & 3) + "];\n";
        o += "    }\n";
        o += "    return sc_" + name + ";\n}\n";
    }
    o += kUberBody;
    for (uint32_t k = 0; k < 6; ++k) {
        const string n = to_string(k);
        o += "    g_cbuf = g_next;\n";
        o += "    run_tev_stage(" + n + "u, f_tev_src_" + n + "(), f_tev_mod_" + n + "(), f_tev_op_" + n +
             "(), f_tev_scale_" + n + "());\n";
        o += "    if (((f_tev_upd() >> " + n + "u) & 1u) != 0u) { g_next.rgb = g_last.rgb; }\n";
        o += "    if (((f_tev_upd() >> " + to_string(8 + k) + "u) & 1u) != 0u) { g_next.a = g_last.a; }\n";
    }
    o += kUberTail;
    return o;
}

string LiteralSource(const Key& key) {
    string o = kPrologue;
    o += "\nvoid main() {\n";
    o += "    vec4 combiner_buffer = vec4(0.0);\n";
    o += "    vec4 next_combiner_buffer = u.tev_cbuf;\n";
    o += "    vec4 last_tex_env_out = vec4(0.0);\n";
    o += "    vec4 pfc = primary_color;\n";
    o += "    vec4 sfc = vec4(0.0);\n";
    if (key.w[f_lit] != 0) {
        EmitLighting(o, key);
    }
    for (uint32_t k = 0; k < 6; ++k) {
        EmitStage(o, key, k);
    }
    o += "    vec4 rounded = round(last_tex_env_out * 255.0) / 255.0;\n";
    o += "    int a8 = int(rounded.a * 255.0 + 0.5);\n";
    o += "    int ref8 = int(u.params.x);\n";
    switch (key.w[f_alpha_test_func]) {
    case 0: o += "    discard;\n"; break;
    case 2: o += "    if (!(a8 == ref8)) { discard; }\n"; break;
    case 3: o += "    if (!(a8 != ref8)) { discard; }\n"; break;
    case 4: o += "    if (!(a8 < ref8)) { discard; }\n"; break;
    case 5: o += "    if (!(a8 <= ref8)) { discard; }\n"; break;
    case 6: o += "    if (!(a8 > ref8)) { discard; }\n"; break;
    case 7: o += "    if (!(a8 >= ref8)) { discard; }\n"; break;
    default: break; // Always
    }
    o += "    frag_color = rounded;\n";
    if (key.w[f_depth_mode] == 0) {
        o += "    float dz = gl_FragCoord.z;\n";
    } else {
        o += "    float dz = clamp(gl_FragCoord.z * gl_FragCoord.w * 0.5, 0.0, 1.0);\n";
    }
    o += "    gl_FragDepth = clamp(dz * u.params.y + u.params.z, 0.0, 1.0);\n";
    o += "}\n";
    return o;
}

double InitGlslang() {
    std::call_once(g_init_once, [] {
        const auto start = std::chrono::steady_clock::now();
        glslang::InitializeProcess();
        g_init_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    });
    return g_init_ms;
}

bool CompileGlsl(bool fragment, const string& source, std::vector<uint32_t>& spirv, string& error) {
    InitGlslang();
    spirv.clear();
    error.clear();
    const EShLanguage stage = fragment ? EShLangFragment : EShLangVertex;
    glslang::TShader shader(stage);
    const char* strings[1] = {source.c_str()};
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        error = string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        error = string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }
    const glslang::TIntermediate* intermediate = program.getIntermediate(stage);
    if (intermediate == nullptr) {
        error = "glslang produced no stage";
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true; // glslang is built without spirv-tools (N2-06)
    options.validate = false;
    std::vector<unsigned int> words;
    glslang::GlslangToSpv(*intermediate, words, &options);
    spirv.assign(words.begin(), words.end());
    return !spirv.empty();
}

std::vector<uint32_t> SaltSpirv(const std::vector<uint32_t>& spirv, const string& salt) {
    if (salt.empty() || spirv.size() < 5) {
        return spirv;
    }
    // Find the first OpSource (opcode 3): the debug section starts there.
    size_t at = 5;
    while (at < spirv.size()) {
        const uint32_t opcode = spirv[at] & 0xffffu;
        const uint32_t count = spirv[at] >> 16;
        if (count == 0) {
            return spirv;
        }
        if (opcode == 3) {
            break;
        }
        at += count;
    }
    if (at >= spirv.size()) {
        return spirv;
    }
    std::vector<uint32_t> text((salt.size() + 1 + 3) / 4, 0);
    std::memcpy(text.data(), salt.c_str(), salt.size());
    std::vector<uint32_t> out(spirv.begin(), spirv.begin() + static_cast<std::ptrdiff_t>(at));
    out.push_back(((1u + static_cast<uint32_t>(text.size())) << 16) | 4u); // OpSourceExtension
    out.insert(out.end(), text.begin(), text.end());
    out.insert(out.end(), spirv.begin() + static_cast<std::ptrdiff_t>(at), spirv.end());
    return out;
}

} // namespace spike
