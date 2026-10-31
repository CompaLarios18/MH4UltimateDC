#version 450
// Derived from Azahar src/video_core/shader/generator/glsl_shader_gen.cpp (GenerateTrivialVertexShader,
// VSUniformBlockDef, GetVertexInterfaceDeclaration; "Licensed under GPLv2 or any later version"),
// shader_gen.h (the Attributes enumeration; GPLv2 or any later version) and glsl_fs_shader_gen.cpp
// (FragmentModule::DefineInterface, the locations of the varyings; "Licensed under GPLv3 or any later
// version") at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6. The licence of this file is GPL-3.0-or-later
// (glsl_fs_shader_gen.cpp says GPLv3 or any later version).
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_shader_gen.cpp,
//   shader_gen.h, glsl_fs_shader_gen.cpp)
//
// The trivial vertex stage (N2-37): it passes the CPU-shaded vertices of a streamed batch to the
// fragment stage. Inputs at locations 0 to 7 as Azahar's Attributes enumeration (position, color,
// texcoord0, texcoord1, texcoord2, texcoord0_w, normquat, view); outputs at locations 1 to 7 as
// DefineInterface (the same as the translated vertex stage of N2-06). Compile with -DUSE_CLIP_PLANES
// for the variant that writes gl_ClipDistance[2] (Azahar's `use_clip_planes`).
//
// Deviations from Azahar's generated text: the two bools of the uniform block are `uint` (std140
// makes the block the same 32 bytes; N2-06's VertexUniformData), and the block is always the
// Vulkan one (set 0, binding 1).

layout(location = 0) in vec4 vert_position;
layout(location = 1) in vec4 vert_color;
layout(location = 2) in vec2 vert_texcoord0;
layout(location = 3) in vec2 vert_texcoord1;
layout(location = 4) in vec2 vert_texcoord2;
layout(location = 5) in float vert_texcoord0_w;
layout(location = 6) in vec4 vert_normquat;
layout(location = 7) in vec3 vert_view;

layout(location = 1) out vec4 primary_color;
layout(location = 2) out vec2 texcoord0;
layout(location = 3) out vec2 texcoord1;
layout(location = 4) out vec2 texcoord2;
layout(location = 5) out float texcoord0_w;
layout(location = 6) out vec4 normquat;
layout(location = 7) out vec3 view;

#ifdef USE_CLIP_PLANES
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[2];
};
#endif

layout(set = 0, binding = 1, std140) uniform vs_data {
    uint enable_clip1;
    uint flip_viewport;
    vec4 clip_coef;
};

const vec2 EPSILON_Z = vec2(0.000001f, -1.00001f);

vec4 SanitizeVertex(vec4 vtx_pos) {
    float ndc_z = vtx_pos.z / vtx_pos.w;
    if (ndc_z > 0.f && ndc_z < EPSILON_Z[0]) {
        vtx_pos.z = 0.f;
    }
    if (ndc_z < -1.f && ndc_z > EPSILON_Z[1]) {
        vtx_pos.z = -vtx_pos.w;
    }
    return vtx_pos;
}

void main() {
    primary_color = vert_color;
    texcoord0 = vert_texcoord0;
    texcoord1 = vert_texcoord1;
    texcoord2 = vert_texcoord2;
    texcoord0_w = vert_texcoord0_w;
    normquat = vert_normquat;
    view = vert_view;
    vec4 vtx_pos = SanitizeVertex(vert_position);
    if (flip_viewport != 0u) {
        vtx_pos.y = -vtx_pos.y;
    }
    gl_Position = vec4(vtx_pos.x, vtx_pos.y, -vtx_pos.z, vtx_pos.w);
#ifdef USE_CLIP_PLANES
    gl_ClipDistance[0] = -vtx_pos.z; // fixed PICA clipping plane z <= 0
    if (enable_clip1 != 0u) {
        gl_ClipDistance[1] = dot(clip_coef, vtx_pos);
    } else {
        gl_ClipDistance[1] = 0.0;
    }
#endif
}
