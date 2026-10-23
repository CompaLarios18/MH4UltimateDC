// The reader of an MDSP capture (N3-04), the format of tools/native/capfmt.py (N0-06's As built):
// the magic `MDSP`, a u32 version (1), then records {u32 type, u32 frame, u64 ticks, u32 size, payload}.
//   1  the two DSP regions (0x8000 bytes each) at the start of an audio frame
//   2  the 160 stereo s16 samples the frame output (640 bytes)
//   3  sample data the DSP read: u32 physical address, u32 size, u32 CRC-32, then the bytes (only the
//      first time an (address, size, CRC) key appears; later records stop after the CRC)
// The reader is streaming: it hands out one record at a time and never holds the file.
#pragma once

#include <array>
#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::dsp_replay {

inline constexpr u32 kRegionsRecord = 1;
inline constexpr u32 kPcmRecord = 2;
inline constexpr u32 kSamplesRecord = 3;
inline constexpr std::size_t kRegionsBytes = 2 * 0x8000;
inline constexpr std::size_t kPcmBytes = 160 * 2 * 2;
inline constexpr std::size_t kSampleHeadBytes = 12;

// A capture that cannot be read or is not valid: the tool exits with 2.
class CaptureError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Record {
    u32 type = 0;
    u32 frame = 0;  // the game's frame counter, not the DSP's
    u64 ticks = 0;
    std::vector<u8> payload;
};

class CaptureReader {
public:
    // Reads and checks the header; throws CaptureError.
    explicit CaptureReader(std::istream& in);
    // The next record, false at the end of the file. Throws CaptureError for a truncated record, a
    // type that is not 1 to 3, or a size that is not the type's (a type-3 record holds 12 bytes, or 12
    // plus its size field's bytes).
    bool Next(Record& record);

private:
    std::istream& in_;
};

struct SampleRead {
    u32 address = 0;
    u32 size = 0;
    u32 crc = 0;
    bool has_bytes = false;
    const u8* bytes = nullptr;  // into the record's payload, when has_bytes
};
// Decodes a type-3 payload; throws CaptureError when the CRC of the bytes is not the one it states.
SampleRead ParseSampleRead(const Record& record);

}  // namespace mh4n::dsp_replay
