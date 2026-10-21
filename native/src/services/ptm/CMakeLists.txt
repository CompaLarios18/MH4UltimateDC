# mh4n_services_ptm: ptm:u, ptm:s and ptm:sysm (N1-15): battery, charging, adapter, shell and pedometer
# answers of Azahar's defaults.
mh4n_module(
    SOURCES
        ptm.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
)
