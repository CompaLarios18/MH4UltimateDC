#version 450
// A full-screen triangle with no vertex input; the colour goes to the fragment stage (location 0).
layout(location = 0) out vec4 v_color;
#ifndef COLOR
#define COLOR vec4(1.0, 0.0, 0.0, 1.0)
#endif
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_color = COLOR;
}
