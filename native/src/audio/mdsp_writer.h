// The writer of MDSP captures (N1-18, --capture-dsp): the binary format of tools/native/capfmt.py
// (its docstring defines it) and of mh4u_runner's --capture-dsp (N0-06): the magic "MDSP", a u32
// version (1), then records {u32 type, u32 frame, u64 ticks, u32 size, payload}, all little-endian,
// and a sibling <file>.json with the manifest (tool, format, version, game, frames, record counts).
//
// The native runtime writes the type-1 records only: the two 0x8000-byte regions of the DSP memory
// as the DSP sees them at the start of an audio frame. It has no output samples to record (type 2:
// the model produces silence) and reads no sample data (type 3: nothing is decoded); both come with
// N3. Captures hold game data (the application's audio state): they live under build/ and never in
// the repository.
#pragma once

#include <cstdio>
#include <string>

#include "mh4n/types.h"

namespace mh4n::audio {

inline constexpr u32 kMdspVersion = 1;
inline constexpr u32 kMdspRegions = 1;  // record types, as capfmt.py's DSP_REGIONS, DSP_PCM, DSP_SAMPLES
inline constexpr u32 kMdspPcm = 2;
inline constexpr u32 kMdspSamples = 3;

class MdspWriter {
public:
    MdspWriter() = default;
    ~MdspWriter() { Close(); }
    MdspWriter(const MdspWriter&) = delete;
    MdspWriter& operator=(const MdspWriter&) = delete;

    // Creates `path` and writes the file header. False (and nothing open) if it cannot be written.
    // `tool` and `program_id` go to the manifest.
    bool Open(const std::string& path, u64 program_id, std::string tool = "mh4u_native_runner");
    bool IsOpen() const { return file_ != nullptr; }

    // A record of any type. False if the file is not open or a write failed.
    bool Add(u32 type, u32 frame, u64 ticks, const void* payload, u32 size);
    // Type 1: region 0 and then region 1, `region_size` bytes each.
    bool AddRegions(u32 frame, u64 ticks, const u8* region0, const u8* region1, u32 region_size);

    // Audio frames written (the type-1 records).
    u32 Frames() const { return frames_; }
    u32 Count(u32 type) const { return type < 4 ? counts_[type] : 0; }

    // Closes the file and writes the manifest. Safe to call twice.
    void Close();

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    std::string tool_;
    u64 program_id_ = 0;
    u32 frames_ = 0;
    u32 counts_[4] = {};
    bool failed_ = false;
};

}  // namespace mh4n::audio
