#version 450
// A uniform texel buffer of RGBA8 texels: pixel x reads texel x.
layout(set = 0, binding = 0) uniform samplerBuffer tb;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(tb, int(gl_FragCoord.x)); }
