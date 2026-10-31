# mh4n_spike_1b (N2-35): the spike that settles option 1b against option 1 of D5 on one GPU.
#
# A spike, not a product tool: no module under native/src/, no ctest, and nothing links it. native/CMakeLists.txt adds
# every native/tools/*/CMakeLists.txt to every build, so this file declares its own option and returns at once
# when it is off (the default): the CI jobs and every worker's build compile nothing of it, and not even a
# target exists. The PC configures with -DMH4N_SPIKE_1B=ON.
#
# It calls Vulkan through volk directly (native/third_party/rhi/volk, N2-25), not through rhi.h, and compiles
# GLSL in-process with N2-06's glslang library (mh4n_glslang, added by native/src/gpu/shader/CMakeLists.txt,
# which the top-level list adds before the tools). volk.c is compiled here as a plain C source of this one
# executable: it does not define VOLK_IMPLEMENTATION, and the plume module (the one translation unit that does)
# is not linked.
option(MH4N_SPIKE_1B "Build the option-1b spike (N2-35): a standalone Vulkan tool, run on the PC" OFF)
if (NOT MH4N_SPIKE_1B)
    return()
endif()

set(MH4N_SPIKE_VOLK "${MH4N_ROOT}/third_party/rhi/volk")

add_executable(mh4n_spike_1b main.cpp key.cpp shaders.cpp gpu.cpp "${MH4N_SPIKE_VOLK}/volk.c")
# Third-party source: no warnings of ours (the option comes after the target's, so it wins).
set_source_files_properties("${MH4N_SPIKE_VOLK}/volk.c" PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(mh4n_spike_1b SYSTEM PRIVATE
    "${MH4N_SPIKE_VOLK}"
    "${MH4N_ROOT}/third_party/vulkan/include")
target_link_libraries(mh4n_spike_1b PRIVATE mh4n_warnings mh4n_glslang Threads::Threads ${CMAKE_DL_LIBS})
