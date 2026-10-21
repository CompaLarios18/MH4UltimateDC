# mh4n_services_apt: APT:U, APT:A and APT:S and the applet manager behind them (N1-14). The HLE
# applets (the software keyboard) are the module mh4n_applets; the CPU time limit commands go to the
# thread manager of mh4n_kernel (N1-27).
mh4n_module(
    SOURCES
        applet_manager.cpp
        apt_service.cpp
    DEPENDS
        mh4n_applets
        mh4n_ipc
        mh4n_kernel
        mh4n_runtime
)
# The headers are included as "services/apt/<name>.h" (the tests), and next to the sources.
target_include_directories(mh4n_services_apt PUBLIC "${MH4N_SRC_DIR}")
target_include_directories(mh4n_services_apt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
