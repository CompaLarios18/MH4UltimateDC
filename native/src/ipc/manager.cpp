// Derived from Azahar src/core/hle/service/sm/sm.cpp, src/core/hle/kernel/client_port.cpp,
// src/core/hle/kernel/client_session.cpp, src/core/hle/kernel/server_session.cpp and
// src/core/hle/kernel/svc.cpp (ConnectToPort, SendSyncRequest) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: svc.cpp says "GPLv3 or any later version"; the others say GPLv2 or any later
// version): ServiceManager (ValidateServiceName, RegisterService, GetServicePort, ConnectToService),
// ClientPort::Connect and its session limit, the checks of svcConnectToPort and svcSendSyncRequest,
// ServerSession::HandleSyncRequest (translate, call the handler, write the reply, the wait that
// follows an HLE reply) and the destruction of a client session.
// Copyright 2017 Citra Emulator Project (sm.cpp)
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (client_port.cpp,
//   client_session.cpp, server_session.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp)
//
// The service manager, sessions and SendSyncRequest (N1-10). See mh4n/service.h.
#include <algorithm>

#include "internal.h"
#include "mh4n/assert.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/scheduler.h"
#include "mh4n/service.h"
#include "mh4n/svc.h"
#include "mh4n/threads.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service";

Result ValidateServiceName(const std::string& name) {
    if (name.empty() || name.size() > kMaxServiceNameLength) {
        return kResultSrvInvalidNameSize;
    }
    if (name.find('\0') != std::string::npos) {
        return kResultSrvNameContainsNul;
    }
    return kResultSuccess;
}

}  // namespace

// ---- sessions ----------------------------------------------------------------------------------------------------

struct ServerSession::Port {
    std::string name;
    u32 max_sessions = kDefaultMaxSessions;
    u32 active = 0;  // sessions open now
};

ServerSession::~ServerSession() {
    Close();
}

void ServerSession::Close() {
    service_.reset();
    if (port_) {
        if (port_->active > 0) {
            --port_->active;
        }
        port_.reset();
    }
}

ClientSession::ClientSession(Kernel& kernel, std::shared_ptr<ServerSession> server,
                             std::string port_name)
    : Object(kernel), server_(std::move(server)), port_name_(std::move(port_name)) {
    SetName(port_name_);
}

// ---- the manager ---------------------------------------------------------------------------------------------------

struct ServiceManager::Observers {
    std::vector<IpcObserver> list;
};

ServiceManager::ServiceManager(ServiceEnvironment environment)
    : context_(std::move(environment)), observers_(std::make_shared<Observers>()) {
    MH4N_ASSERT(context_.env_.kernel != nullptr && context_.env_.scheduler != nullptr &&
                    context_.env_.options != nullptr,
                "a service manager needs the kernel, the scheduler and the options");
    context_.manager_ = this;
}

ServiceManager::~ServiceManager() {
    detail::ForgetServiceManager(this);
    // The sessions the application still holds outlive this object: their services go now, while the
    // kernel and everything the services refer to are still there.
    for (const std::weak_ptr<ServerSession>& weak : sessions_) {
        if (const std::shared_ptr<ServerSession> session = weak.lock()) {
            session->Close();
        }
    }
}

Result ServiceManager::AddEntry(std::map<std::string, Entry>& table, const std::string& name,
                                ServiceFactory factory) {
    if (table.find(name) != table.end()) {
        return kResultAlreadyRegistered;
    }
    if (!factory.create) {
        MH4N_LOG(Error, kLog, "'{}' was registered without a factory", name);
        return kResultSrvAccessDenied;
    }
    auto port = std::make_shared<ServerSession::Port>();
    port->name = name;
    port->max_sessions = factory.max_sessions;
    table.emplace(name, Entry{std::move(factory), std::move(port)});
    return kResultSuccess;
}

Result ServiceManager::RegisterService(const std::string& name, ServiceFactory factory) {
    if (const Result result = ValidateServiceName(name); IsError(result)) {
        return result;
    }
    if (const Result result = AddEntry(services_, name, std::move(factory)); IsError(result)) {
        return result;
    }
    // Whoever asked for the service before it existed gets it now.
    std::vector<std::function<void()>> woken;
    const auto range = waiting_.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        woken.push_back(std::move(it->second));
    }
    waiting_.erase(range.first, range.second);
    for (std::function<void()>& fn : woken) {
        fn();
    }
    return kResultSuccess;
}

Result ServiceManager::AddNamedPort(const std::string& name, ServiceFactory factory) {
    if (name.empty() || name.size() > kMaxPortNameLength) {
        return kResultSrvInvalidNameSize;
    }
    return AddEntry(ports_, name, std::move(factory));
}

void ServiceManager::InstallRegistered() {
    for (const auto& [name, factory] : Registry<ServiceFactory>::Instance().All()) {
        const Result result =
            factory.named_port ? AddNamedPort(name, factory) : RegisterService(name, factory);
        if (IsError(result)) {
            MH4N_LOG(Error, kLog, "cannot install the service '{}': 0x{:08X}", name, result);
        }
    }
}

bool ServiceManager::IsServiceRegistered(const std::string& name) const {
    return services_.find(name) != services_.end();
}

void ServiceManager::NotifyWhenRegistered(const std::string& name, std::function<void()> fn) {
    waiting_.emplace(name, std::move(fn));
}

void ServiceManager::PruneSessions() {
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
                       [](const std::weak_ptr<ServerSession>& s) { return s.expired(); }),
        sessions_.end());
}

std::shared_ptr<ClientSession>
ServiceManager::MakeSession(std::unique_ptr<Service> service,
                            std::shared_ptr<ServerSession::Port> port,
                            const std::string& port_name) {
    std::shared_ptr<ServerSession> server(new ServerSession());
    server->name_ = port_name.empty() ? service->Name() : port_name;
    server->service_ = std::move(service);
    server->port_ = std::move(port);
    if (server->port_) {
        ++server->port_->active;
    }
    if (sessions_.size() >= 64 && (sessions_.size() & (sessions_.size() - 1)) == 0) {
        PruneSessions();
    }
    sessions_.push_back(server);
    return std::make_shared<ClientSession>(context_.GetKernel(), std::move(server), port_name);
}

std::shared_ptr<ClientSession> ServiceManager::CreateSession(std::unique_ptr<Service> service) {
    MH4N_ASSERT(service != nullptr, "a session needs a service");
    return MakeSession(std::move(service), nullptr, {});
}

Result ServiceManager::Connect(Entry& entry, std::shared_ptr<ClientSession>* session) {
    // ClientPort::Connect: the port refuses when all its sessions are open.
    if (entry.port->active >= entry.port->max_sessions) {
        return kResultMaxConnectionsReached;
    }
    std::unique_ptr<Service> service = entry.factory.create(context_);
    if (!service) {
        MH4N_LOG(Error, kLog, "the factory of '{}' made no service", entry.port->name);
        return kResultSrvAccessDenied;
    }
    *session = MakeSession(std::move(service), entry.port, entry.port->name);
    return kResultSuccess;
}

Result ServiceManager::ConnectToPort(const std::string& name, std::shared_ptr<Object>* session) {
    const auto it = ports_.find(name);
    if (it == ports_.end()) {
        return kResultUnknownPort;
    }
    std::shared_ptr<ClientSession> client;
    if (const Result result = Connect(it->second, &client); IsError(result)) {
        return result;
    }
    *session = std::move(client);
    return kResultSuccess;
}

Result ServiceManager::ConnectToService(const std::string& name,
                                        std::shared_ptr<ClientSession>* session) {
    if (const Result result = ValidateServiceName(name); IsError(result)) {
        return result;
    }
    const auto it = services_.find(name);
    if (it == services_.end()) {
        return kResultSrvServiceNotRegistered;
    }
    return Connect(it->second, session);
}

void ServiceManager::AddObserver(IpcObserver observer) {
    observers_->list.push_back(std::move(observer));
}

bool ServiceManager::ReportUnimplemented(const std::string& service, u32 header,
                                         const char* command_name) {
    if (!reported_.emplace(service, header).second) {
        return false;
    }
    // The command's parameters are not logged: they can hold what the player typed.
    MH4N_LOG(Error, kLog, "unimplemented {} 0x{:08x} (command 0x{:04X}{}{})", service, header,
             ipc::Header{header}.CommandId(), command_name != nullptr ? ", " : "",
             command_name != nullptr ? command_name : "");
    return true;
}

// ---- svcSendSyncRequest -------------------------------------------------------------------------------------------

void ServiceManager::Install(SvcTable& svc) {
    svc_ = &svc;
    svc.SetConnectToPortHandler([this](const std::string& name, std::shared_ptr<Object>* session) {
        return ConnectToPort(name, session);
    });
    svc.SetSendSyncRequestHandler(
        [this](Thread& thread, Handle handle) { return SendSyncRequest(thread, handle); });
}

Result ServiceManager::SendSyncRequest(Thread& thread, Handle handle) {
    Kernel& kernel = context_.GetKernel();
    std::shared_ptr<Process> process = thread.owner_process.lock();
    if (!process) {
        process = kernel.CurrentProcess();
    }
    const std::shared_ptr<ClientSession> session =
        process ? process->handle_table.Get<ClientSession>(handle) : nullptr;
    if (!session) {
        return kResultInvalidHandle;
    }
    const std::shared_ptr<ServerSession> server = session->Server();
    if (server->Closed()) {
        return kResultSessionClosed;
    }
    if (thread.tls_address == 0) {
        MH4N_LOG(Error, kLog, "thread {} has no command buffer", thread.thread_id);
        return kResultInvalidBufferDescriptor;
    }

    std::array<u32, ipc::kCommandAreaLength> area{};
    if (!kernel.Memory().ReadBlock(area.data(), CommandBufferAddress(thread),
                                   static_cast<u32>(sizeof(area)))) {
        MH4N_LOG(Error, kLog, "thread {}'s command buffer (0x{:08X}) is not mapped",
                 thread.thread_id, CommandBufferAddress(thread));
        return kResultInvalidBufferDescriptor;
    }
    const auto thread_ptr = std::static_pointer_cast<Thread>(thread.shared_from_this());
    const auto request = std::make_shared<Request>(kernel, thread_ptr, session, next_sequence_++);
    const Result translated = request->Translate(area);
    for (const IpcObserver& observer : observers_->list) {
        if (observer.sent) {
            observer.sent(*request, translated);
        }
    }
    if (IsError(translated)) {
        return translated;
    }
    request->SetRepliedHook(
        [observers = observers_](const Request& done, std::span<const u32> words) {
            for (const IpcObserver& observer : observers->list) {
                if (observer.replied) {
                    observer.replied(done, words);
                }
            }
        });

    Service& service = *server->GetService();
    Handled handled;
    {
        GuestScheduler& scheduler = context_.Scheduler();
        std::optional<ServiceScope> scope;
        if (scheduler.CurrentThread() != nullptr) {
            scope.emplace(scheduler, service.Name());
        }
        handled = service.Handle(*request);
    }

    if (handled == Handled::Deferred) {
        // The client waits until Complete. A thread that is terminated meanwhile cancels the request.
        thread.status = ThreadStatus::WaitIPC;
        thread.wake_hook = [weak = std::weak_ptr<Request>(request)](ThreadWakeupReason reason,
                                                                    WaitObject*,
                                                                    std::optional<Result>&, s32&) {
            if (reason == ThreadWakeupReason::Terminated) {
                if (const std::shared_ptr<Request> pending = weak.lock()) {
                    pending->Cancel();
                }
            }
        };
        return kResultSuccess;
    }

    if (!request->HasReply()) {
        MH4N_LOG(Error, kLog, "{} 0x{:08x}: the handler returned without a reply", service.Name(),
                 request->Header());
        request->MakeReply(1, 0).Push(kResultSuccess);
    }
    const std::vector<u32> words = request->WriteReply(*request->GetReply());
    request->MarkFinished();
    for (const IpcObserver& observer : observers_->list) {
        if (observer.replied) {
            observer.replied(*request, words);
        }
    }
    // A service may have kept a copy (Share) of a request it answered at once.
    request->ReleaseKernelObjects();
    if (reply_delay_ns_ > 0) {
        // Azahar's IPC overhead: the client sleeps a moment after the reply (see service.h).
        thread.status = ThreadStatus::WaitIPC;
        thread.ArmTimeout(reply_delay_ns_);
    }
    return kResultSuccess;
}

}  // namespace mh4n
