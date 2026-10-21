# mh4n_services_ir: ir:USER with no Circle Pad Pro (N1-16). The state (events, the shared memory the
# game gave, the receive buffer) is included as "services/ir/ir_state.h" by the HID capture.
mh4n_module(
    SOURCES
        ir_buffer.cpp
        ir_user.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
)
target_include_directories(mh4n_services_ir PUBLIC "${MH4N_SRC_DIR}")
# The private headers of this directory.
target_include_directories(mh4n_services_ir PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
