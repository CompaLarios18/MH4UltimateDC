#version 450
#extension GL_EXT_samplerless_texture_functions : require
// The depth plane of a depth-stencil image (its Depth view), under the pixel, as green.
layout(set = 0, binding = 0) uniform texture2D dp;
layout(location = 0) out vec4 o;
void main() { o = vec4(0.0, texelFetch(dp, ivec2(gl_FragCoord.xy), 0).x, 0.0, 1.0); }
