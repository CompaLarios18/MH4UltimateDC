#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Unit 0 as 2D (binding 0, sampler 3), unit 1 (binding 4, sampler 5), unit 0's cube and shadow classes (bindings 1 and 2) and
// unit 2 (binding 6): the red channel of each, and the shadow class by texel fetch of the R32_UINT view.
layout(set = 1, binding = 0) uniform texture2D tex0;
layout(set = 1, binding = 1) uniform textureCube tex0_cube;
layout(set = 1, binding = 2) uniform utexture2D tex0_shadow;
layout(set = 1, binding = 3) uniform sampler samp0;
layout(set = 1, binding = 4) uniform texture2D tex1;
layout(set = 1, binding = 5) uniform sampler samp1;
layout(set = 1, binding = 6) uniform texture2D tex2;
layout(set = 1, binding = 7) uniform sampler samp2;
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() {
    float a = texture(sampler2D(tex0, samp0), vec2(0.5)).r;
    float b = texture(sampler2D(tex1, samp1), vec2(0.5)).r;
    float c = texture(sampler2D(tex2, samp2), vec2(0.5)).r;
    float cube = texture(samplerCube(tex0_cube, samp0), vec3(0.0, 0.0, 1.0)).r;
    uint s = texelFetch(tex0_shadow, ivec2(0), 0).r;
    // a: unit 0 (2D class), b: unit 1, c: unit 2; the cube and shadow classes only prove that their dummies are bound
    o_color = vec4(a, b, c, (cube > 0.5 && s != 0u) ? 1.0 : 0.0);
}
