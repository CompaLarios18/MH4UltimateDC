// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (WriteBlending, WriteLogicOp),
// src/video_core/renderer_software/sw_rasterizer.cpp (PixelColor: the blend factors, SourceAlphaSaturate),
// sw_framebuffer.cpp (EvaluateBlendEquation, LogicOp) and src/video_core/pica/regs_framebuffer.h (the enumerator
// values) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: glsl_fs_shader_gen.cpp says "Licensed under
// GPLv3 or any later version"; the others say "GPLv2 or any later version"): the factor expressions of the min/max
// emulation, the 16 logic ops and the equations. The rest is ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp)
// Copyright 2017 Citra Emulator Project (sw_framebuffer.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h)
//
// The output stage of the PICA's fragment pipeline that the hardware cannot do (N2-13), as plain GLSL for the
// fragment generator to embed (fbops::OutputStageGlsl). No #version, no declarations outside the functions, and
// every name starts with mh4n_; a shader that includes it needs GLSL 450 (integer vectors and switch).
// Azahar's enumerator values (FramebufferRegs::LogicOp, BlendEquation, BlendFactor) are the `uint` arguments, so
// the generator passes the register fields as they are: literals in the specialised mode, config fields in the
// ubershader. Colours are the 0..1 floats of the shader; both functions round them to the PICA's 8 bits first.

// All 16 logic ops, on the 8-bit values of the source and destination colour, per channel (the PICA applies them to
// all four). `op` is Azahar's LogicOp: Clear 0, And 1, AndReverse 2, Copy 3, Set 4, CopyInverted 5, NoOp 6,
// Invert 7, Nand 8, Or 9, Nor 10, Xor 11, Equiv 12, AndInverted 13, OrReverse 14, OrInverted 15.
// `dst` is read only by the eleven ops that need it (And, AndReverse, Invert, Nand, Or, Nor, Xor, Equiv,
// AndInverted, OrReverse, OrInverted): for the other five any value will do, and the caller needs no copy of the
// target. NoOp returns `src`: the pipeline discards the colour with a colour write mask of 0, which keeps the
// depth write that a discard would lose.
vec4 mh4n_logic_op(uint op, vec4 src, vec4 dst) {
    uvec4 s = uvec4(clamp(floor(src * 255.0 + 0.5), 0.0, 255.0));
    uvec4 d = uvec4(clamp(floor(dst * 255.0 + 0.5), 0.0, 255.0));
    uvec4 r = s;
    switch (op) {
    case 0u: r = uvec4(0u); break;
    case 1u: r = s & d; break;
    case 2u: r = s & ~d; break;
    case 3u: r = s; break;
    case 4u: r = uvec4(255u); break;
    case 5u: r = ~s; break;
    case 6u: r = s; break;
    case 7u: r = ~d; break;
    case 8u: r = ~(s & d); break;
    case 9u: r = s | d; break;
    case 10u: r = ~(s | d); break;
    case 11u: r = s ^ d; break;
    case 12u: r = ~(s ^ d); break;
    case 13u: r = ~s & d; break;
    case 14u: r = s | ~d; break;
    case 15u: r = ~s | d; break;
    default: break;
    }
    return vec4(r & 255u) / 255.0;
}

// One blend factor, Azahar's BlendFactor (Zero 0, One 1, SourceColor 2, OneMinusSourceColor 3, DestColor 4,
// OneMinusDestColor 5, SourceAlpha 6, OneMinusSourceAlpha 7, DestAlpha 8, OneMinusDestAlpha 9, ConstantColor 10,
// OneMinusConstantColor 11, ConstantAlpha 12, OneMinusConstantAlpha 13, SourceAlphaSaturate 14) as the four lanes
// (r, g, b, a) that a colour half and an alpha half read. SourceAlphaSaturate is min(source alpha, 1 - destination
// alpha) for the colour and 1 for the alpha, as the software renderer and Vulkan have it.
vec4 mh4n_blend_factor(uint factor, vec4 src, vec4 dst, vec4 blend_color) {
    switch (factor) {
    case 0u: return vec4(0.0);
    case 1u: return vec4(1.0);
    case 2u: return src;
    case 3u: return vec4(1.0) - src;
    case 4u: return dst;
    case 5u: return vec4(1.0) - dst;
    case 6u: return src.aaaa;
    case 7u: return vec4(1.0) - src.aaaa;
    case 8u: return dst.aaaa;
    case 9u: return vec4(1.0) - dst.aaaa;
    case 10u: return blend_color;
    case 11u: return vec4(1.0) - blend_color;
    case 12u: return blend_color.aaaa;
    case 13u: return vec4(1.0) - blend_color.aaaa;
    case 14u: return vec4(vec3(min(src.a, 1.0 - dst.a)), 1.0);
    default: return vec4(1.0);
    }
}

// The helper of the two blend functions: the equation `eq` (Azahar's BlendEquation: Add 0, Subtract 1,
// ReverseSubtract 2, Min 3, Max 4; the sum and the differences clamped to 0..1) of the two products. Any other value
// of `eq` takes the larger (callers pass only 0 to 4).
float mh4n_blend_half(uint eq, float a, float b) {
    switch (eq) {
    case 0u: return clamp(a + b, 0.0, 1.0);
    case 1u: return clamp(a - b, 0.0, 1.0);
    case 2u: return clamp(b - a, 0.0, 1.0);
    case 3u: return min(a, b);
    default: return max(a, b);
    }
}

// The Min and Max blend equations with the PICA's factors, per half: when `eq_rgb` (or `eq_a`) is Min (3) or Max (4)
// that half is min or max of source * factor and destination * factor; ANY other equation (0 included, so the
// canonical zeros of a key give the source back) leaves that half of `src` as it is, for the fixed-function blender.
// Vulkan's MIN and MAX ignore the factors, so a Min or Max half whose factors are not both One is computed here and the
// pipeline blends it ONE, ZERO, ADD (state::PipelinePlan::rgb_in_shader and alpha_in_shader; pass the requested
// equation for a half the plan moved and 0 for the others). `dst` is the destination colour (the feedback copy of the
// target) and `blend_color` the blend constant as 0..1 floats; `src` and `dst` are rounded to 8 bits first.
vec4 mh4n_minmax_blend(uint eq_rgb, uint src_rgb, uint dst_rgb, uint eq_a, uint src_a, uint dst_a, vec4 src,
                       vec4 dst, vec4 blend_color) {
    vec4 s = floor(src * 255.0 + 0.5) / 255.0;
    vec4 d = floor(dst * 255.0 + 0.5) / 255.0;
    vec4 result = s;
    if (eq_rgb == 3u || eq_rgb == 4u) {
        vec3 a = s.rgb * mh4n_blend_factor(src_rgb, s, d, blend_color).rgb;
        vec3 b = d.rgb * mh4n_blend_factor(dst_rgb, s, d, blend_color).rgb;
        result.r = mh4n_blend_half(eq_rgb, a.r, b.r);
        result.g = mh4n_blend_half(eq_rgb, a.g, b.g);
        result.b = mh4n_blend_half(eq_rgb, a.b, b.b);
    }
    if (eq_a == 3u || eq_a == 4u) {
        float a = s.a * mh4n_blend_factor(src_a, s, d, blend_color).a;
        float b = d.a * mh4n_blend_factor(dst_a, s, d, blend_color).a;
        result.a = mh4n_blend_half(eq_a, a, b);
    }
    return result;
}

// The whole colour half in the shader, for the colour half that the plan sends here although it is not a Min or Max
// half (state::PipelinePlan::rgb_uses_blend_rgb): the equation `eq` (Add 0, Subtract 1, ReverseSubtract 2, and Min 3 and
// Max 4 too) of source * factor and destination * factor, clamped to 0..1, as a vec3; any other `eq` gives src.rgb.
// The plan sends a colour half here in two cases: its factors read the source alpha while the alpha half is the
// shader's (the blender would read the alpha the shader replaced), and it has a factor of each constant pair
// (ConstantColor and ConstantAlpha, which one constant colour cannot give). ORDER: pass the ORIGINAL source colour as
// `src`, not the result of mh4n_minmax_blend, whose alpha is the alpha half already replaced:
//     vec4 color = mh4n_minmax_blend(eq_rgb_if_minmax_else_0, src_rgb, dst_rgb, eq_a_if_moved_else_0, src_a, dst_a,
//                                    src, dst, blend_color);
//     color.rgb = mh4n_blend_rgb(eq_rgb, src_rgb, dst_rgb, src, dst, blend_color);   // only when the plan says so
vec3 mh4n_blend_rgb(uint eq, uint src_factor, uint dst_factor, vec4 src, vec4 dst, vec4 blend_color) {
    vec4 s = floor(src * 255.0 + 0.5) / 255.0;
    vec4 d = floor(dst * 255.0 + 0.5) / 255.0;
    if (eq > 4u) {
        return s.rgb;
    }
    vec3 a = s.rgb * mh4n_blend_factor(src_factor, s, d, blend_color).rgb;
    vec3 b = d.rgb * mh4n_blend_factor(dst_factor, s, d, blend_color).rgb;
    return vec3(mh4n_blend_half(eq, a.r, b.r), mh4n_blend_half(eq, a.g, b.g), mh4n_blend_half(eq, a.b, b.b));
}
