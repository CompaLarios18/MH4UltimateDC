# mh4n_dsp_replay: an MDSP capture of the reference replayed through our own DSP, frame by frame, and
# judged by D75 (N3-04). The program is native/tools/dsp_replay (only main); the format of the capture is
# that of tools/native/capfmt.py, the manual is docs/native/dsp-replay.md.
mh4n_module(
    SOURCES
        capture.cpp
        judge.cpp
        replay.cpp
        tool.cpp
    DEPENDS
        mh4n_audio
        mh4n_dsp_hle
)
# The headers are included as "dsp_replay/<name>.h".
target_include_directories(mh4n_dsp_replay PUBLIC "${MH4N_SRC_DIR}")
