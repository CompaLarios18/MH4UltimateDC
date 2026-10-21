# mh4n_services_cecd: cecd:u (N1-19).
mh4n_module(
    SOURCES
        cecd_u.cpp
        cecd_store.cpp
        sha256.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
        mh4n_loader
)
