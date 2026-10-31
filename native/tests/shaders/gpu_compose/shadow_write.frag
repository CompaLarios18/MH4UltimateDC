// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (DefineBindingsVK: the shadow image at set 2 binding 0, r32ui uimage2D) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under GPLv3 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
//
#version 450
// The shadow write: a pass with no attachments storing into the R32_UINT view bound at set 2, binding 0.
layout(set = 2, binding = 0, r32ui) uniform uimage2D shadow_out;
layout(location = 0) in vec4 v_color;
void main() {
    imageStore(shadow_out, ivec2(gl_FragCoord.xy), uvec4(7u));
}
