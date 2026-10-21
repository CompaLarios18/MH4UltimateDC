// Derived from Azahar src/core/hle/applets/erreula.h and src/core/hle/applets/erreula.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later
// version"): the layout of the parameter (0xF80 bytes) and its display types, and the applet's
// behaviour (the Request message, Start, Finalize).
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project
//
// The error and EULA applet (N1-14c, decision D43), HLE as in Azahar and made for the ids Error
// (0x206) and Error2 (0x406). Azahar's AppletId enumeration has no separate EULA ids: the EULA
// screen is this applet with the Agree display type, and 0x207/0x407 are the Mint applet's. The
// applet does not show anything: a game that wants to tell the player of an error (a network error,
// say) gets the dialog "closed" at once, and goes on instead of waiting or retrying for ever.
//
//  - PrepareToStartLibraryApplet makes the applet; the Request parameter gets the Response with a
//    shared memory block of the framebuffer's size (Applet::AnswerFramebufferRequest);
//  - StartLibraryApplet's Wakeup parameter holds the 0xF80-byte parameter. The applet copies it (at most
//    that many bytes), sets the result to 1 for the Agree display type (the player "agreed"; the
//    agreed EULA version stays what the game put there) and closes at once, inside the command: the
//    game finds the closing parameter (WakeupByExit from the applet, the 0xF80 bytes with the result)
//    waiting when StartLibraryApplet returns;
//  - WakeupByCancel closes the applet with the parameter as it is (all zero if it never started).
//
// Departures from Azahar, so that a bad input does not crash: a Request parameter of the wrong size
// is logged and answered with kResultUnknown (Azahar asserts), a shared memory block that cannot be
// made is logged and the Response carries no object.
//
// Threads (principle 6): scheduler thread only.
#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "applets/applet.h"

namespace mh4n::apt {

// The layout of the parameter (3dbrew, Error applet), byte by byte.
inline constexpr std::size_t kErrEulaMessageSize = 1900;  // UTF-16 units

enum class ErrEulaDisplayType : u8 { ErrorCode = 0, Error = 1, Agree = 5 };

namespace errlayout {
inline constexpr std::size_t kType = 0x000;
inline constexpr std::size_t kErrorCode = 0x004;
inline constexpr std::size_t kLanguage = 0x00A;
inline constexpr std::size_t kMessage = 0x00C;
inline constexpr std::size_t kAllowHomeButton = 0xEE4;
inline constexpr std::size_t kLaunchSystemSettings = 0xEE6;
inline constexpr std::size_t kResult = 0xF70;
inline constexpr std::size_t kAgreedEulaMinor = 0xF74;
inline constexpr std::size_t kAgreedEulaMajor = 0xF75;
inline constexpr std::size_t kSize = 0xF80;
}  // namespace errlayout

class ErrEula final : public Applet {
public:
    ErrEula(AppletId id, AppletId parent, std::weak_ptr<AppletHost> host, ServiceContext& context)
        : Applet(id, parent, std::move(host)), context_(context) {}

    Result ReceiveParameterImpl(const MessageParameter& parameter) override;
    Result Start(const MessageParameter& parameter) override;
    Result Finalize() override;
    void Update() override {}

    // The parameter the applet holds, as the game gets it back (tests).
    const std::array<u8, errlayout::kSize>& Param() const { return param_; }

private:
    ServiceContext& context_;
    // Made when the Request message arrives: it holds the framebuffer info the game gets with
    // ImportDisplayCaptureInfo.
    std::shared_ptr<SharedMemory> framebuffer_memory_;
    // The parameter received on start (output fields included), zero until then.
    std::array<u8, errlayout::kSize> param_{};
};

}  // namespace mh4n::apt
