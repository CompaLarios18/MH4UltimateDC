// Derived from Azahar src/core/hle/ipc.h, src/core/hle/ipc_helpers.h, src/core/hle/kernel/hle_ipc.h,
// src/core/hle/kernel/hle_ipc.cpp and src/core/hle/kernel/errors.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the layout of the command header and of the translate descriptors (DescriptorType, Header,
// the *Desc functions, GetDescriptorType), the size of the command area, the shape of the request
// parser and reply builder (RequestParser, RequestBuilder) and of the translation (the HLE handle and
// mapped buffer protocols of HLERequestContext), and the result code of a bad descriptor.
// Copyright 2016 Citra Emulator Project (ipc.h)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (ipc_helpers.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle_ipc.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (hle_ipc.cpp)
// Copyright 2017 Citra Emulator Project (errors.h)
//
// IPC (N1-10): what a service sees of a request, and what it builds as a reply. The kernel side (ports,
// sessions, SendSyncRequest) is `ServiceManager` in service.h; the services are `Service` there.
//
// A request is the command buffer of the calling thread (thread-local storage + 0x80, 0x40 words), with
// the 3DS's translation applied:
//  - handles in a handle descriptor are looked up in the client's table; a moved handle is closed in it;
//    the service gets the objects (`PopObject`), and the word of the command holds an index, as in
//    Azahar's HLE handle protocol (a handle descriptor can carry several handles: the count is in the
//    descriptor, `ipc::HandleNumberFromDesc`);
//  - a static buffer is copied out of the client at once (guest address, size and bytes in
//    `StaticBuffer`), so a reply that comes later sees what was sent;
//  - a mapped buffer stays in the client's memory, behind `MappedBuffer`, which checks the range and the
//    permission on every access;
//  - the calling PID descriptor is filled in.
// The words as the client wrote them are kept (`UntranslatedWords`).
//
// A service reads with the cursor (`Pop`, `PopRaw`, `PopObject`, `PopPid`, `PopStaticBuffer`,
// `PopMappedBuffer`) and answers with `MakeReply(normal, translate)`:
//
//   Handled Fs::GetSize(Request& request) {
//       const u64 size = request.Pop<u64>();
//       Reply& reply = request.MakeReply(3, 0);
//       reply.Push(kResultSuccess);
//       reply.Push(size);
//       return Handled::Done;
//   }
//
// The reply is written into the client's command buffer when the handler returns. A handler that has
// to wait returns Handled::Deferred, keeps `request.Share()` and later calls `Complete(request, reply)`
// from any host thread; the reply is written on the scheduler thread and the client resumes. A reply
// can be built away from the request (`Reply(command_id, normal, translate)`).
//
// Departures from Azahar, all deliberate:
//  - A command that is too long, a descriptor that is cut off or not known and a handle that is not in
//    the client's table make svcSendSyncRequest fail (kResultInvalidBufferDescriptor,
//    kResultInvalidHandleInCommand); Azahar asserts.
//  - Every handle of a move descriptor is closed in the client. Azahar compares the whole descriptor
//    word with MoveHandle, so a descriptor with more than one handle leaves them open.
//  - A static receive buffer that is smaller than the data is written short (and logged); Azahar asserts.
//  - A reply handle that cannot be created in the client's table (it is full) is logged and the client
//    reads the handle 0 with the reply's own result; Azahar asserts.
//  - A calling-PID descriptor in a reply is filled in with the client's process id; Azahar's
//    WriteToOutgoingCommandBuffer has no case for it (UNIMPLEMENTED_MSG).
//  - The reply has its own storage, so a service can read the request after starting the reply; the
//    bytes above a sub-word value are zero.
//  - A handler that returns without a reply gets the reply of an unimplemented command, and it is logged.
//
// Threads (principle 6): a Request, its Reply and the cursor belong to the scheduler thread. The
// exceptions are `MappedBuffer::Read/Write` (they touch only the client's range lent by the request, so
// an I/O thread can fill it while the client waits), `Complete`, `Finished` and `Cancelled`.
//
// What a request holds of the kernel (the client thread, the process, the client session, the objects
// the client moved or copied in, the objects of a reply) is released on the scheduler thread, by the
// IPC layer, when the request ends: after the reply is written (immediate or deferred), when the reply
// is dropped because the client stopped waiting, and when the client thread ends (`Cancel`). After
// that, `ClientThreadPtr()`, `Session()`, `IncomingObjects()` and the objects of `Descriptors()` are
// empty, `ClientThread()` must not be called, and `HasReply()` is false. So a copy kept with `Share()`
// can be dropped on any host thread once the request has finished; a service that drops its copy
// without ever completing it leaves the client waiting and the release to whoever drops the last one.
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/types.h"

namespace mh4n {

class Request;
class Service;
class ServerSession;

// The command buffers are little-endian words in guest memory, and the host copies them as they are.
static_assert(std::endian::native == std::endian::little, "IPC words are copied as little-endian");

// ---- result codes (Azahar src/core/hle/kernel/errors.h; the values are its comments') ---------------

inline constexpr Result kResultInvalidBufferDescriptor = 0xD9001830;
inline constexpr Result kResultSessionClosed = 0xC920181A;
inline constexpr Result kResultMaxConnectionsReached =
    0xD0401834;                                           // a port with all its sessions open
inline constexpr Result kResultUnknownPort = 0xD88007FA;  // svcConnectToPort, a name nobody has
// A handle inside a command that is not in the client's table (Azahar's ResultInvalidHandleOs, "returned
// instead of ResultInvalidHandle in some code paths"). svcSendSyncRequest keeps kResultInvalidHandle
// (0xD8E007F7) for a session handle that is not a client session, which leaves no IPC trace record; a
// request that failed here has one (see the IPC trace), so the two are told apart by the result.
inline constexpr Result kResultInvalidHandleInCommand = 0xD9001BF7;
static_assert(kResultInvalidBufferDescriptor == MakeResult(48, 6, 8, 27));
static_assert(kResultSessionClosed == MakeResult(26, 6, 9, 25));
static_assert(kResultMaxConnectionsReached == MakeResult(52, 6, 2, 26));
static_assert(kResultUnknownPort == MakeResult(1018, 1, 4, 27));
static_assert(kResultInvalidHandleInCommand == MakeResult(1015, 6, 8, 27));

namespace ipc {

// Size of the command buffer, in words; the static buffer receive descriptors (two words each, the
// descriptor and the address) follow it: 0x40 + 2 * 16 words, TLS + 0x80 .. TLS + 0x200.
inline constexpr std::size_t kCommandBufferLength = 0x100 / sizeof(u32);
inline constexpr std::size_t kMaxStaticBuffers = 16;
inline constexpr std::size_t kCommandAreaLength = kCommandBufferLength + 2 * kMaxStaticBuffers;

// The words of a thread's static receive buffer `id`, as offsets from its command buffer.
constexpr std::size_t StaticBufferDescriptorWord(std::size_t id) {
    return kCommandBufferLength + 2 * id;
}

enum DescriptorType : u32 {
    // Buffer descriptors (mask 0x0F)
    StaticBuffer = 0x02,
    PXIBuffer = 0x04,
    MappedBuffer = 0x08,
    // Handle descriptors (mask 0x30, after the buffer types have been ruled out)
    CopyHandle = 0x00,
    MoveHandle = 0x10,
    CallingPid = 0x20,
};

// A command header: translate size in bits 0-5, normal size in bits 6-11, command id in bits 16-31.
struct Header {
    u32 raw = 0;
    constexpr u32 TranslateParamsSize() const { return raw & 0x3F; }
    constexpr u32 NormalParamsSize() const { return (raw >> 6) & 0x3F; }
    constexpr u32 CommandId() const { return raw >> 16; }
};

// `normal_params_size` and `translate_params_size` are in words (up to 63 each); the translate size
// counts the descriptors.
constexpr u32 MakeHeader(u16 command_id, u32 normal_params_size, u32 translate_params_size) {
    return (translate_params_size & 0x3Fu) | ((normal_params_size & 0x3Fu) << 6) |
           (static_cast<u32>(command_id) << 16);
}

constexpr u32 MoveHandleDesc(u32 num_handles = 1) {
    return MoveHandle | ((num_handles - 1) << 26);
}
constexpr u32 CopyHandleDesc(u32 num_handles = 1) {
    return CopyHandle | ((num_handles - 1) << 26);
}
constexpr u32 CallingPidDesc() {
    return CallingPid;
}
constexpr bool IsHandleDescriptor(u32 descriptor) {
    return (descriptor & 0xF) == 0x0;
}
constexpr u32 HandleNumberFromDesc(u32 handle_descriptor) {
    return (handle_descriptor >> 26) + 1;
}

// Static buffer: type in bits 0-3, buffer id in bits 10-13, size (18 bits) from bit 14.
constexpr u32 StaticBufferDesc(std::size_t size, u8 buffer_id) {
    return StaticBuffer | ((static_cast<u32>(buffer_id) & 0xFu) << 10) |
           ((static_cast<u32>(size) & 0x3FFFFu) << 14);
}
constexpr u32 StaticBufferDescId(u32 descriptor) {
    return (descriptor >> 10) & 0xF;
}
constexpr u32 StaticBufferDescSize(u32 descriptor) {
    return descriptor >> 14;
}

// PXI buffer: type 4 (| 2 for a read-only one), buffer id in bits 4-7, size from bit 8. The next word
// is the physical address of a table.
constexpr u32 PXIBufferDesc(u32 size, unsigned buffer_id, bool is_read_only) {
    u32 type = PXIBuffer;
    if (is_read_only) {
        type |= 0x2;
    }
    return type | (size << 8) | ((buffer_id & 0xF) << 4);
}
constexpr u32 PXIBufferDescSize(u32 descriptor) {
    return descriptor >> 8;
}

enum MappedBufferPermissions : u32 {
    R = 1,
    W = 2,
    RW = R | W,
};

// Mapped buffer: type 8 in bits 0-3, the permissions in bits 1-2, size (28 bits) from bit 4.
constexpr u32 MappedBufferDesc(std::size_t size, MappedBufferPermissions perms) {
    return MappedBuffer | (static_cast<u32>(perms) << 1) |
           ((static_cast<u32>(size) & 0xFFFFFFFu) << 4);
}
constexpr MappedBufferPermissions MappedBufferDescPerms(u32 descriptor) {
    return static_cast<MappedBufferPermissions>((descriptor >> 1) & 3);
}
constexpr u32 MappedBufferDescSize(u32 descriptor) {
    return descriptor >> 4;
}

// The checks must be done in this order. A handle descriptor whose bits 4-5 are both set has no type
// in the enumeration (0x30).
constexpr DescriptorType GetDescriptorType(u32 descriptor) {
    if (IsHandleDescriptor(descriptor)) {
        return static_cast<DescriptorType>(descriptor & 0x30);
    }
    if (descriptor & MappedBuffer) {
        return MappedBuffer;
    }
    if (descriptor & PXIBuffer) {
        return PXIBuffer;
    }
    return StaticBuffer;
}

// One translate descriptor of a request, in the order they appear.
struct Descriptor {
    DescriptorType type = CopyHandle;
    u32 word = 0;     // the descriptor as the client wrote it
    u32 index = 0;    // its position in the command (the header is word 0)
    u32 size = 0;     // handles: how many; buffers: bytes; the calling PID: 1
    u32 address = 0;  // buffers: the word after the descriptor (the client's address)
    // Handle descriptors: what the client's handles were (null for the handle 0).
    std::vector<std::shared_ptr<Object>> objects;
    // Mapped buffers: the permissions and the index in Request::MappedBuffers.
    MappedBufferPermissions perms = R;
    u32 mapped_id = 0;
};

}  // namespace ipc

// ---- buffers -----------------------------------------------------------------------------------------

// A buffer of the request that stayed in the client's memory. Read and Write check the permission the
// client gave, the range against the buffer, and that the client's memory is mapped there; they return
// false (and log) otherwise. They touch nothing but that range of guest memory, so an I/O thread may call
// them while the client waits for the reply.
class MappedBuffer {
public:
    MappedBuffer(GuestMemory& memory, u32 descriptor, u32 address, u32 id);

    u32 Id() const { return id_; }
    u32 Address() const { return address_; }
    u32 Size() const { return size_; }
    ipc::MappedBufferPermissions Perms() const { return perms_; }
    bool Readable() const { return (perms_ & ipc::R) != 0; }
    bool Writable() const { return (perms_ & ipc::W) != 0; }
    // The descriptor a reply pushes to give the buffer back.
    u32 GenerateDescriptor() const { return ipc::MappedBufferDesc(size_, perms_); }

    bool Read(void* dst, u32 offset, u32 size) const;
    bool Write(const void* src, u32 offset, u32 size) const;

private:
    GuestMemory* memory_;
    u32 id_;
    u32 address_;
    u32 size_;
    ipc::MappedBufferPermissions perms_;
};

// A static buffer of the request: where it was in the client and its bytes (zeros if the client's memory
// could not be read).
struct StaticBuffer {
    u32 id = 0;
    u32 address = 0;
    u32 size = 0;
    std::vector<u8> data;
};

// ---- the reply ---------------------------------------------------------------------------------------

// What a service answers. The sizes are fixed when it is made (as in Azahar's RequestBuilder); the words
// are pushed in order, normal parameters first. Pushing more or fewer words than declared is logged.
class Reply {
public:
    // The reply to command `command_id`, `normal_params_size` words of normal parameters (the result
    // code is the first) and `translate_params_size` words of translate parameters, descriptors
    // included.
    Reply(u16 command_id, u32 normal_params_size, u32 translate_params_size);
    Reply(Reply&&) noexcept = default;
    Reply& operator=(Reply&&) noexcept = default;
    Reply(const Reply&) = delete;
    Reply& operator=(const Reply&) = delete;

    // Integers (a word each, or two for 64 bits; 8 and 16 bits take a whole word, the rest zero),
    // booleans (one word), floats, doubles, enumerations of up to 32 bits and Result.
    template <class T> void Push(T value) {
        if constexpr (std::is_enum_v<T>) {
            static_assert(sizeof(T) <= sizeof(u32), "64-bit enumerations cannot be pushed");
            Push(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            PushWord(value ? 1u : 0u);
        } else if constexpr (std::is_floating_point_v<T>) {
            if constexpr (sizeof(T) == 4) {
                PushWord(std::bit_cast<u32>(value));
            } else {
                Push(std::bit_cast<u64>(value));
            }
        } else {
            static_assert(std::is_integral_v<T>,
                          "Push takes integers, booleans, floats and enumerations");
            if constexpr (sizeof(T) == 8) {
                PushWord(static_cast<u32>(static_cast<u64>(value)));
                PushWord(static_cast<u32>(static_cast<u64>(value) >> 32));
            } else {
                PushWord(static_cast<u32>(static_cast<std::make_unsigned_t<T>>(value)));
            }
        }
    }
    template <class First, class... Other> void Push(const First& first, const Other&... other) {
        Push(first);
        (Push(other), ...);
    }

    // The bytes of a trivially copyable structure, rounded up to whole words.
    template <class T> void PushRaw(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "raw values are copied");
        PushBytes(&value, sizeof(T));
    }

    // Handles: one descriptor for all the objects given. HLE services do not tell copy from move.
    template <class... O> void PushCopyObjects(std::shared_ptr<O>... objects) {
        PushHandles(ipc::CopyHandleDesc(sizeof...(O)),
                    {std::shared_ptr<Object>(std::move(objects))...});
    }
    template <class... O> void PushMoveObjects(std::shared_ptr<O>... objects) {
        PushHandles(ipc::MoveHandleDesc(sizeof...(O)),
                    {std::shared_ptr<Object>(std::move(objects))...});
    }

    // A static buffer: copied into the receive buffer the client described at TLS + 0x180 + 8 * id when
    // the reply is written; the address word is filled in then.
    void PushStaticBuffer(std::vector<u8> buffer, u8 buffer_id);
    // Gives a mapped buffer of the request back (descriptor and id).
    void PushMappedBuffer(const MappedBuffer& buffer);
    // The calling PID descriptor and the PID, filled in when the reply is written.
    void PushPid();

    // ---- for the IPC layer ----
    u32 HeaderWord() const { return words_[0]; }
    u32 CommandSize() const {
        return 1 + ipc::Header{words_[0]}.NormalParamsSize() +
               ipc::Header{words_[0]}.TranslateParamsSize();
    }
    // Logs a mismatch between the declared size and the words pushed; true if they agree.
    bool Validate() const;
    const std::array<u32, ipc::kCommandBufferLength>& Words() const { return words_; }
    const std::vector<std::shared_ptr<Object>>& Objects() const { return objects_; }
    const std::optional<std::vector<u8>>& StaticData(u32 id) const { return static_buffers_[id]; }

private:
    friend class Request;
    void PushWord(u32 word);
    void PushBytes(const void* data, std::size_t size);
    void PushHandles(u32 descriptor, std::vector<std::shared_ptr<Object>> objects);

    std::array<u32, ipc::kCommandBufferLength> words_{};
    std::size_t index_ = 1;
    bool overflow_ = false;
    std::vector<std::shared_ptr<Object>> objects_;  // the words of handles hold indices into this
    std::array<std::optional<std::vector<u8>>, ipc::kMaxStaticBuffers> static_buffers_;
};

// ---- the sessions -----------------------------------------------------------------------------------

// The server side of a session: one Service per session. The client side is a ClientSession in the
// application's handle table; a request in flight keeps the server side alive.
class ServerSession {
public:
    ~ServerSession();
    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    Service* GetService() const { return service_.get(); }
    bool Closed() const { return service_ == nullptr; }
    // The service's name for a session that came out of a port; the name given to a session a service
    // handed out.
    const std::string& Name() const { return name_; }

private:
    friend class ServiceManager;
    struct Port;
    ServerSession() = default;
    // Destroys the service (the client closed its last handle, or the manager is going).
    void Close();

    std::unique_ptr<Service> service_;
    std::shared_ptr<Port> port_;  // null for sessions services hand out
    std::string name_;
};

// The client side: what svcConnectToPort and GetServiceHandle give the application.
class ClientSession final : public Object {
public:
    static constexpr HandleType kHandleType = HandleType::ClientSession;
    ClientSession(Kernel& kernel, std::shared_ptr<ServerSession> server, std::string port_name);
    HandleType GetHandleType() const override { return kHandleType; }
    const char* TypeName() const override { return "ClientSession"; }

    const std::shared_ptr<ServerSession>& Server() const { return server_; }
    // The port or service name this session came out of; empty for a session a service handed out.
    const std::string& PortName() const { return port_name_; }

private:
    std::shared_ptr<ServerSession> server_;
    std::string port_name_;
};

// ---- the request ---------------------------------------------------------------------------------------

// What a handler returns: the reply is ready (or none is needed, see MakeReply), or it comes later
// through Complete.
enum class Handled : u8 { Done, Deferred };

class Request : public std::enable_shared_from_this<Request> {
public:
    // Made by ServiceManager for a request of `thread` through `session`; Translate fills it in.
    Request(Kernel& kernel, std::shared_ptr<Thread> thread, std::shared_ptr<ClientSession> session,
            u32 sequence);
    ~Request();
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    // The translation described at the top of this file, from the thread's command area as read from
    // guest memory (ipc::kCommandAreaLength words). The error codes are the result of svcSendSyncRequest.
    Result Translate(std::span<const u32> area);

    // ---- who ----
    u32 Sequence() const { return sequence_; }  // 1 for the first request of the run
    // The client thread; only until the request ends (see the top of this file).
    Thread& ClientThread() const { return *thread_; }
    u32 ClientThreadId() const { return thread_id_; }  // valid for ever
    const std::shared_ptr<Thread>& ClientThreadPtr() const { return thread_; }
    // The client session the request came through; null once the request has ended.
    const std::shared_ptr<ClientSession>& Session() const { return session_; }
    // The process id the calling PID descriptor carries.
    u32 CallingPid() const { return pid_; }
    GuestMemory& Memory() const { return kernel_.Memory(); }
    Kernel& GetKernel() const { return kernel_; }

    // ---- the command ----
    u32 Header() const { return cmd_[0]; }
    u16 CommandId() const { return static_cast<u16>(ipc::Header{cmd_[0]}.CommandId()); }
    u32 NormalParamsSize() const { return ipc::Header{cmd_[0]}.NormalParamsSize(); }
    u32 TranslateParamsSize() const { return ipc::Header{cmd_[0]}.TranslateParamsSize(); }
    // The words as the client wrote them (header, normal parameters, translate parameters), and as the
    // service sees them (handles and mapped buffers replaced by indices, the PID filled in).
    std::span<const u32> UntranslatedWords() const { return {untranslated_.data(), command_size_}; }
    std::span<const u32> Words() const { return {cmd_.data(), command_size_}; }
    const std::vector<ipc::Descriptor>& Descriptors() const { return descriptors_; }
    const std::vector<std::shared_ptr<Object>>& IncomingObjects() const { return incoming_; }
    const std::vector<MappedBuffer>& MappedBuffers() const { return mapped_; }
    const std::vector<StaticBuffer>& StaticBuffers() const { return static_in_; }

    // ---- the cursor (it starts after the header) ----
    void Rewind() { cursor_ = 1; }
    std::size_t Cursor() const { return cursor_; }
    // The next word as a T (the types Reply::Push takes); a word past the command reads as 0.
    template <class T> T Pop() {
        if constexpr (std::is_enum_v<T>) {
            return static_cast<T>(Pop<std::underlying_type_t<T>>());
        } else if constexpr (std::is_same_v<T, bool>) {
            return (NextWord() & 0xFF) != 0;
        } else if constexpr (std::is_floating_point_v<T>) {
            if constexpr (sizeof(T) == 4) {
                return std::bit_cast<T>(NextWord());
            } else {
                return std::bit_cast<T>(Pop<u64>());
            }
        } else {
            static_assert(std::is_integral_v<T>,
                          "Pop returns integers, booleans, floats and enumerations");
            if constexpr (sizeof(T) == 8) {
                const u64 low = NextWord();
                const u64 high = NextWord();
                return static_cast<T>(high << 32 | low);
            } else {
                return static_cast<T>(NextWord());
            }
        }
    }
    // The bytes of a trivially copyable structure, from whole words.
    template <class T> T PopRaw() {
        static_assert(std::is_trivially_copyable_v<T>, "raw values are copied");
        T value;
        PopBytes(&value, sizeof(T));
        return value;
    }
    // One descriptor with exactly N handles, resolved to the client's objects (null for the handle 0).
    // Anything else at the cursor is logged and gives nulls.
    template <std::size_t N> std::array<std::shared_ptr<Object>, N> PopGenericObjects() {
        std::array<std::shared_ptr<Object>, N> out;
        const std::vector<std::shared_ptr<Object>> got = PopHandleDescriptor(N);
        for (std::size_t i = 0; i < N && i < got.size(); ++i) {
            out[i] = got[i];
        }
        return out;
    }
    std::shared_ptr<Object> PopGenericObject() { return PopGenericObjects<1>()[0]; }
    // The object if it is a T, else null.
    template <class T> std::shared_ptr<T> PopObject() { return ObjectCast<T>(PopGenericObject()); }
    template <class... T> std::tuple<std::shared_ptr<T>...> PopObjects() {
        auto objects = PopGenericObjects<sizeof...(T)>();
        return PopObjectsHelper<T...>(objects, std::index_sequence_for<T...>{});
    }
    // The calling PID descriptor and the PID; 0 (and a log) if the next word is not the descriptor.
    u32 PopPid();
    // A static buffer descriptor and its address word; null (and a log) if the next word is not one.
    const StaticBuffer* PopStaticBuffer();
    // A mapped buffer descriptor and its id; null (and a log) if the next word is not one.
    const MappedBuffer* PopMappedBuffer();
    // The object a handle word of the translated command stands for.
    std::shared_ptr<Object> GetIncomingHandle(u32 id) const;

    // ---- the reply ----
    // Starts the reply to this request (the same command id) and returns it. Call it once; a second call
    // starts over.
    Reply& MakeReply(u32 normal_params_size, u32 translate_params_size);
    bool HasReply() const { return reply_.has_value(); }
    Reply* GetReply() { return reply_ ? &*reply_ : nullptr; }

    // For Handled::Deferred.
    std::shared_ptr<Request> Share() { return shared_from_this(); }
    // True once the reply has been delivered, or dropped because the client had stopped waiting for it
    // (`Complete` ran). A client thread that ended before the reply came makes `Cancelled()` true, but
    // not `Finished()`, until `Complete` has run. Any thread.
    bool Finished() const { return finished_; }

    // ---- for the IPC layer ----
    // Writes `reply` into the client thread's command buffer and receive buffers as the 3DS's
    // translation does, and returns the words the client reads (the command part). Scheduler thread.
    std::vector<u32> WriteReply(const Reply& reply);
    void MarkFinished() { finished_ = true; }
    // The client thread ended while the request waited (Thread::Stop): a late Complete does nothing.
    // Scheduler thread; releases what the request holds (ReleaseKernelObjects).
    void Cancel();
    bool Cancelled() const { return cancelled_; }
    // Lets go of the client thread, process and session, the incoming objects and the reply, and of the
    // replied hook: the destructors that run are the scheduler thread's. Scheduler thread; the IPC layer
    // calls it when the request ends.
    void ReleaseKernelObjects();
    // Called after the reply is written, with the words the client reads.
    using RepliedHook = std::function<void(const Request&, std::span<const u32> words)>;
    void SetRepliedHook(RepliedHook hook) { replied_hook_ = std::move(hook); }
    // The scheduler-thread half of Complete: writes the reply, tells the hook and resumes the client.
    void Deliver(Reply reply);

private:
    friend class ServiceManager;
    template <class... T, std::size_t... I>
    static std::tuple<std::shared_ptr<T>...>
    PopObjectsHelper(std::array<std::shared_ptr<Object>, sizeof...(T)>& objects,
                     std::index_sequence<I...>) {
        return std::make_tuple(ObjectCast<T>(std::move(objects[I]))...);
    }
    u32 NextWord();
    void PopBytes(void* dst, std::size_t size);
    std::vector<std::shared_ptr<Object>> PopHandleDescriptor(std::size_t expected);
    u32 AddIncoming(std::shared_ptr<Object> object);

    Kernel& kernel_;
    std::shared_ptr<Thread> thread_;
    std::shared_ptr<Process> process_;
    std::shared_ptr<ClientSession> session_;
    u32 thread_id_ = 0;
    u32 sequence_;
    u32 pid_ = 0;
    std::array<u32, ipc::kCommandBufferLength> untranslated_{};
    std::array<u32, ipc::kCommandBufferLength> cmd_{};
    std::size_t command_size_ = 1;
    std::vector<ipc::Descriptor> descriptors_;
    std::vector<std::shared_ptr<Object>> incoming_;
    std::vector<StaticBuffer> static_in_;
    std::vector<MappedBuffer> mapped_;
    std::size_t cursor_ = 1;
    bool cursor_warned_ = false;
    std::optional<Reply> reply_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> cancelled_{false};
    RepliedHook replied_hook_;
};

// Delivers the reply of a deferred request. Callable from any host thread: the reply is written on the
// scheduler thread (Kernel::Post), the client thread resumes, and the observers see it. Does nothing
// (and logs) if the request already has its reply, and nothing if the client thread ended meanwhile.
void Complete(const std::shared_ptr<Request>& request, Reply reply);

}  // namespace mh4n
