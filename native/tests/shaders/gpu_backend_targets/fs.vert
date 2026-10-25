#version 450
// One full-screen triangle from the vertex index; Z is the depth every vertex has (a draw's depth).
#ifndef Z
#define Z 0.0
#endif
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, Z, 1.0);
}
