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

// The blend equation of a half that the pipeline leaves to the shader, with the PICA's factors: per half, min or max
// (Min 3, Max 4) of source * factor and destination * factor, or their sum (Add 0), difference (Subtract 1) or
// reversed difference (ReverseSubtract 2), clamped to 0..1; `eq_rgb` and `eq_a` are Azahar's BlendEquation. A half
// whose equation is another value (use 0xFFFFFFFFu) is returned as the source, for the fixed-function blender.
// Vulkan's MIN and MAX ignore the factors, so a Min or Max half whose factors are not both One is computed here and the
// pipeline blends it ONE, ZERO, ADD; the sum and the differences are computed here only for the colour half when the
// alpha half is, and its factors read the source alpha (the pipeline would see the alpha the shader replaced).
// `dst` is the destination colour (the feedback copy of the target) and `blend_color` the blend constant as 0..1 floats.
float mh4n_blend_half(uint eq, float a, float b) {
    switch (eq) {
    case 0u: return clamp(a + b, 0.0, 1.0);
    case 1u: return clamp(a - b, 0.0, 1.0);
    case 2u: return clamp(b - a, 0.0, 1.0);
    case 3u: return min(a, b);
    default: return max(a, b);
    }
}

vec4 mh4n_minmax_blend(uint eq_rgb, uint src_rgb, uint dst_rgb, uint eq_a, uint src_a, uint dst_a, vec4 src,
                       vec4 dst, vec4 blend_color) {
    vec4 s = floor(src * 255.0 + 0.5) / 255.0;
    vec4 d = floor(dst * 255.0 + 0.5) / 255.0;
    vec4 result = s;
    if (eq_rgb <= 4u) {
        vec3 a = s.rgb * mh4n_blend_factor(src_rgb, s, d, blend_color).rgb;
        vec3 b = d.rgb * mh4n_blend_factor(dst_rgb, s, d, blend_color).rgb;
        result.r = mh4n_blend_half(eq_rgb, a.r, b.r);
        result.g = mh4n_blend_half(eq_rgb, a.g, b.g);
        result.b = mh4n_blend_half(eq_rgb, a.b, b.b);
    }
    if (eq_a <= 4u) {
        float a = s.a * mh4n_blend_factor(src_a, s, d, blend_color).a;
        float b = d.a * mh4n_blend_factor(dst_a, s, d, blend_color).a;
        result.a = mh4n_blend_half(eq_a, a, b);
    }
    return result;
}
