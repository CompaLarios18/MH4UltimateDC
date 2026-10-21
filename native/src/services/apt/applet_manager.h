// Derived from Azahar src/core/hle/service/apt/applet_manager.h, src/core/hle/service/apt/applet_manager.cpp,
// src/core/hle/service/apt/apt.h and src/core/core_timing.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: each of them says "GPLv2 or any later version" or "GPLv2+"): the applet manager's
// state (the four applet slots, the pending parameter, the delayed parameter, the library applet
// bookkeeping) and its operations, as far as the commands the game sends need them.
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (applet_manager.h,
//   applet_manager.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (apt.h)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
//
// The applet manager (N1-14): which applet is in which slot, which parameter is waiting for whom, and
// the HLE library applets (the software keyboard) that live inside it. APT:U, APT:A and APT:S are three
// names of one module and share one manager.
//
// What is here is what the commands of docs/research/service-commands.md need: Initialize, Enable,
// GetLockHandle, the parameters, the library applets, the system applet starters, the jump to the HOME
// menu, closing the application, the application jump, the capture buffer info. The rest of Azahar's
// manager (Finalize, the preloading of applets, the DSP sleep signals, the system menu argument, the
// startup argument, the framebuffer capture for the HOME menu, the HOME and power buttons, starting a
// title) is not here because no command the game sends reaches it; those commands are answered as any
// unimplemented one is.
//
// Departures from Azahar:
//  - Nothing can start a title: Azahar's NS::LaunchTitle finds no HOME menu, system applet or title
//    on the reference's console either, so StartSystemApplet fails as it does there. Asking for a
//    system reset (DoApplicationJump without a HOME menu, which reboots in Azahar) or for a shutdown
//    (CloseApplication with no HOME menu to return to) ends the run with status 0.
//  - The only HLE library applets are the software keyboards (Azahar has the Mii selector, the error
//    and EULA applet and the mint applet too): the others fail as a library applet Azahar cannot make
//    does.
//  - The framebuffer capture that a RequestForSysApplet message triggers (the HOME menu's screenshot of
//    the suspended application) is not done; the reply to the message is.
//  - The things Azahar asserts: an applet attribute that names no slot (Initialize answers with
//    kResultInvalidAppletSlot), a capture buffer info that is too short (ignored), an application jump
//    with the stored-parameters flag (answered with kResultUnknown).
//  - The timer events are exact in ticks: an update that runs late does not shift the ones after it
//    (Azahar's `interval - cycles_late` does the same).
//
// Threads (principle 6): scheduler thread only.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "applets/applet.h"
#include "mh4n/kernel.h"
#include "mh4n/service.h"
#include "mh4n/types.h"

namespace mh4n::apt {

enum class AppletPos : u32 {
    Application = 0,
    Library = 1,
    System = 2,
    SysLibrary = 3,
    Resident = 4,
    AutoLibrary = 5,
    Invalid = 0xFF,
};

// The attributes of an applet: the position in bits 0-2, "no exit on system applet" in bit 28 and
// "is the HOME menu" in bit 29.
struct AppletAttributes {
    u32 raw = 0;

    AppletAttributes() = default;
    explicit AppletAttributes(u32 attributes) : raw(attributes) {}

    // Three bits, as Azahar's BitField<0, 3, AppletPos>.
    AppletPos Pos() const { return static_cast<AppletPos>(raw & 7); }
    void SetPos(AppletPos pos) { raw = (raw & ~7u) | (static_cast<u32>(pos) & 7); }
    bool NoExitOnSystemApplet() const { return ((raw >> 28) & 1) != 0; }
    bool IsHomeMenu() const { return ((raw >> 29) & 1) != 0; }
    void SetIsHomeMenu(bool value) { raw = (raw & ~(1u << 29)) | (value ? 1u << 29 : 0); }
};

// Application Old/New 3DS target platforms and running modes (only the Old 3DS ones are answered).
enum class TargetPlatform : u8 { Old3ds = 0, New3ds = 1 };
enum class ApplicationRunningMode : u8 {
    NoApplication = 0,
    Old3dsRegistered = 1,
    New3dsRegistered = 2,
    Old3dsUnregistered = 3,
    New3dsUnregistered = 4,
};

enum class ApplicationJumpFlags : u8 {
    UseInputParameters = 0,
    UseStoredParameters = 1,
    UseCurrentParameters = 2,
};

// A result and, when it is a success, a value (Azahar's ResultVal).
template <class T> struct ResultVal {
    Result code = kResultSuccess;
    T value{};
    bool Failed() const { return IsError(code); }
    bool Succeeded() const { return !IsError(code); }
};

struct AppletManInfo {
    AppletPos active_applet_pos;
    AppletId requested_applet_id;
    AppletId home_menu_applet_id;
    AppletId active_applet_id;
};

struct GetLockHandleResult {
    AppletAttributes corrected_attributes;
    u32 state = 0;
    std::shared_ptr<Mutex> lock;
};

struct InitializeResult {
    std::shared_ptr<Event> notification_event;
    std::shared_ptr<Event> parameter_event;
};

class AppletManager final : public AppletHost, public std::enable_shared_from_this<AppletManager> {
public:
    explicit AppletManager(ServiceContext& context);
    ~AppletManager() override;
    AppletManager(const AppletManager&) = delete;
    AppletManager& operator=(const AppletManager&) = delete;

    // The program id of the application, which the slot of the application gets at Initialize
    // (Azahar: the current process's code set); set by the boot step.
    void SetProgramId(u64 program_id) { program_id_ = program_id; }

    // ---- AppletHost ----
    ServiceContext& Context() override { return context_; }
    void CancelAndSendParameter(const MessageParameter& parameter) override;
    Result PrepareToCloseLibraryApplet(bool not_pause, bool exiting, bool jump_home) override;
    Result CloseLibraryApplet(std::shared_ptr<Object> object, const std::vector<u8>& buffer) override;

    // ---- the parameters ----
    Result SendParameter(const MessageParameter& parameter);
    ResultVal<MessageParameter> GlanceParameter(AppletId app_id);
    ResultVal<MessageParameter> ReceiveParameter(AppletId app_id);
    bool CancelParameter(bool check_sender, AppletId sender_appid, bool check_receiver,
                         AppletId receiver_appid);

    // ---- registering ----
    ResultVal<GetLockHandleResult> GetLockHandle(AppletAttributes attributes);
    ResultVal<InitializeResult> Initialize(AppletId app_id, AppletAttributes attributes);
    Result Enable(AppletAttributes attributes);
    bool IsRegistered(AppletId app_id);
    ResultVal<Notification> InquireNotification(AppletId app_id);
    ResultVal<AppletManInfo> GetAppletManInfo(AppletPos requested_applet_pos);

    // ---- library applets ----
    Result PrepareToStartLibraryApplet(AppletId applet_id);
    Result PreloadLibraryApplet(AppletId applet_id);
    Result FinishPreloadingLibraryApplet(AppletId applet_id);
    Result StartLibraryApplet(AppletId applet_id, std::shared_ptr<Object> object,
                              const std::vector<u8>& buffer);
    Result CancelLibraryApplet(bool app_exiting);

    // ---- system applets and the HOME menu ----
    Result PrepareToStartSystemApplet(AppletId applet_id);
    Result StartSystemApplet(AppletId applet_id, std::shared_ptr<Object> object,
                             const std::vector<u8>& buffer);
    Result PrepareToJumpToHomeMenu();
    Result JumpToHomeMenu(std::shared_ptr<Object> object, const std::vector<u8>& buffer);

    // ---- closing and jumping ----
    Result PrepareToCloseApplication(bool return_to_sys);
    Result CloseApplication(std::shared_ptr<Object> object, const std::vector<u8>& buffer);
    Result PrepareToDoApplicationJump(u64 title_id, ApplicationJumpFlags flags);
    Result DoApplicationJump();

    void SendCaptureBufferInfo(const std::vector<u8>& buffer);

    TargetPlatform GetTargetPlatform() const { return TargetPlatform::Old3ds; }
    ApplicationRunningMode GetApplicationRunningMode();

    // ---- for the tests ----
    bool HasPendingParameter() const { return next_parameter_.has_value(); }
    bool HasHleApplet(AppletId id) const;

private:
    static constexpr std::size_t kNumAppletSlots = 4;

    enum class AppletSlot : u8 {
        Application,
        SystemApplet,
        HomeMenu,
        LibraryApplet,

        // An invalid tag
        Error,
    };

    struct AppletSlotData {
        AppletId applet_id = AppletId::None;
        AppletSlot slot = AppletSlot::Error;
        u64 title_id = 0;
        bool registered = false;
        bool loaded = false;
        AppletAttributes attributes;
        Notification notification = Notification::None;
        std::shared_ptr<Event> notification_event;
        std::shared_ptr<Event> parameter_event;

        void Reset() {
            applet_id = AppletId::None;
            registered = false;
            title_id = 0;
            attributes.raw = 0;
        }
    };

    AppletSlotData* GetAppletSlot(AppletSlot slot);
    AppletId GetAppletSlotId(AppletSlot slot);
    AppletSlot GetAppletSlotFromId(AppletId id);
    AppletSlot GetAppletSlotFromAttributes(AppletAttributes attributes);
    AppletSlot GetAppletSlotFromPos(AppletPos pos);

    // Checks if the Application slot has already been registered and sends the parameter to it,
    // otherwise it queues for sending when the application registers itself with Enable.
    void SendApplicationParameterAfterRegistration(const MessageParameter& parameter);
    void EnsureHomeMenuLoaded();

    Result CreateHleApplet(AppletId id, AppletId parent, bool preload);
    void ScheduleAppletUpdate(AppletId id, u64 due_tick);
    void HleAppletUpdateEvent(AppletId id, u64 due_tick);

    ServiceContext& context_;
    u64 program_id_ = 0;

    // APT lock retrieved via GetLockHandle.
    std::shared_ptr<Mutex> lock_;

    // Parameter data to be returned in the next call to Glance/ReceiveParameter.
    std::optional<MessageParameter> next_parameter_;
    // This parameter will be sent to the application/applet once they register themselves by using
    // Enable.
    std::optional<MessageParameter> delayed_parameter_;

    // PrepareToDoApplicationJump's parameters (applet_manager.h's ApplicationJumpParameters, without the
    // media types: nothing here reads them).
    struct ApplicationJumpParameters {
        u64 next_title_id = ~0ULL;
        ApplicationJumpFlags flags{};
        u64 current_title_id = ~0ULL;
    };
    ApplicationJumpParameters app_jump_parameters_;

    std::optional<CaptureBufferInfo> capture_info_;

    std::array<AppletSlotData, kNumAppletSlots> applet_slots_{};
    AppletSlot active_slot_ = AppletSlot::Error;

    AppletSlot last_library_launcher_slot_ = AppletSlot::Error;
    SignalType library_applet_closing_command_ = SignalType::None;
    AppletId last_prepared_library_applet_ = AppletId::None;
    AppletSlot last_system_launcher_slot_ = AppletSlot::Error;
    AppletSlot last_jump_to_home_slot_ = AppletSlot::Error;
    // Set by OrderToCloseApplication and CancelApplication, which the game does not send; so they
    // stay false.
    bool ordered_to_close_application_ = false;
    bool application_cancelled_ = false;
    AppletSlot application_close_target_ = AppletSlot::Error;

    std::map<AppletId, std::shared_ptr<Applet>> hle_applets_;
};

}  // namespace mh4n::apt
