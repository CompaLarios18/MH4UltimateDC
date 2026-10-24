#version 450
// Writes data[gl_VertexIndex] = a.x + 256 * a.y + 1000000 * b.x from two vertex attributes read from two
// bindings (the same buffer at two offsets in the test).
layout(location = 0) in uvec4 a;
layout(location = 1) in uvec4 b;
layout(std430, set = 0, binding = 0) buffer Out { uint data[]; } out_buf;

void main() {
    out_buf.data[gl_VertexIndex] = a.x + 256u * a.y + 1000000u * b.x;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
