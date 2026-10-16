// Reader for MGPU captures (format version 1, docs/native/mgpu-format.md). Mirrors
// tools/native/gcap.py: the same record types, the same payload sizes, the same refusals.
#pragma once

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/types.h"

namespace mh4n::gpu_capture {

inline constexpr u32 kMgpuVersion = 1;
inline constexpr u32 kPageSize = 4096;
inline constexpr u32 kNumPicaRegs = 0x300;

enum RecordType : u32 {
    kInit = 1,
    kMem = 2,
    kCpuPages = 3,
    kGx = 4,
    kPicaReg = 5,
    kGpuPages = 6,
    kMmio = 7,
    kSwap = 8,
    kFill = 9,
    kIrq = 10,
    kFlush = 11,
    kFrame = 12,
};
inline constexpr u32 kLastRecordType = 12;

// GX command ids (Service::GSP::CommandId).
enum GxId : u32 {
    kGxRequestDma = 0,
    kGxSubmitCmdList = 1,
    kGxMemoryFill = 2,
    kGxDisplayTransfer = 3,
    kGxTextureCopy = 4,
    kGxCacheFlush = 5,
};

// FLUSH kinds.
enum FlushKind : u32 {
    kFlushDataCache = 1,
    kInvalidateDataCache = 2,
    kStoreDataCache = 3,
    kCacheFlushRegion = 4,
};

const char* RecordName(u32 type);
const char* GxName(u32 id);

struct Record {
    u32 type = 0;
    u32 frame = 0; // the game's frame counter when the record was written
    u64 ticks = 0;
    std::vector<u8> payload;

    const char* name() const {
        return RecordName(type);
    }
};

// Reads the records of a capture one by one.
class Reader {
public:
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();

    // False, with *error set, if the file cannot be opened, is not an MGPU file, or has a
    // version this reader does not know.
    bool Open(const std::string& path, std::string* error);

    enum class Status { Record, End, Error };
    // Record: `out` holds the next record. End: the file ended cleanly. Error: it is damaged
    // (truncated header or payload); *error says where. Records before the damage were returned.
    Status Next(Record& out, std::string* error);

    // The 0-based index of the record the next call returns.
    size_t next_index() const {
        return index_;
    }

private:
    std::FILE* file_ = nullptr;
    size_t index_ = 0;
    u64 file_size_ = ~u64{0};
    u64 position_ = 0;
};

// ---- payload decoding -------------------------------------------------------------------------
// Each returns false with *error set when the payload does not have the size or content the
// format requires (gcap.py's check_record).

struct InitBlockData {
    u32 kind;
    std::span<const u8> data;
};
bool ParseInit(const Record& record, std::vector<InitBlockData>& blocks, std::string* error);
// The size every INIT block kind must have (0 for an unknown kind).
u32 InitBlockSize(u32 kind);
// The blocks of a validated INIT record as the Gpu gets them.
bool MakeInitState(const Record& record, gpu::InitState& state, std::string* error);

struct MemData {
    u32 address;
    std::span<const u8> data;
};
bool ParseMem(const Record& record, MemData& mem, std::string* error);

struct PageData {
    u32 address;
    std::span<const u8, kPageSize> data;
};
bool ParsePages(const Record& record, std::vector<PageData>& pages, std::string* error);

bool ParseGx(const Record& record, gpu::GxCommand& command, std::string* error);
bool ParsePicaReg(const Record& record, std::vector<gpu::RegisterWrite>& writes,
                  std::string* error);

// The 4-bit byte mask of a PICA_REG write as a 32-bit bit mask (bit n of the mask -> byte n).
inline u32 ExpandMask(u32 mask) {
    u32 m = 0;
    for (u32 n = 0; n < 4; ++n) {
        if (mask & (1u << n)) {
            m |= 0xFFu << (8 * n);
        }
    }
    return m;
}

struct MmioData {
    u32 address, value;
};
bool ParseMmio(const Record& record, MmioData& mmio, std::string* error);

struct SwapData {
    u32 screen;
    gpu::FrameBufferInfo info;
};
bool ParseSwap(const Record& record, SwapData& swap, std::string* error);

bool ParseFill(const Record& record, u32& word, std::string* error);

struct IrqData {
    u32 id;
    u64 delay_ns;
};
bool ParseIrq(const Record& record, IrqData& irq, std::string* error);

struct FlushData {
    u32 kind, address, size;
};
bool ParseFlush(const Record& record, FlushData& flush, std::string* error);

bool ParseFrame(const Record& record, u32& frame, std::string* error);

// Validates one record on its own: the unknown type, the payload size and, where the format has
// them, the content rules (gcap.py's check_record). Empty string: valid.
std::string CheckRecord(const Record& record);

} // namespace mh4n::gpu_capture
