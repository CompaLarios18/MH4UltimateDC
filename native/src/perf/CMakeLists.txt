# mh4n_perf: --perf-log FILE (N1-23), a CSV row per game frame with the time of the scheduler's
# counters (N1-06). A boot step of the phase Tracing subscribes to Runtime::OnFrame; without the
# option it does nothing at all. The header is included as "perf/perf_log.h".
mh4n_module(
    SOURCES
        perf_log.cpp
    DEPENDS
        mh4n_runtime
    THIRD_PARTY
        mh4n_fmt
)
target_include_directories(mh4n_perf PUBLIC "${MH4N_SRC_DIR}")
