# mh4n_input_live: the live InputSource `live` (N2b-08): the app feeds it from any thread, the game
# sees it change only at frame boundaries (D59), and --record-input writes what the game saw as a
# frame-keyed script that --input-file reads back. It needs nothing of port/src/schedule.cpp (it only
# writes the grammar); the tests parse the result with it.
mh4n_module(
    SOURCES
        live_input.cpp
        script_recorder.cpp
    DEPENDS
        mh4n_runtime
    THIRD_PARTY
        mh4n_fmt
)
# The headers are included as "input_live/<name>.h".
target_include_directories(mh4n_input_live PUBLIC "${MH4N_SRC_DIR}")
