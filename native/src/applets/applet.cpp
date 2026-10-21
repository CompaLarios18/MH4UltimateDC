// Derived from Azahar src/core/hle/applets/applet.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): Applet::ReceiveParameter,
// Applet::SendParameter and Applet::CloseApplet.
// Copyright 2015 Citra Emulator Project
#include "applets/applet.h"

#include <cstring>
#include <string>

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

Result Applet::AnswerFramebufferRequest(ServiceContext& context, const MessageParameter& parameter,
                                        std::shared_ptr<SharedMemory>& memory, const char* name) {
    if (parameter.signal != SignalType::Request) {
        MH4N_LOG(Error, kLog, "unsupported signal {:08X}", static_cast<u32>(parameter.signal));
        // TODO(Subv): Find the right error code (Azahar's too)
        return kResultUnknown;
    }

    // The message contains a buffer with the size of the framebuffer shared memory.
    if (parameter.buffer.size() != sizeof(CaptureBufferInfo)) {
        // Azahar asserts.
        MH4N_LOG(Error, kLog, "the Request parameter of {} has {} bytes, not {}", name,
                 parameter.buffer.size(), sizeof(CaptureBufferInfo));
        return kResultUnknown;
    }
    CaptureBufferInfo capture_info;
    std::memcpy(&capture_info, parameter.buffer.data(), sizeof(capture_info));

    // Create the SharedMemory that will hold the framebuffer data: a block of the system region.
    memory = context.GetKernel().NewSharedMemoryForApplet(
        0, capture_info.size, MemoryPermission::ReadWrite, MemoryPermission::ReadWrite,
        std::string(name) + " Memory");
    if (!memory) {
        // Azahar: "Not enough space in region to allocate shared memory!" (an assertion).
        MH4N_LOG(Error, kLog, "cannot make the framebuffer shared memory of {} bytes for {}",
                 capture_info.size, name);
    }

    // Send the response message with the newly created SharedMemory.
    SendParameter({
        .sender_id = id_,
        .destination_id = parent_,
        .signal = SignalType::Response,
        .object = memory,
        .buffer = {},
    });
    return kResultSuccess;
}

}  // namespace mh4n::apt
