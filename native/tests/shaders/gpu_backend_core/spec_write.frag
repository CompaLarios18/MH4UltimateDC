#version 450
// A fragment stage in a pass with no attachments: each of the 8x8 pixels writes data[y * 8 + x] =
// kValue (constant 0) + kExtra (constant 3) * 100; the defaults are 7 and 0.
layout(constant_id = 0) const uint kValue = 7u;
layout(constant_id = 3) const uint kExtra = 0u;
layout(std430, set = 0, binding = 0) buffer Out { uint data[]; } out_buf;

void main() {
    uvec2 p = uvec2(gl_FragCoord.xy);
    out_buf.data[p.y * 8u + p.x] = kValue + kExtra * 100u;
}
