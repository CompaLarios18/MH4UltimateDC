# mh4n_shots: the native runner's screenshots (N2b-14): --shots, --shots-from and --shot-every with the
# reference runner's syntax, names and timing, taken from any GPU that implements present::Display.
# The schedule is the reference's own (Schedule::ShotSchedule, port/src/schedule.cpp, compiled into
# mh4n_runtime); a copy of native/ on its own has no port/ around it and the module is then empty.
mh4n_module(
    SOURCES
        shots.cpp
    DEPENDS
        mh4n_runtime
    THIRD_PARTY
        mh4n_fmt
)
# The header is included as "shots/shots.h".
target_include_directories(mh4n_shots PUBLIC "${MH4N_SRC_DIR}")
