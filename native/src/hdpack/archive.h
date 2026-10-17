// The HD pack archive (N2-17): one file with the converted textures and an index, keyed by
// the hash the pack names them by. N2-18 reads it with ArchiveReader.
//
//   header   64 bytes: "MH4HDPK1", u32 version (1), u32 flags (bit 0: the hashes are
//            use_new_hash hashes), u32 entry count, u32 entry size (40), u64 index offset,
//            u64 file size, u32 CRC-32 of the index, zero up to 64.
//   data     the blobs, in the order they were converted. A blob is the levels of one image,
//            level 0 first, each LevelBytes(format, w >> n, h >> n) long (no padding).
//   index    entry count * 40 bytes, sorted by (hash, map type), at the end of the file:
//              u64 hash, u64 offset, u64 size, u32 width, u32 height, u32 CRC-32 of the blob,
//              u8 map type (0 colour, 1 normal), u8 format (Azahar's CustomPixelFormat: 0 RGBA8,
//              1 BC1, 2 BC3, 3 BC5, 4 BC7), u8 mip count, u8 zero.
//            Several entries (hashes, maps) may share one blob.
// All numbers are little-endian.
#pragma once

#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "hdpack/image.h"
#include "hdpack/pack.h"
#include "mh4n/types.h"

namespace mh4n::hdpack {

inline constexpr std::size_t kArchiveHeaderSize = 64;
inline constexpr std::size_t kArchiveEntrySize = 40;
inline constexpr u32 kArchiveVersion = 1;
inline constexpr u32 kArchiveFlagNewHash = 1;

struct ArchiveEntry {
    u64 hash = 0;
    u64 offset = 0;
    u64 size = 0;
    u32 width = 0;
    u32 height = 0;
    u32 crc = 0;
    MapType type = MapType::Color;
    PixelFormat format = PixelFormat::BC7;
    u8 mip_count = 1;
};

// The bytes of an entry's level `level` are at [LevelOffset(entry, level), + the level's size).
u64 LevelOffset(const ArchiveEntry& entry, u32 level);
// The size of one level of an entry.
u64 LevelSize(const ArchiveEntry& entry, u32 level);

// Writes an archive. The data goes to the file right away, so an interrupted run can continue:
// Begin with the size the data had when the last blob was complete. The index is written by
// Finish; until then the header says there is none and ArchiveReader refuses the file.
class ArchiveWriter {
public:
    ~ArchiveWriter();
    // `resume_size` 0 starts a new file; otherwise the file is cut to that size (it must be at
    // least the header's size) and appended to.
    bool Begin(const std::string& path, bool use_new_hash, u64 resume_size, std::string& error);
    // Appends a blob; returns its offset.
    std::optional<u64> Append(const u8* data, std::size_t size, std::string& error);
    // Flushes to the operating system.
    bool Flush(std::string& error);
    // The size of the data so far (header included).
    u64 size() const {
        return size_;
    }
    bool Finish(std::vector<ArchiveEntry> entries, std::string& error);

private:
    std::fstream file_;
    bool use_new_hash_ = false;
    u64 size_ = 0;
};

class ArchiveReader {
public:
    bool Open(const std::string& path, std::string& error);
    bool use_new_hash() const {
        return (flags_ & kArchiveFlagNewHash) != 0;
    }
    const std::vector<ArchiveEntry>& entries() const {
        return entries_;
    }
    u64 file_size() const {
        return file_size_;
    }
    const ArchiveEntry* Find(u64 hash, MapType type = MapType::Color) const;
    // Reads a whole blob / one level, checking nothing but the bounds (Verify checks the CRC).
    bool ReadBlob(const ArchiveEntry& entry, std::vector<u8>& out, std::string& error) const;
    bool ReadLevel(const ArchiveEntry& entry, u32 level, std::vector<u8>& out,
                   std::string& error) const;
    bool Verify(const ArchiveEntry& entry, std::string& error) const;

private:
    std::string path_;
    u32 flags_ = 0;
    u64 file_size_ = 0;
    std::vector<ArchiveEntry> entries_;
};

} // namespace mh4n::hdpack
