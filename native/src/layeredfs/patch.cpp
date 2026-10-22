// Derived from Azahar src/core/file_sys/patch.cpp and src/core/file_sys/patch.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later version").
// Copyright 2019-2026 Citra Emulator Project / Azahar Emulator Project (patch.cpp, patch.h)
#include "patch.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>

#include "mh4n/crc32.h"
#include "mh4n/log.h"

namespace mh4n::layeredfs::patch {

namespace {
constexpr const char* kLog = "Service.FS";
}

Status ApplyIps(const std::vector<u8>& ips, std::vector<u8>& buffer) {
    // Azahar builds the header string from 5 bytes without looking at the size.
    if (ips.size() < 5 || std::memcmp(ips.data(), "PATCH", 5) != 0) {
        MH4N_LOG(Info, kLog, "Attempted to load invalid IPS");
        return Status::ErrorPatches;
    }

    std::size_t cursor = 5;
    const std::size_t patch_length = ips.size() - 3;

    while (cursor < patch_length) {
        if (std::memcmp(&ips[cursor], "EOF", 3) == 0) {
            break;
        }

        // A record needs its 5-byte header (Azahar reads whatever follows the patch).
        if (cursor + 5 > ips.size()) {
            return Status::ErrorPatches;
        }
        const std::size_t offset = static_cast<std::size_t>(ips[cursor]) << 16 |
                                   static_cast<std::size_t>(ips[cursor + 1]) << 8 | ips[cursor + 2];
        std::size_t length = static_cast<std::size_t>(ips[cursor + 3]) << 8 | ips[cursor + 4];

        // check for an rle record
        if (length == 0) {
            if (cursor + 8 > ips.size()) {
                return Status::ErrorPatches;
            }
            length = static_cast<std::size_t>(ips[cursor + 5]) << 8 | ips[cursor + 6];

            if (buffer.size() < offset + length) {
                return Status::ErrorPatches;
            }

            for (std::size_t i = 0; i < length; ++i) {
                buffer[offset + i] = ips[cursor + 7];
            }

            cursor += 8;
            continue;
        }

        if (buffer.size() < offset + length) {
            return Status::ErrorPatches;
        }
        if (cursor + 5 + length > ips.size()) {
            return Status::ErrorPatches;  // the record's bytes run past the patch
        }
        std::memcpy(&buffer[offset], &ips[cursor + 5], length);
        cursor += length + 5;
    }
    return Status::Success;
}

namespace {

// The BPS format uses variable length encoding for all integers.
// Realistically uint32s are more than enough for code patching.
using Number = u32;

constexpr std::size_t kMagicSize = 4;
constexpr std::size_t kFooterSize = 12;

// Utility class to make keeping track of offsets and bound checks less error prone.
template <typename T>
class Stream {
public:
    Stream(T* ptr, std::size_t size) : ptr_{ptr}, size_{size} {}

    bool Read(void* buffer, std::size_t length) {
        if (offset_ + length > size_) {
            return false;
        }
        std::memcpy(buffer, ptr_ + offset_, length);
        offset_ += length;
        return true;
    }

    template <typename OtherType>
    bool CopyFrom(Stream<OtherType>& other, std::size_t length) {
        if (offset_ + length > size_) {
            return false;
        }
        if (!other.Read(ptr_ + offset_, length)) {
            return false;
        }
        offset_ += length;
        return true;
    }

    template <typename ValueType>
    std::optional<ValueType> Read() {
        static_assert(std::is_trivial_v<ValueType>);
        ValueType val{};
        if (!Read(&val, sizeof(val))) {
            return std::nullopt;
        }
        return val;
    }

    Number ReadNumber() {
        Number data = 0, shift = 1;
        std::optional<u8> x;
        while ((x = Read<u8>())) {
            data += (*x & 0x7f) * shift;
            if (*x & 0x80) {
                break;
            }
            shift <<= 7;
            data += shift;
        }
        return data;
    }

    T* data() const { return ptr_; }
    std::size_t size() const { return size_; }
    std::size_t Tell() const { return offset_; }

    bool Seek(std::size_t offset) {
        if (offset > size_) {
            return false;
        }
        offset_ = offset;
        return true;
    }

private:
    T* ptr_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
};

class PatchApplier {
public:
    PatchApplier(Stream<const u8> source, Stream<u8> target, Stream<const u8> patch)
        : source_{source}, target_{target}, patch_{patch} {}

    Status Apply() {
        // Azahar dereferences the empty optional of a patch shorter than this.
        if (patch_.size() < kMagicSize + kFooterSize) {
            MH4N_LOG(Error, kLog, "Invalid BPS magic");
            return Status::ErrorPatches;
        }
        const auto magic = *patch_.Read<std::array<char, kMagicSize>>();
        if (std::string_view(magic.data(), magic.size()) != "BPS1") {
            MH4N_LOG(Error, kLog, "Invalid BPS magic");
            return Status::ErrorPatches;
        }

        const Number source_size = patch_.ReadNumber();
        const Number target_size = patch_.ReadNumber();
        const Number metadata_size = patch_.ReadNumber();
        if (source_size > source_.size() || target_size > target_.size() || metadata_size != 0) {
            MH4N_LOG(Error, kLog, "Invalid sizes");
            return Status::ErrorPatches;
        }

        const std::size_t command_start_offset = patch_.Tell();
        const std::size_t command_end_offset = patch_.size() - kFooterSize;
        patch_.Seek(command_end_offset);
        const u32 source_crc32 = *patch_.Read<u32>();
        const u32 target_crc32 = *patch_.Read<u32>();
        patch_.Seek(command_start_offset);

        if (Crc32(source_.data(), source_size) != source_crc32) {
            MH4N_LOG(Error, kLog, "Unexpected source hash");
            return Status::ErrorPatchesInvalidTitle;
        }

        // Process all patch commands.
        std::memset(target_.data(), 0, target_.size());
        while (patch_.Tell() < command_end_offset) {
            if (!HandleCommand()) {
                return Status::ErrorPatches;
            }
        }

        if (Crc32(target_.data(), target_size) != target_crc32) {
            MH4N_LOG(Error, kLog, "Unexpected target hash");
            return Status::ErrorPatches;
        }

        return Status::Success;
    }

private:
    bool HandleCommand() {
        const std::size_t offset = patch_.Tell();
        const Number data = patch_.ReadNumber();
        const Number command = data & 3;
        const Number length = (data >> 2) + 1;

        const bool ok = [&] {
            switch (command) {
            case 0:
                return SourceRead(length);
            case 1:
                return TargetRead(length);
            case 2:
                return SourceCopy(length);
            case 3:
                return TargetCopy(length);
            default:
                return false;
            }
        }();
        if (!ok) {
            MH4N_LOG(Error, kLog, "Failed to process command {} at 0x{:x}", command, offset);
        }
        return ok;
    }

    bool SourceRead(Number length) { return source_.Seek(target_.Tell()) && target_.CopyFrom(source_, length); }

    bool TargetRead(Number length) { return target_.CopyFrom(patch_, length); }

    bool SourceCopy(Number length) {
        const Number data = patch_.ReadNumber();
        source_relative_offset_ += (data & 1 ? -1 : +1) * static_cast<int>(data >> 1);
        if (!source_.Seek(source_relative_offset_) || !target_.CopyFrom(source_, length)) {
            return false;
        }
        source_relative_offset_ += length;
        return true;
    }

    bool TargetCopy(Number length) {
        const Number data = patch_.ReadNumber();
        target_relative_offset_ += (data & 1 ? -1 : +1) * static_cast<int>(data >> 1);
        if (target_.Tell() + length > target_.size()) {
            return false;
        }
        // Azahar's check alone lets an offset that wrapped below zero through.
        if (target_relative_offset_ >= target_.size() || target_relative_offset_ + length > target_.size()) {
            return false;
        }
        // Byte by byte copy.
        for (std::size_t i = 0; i < length; ++i) {
            target_.data()[target_.Tell() + i] = target_.data()[target_relative_offset_++];
        }
        target_.Seek(target_.Tell() + length);
        return true;
    }

    std::size_t source_relative_offset_ = 0;
    std::size_t target_relative_offset_ = 0;
    Stream<const u8> source_;
    Stream<u8> target_;
    Stream<const u8> patch_;
};

}  // namespace

Status ApplyBps(const std::vector<u8>& bps, std::vector<u8>& buffer) {
    Stream<const u8> patch_stream{bps.data(), bps.size()};

    // Move the offset past the file format marker (i.e. "BPS1")
    patch_stream.Seek(kMagicSize);

    const Number source_size = patch_stream.ReadNumber();
    const Number target_size = patch_stream.ReadNumber();

    if (target_size > source_size) {
        MH4N_LOG(Info, kLog, "Resizing target to {}", target_size);
        buffer.resize(target_size);
    }

    patch_stream.Seek(0);

    const std::vector<u8> source = buffer;
    Stream<const u8> source_stream{source.data(), source.size()};
    Stream<u8> target_stream{buffer.data(), buffer.size()};
    PatchApplier applier{source_stream, target_stream, patch_stream};
    return applier.Apply();
}

}  // namespace mh4n::layeredfs::patch
