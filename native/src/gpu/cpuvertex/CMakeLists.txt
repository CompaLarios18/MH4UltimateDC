# mh4n_gpu_cpuvertex: the CPU shader runner (N2-36). Runs the vertex shader and the geometry shader
# of the draws Azahar does not accelerate on the CPU, as Azahar's PicaCore::DrawArrays and
# DrawImmediate do, and returns the output vertices that the primitive assembler (N2-37) turns into
# triangles. No backend and no device; the API is the header comment of cpu_vertex.h.
mh4n_module(
    SOURCES
        cpu_vertex.cpp
        geometry_pipeline.cpp
        shader_interpreter.cpp
    DEPENDS
        mh4n_gpu_pica
        mh4n_gpu_vertex
)
# The headers are included as "gpu/cpuvertex/<name>.h".
target_include_directories(mh4n_gpu_cpuvertex PUBLIC "${MH4N_SRC_DIR}")
