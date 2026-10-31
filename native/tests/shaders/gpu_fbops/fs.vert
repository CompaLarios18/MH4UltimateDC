#version 450
// One full-screen triangle from the vertex index. Z is the PICA's clip z over w of the draw, in [-1, 0]; the vertex
// stage outputs it negated, as Azahar's vertex shaders do (glsl_fs_shader_gen.cpp WriteDepth), so the fragment
// stage's gl_FragCoord.z is -Z.
layout(constant_id = 0) const float Z = 0.0;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, -Z, 1.0);
}
