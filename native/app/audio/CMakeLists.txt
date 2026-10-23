# The SDL3 audio device of mh4u-native (N3-06): the `sdl` AudioSinkFactory, --volume and
# --audio-log, and the feeder that pulls the `stream` sink (native/src/audio_out) into SDL's audio
# stream. The headers are included as "audio/<name>.h".
mh4n_app_module(SOURCES
    feeder.cpp
    sdl_sink.cpp)
target_include_directories(mh4n_app_audio PUBLIC "${MH4N_APP_DIR}" "${MH4N_SRC_DIR}")
