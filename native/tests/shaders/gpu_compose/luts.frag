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
