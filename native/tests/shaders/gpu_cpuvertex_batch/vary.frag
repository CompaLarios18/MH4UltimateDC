#version 450
// Writes one of the seven varyings of the trivial vertex stage (locations 1 to 7, as the translated
// vertex stage and the fragment generator declare them) to the colour output; SHOW is the expression.
layout(location = 1) in vec4 primary_color;
layout(location = 2) in vec2 texcoord0;
layout(location = 3) in vec2 texcoord1;
layout(location = 4) in vec2 texcoord2;
layout(location = 5) in float texcoord0_w;
layout(location = 6) in vec4 normquat;
layout(location = 7) in vec3 view;
layout(location = 0) out vec4 o;
void main() { o = SHOW; }
