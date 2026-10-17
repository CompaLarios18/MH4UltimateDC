// Derived from Azahar src/core/hle/ipc.h and src/core/hle/kernel/hle_ipc.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: each says "GPLv2 or any later version"): the layouts of the translate descriptors
// and the walk over the translate parameters of a recorded command (Part and ParseCommand, the same
// walk as the runtime's IPC trace writer, which is derived from the same two files).
// Copyright 2016 Citra Emulator Project (ipc.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (hle_ipc.cpp)
#include "ipc_replay/replay.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <thread>

#include <fmt/format.h>

#include "mh4n/crc32.h"
#include "mh4n/ipc.h"
#include "mh4n/registry.h"
#include "mh4n/threads.h"

namespace mh4n::ipc_replay {

namespace {

constexpr u32 kSvcSendSyncRequest = 0x32;
constexpr u32 kSvcCloseHandle = 0x23;
constexpr u32 kSvcDuplicateHandle = 0x27;
constexpr std::size_t kNoCreation = ~static_cast<std::size_t>(0);

// ---- the translate parameters of a command ---------------------------------------------------------------

struct Part {
    enum class Type { Handles, Pid, Static, Mapped, Pxi } type = Type::Handles;
    u32 index = 0;    // the word of the descriptor
    u32 size = 0;     // handles: how many; buffers: bytes
    bool move = false;
    u32 perms = 0;    // mapped buffers
    u32 address = 0;  // buffers
};

// Splits the translate parameters of `command`; stops at a descriptor it does not know.
std::vector<Part> ParseCommand(const std::vector<u32>& command) {
    std::vector<Part> parts;
    if (command.empty()) {
        return parts;
    }
    const ipc::Header header{command[0]};
    const std::size_t end = std::min<std::size_t>(
        command.size(), 1u + header.NormalParamsSize() + header.TranslateParamsSize());
    std::size_t i = 1u + header.NormalParamsSize();
    while (i < end) {
        const u32 descriptor = command[i];
        Part part;
        part.index = static_cast<u32>(i++);
        switch (ipc::GetDescriptorType(descriptor)) {
        case ipc::CopyHandle:
        case ipc::MoveHandle:
            part.type = Part::Type::Handles;
            part.move = ipc::GetDescriptorType(descriptor) == ipc::MoveHandle;
            part.size = std::min<u32>(ipc::HandleNumberFromDesc(descriptor), static_cast<u32>(end - i));
            i += part.size;
            break;
        case ipc::CallingPid:
            part.type = Part::Type::Pid;
            part.size = 1;
            i += 1;
            break;
        case ipc::MappedBuffer:
            part.type = Part::Type::Mapped;
            part.perms = static_cast<u32>(ipc::MappedBufferDescPerms(descriptor));
            part.size = ipc::MappedBufferDescSize(descriptor);
            part.address = i < end ? command[i] : 0;
            ++i;
            break;
        case ipc::PXIBuffer:
            part.type = Part::Type::Pxi;
            part.size = ipc::PXIBufferDescSize(descriptor);
            part.address = i < end ? command[i] : 0;
            ++i;
            break;
        case ipc::StaticBuffer:
            part.type = Part::Type::Static;
            part.size = ipc::StaticBufferDescSize(descriptor);
            part.address = i < end ? command[i] : 0;
            ++i;
            break;
        default:
            return parts;
        }
        parts.push_back(part);
    }
    return parts;
}

const char* MappedKind(u32 perms) {
    return perms == 1 ? "mapped-r" : perms == 2 ? "mapped-w" : "mapped-rw";
}

std::string Hex(u32 value) {
    return fmt::format("0x{:08x}", value);
}

// The first normal parameter of a reply, which is its result.
std::optional<u32> ResultOf(const std::vector<u32>& words) {
    if (words.size() < 2 || ipc::Header{words[0]}.NormalParamsSize() < 1) {
        return std::nullopt;
    }
    return words[1];
}

std::string BaseService(const std::string& session) {
    return session.substr(0, session.find('/'));
}

// `fs:USER/file#3` -> `file`; empty for a name that has no kind.
std::string KindInName(const std::string& name) {
    const std::size_t slash = name.rfind('/');
    const std::size_t hash = name.rfind('#');
    if (slash == std::string::npos || hash == std::string::npos || hash < slash) {
        return {};
    }
    return name.substr(slash + 1, hash - slash - 1);
}

std::vector<std::string> SplitNames(const std::string& token) {
    std::vector<std::string> names;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = token.find('+', start);
        names.push_back(token.substr(start, at == std::string::npos ? at : at - start));
        if (at == std::string::npos) {
            return names;
        }
        start = at + 1;
    }
}

// What the IPC trace calls the kind of an object a reply handed out (ipc/trace.cpp).
std::string NativeKind(const Object& object) {
    if (object.GetHandleType() == HandleType::ClientSession) {
        const Service* service = static_cast<const ClientSession&>(object).Server()->GetService();
        return service != nullptr ? service->SessionKind() : "session";
    }
    std::string kind = object.TypeName();
    for (char& c : kind) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return kind;
}

// ---- lining the SVC trace up with the IPC trace ----------------------------------------------------------------

struct Creation {
    SvcCall call;
};

struct Alignment {
    std::vector<Creation> creations;
    // Per request: the reference handle -> the creation that made it, for the handles it carries.
    std::map<const IpcRecord*, std::map<u32, std::size_t>> carried;
    // Per request: the handles closed after it (reference value, creation or kNoCreation).
    std::map<const IpcRecord*, std::vector<std::pair<u32, std::size_t>>> closed_after;
};

bool IsCreatingCall(u32 svc) {
    return svc == 0x13 || svc == 0x15 || svc == 0x17 || svc == 0x1A || svc == 0x1E || svc == 0x21;
}

Alignment Align(const IpcTrace& trace, const SvcTrace& svc) {
    Alignment out;
    std::map<std::string, std::vector<const IpcRecord*>> by_thread;
    for (const IpcRecord& record : trace.records) {
        by_thread[record.thread].push_back(&record);
    }
    for (auto& [thread, records] : by_thread) {
        std::stable_sort(records.begin(), records.end(),
                         [](const IpcRecord* a, const IpcRecord* b) { return a->seq < b->seq; });
    }
    std::map<std::string, std::size_t> sent;
    std::map<u32, std::size_t> live;  // handle -> creation
    const IpcRecord* last = nullptr;
    for (const SvcCall& call : svc.calls) {
        const bool ok = call.has_out && call.out[0] == 0;
        if (call.svc == kSvcSendSyncRequest) {
            // A call that failed with 'invalid handle' is not a request the recorder saw.
            if (call.has_out && call.out[0] == kResultInvalidHandle) {
                continue;
            }
            const auto queue = by_thread.find(call.thread);
            std::size_t& n = sent[call.thread];
            if (queue == by_thread.end() || n >= queue->second.size()) {
                continue;
            }
            const IpcRecord* record = queue->second[n++];
            last = record;
            if (!record->in) {
                continue;
            }
            for (const Part& part : ParseCommand(*record->in)) {
                if (part.type != Part::Type::Handles) {
                    continue;
                }
                for (u32 j = 0; j < part.size; ++j) {
                    const u32 handle = (*record->in)[part.index + 1 + j];
                    const auto known = live.find(handle);
                    if (known != live.end()) {
                        out.carried[record][handle] = known->second;
                    }
                    if (part.move) {
                        live.erase(handle);
                    }
                }
            }
        } else if (ok && IsCreatingCall(call.svc)) {
            out.creations.push_back(Creation{call});
            live[call.out[1]] = out.creations.size() - 1;
        } else if (ok && call.svc == kSvcDuplicateHandle) {
            const auto source = live.find(call.args[1]);
            if (source != live.end()) {
                live[call.out[1]] = source->second;
            }
        } else if (ok && call.svc == kSvcCloseHandle) {
            const auto known = live.find(call.args[0]);
            const std::size_t creation = known != live.end() ? known->second : kNoCreation;
            live.erase(call.args[0]);
            if (last != nullptr) {
                out.closed_after[last].push_back({call.args[0], creation});
            }
        }
    }
    return out;
}

// ---- the engine ---------------------------------------------------------------------------------------------------

class Engine {
public:
    Engine(Runtime& runtime, ServiceManager& manager, const IpcTrace& trace, const SvcTrace* svc,
           const ReplayConfig& config)
        : manager_(manager), trace_(trace), svc_(svc), config_(config),
          kernel_(runtime.GetKernel()), memory_(runtime.Memory()), scheduler_(runtime.Scheduler()) {}

    ReplayReport Run();

private:
    enum class Outcome { Matched, Differed, Skipped };

    bool Setup(std::string* error);
    bool NewScratchThread();
    bool Selected(const std::string& base) const;
    bool FindFree(u32 size, const std::vector<std::pair<u32, u32>>& used, u32* address);
    bool EnsureMapped(u32 address, u32 size);

    void Skip(const std::string& service, const std::string& reason) { ++report_.skipped[{service, reason}]; }
    Outcome Step(const IpcRecord& record);
    void Finish(const IpcRecord& record);

    Handle SessionFor(const std::string& session);
    bool NativeHandle(const IpcRecord& record, u32 reference, Handle* native, std::string* reason);
    std::shared_ptr<Object> MakeObject(const SvcCall& call);
    void CloseReference(u32 reference, std::size_t creation);
    void Forget(Handle native);

    bool WaitForReply(std::string* problem);
    void Compare(const IpcRecord& record, const std::vector<u32>& words,
                 const std::vector<Part>& request_parts, Difference* diff);
    void LearnHandles(const IpcRecord& record, const std::vector<u32>& words);

    ServiceManager& manager_;
    const IpcTrace& trace_;
    const SvcTrace* svc_;
    const ReplayConfig& config_;
    Kernel& kernel_;
    GuestMemory& memory_;
    GuestScheduler& scheduler_;
    std::shared_ptr<Process> process_;
    std::shared_ptr<Thread> thread_;
    u32 tls_ = 0;
    u32 receive_ = 0;
    u32 receive_size_ = 0;
    Alignment alignment_;
    std::set<std::string> explicit_;
    std::map<std::string, bool> stopped_;
    std::map<u32, Handle> handle_map_;            // reference handle -> native
    std::map<std::string, Handle> sessions_;      // session name -> native handle
    std::map<std::size_t, Handle> creation_handle_;  // creation -> native handle
    ReplayReport report_;
};

bool Engine::Selected(const std::string& base) const {
    if (!explicit_.empty()) {
        return explicit_.count(base) != 0;
    }
    return Registry<ServiceFactory>::Instance().Find(base) != nullptr;
}

bool Engine::EnsureMapped(u32 address, u32 size) {
    if (size == 0) {
        return true;
    }
    const u64 last = static_cast<u64>(address) + size;
    if (last > kManagedEnd) {
        return false;
    }
    for (u64 page = address & ~kPageMask; page < last; page += kPageSize) {
        if (!memory_.IsMapped(static_cast<u32>(page), kPageSize) &&
            IsError(memory_.Map(static_cast<u32>(page), kPageSize, MemPerm::ReadWrite, MemState::Private))) {
            return false;
        }
    }
    return true;
}

// Maps `size` bytes of memory the trace's buffers do not touch.
bool Engine::FindFree(u32 size, const std::vector<std::pair<u32, u32>>& used, u32* address) {
    const u32 rounded = (size + kPageMask) & ~kPageMask;
    for (u64 base = 0x00200000; base + rounded <= 0x1E000000; base += 0x00100000) {
        bool clash = false;
        for (const auto& [lo, hi] : used) {
            if (lo < base + rounded && hi > base) {
                clash = true;
                break;
            }
        }
        if (!clash && IsSuccess(memory_.Map(static_cast<u32>(base), rounded, MemPerm::ReadWrite,
                                            MemState::Private))) {
            *address = static_cast<u32>(base);
            return true;
        }
    }
    return false;
}

bool Engine::NewScratchThread() {
    thread_ = kernel_.NewThread("ipc-replay", 48, 0, process_);
    // A thread with thread-local storage gives its resource limit slot back when it ends.
    process_->resource_limit->Reserve(ResourceLimitType::Thread, 1);
    thread_->tls_address = tls_;
    scheduler_.AttachFunction(thread_, [] {});
    return true;
}

bool Engine::Setup(std::string* error) {
    process_ = kernel_.CurrentProcess();
    if (!process_) {
        *error = "the runtime has no process";
        return false;
    }
    for (const std::string& name : config_.services) {
        if (Registry<ServiceFactory>::Instance().Find(name) == nullptr) {
            std::string known;
            for (const auto& [registered, factory] : Registry<ServiceFactory>::Instance().All()) {
                known += (known.empty() ? "" : ", ") + registered;
            }
            *error = fmt::format("no service `{}` is registered natively (registered: {})", name,
                                 known.empty() ? "none" : known);
            return false;
        }
        explicit_.insert(name);
    }
    // The buffers the trace puts in memory, and the receive area the replies need.
    std::vector<std::pair<u32, u32>> used;
    u32 receive = 0x1000;
    for (const IpcRecord& record : trace_.records) {
        if (record.in) {
            for (const Part& part : ParseCommand(*record.in)) {
                if (part.type == Part::Type::Static || part.type == Part::Type::Mapped ||
                    part.type == Part::Type::Pxi) {
                    const u64 lo = part.address & ~kPageMask;
                    const u64 hi = (static_cast<u64>(part.address) + part.size + kPageMask) & ~static_cast<u64>(kPageMask);
                    used.push_back({static_cast<u32>(lo), static_cast<u32>(std::min<u64>(hi, 0xFFFFFFFFull))});
                }
            }
        }
        u32 total = 0;
        for (const IpcDesc& desc : record.outdesc) {
            if (desc.kind == "static") {
                total += (std::min<u32>(desc.size + 0x40, 0x3FFFF) + 15) & ~15u;
            }
        }
        receive = std::max(receive, total);
    }
    if (!FindFree(kPageSize, used, &tls_)) {
        *error = "no scratch memory for the thread-local storage";
        return false;
    }
    used.push_back({tls_, tls_ + kPageSize});
    receive_size_ = (receive + kPageMask) & ~kPageMask;
    if (!FindFree(receive_size_, used, &receive_)) {
        *error = "no scratch memory for the receive buffers";
        return false;
    }
    NewScratchThread();
    manager_.SetReplyDelayNs(0);
    if (svc_ != nullptr) {
        alignment_ = Align(trace_, *svc_);
    }
    return true;
}

// ---- objects and handles --------------------------------------------------------------------------------------------

std::shared_ptr<Object> Engine::MakeObject(const SvcCall& call) {
    switch (call.svc) {
    case 0x17:
        return kernel_.NewEvent(static_cast<ResetType>(call.args[1]));
    case 0x13:
        return kernel_.NewMutex(false);
    case 0x15: {
        std::shared_ptr<Semaphore> semaphore;
        return IsSuccess(kernel_.NewSemaphore(&semaphore, static_cast<s32>(call.args[1]),
                                              static_cast<s32>(call.args[2])))
                   ? semaphore
                   : nullptr;
    }
    case 0x1A:
        return kernel_.NewTimer(static_cast<ResetType>(call.args[1]));
    case 0x21:
        return kernel_.NewAddressArbiter();
    case 0x1E: {
        // r0 the other processes' permission, r2 the size, r3 the creator's.
        if (!process_->resource_limit->Reserve(ResourceLimitType::SharedMemory, 1)) {
            return nullptr;
        }
        std::shared_ptr<SharedMemory> block;
        return IsSuccess(kernel_.NewSharedMemory(&block, process_, call.args[2],
                                                 static_cast<MemoryPermission>(call.args[3]),
                                                 static_cast<MemoryPermission>(call.args[0]), 0,
                                                 FcramRegion::Base))
                   ? block
                   : nullptr;
    }
    default:
        return nullptr;
    }
}

bool Engine::NativeHandle(const IpcRecord& record, u32 reference, Handle* native, std::string* reason) {
    if (reference == 0 || reference == kHandleCurrentThread || reference == kHandleCurrentProcess) {
        *native = reference;
        return true;
    }
    const auto replied = handle_map_.find(reference);
    if (replied != handle_map_.end() && process_->handle_table.IsValid(replied->second)) {
        *native = replied->second;
        return true;
    }
    const auto carried = alignment_.carried.find(&record);
    if (carried != alignment_.carried.end()) {
        const auto creation = carried->second.find(reference);
        if (creation != carried->second.end()) {
            const auto made = creation_handle_.find(creation->second);
            if (made != creation_handle_.end() && process_->handle_table.IsValid(made->second)) {
                *native = made->second;
                return true;
            }
            const std::shared_ptr<Object> object = MakeObject(alignment_.creations[creation->second].call);
            Handle handle = 0;
            if (object && IsSuccess(process_->handle_table.Create(&handle, object))) {
                creation_handle_[creation->second] = handle;
                *native = handle;
                return true;
            }
            *reason = "an object of the SVC trace could not be made";
            return false;
        }
    }
    *reason = svc_ == nullptr ? "a handle that no reply introduced (no SVC trace given)"
                              : "a handle neither a reply nor a creating SVC call introduced";
    return false;
}

void Engine::Forget(Handle native) {
    for (auto it = handle_map_.begin(); it != handle_map_.end();) {
        it = it->second == native ? handle_map_.erase(it) : std::next(it);
    }
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        it = it->second == native ? sessions_.erase(it) : std::next(it);
    }
    for (auto it = creation_handle_.begin(); it != creation_handle_.end();) {
        it = it->second == native ? creation_handle_.erase(it) : std::next(it);
    }
}

void Engine::CloseReference(u32 reference, std::size_t creation) {
    Handle native = 0;
    const auto replied = handle_map_.find(reference);
    if (replied != handle_map_.end()) {
        native = replied->second;
    } else if (creation != kNoCreation && creation_handle_.count(creation) != 0) {
        native = creation_handle_[creation];
    } else {
        return;
    }
    Forget(native);
    if (process_->handle_table.IsValid(native)) {
        process_->handle_table.Close(native);
    }
}

Handle Engine::SessionFor(const std::string& session) {
    const auto known = sessions_.find(session);
    if (known != sessions_.end() && process_->handle_table.Get<ClientSession>(known->second)) {
        return known->second;
    }
    if (session.find('/') != std::string::npos || session.starts_with("unnamed#")) {
        return 0;  // only the reply that hands it out can make it
    }
    std::shared_ptr<Object> object;
    if (IsError(manager_.ConnectToPort(session, &object))) {
        std::shared_ptr<ClientSession> client;
        if (IsError(manager_.ConnectToService(session, &client))) {
            return 0;
        }
        object = client;
    }
    Handle handle = 0;
    if (IsError(process_->handle_table.Create(&handle, object))) {
        return 0;
    }
    sessions_[session] = handle;
    return handle;
}

// ---- one request --------------------------------------------------------------------------------------------------------

bool Engine::WaitForReply(std::string* problem) {
    const auto deadline = std::chrono::steady_clock::now() + config_.reply_timeout;
    while (thread_->status == ThreadStatus::WaitIPC) {
        scheduler_.RunPending();
        if (thread_->status != ThreadStatus::WaitIPC) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            *problem = fmt::format("no reply within {} ms", config_.reply_timeout.count());
            // Ends the thread, which cancels the request; the next one gets a new thread.
            scheduler_.Terminate(thread_);
            NewScratchThread();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    thread_->status = ThreadStatus::Running;
    return true;
}

Engine::Outcome Engine::Step(const IpcRecord& record) {
    const std::string base = BaseService(record.session);
    if (!Selected(base)) {
        if (explicit_.empty()) {
            Skip(base, "service not registered natively");
        }
        return Outcome::Skipped;
    }
    if (!record.cmd || !record.in) {
        Skip(base, "the trace has no request for it");
        return Outcome::Skipped;
    }
    if (!record.out) {
        Skip(base, "the trace has no reply for it");
        return Outcome::Skipped;
    }
    if (stopped_[base]) {
        Skip(base, "after the first difference");
        return Outcome::Skipped;
    }
    const std::vector<u32>& in = *record.in;
    if (in.size() > ipc::kCommandBufferLength) {
        Skip(base, "request longer than a command buffer");
        return Outcome::Skipped;
    }

    const Handle session = SessionFor(record.session);
    if (session == 0) {
        Skip(base, "session not available (no native service, or never handed out)");
        return Outcome::Skipped;
    }

    // The command area: the words with the native handles, then the receive buffer descriptors.
    std::array<u32, ipc::kCommandAreaLength> area{};
    std::copy(in.begin(), in.end(), area.begin());
    const std::vector<Part> parts = ParseCommand(in);
    std::vector<Handle> moved;  // native handles the request moves
    for (const Part& part : parts) {
        if (part.type != Part::Type::Handles) {
            continue;
        }
        for (u32 j = 0; j < part.size; ++j) {
            Handle native = 0;
            std::string reason;
            if (!NativeHandle(record, in[part.index + 1 + j], &native, &reason)) {
                Skip(base, reason);
                return Outcome::Skipped;
            }
            area[part.index + 1 + j] = native;
            if (part.move && native != 0 && native != kHandleCurrentThread &&
                native != kHandleCurrentProcess) {
                moved.push_back(native);
            }
        }
    }

    // The buffers at their recorded addresses.
    for (const Part& part : parts) {
        if (part.type != Part::Type::Static && part.type != Part::Type::Mapped) {
            continue;
        }
        if (!EnsureMapped(part.address, part.size)) {
            Skip(base, "a buffer lies outside the memory the replay can map");
            return Outcome::Skipped;
        }
        std::vector<u8> bytes(part.size, 0);
        const auto data = record.data.find(part.index);
        if (data != record.data.end()) {
            std::copy_n(data->second.begin(), std::min<std::size_t>(data->second.size(), bytes.size()),
                        bytes.begin());
        }
        if (!bytes.empty() && !memory_.WriteBlock(part.address, bytes.data(), part.size)) {
            Skip(base, "a buffer lies outside the memory the replay can map");
            return Outcome::Skipped;
        }
    }

    // The receive buffers of the static buffers the recorded reply carries.
    {
        u32 cursor = receive_;
        for (const IpcDesc& desc : record.outdesc) {
            if (desc.kind != "static" || desc.index >= record.out->size()) {
                continue;
            }
            const u32 id = ipc::StaticBufferDescId((*record.out)[desc.index]);
            const u32 capacity = std::min<u32>(desc.size + 0x40, 0x3FFFF);
            const std::vector<u8> zeros(capacity, 0);
            if (cursor + capacity > receive_ + receive_size_ ||
                !memory_.WriteBlock(cursor, zeros.data(), capacity)) {
                Skip(base, "the receive buffers do not fit the replay's scratch area");
                return Outcome::Skipped;
            }
            area[ipc::StaticBufferDescriptorWord(id)] = ipc::StaticBufferDesc(capacity, static_cast<u8>(id));
            area[ipc::StaticBufferDescriptorWord(id) + 1] = cursor;
            cursor += (capacity + 15) & ~15u;
        }
    }
    const u32 command_address = CommandBufferAddress(*thread_);
    memory_.WriteBlock(command_address, area.data(), static_cast<u32>(sizeof(area)));

    Difference diff;
    diff.service = base;
    diff.seq = record.seq;
    diff.session = record.session;
    diff.header = *record.cmd;

    thread_->status = ThreadStatus::Running;
    const Result sent = manager_.SendSyncRequest(*thread_, session);
    for (const Handle native : moved) {
        Forget(native);  // the kernel closed it in the client's table
    }
    std::string problem;
    if (IsError(sent)) {
        diff.what.push_back(fmt::format("svcSendSyncRequest failed with {}", Hex(sent)));
    } else if (!WaitForReply(&problem)) {
        diff.what.push_back(problem);
    } else {
        std::vector<u32> words(1);
        if (!memory_.ReadBlock(words.data(), command_address, 4)) {
            diff.what.push_back("the reply cannot be read");
        } else {
            const ipc::Header header{words[0]};
            words.resize(std::min<std::size_t>(ipc::kCommandBufferLength,
                                               1u + header.NormalParamsSize() + header.TranslateParamsSize()));
            memory_.ReadBlock(words.data(), command_address, static_cast<u32>(words.size() * 4));
            Compare(record, words, parts, &diff);
            LearnHandles(record, words);
        }
    }

    ServiceTally& tally = report_.services[base];
    ++tally.replayed;
    if (diff.what.empty()) {
        return Outcome::Matched;
    }
    ++tally.differ;
    report_.differences.push_back(std::move(diff));
    if (!config_.keep_going) {
        stopped_[base] = true;
    }
    return Outcome::Differed;
}

void Engine::Compare(const IpcRecord& record, const std::vector<u32>& words,
                     const std::vector<Part>& request_parts, Difference* diff) {
    const std::vector<u32>& expected = *record.out;
    const auto expected_result = ResultOf(expected);
    const auto got_result = ResultOf(words);
    const auto result_differs = [&] {
        if (expected_result != got_result) {
            diff->what.push_back(fmt::format("result: expected {}, got {}",
                                             expected_result ? Hex(*expected_result) : "none",
                                             got_result ? Hex(*got_result) : "none"));
        }
    };
    if (config_.time_dependent.Matches(record.session, *record.cmd)) {
        result_differs();
        return;
    }
    if (expected[0] != words[0]) {
        diff->what.push_back(fmt::format("header: expected {}, got {}", Hex(expected[0]), Hex(words[0])));
    }
    result_differs();
    // The normal parameters after the result.
    const u32 normal = std::min(ipc::Header{expected[0]}.NormalParamsSize(), ipc::Header{words[0]}.NormalParamsSize());
    for (u32 i = 2; i <= normal; ++i) {
        if (i < expected.size() && i < words.size() && expected[i] != words[i]) {
            diff->what.push_back(fmt::format("normal word {} differs", i));
        }
    }

    // The descriptors, as the IPC trace lists them: the reply's own, then the request's writable
    // mapped buffers.
    std::vector<IpcDesc> got;
    for (const Part& part : ParseCommand(words)) {
        IpcDesc desc;
        desc.index = part.index;
        desc.size = part.size;
        desc.token = "-";
        switch (part.type) {
        case Part::Type::Handles:
            desc.kind = part.move ? "move" : "copy";
            break;
        case Part::Type::Pid:
            desc.kind = "pid";
            break;
        case Part::Type::Static: {
            desc.kind = "static";
            std::vector<u8> bytes(part.size);
            if (memory_.ReadBlock(bytes.data(), part.address, part.size)) {
                desc.token = Hex(Crc32(bytes.data(), bytes.size()));
            }
            break;
        }
        case Part::Type::Mapped:  // the request's own descriptor covers it
        case Part::Type::Pxi:
            continue;
        }
        got.push_back(std::move(desc));
    }
    for (const Part& part : request_parts) {
        if (part.type == Part::Type::Mapped && (part.perms & 2) != 0) {
            IpcDesc desc;
            desc.kind = MappedKind(part.perms);
            desc.index = part.index;
            desc.size = part.size;
            desc.token = "-";
            std::vector<u8> bytes(part.size);
            if (memory_.ReadBlock(bytes.data(), part.address, part.size)) {
                desc.token = Hex(Crc32(bytes.data(), bytes.size()));
            }
            got.push_back(std::move(desc));
        }
    }

    if (got.size() != record.outdesc.size()) {
        diff->what.push_back(fmt::format("the reply has {} descriptors, expected {}", got.size(),
                                         record.outdesc.size()));
    }
    for (std::size_t i = 0; i < std::min(got.size(), record.outdesc.size()); ++i) {
        const IpcDesc& want = record.outdesc[i];
        const IpcDesc& have = got[i];
        const std::string where = fmt::format("descriptor {} ({}, index {})", i, want.kind, want.index);
        if (want.kind != have.kind) {
            diff->what.push_back(fmt::format("descriptor {}: expected {}, got {} at index {}", i, want.kind,
                                             have.kind, have.index));
            continue;
        }
        if (want.index != have.index) {
            diff->what.push_back(fmt::format("{}: got it at index {}", where, have.index));
        }
        if (want.size != have.size) {
            diff->what.push_back(fmt::format("{}: size expected {}, got {}", where, want.size, have.size));
        }
        if (want.IsBuffer() && want.token != "-" && want.token != have.token) {
            diff->what.push_back(fmt::format("{}: crc expected {}, got {}", where, want.token, have.token));
        }
        if (want.IsHandles() && want.index == have.index && want.size == have.size) {
            const std::vector<std::string> names = SplitNames(want.token);
            for (u32 j = 0; j < want.size; ++j) {
                const std::size_t at = want.index + 1 + j;
                if (at >= expected.size() || at >= words.size()) {
                    continue;
                }
                const bool want_null = expected[at] == 0;
                const bool have_null = words[at] == 0;
                if (want_null != have_null) {
                    diff->what.push_back(fmt::format("{}: handle {} expected {}, got {}", where, j,
                                                     want_null ? "null" : "a handle",
                                                     have_null ? "null" : "a handle"));
                    continue;
                }
                if (have_null) {
                    continue;
                }
                const std::shared_ptr<Object> object = process_->handle_table.GetGeneric(words[at]);
                if (!object) {
                    diff->what.push_back(fmt::format("{}: handle {} is not in the client's table", where, j));
                    continue;
                }
                const std::string kind = j < names.size() ? KindInName(names[j]) : std::string();
                if (!kind.empty() && kind != NativeKind(*object)) {
                    diff->what.push_back(fmt::format("{}: handle {} kind expected {}, got {}", where, j, kind,
                                                     NativeKind(*object)));
                }
            }
        }
    }
}

void Engine::LearnHandles(const IpcRecord& record, const std::vector<u32>& words) {
    const std::vector<u32>& expected = *record.out;
    const std::vector<Part> want_parts = ParseCommand(expected);
    for (const Part& part : ParseCommand(words)) {
        if (part.type != Part::Type::Handles) {
            continue;
        }
        const auto want = std::find_if(want_parts.begin(), want_parts.end(), [&](const Part& p) {
            return p.type == Part::Type::Handles && p.index == part.index && p.size == part.size;
        });
        if (want == want_parts.end()) {
            continue;
        }
        std::vector<std::string> names;
        for (const IpcDesc& desc : record.outdesc) {
            if (desc.IsHandles() && desc.index == part.index) {
                names = SplitNames(desc.token);
            }
        }
        for (u32 j = 0; j < part.size; ++j) {
            const Handle native = words[part.index + 1 + j];
            const u32 reference = expected[part.index + 1 + j];
            if (native == 0 || reference == 0) {
                continue;
            }
            handle_map_[reference] = native;
            if (const auto session = process_->handle_table.Get<ClientSession>(native)) {
                if (j < names.size() && names[j] != "-" && names[j] != "0") {
                    sessions_[names[j]] = native;
                }
                if (!session->PortName().empty()) {
                    sessions_[session->PortName()] = native;
                }
            }
        }
    }
}

void Engine::Finish(const IpcRecord& record) {
    const auto closed = alignment_.closed_after.find(&record);
    if (closed == alignment_.closed_after.end()) {
        return;
    }
    for (const auto& [reference, creation] : closed->second) {
        CloseReference(reference, creation);
    }
}

ReplayReport Engine::Run() {
    if (!Setup(&report_.error)) {
        return std::move(report_);
    }
    std::vector<const IpcRecord*> order;
    for (const IpcRecord& record : trace_.records) {
        order.push_back(&record);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const IpcRecord* a, const IpcRecord* b) { return a->seq < b->seq; });
    for (const IpcRecord* record : order) {
        Step(*record);
        Finish(*record);
    }
    return std::move(report_);
}

}  // namespace

u32 ReplayReport::Replayed() const {
    u32 total = 0;
    for (const auto& [name, tally] : services) {
        total += tally.replayed;
    }
    return total;
}

u32 ReplayReport::Differing() const {
    u32 total = 0;
    for (const auto& [name, tally] : services) {
        total += tally.differ;
    }
    return total;
}

int ReplayReport::ExitStatus() const {
    if (!error.empty() || Replayed() == 0) {
        return 2;
    }
    return Differing() != 0 ? 1 : 0;
}

ReplayReport Replay(Runtime& runtime, ServiceManager& manager, const IpcTrace& trace,
                    const SvcTrace* svc, const ReplayConfig& config) {
    return Engine(runtime, manager, trace, svc, config).Run();
}

std::string FormatReport(const ReplayReport& report) {
    std::string out;
    if (!report.error.empty()) {
        return "error: " + report.error + "\n";
    }
    for (const Difference& diff : report.differences) {
        out += fmt::format("DIFF {} seq={} session={} header={}\n", diff.service, diff.seq, diff.session,
                           Hex(diff.header));
        for (const std::string& line : diff.what) {
            out += "  " + line + "\n";
        }
    }
    u32 skipped = 0;
    for (const auto& [key, count] : report.skipped) {
        skipped += count;
    }
    out += fmt::format("replayed {} requests: {} matched, {} differ, {} skipped\n", report.Replayed(),
                       report.Replayed() - report.Differing(), report.Differing(), skipped);
    for (const auto& [name, tally] : report.services) {
        out += fmt::format("  {}: {} replayed, {} differ\n", name, tally.replayed, tally.differ);
    }
    for (const auto& [key, count] : report.skipped) {
        out += fmt::format("  skipped {} x {}: {}\n", count, key.first, key.second);
    }
    return out;
}

}  // namespace mh4n::ipc_replay
