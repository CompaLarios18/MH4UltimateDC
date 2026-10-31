#version 450
// The shadow write: a pass with no attachments storing into the R32_UINT view bound at set 2, binding 0.
layout(set = 2, binding = 0, r32ui) uniform uimage2D shadow_out;
layout(location = 0) in vec4 v_color;
void main() {
    imageStore(shadow_out, ivec2(gl_FragCoord.xy), uvec4(7u));
}
