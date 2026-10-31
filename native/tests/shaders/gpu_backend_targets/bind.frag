#version 450
// The per-draw bindings: red is the bound texture's red texel, green is the uniform value read from the ring
// at this draw's offset (the draw writes what it read).
layout(set = 0, binding = 0, std140) uniform Block { uvec4 v; } u;
layout(set = 0, binding = 1) uniform texture2D tex;
layout(set = 0, binding = 2) uniform sampler smp;
layout(location = 0) out vec4 o;
void main() { o = vec4(texelFetch(sampler2D(tex, smp), ivec2(0, 0), 0).r, float(u.v.x) / 255.0, 0.0, 1.0); }
