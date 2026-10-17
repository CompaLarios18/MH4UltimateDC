// Derived from Azahar src/core/hle/service/sm/srv.cpp, src/core/hle/service/sm/srv.h and
// src/core/hle/service/sm/sm.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of
// them says "GPLv2 or any later version"): the commands of the srv: port, their command ids, replies
// and result codes (SRV::RegisterClient, EnableNotification, GetServiceHandle with its wait flag and
// the delayed reply, Subscribe, Unsubscribe, PublishToSubscriber), and the table of command names.
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (srv.cpp)
// Copyright 2014 Citra Emulator Project (srv.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (sm.h)
//
// The srv: port (N1-10): the application's way to the services. See mh4n/service.h for the manager.
//
// Departures from Azahar:
//  - RegisterService (0x03) and the other commands for the servers of system modules are not
//    implemented: no application sends them, and there are no server ports to hand out. They are
//    answered as unknown commands.
//  - ReceiveNotification answers "nothing pending" explicitly (Azahar has no handler, and answers an
//    unknown command: the same words) and does not log an error.
//  - GetServiceHandle for a service with all its sessions open answers with the error; Azahar asserts.
//  - The notification semaphore belongs to the session (Azahar's SRV object is one for all of them).
#include <array>
#include <memory>
#include <string>

#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.SRV";

// The notification semaphore's maximum count.
constexpr s32 kMaxPendingNotifications = 16;

// The notification that powers the console off (3dbrew "NS and Home Menu"): Azahar shuts down on it,
// "as homebrew may use it to power off the console".
constexpr u32 kNotificationShutdown = 0x203;

class Srv final : public Service {
public:
    explicit Srv(ServiceContext& context) : Service(context, "srv:") {
        Register(0x0001, "RegisterClient", &Srv::RegisterClient);
        Register(0x0002, "EnableNotification", &Srv::EnableNotification);
        Register(0x0003, "RegisterService");
        Register(0x0004, "UnregisterService");
        Register(0x0005, "GetServiceHandle", &Srv::GetServiceHandle);
        Register(0x0006, "RegisterPort");
        Register(0x0007, "UnregisterPort");
        Register(0x0008, "GetPort");
        Register(0x0009, "Subscribe", &Srv::Subscribe);
        Register(0x000A, "Unsubscribe", &Srv::Unsubscribe);
        Register(0x000B, "ReceiveNotification", &Srv::ReceiveNotification);
        Register(0x000C, "PublishToSubscriber", &Srv::PublishToSubscriber);
        Register(0x000D, "PublishAndGetSubscriber");
        Register(0x000E, "IsServiceRegistered");
    }

private:
    // Inputs:  0: 0x00010002, 1: the calling PID descriptor (0x20), 2: the PID (filled in)
    // Outputs: 0: 0x00010040, 1: result
    void RegisterClient(Request& request) {
        const u32 pid_descriptor = request.Pop<u32>();
        if (pid_descriptor != ipc::CallingPidDesc()) {
            request.MakeReply(1, 0).Push(kResultInvalidBufferDescriptor);
            return;
        }
        const u32 caller_pid = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) RegisterClient called. Caller PID={}", caller_pid);
    }

    // Inputs:  0: 0x00020000
    // Outputs: 0: 0x00020042, 1: result, 2: the translation descriptor 0x00000000, 3: the semaphore
    void EnableNotification(Request& request) {
        std::shared_ptr<Semaphore> semaphore;
        const Result created = Context().GetKernel().NewSemaphore(
            &semaphore, 0, kMaxPendingNotifications, "SRV:Notification");
        notification_semaphore_ = semaphore;
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(created);
        reply.PushCopyObjects(semaphore);
        MH4N_LOG(Warning, kLog, "(STUBBED) EnableNotification called");
    }

    // Inputs:  0: 0x00050100, 1-2: the service name (8 bytes, UTF-8), 3: its length, 4: flags (bit 0:
    //          if not set, wait for the service to be registered)
    // Outputs: 0: 0x00050042 (0x00050040 on an error), 1: result, 2: the translation descriptor,
    //          3: the session handle
    Handled GetServiceHandle(Request& request) {
        const auto name_buf = request.PopRaw<std::array<char, 8>>();
        const std::size_t name_len = request.Pop<u32>();
        const u32 flags = request.Pop<u32>();
        const bool wait_until_available = (flags & 1) == 0;

        if (name_len > kMaxServiceNameLength) {
            request.MakeReply(1, 0).Push(kResultSrvInvalidNameSize);
            MH4N_LOG(Error, kLog,
                     "GetServiceHandle called with name_len=0x{:X} -> invalid name size", name_len);
            return Handled::Done;
        }
        const std::string name(name_buf.data(), name_len);

        ServiceManager& manager = Context().Services();
        std::shared_ptr<ClientSession> session;
        const Result result = manager.ConnectToService(name, &session);
        if (IsError(result)) {
            if (wait_until_available && result == kResultSrvServiceNotRegistered) {
                MH4N_LOG(Info, kLog, "GetServiceHandle service={} delayed", name);
                manager.NotifyWhenRegistered(name, [&manager, pending = request.Share(), name] {
                    FinishDelayed(manager, pending, name);
                });
                return Handled::Deferred;
            }
            request.MakeReply(1, 0).Push(result);
            MH4N_LOG(Error, kLog, "GetServiceHandle service={} -> error 0x{:08X}", name, result);
            return Handled::Done;
        }
        MH4N_LOG(Debug, kLog, "GetServiceHandle service={} -> session={}", name,
                 session->ObjectId());
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(result);
        reply.PushMoveObjects(std::move(session));
        return Handled::Done;
    }

    // The service registered while the client waited (Azahar's ThreadCallback): connect and answer.
    static void FinishDelayed(ServiceManager& manager, const std::shared_ptr<Request>& pending,
                              const std::string& name) {
        MH4N_LOG(Info, kLog, "GetServiceHandle service={} wakeup", name);
        std::shared_ptr<ClientSession> session;
        const Result result = manager.ConnectToService(name, &session);
        if (IsError(result)) {
            MH4N_LOG(Error, kLog, "GetServiceHandle service={} -> error 0x{:08X}", name, result);
            Reply reply(0x5, 1, 0);
            reply.Push(result);
            Complete(pending, std::move(reply));
            return;
        }
        MH4N_LOG(Debug, kLog, "GetServiceHandle service={} -> session={}", name,
                 session->ObjectId());
        Reply reply(0x5, 1, 2);
        reply.Push(result);
        reply.PushMoveObjects(std::move(session));
        Complete(pending, std::move(reply));
    }

    // Inputs:  0: 0x00090040, 1: the notification id
    // Outputs: 0: 0x00090040, 1: result
    void Subscribe(Request& request) {
        const u32 notification_id = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) Subscribe called, notification_id=0x{:X}",
                 notification_id);
    }

    // Inputs:  0: 0x000A0040, 1: the notification id
    // Outputs: 0: 0x000A0040, 1: result
    void Unsubscribe(Request& request) {
        const u32 notification_id = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) Unsubscribe called, notification_id=0x{:X}",
                 notification_id);
    }

    // Outputs: 0: 0x000B0040, 1: result. No notification is ever pending.
    void ReceiveNotification(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Debug, kLog, "ReceiveNotification: nothing pending");
    }

    // Inputs:  0: 0x000C0080, 1: the notification id, 2: flags (bit 0: only fire if not fired, bit 1:
    //          report errors)
    // Outputs: 0: 0x000C0040, 1: result
    void PublishToSubscriber(Request& request) {
        const u32 notification_id = request.Pop<u32>();
        const u8 flags = request.Pop<u8>();
        if (notification_id == kNotificationShutdown) {
            Context().RequestExit(0, "srv: PublishToSubscriber 0x203 (power off)");
        } else {
            MH4N_LOG(Warning, kLog,
                     "(STUBBED) PublishToSubscriber called, notification_id=0x{:X}, flags={}",
                     notification_id, flags);
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    std::shared_ptr<Semaphore> notification_semaphore_;
};

MH4N_REGISTER(ServiceFactory, "srv:",
              ServiceFactory::NamedPort(
                  [](ServiceContext& context) { return std::make_unique<Srv>(context); }, 64));

} // namespace

} // namespace mh4n
