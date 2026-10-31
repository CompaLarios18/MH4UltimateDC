// The output-stage functions of the fragment pipeline as GLSL text (N2-13), for N2-08's generator to embed.
#pragma once

#include <string_view>

namespace mh4n::gpu::fbops {

// The text of output_stage.glsl, exactly as the file has it (the build embeds it: embed_glsl.cmake), to put into a
// fragment shader after its #version line (GLSL 450). It declares five functions (two of them helpers) and nothing else:
//
//   vec4 mh4n_logic_op(uint op, vec4 src, vec4 dst)
//       All 16 logic ops on the 8-bit rounded values; `op` is Azahar's FramebufferRegs::LogicOp value. `dst` is read
//       only by the eleven ops that need it (state::LogicOpNeedsDestination); NoOp returns `src` and the pipeline
//       discards it with the colour write mask 0 (state::PlanPipeline).
//   vec4 mh4n_minmax_blend(uint eq_rgb, uint src_rgb, uint dst_rgb, uint eq_a, uint src_a, uint dst_a,
//                          vec4 src, vec4 dst, vec4 blend_color)
//       Per half, the Min (3) or Max (4) of source * factor and destination * factor, with Azahar's BlendEquation and
//       BlendFactor values; ANY other equation leaves that half of `src` as it is (so equation 0 with factors 0 gives
//       the source back). Pass the requested equation for a half the plan moved into the shader
//       (PipelinePlan::rgb_in_shader, alpha_in_shader) and 0 for the others.
//   vec3 mh4n_blend_rgb(uint eq, uint src_factor, uint dst_factor, vec4 src, vec4 dst, vec4 blend_color)
//       The whole colour half (Add, Subtract, ReverseSubtract, Min, Max; clamped to 0..1), for the colour half the plan
//       sends to the shader although it is not a Min or Max half (PipelinePlan::rgb_uses_blend_rgb). Pass the
//       ORIGINAL `src`, not mh4n_minmax_blend's result, whose alpha is the alpha half already replaced:
//           vec4 color = mh4n_minmax_blend(...); color.rgb = mh4n_blend_rgb(eq_rgb, src_rgb, dst_rgb, src, dst, bc);
//   vec4 mh4n_blend_factor(uint factor, vec4 src, vec4 dst, vec4 blend_color)
//   float mh4n_blend_half(uint eq, float a, float b)
//       The helpers of the two; the lanes of a factor are (r, g, b, a).
//
// The generator calls them with literals in the specialised mode and with config fields in the ubershader; the
// destination colour is the feedback copy of the target (Recorder::FeedbackCopy), read with texelFetch.
std::string_view OutputStageGlsl();

}  // namespace mh4n::gpu::fbops
