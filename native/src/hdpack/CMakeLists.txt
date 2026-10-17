# mh4n_hdpack: the HD pack pre-processor (N2-17): reads a texture pack in the layout Azahar loads,
# builds the mip chains, encodes to BC7 and writes one indexed archive. The program is
# native/tools/hdpack (only main); the archive reader is what N2-18's streaming uses.
#
# The two third-party encoders are built here, as their own libraries, without warnings:
#   native/third_party/bc7/   bc7enc_rdo's BC7 block encoder (MIT or public domain)
#   native/third_party/png/   LodePNG (zlib licence)
add_library(mh4n_hdpack_bc7enc STATIC "${MH4N_ROOT}/third_party/bc7/bc7enc.cpp")
target_include_directories(mh4n_hdpack_bc7enc SYSTEM PUBLIC "${MH4N_ROOT}/third_party/bc7")
target_compile_options(mh4n_hdpack_bc7enc PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)

add_library(mh4n_hdpack_lodepng STATIC "${MH4N_ROOT}/third_party/png/lodepng.cpp")
target_include_directories(mh4n_hdpack_lodepng SYSTEM PUBLIC "${MH4N_ROOT}/third_party/png")
target_compile_options(mh4n_hdpack_lodepng PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)

mh4n_module(
    SOURCES
        archive.cpp
        bc7.cpp
        convert.cpp
        image.cpp
        json.cpp
        mips.cpp
        pack.cpp
        tool.cpp
    THIRD_PARTY
        mh4n_hdpack_bc7enc
        mh4n_hdpack_lodepng
        Threads::Threads
)
# The headers are included as "hdpack/<name>.h".
target_include_directories(mh4n_hdpack PUBLIC "${MH4N_SRC_DIR}")
