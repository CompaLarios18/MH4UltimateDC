#version 450
// A vertex-stage pipeline in a pass with no attachments: writes data[gl_VertexIndex] = index * scale + bias
// into a storage buffer (set 0, binding 0); scale and bias come from a uniform buffer (set 0, binding 1).
layout(std430, set = 0, binding = 0) buffer Out { uint data[]; } out_buf;
layout(std140, set = 0, binding = 1) uniform U { uint scale; uint bias; } u;

void main() {
    out_buf.data[gl_VertexIndex] = uint(gl_VertexIndex) * u.scale + u.bias;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);  // every triangle is degenerate: nothing is rasterised
}
