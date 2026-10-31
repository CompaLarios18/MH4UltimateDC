#version 450
// The test's stand-in for the fixed fix-up geometry shader: triangles in, a triangle strip out, the positions passed through.
// It also writes the colour (green) so that a draw that went through the stage can be told from one that did not.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 in_color[];
layout(location = 0) out vec4 v_color;
void main() {
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        v_color = vec4(0.0, 1.0, 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
