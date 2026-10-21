# mh4n_services_hid: hid:USER and hid:SPVR over one HID module, and --capture-hid (N1-16). The pad,
# touch, accelerometer and gyroscope blocks of the shared memory are updated by events on the
# scheduler's timer queue, from the input source (the scripted one is src/input_script).
mh4n_module(
    SOURCES
        hid_capture.cpp
        hid_module.cpp
        hid_service.cpp
        hid_state.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
        mh4n_runtime
        mh4n_services_ir
    THIRD_PARTY
        mh4n_fmt
)
# The headers are included as "services/hid/<name>.h" (the tests), and next to the sources.
target_include_directories(mh4n_services_hid PUBLIC "${MH4N_SRC_DIR}")
target_include_directories(mh4n_services_hid PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
