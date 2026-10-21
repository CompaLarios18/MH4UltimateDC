# mh4n_services_nwm: nwm::UDS, the local-wireless service, as Azahar answers it when there is no room
# (N1-29).
mh4n_module(
    SOURCES
        nwm_uds.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
)
