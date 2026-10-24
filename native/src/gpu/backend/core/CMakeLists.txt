# mh4n_gpu_backend_core (N2-26): the core of the backend behind native/src/gpu/backend/rhi.h and
# vulkan_access.h (both written here and frozen at merge): the instance and the device chosen by
# capability, the capability structure and its mask, buffers and rings, binding sets, pipelines, the
# recorder with a pass that has no attachments, one submit path behind the submit lock with the owner-rule
# checks, buffer readback and the Vulkan handles the app needs. Images, formats and per-draw bindings are
# targets/ (N2-27), waits, the pipeline cache and readback of images sync/ and readback/ (N2-28), dynamic
# blend and stencil state state/ (N2-13): each keeps its per-backend state in a slot of Backend::Impl
# (impl.h) and never edits this directory.
#
# MH4N_RHI_CHECKS (ON by default in every build type, RelWithDebInfo included; OFF only for a shipping
# build) compiles the backend's owner-rule checks. It is exported as the public compile definition
# MH4N_RHI_CHECKS=1 or 0, so the modules that link this one put their checks under the same switch.
option(MH4N_RHI_CHECKS "Compile the backend's owner-rule checks (off only for a shipping build)" ON)

mh4n_module(
    SOURCES
        access.cpp
        buffers.cpp
        device.cpp
        instance.cpp
        pipelines.cpp
        policy.cpp
        recorder.cpp
        submit.cpp
        to_plume.cpp
    DEPENDS
        mh4n_gpu_backend_plume
)
# The headers are included as "gpu/backend/core/<name>.h", rhi.h as "gpu/backend/rhi.h".
target_include_directories(mh4n_gpu_backend_core PUBLIC "${MH4N_SRC_DIR}")
target_compile_definitions(mh4n_gpu_backend_core PUBLIC MH4N_RHI_CHECKS=$<BOOL:${MH4N_RHI_CHECKS}>)
