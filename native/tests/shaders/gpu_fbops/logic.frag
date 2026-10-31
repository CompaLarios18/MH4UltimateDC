#version 450
#extension GL_GOOGLE_include_directive : require
// mh4n_logic_op of fbops/output_stage.glsl (the file the product embeds) on a constant source colour (R, G, B, A) and
// the destination under the pixel: the feedback copy of the target, bound as a texture, or with NO_DST nothing (the
// five ops that do not read it).
#include "output_stage.glsl"
layout(constant_id = 1) const float R = 0.0;
layout(constant_id = 2) const float G = 0.0;
layout(constant_id = 3) const float B = 0.0;
layout(constant_id = 4) const float A = 0.0;
layout(constant_id = 7) const uint OP = 3u;
#ifndef NO_DST
layout(set = 0, binding = 0) uniform texture2D dst_tex;
layout(set = 0, binding = 1) uniform sampler dst_smp;
#endif
layout(location = 0) out vec4 o;
void main() {
#ifdef NO_DST
    vec4 dst = vec4(0.0);
#else
    vec4 dst = texelFetch(sampler2D(dst_tex, dst_smp), ivec2(gl_FragCoord.xy), 0);
#endif
    o = mh4n_logic_op(OP, vec4(R, G, B, A), dst);
}
