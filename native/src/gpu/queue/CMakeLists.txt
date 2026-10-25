# mh4n_gpu_queue (N2-22): the renderer thread. The GPU wrapper `queue` runs another gpu::Gpu on a thread
# of its own: Execute, WriteReg, SetBufferSwap, SetColorFill, FlushRegion, InvalidateRegion and VBlank
# are queued and return at once, interrupts reach the host in the order the commands complete, writes into
# guest memory go through Host::Post (QueueHost keeps the list InvalidateRegion runs), at most one frame is
# in flight beyond the one being built, and with MH4N_RHI_CHECKS every call forwarded to the wrapped GPU is
# asserted to run on the renderer thread (the backend's owner-rule handler, OwnerRule::WrongThread).
# Registered as the wrapper `queue` with the capture player's option `--queue` (queue_wrapper.cpp).
mh4n_module(
    SOURCES
        queue_gpu.cpp
        queue_wrapper.cpp
    DEPENDS
        mh4n_gpu_backend_core
)
# The header is included as "gpu/queue/queue_gpu.h".
target_include_directories(mh4n_gpu_queue PUBLIC "${MH4N_SRC_DIR}")
