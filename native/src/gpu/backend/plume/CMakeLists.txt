# mh4n_gpu_backend_plume (N2-25): plume's Vulkan backend, patched (native/third_party/rhi/plume/patched), with
# volk and VulkanMemoryAllocator (patched too: native/third_party/rhi/vma/patched), as the static library
# mh4n_rhi_plume. The wrapper above it (N2-26 to N2-28) and the tests link it through this module.
#
# plume's own CMakeLists.txt picks its backends by platform, so the Vulkan backend's files are listed here by
# hand. plume_vulkan.cpp is the one translation unit that defines VOLK_IMPLEMENTATION and VMA_IMPLEMENTATION
# (it includes volk.c); nothing else in native/ may. PLUME_EXTERNAL_SURFACE_ONLY (patch 08) compiles plume's
# window-surface code and window types out, so no build needs X11, Win32 window or Cocoa headers: surfaces
# come from the caller as a VkSurfaceKHR. The Vulkan headers are the tree's one copy, native/third_party/vulkan
# (no VK_USE_PLATFORM_* is defined). Nothing here runs at static initialisation, and nothing loads Vulkan until
# an instance is created (volkInitialize).
set(MH4N_RHI_DIR "${MH4N_ROOT}/third_party/rhi")

add_library(mh4n_rhi_plume STATIC "${MH4N_RHI_DIR}/plume/patched/plume_vulkan.cpp")
set_target_properties(mh4n_rhi_plume PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(mh4n_rhi_plume SYSTEM PUBLIC
    "${MH4N_RHI_DIR}/plume/patched"
    "${MH4N_RHI_DIR}/volk"
    "${MH4N_RHI_DIR}/vma/patched"
    "${MH4N_ROOT}/third_party/vulkan/include")
target_compile_definitions(mh4n_rhi_plume PUBLIC PLUME_EXTERNAL_SURFACE_ONLY)
target_compile_definitions(mh4n_rhi_plume PRIVATE $<$<BOOL:${WIN32}>:NOMINMAX>)
# Third-party code: no warnings of ours (mh4n_warnings is not linked).
target_compile_options(mh4n_rhi_plume PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)
target_link_libraries(mh4n_rhi_plume PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

mh4n_module(
    SOURCES
        vendor_info.cpp
    THIRD_PARTY
        mh4n_rhi_plume
)
# The headers are included as "gpu/backend/plume/<name>.h".
target_include_directories(mh4n_gpu_backend_plume PUBLIC "${MH4N_SRC_DIR}")
