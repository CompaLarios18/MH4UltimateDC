# mh4n_dsp_hle: the sources of our own HLE DSP (N3-02): the buffer queue of a source, the PCM8, PCM16
# and ADPCM decoders, the interpolation, the simple and biquad filters and the mix of a source into the
# intermediate mixes, bit for bit as Azahar's HLE DSP does them (tools/native/dsp_oracle compiles
# Azahar's own code and the committed table says so). The mixers, the effects and the frame come with
# N3-03; the service that uses it with N3-11. The status rules are those of the model (mh4n_audio).
mh4n_module(
    SOURCES
        codec.cpp
        filter.cpp
        interpolate.cpp
        source.cpp
    DEPENDS
        mh4n_audio
    THIRD_PARTY
        mh4n_fmt
)
# The headers are included as "dsp_hle/<name>.h".
target_include_directories(mh4n_dsp_hle PUBLIC "${MH4N_SRC_DIR}")

# No fused multiply-add: the gain ramp and the mix are Azahar's floats (the oracle is built the same
# way), whatever the host. Precedent: native/src/screen_layout/CMakeLists.txt.
if (NOT MSVC)
    set_source_files_properties(source.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
