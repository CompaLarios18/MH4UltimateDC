// Derived from Azahar src/video_core/shader/generator/glsl_shader_gen.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later, as the file says: "Licensed under GPLv2 or any later version"): the vertex stage outputs the z
// of the PICA negated (gl_Position = vec4(x, y, -z, w)). The rest is ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_shader_gen.cpp)
#version 450
// One full-screen triangle from the vertex index. Z is the PICA's clip z over w of the draw, in [-1, 0]; the vertex
// stage outputs it negated, as Azahar's vertex shaders do (glsl_shader_gen.cpp), so the fragment
// stage's gl_FragCoord.z is -Z.
layout(constant_id = 0) const float Z = 0.0;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, -Z, 1.0);
}
