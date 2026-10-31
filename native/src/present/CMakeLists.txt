# mh4n_present: the compositor (N2b-05). Draws one present::DisplayFrame into a backend image with a
# screen_layout::Layout: the top and bottom screens in their rectangles (rotated upright, the stored
# region only), the bottom screen's opacity and crop, the further views, nearest or linear filtering,
# CPU-pixel screens uploaded, and the app's UiLayer called last. It goes through the backend's rhi.h
# only (N2-26 to N2-28); its shaders are GLSL compiled at start-up with N2-06's glslang.
#
# The swapchain and the present thread (N2b-06) and the frame sources (N2b-07, N2b-13) are other modules.
#
# screen_layout.h includes the port's platform.h: a copy of native/ on its own (no port/ around it) gets
# an empty module, as mh4n_screen_layout does (its test skips itself).
set(MH4N_PRESENT_PLATFORM_HEADER "${MH4N_ROOT}/../port/src/platform.h")

if (EXISTS "${MH4N_PRESENT_PLATFORM_HEADER}")
    set(mh4n_present_sources compositor.cpp shader_compile.cpp)
    set(mh4n_present_available ON)
else()
    message(STATUS "mh4n_present: port/src/platform.h not found around native/: an empty module")
    set(mh4n_present_sources unavailable.cpp)
    set(mh4n_present_available OFF)
endif()

mh4n_module(
    SOURCES
        ${mh4n_present_sources}
    DEPENDS
        mh4n_gpu_backend_core
        mh4n_gpu_backend_targets
        mh4n_gpu_backend_sync
        mh4n_gpu_backend_readback
        mh4n_gpu_shader
        mh4n_screen_layout
    THIRD_PARTY
        mh4n_fmt
)
# The headers are included as "present/<name>.h".
target_include_directories(mh4n_present PUBLIC "${MH4N_SRC_DIR}")
