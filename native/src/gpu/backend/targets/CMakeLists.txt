# mh4n_gpu_backend_targets (N2-27): images and render targets of the backend behind native/src/gpu/backend/rhi.h:
# the format query and chooser, images and their views, samplers, copies, clears, blits and passes with
# attachments, the one hidden layout policy with the owner rule, the feedback copy, the swapchain image's way to
# presentation (WrapExternalImage, PrepareForPresent), and per-draw bindings recycled by timeline value.
# It keeps its per-backend and per-recorder state in the slots core/impl.h reserves and never edits core/.
mh4n_module(
    SOURCES
        bindings.cpp
        formats.cpp
        images.cpp
        layouts.cpp
        ops.cpp
        upload.cpp
    DEPENDS
        mh4n_gpu_backend_core
)
