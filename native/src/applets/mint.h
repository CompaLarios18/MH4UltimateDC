// Derived from Azahar src/core/hle/applets/mint.h and src/core/hle/applets/mint.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later
// version"): the applet's behaviour (the Request message, Start, Finalize).
// Copyright 2016 Citra Emulator Project
//
// The Mint applet (N1-14c, decision D43), HLE as in Azahar for the ids Mint (0x207) and Mint2 (0x407):
// it accepts the parameter and closes at once, answering a buffer of zeros of the size of the
// parameter it got (Azahar does not know the format: "TODO(Subv): Reverse the parameter format for
// the Mint applet"). The flow is the one of the error applet (erreula.h).
//
// Departures from Azahar: as the error applet's (a Request parameter of the wrong size is logged and
// answered with kResultUnknown instead of an assertion).
//
// Threads (principle 6): scheduler thread only.
#pragma once

#include <memory>
#include <vector>

#include "applets/applet.h"

namespace mh4n::apt {

class Mint final : public Applet {
public:
    Mint(AppletId id, AppletId parent, std::weak_ptr<AppletHost> host, ServiceContext& context)
        : Applet(id, parent, std::move(host)), context_(context) {}

    Result ReceiveParameterImpl(const MessageParameter& parameter) override;
    Result Start(const MessageParameter& parameter) override;
    Result Finalize() override;
    void Update() override {}

private:
    ServiceContext& context_;
    // Made when the Request message arrives (the framebuffer info of ImportDisplayCaptureInfo).
    std::shared_ptr<SharedMemory> framebuffer_memory_;
    // Parameter received by the applet on start: only its size matters.
    std::vector<u8> startup_param_;
};

}  // namespace mh4n::apt
