#version 450
// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (FragmentModule::DefineInterface) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version")
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
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
