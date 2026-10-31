// Derived from Azahar src/video_core/shader/generator/shader_uniforms.h (FSUniformData and LightSrc: the members and their
// order) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: the file says "Licensed under GPLv2 or any later
// version").
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (shader_uniforms.h)
//
// The std140 declaration of the fragment uniform block (set 0, binding 2) that native/src/gpu/fragment/uniforms.h
// documents: FragmentUniforms, then the key itself (cfg, 70 words in 72) and the reserved gas region. Included by the
// hand-written test shaders of gpu_compose_test.
struct LightSrc {
    vec3 specular_0;
    vec3 specular_1;
    vec3 diffuse;
    vec3 ambient;
    vec3 position;
    vec3 spot_direction;
    float dist_atten_bias;
    float dist_atten_scale;
};

layout(std140, set = 0, binding = 2) uniform FSBlock {
    int framebuffer_scale;
    int alphatest_ref;
    float depth_scale;
    float depth_offset;
    float shadow_bias_constant;
    float shadow_bias_linear;
    int scissor_x1;
    int scissor_y1;
    int scissor_x2;
    int scissor_y2;
    int fog_lut_offset;
    int proctex_noise_lut_offset;
    int proctex_color_map_offset;
    int proctex_alpha_map_offset;
    int proctex_lut_offset;
    int proctex_diff_lut_offset;
    float proctex_bias;
    int shadow_texture_bias;
    ivec4 lighting_lut_offset[6];
    vec3 fog_color;
    vec2 proctex_noise_f;
    vec2 proctex_noise_a;
    vec2 proctex_noise_p;
    vec3 lighting_global_ambient;
    LightSrc light_src[8];
    vec4 const_color[6];
    vec4 tev_combiner_buffer_color;
    vec3 tex_lod_bias;
    vec4 tex_border_color[3];
    vec4 blend_color;
    uvec4 cfg[18];
    uvec4 gas_regs[4];
    uvec4 gas_table[4];
};
