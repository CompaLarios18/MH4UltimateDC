# mh4n_gpu_backend_sync (N2-28): what the renderer thread, the compositor and the capture player need from the
# backend beyond drawing, behind native/src/gpu/backend/rhi.h: Submit with waits (each wait with its own stage;
# without the `timeline` bit the submitter first waits on the CPU for a value that has not completed, through
# core's fence fallback), Retire (deferred destruction on the completed value), the pipeline-cache blob with its
# header check, and the two GPU timestamps per frame (Recorder::MarkFrame, Backend::FrameGpuTimeNs).
# It keeps its per-backend state in the slot core/impl.h reserves (Backend::Impl::sync) and never edits core/.
mh4n_module(
    SOURCES
        pipeline_cache.cpp
        submit.cpp
        timestamps.cpp
    DEPENDS
        mh4n_gpu_backend_core
)
