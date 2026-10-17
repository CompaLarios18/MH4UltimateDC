# mh4n_ipc: requests and replies, sessions, ports, the service manager and --trace-ipc (N1-10).
# The `services` boot step installs the registered services and the SVC layer's ConnectToPort and
# SendSyncRequest handlers; the services themselves are other modules (services/srv, services/err, ...).
mh4n_module(
    SOURCES
        boot.cpp
        manager.cpp
        request.cpp
        service.cpp
        trace.cpp
    DEPENDS
        mh4n_memory
        mh4n_kernel
        mh4n_runtime
    THIRD_PARTY
        mh4n_fmt
)
target_include_directories(mh4n_ipc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
