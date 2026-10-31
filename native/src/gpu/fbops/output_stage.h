// The output-stage functions of the fragment pipeline as GLSL text (N2-13), for N2-08's generator to embed.
#pragma once

#include <string_view>

namespace mh4n::gpu::fbops {

// The text of output_stage.glsl, exactly as the file has it (the build embeds it: embed_glsl.cmake), to put into a
// fragment shader after its #version line (GLSL 450). It declares four functions (two of them helpers) and nothing else:
//
//   vec4 mh4n_logic_op(uint op, vec4 src, vec4 dst)
//       All 16 logic ops on the 8-bit rounded values; `op` is Azahar's FramebufferRegs::LogicOp value. `dst` is read
//       only by the eleven ops that need it (state::LogicOpNeedsDestination); NoOp returns `src` and the pipeline
//       discards it with the colour write mask 0 (state::PlanPipeline).
//   vec4 mh4n_minmax_blend(uint eq_rgb, uint src_rgb, uint dst_rgb, uint eq_a, uint src_a, uint dst_a,
//                          vec4 src, vec4 dst, vec4 blend_color)
//       The blend equation of the halves the pipeline leaves to the shader: the Min or Max of source * factor and
//       destination * factor per half (Azahar's BlendEquation Min 3 and Max 4 and BlendFactor values), or for the
//       colour half whose plan moved it (rgb_in_shader) the sum or a difference (Add 0, Subtract 1, ReverseSubtract 2).
//       Pass the requested equation for a half the plan moved into the shader (PipelinePlan::rgb_in_shader,
//       alpha_in_shader) and 0xFFFFFFFFu for the others: such a half is returned as the source.
//   vec4 mh4n_blend_factor(uint factor, vec4 src, vec4 dst, vec4 blend_color)
//   float mh4n_blend_half(uint eq, float a, float b)
//       The helpers of the second; the lanes of a factor are (r, g, b, a).
//
// The generator calls them with literals in the specialised mode and with config fields in the ubershader; the
// destination colour is the feedback copy of the target (Recorder::FeedbackCopy), read with texelFetch.
std::string_view OutputStageGlsl();

}  // namespace mh4n::gpu::fbops
