// Derived from Azahar src/core/hle/service/service.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): ServiceFrameworkBase::HandleSyncRequest
// and ReportUnimplementedFunction (what a command nobody handles is answered with).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
//
// Service and ServiceContext (N1-10). See mh4n/service.h.
#include "mh4n/service.h"

#include "mh4n/log.h"

namespace mh4n {

// ---- ServiceContext -----------------------------------------------------------------------------------------

ServiceContext::ServiceContext(ServiceEnvironment environment) : env_(std::move(environment)) {}

void ServiceContext::RequestExit(int status, const std::string& reason) const {
    if (env_.request_exit) {
        env_.request_exit(status, reason);
    } else {
        MH4N_LOG(Critical, "Service", "exit {} requested with nothing to stop: {}", status, reason);
    }
}

std::shared_ptr<void>& ServiceContext::StateSlot(std::string_view key) {
    return state_.try_emplace(std::string(key)).first->second;
}

std::shared_ptr<ClientSession> ServiceContext::CreateSession(std::unique_ptr<Service> service) {
    return manager_->CreateSession(std::move(service));
}

// ---- Service ---------------------------------------------------------------------------------------------------

Service::Service(ServiceContext& context, std::string name)
    : context_(context), name_(std::move(name)) {}

Service::~Service() = default;

void Service::AddCommand(u16 command_id, const char* name, Handler handler) {
    commands_[command_id] = Command{name, std::move(handler)};
}

const char* Service::CommandName(u16 command_id) const {
    const auto it = commands_.find(command_id);
    return it == commands_.end() ? nullptr : it->second.name;
}

Handled Service::Handle(Request& request) {
    const auto it = commands_.find(request.CommandId());
    if (it == commands_.end() || !it->second.handler) {
        ReplyUnimplemented(request);
        return Handled::Done;
    }
    return it->second.handler(request);
}

void Service::ReplyUnimplemented(Request& request) {
    context_.Services().ReportUnimplemented(name_, request.Header(),
                                            CommandName(request.CommandId()));
    // Azahar: the header keeps the command id with one normal parameter and no translate parameters,
    // and the result is 0 ("Hack - ignore error").
    request.MakeReply(1, 0).Push(kResultSuccess);
}

}  // namespace mh4n
