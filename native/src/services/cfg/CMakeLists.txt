# mh4n_services_cfg: cfg:u, cfg:s, cfg:i, cfg:nor and the console's identity (N1-15): the config
# savegame in the user folder (Azahar's file), the console unique ID, the MAC address.
mh4n_module(
    SOURCES
        cfg_store.cpp
        cfg_tables.cpp
        cfg_service.cpp
        sha256.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
        mh4n_runtime
)
target_include_directories(mh4n_services_cfg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
