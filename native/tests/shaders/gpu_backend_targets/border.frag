#version 450
// A 16x16 target sampling a 1x1 texture over uv * 3 - 1: the pixels whose coordinate is outside [0, 1] return
// the sampler's border colour (nearest filtering, so nothing blends with it).
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler smp;
layout(location = 0) out vec4 o;
void main() { o = texture(sampler2D(tex, smp), gl_FragCoord.xy / 16.0 * 3.0 - 1.0); }
