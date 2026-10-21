// Derived from Azahar src/core/hle/applets/mint.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): Mint::ReceiveParameterImpl, Start and
// Finalize.
// Copyright 2016 Citra Emulator Project
#include "applets/mint.h"

#include "mh4n/log.h"

namespace mh4n::apt {

namespace {
constexpr const char* kLog = "Service.APT";
}

Result Mint::ReceiveParameterImpl(const MessageParameter& parameter) {
    return AnswerFramebufferRequest(context_, parameter, framebuffer_memory_, "Mint");
}

Result Mint::Start(const MessageParameter& parameter) {
    startup_param_ = parameter.buffer;

    // TODO(Subv): Set the expected fields in the response buffer before resending it to the
    // application.
    // TODO(Subv): Reverse the parameter format for the Mint applet (Azahar's notes)
    MH4N_LOG(Info, kLog, "the Mint applet {:03X} is closed at once", static_cast<u32>(id_));

    // Let the application know that we're closing
    Finalize();
    return kResultSuccess;
}

Result Mint::Finalize() {
    CloseApplet(nullptr, std::vector<u8>(startup_param_.size(), 0));
    return kResultSuccess;
}

}  // namespace mh4n::apt
