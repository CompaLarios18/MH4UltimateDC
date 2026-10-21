// Derived from Azahar src/core/hle/applets/applet.h, src/core/hle/applets/applet.cpp,
// src/core/hle/service/apt/applet_manager.h, src/core/hle/service/apt/applet_manager.cpp,
// src/core/hle/service/apt/errors.h, src/core/hle/result.h and src/core/core_timing.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version" or "GPLv2+"): the signal, notification and applet-id enumerations, the message
// parameter, the capture buffer info layout, the result codes of the applet module (and the
// numbers of result.h they are made of), the applets' update period, and the shape and behaviour of
// an HLE applet (Applet::ReceiveParameter's dispatch on the signal, SendParameter, CloseApplet).
// Copyright 2015 Citra Emulator Project (applet.h, applet.cpp)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (applet_manager.h,
//   applet_manager.cpp)
// Copyright 2018-2025 Citra Emulator Project / Azahar Emulator Project (errors.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (result.h)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
//
// What an HLE library applet is and what it sees of APT (N1-14). The applet manager
// (services/apt) implements `AppletHost`; an applet (swkbd.h) derives from `Applet`. The types here
// are the ones both sides share, so the applets module does not depend on the service.
//
// Threads (principle 6): everything here belongs to the scheduler thread.
#pragma once

#include <memory>
#include <vector>

#include "mh4n/kernel.h"
#include "mh4n/service.h"
#include "mh4n/types.h"

namespace mh4n::apt {

// ---- enumerations (applet_manager.h) -------------------------------------------------------------

// Signals used by APT functions.
enum class SignalType : u32 {
    None = 0x0,
    Wakeup = 0x1,
    Request = 0x2,
    Response = 0x3,
    Exit = 0x4,
    Message = 0x5,
    HomeButtonSingle = 0x6,
    HomeButtonDouble = 0x7,
    DspSleep = 0x8,
    DspWakeup = 0x9,
    WakeupByExit = 0xA,
    WakeupByPause = 0xB,
    WakeupByCancel = 0xC,
    WakeupByCancelAll = 0xD,
    WakeupByPowerButtonClick = 0xE,
    WakeupToJumpHome = 0xF,
    RequestForSysApplet = 0x10,
    WakeupToLaunchApplication = 0x11,
};

enum class Notification : u32 {
    None = 0,
    HomeButtonSingle = 1,
    HomeButtonDouble = 2,
    SleepQuery = 3,
    SleepCancelledByOpen = 4,
    SleepAccepted = 5,
    SleepAwake = 6,
    Shutdown = 7,
    PowerButtonClick = 8,
    PowerButtonClear = 9,
    TrySleep = 10,
    OrderToClose = 11,
};

// App ids used by APT functions.
enum class AppletId : u32 {
    None = 0,
    AnySystemApplet = 0x100,
    HomeMenu = 0x101,
    AlternateMenu = 0x103,
    Camera = 0x110,
    FriendList = 0x112,
    GameNotes = 0x113,
    InternetBrowser = 0x114,
    InstructionManual = 0x115,
    Notifications = 0x116,
    Miiverse = 0x117,
    MiiversePost = 0x118,
    AmiiboSettings = 0x119,
    AnySysLibraryApplet = 0x200,
    SoftwareKeyboard1 = 0x201,
    Ed1 = 0x202,
    PnoteApp = 0x204,
    SnoteApp = 0x205,
    Error = 0x206,
    Mint = 0x207,
    Extrapad = 0x208,
    Memolib = 0x209,
    Application = 0x300,
    Tiger = 0x301,
    AnyLibraryApplet = 0x400,
    SoftwareKeyboard2 = 0x401,
    Ed2 = 0x402,
    PnoteApp2 = 0x404,
    SnoteApp2 = 0x405,
    Error2 = 0x406,
    Mint2 = 0x407,
    Extrapad2 = 0x408,
    Memolib2 = 0x409,
    TypeMask = 0xF00,
};

// ---- result codes (errors.h, result.h: ErrorModule::Applet is 51) ------------------------------------

inline constexpr u32 kModuleApplet = 51;
// ErrorSummary and ErrorLevel values of result.h.
inline constexpr u32 kSummaryNotFound = 4;
inline constexpr u32 kSummaryInvalidState = 5;
inline constexpr u32 kSummaryNotSupported = 6;
inline constexpr u32 kLevelStatus = 25;
inline constexpr u32 kLevelPermanent = 27;
// ErrorDescription values of result.h and the codes of errors.h.
inline constexpr u32 kDescNoData = 1007;
inline constexpr u32 kDescNotFound = 1018;
inline constexpr u32 kDescAlreadyExists = 1020;
inline constexpr u32 kErrParameterPresent = 2;
inline constexpr u32 kErrInvalidAppletSlot = 4;

// A parameter is waiting that nobody has received (SendParameter, PrepareToStart..., ...).
inline constexpr Result kResultParameterPresent =
    MakeResult(kErrParameterPresent, kModuleApplet, kSummaryInvalidState, kLevelStatus);
inline constexpr Result kResultInvalidAppletSlot =
    MakeResult(kErrInvalidAppletSlot, kModuleApplet, kSummaryInvalidState, kLevelStatus);
inline constexpr Result kResultAlreadyExists =
    MakeResult(kDescAlreadyExists, kModuleApplet, kSummaryInvalidState, kLevelStatus);
inline constexpr Result kResultNoData =
    MakeResult(kDescNoData, kModuleApplet, kSummaryInvalidState, kLevelStatus);
inline constexpr Result kResultAppletNotFound =
    MakeResult(kDescNotFound, kModuleApplet, kSummaryNotFound, kLevelStatus);
// What Azahar answers for an applet it cannot make or a title it cannot start ("TODO(Subv): Find the
// right error code").
inline constexpr Result kResultAppletNotSupported =
    MakeResult(kDescNotFound, kModuleApplet, kSummaryNotSupported, kLevelPermanent);
// Azahar's ResultUnknown.
inline constexpr Result kResultUnknown = 0xFFFFFFFF;
static_assert(kResultParameterPresent == 0xC8A0CC02);
static_assert(kResultInvalidAppletSlot == 0xC8A0CC04);
static_assert(kResultAlreadyExists == 0xC8A0CFFC);
static_assert(kResultNoData == 0xC8A0CFEF);
static_assert(kResultAppletNotFound == 0xC880CFFA);
static_assert(kResultAppletNotSupported == 0xD8C0CFFA);

// ---- the message parameter (applet_manager.h) ------------------------------------------------------------

// Holds the information of a parameter sent with SendParameter and read with Glance/ReceiveParameter.
struct MessageParameter {
    AppletId sender_id = AppletId::None;
    AppletId destination_id = AppletId::None;
    SignalType signal = SignalType::None;
    std::shared_ptr<Object> object;
    std::vector<u8> buffer;
};

// Used by the application to pass information about the current framebuffer to applets.
enum class DisplayBufferMode : u32 {
    R8G8B8A8 = 0,
    R8G8B8 = 1,
    R5G6B5 = 2,
    R5G5B5A1 = 3,
    R4G4B4A4 = 4,
    Unimportable = 0xFFFFFFFF,
};

struct CaptureBufferInfo {
    u32 size;
    u8 is_3d;
    u8 padding[3];
    u32 top_screen_left_offset;
    u32 top_screen_right_offset;
    DisplayBufferMode top_screen_format;
    u32 bottom_screen_left_offset;
    u32 bottom_screen_right_offset;
    DisplayBufferMode bottom_screen_format;
};
static_assert(sizeof(CaptureBufferInfo) == 0x20, "CaptureBufferInfo struct has incorrect size");

// ---- the applet manager, as an applet sees it -------------------------------------------------------------

class AppletHost {
public:
    virtual ~AppletHost() = default;

    virtual ServiceContext& Context() = 0;
    // Clears any parameter that is waiting and places `parameter` (AppletManager::CancelAndSendParameter).
    virtual void CancelAndSendParameter(const MessageParameter& parameter) = 0;
    virtual Result PrepareToCloseLibraryApplet(bool not_pause, bool exiting, bool jump_home) = 0;
    virtual Result CloseLibraryApplet(std::shared_ptr<Object> object,
                                      const std::vector<u8>& buffer) = 0;
};

// ---- an HLE applet (applet.h) -------------------------------------------------------------------------------

class Applet {
public:
    virtual ~Applet() = default;

    // Handles a parameter from the application: Wakeup starts the applet (and makes it active when that
    // worked), WakeupByCancel finishes it, anything else is the applet's own business.
    Result ReceiveParameter(const MessageParameter& parameter);

    // Whether the applet is running at all, and whether it is active instead of the application.
    bool IsRunning() const { return is_running_; }
    bool IsActive() const { return is_active_; }

    // An update tick (every 4,468,352 ticks of game time while the applet runs): the applet may send
    // commands, finish, ...
    virtual void Update() = 0;

protected:
    Applet(AppletId id, AppletId parent, std::weak_ptr<AppletHost> host)
        : id_(id), parent_(parent), host_(std::move(host)) {}

    virtual Result ReceiveParameterImpl(const MessageParameter& parameter) = 0;
    // The applet start event, triggered from the application.
    virtual Result Start(const MessageParameter& parameter) = 0;
    // Sends the "library applet closing" signal to the application with the data buffers.
    virtual Result Finalize() = 0;

    // Sends `parameter` through the manager (CancelAndSendParameter).
    void SendParameter(const MessageParameter& parameter);
    // PrepareToCloseLibraryApplet(true, false, false) and CloseLibraryApplet, then the applet is neither
    // active nor running.
    void CloseApplet(std::shared_ptr<Object> object, const std::vector<u8>& buffer);

    // The manager, if it still exists.
    std::shared_ptr<AppletHost> Host() const { return host_.lock(); }

    AppletId id_;      // id of this applet
    AppletId parent_;  // id of this applet's parent
    bool is_running_ = true;
    bool is_active_ = false;

private:
    std::weak_ptr<AppletHost> host_;
};

// The period of the applets' update events: usToCycles(16666) in Azahar (BASE_CLOCK_RATE_ARM11 =
// 268,111,856 Hz, src/core/core_timing.h), exact in ticks.
inline constexpr u64 kAppletUpdateIntervalTicks = 268111856ull * 16666ull / 1000000ull;
static_assert(kAppletUpdateIntervalTicks == 4468352);

}  // namespace mh4n::apt
