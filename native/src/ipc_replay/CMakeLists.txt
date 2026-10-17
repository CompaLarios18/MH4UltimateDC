# mh4n_ipc_replay: replays the requests of an IPC trace against the native services alone and reports
# where the replies differ (N1-11). The program is native/tools/ipc_replay (only main); the format of
# the traces is that of tools/native/tracefmt.py, the manual is docs/native/ipc-replay.md.
mh4n_module(
    SOURCES
        replay.cpp
        time_dependent.cpp
        tool.cpp
        trace.cpp
    DEPENDS
        mh4n_ipc
        mh4n_kernel
        mh4n_memory
        mh4n_runtime
    THIRD_PARTY
        mh4n_fmt
)
# The headers are included as "ipc_replay/<name>.h".
target_include_directories(mh4n_ipc_replay PUBLIC "${MH4N_SRC_DIR}")
