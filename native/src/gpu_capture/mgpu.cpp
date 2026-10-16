#include "gpu_capture/mgpu.h"

#include <cstring>
#include <filesystem>
#include <system_error>

#include <fmt/format.h>

namespace mh4n::gpu_capture {

namespace {

constexpr char kMagic[4] = {'M', 'G', 'P', 'U'};
constexpr size_t kFileHeaderSize = 8;
constexpr size_t kRecordHeaderSize = 20;

u16 Le16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 Le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
u64 Le64(const u8* p) {
    return static_cast<u64>(Le32(p)) | (static_cast<u64>(Le32(p + 4)) << 32);
}

bool Fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

constexpr u32 kSetupSize = 12 + 4096 * 4 + 4096 * 4 + 96 * 16 + 16 + 16 + 20;

} // namespace

const char* RecordName(u32 type) {
    switch (type) {
    case kInit:
        return "INIT";
    case kMem:
        return "MEM";
    case kCpuPages:
        return "CPU_PAGES";
    case kGx:
        return "GX";
    case kPicaReg:
        return "PICA_REG";
    case kGpuPages:
        return "GPU_PAGES";
    case kMmio:
        return "MMIO";
    case kSwap:
        return "SWAP";
    case kFill:
        return "FILL";
    case kIrq:
        return "IRQ";
    case kFlush:
        return "FLUSH";
    case kFrame:
        return "FRAME";
    default:
        return "UNKNOWN";
    }
}

const char* GxName(u32 id) {
    switch (id) {
    case kGxRequestDma:
        return "REQUEST_DMA";
    case kGxSubmitCmdList:
        return "SUBMIT_CMDLIST";
    case kGxMemoryFill:
        return "MEMORY_FILL";
    case kGxDisplayTransfer:
        return "DISPLAY_TRANSFER";
    case kGxTextureCopy:
        return "TEXTURE_COPY";
    case kGxCacheFlush:
        return "CACHE_FLUSH";
    default:
        return "UNKNOWN";
    }
}

u32 InitBlockSize(u32 kind) {
    switch (static_cast<gpu::InitBlock>(kind)) {
    case gpu::InitBlock::GpuRegs:
        return 0x432 * 4;
    case gpu::InitBlock::LcdRegs:
        return 0x400 * 4;
    case gpu::InitBlock::PicaRegs:
        return kNumPicaRegs * 4;
    case gpu::InitBlock::VsSetup:
    case gpu::InitBlock::GsSetup:
        return kSetupSize;
    case gpu::InitBlock::DefaultAttrs:
        return 16 * 16;
    case gpu::InitBlock::LightingLut:
        return 24 * 256 * 4;
    case gpu::InitBlock::FogLut:
        return 128 * 4;
    case gpu::InitBlock::ProcTex:
        return (128 * 3 + 256 * 2) * 4;
    case gpu::InitBlock::Immediate:
        return 16 * 16 + 8 + 20;
    case gpu::InitBlock::GeometryPipeline:
        return 8 * 4 + 16 * 16;
    }
    return 0;
}

// ---- Reader -----------------------------------------------------------------------------------

Reader::~Reader() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

bool Reader::Open(const std::string& path, std::string* error) {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    index_ = 0;
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        return Fail(error, fmt::format("cannot open {}", path));
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    file_size_ = ec ? ~u64{0} : static_cast<u64>(size);
    u8 head[kFileHeaderSize];
    if (std::fread(head, 1, sizeof(head), file_) != sizeof(head)) {
        return Fail(error, "truncated: file header");
    }
    position_ = kFileHeaderSize;
    if (std::memcmp(head, kMagic, 4) != 0) {
        return Fail(error, "wrong magic, expected MGPU");
    }
    const u32 version = Le32(head + 4);
    if (version != kMgpuVersion) {
        return Fail(error, fmt::format("unsupported format version {} (known: {})", version,
                                       kMgpuVersion));
    }
    return true;
}

Reader::Status Reader::Next(Record& out, std::string* error) {
    if (file_ == nullptr) {
        Fail(error, "no file open");
        return Status::Error;
    }
    u8 head[kRecordHeaderSize];
    const size_t got = std::fread(head, 1, sizeof(head), file_);
    if (got == 0) {
        return Status::End;
    }
    if (got < sizeof(head)) {
        Fail(error, fmt::format("truncated: record {} header", index_));
        return Status::Error;
    }
    position_ += kRecordHeaderSize;
    out.type = Le32(head);
    out.frame = Le32(head + 4);
    out.ticks = Le64(head + 8);
    const u32 size = Le32(head + 16);
    if (file_size_ != ~u64{0} && position_ + size > file_size_) {
        // Do not allocate what the file cannot hold (a damaged size field).
        Fail(error, fmt::format("truncated: record {} payload needs {} bytes, got {}", index_, size,
                                file_size_ - position_));
        return Status::Error;
    }
    out.payload.resize(size);
    if (size != 0 && std::fread(out.payload.data(), 1, size, file_) != size) {
        Fail(error, fmt::format("truncated: record {} payload needs {} bytes", index_, size));
        return Status::Error;
    }
    position_ += size;
    ++index_;
    return Status::Record;
}

// ---- payload decoding -------------------------------------------------------------------------

bool ParseInit(const Record& record, std::vector<InitBlockData>& blocks, std::string* error) {
    blocks.clear();
    const auto& p = record.payload;
    if (p.size() < 4) {
        return Fail(error, "INIT: payload shorter than the block count");
    }
    const u32 count = Le32(p.data());
    size_t pos = 4;
    for (u32 i = 0; i < count; ++i) {
        if (pos + 8 > p.size()) {
            return Fail(error, fmt::format("INIT: block {} header past the end", i));
        }
        const u32 kind = Le32(p.data() + pos);
        const u32 size = Le32(p.data() + pos + 4);
        pos += 8;
        if (pos + size > p.size()) {
            return Fail(error, fmt::format("INIT: block {} (kind {}) past the end", i, kind));
        }
        blocks.push_back({kind, std::span<const u8>(p.data() + pos, size)});
        pos += size;
    }
    if (pos != p.size()) {
        return Fail(error, fmt::format("INIT: {} trailing bytes", p.size() - pos));
    }
    return true;
}

bool MakeInitState(const Record& record, gpu::InitState& state, std::string* error) {
    std::vector<InitBlockData> blocks;
    if (!ParseInit(record, blocks, error)) {
        return false;
    }
    state = {};
    for (const auto& block : blocks) {
        if (block.kind < 1 || block.kind > gpu::kInitBlockCount) {
            return Fail(error, fmt::format("INIT: unknown block kind {}", block.kind));
        }
        state.blocks[block.kind] = block.data;
    }
    for (u32 kind = 1; kind <= gpu::kInitBlockCount; ++kind) {
        if (state.blocks[kind].size() != InitBlockSize(kind)) {
            return Fail(error, fmt::format("INIT: block kind {} is {} bytes, expected {}", kind,
                                           state.blocks[kind].size(), InitBlockSize(kind)));
        }
    }
    return true;
}

bool ParseMem(const Record& record, MemData& mem, std::string* error) {
    const auto& p = record.payload;
    if (p.size() < 8) {
        return Fail(error, "MEM: payload shorter than its header");
    }
    const u32 size = Le32(p.data() + 4);
    if (p.size() != 8u + size) {
        return Fail(error, fmt::format("MEM: size field {} does not match payload {}", size,
                                       p.size() - 8));
    }
    mem.address = Le32(p.data());
    mem.data = std::span<const u8>(p.data() + 8, size);
    return true;
}

bool ParsePages(const Record& record, std::vector<PageData>& pages, std::string* error) {
    pages.clear();
    const auto& p = record.payload;
    const char* name = RecordName(record.type);
    if (p.size() < 4) {
        return Fail(error, fmt::format("{}: payload shorter than the count", name));
    }
    const u32 count = Le32(p.data());
    if (p.size() != 4 + static_cast<u64>(count) * (4 + kPageSize)) {
        return Fail(error, fmt::format("{}: payload size does not match count {}", name, count));
    }
    pages.reserve(count);
    size_t pos = 4;
    for (u32 i = 0; i < count; ++i) {
        const u32 address = Le32(p.data() + pos);
        if (address % kPageSize != 0) {
            return Fail(error, fmt::format("{}: address 0x{:08x} not page aligned", name, address));
        }
        pages.push_back({address, std::span<const u8, kPageSize>(p.data() + pos + 4, kPageSize)});
        pos += 4 + kPageSize;
    }
    return true;
}

bool ParseGx(const Record& record, gpu::GxCommand& command, std::string* error) {
    if (record.payload.size() != 32) {
        return Fail(error, fmt::format("GX: payload is {} bytes, expected 32", record.payload.size()));
    }
    for (int i = 0; i < 8; ++i) {
        command.words[i] = Le32(record.payload.data() + 4 * i);
    }
    return true;
}

bool ParsePicaReg(const Record& record, std::vector<gpu::RegisterWrite>& writes,
                  std::string* error) {
    writes.clear();
    const auto& p = record.payload;
    if (p.size() < 4) {
        return Fail(error, "PICA_REG: payload shorter than the count");
    }
    const u32 count = Le32(p.data());
    if (p.size() != 4 + 8 * static_cast<u64>(count)) {
        return Fail(error, fmt::format("PICA_REG: payload size does not match count {}", count));
    }
    writes.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const u8* e = p.data() + 4 + 8 * i;
        const u16 reg = Le16(e);
        const u8 mask = e[2];
        if (e[3] != 0) {
            return Fail(error, fmt::format("PICA_REG: entry {} has a nonzero pad byte", i));
        }
        if (mask > 0xF) {
            return Fail(error, fmt::format("PICA_REG: entry {} has byte mask 0x{:02x}", i, mask));
        }
        if (reg >= kNumPicaRegs) {
            return Fail(error,
                        fmt::format("PICA_REG: entry {} register 0x{:03x} out of range", i, reg));
        }
        writes.push_back({reg, mask, Le32(e + 4)});
    }
    return true;
}

bool ParseMmio(const Record& record, MmioData& mmio, std::string* error) {
    if (record.payload.size() != 8) {
        return Fail(error,
                    fmt::format("MMIO: payload is {} bytes, expected 8", record.payload.size()));
    }
    mmio.address = Le32(record.payload.data());
    mmio.value = Le32(record.payload.data() + 4);
    return true;
}

bool ParseSwap(const Record& record, SwapData& swap, std::string* error) {
    if (record.payload.size() != 32) {
        return Fail(error,
                    fmt::format("SWAP: payload is {} bytes, expected 32", record.payload.size()));
    }
    const u8* p = record.payload.data();
    swap.screen = Le32(p);
    swap.info = {Le32(p + 4),  Le32(p + 8),  Le32(p + 12), Le32(p + 16),
                 Le32(p + 20), Le32(p + 24), Le32(p + 28)};
    return true;
}

bool ParseFill(const Record& record, u32& word, std::string* error) {
    if (record.payload.size() != 4) {
        return Fail(error,
                    fmt::format("FILL: payload is {} bytes, expected 4", record.payload.size()));
    }
    word = Le32(record.payload.data());
    return true;
}

bool ParseIrq(const Record& record, IrqData& irq, std::string* error) {
    if (record.payload.size() != 12) {
        return Fail(error,
                    fmt::format("IRQ: payload is {} bytes, expected 12", record.payload.size()));
    }
    irq.id = Le32(record.payload.data());
    irq.delay_ns = Le64(record.payload.data() + 4);
    return true;
}

bool ParseFlush(const Record& record, FlushData& flush, std::string* error) {
    if (record.payload.size() != 12) {
        return Fail(error,
                    fmt::format("FLUSH: payload is {} bytes, expected 12", record.payload.size()));
    }
    flush.kind = Le32(record.payload.data());
    flush.address = Le32(record.payload.data() + 4);
    flush.size = Le32(record.payload.data() + 8);
    return true;
}

bool ParseFrame(const Record& record, u32& frame, std::string* error) {
    if (record.payload.size() != 4) {
        return Fail(error,
                    fmt::format("FRAME: payload is {} bytes, expected 4", record.payload.size()));
    }
    frame = Le32(record.payload.data());
    return true;
}

std::string CheckRecord(const Record& record) {
    std::string error;
    switch (record.type) {
    case kInit: {
        std::vector<InitBlockData> blocks;
        if (!ParseInit(record, blocks, &error)) {
            return error;
        }
        u32 previous = 0;
        for (const auto& block : blocks) {
            if (InitBlockSize(block.kind) == 0) {
                return fmt::format("INIT: unknown block kind {}", block.kind);
            }
            if (block.kind <= previous) {
                return fmt::format("INIT: block kind {} after kind {}; kinds must appear once, in "
                                   "increasing order",
                                   block.kind, previous);
            }
            previous = block.kind;
            if (block.data.size() != InitBlockSize(block.kind)) {
                return fmt::format("INIT: block kind {} is {} bytes, expected {}", block.kind,
                                   block.data.size(), InitBlockSize(block.kind));
            }
        }
        if (blocks.size() != gpu::kInitBlockCount) {
            return fmt::format("INIT: {} blocks, expected {} (every kind once)", blocks.size(),
                               gpu::kInitBlockCount);
        }
        return {};
    }
    case kMem: {
        MemData mem;
        return ParseMem(record, mem, &error) ? std::string() : error;
    }
    case kCpuPages:
    case kGpuPages: {
        std::vector<PageData> pages;
        return ParsePages(record, pages, &error) ? std::string() : error;
    }
    case kGx: {
        gpu::GxCommand command;
        return ParseGx(record, command, &error) ? std::string() : error;
    }
    case kPicaReg: {
        std::vector<gpu::RegisterWrite> writes;
        return ParsePicaReg(record, writes, &error) ? std::string() : error;
    }
    case kMmio: {
        MmioData mmio;
        return ParseMmio(record, mmio, &error) ? std::string() : error;
    }
    case kSwap: {
        SwapData swap;
        return ParseSwap(record, swap, &error) ? std::string() : error;
    }
    case kFill: {
        u32 word;
        return ParseFill(record, word, &error) ? std::string() : error;
    }
    case kIrq: {
        IrqData irq;
        return ParseIrq(record, irq, &error) ? std::string() : error;
    }
    case kFlush: {
        FlushData flush;
        if (!ParseFlush(record, flush, &error)) {
            return error;
        }
        if (flush.kind < kFlushDataCache || flush.kind > kCacheFlushRegion) {
            return fmt::format("FLUSH: unknown kind {}", flush.kind);
        }
        return {};
    }
    case kFrame: {
        u32 frame;
        if (!ParseFrame(record, frame, &error)) {
            return error;
        }
        if (frame != record.frame) {
            return fmt::format("FRAME: payload {} differs from the header frame {}", frame,
                               record.frame);
        }
        return {};
    }
    default:
        return fmt::format("unknown record type {}", record.type);
    }
}

} // namespace mh4n::gpu_capture
