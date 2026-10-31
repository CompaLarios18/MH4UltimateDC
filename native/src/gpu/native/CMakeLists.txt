# mh4n_gpu_native (N2-33): the composition of the native renderer that does not depend on how shaders are generated
# (docs/native/gpu-fragment-design.md 4.1, 4.3, 4.5, 4.7, 5.1, 5.5, 5.6, 5.10): the pipeline key and the provider interface
# (`pipeline_key.h`, a header-only INTERFACE target of its own), the default provider (a synchronous build on a miss, the
# forced-ubershader switch), the one pipeline layout, the vertex-input key from N2-05's description, and the
# `DrawComposer` with its slots (pass kind, fix-up stage, destination copy, bindings). N2-08 adds the generator, the
# ubershader, `--gpu native` and the stubs on top.

# pipeline_key.h depends only on the fragment key's header and rhi.h; N2-16's pipeline/ links this target.
add_library(mh4n_gpu_pipeline_key INTERFACE)
target_include_directories(mh4n_gpu_pipeline_key INTERFACE "${MH4N_SRC_DIR}")
target_link_libraries(mh4n_gpu_pipeline_key INTERFACE mh4n_headers)

mh4n_module(
    SOURCES
        compose.cpp
        default_provider.cpp
        vertex_input.cpp
    DEPENDS
        mh4n_gpu_fragment
        mh4n_gpu_vertex
        mh4n_gpu_backend_targets
)
# The headers are included as "gpu/native/<name>.h".
target_include_directories(mh4n_gpu_native PUBLIC "${MH4N_SRC_DIR}")
target_link_libraries(mh4n_gpu_native PUBLIC mh4n_gpu_pipeline_key)
