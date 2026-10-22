// A 16-bit PCM WAV file writer (N3-05): the `wav` sink's file, and the test's. The header is
// written when the file is opened with sizes of 0 and again, with the real sizes, by Close.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

#include "mh4n/types.h"

namespace mh4n::audio_out {

class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Creates `path` and writes the header. False, with nothing open, if the file cannot be created.
    bool Open(const std::string& path, u32 sample_rate, u16 channels);
    bool IsOpen() const { return file_ != nullptr; }
    // Appends `frames` frames of interleaved s16 samples. False once a write has failed.
    bool Write(const s16* samples, std::size_t frames);
    // Rewrites the header with the sizes and closes the file. True if every write and the close
    // succeeded. Calling it again does nothing and returns the same answer.
    bool Close();

    u64 FramesWritten() const { return frames_; }

private:
    std::FILE* file_ = nullptr;
    u16 channels_ = 0;
    u32 sample_rate_ = 0;
    u64 frames_ = 0;
    bool ok_ = true;
};

}  // namespace mh4n::audio_out
