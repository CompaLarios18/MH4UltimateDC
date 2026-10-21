# mh4n_services_gsp: the gsp::Gpu and gsp::Lcd services (N1-17): register writes, the GX command
# queue, the interrupt relay queue, the framebuffer information, the GPU right and the VBlank timer,
# in front of mh4n/gpu.h. The `services.ipc.gsp` boot step wires the runtime's interrupt and
# refresh-rate sinks. The null GPU it is used with is mh4n_gpu_null.
mh4n_module(
    SOURCES
        gsp_gpu.cpp
        gsp_lcd.cpp
        state.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
        mh4n_memory
        mh4n_runtime
)
target_include_directories(mh4n_services_gsp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
