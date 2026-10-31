#version 450
#extension GL_GOOGLE_include_directive : require
#include "fs_block.glsl"
// const_color[0] from the block, and the low byte of the first word of cfg (the framebuffer part's raw word) as the alpha:
// proves the member offsets and that the key itself follows the uniforms.
layout(location = 0) out vec4 o_color;
void main() {
    o_color = vec4(const_color[0].rgb, float(cfg[0].x & 0xFFu) / 255.0);
}
