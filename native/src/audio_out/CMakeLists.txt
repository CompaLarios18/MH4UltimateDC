# mh4n_audio_out: what sits between the DSP and an audio device (N3-05), with no SDL: the `stream`
# AudioSink (a ring of stereo frames for a device thread to pull from), the drift controller that
# keeps the device's clock and the game's apart, and the `wav` AudioSink for tests. The SDL3 device
# is native/app/audio (N3-06). The headers are included as "audio_out/<name>.h".
mh4n_module(
    SOURCES
        drift_controller.cpp
        stream_factory.cpp
        stream_sink.cpp
        wav_sink.cpp
        wav_writer.cpp
    THIRD_PARTY
        mh4n_fmt
)
target_include_directories(mh4n_audio_out PUBLIC "${MH4N_SRC_DIR}")
