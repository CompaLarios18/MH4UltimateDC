# mh4n_gpu_backend_readback (N2-28): render targets into guest memory in the PICA's Morton-tiled layouts and formats
# (Backend::ReadbackToGuest, through compute passes of ours: shaders/readback.comp, its SPIR-V committed as
# readback_shaders.inc by tools/native/spirv_inc.py) and ReadImage, a region of any backend image in the image's own
# format. It records on internal recorders through the targets module's first-use path (Acquire, Normalize), so the
# owner rule and the one layout policy hold, and waits for the result on the CPU.
mh4n_module(
    SOURCES
        image.cpp
        readback.cpp
    DEPENDS
        mh4n_gpu_backend_core
        mh4n_gpu_backend_targets
        mh4n_gpu_backend_sync
)
