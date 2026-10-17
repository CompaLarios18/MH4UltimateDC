// Derived from Azahar src/core/hle/service/service.h, src/core/hle/service/service.cpp,
// src/core/hle/service/sm/sm.h, src/core/hle/service/sm/sm.cpp, src/core/hle/kernel/client_port.cpp,
// src/core/hle/kernel/server_port.cpp, src/core/hle/kernel/server_session.cpp and
// src/core/hle/kernel/svc.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp says
// "GPLv3 or any later version"; the others say "GPLv2 or any later version"): the shape of a service (ServiceFrameworkBase: a name, a maximum number of sessions and a
// table from command id to handler, DefaultMaxSessions, kMaxPortSize), the service manager's checks
// and result codes (ServiceManager, ValidateServiceName, the SRV result codes), the connection limit
// of a port (ClientPort::Connect), the longest name svcConnectToPort takes (PortNameMaxLength) and the
// delay a reply to an HLE service takes (ServerSession::HandleSyncRequest).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (service.h, service.cpp, svc.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (sm.h)
// Copyright 2017 Citra Emulator Project (sm.cpp)
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (client_port.cpp,
//   server_port.cpp, server_session.cpp)
//
// Services and the service manager (N1-10). A service is a `Service`: a name and a table from command
// id (`header >> 16`) to handler. Each session has its own instance, made by a `ServiceFactory`:
//
//   class Echo final : public Service {
//   public:
//       explicit Echo(ServiceContext& context) : Service(context, "echo:u") {
//           Register(0x0001, "Echo", &Echo::Handle1);
//           Register(0x0002, "NotYet");                  // known, not implemented
//       }
//   private:
//       void Handle1(Request& request) { ... }
//   };
//   MH4N_REGISTER(ServiceFactory, "echo:u", [](ServiceContext& c) { return std::make_unique<Echo>(c); });
//
// A name registered this way is a service the application reaches with srv: GetServiceHandle. The two
// named ports (`srv:` and `err:f`, which svcConnectToPort reaches) are registered with
// `ServiceFactory::NamedPort`. A factory gets a ServiceContext: the game and user folders, the options,
// the kernel, the scheduler, the thread manager and the host interfaces, so a service needs nothing
// from the runner. State that all the sessions of a service share lives in `ServiceContext::State`.
//
// An unknown command (or one registered without a handler) is logged once as `unimplemented <service>
// 0x<header>` and answered as Azahar's ServiceFrameworkBase answers it: the command id of the request,
// one normal parameter, the result 0.
//
// The manager is made by the `services` boot step of the runtime (mh4n_ipc), which installs every
// registered factory and the SVC layer's ConnectToPort and SendSyncRequest handlers. `FindServiceManager`
// gives it to a tool that boots a runtime with `Runtime::Prepare`.
//
// Every reply to an HLE service makes the client wait 39 microseconds of game time after the reply is
// written (Azahar's IPCDelayNanoseconds): without it a thread that only talks to services would never
// let a lower-priority thread of its core run. `SetReplyDelayNs(0)` turns it off.
//
// Departures from Azahar:
//  - A port's session count drops when the client closes its last handle to a session (Azahar counts
//    the server session's destruction, which comes with it).
//  - GetServiceHandle for a service with all its sessions open does not wait (Azahar asserts); the
//    caller gets the error.
//
// Threads (principle 6): the manager, the sessions and the services belong to the scheduler thread; a
// service hands work to other host threads itself and answers with `Complete` (ipc.h).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "mh4n/host.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/options.h"
#include "mh4n/types.h"

namespace mh4n {

class GuestScheduler;
class Runtime;
class Service;
class ServiceManager;
class SvcTable;
class ThreadManager;

// ---- limits and result codes ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxServiceNameLength = 8;  // a name registered with srv:
inline constexpr std::size_t kMaxPortNameLength = 11;    // a name svcConnectToPort takes
inline constexpr u32 kDefaultMaxSessions = 10;           // Azahar's DefaultMaxSessions
inline constexpr s64 kIpcReplyDelayNs = 39000;           // see the overview

// The results of src/core/hle/service/sm/sm.h (the values are its comments').
inline constexpr Result kResultSrvServiceNotRegistered = 0xD0406401;
inline constexpr Result kResultSrvMaxConnectionsReached = 0xD0406402;
inline constexpr Result kResultSrvInvalidNameSize = 0xD9006405;
inline constexpr Result kResultSrvAccessDenied = 0xD8E06406;
inline constexpr Result kResultSrvNameContainsNul = 0xD9006407;
inline constexpr Result kResultAlreadyRegistered = 0xD9001BFC;
static_assert(kResultSrvServiceNotRegistered == MakeResult(1, 25, 2, 26));
static_assert(kResultSrvMaxConnectionsReached == MakeResult(2, 25, 2, 26));
static_assert(kResultSrvInvalidNameSize == MakeResult(5, 25, 8, 27));
static_assert(kResultSrvAccessDenied == MakeResult(6, 25, 7, 27));
static_assert(kResultSrvNameContainsNul == MakeResult(7, 25, 8, 27));
static_assert(kResultAlreadyRegistered == MakeResult(1020, 6, 8, 27));

// ---- what the manager is made from -----------------------------------------------------------------------

struct ServiceEnvironment {
    std::string game_folder;
    std::string user_folder;
    const Options* options = nullptr;
    Kernel* kernel = nullptr;
    GuestScheduler* scheduler = nullptr;
    ThreadManager* threads = nullptr;
    HostInterfaces* interfaces = nullptr;
    // Ends the run with `status` (the runtime's RequestExit); empty in a tool that has no run.
    std::function<void(int status, const std::string& reason)> request_exit;
};

// What a factory and a service get. The references stay valid while the manager does.
class ServiceContext {
public:
    explicit ServiceContext(ServiceEnvironment environment);

    const std::string& GameFolder() const { return env_.game_folder; }
    const std::string& UserFolder() const { return env_.user_folder; }
    const Options& GetOptions() const { return *env_.options; }
    Kernel& GetKernel() const { return *env_.kernel; }
    GuestMemory& Memory() const { return env_.kernel->Memory(); }
    GuestScheduler& Scheduler() const { return *env_.scheduler; }
    ThreadManager& Threads() const { return *env_.threads; }
    HostInterfaces& Interfaces() const { return *env_.interfaces; }
    ServiceManager& Services() const { return *manager_; }
    void RequestExit(int status, const std::string& reason) const;

    // State the sessions of one service share, made on first use. `key` is the service's name (or
    // anything the services agree on); asking for one key with two types is a bug.
    template <class T> std::shared_ptr<T> State(std::string_view key) {
        std::shared_ptr<void>& slot = StateSlot(key);
        if (!slot) {
            slot = std::make_shared<T>();
        }
        return std::static_pointer_cast<T>(slot);
    }

    // A session of `service` for the application: the client side to push into a reply. It counts against
    // no port. IPC traces name it `<this service>/<kind>#<k>` with the kind `service->SessionKind()`.
    std::shared_ptr<ClientSession> CreateSession(std::unique_ptr<Service> service);

private:
    friend class ServiceManager;
    std::shared_ptr<void>& StateSlot(std::string_view key);

    ServiceEnvironment env_;
    ServiceManager* manager_ = nullptr;
    std::map<std::string, std::shared_ptr<void>, std::less<>> state_;
};

// ---- a service -----------------------------------------------------------------------------------------

class Service {
public:
    using Handler = std::function<Handled(Request&)>;

    Service(ServiceContext& context, std::string name);
    virtual ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    const std::string& Name() const { return name_; }
    ServiceContext& Context() const { return context_; }
    // How an IPC trace names the sessions this service hands out: "file", "dir", or "session".
    virtual const char* SessionKind() const { return "session"; }

    // Runs the handler of the request's command id; the default is the table below.
    virtual Handled Handle(Request& request);
    // The name a command was registered with, or null.
    const char* CommandName(u16 command_id) const;

protected:
    // A handler as a member function; the second form can answer Handled::Deferred. Registering an id
    // again replaces it.
    template <class Self>
    void Register(u16 command_id, const char* name, void (Self::*handler)(Request&)) {
        AddCommand(command_id, name, [this, handler](Request& request) {
            (static_cast<Self*>(this)->*handler)(request);
            return Handled::Done;
        });
    }
    template <class Self>
    void Register(u16 command_id, const char* name, Handled (Self::*handler)(Request&)) {
        AddCommand(command_id, name, [this, handler](Request& request) {
            return (static_cast<Self*>(this)->*handler)(request);
        });
    }
    // A command the service knows of and does not implement (Azahar's nullptr entries): answered as an
    // unknown one.
    void Register(u16 command_id, const char* name) { AddCommand(command_id, name, nullptr); }

    // The answer to a command nobody handles: logged once, then one normal parameter, the result 0.
    void ReplyUnimplemented(Request& request);

private:
    void AddCommand(u16 command_id, const char* name, Handler handler);

    struct Command {
        const char* name;
        Handler handler;
    };
    ServiceContext& context_;
    std::string name_;
    std::map<u16, Command> commands_;
};

// ---- registering services --------------------------------------------------------------------------------

// Registered by name: MH4N_REGISTER(ServiceFactory, "fs:USER", [](ServiceContext& c) { ... }). `create`
// makes the Service for one session.
struct ServiceFactory {
    using Create = std::function<std::unique_ptr<Service>(ServiceContext&)>;

    Create create;
    u32 max_sessions = kDefaultMaxSessions;  // sessions open at once; more are refused
    bool named_port = false;                 // reached by svcConnectToPort, not by srv:

    ServiceFactory() = default;
    template <class F, class = std::enable_if_t<
                           std::is_invocable_r_v<std::unique_ptr<Service>, F&, ServiceContext&>>>
    ServiceFactory(F&& factory, u32 sessions = kDefaultMaxSessions)  // NOLINT: lambdas convert
        : create(std::forward<F>(factory)), max_sessions(sessions) {}

    template <class F>
    static ServiceFactory NamedPort(F&& factory, u32 sessions = kDefaultMaxSessions) {
        ServiceFactory made(std::forward<F>(factory), sessions);
        made.named_port = true;
        return made;
    }
};

// ---- tracing ------------------------------------------------------------------------------------------------

// Observers see every request on the scheduler thread, in the order the threads sent them.
struct IpcObserver {
    // The request has been translated and is about to reach its service. `translation` is the error
    // that stopped it (the service never sees the request), kResultSuccess otherwise.
    std::function<void(const Request&, Result translation)> sent;
    // The reply is in the client's command buffer; `words` are the words the client reads (the header,
    // the normal parameters and the translate parameters).
    std::function<void(const Request&, std::span<const u32> words)> replied;
};

// ---- the manager ---------------------------------------------------------------------------------------------

class ServiceManager {
public:
    explicit ServiceManager(ServiceEnvironment environment);
    ~ServiceManager();
    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;

    ServiceContext& Context() { return context_; }

    // ---- what exists ----
    // A service for srv: GetServiceHandle. The name has 1 to 8 characters and no NUL (kResultSrv*), and
    // is not taken yet (kResultAlreadyRegistered). Wakes whoever waits for it.
    Result RegisterService(const std::string& name, ServiceFactory factory);
    // A port for svcConnectToPort (at most 11 characters, not taken yet).
    Result AddNamedPort(const std::string& name, ServiceFactory factory);
    // Every entry of Registry<ServiceFactory>, in name order: the named ones as ports, the rest as
    // services.
    void InstallRegistered();
    bool IsServiceRegistered(const std::string& name) const;

    // ---- connecting ----
    // svcConnectToPort: a session of the named port (kResultUnknownPort; kResultMaxConnectionsReached
    // when all the port's sessions are open).
    Result ConnectToPort(const std::string& name, std::shared_ptr<Object>* session);
    // What srv: GetServiceHandle does for a name: the checks of the service manager
    // (kResultSrvInvalidNameSize, kResultSrvNameContainsNul, kResultSrvServiceNotRegistered), then a
    // new session (kResultMaxConnectionsReached).
    Result ConnectToService(const std::string& name, std::shared_ptr<ClientSession>* session);
    // `fn` runs on the scheduler thread, once, the next time `name` is registered.
    void NotifyWhenRegistered(const std::string& name, std::function<void()> fn);
    // A session whose service the caller made (ServiceContext::CreateSession).
    std::shared_ptr<ClientSession> CreateSession(std::unique_ptr<Service> service);

    // ---- svcSendSyncRequest ----
    // The SVC layer's two handlers (svc.h) are these.
    void Install(SvcTable& svc);
    // The request in the command buffer of `thread`, through the session `handle` names. kResultInvalidHandle
    // if it is not a session of the thread's process (nothing else happens); an error code if the
    // translation fails (the thread keeps running); else kResultSuccess. A reply that is ready is in the
    // thread's command buffer; the thread then waits the reply delay (status WaitIPC, woken by the
    // timer), and a deferred reply leaves it in WaitIPC until Complete. The caller (the SVC layer)
    // parks a thread that is not Running. Needs `thread` to have a TLS slot.
    Result SendSyncRequest(Thread& thread, Handle handle);

    void AddObserver(IpcObserver observer);
    void SetReplyDelayNs(s64 ns) { reply_delay_ns_ = ns; }
    s64 ReplyDelayNs() const { return reply_delay_ns_; }

    // Logs `unimplemented <service> 0x<header>` the first time it sees that pair; true if it did.
    bool ReportUnimplemented(const std::string& service, u32 header, const char* command_name);

private:
    friend class ServiceContext;
    struct Entry {
        ServiceFactory factory;
        std::shared_ptr<ServerSession::Port> port;
    };
    struct Observers;

    Result AddEntry(std::map<std::string, Entry>& table, const std::string& name,
                    ServiceFactory factory);
    Result Connect(Entry& entry, std::shared_ptr<ClientSession>* session);
    std::shared_ptr<ClientSession> MakeSession(std::unique_ptr<Service> service,
                                               std::shared_ptr<ServerSession::Port> port,
                                               const std::string& port_name);
    void PruneSessions();

    ServiceContext context_;
    std::map<std::string, Entry> services_;
    std::map<std::string, Entry> ports_;
    std::multimap<std::string, std::function<void()>> waiting_;
    std::vector<std::weak_ptr<ServerSession>> sessions_;
    std::shared_ptr<Observers> observers_;
    std::set<std::pair<std::string, u32>> reported_;
    SvcTable* svc_ = nullptr;
    s64 reply_delay_ns_ = kIpcReplyDelayNs;
    u32 next_sequence_ = 1;
};

// The manager of a runtime whose `services` boot step has run, or null.
ServiceManager* FindServiceManager(Runtime& runtime);

}  // namespace mh4n
