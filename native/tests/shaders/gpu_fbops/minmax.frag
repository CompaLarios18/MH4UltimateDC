#version 450
#extension GL_GOOGLE_include_directive : require
// The blend functions of fbops/output_stage.glsl (the file the product embeds) composed the way a generator does, on a
// constant source colour (R, G, B, A), the destination under the pixel (the feedback copy of the target) and a blend
// colour (BR, BG, BB, BA): mh4n_minmax_blend for the Min and Max halves (EQ_RGB and EQ_A are the equations of the
// halves the plan moved, 0 for the others), then, when USE_BLEND_RGB is set, mh4n_blend_rgb for the colour half from the
// ORIGINAL source colour.
#include "output_stage.glsl"
layout(constant_id = 1) const float R = 0.0;
layout(constant_id = 2) const float G = 0.0;
layout(constant_id = 3) const float B = 0.0;
layout(constant_id = 4) const float A = 0.0;
layout(constant_id = 7) const uint EQ_RGB = 0u;
layout(constant_id = 8) const uint SRC_RGB = 0u;
layout(constant_id = 9) const uint DST_RGB = 0u;
layout(constant_id = 10) const uint EQ_A = 0u;
layout(constant_id = 11) const uint SRC_A = 0u;
layout(constant_id = 12) const uint DST_A = 0u;
layout(constant_id = 13) const float BR = 0.0;
layout(constant_id = 14) const float BG = 0.0;
layout(constant_id = 15) const float BB = 0.0;
layout(constant_id = 16) const float BA = 0.0;
layout(constant_id = 17) const uint BLEND_RGB_EQ = 0u;
layout(constant_id = 18) const uint USE_BLEND_RGB = 0u;
layout(set = 0, binding = 0) uniform texture2D dst_tex;
layout(set = 0, binding = 1) uniform sampler dst_smp;
layout(location = 0) out vec4 o;
void main() {
    vec4 src = vec4(R, G, B, A);
    vec4 dst = texelFetch(sampler2D(dst_tex, dst_smp), ivec2(gl_FragCoord.xy), 0);
    vec4 blend_color = vec4(BR, BG, BB, BA);
    o = mh4n_minmax_blend(EQ_RGB, SRC_RGB, DST_RGB, EQ_A, SRC_A, DST_A, src, dst, blend_color);
    if (USE_BLEND_RGB != 0u) {
        o.rgb = mh4n_blend_rgb(BLEND_RGB_EQ, SRC_RGB, DST_RGB, src, dst, blend_color);
    }
}
