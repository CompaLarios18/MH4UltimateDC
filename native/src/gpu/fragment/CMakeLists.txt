# mh4n_gpu_fragment (N2-33): the parts of the fragment pipeline that do not depend on how shaders are generated
# (docs/native/gpu-fragment-design.md 4.3, 4.7, 4.8, 5.10): the fragment key (Azahar's FSConfig, ported whole, with
# the canonical rule), the device profile, `Needs`, the fragment uniforms (FSUniformData plus the key itself and the
# reserved gas region, one ring allocation), the lookup-table rings, the feature interface with its registry, and
# the per-draw driver. The generator, the ubershader, TEV and the five feature directories (sampling/, lighting/,
# fog/, proctex/, shadow/) are N2-08's and the feature packages': they add their own sources under this directory.
mh4n_module(
    SOURCES
        feature.cpp
        key.cpp
        state.cpp
        tables.cpp
        uniforms.cpp
    DEPENDS
        mh4n_gpu_pica
        mh4n_gpu_backend_core
)
# The headers are included as "gpu/fragment/<name>.h".
target_include_directories(mh4n_gpu_fragment PUBLIC "${MH4N_SRC_DIR}")
