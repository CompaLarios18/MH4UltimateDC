# mh4n_gpu_null: the null GPU (N1-17), registered as `null` (the runtime's default GPU). It completes
# every GX command at once, signals the interrupts Azahar's GPU signals for it, does the memory
# fills and DMA copies and draws nothing. The blitter's memory fill and the speed table of Azahar's
# DelayGenerator come from mh4n_gpu_gx (N2-03).
mh4n_module(
    SOURCES
        null_gpu.cpp
    DEPENDS
        mh4n_gpu_gx
)
# The header is included as "gpu_null/null_gpu.h".
target_include_directories(mh4n_gpu_null PUBLIC "${MH4N_SRC_DIR}")
