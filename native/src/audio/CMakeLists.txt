# mh4n_audio: the model of the DSP (N1-18): the shared memory layout, the sources' buffer queues and
# the pipes of Azahar's HLE DSP without decoding or mixing, the writer of MDSP captures and the null
# AudioSink. The service that uses it is services/dsp; N3 replaces the model with the full HLE DSP
# behind the same service.
mh4n_module(
    SOURCES
        dsp_model.cpp
        mdsp_writer.cpp
        null_sink.cpp
    THIRD_PARTY
        mh4n_fmt
)
# The headers are included as "audio/<name>.h".
target_include_directories(mh4n_audio PUBLIC "${MH4N_SRC_DIR}")
