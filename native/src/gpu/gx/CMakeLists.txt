# mh4n_gpu_gx: the GX engine on the CPU (N2-03): memory fill, display transfer, texture copy and DMA,
# as Azahar's software blitter does them, and the `cpu-gx` Gpu for the capture player.
mh4n_module(
    SOURCES
        blitter.cpp
        engine.cpp
)
# The headers are included as "gpu/gx/<name>.h".
target_include_directories(mh4n_gpu_gx PUBLIC "${MH4N_SRC_DIR}")
