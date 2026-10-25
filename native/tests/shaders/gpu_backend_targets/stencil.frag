#version 450
#extension GL_EXT_samplerless_texture_functions : require
// The stencil plane of a depth-stencil image (its Stencil view), under the pixel, as red / 255.
layout(set = 0, binding = 0) uniform utexture2D st;
layout(location = 0) out vec4 o;
void main() { o = vec4(float(texelFetch(st, ivec2(gl_FragCoord.xy), 0).x) / 255.0, 0.0, 0.0, 1.0); }
