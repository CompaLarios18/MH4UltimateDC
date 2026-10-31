# mh4n_gpu_backend_state (N2-13): the framebuffer-operation side of the backend behind native/src/gpu/backend/rhi.h:
# the dynamic-state calls rhi.h declares for it (Recorder::SetBlendConstants, SetStencilReference, SetStencilMasks: the
# blend constants, the stencil reference and the two stencil masks are never baked into a pipeline) and the choice of
# path by capability for the fixed-function state a draw asks for (PlanPipeline: hardware logic ops or the shader's
# emulation of all 16, hardware min/max blending or the shader's, the constant-alpha factors the vendored backend
# cannot express). It keeps no per-backend state, so it uses none of the slots core/impl.h reserves, and it never edits
# core/.
mh4n_module(
    SOURCES
        dynamic.cpp
        plan.cpp
    DEPENDS
        mh4n_gpu_backend_core
)
