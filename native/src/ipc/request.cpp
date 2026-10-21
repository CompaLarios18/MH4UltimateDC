// Derived from Azahar src/core/hle/kernel/hle_ipc.cpp, src/core/hle/ipc_helpers.h and src/core/hle/ipc.h
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): HLERequestContext::PopulateFromIncomingCommandBuffer and WriteToOutgoingCommandBuffer (the
// translation of handles, static buffers, mapped buffers and the calling PID), MappedBuffer, and the
// cursor and builder helpers of RequestParser and RequestBuilder.
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (hle_ipc.cpp)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (ipc_helpers.h)
// Copyright 2016 Citra Emulator Project (ipc.h)
//
// Requests and replies (N1-10). See mh4n/ipc.h.
#include "mh4n/ipc.h"

#include <algorithm>
#include <cstring>

#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "mh4n/threads.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Kernel.IPC";

}  // namespace

// ---- MappedBuffer ------------------------------------------------------------------------------------------

MappedBuffer::MappedBuffer(GuestMemory& memory, u32 descriptor, u32 address, u32 id)
    : memory_(&memory), id_(id), address_(address), size_(ipc::MappedBufferDescSize(descriptor)),
      perms_(ipc::MappedBufferDescPerms(descriptor)) {}

bool MappedBuffer::Read(void* dst, u32 offset, u32 size) const {
    if (!Readable()) {
        MH4N_LOG(Warning, kLog,
                 "read of mapped buffer {} (0x{:08X}, {} bytes), which the client gave no read "
                 "permission",
                 id_, address_, size_);
        return false;
    }
    if (static_cast<u64>(offset) + size > size_) {
        MH4N_LOG(Warning, kLog,
                 "read of {} bytes at +0x{:X} is outside mapped buffer {} ({} bytes)", size, offset,
                 id_, size_);
        return false;
    }
    if (!memory_->ReadBlock(dst, address_ + offset, size)) {
        MH4N_LOG(Warning, kLog, "mapped buffer {} (0x{:08X}) is not mapped in the client", id_,
                 address_);
        return false;
    }
    return true;
}

bool MappedBuffer::Write(const void* src, u32 offset, u32 size) const {
    if (!Writable()) {
        MH4N_LOG(Warning, kLog,
                 "write to mapped buffer {} (0x{:08X}, {} bytes), which the client gave no write "
                 "permission",
                 id_, address_, size_);
        return false;
    }
    if (static_cast<u64>(offset) + size > size_) {
        MH4N_LOG(Warning, kLog,
                 "write of {} bytes at +0x{:X} is outside mapped buffer {} ({} bytes)", size,
                 offset, id_, size_);
        return false;
    }
    if (!memory_->WriteBlock(address_ + offset, src, size)) {
        MH4N_LOG(Warning, kLog, "mapped buffer {} (0x{:08X}) is not mapped in the client", id_,
                 address_);
        return false;
    }
    return true;
}

// ---- Reply ---------------------------------------------------------------------------------------------------

Reply::Reply(u16 command_id, u32 normal_params_size, u32 translate_params_size) {
    words_[0] = ipc::MakeHeader(command_id, normal_params_size, translate_params_size);
    if (CommandSize() > ipc::kCommandBufferLength) {
        MH4N_LOG(Error, kLog, "reply 0x{:04X} declares {} words; a command buffer has {}",
                 command_id, CommandSize(), ipc::kCommandBufferLength);
    }
}

void Reply::PushWord(u32 word) {
    if (index_ >= ipc::kCommandBufferLength) {
        if (!overflow_) {
            MH4N_LOG(Error, kLog, "reply 0x{:08X}: more than {} words pushed", words_[0],
                     ipc::kCommandBufferLength);
        }
        overflow_ = true;
        return;
    }
    words_[index_] = word;
    written_[index_] = 0xFFFFFFFFu;
    ++index_;
}

void Reply::PushPartial(u32 value, std::size_t size) {
    if (index_ >= ipc::kCommandBufferLength) {
        PushWord(0);  // logs the overflow
        return;
    }
    // D53: only `size` bytes are written; the others keep what the word holds (the request's bytes).
    const u32 mask = size >= 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1u;
    words_[index_] = (words_[index_] & ~mask) | (value & mask);
    written_[index_] |= mask;
    ++index_;
}

void Reply::PushBytes(const void* data, std::size_t size) {
    const std::size_t count = (size + 3) / 4;
    if (index_ + count > ipc::kCommandBufferLength) {
        if (!overflow_) {
            MH4N_LOG(Error, kLog, "reply 0x{:08X}: more than {} words pushed", words_[0],
                     ipc::kCommandBufferLength);
        }
        overflow_ = true;
        return;
    }
    // Only `size` bytes are written (Azahar memcpys sizeof(T)): the rest of the last word keeps what it
    // holds, the request's bytes once seeded (D53).
    std::memcpy(&words_[index_], data, size);
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t in_word = std::min<std::size_t>(size - 4 * k, 4);
        written_[index_ + k] |= in_word >= 4 ? 0xFFFFFFFFu : (1u << (8 * in_word)) - 1u;
    }
    index_ += count;
}

std::array<u32, ipc::kCommandBufferLength>
Reply::SeededWords(std::span<const u32> request_words) const {
    std::array<u32, ipc::kCommandBufferLength> words = words_;
    if (seeded_) {
        return words;
    }
    // The normal parameters of the reply, at the indices where the request had a word; the header is
    // the reply's own and the translate parameters are never touched.
    const std::size_t normal = ipc::Header{words_[0]}.NormalParamsSize();
    const std::size_t last = std::min({normal + 1, request_words.size(), ipc::kCommandBufferLength});
    for (std::size_t i = 1; i < last; ++i) {
        words[i] = (words_[i] & written_[i]) | (request_words[i] & ~written_[i]);
    }
    return words;
}

void Reply::SeedFrom(std::span<const u32> request_words) {
    if (seeded_) {
        return;
    }
    words_ = SeededWords(request_words);
    seeded_ = true;
}

void Reply::PushHandles(u32 descriptor, std::vector<std::shared_ptr<Object>> objects) {
    PushWord(descriptor);
    for (std::shared_ptr<Object>& object : objects) {
        PushWord(static_cast<u32>(objects_.size()));
        objects_.push_back(std::move(object));
    }
}

void Reply::PushStaticBuffer(std::vector<u8> buffer, u8 buffer_id) {
    if (buffer_id >= ipc::kMaxStaticBuffers) {
        MH4N_LOG(Error, kLog, "static buffer id {} is out of range", buffer_id);
        return;
    }
    PushWord(ipc::StaticBufferDesc(buffer.size(), buffer_id));
    // replaced by the address of the client's receive buffer when the reply is written
    PushWord(0xDEADC0DE);
    static_buffers_[buffer_id] = std::move(buffer);
}

void Reply::PushMappedBuffer(const MappedBuffer& buffer) {
    PushWord(buffer.GenerateDescriptor());
    PushWord(buffer.Id());
}

void Reply::PushPid() {
    PushWord(ipc::CallingPidDesc());
    PushWord(0);  // the client's process id, filled in when the reply is written
}

bool Reply::Validate() const {
    const std::size_t expected = CommandSize();
    if (overflow_ || index_ != expected) {
        MH4N_LOG(Error, kLog, "reply 0x{:08X} declares {} words and {} were pushed", words_[0],
                 expected, index_);
        return false;
    }
    return true;
}

// ---- Request: the translation --------------------------------------------------------------------------------

Request::Request(Kernel& kernel, std::shared_ptr<Thread> thread,
                 std::shared_ptr<ClientSession> session, u32 sequence)
    : kernel_(kernel), thread_(std::move(thread)), session_(std::move(session)),
      sequence_(sequence) {
    thread_id_ = thread_->thread_id;
    process_ = thread_->owner_process.lock();
    if (!process_) {
        process_ = kernel_.CurrentProcess();
    }
    if (process_) {
        pid_ = process_->process_id;
    }
}

Request::~Request() = default;

void Request::ReleaseKernelObjects() {
    // Everything goes into locals that die at the end of this function, on the calling (scheduler)
    // thread, whoever drops the last copy of the request afterwards. The objects can be the last
    // reference to a session, whose destructor destroys a service, or to a thread.
    std::vector<std::shared_ptr<Object>> objects = std::move(incoming_);
    incoming_.clear();
    for (ipc::Descriptor& descriptor : descriptors_) {
        for (std::shared_ptr<Object>& object : descriptor.objects) {
            objects.push_back(std::move(object));
        }
        descriptor.objects.clear();
    }
    std::optional<Reply> reply = std::move(reply_);
    reply_.reset();
    RepliedHook hook = std::move(replied_hook_);
    replied_hook_ = nullptr;
    std::shared_ptr<ClientSession> session = std::move(session_);
    session_.reset();
    std::shared_ptr<Process> process = std::move(process_);
    process_.reset();
    std::shared_ptr<Thread> thread = std::move(thread_);
    thread_.reset();
}

void Request::Cancel() {
    cancelled_ = true;
    ReleaseKernelObjects();
}

u32 Request::AddIncoming(std::shared_ptr<Object> object) {
    incoming_.push_back(std::move(object));
    return static_cast<u32>(incoming_.size() - 1);
}

std::shared_ptr<Object> Request::GetIncomingHandle(u32 id) const {
    if (id >= incoming_.size()) {
        MH4N_LOG(Error, kLog, "request 0x{:08X} asks for object {}, of {}", Header(), id,
                 incoming_.size());
        return nullptr;
    }
    return incoming_[id];
}

Result Request::Translate(std::span<const u32> area) {
    MH4N_ASSERT(process_ != nullptr, "a request without a process");
    MH4N_ASSERT(area.size() >= ipc::kCommandBufferLength, "a command area of {} words",
                area.size());
    std::copy_n(area.begin(), ipc::kCommandBufferLength, untranslated_.begin());
    const ipc::Header header{area[0]};
    const std::size_t untranslated_size = 1u + header.NormalParamsSize();
    const std::size_t command_size = untranslated_size + header.TranslateParamsSize();
    if (command_size > ipc::kCommandBufferLength) {
        MH4N_LOG(Error, kLog, "request 0x{:08X} is {} words long; a command buffer has {}",
                 header.raw, command_size, ipc::kCommandBufferLength);
        return kResultInvalidBufferDescriptor;
    }
    command_size_ = command_size;
    cmd_ = {};
    std::copy_n(area.begin(), untranslated_size, cmd_.begin());

    std::vector<Handle> to_close;
    std::size_t i = untranslated_size;
    while (i < command_size) {
        const u32 descriptor = cmd_[i] = area[i];
        ipc::Descriptor info;
        info.word = descriptor;
        info.index = static_cast<u32>(i);
        ++i;
        info.type = ipc::GetDescriptorType(descriptor);
        switch (info.type) {
        case ipc::CopyHandle:
        case ipc::MoveHandle: {
            const u32 count = ipc::HandleNumberFromDesc(descriptor);
            if (i + count > command_size) {
                MH4N_LOG(Error, kLog,
                         "request 0x{:08X}: the handle descriptor at word {} is cut off",
                         header.raw, info.index);
                return kResultInvalidBufferDescriptor;
            }
            info.size = count;
            for (u32 j = 0; j < count; ++j) {
                const Handle handle = area[i];
                std::shared_ptr<Object> object;
                if (handle != 0) {
                    object = process_->handle_table.GetGeneric(handle);
                    if (!object) {
                        MH4N_LOG(Error, kLog,
                                 "request 0x{:08X}: handle 0x{:08X} is not in the client's table",
                                 header.raw, handle);
                        return kResultInvalidHandleInCommand;
                    }
                    if (info.type == ipc::MoveHandle) {
                        to_close.push_back(handle);
                    }
                }
                info.objects.push_back(object);
                cmd_[i++] = AddIncoming(std::move(object));
            }
            break;
        }
        case ipc::CallingPid: {
            if (i + 1 > command_size) {
                MH4N_LOG(Error, kLog, "request 0x{:08X}: the calling PID descriptor is cut off",
                         header.raw);
                return kResultInvalidBufferDescriptor;
            }
            info.size = 1;
            cmd_[i++] = pid_;
            break;
        }
        case ipc::StaticBuffer: {
            if (i + 1 > command_size) {
                MH4N_LOG(Error, kLog, "request 0x{:08X}: the static buffer descriptor is cut off",
                         header.raw);
                return kResultInvalidBufferDescriptor;
            }
            const u32 address = area[i];
            info.address = address;
            info.size = ipc::StaticBufferDescSize(descriptor);
            StaticBuffer buffer;
            buffer.id = ipc::StaticBufferDescId(descriptor);
            buffer.address = address;
            buffer.size = info.size;
            buffer.data.assign(info.size, 0);
            if (!kernel_.Memory().ReadBlock(buffer.data.data(), address, info.size)) {
                MH4N_LOG(Warning, kLog,
                         "request 0x{:08X}: static buffer {} (0x{:08X}, {} bytes) is not mapped",
                         header.raw, buffer.id, address, info.size);
            }
            static_in_.push_back(std::move(buffer));
            cmd_[i++] = address;
            break;
        }
        case ipc::MappedBuffer: {
            if (i + 1 > command_size) {
                MH4N_LOG(Error, kLog, "request 0x{:08X}: the mapped buffer descriptor is cut off",
                         header.raw);
                return kResultInvalidBufferDescriptor;
            }
            info.address = area[i];
            info.size = ipc::MappedBufferDescSize(descriptor);
            info.perms = ipc::MappedBufferDescPerms(descriptor);
            info.mapped_id = static_cast<u32>(mapped_.size());
            mapped_.emplace_back(kernel_.Memory(), descriptor, area[i], info.mapped_id);
            cmd_[i++] = info.mapped_id;
            break;
        }
        case ipc::PXIBuffer: {
            // Nothing serves these (Azahar's translation does not know them): the words pass through.
            if (i + 1 > command_size) {
                MH4N_LOG(Error, kLog, "request 0x{:08X}: the PXI buffer descriptor is cut off",
                         header.raw);
                return kResultInvalidBufferDescriptor;
            }
            info.address = area[i];
            info.size = ipc::PXIBufferDescSize(descriptor);
            cmd_[i] = area[i];
            ++i;
            break;
        }
        default:
            MH4N_LOG(Error, kLog, "request 0x{:08X}: unsupported descriptor 0x{:08X} at word {}",
                     header.raw, descriptor, info.index);
            return kResultInvalidBufferDescriptor;
        }
        descriptors_.push_back(std::move(info));
    }
    // The client's moved handles go once the whole command is known to be good.
    for (const Handle handle : to_close) {
        process_->handle_table.Close(handle);
    }
    return kResultSuccess;
}

// ---- Request: the cursor ---------------------------------------------------------------------------------------

u32 Request::NextWord() {
    if (cursor_ >= ipc::kCommandBufferLength) {
        if (!cursor_warned_) {
            MH4N_LOG(Error, kLog, "request 0x{:08X}: read past the command buffer", Header());
            cursor_warned_ = true;
        }
        return 0;
    }
    return cmd_[cursor_++];
}

void Request::PopBytes(void* dst, std::size_t size) {
    const std::size_t count = (size + 3) / 4;
    std::vector<u32> words(count);
    for (std::size_t k = 0; k < count; ++k) {
        words[k] = NextWord();
    }
    std::memcpy(dst, words.data(), size);
}

std::vector<std::shared_ptr<Object>> Request::PopHandleDescriptor(std::size_t expected) {
    const u32 descriptor = NextWord();
    const ipc::DescriptorType type = ipc::GetDescriptorType(descriptor);
    if (type != ipc::CopyHandle && type != ipc::MoveHandle) {
        MH4N_LOG(Error, kLog, "request 0x{:08X}: expected a handle descriptor, found 0x{:08X}",
                 Header(), descriptor);
        return {};
    }
    const u32 count = ipc::HandleNumberFromDesc(descriptor);
    std::vector<std::shared_ptr<Object>> objects;
    for (u32 j = 0; j < count; ++j) {
        objects.push_back(GetIncomingHandle(NextWord()));
    }
    if (count != expected) {
        MH4N_LOG(Error, kLog,
                 "request 0x{:08X}: the handle descriptor 0x{:08X} carries {} handles, {} expected",
                 Header(), descriptor, count, expected);
        return {};
    }
    return objects;
}

u32 Request::PopPid() {
    const u32 descriptor = NextWord();
    if (descriptor != ipc::CallingPidDesc()) {
        MH4N_LOG(Error, kLog,
                 "request 0x{:08X}: expected the calling PID descriptor, found 0x{:08X}", Header(),
                 descriptor);
        return 0;
    }
    return NextWord();
}

const StaticBuffer* Request::PopStaticBuffer() {
    const u32 descriptor = NextWord();
    NextWord();  // the address, which the translation left as it was
    if (ipc::GetDescriptorType(descriptor) != ipc::StaticBuffer) {
        MH4N_LOG(Error, kLog,
                 "request 0x{:08X}: expected a static buffer descriptor, found 0x{:08X}", Header(),
                 descriptor);
        return nullptr;
    }
    const u32 id = ipc::StaticBufferDescId(descriptor);
    for (const StaticBuffer& buffer : static_in_) {
        if (buffer.id == id) {
            return &buffer;
        }
    }
    MH4N_LOG(Error, kLog, "request 0x{:08X}: no static buffer {}", Header(), id);
    return nullptr;
}

const MappedBuffer* Request::PopMappedBuffer() {
    const u32 descriptor = NextWord();
    const u32 id = NextWord();
    if (ipc::GetDescriptorType(descriptor) != ipc::MappedBuffer || id >= mapped_.size()) {
        MH4N_LOG(Error, kLog,
                 "request 0x{:08X}: expected a mapped buffer descriptor, found 0x{:08X} and {}",
                 Header(), descriptor, id);
        return nullptr;
    }
    return &mapped_[id];
}

// ---- Request: the reply ----------------------------------------------------------------------------------------

Reply& Request::MakeReply(u32 normal_params_size, u32 translate_params_size) {
    if (cursor_ < command_size_) {
        MH4N_LOG(Debug, kLog, "request 0x{:08X}: the reply starts with {} of {} words read",
                 Header(), cursor_, command_size_);
    }
    reply_.emplace(NewReply(normal_params_size, translate_params_size));
    return *reply_;
}

Reply Request::NewReply(u32 normal_params_size, u32 translate_params_size) const {
    Reply reply(CommandId(), normal_params_size, translate_params_size);
    // D53: the bytes the service does not write are the request's (the translated words, as Azahar's
    // command buffer holds them).
    reply.SeedFrom(Words());
    return reply;
}

std::vector<u32> Request::WriteReply(const Reply& reply) {
    reply.Validate();
    // D53: a reply that was not made for this request (a Reply built by hand) is seeded here.
    const std::array<u32, ipc::kCommandBufferLength> words = reply.SeededWords(Words());
    const std::size_t command_size =
        std::min<std::size_t>(reply.CommandSize(), ipc::kCommandBufferLength);
    const ipc::Header header{words[0]};
    const std::size_t untranslated_size =
        std::min<std::size_t>(1u + header.NormalParamsSize(), command_size);

    // The client's static receive buffers are described after its command buffer.
    const u32 base = CommandBufferAddress(*thread_);
    std::array<u32, ipc::kCommandAreaLength> area{};
    GuestMemory& memory = kernel_.Memory();
    if (!memory.ReadBlock(area.data(), base, static_cast<u32>(area.size() * sizeof(u32)))) {
        MH4N_LOG(Error, kLog, "thread {}'s command buffer (0x{:08X}) is not mapped",
                 thread_->thread_id, base);
    }

    std::vector<u32> out(command_size, 0);
    std::copy_n(words.begin(), untranslated_size, out.begin());
    std::size_t i = untranslated_size;
    while (i < command_size) {
        const u32 descriptor = out[i] = words[i];
        ++i;
        switch (ipc::GetDescriptorType(descriptor)) {
        case ipc::CopyHandle:
        case ipc::MoveHandle: {
            // HLE services do not tell a copy from a move: the object goes into the client's table.
            const u32 count =
                std::min<std::size_t>(ipc::HandleNumberFromDesc(descriptor), command_size - i);
            for (u32 j = 0; j < count; ++j) {
                const u32 index = words[i];
                Handle handle = 0;
                const std::shared_ptr<Object> object =
                    index < reply.objects_.size() ? reply.objects_[index] : nullptr;
                if (object != nullptr) {
                    const Result result = process_->handle_table.Create(&handle, object);
                    if (IsError(result)) {
                        MH4N_LOG(Error, kLog,
                                 "reply 0x{:08X}: cannot give the client a handle: 0x{:08X}",
                                 words[0], result);
                        handle = 0;
                    }
                }
                out[i++] = handle;
            }
            break;
        }
        case ipc::CallingPid:
            if (i < command_size) {
                out[i++] = pid_;
            }
            break;
        case ipc::StaticBuffer: {
            if (i >= command_size) {
                break;
            }
            const u32 id = ipc::StaticBufferDescId(descriptor);
            const std::optional<std::vector<u8>>& data = reply.static_buffers_[id];
            const u32 receive_descriptor = area[ipc::StaticBufferDescriptorWord(id)];
            const u32 target = area[ipc::StaticBufferDescriptorWord(id) + 1];
            if (data.has_value()) {
                const u32 capacity = ipc::StaticBufferDescSize(receive_descriptor);
                u32 size = static_cast<u32>(data->size());
                if (size > capacity) {
                    MH4N_LOG(Error, kLog,
                             "reply 0x{:08X}: static buffer {} has {} bytes and the client's "
                             "receive buffer {}",
                             words[0], id, size, capacity);
                    size = capacity;
                }
                if (!memory.WriteBlock(target, data->data(), size)) {
                    MH4N_LOG(
                        Error, kLog,
                        "reply 0x{:08X}: the client's receive buffer {} (0x{:08X}) is not mapped",
                        words[0], id, target);
                }
            }
            out[i++] = target;
            break;
        }
        case ipc::MappedBuffer: {
            if (i >= command_size) {
                break;
            }
            const u32 id = words[i];
            out[i++] = id < mapped_.size() ? mapped_[id].Address() : 0;
            break;
        }
        default:
            MH4N_LOG(Error, kLog, "reply 0x{:08X}: unsupported descriptor 0x{:08X}",
                     words[0], descriptor);
            break;
        }
    }
    if (!memory.WriteBlock(base, out.data(), static_cast<u32>(out.size() * sizeof(u32)))) {
        MH4N_LOG(Error, kLog, "thread {}'s command buffer (0x{:08X}) is not mapped",
                 thread_->thread_id, base);
    }
    return out;
}

void Request::Deliver(Reply reply) {
    // On every path out of here (reply written, dropped, already finished) the kernel objects go.
    struct Release {
        Request& request;
        ~Release() { request.ReleaseKernelObjects(); }
    } release{*this};
    if (finished_) {
        MH4N_LOG(Error, kLog, "request 0x{:08X} of thread {} already has its reply", Header(),
                 thread_id_);
        return;
    }
    finished_ = true;
    if (cancelled_ || thread_ == nullptr || thread_->status != ThreadStatus::WaitIPC) {
        MH4N_LOG(Debug, kLog,
                 "the reply to request 0x{:08X} comes after thread {} stopped waiting for it",
                 Header(), thread_id_);
        return;
    }
    const std::vector<u32> words = WriteReply(reply);
    if (replied_hook_) {
        replied_hook_(*this, words);
    }
    thread_->ResumeFromWait(std::nullopt);
}

void Complete(const std::shared_ptr<Request>& request, Reply reply) {
    // std::function needs a copyable callable.
    auto shared = std::make_shared<Reply>(std::move(reply));
    request->GetKernel().Post([request, shared] { request->Deliver(std::move(*shared)); });
}

}  // namespace mh4n
