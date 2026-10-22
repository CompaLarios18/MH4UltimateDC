# mh4n_display_mem: the `null-display` GPU (N2b-07). The null GPU (N1-17) wrapped so that each
# VBlank also builds the displayed screens from guest memory, the way Azahar's software renderer
# shows them (gpu_capture::DisplayShadow), and publishes them as CPU pixels (present.h). A tool for
# our own programs, whose pictures the CPU draws, and for the app's plumbing before the renderer.
#
# The top screen's rows come from NativePlatform (mh4n_platform) when the repository around native/
# has the hooks; without them (a copy of native/ on its own) the module has no platform to ask.
set(MH4N_DISPLAY_MEM_ROOT "${MH4N_ROOT}/..")
set(mh4n_display_mem_depends mh4n_gpu_null mh4n_gpu_capture mh4n_runtime)
set(mh4n_display_mem_have_platform OFF)
if (EXISTS "${MH4N_DISPLAY_MEM_ROOT}/port/src/hooks.cpp"
    AND EXISTS "${MH4N_DISPLAY_MEM_ROOT}/port/src/platform.h"
    AND EXISTS "${MH4N_DISPLAY_MEM_ROOT}/port/src/schedule.h"
    AND EXISTS "${MH4N_DISPLAY_MEM_ROOT}/tools/recomp/runtime/recomp.h")
    set(mh4n_display_mem_have_platform ON)
    list(APPEND mh4n_display_mem_depends mh4n_platform)
endif()

mh4n_module(
    SOURCES
        display_mem.cpp
    DEPENDS
        ${mh4n_display_mem_depends}
    THIRD_PARTY
        mh4n_fmt
)
# The header is included as "display_mem/display_mem.h".
target_include_directories(mh4n_display_mem PUBLIC "${MH4N_SRC_DIR}")
if (mh4n_display_mem_have_platform)
    target_compile_definitions(mh4n_display_mem PUBLIC MH4N_DISPLAY_MEM_PLATFORM=1)
endif()
