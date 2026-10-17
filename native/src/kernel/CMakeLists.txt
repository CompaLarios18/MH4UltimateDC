# mh4n_kernel: kernel objects, the handle table and the Kernel facade (N1-05). N1-06 (scheduler),
# N1-07 (SVCs) and N1-27 (threads) add their .cpp files to this directory; the glob picks them up
# without editing this file (add `DEPENDS` for the modules they use when they need to).
file(GLOB kernel_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(SORT kernel_sources)

mh4n_module(
    SOURCES ${kernel_sources}
    DEPENDS mh4n_memory
)
