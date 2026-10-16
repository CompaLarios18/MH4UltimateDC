# mh4n_gpu_pica: the PICA command processor (N2-04). Decodes command lists, keeps the internal
# register file and the state behind it (shader setups, look-up tables, default attributes,
# immediate mode, geometry pipeline), and hands each draw to a Backend. Registers the capture
# player's `--gpu pica-only`.
mh4n_module(
    SOURCES
        command_processor.cpp
        pica_gpu.cpp
        state.cpp
)
# The headers are included as "gpu/pica/<name>.h".
target_include_directories(mh4n_gpu_pica PUBLIC "${MH4N_SRC_DIR}")
