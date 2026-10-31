// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (DefineBindingsVK: the set and binding numbers of the three texel buffers, 3, 4 and 5) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
//
#version 450
#extension GL_GOOGLE_include_directive : require
#include "fs_block.glsl"
// The three texel buffers read at the offsets the uniforms keep.
layout(set = 0, binding = 3) uniform samplerBuffer lut_lf;    // R32G32_SFLOAT
layout(set = 0, binding = 4) uniform samplerBuffer lut_rg;    // R32G32_SFLOAT
layout(set = 0, binding = 5) uniform samplerBuffer lut_rgba;  // R32G32B32A32_SFLOAT
layout(location = 0) out vec4 o_color;
void main() {
    vec4 a = texelFetch(lut_lf, fog_lut_offset);
    vec4 b = texelFetch(lut_rg, proctex_noise_lut_offset + 1);
    vec4 c = texelFetch(lut_rgba, proctex_lut_offset + 2);
    o_color = vec4(a.x, b.y, c.z, c.w);
}
