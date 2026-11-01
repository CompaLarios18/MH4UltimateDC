# mh4n_swapchain: the swapchain and the present thread (N2b-06, docs/native/presentation.md). A present::ScreenSink
# that keeps the newest published frame in a one-slot mailbox and, on a thread of its own, composes it
# (mh4n_present's Compositor) into a swapchain image (or an offscreen target), presents it, and logs when each frame
# was shown. It goes through the backend's rhi.h and vulkan_access.h only (the handles, QueueLock,
# WrapExternalImage); it includes nothing from native/src/gpu/backend/ besides those two headers.
#
# screen_layout.h includes the port's platform.h, as mh4n_present does: a copy of native/ on its own (no port/ around
# it) gets an empty module (the test skips itself).
set(MH4N_SWAPCHAIN_PLATFORM_HEADER "${MH4N_ROOT}/../port/src/platform.h")

if (EXISTS "${MH4N_SWAPCHAIN_PLATFORM_HEADER}")
    set(mh4n_swapchain_sources policy.cpp present_log.cpp presenter.cpp vk_swapchain.cpp)
else()
    message(STATUS "mh4n_swapchain: port/src/platform.h not found around native/: an empty module")
    set(mh4n_swapchain_sources unavailable.cpp)
endif()

mh4n_module(
    SOURCES
        ${mh4n_swapchain_sources}
    DEPENDS
        mh4n_gpu_backend_core
        mh4n_gpu_backend_targets
        mh4n_gpu_backend_sync
        mh4n_gpu_backend_readback
        mh4n_present
        mh4n_screen_layout
    THIRD_PARTY
        mh4n_fmt
)
# The headers are included as "swapchain/<name>.h".
target_include_directories(mh4n_swapchain PUBLIC "${MH4N_SRC_DIR}")
