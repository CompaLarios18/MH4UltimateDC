# mh4n_gpu_shader: the PICA vertex shader as SPIR-V (N2-06). Ports Azahar's GLSL decompiler (the
# program -> GLSL translator, the vertex shader around it, the uniform blocks) to Vulkan GLSL and
# compiles it with glslang, linked as a library: native/third_party/glslang (mh4n_glslang), added here
# with its own small CMakeLists.txt. The interface is the header comment of vertex_shader.h. No Vulkan
# or RHI type is used; the tests run the SPIR-V on the backend core.
add_subdirectory("${MH4N_ROOT}/third_party/glslang" "${CMAKE_CURRENT_BINARY_DIR}/glslang")

mh4n_module(
    SOURCES
        glsl_decompiler.cpp
        pica_isa.cpp
        spirv_compile.cpp
        vertex_shader.cpp
    DEPENDS
        mh4n_gpu_pica
        mh4n_gpu_vertex
    THIRD_PARTY
        mh4n_glslang
)
# The headers are included as "gpu/shader/<name>.h".
target_include_directories(mh4n_gpu_shader PUBLIC "${MH4N_SRC_DIR}")
