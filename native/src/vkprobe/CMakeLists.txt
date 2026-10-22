# mh4n_vkprobe (N2-24): reports which Vulkan features MH4U's rendering needs a device offers, from
# a live device or from `vulkaninfo --json`. The program is native/tools/vkprobe (only main); the
# manual is docs/research/vkprobe.md.
#
# The live probe loads the Vulkan loader at run time and takes its declarations from the vendored
# vulkan_core.h (native/third_party/vulkan, VK_NO_PROTOTYPES): nothing links against Vulkan, so
# every job builds it with no Vulkan package. The reader and the report need no Vulkan at all.
mh4n_module(
    SOURCES
        input.cpp
        json.cpp
        live.cpp
        report.cpp
        tool.cpp
    THIRD_PARTY
        mh4n_fmt
        ${CMAKE_DL_LIBS}
)
# The headers are included as "vkprobe/<name>.h".
target_include_directories(mh4n_vkprobe PUBLIC "${MH4N_SRC_DIR}")
target_include_directories(mh4n_vkprobe SYSTEM PRIVATE "${MH4N_ROOT}/third_party/vulkan/include")
target_compile_definitions(mh4n_vkprobe PRIVATE VK_NO_PROTOTYPES)
