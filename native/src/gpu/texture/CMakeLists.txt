# mh4n_gpu_texture: PICA texture decoding, the custom-texture hashes and the texture cache (N2-14),
# and the capture player's `--check tex-hashes`. XXH3 (native/third_party/xxhash) is the cache's
# content hash and is only included by cache.cpp.
mh4n_module(
    SOURCES
        cache.cpp
        cityhash.cpp
        decode.cpp
        hash.cpp
        tex_hashes.cpp
        texture_regs.cpp
    DEPENDS
        mh4n_gpu_capture
        mh4n_gpu_pica
)
# The headers are included as "gpu/texture/<name>.h".
target_include_directories(mh4n_gpu_texture PUBLIC "${MH4N_SRC_DIR}")
# xxhash.h is third-party: no warnings from it.
target_include_directories(mh4n_gpu_texture SYSTEM PRIVATE "${MH4N_ROOT}/third_party/xxhash")
