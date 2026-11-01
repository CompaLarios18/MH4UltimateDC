# mh4n_gpu_fragment (N2-33, N2-08): the fragment pipeline that does not depend on the device or the composition
# (docs/native/gpu-fragment-design.md 4.3, 4.4, 4.7, 4.8, 5.10): the fragment key (Azahar's FSConfig, ported whole, with
# the canonical rule), the device profile, `Needs`, the fragment uniforms (FSUniformData plus the key itself and the
# reserved gas region, one ring allocation), the lookup-table rings, the feature interface with its registry, and the
# per-draw driver (N2-33); the generator (N2-08): the two-mode emitter, the core stages (prologue, scissor, depth, the six
# TEV stages, the alpha test, the output) in the specialised shader and the ubershader, and the five feature directories
# (sampling/, lighting/, fog/, proctex/, shadow/) with the stubs of the features that come next (N2-23, N2-09 to N2-12), each
# of which replaces the file of its directory and nothing else.
mh4n_module(
    SOURCES
        emitter.cpp
        feature.cpp
        generator.cpp
        spirv_compile.cpp
        key.cpp
        state.cpp
        tables.cpp
        uniforms.cpp
        fog/fog.cpp
        lighting/lighting.cpp
        proctex/proctex.cpp
        sampling/sampling.cpp
        shadow/shadow.cpp
    DEPENDS
        mh4n_gpu_pica
        mh4n_gpu_backend_core
        mh4n_gpu_fbops
    THIRD_PARTY
        mh4n_glslang
)
# The headers are included as "gpu/fragment/<name>.h".
target_include_directories(mh4n_gpu_fragment PUBLIC "${MH4N_SRC_DIR}")
