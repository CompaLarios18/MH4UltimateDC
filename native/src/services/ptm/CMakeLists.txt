# mh4n_services_ptm: ptm:u, ptm:s and ptm:sysm (N1-15): battery, charging, adapter, shell and pedometer
# answers of Azahar's defaults, and the play coin file Azahar's ptm makes at start-up (N1-12b, needs the fs module's
# SharedExtSaveData archive).
mh4n_module(
    SOURCES
        ptm.cpp
        ptm_gamecoin.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
        mh4n_runtime
        mh4n_services_fs
)
