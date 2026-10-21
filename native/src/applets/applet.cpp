// Derived from Azahar src/core/hle/applets/applet.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): Applet::ReceiveParameter,
// Applet::SendParameter and Applet::CloseApplet.
// Copyright 2015 Citra Emulator Project
#include "applets/applet.h"

#include "mh4n/log.h"

namespace mh4n::apt {

namespace {
constexpr const char* kLog = "Service.APT";
}

Result Applet::ReceiveParameter(const MessageParameter& parameter) {
    switch (parameter.signal) {
    case SignalType::Wakeup: {
        const Result result = Start(parameter);
        if (!IsError(result)) {
            is_active_ = true;
        }
        return result;
    }
    case SignalType::WakeupByCancel:
        return Finalize();
    default:
        return ReceiveParameterImpl(parameter);
    }
}

void Applet::SendParameter(const MessageParameter& parameter) {
    if (const std::shared_ptr<AppletHost> host = host_.lock()) {
        host->CancelAndSendParameter(parameter);
    } else {
        MH4N_LOG(Error, kLog, "called after destructing applet manager");
    }
}

void Applet::CloseApplet(std::shared_ptr<Object> object, const std::vector<u8>& buffer) {
    if (const std::shared_ptr<AppletHost> host = host_.lock()) {
        // The results are ignored, as Azahar's: a parameter that is still waiting makes the first
        // one fail, and the closing command then stays what it was.
        host->PrepareToCloseLibraryApplet(true, false, false);
        host->CloseLibraryApplet(std::move(object), buffer);
    } else {
        MH4N_LOG(Error, kLog, "called after destructing applet manager");
    }

    is_active_ = false;
    is_running_ = false;
}

}  // namespace mh4n::apt
