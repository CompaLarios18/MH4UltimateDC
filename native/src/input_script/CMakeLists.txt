# mh4n_input_script: the scripted InputSource `script` (N1-16), the runtime's default: it plays the
# presses of --input and --input-file (parsed by port/src/schedule.cpp through mh4n_runtime).
mh4n_module(
    SOURCES
        script_input.cpp
    DEPENDS
        mh4n_runtime
    THIRD_PARTY
        mh4n_fmt
)
# The headers are included as "input_script/<name>.h".
target_include_directories(mh4n_input_script PUBLIC "${MH4N_SRC_DIR}")
