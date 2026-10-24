# mh4n_gpu_vertex: vertex loading (N2-05). Turns a PICA draw call's vertex arrays, index array,
# default/fixed attributes and immediate-mode vertex into host vertex buffers, index buffers and a
# vertex-input description that does not depend on the RHI (see vertex_loader.h).
mh4n_module(
    SOURCES
        vertex_loader.cpp
    DEPENDS
        mh4n_gpu_pica
)
# The headers are included as "gpu/vertex/<name>.h".
target_include_directories(mh4n_gpu_vertex PUBLIC "${MH4N_SRC_DIR}")
