# mh4n_runtime: the Runtime (boot order, frame watcher, host interfaces, the runner's own options
# and traces) of N1-09. Later modules add boot steps, options and devices from their own
# directories; nothing here needs editing for that.
#
# port/src/schedule.cpp (N0-08: the parsing of times, input events and scheduled options, standard
# library only) is compiled into this module, so every module that depends on mh4n_runtime can
# include "schedule.h" and call Schedule::Parse*. A copy of native/ on its own (see
# tests/test_native_cmake_layout.py) has no port/ around it: the runtime then does without, and
# parses --ms and --frames itself.
set(MH4N_SCHEDULE_SOURCE "${MH4N_ROOT}/../port/src/schedule.cpp")
set(MH4N_SCHEDULE_HEADER "${MH4N_ROOT}/../port/src/schedule.h")

set(mh4n_runtime_sources
    devices.cpp
    gpu_host.cpp
    options.cpp
    runtime.cpp
    state_trace.cpp
    svc_trace.cpp
)
set(mh4n_runtime_have_schedule OFF)
if (EXISTS "${MH4N_SCHEDULE_SOURCE}" AND EXISTS "${MH4N_SCHEDULE_HEADER}")
    set(mh4n_runtime_have_schedule ON)
    list(APPEND mh4n_runtime_sources "${MH4N_SCHEDULE_SOURCE}")
else()
    message(STATUS "mh4n_runtime: port/src/schedule.cpp not found around native/: no Schedule library")
endif()

mh4n_module(
    SOURCES ${mh4n_runtime_sources}
    DEPENDS
        mh4n_memory
        mh4n_loader
        mh4n_cpu
        mh4n_kernel
    THIRD_PARTY
        mh4n_fmt
)

if (mh4n_runtime_have_schedule)
    # schedule.h is included as "schedule.h".
    get_filename_component(MH4N_SCHEDULE_DIR "${MH4N_SCHEDULE_HEADER}" DIRECTORY)
    target_include_directories(mh4n_runtime PUBLIC "${MH4N_SCHEDULE_DIR}")
    target_compile_definitions(mh4n_runtime PUBLIC MH4N_HAVE_SCHEDULE=1)
endif()
# The private headers of this directory.
target_include_directories(mh4n_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
