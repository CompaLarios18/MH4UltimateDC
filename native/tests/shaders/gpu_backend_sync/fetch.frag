#version 450
// The texel under the pixel of a bound texture (texelFetch: no filtering), plus ADD (feedback: a draw that
// reads the target's previous contents through the twin).
#ifndef ADD
#define ADD vec4(0.0)
#endif
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler smp;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(sampler2D(tex, smp), ivec2(gl_FragCoord.xy), 0) + ADD; }
