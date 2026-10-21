// Derived from Azahar src/core/hle/service/ir/ir_user.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): BufferManager's Put and Release (see
// the header next to this file).
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project
#include <cstring>

#include "services/ir/ir_state.h"

namespace mh4n::ir {

BufferManager::BufferManager(std::shared_ptr<SharedMemory> shared_memory, u32 info_offset,
                             u32 buffer_offset, u32 max_packet_count, u32 buffer_size)
    : shared_memory_(std::move(shared_memory)),
      info_offset_(info_offset),
      buffer_offset_(buffer_offset),
      max_packet_count_(max_packet_count),
      max_data_size_(buffer_size - static_cast<u32>(sizeof(PacketInfo)) * max_packet_count) {
    UpdateBufferInfo();
}

u8* BufferManager::Pointer(u64 offset, u64 size) const {
    if (!shared_memory_ || offset + size > shared_memory_->Size()) {
        return nullptr;
    }
    return shared_memory_->GetPointer(static_cast<u32>(offset));
}

bool BufferManager::GetPacketInfo(u32 index, PacketInfo* out) const {
    const u8* pointer =
        Pointer(u64{buffer_offset_} + sizeof(PacketInfo) * u64{index}, sizeof(PacketInfo));
    if (pointer == nullptr) {
        return false;
    }
    std::memcpy(out, pointer, sizeof(PacketInfo));
    return true;
}

bool BufferManager::SetPacketInfo(u32 index, const PacketInfo& packet_info) const {
    u8* pointer =
        Pointer(u64{buffer_offset_} + sizeof(PacketInfo) * u64{index}, sizeof(PacketInfo));
    if (pointer == nullptr) {
        return false;
    }
    std::memcpy(pointer, &packet_info, sizeof(PacketInfo));
    return true;
}

u8* BufferManager::DataPointer(u64 offset, u64 size) const {
    return Pointer(u64{buffer_offset_} + sizeof(PacketInfo) * u64{max_packet_count_} + offset, size);
}

void BufferManager::UpdateBufferInfo() const {
    if (info_offset_ == 0) {
        return;
    }
    if (u8* pointer = Pointer(info_offset_, sizeof(BufferInfo))) {
        std::memcpy(pointer, &info_, sizeof(BufferInfo));
    }
}

bool BufferManager::Put(std::span<const u8> packet) {
    if (info_.packet_count == max_packet_count_) {
        return false;
    }

    u32 write_offset;

    // finds free space offset in data buffer
    if (info_.packet_count == 0) {
        write_offset = 0;
        if (packet.size() > max_data_size_)
            return false;
    } else {
        const u32 last_index = (info_.end_index + max_packet_count_ - 1) % max_packet_count_;
        PacketInfo first, last;
        if (!GetPacketInfo(info_.begin_index, &first) || !GetPacketInfo(last_index, &last) ||
            max_data_size_ == 0) {
            return false;
        }
        write_offset = (last.offset + last.size) % max_data_size_;
        const u32 free_space = (first.offset + max_data_size_ - write_offset) % max_data_size_;
        if (packet.size() > free_space)
            return false;
    }

    // writes packet info
    const PacketInfo packet_info{write_offset, static_cast<u32>(packet.size())};
    if (!SetPacketInfo(info_.end_index, packet_info)) {
        return false;
    }

    // writes packet data, byte by byte around the end of the circular buffer
    for (std::size_t i = 0; i < packet.size(); ++i) {
        u8* destination = DataPointer((write_offset + i) % max_data_size_, 1);
        if (destination == nullptr) {
            return false;
        }
        *destination = packet[i];
    }

    // updates buffer info
    info_.end_index++;
    info_.end_index %= max_packet_count_;
    info_.packet_count++;
    UpdateBufferInfo();
    return true;
}

bool BufferManager::Release(u32 count) {
    if (info_.packet_count < count) {
        return false;
    }
    if (max_packet_count_ == 0) {
        return count == 0;  // Azahar divides by zero below
    }

    info_.packet_count -= count;
    info_.begin_index += count;
    info_.begin_index %= max_packet_count_;
    UpdateBufferInfo();
    return true;
}

}  // namespace mh4n::ir
