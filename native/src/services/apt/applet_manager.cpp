// Derived from Azahar src/core/hle/service/apt/applet_manager.cpp and
// src/core/hle/service/apt/applet_manager.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: both say "GPLv2 or any later version"): the applet manager's operations (see the
// header next to this file).
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project
#include "applet_manager.h"

#include <algorithm>
#include <cstring>

#include "applets/erreula.h"
#include "applets/mii_selector.h"
#include "applets/mint.h"
#include "applets/swkbd.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"

namespace mh4n::apt {

namespace {

constexpr const char* kLog = "Service.APT";

bool IsSystemAppletId(AppletId applet_id) {
    return (static_cast<u32>(applet_id) & static_cast<u32>(AppletId::TypeMask)) ==
           static_cast<u32>(AppletId::AnySystemApplet);
}

bool IsApplicationAppletId(AppletId applet_id) {
    return (static_cast<u32>(applet_id) & static_cast<u32>(AppletId::TypeMask)) ==
           static_cast<u32>(AppletId::Application);
}

}  // namespace

AppletManager::AppletManager(ServiceContext& context) : context_(context) {
    Kernel& kernel = context_.GetKernel();
    lock_ = kernel.NewMutex(false, "APT_U:Lock");
    for (std::size_t slot = 0; slot < applet_slots_.size(); ++slot) {
        AppletSlotData& slot_data = applet_slots_[slot];
        slot_data.slot = static_cast<AppletSlot>(slot);
        slot_data.applet_id = AppletId::None;
        slot_data.attributes.raw = 0;
        slot_data.registered = false;
        slot_data.loaded = false;
        slot_data.notification_event = kernel.NewEvent(ResetType::OneShot, "APT:Notification");
        slot_data.parameter_event = kernel.NewEvent(ResetType::OneShot, "APT:Parameter");
    }
}

AppletManager::~AppletManager() = default;

bool AppletManager::HasHleApplet(AppletId id) const {
    const auto it = hle_applets_.find(id);
    return it != hle_applets_.end() && it->second != nullptr;
}

// ---- the slots ---------------------------------------------------------------------------------------------

AppletManager::AppletSlotData* AppletManager::GetAppletSlot(AppletSlot slot) {
    MH4N_ASSERT(static_cast<std::size_t>(slot) < applet_slots_.size(), "no such applet slot {}",
                static_cast<unsigned>(slot));
    return &applet_slots_[static_cast<std::size_t>(slot)];
}

AppletId AppletManager::GetAppletSlotId(AppletSlot slot) {
    return slot != AppletSlot::Error ? GetAppletSlot(slot)->applet_id : AppletId::None;
}

AppletManager::AppletSlot AppletManager::GetAppletSlotFromId(AppletId id) {
    if (id == AppletId::Application) {
        if (GetAppletSlot(AppletSlot::Application)->applet_id != AppletId::None) {
            return AppletSlot::Application;
        }
        return AppletSlot::Error;
    }

    if (id == AppletId::AnySystemApplet) {
        if (GetAppletSlot(AppletSlot::SystemApplet)->applet_id != AppletId::None) {
            return AppletSlot::SystemApplet;
        }

        // The Home Menu is also a system applet, but it lives in its own slot to be able to run
        // concurrently with other system applets.
        if (GetAppletSlot(AppletSlot::HomeMenu)->applet_id != AppletId::None) {
            return AppletSlot::HomeMenu;
        }

        return AppletSlot::Error;
    }

    if (id == AppletId::AnyLibraryApplet || id == AppletId::AnySysLibraryApplet) {
        AppletSlotData* slot_data = GetAppletSlot(AppletSlot::LibraryApplet);
        if (slot_data->applet_id == AppletId::None) {
            return AppletSlot::Error;
        }

        const AppletPos applet_pos = slot_data->attributes.Pos();
        if ((id == AppletId::AnyLibraryApplet && applet_pos == AppletPos::Library) ||
            (id == AppletId::AnySysLibraryApplet && applet_pos == AppletPos::SysLibrary)) {
            return AppletSlot::LibraryApplet;
        }

        return AppletSlot::Error;
    }

    if (id == AppletId::HomeMenu || id == AppletId::AlternateMenu) {
        if (GetAppletSlot(AppletSlot::HomeMenu)->applet_id != AppletId::None) {
            return AppletSlot::HomeMenu;
        }

        return AppletSlot::Error;
    }

    for (std::size_t slot = 0; slot < applet_slots_.size(); ++slot) {
        if (applet_slots_[slot].applet_id == id) {
            return static_cast<AppletSlot>(slot);
        }
    }

    return AppletSlot::Error;
}

AppletManager::AppletSlot AppletManager::GetAppletSlotFromAttributes(AppletAttributes attributes) {
    // Mapping from AppletPos to AppletSlot
    static constexpr std::array<AppletSlot, 6> applet_position_slots = {
        AppletSlot::Application,   AppletSlot::LibraryApplet, AppletSlot::SystemApplet,
        AppletSlot::LibraryApplet, AppletSlot::Error,         AppletSlot::LibraryApplet};

    const u32 applet_pos_value = static_cast<u32>(attributes.Pos());
    if (applet_pos_value >= applet_position_slots.size()) {
        return AppletSlot::Error;
    }

    const AppletSlot slot = applet_position_slots[applet_pos_value];
    if (slot == AppletSlot::Error) {
        return AppletSlot::Error;
    }

    // The Home Menu is a system applet, however, it has its own applet slot so that it can run
    // concurrently with other system applets.
    if (slot == AppletSlot::SystemApplet && attributes.IsHomeMenu()) {
        return AppletSlot::HomeMenu;
    }

    return slot;
}

AppletManager::AppletSlot AppletManager::GetAppletSlotFromPos(AppletPos pos) {
    AppletId applet_id;
    switch (pos) {
    case AppletPos::Application:
        applet_id = AppletId::Application;
        break;
    case AppletPos::Library:
        applet_id = AppletId::AnyLibraryApplet;
        break;
    case AppletPos::System:
        applet_id = AppletId::AnySystemApplet;
        break;
    case AppletPos::SysLibrary:
        applet_id = AppletId::AnySysLibraryApplet;
        break;
    default:
        return AppletSlot::Error;
    }
    return GetAppletSlotFromId(applet_id);
}

// ---- the parameters ----------------------------------------------------------------------------------------

void AppletManager::CancelAndSendParameter(const MessageParameter& parameter) {
    MH4N_LOG(Debug, kLog, "Sending parameter from {:03X} to {:03X} with signal {:08X} and size {:08X}",
             static_cast<u32>(parameter.sender_id), static_cast<u32>(parameter.destination_id),
             static_cast<u32>(parameter.signal), parameter.buffer.size());

    // If the applet is being HLEd, send directly to the applet. (The applet is kept alive by this
    // pointer: it may close itself while it handles the parameter.)
    const auto applet_it = hle_applets_.find(parameter.destination_id);
    const std::shared_ptr<Applet> applet =
        applet_it != hle_applets_.end() ? applet_it->second : nullptr;
    if (applet != nullptr) {
        applet->ReceiveParameter(parameter);
        return;
    }

    // Otherwise, send the parameter the LLE way.
    next_parameter_ = parameter;

    if (parameter.signal == SignalType::RequestForSysApplet) {
        // APT handles RequestForSysApplet messages itself.
        MH4N_LOG(Debug, kLog, "Replying to RequestForSysApplet from {:03X}",
                 static_cast<u32>(parameter.sender_id));

        if (parameter.buffer.size() >= sizeof(CaptureBufferInfo)) {
            // Azahar also copies the application's framebuffers into a backup for the HOME menu's
            // screenshot (CaptureFrameBuffers, TransferCapturedFramebuffers). There is no HOME menu.
            MH4N_LOG(Warning, kLog,
                     "(STUBBED) the framebuffers of the suspended application are not captured");
        }

        next_parameter_->sender_id = parameter.destination_id;
        next_parameter_->destination_id = parameter.sender_id;
        next_parameter_->signal = SignalType::Response;
        next_parameter_->buffer.clear();
        next_parameter_->object = nullptr;
    } else if (IsSystemAppletId(parameter.sender_id) &&
               IsApplicationAppletId(parameter.destination_id) && parameter.object) {
        // When a message is sent from a system applet to an application, APT replaces its object
        // with the zero handle.
        next_parameter_->object = nullptr;
    }

    // Signal the event to let the receiver know that a new parameter is ready to be read
    const AppletSlot slot = GetAppletSlotFromId(next_parameter_->destination_id);
    if (slot != AppletSlot::Error) {
        GetAppletSlot(slot)->parameter_event->Signal();
    } else {
        MH4N_LOG(Debug, kLog, "No applet was registered with ID {:03X}",
                 static_cast<u32>(next_parameter_->destination_id));
    }
}

Result AppletManager::SendParameter(const MessageParameter& parameter) {
    // A new parameter can not be sent if the previous one hasn't been consumed yet
    if (next_parameter_) {
        MH4N_LOG(Warning, kLog, "Parameter from {:03X} to {:03X} blocked by pending parameter.",
                 static_cast<u32>(parameter.sender_id), static_cast<u32>(parameter.destination_id));
        return kResultParameterPresent;
    }

    CancelAndSendParameter(parameter);
    return kResultSuccess;
}

ResultVal<MessageParameter> AppletManager::GlanceParameter(AppletId app_id) {
    ResultVal<MessageParameter> out;
    if (!next_parameter_) {
        out.code = kResultNoData;
        return out;
    }

    if (next_parameter_->destination_id != app_id) {
        out.code = kResultAppletNotFound;
        return out;
    }

    out.value = *next_parameter_;

    // Note: The NS module always clears the DSPSleep and DSPWakeup signals even in GlanceParameter.
    if (next_parameter_->signal == SignalType::DspSleep ||
        next_parameter_->signal == SignalType::DspWakeup) {
        next_parameter_.reset();
    }

    return out;
}

ResultVal<MessageParameter> AppletManager::ReceiveParameter(AppletId app_id) {
    ResultVal<MessageParameter> result = GlanceParameter(app_id);
    if (result.Succeeded()) {
        MH4N_LOG(Debug, kLog,
                 "Received parameter from {:03X} to {:03X} with signal {:08X} and size {:08X}",
                 static_cast<u32>(result.value.sender_id), static_cast<u32>(result.value.destination_id),
                 static_cast<u32>(result.value.signal), result.value.buffer.size());

        // Clear the parameter
        next_parameter_.reset();
    }
    return result;
}

bool AppletManager::CancelParameter(bool check_sender, AppletId sender_appid, bool check_receiver,
                                    AppletId receiver_appid) {
    const bool cancellation_success =
        next_parameter_ && (!check_sender || next_parameter_->sender_id == sender_appid) &&
        (!check_receiver || next_parameter_->destination_id == receiver_appid);

    if (cancellation_success) {
        next_parameter_.reset();
    }

    return cancellation_success;
}

// ---- registering ---------------------------------------------------------------------------------------------

ResultVal<GetLockHandleResult> AppletManager::GetLockHandle(AppletAttributes attributes) {
    AppletAttributes corrected_attributes = attributes;
    if (attributes.Pos() == AppletPos::Library || attributes.Pos() == AppletPos::SysLibrary ||
        attributes.Pos() == AppletPos::AutoLibrary) {
        const AppletPos corrected_pos = last_library_launcher_slot_ == AppletSlot::Application
                                            ? AppletPos::Library
                                            : AppletPos::SysLibrary;
        corrected_attributes.SetPos(corrected_pos);
        MH4N_LOG(Debug, kLog, "Corrected applet attributes from {:08X} to {:08X}", attributes.raw,
                 corrected_attributes.raw);
    }

    ResultVal<GetLockHandleResult> out;
    out.value = GetLockHandleResult{corrected_attributes, 0, lock_};
    return out;
}

ResultVal<InitializeResult> AppletManager::Initialize(AppletId app_id, AppletAttributes attributes) {
    ResultVal<InitializeResult> out;
    const AppletSlot slot = GetAppletSlotFromAttributes(attributes);
    // Note: The real NS service does not check if the attributes value is valid before accessing the
    // data in the array. Azahar asserts "Invalid application attributes"; here it is an error.
    if (slot == AppletSlot::Error) {
        MH4N_LOG(Error, kLog, "Invalid application attributes {:08X}", attributes.raw);
        out.code = kResultInvalidAppletSlot;
        return out;
    }

    AppletSlotData* slot_data = GetAppletSlot(slot);
    if (slot_data->registered) {
        MH4N_LOG(Warning, kLog, "Applet attempted to register in occupied slot {:02X}",
                 static_cast<unsigned>(slot));
        out.code = kResultAlreadyExists;
        return out;
    }

    MH4N_LOG(Debug, kLog, "Initializing applet with ID {:03X} and attributes {:08X}.",
             static_cast<u32>(app_id), attributes.raw);
    slot_data->applet_id = app_id;
    // Note: In the real console the title id of a given applet slot is set by the APT module when
    // calling StartApplication.
    slot_data->title_id = program_id_;
    slot_data->attributes.raw = attributes.raw;

    // Applications need to receive a Wakeup signal to actually start up, this signal is usually sent
    // by the Home Menu after starting the app by way of APT::WakeupApplication. However, if nothing
    // is running yet the signal should be sent by APT::Initialize itself.
    if (active_slot_ == AppletSlot::Error) {
        active_slot_ = slot;

        // APT automatically calls enable on the first registered applet.
        Enable(attributes);

        // Wake up the applet.
        SendParameter({
            .sender_id = AppletId::None,
            .destination_id = app_id,
            .signal = SignalType::Wakeup,
            .object = nullptr,
            .buffer = {},
        });
    }

    out.value = InitializeResult{slot_data->notification_event, slot_data->parameter_event};
    return out;
}

Result AppletManager::Enable(AppletAttributes attributes) {
    const AppletSlot slot = GetAppletSlotFromAttributes(attributes);
    if (slot == AppletSlot::Error) {
        MH4N_LOG(Warning, kLog,
                 "Attempted to register with attributes {:08X}, but could not find slot.",
                 attributes.raw);
        return kResultInvalidAppletSlot;
    }

    MH4N_LOG(Debug, kLog, "Registering applet with attributes {:08X}.", attributes.raw);
    AppletSlotData* slot_data = GetAppletSlot(slot);
    slot_data->registered = true;

    if (slot_data->applet_id != AppletId::None && slot_data->attributes.Pos() == AppletPos::System &&
        slot_data->attributes.IsHomeMenu()) {
        slot_data->attributes.raw |= attributes.raw;
        MH4N_LOG(Debug, kLog, "Updated home menu attributes to {:08X}.", slot_data->attributes.raw);
    }

    // Send any outstanding parameters to the newly-registered application
    if (delayed_parameter_ && delayed_parameter_->destination_id == slot_data->applet_id) {
        // TODO: Real APT would loop trying to send the parameter until it succeeds, essentially
        // waiting for existing parameters to be delivered.
        const MessageParameter delayed = std::move(*delayed_parameter_);
        delayed_parameter_.reset();
        CancelAndSendParameter(delayed);
    }

    return kResultSuccess;
}

bool AppletManager::IsRegistered(AppletId app_id) {
    const AppletSlot slot = GetAppletSlotFromId(app_id);
    return slot != AppletSlot::Error && GetAppletSlot(slot)->registered;
}

ResultVal<Notification> AppletManager::InquireNotification(AppletId app_id) {
    ResultVal<Notification> out;
    const AppletSlot slot = GetAppletSlotFromId(app_id);
    if (slot != AppletSlot::Error) {
        AppletSlotData* slot_data = GetAppletSlot(slot);
        if (slot_data->registered) {
            out.value = slot_data->notification;
            slot_data->notification = Notification::None;
            return out;
        }
    }

    out.code = kResultAppletNotFound;
    return out;
}

ResultVal<AppletManInfo> AppletManager::GetAppletManInfo(AppletPos requested_applet_pos) {
    AppletPos active_applet_pos = AppletPos::Invalid;
    AppletId active_applet_id = AppletId::None;
    if (active_slot_ != AppletSlot::Error) {
        AppletSlotData* active_slot_data = GetAppletSlot(active_slot_);
        if (active_slot_data->applet_id != AppletId::None) {
            active_applet_pos = active_slot_data->attributes.Pos();
            active_applet_id = active_slot_data->applet_id;
        }
    }

    AppletId requested_applet_id = AppletId::None;
    const AppletSlot requested_slot = GetAppletSlotFromPos(requested_applet_pos);
    if (requested_slot != AppletSlot::Error) {
        AppletSlotData* requested_slot_data = GetAppletSlot(requested_slot);
        if (requested_slot_data->registered) {
            requested_applet_id = requested_slot_data->applet_id;
        }
    }

    ResultVal<AppletManInfo> out;
    out.value = AppletManInfo{
        .active_applet_pos = active_applet_pos,
        .requested_applet_id = requested_applet_id,
        .home_menu_applet_id = AppletId::HomeMenu,
        .active_applet_id = active_applet_id,
    };
    return out;
}

void AppletManager::SendApplicationParameterAfterRegistration(const MessageParameter& parameter) {
    const AppletSlot slot = GetAppletSlotFromId(parameter.destination_id);

    // If the application is already registered, immediately send the parameter
    if (slot != AppletSlot::Error && GetAppletSlot(slot)->registered) {
        CancelAndSendParameter(parameter);
        return;
    }

    // Otherwise queue it until the Application calls APT::Enable
    delayed_parameter_ = parameter;
}

// ---- library applets -----------------------------------------------------------------------------------------

Result AppletManager::CreateHleApplet(AppletId id, AppletId parent, bool preload) {
    switch (id) {
    case AppletId::SoftwareKeyboard1:
    case AppletId::SoftwareKeyboard2:
        hle_applets_[id] = std::make_shared<SoftwareKeyboard>(id, parent, weak_from_this(), context_);
        break;
    case AppletId::Ed1:
    case AppletId::Ed2:
        hle_applets_[id] = std::make_shared<MiiSelector>(id, parent, weak_from_this(), context_);
        break;
    case AppletId::Error:
    case AppletId::Error2:
        hle_applets_[id] = std::make_shared<ErrEula>(id, parent, weak_from_this(), context_);
        break;
    case AppletId::Mint:
    case AppletId::Mint2:
        hle_applets_[id] = std::make_shared<Mint>(id, parent, weak_from_this(), context_);
        break;
    default:
        // Azahar's default case (the other ids, the system applets' too: "Could not create applet").
        MH4N_LOG(Error, kLog, "Could not create applet {:03X}", static_cast<u32>(id));
        // TODO(Subv): Find the right error code
        return kResultAppletNotSupported;
    }

    AppletAttributes attributes;
    attributes.SetPos(AppletPos::AutoLibrary);
    attributes.SetIsHomeMenu(false);
    const ResultVal<GetLockHandleResult> lock_handle_data = GetLockHandle(attributes);

    Initialize(id, lock_handle_data.value.corrected_attributes);
    Enable(lock_handle_data.value.corrected_attributes);
    if (preload) {
        FinishPreloadingLibraryApplet(id);
    }

    // Schedule the update event
    ScheduleAppletUpdate(id, context_.GetKernel().NowTicks() + kAppletUpdateIntervalTicks);
    return kResultSuccess;
}

// Each update comes one period after the tick the last one was due at, not after the tick it ran at
// (Azahar's `usToCycles(interval) - cycles_late`): a late event does not shift the ones after it.
void AppletManager::ScheduleAppletUpdate(AppletId id, u64 due_tick) {
    std::weak_ptr<AppletManager> weak = weak_from_this();
    context_.GetKernel().ScheduleAt(due_tick, [weak, id, due_tick] {
        if (const std::shared_ptr<AppletManager> self = weak.lock()) {
            self->HleAppletUpdateEvent(id, due_tick);
        }
    });
}

// Handles updating the current Applet every time it's called.
void AppletManager::HleAppletUpdateEvent(AppletId id, u64 due_tick) {
    const auto it = hle_applets_.find(id);
    const std::shared_ptr<Applet> applet = it != hle_applets_.end() ? it->second : nullptr;
    if (applet == nullptr) {
        // Dead applet, exit event loop.
        MH4N_LOG(Warning, kLog, "Attempted to update missing applet id={:03X}", static_cast<u32>(id));
        return;
    }

    if (applet->IsActive()) {
        applet->Update();
    }

    // If the applet is still running after the last update, reschedule the event
    if (applet->IsRunning()) {
        ScheduleAppletUpdate(id, due_tick + kAppletUpdateIntervalTicks);
    } else {
        // Otherwise the applet has terminated, in which case we should clean it up
        hle_applets_[id] = nullptr;
    }
}

Result AppletManager::PrepareToStartLibraryApplet(AppletId applet_id) {
    // The real APT service returns an error if there's a pending APT parameter when this function is
    // called.
    if (next_parameter_) {
        return kResultParameterPresent;
    }

    if (GetAppletSlot(AppletSlot::LibraryApplet)->registered) {
        return kResultAlreadyExists;
    }

    last_library_launcher_slot_ = active_slot_;
    last_prepared_library_applet_ = applet_id;

    // Azahar can start the applet's own title (lle_applets) and falls back to an HLE implementation
    // if it cannot; there is no title here.
    const auto it = hle_applets_.find(applet_id);
    if (it != hle_applets_.end() && it->second != nullptr) {
        MH4N_LOG(Warning, kLog, "Applet has already been started id={:03X}", static_cast<u32>(applet_id));
        return kResultSuccess;
    }

    const AppletId parent = GetAppletSlotId(last_library_launcher_slot_);
    MH4N_LOG(Debug, kLog, "Creating HLE applet {:03X} with parent {:03X}", static_cast<u32>(applet_id),
             static_cast<u32>(parent));
    return CreateHleApplet(applet_id, parent, false);
}

Result AppletManager::PreloadLibraryApplet(AppletId applet_id) {
    if (GetAppletSlot(AppletSlot::LibraryApplet)->registered) {
        return kResultAlreadyExists;
    }

    last_library_launcher_slot_ = active_slot_;
    last_prepared_library_applet_ = applet_id;

    // As PrepareToStartLibraryApplet: no title to start, the HLE applet or nothing.
    const auto it = hle_applets_.find(applet_id);
    if (it != hle_applets_.end() && it->second != nullptr) {
        MH4N_LOG(Warning, kLog, "Applet has already been started id={:08X}", static_cast<u32>(applet_id));
        return kResultSuccess;
    }

    const AppletId parent = GetAppletSlotId(last_library_launcher_slot_);
    MH4N_LOG(Debug, kLog, "Creating HLE applet {:03X} with parent {:03X}", static_cast<u32>(applet_id),
             static_cast<u32>(parent));
    return CreateHleApplet(applet_id, parent, true);
}

Result AppletManager::FinishPreloadingLibraryApplet(AppletId /*applet_id*/) {
    // TODO(Subv): This function should fail depending on the applet preparation state (Azahar's note).
    GetAppletSlot(AppletSlot::LibraryApplet)->loaded = true;
    return kResultSuccess;
}

Result AppletManager::StartLibraryApplet(AppletId applet_id, std::shared_ptr<Object> object,
                                         const std::vector<u8>& buffer) {
    active_slot_ = AppletSlot::LibraryApplet;

    const Result send_res = SendParameter({
        .sender_id = GetAppletSlotId(last_library_launcher_slot_),
        .destination_id = applet_id,
        .signal = SignalType::Wakeup,
        .object = std::move(object),
        .buffer = buffer,
    });
    if (IsError(send_res)) {
        active_slot_ = last_library_launcher_slot_;
        return send_res;
    }

    return kResultSuccess;
}

Result AppletManager::PrepareToCloseLibraryApplet(bool not_pause, bool exiting, bool jump_home) {
    if (next_parameter_) {
        return kResultParameterPresent;
    }

    if (!not_pause) {
        library_applet_closing_command_ = SignalType::WakeupByPause;
    } else if (jump_home) {
        library_applet_closing_command_ = SignalType::WakeupToJumpHome;
    } else if (exiting) {
        library_applet_closing_command_ = SignalType::WakeupByCancel;
    } else {
        library_applet_closing_command_ = SignalType::WakeupByExit;
    }

    return kResultSuccess;
}

Result AppletManager::CloseLibraryApplet(std::shared_ptr<Object> object, const std::vector<u8>& buffer) {
    AppletSlotData* slot = GetAppletSlot(AppletSlot::LibraryApplet);
    const AppletId destination_id = GetAppletSlotId(last_library_launcher_slot_);

    active_slot_ = last_library_launcher_slot_;

    MessageParameter param = {
        .sender_id = slot->applet_id,
        .destination_id = destination_id,
        .signal = library_applet_closing_command_,
        .object = std::move(object),
        .buffer = buffer,
    };

    if (library_applet_closing_command_ != SignalType::WakeupByPause) {
        CancelAndSendParameter(param);
        // TODO: Terminate the running applet title
        slot->Reset();
    } else {
        SendParameter(param);
    }

    // Azahar transfers the captured framebuffers back to VRAM here when a system applet launched the
    // library applet while an application is running (TransferCapturedFramebuffers): there is no
    // system applet.
    return kResultSuccess;
}

Result AppletManager::CancelLibraryApplet(bool /*app_exiting*/) {
    if (next_parameter_) {
        return kResultParameterPresent;
    }

    AppletSlotData* slot = GetAppletSlot(AppletSlot::LibraryApplet);
    if (!slot->registered) {
        return kResultInvalidAppletSlot;
    }

    return SendParameter({
        .sender_id = GetAppletSlotId(last_library_launcher_slot_),
        .destination_id = slot->applet_id,
        .signal = SignalType::WakeupByCancel,
        .object = nullptr,
        .buffer = {},
    });
}

// ---- system applets and the HOME menu --------------------------------------------------------------------

Result AppletManager::PrepareToStartSystemApplet(AppletId /*applet_id*/) {
    // The real APT service returns an error if there's a pending APT parameter when this function is
    // called.
    if (next_parameter_) {
        return kResultParameterPresent;
    }

    last_system_launcher_slot_ = active_slot_;
    return kResultSuccess;
}

Result AppletManager::StartSystemApplet(AppletId applet_id, std::shared_ptr<Object> object,
                                        const std::vector<u8>& buffer) {
    AppletId source_applet_id = AppletId::Application;
    if (last_system_launcher_slot_ != AppletSlot::Error) {
        AppletSlotData* launcher_slot_data = GetAppletSlot(last_system_launcher_slot_);
        source_applet_id = launcher_slot_data->applet_id;

        // APT generally clears and terminates the caller of StartSystemApplet. This helps in
        // situations such as a system applet launching another system applet, which would otherwise
        // deadlock.
        // TODO: In real APT, the check for AppletSlot::Application does not exist; there is something
        // wrong with our implementation somewhere that makes this necessary. Otherwise, games that
        // attempt to launch system applets will be cleared and emulation will crash.
        if (!launcher_slot_data->registered ||
            (last_system_launcher_slot_ != AppletSlot::Application &&
             !launcher_slot_data->attributes.NoExitOnSystemApplet())) {
            launcher_slot_data->Reset();
            // TODO: Implement launcher process termination.
        }
    }

    // If a system applet is not already registered, it is started by APT.
    const AppletSlot slot_id =
        applet_id == AppletId::HomeMenu ? AppletSlot::HomeMenu : AppletSlot::SystemApplet;
    if (!GetAppletSlot(slot_id)->registered) {
        // NS::LaunchTitle(NAND, the applet's title): there is no such title on the reference's
        // console, so it fails as it does there.
        // TODO: Find the right error code.
        MH4N_LOG(Warning, kLog, "cannot start the system applet {:03X}: no such title",
                 static_cast<u32>(applet_id));
        return kResultAppletNotSupported;
    }

    active_slot_ = slot_id;

    SendApplicationParameterAfterRegistration({
        .sender_id = source_applet_id,
        .destination_id = applet_id,
        .signal = SignalType::Wakeup,
        .object = std::move(object),
        .buffer = buffer,
    });

    return kResultSuccess;
}

void AppletManager::EnsureHomeMenuLoaded() {
    // TODO(Subv): The real APT service sends signal 12 (WakeupByCancel) to the currently running
    // System applet, waits for it to finish, and then launches the Home Menu.
    // Azahar asserts "A system applet is already running".
    if (GetAppletSlot(AppletSlot::SystemApplet)->registered) {
        MH4N_LOG(Error, kLog, "A system applet is already running");
    }

    if (GetAppletSlot(AppletSlot::HomeMenu)->registered) {
        // The Home Menu is already running.
        return;
    }

    MH4N_LOG(Warning, kLog, "The Home Menu failed to launch, application jumping will not work.");
}

Result AppletManager::PrepareToJumpToHomeMenu() {
    if (next_parameter_) {
        return kResultParameterPresent;
    }

    last_jump_to_home_slot_ = active_slot_;

    capture_info_.reset();

    if (last_jump_to_home_slot_ == AppletSlot::Application) {
        EnsureHomeMenuLoaded();
    }

    return kResultSuccess;
}

Result AppletManager::JumpToHomeMenu(std::shared_ptr<Object> object, const std::vector<u8>& buffer) {
    if (last_jump_to_home_slot_ != AppletSlot::Error) {
        AppletSlotData* slot_data = GetAppletSlot(last_jump_to_home_slot_);
        if (slot_data->applet_id != AppletId::None) {
            MessageParameter param;
            param.object = std::move(object);
            param.buffer = buffer;

            switch (slot_data->attributes.Pos()) {
            case AppletPos::Application:
                active_slot_ = AppletSlot::HomeMenu;

                param.destination_id = AppletId::HomeMenu;
                param.sender_id = AppletId::Application;
                param.signal = SignalType::WakeupByPause;
                SendParameter(param);
                break;
            case AppletPos::Library:
                param.destination_id = slot_data->applet_id;
                param.sender_id = slot_data->applet_id;
                param.signal = SignalType::WakeupByCancel;
                SendParameter(param);
                break;
            case AppletPos::System:
                if (slot_data->attributes.IsHomeMenu()) {
                    param.destination_id = slot_data->applet_id;
                    param.sender_id = slot_data->applet_id;
                    param.signal = SignalType::WakeupToJumpHome;
                    SendParameter(param);
                }
                break;
            case AppletPos::SysLibrary: {
                AppletSlotData* system_slot_data = GetAppletSlot(AppletSlot::SystemApplet);
                param.destination_id = slot_data->applet_id;
                param.sender_id = slot_data->applet_id;
                param.signal = system_slot_data->registered ? SignalType::WakeupByCancel
                                                            : SignalType::WakeupToJumpHome;
                SendParameter(param);
                break;
            }
            default:
                break;
            }
        }
    }

    return kResultSuccess;
}

// ---- closing and jumping ---------------------------------------------------------------------------------

Result AppletManager::PrepareToCloseApplication(bool return_to_sys) {
    if (active_slot_ == AppletSlot::Error) {
        return kResultInvalidAppletSlot;
    }

    AppletSlotData* active_slot_data = GetAppletSlot(active_slot_);
    if (active_slot_data->applet_id == AppletId::None ||
        active_slot_data->attributes.Pos() != AppletPos::Application) {
        return kResultInvalidAppletSlot;
    }

    AppletSlotData* system_slot_data = GetAppletSlot(AppletSlot::SystemApplet);
    AppletSlotData* home_menu_slot_data = GetAppletSlot(AppletSlot::HomeMenu);

    if (!application_cancelled_ && return_to_sys) {
        // TODO: Left side of the OR also includes "&& !power_button_clicked", but this isn't
        // implemented yet.
        if (!ordered_to_close_application_ || !system_slot_data->registered) {
            application_close_target_ = AppletSlot::HomeMenu;
        } else {
            application_close_target_ = AppletSlot::SystemApplet;
        }
    } else {
        application_close_target_ = AppletSlot::Error;
    }

    if (application_close_target_ != AppletSlot::HomeMenu && !system_slot_data->registered &&
        !home_menu_slot_data->registered) {
        return kResultInvalidAppletSlot;
    }

    if (next_parameter_) {
        return kResultParameterPresent;
    }

    if (application_close_target_ == AppletSlot::HomeMenu) {
        // Real APT would make sure home menu is loaded here. However, this is only really needed if
        // the home menu wasn't loaded in the first place. Since we want to preserve normal behavior
        // when the user loaded the game directly without going through home menu, we skip this. Then,
        // later we just close to the game list when the application finishes closing.
        // EnsureHomeMenuLoaded();
    }

    return kResultSuccess;
}

Result AppletManager::CloseApplication(std::shared_ptr<Object> object, const std::vector<u8>& buffer) {
    ordered_to_close_application_ = false;
    application_cancelled_ = false;

    GetAppletSlot(AppletSlot::Application)->Reset();

    if (application_close_target_ != AppletSlot::Error) {
        // If exiting to the home menu and it is not loaded, exit to game list.
        if (application_close_target_ == AppletSlot::HomeMenu &&
            !GetAppletSlot(application_close_target_)->registered) {
            // Azahar: system.RequestShutdown().
            context_.RequestExit(0, "APT: the application closed and there is no HOME menu to return to");
        } else {
            active_slot_ = application_close_target_;

            CancelAndSendParameter({
                .sender_id = AppletId::Application,
                .destination_id = GetAppletSlot(application_close_target_)->applet_id,
                .signal = SignalType::WakeupByExit,
                .object = std::move(object),
                .buffer = buffer,
            });
        }
    }

    // TODO: Terminate the application process.
    return kResultSuccess;
}

Result AppletManager::PrepareToDoApplicationJump(u64 title_id, ApplicationJumpFlags flags) {
    // A running application can not launch another application directly because the applet state for
    // the Application slot is already in use. The way this is implemented in hardware is to launch the
    // Home Menu and tell it to launch our desired application.

    // Azahar asserts "Unimplemented application jump flags 1".
    if (flags == ApplicationJumpFlags::UseStoredParameters) {
        MH4N_LOG(Error, kLog, "Unimplemented application jump flags 1");
        return kResultUnknown;
    }

    // Save the title data to send it to the Home Menu when DoApplicationJump is called.
    AppletSlotData* application_slot_data = GetAppletSlot(AppletSlot::Application);
    app_jump_parameters_.current_title_id = application_slot_data->title_id;

    if (flags == ApplicationJumpFlags::UseCurrentParameters) {
        app_jump_parameters_.next_title_id = app_jump_parameters_.current_title_id;
    } else {
        // Azahar converts the title id to the console's region (ConvertTitleID: the MSET, eShop and
        // NNID settings titles); nothing here starts a title.
        app_jump_parameters_.next_title_id = title_id;
    }
    app_jump_parameters_.flags = flags;

    // Note: The real console uses the Home Menu to perform the application jump, therefore the menu
    // needs to be running. The real APT module starts the Home Menu here if it's not already running,
    // we don't have to do this.
    return kResultSuccess;
}

Result AppletManager::DoApplicationJump() {
    // Note: The real console uses the Home Menu to perform the application jump, it goes
    // OldApplication->Home Menu->NewApplication. We do not need to use the Home Menu to do this so we
    // launch the new application directly. In the real APT service, the Home Menu must be running to
    // do this, otherwise error 0xC8A0CFF0 is returned.

    AppletSlotData* application_slot_data = GetAppletSlot(AppletSlot::Application);
    application_slot_data->Reset();

    if (GetAppletSlot(AppletSlot::HomeMenu)->registered) {
        // If the home menu is running, use it to jump to the next application. The home menu will
        // call GetProgramIdOnApplicationJump and PrepareToStartApplication/StartApplication to launch
        // the title.
        active_slot_ = AppletSlot::HomeMenu;
        SendParameter({
            .sender_id = AppletId::Application,
            .destination_id = AppletId::HomeMenu,
            .signal = SignalType::WakeupToLaunchApplication,
            .object = nullptr,
            .buffer = {},
        });

        // TODO: APT terminates the application here, usually it will exit itself properly though.
        return kResultSuccess;
    }

    // Otherwise, work around the missing home menu by launching the title directly: Azahar resets the
    // system into the new title (NS::RebootToTitle -> System::RequestReset).
    context_.RequestExit(0, "APT: DoApplicationJump asks for a system reset into another title");
    return kResultSuccess;
}

void AppletManager::SendCaptureBufferInfo(const std::vector<u8>& buffer) {
    // Azahar asserts "CaptureBufferInfo is too small."
    if (buffer.size() < sizeof(CaptureBufferInfo)) {
        MH4N_LOG(Error, kLog, "CaptureBufferInfo is too small: {} bytes", buffer.size());
        return;
    }

    capture_info_.emplace();
    std::memcpy(&*capture_info_, buffer.data(), sizeof(CaptureBufferInfo));
}

ApplicationRunningMode AppletManager::GetApplicationRunningMode() {
    AppletSlotData* slot_data = GetAppletSlot(AppletSlot::Application);
    if (slot_data->applet_id == AppletId::None) {
        return ApplicationRunningMode::NoApplication;
    }

    // APT checks whether the system is a New 3DS and the 804MHz CPU speed is enabled to determine the
    // result: an Old 3DS is never in the New 3DS mode.
    if (slot_data->registered) {
        return ApplicationRunningMode::Old3dsRegistered;
    }
    return ApplicationRunningMode::Old3dsUnregistered;
}

}  // namespace mh4n::apt
