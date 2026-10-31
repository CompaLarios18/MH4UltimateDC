// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later, as the file says: "Licensed under GPLv3 or any later version"): the depth of a fragment,
// z_over_w * depth_scale + depth_offset with z_over_w = -gl_FragCoord.z (FragmentModule::WriteDepth). The rest is ours.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp)
#version 450
// One constant colour (R, G, B, A) and the PICA's depth: z_over_w * depth_scale + depth_offset, the line Azahar's
// fragment generator writes to gl_FragDepth in every draw (WriteDepth), with z_over_w = -gl_FragCoord.z.
layout(constant_id = 1) const float R = 0.0;
layout(constant_id = 2) const float G = 0.0;
layout(constant_id = 3) const float B = 0.0;
layout(constant_id = 4) const float A = 0.0;
layout(constant_id = 5) const float DEPTH_SCALE = 1.0;
layout(constant_id = 6) const float DEPTH_OFFSET = 0.0;
layout(location = 0) out vec4 o;
void main() {
    o = vec4(R, G, B, A);
    gl_FragDepth = (-gl_FragCoord.z) * DEPTH_SCALE + DEPTH_OFFSET;
}
