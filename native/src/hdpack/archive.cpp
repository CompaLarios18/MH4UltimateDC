#include "hdpack/archive.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include <fmt/format.h>

#include "hdpack/bytes.h"
#include "mh4n/crc32.h"

namespace mh4n::hdpack {

namespace {

void MakeHeader(u8* h, u32 flags, u32 count, u64 index_offset, u64 file_size, u32 index_crc) {
    std::memset(h, 0, kArchiveHeaderSize);
    std::memcpy(h, "MH4HDPK1", 8);
    Put32(h + 8, kArchiveVersion);
    Put32(h + 12, flags);
    Put32(h + 16, count);
    Put32(h + 20, static_cast<u32>(kArchiveEntrySize));
    Put64(h + 24, index_offset);
    Put64(h + 32, file_size);
    Put32(h + 40, index_crc);
}

void PutEntry(u8* p, const ArchiveEntry& e) {
    Put64(p, e.hash);
    Put64(p + 8, e.offset);
    Put64(p + 16, e.size);
    Put32(p + 24, e.width);
    Put32(p + 28, e.height);
    Put32(p + 32, e.crc);
    p[36] = static_cast<u8>(e.type);
    p[37] = static_cast<u8>(e.format);
    p[38] = e.mip_count;
    p[39] = 0;
}

ArchiveEntry GetEntry(const u8* p) {
    ArchiveEntry e;
    e.hash = Get64(p);
    e.offset = Get64(p + 8);
    e.size = Get64(p + 16);
    e.width = Get32(p + 24);
    e.height = Get32(p + 28);
    e.crc = Get32(p + 32);
    e.type = static_cast<MapType>(p[36]);
    e.format = static_cast<PixelFormat>(p[37]);
    e.mip_count = p[38];
    return e;
}

} // namespace

u64 LevelSize(const ArchiveEntry& entry, u32 level) {
    return LevelBytes(entry.format, std::max(1u, entry.width >> level),
                      std::max(1u, entry.height >> level));
}

u64 LevelOffset(const ArchiveEntry& entry, u32 level) {
    u64 offset = entry.offset;
    for (u32 i = 0; i < level; ++i) {
        offset += LevelSize(entry, i);
    }
    return offset;
}

ArchiveWriter::~ArchiveWriter() = default;

bool ArchiveWriter::Begin(const std::string& path, bool use_new_hash, u64 resume_size,
                          std::string& error) {
    use_new_hash_ = use_new_hash;
    file_.close();
    file_.clear();
    if (resume_size == 0) {
        file_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!file_) {
            error = path + ": cannot create";
            return false;
        }
        u8 header[kArchiveHeaderSize];
        // index offset 0: "no index yet"
        MakeHeader(header, use_new_hash ? kArchiveFlagNewHash : 0, 0, 0, 0, 0);
        file_.write(reinterpret_cast<const char*>(header), sizeof(header));
        size_ = kArchiveHeaderSize;
    } else {
        std::error_code ec;
        if (resume_size < kArchiveHeaderSize || std::filesystem::file_size(path, ec) < resume_size) {
            error = path + ": the partial file is shorter than the journal says";
            return false;
        }
        std::filesystem::resize_file(path, resume_size, ec);
        if (ec) {
            error = path + ": " + ec.message();
            return false;
        }
        file_.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!file_) {
            error = path + ": cannot open";
            return false;
        }
        file_.seekp(static_cast<std::streamoff>(resume_size));
        size_ = resume_size;
    }
    if (!file_) {
        error = path + ": write failed";
        return false;
    }
    return true;
}

std::optional<u64> ArchiveWriter::Append(const u8* data, std::size_t size, std::string& error) {
    const u64 offset = size_;
    file_.seekp(static_cast<std::streamoff>(offset));
    file_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file_) {
        error = "writing the archive failed (disk full?)";
        return std::nullopt;
    }
    size_ += size;
    return offset;
}

bool ArchiveWriter::Flush(std::string& error) {
    file_.flush();
    if (!file_) {
        error = "writing the archive failed (disk full?)";
        return false;
    }
    return true;
}

bool ArchiveWriter::Finish(std::vector<ArchiveEntry> entries, std::string& error) {
    std::sort(entries.begin(), entries.end(), [](const ArchiveEntry& a, const ArchiveEntry& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.type < b.type;
    });
    std::vector<u8> index(entries.size() * kArchiveEntrySize);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        PutEntry(index.data() + i * kArchiveEntrySize, entries[i]);
    }
    const u64 index_offset = size_;
    file_.seekp(static_cast<std::streamoff>(index_offset));
    file_.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
    const u64 file_size = index_offset + index.size();
    u8 header[kArchiveHeaderSize];
    MakeHeader(header, use_new_hash_ ? kArchiveFlagNewHash : 0, static_cast<u32>(entries.size()),
               index_offset, file_size, Crc32(index.data(), index.size()));
    file_.seekp(0);
    file_.write(reinterpret_cast<const char*>(header), sizeof(header));
    file_.flush();
    if (!file_) {
        error = "writing the archive's index failed (disk full?)";
        return false;
    }
    file_.close();
    size_ = file_size;
    return true;
}

bool ArchiveReader::Open(const std::string& path, std::string& error) {
    path_ = path;
    entries_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot open";
        return false;
    }
    u8 header[kArchiveHeaderSize];
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || std::memcmp(header, "MH4HDPK1", 8) != 0) {
        error = path + ": not an HD pack archive";
        return false;
    }
    if (Get32(header + 8) != kArchiveVersion || Get32(header + 20) != kArchiveEntrySize) {
        error = path + ": unsupported archive version";
        return false;
    }
    flags_ = Get32(header + 12);
    const u32 count = Get32(header + 16);
    const u64 index_offset = Get64(header + 24);
    file_size_ = Get64(header + 32);
    std::error_code ec;
    if (index_offset < kArchiveHeaderSize || file_size_ != std::filesystem::file_size(path, ec) ||
        index_offset + static_cast<u64>(count) * kArchiveEntrySize != file_size_) {
        error = path + ": incomplete archive (no index, or the size does not match)";
        return false;
    }
    std::vector<u8> index(static_cast<std::size_t>(count) * kArchiveEntrySize);
    in.seekg(static_cast<std::streamoff>(index_offset));
    in.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index.size()));
    if (!in || Crc32(index.data(), index.size()) != Get32(header + 40)) {
        error = path + ": the index is damaged";
        return false;
    }
    entries_.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        ArchiveEntry e = GetEntry(index.data() + static_cast<std::size_t>(i) * kArchiveEntrySize);
        // Every field is range-checked before LevelOffset runs on it (a shift by 32 or more is
        // undefined, and offset + size could wrap), so a damaged index with a valid CRC is refused.
        const bool sane =
            e.width != 0 && e.height != 0 && e.width <= kMaxDimension &&
            e.height <= kMaxDimension && e.mip_count != 0 &&
            e.mip_count <= FullMipCount(e.width, e.height) && static_cast<u8>(e.format) <= 4 &&
            static_cast<u8>(e.type) <= 1 && e.offset >= kArchiveHeaderSize &&
            e.offset <= index_offset && e.size <= index_offset - e.offset;
        if (!sane || LevelOffset(e, e.mip_count) - e.offset != e.size) {
            error = fmt::format("{}: entry {} is inconsistent", path, i);
            return false;
        }
        entries_.push_back(e);
    }
    return true;
}

const ArchiveEntry* ArchiveReader::Find(u64 hash, MapType type) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), std::make_pair(hash, type),
                               [](const ArchiveEntry& e, const std::pair<u64, MapType>& key) {
                                   return e.hash != key.first ? e.hash < key.first : e.type < key.second;
                               });
    if (it == entries_.end() || it->hash != hash || it->type != type) {
        return nullptr;
    }
    return &*it;
}

namespace {
bool ReadRange(const std::string& path, u64 offset, u64 size, std::vector<u8>& out,
               std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot open";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!in) {
        error = path + ": short read";
        return false;
    }
    return true;
}
} // namespace

bool ArchiveReader::ReadBlob(const ArchiveEntry& entry, std::vector<u8>& out,
                             std::string& error) const {
    return ReadRange(path_, entry.offset, entry.size, out, error);
}

bool ArchiveReader::ReadLevel(const ArchiveEntry& entry, u32 level, std::vector<u8>& out,
                              std::string& error) const {
    if (level >= entry.mip_count) {
        error = "no such level";
        return false;
    }
    return ReadRange(path_, LevelOffset(entry, level), LevelSize(entry, level), out, error);
}

bool ArchiveReader::Verify(const ArchiveEntry& entry, std::string& error) const {
    std::vector<u8> blob;
    if (!ReadBlob(entry, blob, error)) {
        return false;
    }
    if (Crc32(blob.data(), blob.size()) != entry.crc) {
        error = fmt::format("blob of {:016X}: CRC mismatch", entry.hash);
        return false;
    }
    return true;
}

} // namespace mh4n::hdpack
