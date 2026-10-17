// Derived from Azahar src/core/hle/ipc.h and src/core/hle/kernel/hle_ipc.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: each says "GPLv2 or any later version"): the layouts of the translate descriptors
// that the trace walks (handles, the calling PID, static, PXI and mapped buffers) and the walk over the
// translate parameters of a command (PopulateFromIncomingCommandBuffer).
// Copyright 2016 Citra Emulator Project (ipc.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (hle_ipc.cpp)
//
// --trace-ipc FILE and --trace-ipc-full (N1-10): every IPC request with its reply, in the format of
// N0-21 (tools/compare_ipc_traces.py, tools/native/tracefmt.py):
//
//   # ipctrace v1 <date> game=0x<title id> cpu=native new3ds=off
//   # thread <id> parent=<id> entry=<hex> arg=<hex> stack=<hex> prio=<n> core=<requested>/<resolved>
//   <thread> seq=<n> session=<name> cmd=<header> in=<words> desc=<descriptors> out=<words>
//            outdesc=<descriptors> t=<ticks> [data=<index>:<hex>]...
//
// One line per request, written when its reply is: a service that defers its reply comes after the
// requests other threads sent later. Words are as the client wrote and read them. A request that never
// got a reply (the run ended) has `out=-` and `outdesc=-`; one that failed in translation has `cmd=-`
// and `in=-`. `session` is the port or service name for a session that came out of one (`fs:USER`), and
// `<service>/<kind>#<k>` for a session or object a reply handed out (kind `file`, `dir`, `session`, or
// the lowercase kernel type): the k-th of that kind from that service, counting from 0. A session the
// trace knows nothing about is `unnamed#<k>`. Never a file name. `t` is game-clock ticks.
//
// The input buffers' CRC-32 is taken when the service gets the request; the output buffers' when the
// reply is written (what it wrote into the client's static receive buffers, and the writable mapped
// buffers of the request). A buffer that cannot be read has the token `-`.
//
// The traces hold file names, save bytes and the player's name: keep them under build/.
#include <array>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "mh4n/crc32.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

namespace mh4n {

MH4N_OPTION("runner", "--trace-ipc", "FILE",
            "write every IPC request with its reply (format of tools/compare_ipc_traces.py)");
MH4N_OPTION("runner", "--trace-ipc-full", "",
            "with --trace-ipc: also the bytes of the input buffers (data=)");

namespace {

constexpr const char* kLog = "Runtime";

// One translate descriptor of a command, as the trace describes it.
struct Part {
    enum class Type { Handles, Pid, Static, Mapped, Pxi };
    Type type = Type::Handles;
    bool move = false;
    u32 index = 0;    // the descriptor's word
    u32 size = 0;     // handles: how many; buffers: bytes
    u32 perms = 0;    // mapped: 1 read, 2 write
    u32 address = 0;  // buffers: the word after the descriptor
};

// Splits the translate parameters of `command` into parts. Stops at a descriptor it does not know.
std::vector<Part> ParseCommand(const std::vector<u32>& command) {
    std::vector<Part> parts;
    if (command.empty()) {
        return parts;
    }
    const ipc::Header header{command[0]};
    const std::size_t end = std::min<std::size_t>(command.size(), 1 + header.NormalParamsSize() +
                                                                      header.TranslateParamsSize());
    std::size_t i = 1 + header.NormalParamsSize();
    while (i < end) {
        const u32 descriptor = command[i];
        Part part;
        part.index = static_cast<u32>(i++);
        switch (ipc::GetDescriptorType(descriptor)) {
        case ipc::CopyHandle:
        case ipc::MoveHandle:
            part.type = Part::Type::Handles;
            part.move = ipc::GetDescriptorType(descriptor) == ipc::MoveHandle;
            part.size =
                std::min<u32>(ipc::HandleNumberFromDesc(descriptor), static_cast<u32>(end - i));
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

std::string Describe(const char* kind, u32 index, u32 size, const std::string& token) {
    return fmt::format("{}:{}:{}:{}", kind, index, size, token);
}

const char* MappedKind(u32 perms) {
    return perms == 1   ? "mapped-r"
           : perms == 2 ? "mapped-w"
           : perms == 3 ? "mapped-rw"
                        : "mapped-0";
}

std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : ",") + item;
    }
    return out.empty() ? "-" : out;
}

std::string JoinWords(std::span<const u32> words) {
    std::string out;
    for (const u32 w : words) {
        out += fmt::format("{}0x{:08x}", out.empty() ? "" : ",", w);
    }
    return out.empty() ? "-" : out;
}

class IpcTracer {
public:
    IpcTracer(Runtime& rt, const std::string& path, bool full)
        : rt_(rt), full_(full), file_(std::fopen(path.c_str(), "w")) {}
    ~IpcTracer() { Close(); }
    bool ok() const { return file_ != nullptr; }

    void WriteHeader() {
        const ProcessImage* image = rt_.Image();
        const std::time_t now = std::time(nullptr);
        char date[16] = "0000-00-00";
        if (const std::tm* utc = std::gmtime(&now)) {
            std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
        }
        Write(fmt::format("# ipctrace v1 {} game=0x{:016x} cpu=native new3ds=off\n", date,
                          image != nullptr ? image->program_id : 0));
    }

    SvcHooks Hooks() {
        SvcHooks hooks;
        hooks.thread_created = [this](const SvcThreadCreated& t) {
            WriteMainThread();
            Write(fmt::format(
                "# thread {} parent={} entry={:08X} arg={:08X} stack={:08X} prio={} core={}/{}\n",
                t.thread_id, t.parent_id, t.entry, t.arg, t.stack_top, t.priority,
                t.requested_processor, t.resolved_processor));
        };
        return hooks;
    }

    IpcObserver Observer() {
        IpcObserver observer;
        observer.sent = [this](const Request& request, Result translation) {
            Sent(request, translation);
        };
        observer.replied = [this](const Request& request, std::span<const u32> words) {
            Replied(request, words);
        };
        return observer;
    }

    // The run is over: the requests that never got a reply, then the file.
    void Finish() {
        WriteMainThread();
        std::vector<Pending> rest;
        for (auto& [seq, pending] : pending_) {
            rest.push_back(std::move(pending));
        }
        pending_.clear();
        for (const Pending& pending : rest) {
            WriteLine(pending, nullptr, {});
        }
        Close();
    }

private:
    // A request between its sending and its reply.
    struct Pending {
        u32 seq = 0;
        u32 thread = 0;
        std::string session;
        std::string service;
        bool have_request = false;
        std::vector<u32> in_words;
        std::vector<Part> in_parts;
        std::vector<std::string> desc;
        std::vector<std::string> data;
    };

    void Write(const std::string& text) {
        if (file_ != nullptr) {
            std::fputs(text.c_str(), file_);
        }
    }
    void Close() {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    void WriteMainThread() {
        if (main_written_) {
            return;
        }
        const std::shared_ptr<Thread> thread = rt_.MainThread();
        if (!thread) {
            return;
        }
        main_written_ = true;
        Write(fmt::format(
            "# thread {} parent=0 entry={:08X} arg={:08X} stack={:08X} prio={} core={}/{}\n",
            thread->thread_id, thread->entry_point, 0u, thread->stack_top, thread->nominal_priority,
            thread->processor_id, thread->processor_id));
    }

    void WriteLine(const Pending& p, const std::vector<u32>* reply,
                   const std::vector<std::string>& outdesc) {
        WriteMainThread();
        std::string line = fmt::format(
            "{} seq={} session={} cmd={} in={} desc={} out={} outdesc={} t={}", p.thread, p.seq,
            p.session, p.have_request ? fmt::format("0x{:08x}", p.in_words[0]) : std::string("-"),
            p.have_request ? JoinWords(p.in_words) : std::string("-"),
            p.have_request ? Join(p.desc) : std::string("-"),
            reply ? JoinWords(*reply) : std::string("-"), reply ? Join(outdesc) : std::string("-"),
            rt_.Clock().Now());
        for (const std::string& data : p.data) {
            line += " data=" + data;
        }
        line += '\n';
        Write(line);
    }

    // The CRC-32 of `size` bytes of the client's memory from `address`, and their hex if asked. False
    // if any of it is not mapped.
    bool CrcGuest(u32 address, u32 size, u32& crc, std::string* hex) {
        crc = 0;
        if (size == 0) {
            return true;
        }
        GuestMemory& memory = rt_.Memory();
        if (static_cast<u64>(address) + size > (1ull << 32) || !memory.IsMapped(address, size)) {
            return false;
        }
        std::vector<u8> chunk(std::min<u32>(size, 0x10000));
        for (u64 at = address; at < static_cast<u64>(address) + size;) {
            const std::size_t n = static_cast<std::size_t>(
                std::min<u64>(chunk.size(), static_cast<u64>(address) + size - at));
            if (!memory.ReadBlock(chunk.data(), static_cast<u32>(at), static_cast<u32>(n))) {
                return false;
            }
            crc = Crc32(chunk.data(), n, crc);
            if (hex != nullptr) {
                for (std::size_t i = 0; i < n; ++i) {
                    *hex += fmt::format("{:02x}", chunk[i]);
                }
            }
            at += n;
        }
        return true;
    }

    static std::string CrcToken(bool ok, u32 crc) {
        return ok ? fmt::format("0x{:08x}", crc) : std::string("-");
    }

    // The service's port name for a session that has one (fs:USER); the name a reply gave a session
    // without one (fs:USER/file#3); else a number.
    std::string SessionName(const ClientSession& session) {
        if (!session.PortName().empty()) {
            return session.PortName();
        }
        const auto known = names_.find(session.ObjectId());
        if (known != names_.end()) {
            return known->second;
        }
        std::string name = fmt::format("unnamed#{}", unnamed_++);
        names_[session.ObjectId()] = name;
        return name;
    }

    // What kind of object a reply handed out: file, dir, session, or the lowercase kernel type.
    static std::string KindOf(const Object& object) {
        if (object.GetHandleType() == HandleType::ClientSession) {
            const auto& session = static_cast<const ClientSession&>(object);
            const Service* service = session.Server()->GetService();
            return service != nullptr ? service->SessionKind() : "session";
        }
        std::string kind = object.TypeName();
        for (char& c : kind) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return kind;
    }

    // `<service>/<kind>#<k>`, k counting from 0 per service and kind; the same object keeps its name.
    std::string NameHandedOut(const Object& object, const std::string& service) {
        const auto known = names_.find(object.ObjectId());
        if (known != names_.end()) {
            return known->second;
        }
        const std::string prefix = service + "/" + KindOf(object);
        const std::string name = fmt::format("{}#{}", prefix, counts_[prefix]++);
        names_[object.ObjectId()] = name;
        return name;
    }

    void Sent(const Request& request, Result translation) {
        const u32 thread_id = request.ClientThread().thread_id;
        // A request the thread sent before and never saw a reply to (the thread ended, or the call failed).
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.thread == thread_id) {
                WriteLine(it->second, nullptr, {});
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        Pending p;
        p.seq = request.Sequence();
        p.thread = thread_id;
        p.session = SessionName(*request.Session());
        p.service = p.session.substr(0, p.session.find('/'));
        if (IsError(translation)) {
            WriteLine(p, nullptr, {});
            return;
        }
        p.have_request = true;
        const std::span<const u32> in = request.UntranslatedWords();
        p.in_words.assign(in.begin(), in.end());
        p.in_parts = ParseCommand(p.in_words);
        const auto& descriptors = request.Descriptors();
        for (const Part& part : p.in_parts) {
            switch (part.type) {
            case Part::Type::Handles: {
                std::string token;
                for (u32 j = 0; j < part.size; ++j) {
                    const std::size_t at = part.index + 1 + j;
                    std::string name = "-";
                    if (at < p.in_words.size() && p.in_words[at] == 0) {
                        name = "0";
                    } else {
                        const Object* object = ObjectOf(descriptors, part.index, j);
                        const auto known =
                            object != nullptr ? names_.find(object->ObjectId()) : names_.end();
                        if (known != names_.end()) {
                            name = known->second;
                        }
                    }
                    token += (j != 0 ? "+" : "") + name;
                }
                p.desc.push_back(
                    Describe(part.move ? "move" : "copy", part.index, part.size, token));
                break;
            }
            case Part::Type::Pid:
                p.desc.push_back(Describe("pid", part.index, part.size, "-"));
                break;
            case Part::Type::Pxi:
                p.desc.push_back(Describe("pxi", part.index, part.size, "-"));
                break;
            case Part::Type::Static:
            case Part::Type::Mapped: {
                const bool is_static = part.type == Part::Type::Static;
                const bool readable = is_static || (part.perms & 1) != 0;
                u32 crc = 0;
                std::string hex;
                const bool ok =
                    readable && CrcGuest(part.address, part.size, crc, full_ ? &hex : nullptr);
                p.desc.push_back(Describe(is_static ? "static" : MappedKind(part.perms), part.index,
                                          part.size, CrcToken(ok, crc)));
                if (ok && full_) {
                    p.data.push_back(fmt::format("{}:{}", part.index, hex));
                }
                break;
            }
            }
        }
        pending_[p.seq] = std::move(p);
    }

    static const Object* ObjectOf(const std::vector<ipc::Descriptor>& descriptors, u32 index,
                                  u32 j) {
        for (const ipc::Descriptor& d : descriptors) {
            if (d.index == index) {
                return j < d.objects.size() ? d.objects[j].get() : nullptr;
            }
        }
        return nullptr;
    }

    void Replied(const Request& request, std::span<const u32> words) {
        const auto it = pending_.find(request.Sequence());
        if (it == pending_.end()) {
            return;
        }
        Pending p = std::move(it->second);
        pending_.erase(it);
        const std::vector<u32> reply(words.begin(), words.end());
        const std::shared_ptr<Process> process = request.ClientThread().owner_process.lock();
        std::vector<std::string> outdesc;
        for (const Part& part : ParseCommand(reply)) {
            switch (part.type) {
            case Part::Type::Handles: {
                std::string token;
                for (u32 j = 0; j < part.size; ++j) {
                    const std::size_t at = part.index + 1 + j;
                    std::string name = "-";
                    if (at < reply.size() && reply[at] == 0) {
                        name = "0";
                    } else if (at < reply.size() && process) {
                        if (const auto object = process->handle_table.GetGeneric(reply[at])) {
                            name = NameHandedOut(*object, p.service);
                        }
                    }
                    token += (j != 0 ? "+" : "") + name;
                }
                outdesc.push_back(
                    Describe(part.move ? "move" : "copy", part.index, part.size, token));
                break;
            }
            case Part::Type::Pid:
                outdesc.push_back(Describe("pid", part.index, part.size, "-"));
                break;
            case Part::Type::Static: {
                u32 crc = 0;
                const bool ok = CrcGuest(part.address, part.size, crc, nullptr);
                outdesc.push_back(Describe("static", part.index, part.size, CrcToken(ok, crc)));
                break;
            }
            case Part::Type::Mapped:  // the request's own descriptor covers it
            case Part::Type::Pxi:
                break;
            }
        }
        for (const Part& part : p.in_parts) {
            if (part.type == Part::Type::Mapped && (part.perms & 2) != 0) {
                u32 crc = 0;
                const bool ok = CrcGuest(part.address, part.size, crc, nullptr);
                outdesc.push_back(
                    Describe(MappedKind(part.perms), part.index, part.size, CrcToken(ok, crc)));
            }
        }
        WriteLine(p, &reply, outdesc);
    }

    Runtime& rt_;
    bool full_;
    std::FILE* file_;
    bool main_written_ = false;
    std::map<u32, Pending> pending_;     // by sequence number
    std::map<u32, std::string> names_;   // kernel object id -> the name a reply gave it
    std::map<std::string, u32> counts_;  // "<service>/<kind>" -> how many were handed out
    u32 unnamed_ = 0;
};

void StartIpcTrace(Runtime& rt) {
    const auto path = rt.GetOptions().Get("--trace-ipc");
    const bool full = rt.GetOptions().Has("--trace-ipc-full");
    if (!path) {
        if (full) {
            MH4N_LOG(Error, kLog, "--trace-ipc-full needs --trace-ipc FILE");
            rt.RequestExit(2, "--trace-ipc-full needs --trace-ipc FILE");
        }
        return;
    }
    ServiceManager* manager = FindServiceManager(rt);
    if (manager == nullptr) {
        MH4N_LOG(Error, kLog, "--trace-ipc needs the services phase");
        rt.RequestExit(2, "--trace-ipc needs the services phase");
        return;
    }
    auto tracer = std::make_shared<IpcTracer>(rt, *path, full);
    if (!tracer->ok()) {
        MH4N_LOG(Error, kLog, "cannot write {}", *path);
        rt.RequestExit(2, "cannot write " + *path);
        return;
    }
    tracer->WriteHeader();
    rt.AddSvcHooks(tracer->Hooks());
    manager->AddObserver(tracer->Observer());
    rt.OnFinish([tracer] { tracer->Finish(); });
    rt.Own(tracer);
}

MH4N_REGISTER(BootStep, "tracing.ipc", BootStep{Phase::Tracing, StartIpcTrace});

}  // namespace

}  // namespace mh4n
