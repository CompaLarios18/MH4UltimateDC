#version 450
layout(set=0, binding=0) uniform texture2D tex;
layout(set=0, binding=1) uniform sampler smp;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){ o = texelFetch(sampler2D(tex, smp), ivec2(gl_FragCoord.xy), 0); }
