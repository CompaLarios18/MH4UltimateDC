#version 450
#extension GL_EXT_samplerless_texture_functions : require
// The destination copy: tex_color (set 1, binding 8) read by texel, plus a green offset.
layout(set = 1, binding = 8) uniform texture2D tex_color;
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 dst = texelFetch(tex_color, ivec2(gl_FragCoord.xy), 0);
    o_color = dst + vec4(0.0, 0.4, 0.0, 0.0);
}
