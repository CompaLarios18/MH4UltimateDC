# mh4n_gpu_capture: the MGPU reader, the capture host, the comparison harness and the two Gpus that
# ship with it (N2-02). Other modules that add a Gpu wrapper, a check or a column list this one in
# DEPENDS and include "gpu_capture/check.h".
mh4n_module(
    SOURCES
        capture_host.cpp
        checks.cpp
        diagnostics.cpp
        gpus.cpp
        image.cpp
        mgpu.cpp
        player.cpp
    # player.cpp includes gpu/pica/init_regs.h (the rule that builds the internal registers from an
    # INIT record, N2-40).
    DEPENDS
        mh4n_gpu_pica
)
# The headers are included as "gpu_capture/<name>.h".
target_include_directories(mh4n_gpu_capture PUBLIC "${MH4N_SRC_DIR}")
