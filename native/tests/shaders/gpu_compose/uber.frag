#version 450
// The test's ubershader fallback: blue.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = vec4(0.0, 0.0, 1.0, 1.0);
}
