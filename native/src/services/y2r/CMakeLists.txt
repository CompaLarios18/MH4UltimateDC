# mh4n_services_y2r: y2r:u, the YUV to RGB converter (N1-19).
mh4n_module(
    SOURCES
        y2r_convert.cpp
        y2r_u.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
        mh4n_memory
)
