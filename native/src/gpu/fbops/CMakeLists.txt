# mh4n_gpu_fbops (N2-13): the PICA's framebuffer operations for the renderer. MakeFixedFunctionState turns the
# registers (blend equations and factors, colour write mask, logic op, depth, stencil, cull, front face) into
# rhi.h's FixedFunctionState in the form the registers request it, MakeDynamicState the blend constants and the
# stencil reference and masks into the values of the dynamic-state calls, and OutputStageGlsl is the GLSL text of the
# two output-stage functions the hardware cannot do (mh4n_logic_op, mh4n_minmax_blend) for the fragment generator
# to embed. The device profile is applied by backend/state (PlanPipeline) when the pipeline is built; this module
# talks rhi.h types only.
#
# output_stage.glsl is the one source of that text: embed_glsl.cmake turns it into a C++ source at build time, and the
# test shaders of native/tests/shaders/gpu_fbops #include it, so the committed test SPIR-V and the product share it.
set(MH4N_FBOPS_GLSL "${CMAKE_CURRENT_SOURCE_DIR}/output_stage.glsl")
set(MH4N_FBOPS_GLSL_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/output_stage_glsl.cpp")
add_custom_command(
    OUTPUT "${MH4N_FBOPS_GLSL_CPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} "-DINPUT=${MH4N_FBOPS_GLSL}" "-DOUTPUT=${MH4N_FBOPS_GLSL_CPP}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_glsl.cmake"
    DEPENDS "${MH4N_FBOPS_GLSL}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_glsl.cmake"
    COMMENT "Embedding output_stage.glsl"
    VERBATIM)

mh4n_module(
    SOURCES
        fixed_function.cpp
        "${MH4N_FBOPS_GLSL_CPP}"
    DEPENDS
        mh4n_gpu_pica
        mh4n_gpu_backend_state
)
# The headers are included as "gpu/fbops/<name>.h".
target_include_directories(mh4n_gpu_fbops PUBLIC "${MH4N_SRC_DIR}")
