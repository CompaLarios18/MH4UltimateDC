// Derived from Azahar src/core/hle/service/ir/ir_user.cpp and src/core/hle/service/ir/ir_user.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later
// version"): the header of the ir:USER shared memory, the buffer manager of the receive buffer
// (BufferInfo, PacketInfo, Put, Release) and the state the service keeps between sessions.
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (ir_user.cpp)
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (ir_user.h)
//
// The state of ir:USER (N1-16), kept in the service context under the key "ir:USER" because the
// service has a single session at a time but its events and shared memory outlive a session (Azahar's
// IR_USER object is one for the whole run). Included as "services/ir/ir_state.h" (the capture of
// the HID module reads the shared memory from it).
#pragma once

#include <memory>
#include <span>

#include "mh4n/kernel.h"
#include "mh4n/service.h"
#include "mh4n/types.h"

namespace mh4n::ir {

// "This is a header that will present in the ir:USER shared memory if it is initialized with
// InitializeIrNopShared service function."
struct SharedMemoryHeader {
    u32 latest_receive_error_result;
    u32 latest_send_error_result;
    u8 connection_status;
    u8 trying_to_connect_status;
    u8 connection_role;
    u8 machine_id;
    u8 connected;
    u8 network_id;
    u8 initialized;
    u8 unknown;
};
static_assert(sizeof(SharedMemoryHeader) == 16);
inline constexpr u32 kConnectionStatusOffset = 8;
inline constexpr u32 kTryingToConnectStatusOffset = 9;
inline constexpr u32 kConnectionRoleOffset = 10;
inline constexpr u32 kConnectedOffset = 12;
inline constexpr u32 kInitializedOffset = 14;

// The receive buffer: BufferInfo (the queue's indices and count) at `info_offset`, a circular queue
// of PacketInfo {offset, size} at `buffer_offset`, and after it the circular data buffer.
//
// Azahar's manager trusts the sizes the game gives it. This one checks every access against the
// size of the shared memory (an access outside it fails the operation) and does not divide by a
// zero packet count.
class BufferManager {
public:
    BufferManager(std::shared_ptr<SharedMemory> shared_memory, u32 info_offset, u32 buffer_offset,
                  u32 max_packet_count, u32 buffer_size);

    // Puts a packet at the head of the buffer. False if the queue or the data buffer is full.
    bool Put(std::span<const u8> packet);
    // Releases packets from the tail of the buffer. False if fewer are queued.
    bool Release(u32 count);

    struct BufferInfo {
        u32 begin_index;
        u32 end_index;
        u32 packet_count;
        u32 unknown;
    };
    static_assert(sizeof(BufferInfo) == 16);
    const BufferInfo& Info() const { return info_; }

private:
    struct PacketInfo {
        u32 offset;
        u32 size;
    };
    static_assert(sizeof(PacketInfo) == 8);

    u8* Pointer(u64 offset, u64 size) const;
    bool GetPacketInfo(u32 index, PacketInfo* out) const;
    bool SetPacketInfo(u32 index, const PacketInfo& packet_info) const;
    u8* DataPointer(u64 offset, u64 size) const;
    void UpdateBufferInfo() const;

    BufferInfo info_{0, 0, 0, 0};
    std::shared_ptr<SharedMemory> shared_memory_;
    u32 info_offset_;
    u32 buffer_offset_;
    u32 max_packet_count_;
    u32 max_data_size_;
};

class Shared {
public:
    void Ensure(ServiceContext& context);

    std::shared_ptr<Event> connection_status_event;
    std::shared_ptr<Event> send_event;
    std::shared_ptr<Event> receive_event;
    // The block the game gave InitializeIrNopShared (empty before, and after FinalizeIrNop).
    std::shared_ptr<SharedMemory> shared_memory;
    std::unique_ptr<BufferManager> receive_buffer;
    // The first block any InitializeIrNopShared brought: --capture-hid keeps writing it (as the
    // reference does) even after Finalize.
    std::shared_ptr<SharedMemory> capture_memory;
    // Always false for now: see RequireConnection.
    bool connected_device = false;

private:
    bool initialised_ = false;
};

std::shared_ptr<Shared> GetShared(ServiceContext& context);

}  // namespace mh4n::ir
