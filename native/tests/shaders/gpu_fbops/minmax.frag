#version 450
#extension GL_GOOGLE_include_directive : require
// mh4n_minmax_blend of fbops/output_stage.glsl (the file the product embeds) on a constant source colour (R, G, B, A),
// the destination under the pixel (the feedback copy of the target) and a blend colour (BR, BG, BB, BA).
#include "output_stage.glsl"
layout(constant_id = 1) const float R = 0.0;
layout(constant_id = 2) const float G = 0.0;
layout(constant_id = 3) const float B = 0.0;
layout(constant_id = 4) const float A = 0.0;
layout(constant_id = 7) const uint EQ_RGB = 3u;
layout(constant_id = 8) const uint SRC_RGB = 1u;
layout(constant_id = 9) const uint DST_RGB = 1u;
layout(constant_id = 10) const uint EQ_A = 3u;
layout(constant_id = 11) const uint SRC_A = 1u;
layout(constant_id = 12) const uint DST_A = 1u;
layout(constant_id = 13) const float BR = 0.0;
layout(constant_id = 14) const float BG = 0.0;
layout(constant_id = 15) const float BB = 0.0;
layout(constant_id = 16) const float BA = 0.0;
layout(set = 0, binding = 0) uniform texture2D dst_tex;
layout(set = 0, binding = 1) uniform sampler dst_smp;
layout(location = 0) out vec4 o;
void main() {
    vec4 dst = texelFetch(sampler2D(dst_tex, dst_smp), ivec2(gl_FragCoord.xy), 0);
    o = mh4n_minmax_blend(EQ_RGB, SRC_RGB, DST_RGB, EQ_A, SRC_A, DST_A, vec4(R, G, B, A), dst, vec4(BR, BG, BB, BA));
}
