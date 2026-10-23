#include "dsp_replay/capture.h"

#include <cstring>

#include "mh4n/crc32.h"

namespace mh4n::dsp_replay {

namespace {

constexpr char kMagic[4] = {'M', 'D', 'S', 'P'};
constexpr u32 kVersion = 1;
// A sanity bound on one record's payload: a type-3 record can hold a whole buffer.
constexpr u32 kMaxPayload = 0x20000000;

u32 LoadU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
u64 LoadU64(const u8* p) {
    return static_cast<u64>(LoadU32(p)) | (static_cast<u64>(LoadU32(p + 4)) << 32);
}

void ReadExact(std::istream& in, void* out, std::size_t size, const char* what) {
    in.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    if (static_cast<std::size_t>(in.gcount()) != size) {
        throw CaptureError(std::string("truncated capture: ") + what);
    }
}

}  // namespace

CaptureReader::CaptureReader(std::istream& in) : in_(in) {
    u8 head[8];
    in_.read(reinterpret_cast<char*>(head), sizeof(head));
    if (static_cast<std::size_t>(in_.gcount()) != sizeof(head)) {
        throw CaptureError("not a capture: shorter than its header");
    }
    if (std::memcmp(head, kMagic, 4) != 0) {
        throw CaptureError("not a DSP capture: the magic is not MDSP");
    }
    if (LoadU32(head + 4) != kVersion) {
        throw CaptureError("unsupported MDSP version " + std::to_string(LoadU32(head + 4)));
    }
}

bool CaptureReader::Next(Record& record) {
    u8 head[20];
    in_.read(reinterpret_cast<char*>(head), 1);
    if (in_.gcount() == 0) {
        return false;
    }
    ReadExact(in_, head + 1, sizeof(head) - 1, "a record header");
    record.type = LoadU32(head);
    record.frame = LoadU32(head + 4);
    record.ticks = LoadU64(head + 8);
    const u32 size = LoadU32(head + 16);
    if (size > kMaxPayload) {
        throw CaptureError("a record claims " + std::to_string(size) + " bytes");
    }
    record.payload.resize(size);
    if (size > 0) {
        ReadExact(in_, record.payload.data(), size, "a record's payload");
    }
    switch (record.type) {
    case kRegionsRecord:
        if (size != kRegionsBytes) {
            throw CaptureError("a REGIONS record has " + std::to_string(size) + " bytes");
        }
        break;
    case kPcmRecord:
        if (size != kPcmBytes) {
            throw CaptureError("a PCM record has " + std::to_string(size) + " bytes");
        }
        break;
    case kSamplesRecord:
        if (size < kSampleHeadBytes) {
            throw CaptureError("a SAMPLES record has " + std::to_string(size) + " bytes");
        }
        if (size != kSampleHeadBytes &&
            size != kSampleHeadBytes + static_cast<u64>(LoadU32(record.payload.data() + 4))) {
            throw CaptureError("a SAMPLES record's size does not match its read");
        }
        break;
    default:
        throw CaptureError("unknown record type " + std::to_string(record.type));
    }
    return true;
}

SampleRead ParseSampleRead(const Record& record) {
    SampleRead read;
    const u8* p = record.payload.data();
    read.address = LoadU32(p);
    read.size = LoadU32(p + 4);
    read.crc = LoadU32(p + 8);
    read.has_bytes = record.payload.size() > kSampleHeadBytes;
    if (read.has_bytes) {
        read.bytes = p + kSampleHeadBytes;
        if (Crc32(read.bytes, read.size) != read.crc) {
            throw CaptureError("a SAMPLES record's bytes do not match its CRC-32");
        }
    }
    return read;
}

}  // namespace mh4n::dsp_replay
