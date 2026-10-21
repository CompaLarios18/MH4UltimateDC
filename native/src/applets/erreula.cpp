// Derived from Azahar src/core/hle/applets/erreula.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): ErrEula::ReceiveParameterImpl, Start and
// Finalize.
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project
#include "applets/erreula.h"

#include <algorithm>
#include <cstring>

#include "mh4n/log.h"

namespace mh4n::apt {

namespace {
constexpr const char* kLog = "Service.APT";
}

Result ErrEula::ReceiveParameterImpl(const MessageParameter& parameter) {
    return AnswerFramebufferRequest(context_, parameter, framebuffer_memory_, "ErrEula");
}

Result ErrEula::Start(const MessageParameter& parameter) {
    std::memcpy(param_.data(), parameter.buffer.data(),
                std::min(parameter.buffer.size(), param_.size()));

    // Do something here, like showing error codes, or prompting for EULA agreement (Azahar's note).
    // Nothing is shown: the player "agrees" to an EULA, and an error is "read".
    if (param_[errlayout::kType] == static_cast<u8>(ErrEulaDisplayType::Agree)) {
        const u32 agreed = 1;
        std::memcpy(param_.data() + errlayout::kResult, &agreed, sizeof(agreed));
    }
    MH4N_LOG(Info, kLog, "the error/EULA applet {:03X} (display type {}) is closed at once",
             static_cast<u32>(id_), static_cast<u32>(param_[errlayout::kType]));

    // Let the application know that we're closing.
    Finalize();
    return kResultSuccess;
}

Result ErrEula::Finalize() {
    CloseApplet(nullptr, std::vector<u8>(param_.begin(), param_.end()));
    return kResultSuccess;
}

}  // namespace mh4n::apt
