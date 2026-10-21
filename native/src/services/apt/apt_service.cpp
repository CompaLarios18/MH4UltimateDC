// Derived from Azahar src/core/hle/service/apt/apt.cpp, src/core/hle/service/apt/apt.h,
// src/core/hle/service/apt/apt_u.cpp, src/core/hle/service/apt/apt_a.cpp,
// src/core/hle/service/apt/apt_s.cpp and src/core/hle/service/pm/pm_app.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the command table shared by the three services, the commands the game sends with their
// parameters and replies, and the CPU time limit commands (pm_app.cpp: what they set and read).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (apt.cpp, apt.h, apt_s.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (apt_u.cpp, apt_a.cpp,
//   pm_app.cpp)
//
// APT:U, APT:A and APT:S (N1-14): three names of the same commands over one applet manager
// (applet_manager.h); the game picks the name at run time. Each accepts two sessions, as Azahar's.
//
// The commands are the ones docs/research/service-commands.md lists for the game plus
// GetAppCpuTimeLimit. The others of Azahar's table are registered by name and answered as any
// unimplemented command is (logged once as `unimplemented <service> <header>`, one normal parameter,
// the result 0): the game does not send them, and Azahar's table has them without a handler too in
// places (SleepSystem).
//
// SetAppCpuTimeLimit and GetAppCpuTimeLimit go to the thread manager's core-1 limiter (N1-27),
// which stands where Azahar's pm:app and the application's resource limit stand.
//
// Departures from Azahar:
//  - A size the game gives for a buffer (ReceiveParameter, GlanceParameter, AppletUtility) is cut to
//    the largest a static buffer descriptor can describe (0x3FFFF bytes): a client cannot have a
//    larger receive buffer, and Azahar would allocate whatever 32-bit size it is given.
//  - GetSharedFont is not answered: the game does not send it. (The shared font is the system archive
//    that services/fs serves; a future caller would read it there.)
#include <algorithm>
#include <memory>
#include <vector>

#include "applet_manager.h"
#include "shared_font.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"

namespace mh4n::apt {

namespace {

constexpr const char* kLog = "Service.APT";

// "Each APT service can only have up to 2 sessions connected at the same time." (apt.h)
constexpr u32 kMaxAptSessions = 2;
// A static buffer descriptor holds an 18-bit size.
constexpr u32 kMaxStaticBufferSize = 0x3FFFF;

// What the three services and the boot step share (Azahar's APT::Module).
struct AptState {
    std::shared_ptr<AppletManager> applet_manager;
    u64 program_id = 0;
    // APT::Module::screen_capture_post_permission ("TODO(JamePeng): verify the initial value")
    u32 screen_capture_post_permission = 0;
    // APT::Module::shared_font_mem: "APT:SharedFont", 0x332000 bytes (3272 KB) from the SYSTEM region,
    // ReadWrite for the owner and Read for the others, made with the module and kept for the run
    // whether or not the game asks for the font. Only the allocation is reproduced (it is what the
    // SYSTEM region's usage shows); LoadSharedFont and GetSharedFont stay unimplemented.
    std::shared_ptr<SharedMemory> shared_font_mem;

    void Ensure(ServiceContext& context) {
        if (!applet_manager) {
            applet_manager = std::make_shared<AppletManager>(context);
            applet_manager->SetProgramId(program_id);
            // Azahar unwraps the result (it asserts when the region is full); here a failure is logged
            // and the module goes on without the block.
            const Result result = AllocateSharedFontBlock(context.GetKernel(), &shared_font_mem);
            if (IsError(result)) {
                MH4N_LOG(Error, kLog, "cannot allocate the shared font block: 0x{:08X}", result);
                shared_font_mem.reset();
            }
        }
    }
};

std::shared_ptr<AptState> GetState(ServiceContext& context) {
    std::shared_ptr<AptState> state = context.State<AptState>("apt");
    state->Ensure(context);
    return state;
}

}  // namespace

Result AllocateSharedFontBlock(Kernel& kernel, std::shared_ptr<SharedMemory>* out) {
    out->reset();
    return kernel.NewSharedMemory(out, nullptr, kSharedFontSize, MemoryPermission::ReadWrite,
                                  MemoryPermission::Read, 0, FcramRegion::System, "APT:SharedFont");
}

std::shared_ptr<SharedMemory> SharedFontBlock(ServiceContext& context) {
    return GetState(context)->shared_font_mem;
}

namespace {

std::vector<u8> BufferData(const StaticBuffer* buffer) {
    return buffer != nullptr ? buffer->data : std::vector<u8>();
}

u32 ClampBufferSize(u32 size, const char* what) {
    if (size > kMaxStaticBufferSize) {
        MH4N_LOG(Warning, kLog, "{}: buffer size 0x{:X} cut to 0x{:X}", what, size,
                 kMaxStaticBufferSize);
        return kMaxStaticBufferSize;
    }
    return size;
}

class AptInterface final : public Service {
public:
    AptInterface(ServiceContext& context, const char* name)
        : Service(context, name), state_(GetState(context)), apt_(*state_->applet_manager) {
        // clang-format off
        Register(0x0001, "GetLockHandle", &AptInterface::GetLockHandle);
        Register(0x0002, "Initialize", &AptInterface::Initialize);
        Register(0x0003, "Enable", &AptInterface::Enable);
        Register(0x0004, "Finalize");
        Register(0x0005, "GetAppletManInfo", &AptInterface::GetAppletManInfo);
        Register(0x0006, "GetAppletInfo");
        Register(0x0007, "GetLastSignaledAppletId");
        Register(0x0008, "CountRegisteredApplet");
        Register(0x0009, "IsRegistered", &AptInterface::IsRegistered);
        Register(0x000A, "GetAttribute");
        Register(0x000B, "InquireNotification", &AptInterface::InquireNotification);
        Register(0x000C, "SendParameter", &AptInterface::SendParameter);
        Register(0x000D, "ReceiveParameter", &AptInterface::ReceiveParameter);
        Register(0x000E, "GlanceParameter", &AptInterface::GlanceParameter);
        Register(0x000F, "CancelParameter", &AptInterface::CancelParameter);
        Register(0x0010, "DebugFunc");
        Register(0x0011, "MapProgramIdForDebug");
        Register(0x0012, "SetHomeMenuAppletIdForDebug");
        Register(0x0013, "GetPreparationState");
        Register(0x0014, "SetPreparationState");
        Register(0x0015, "PrepareToStartApplication");
        Register(0x0016, "PreloadLibraryApplet", &AptInterface::PreloadLibraryApplet);
        Register(0x0017, "FinishPreloadingLibraryApplet", &AptInterface::FinishPreloadingLibraryApplet);
        Register(0x0018, "PrepareToStartLibraryApplet", &AptInterface::PrepareToStartLibraryApplet);
        Register(0x0019, "PrepareToStartSystemApplet", &AptInterface::PrepareToStartSystemApplet);
        Register(0x001A, "PrepareToStartNewestHomeMenu");
        Register(0x001B, "StartApplication");
        Register(0x001C, "WakeupApplication");
        Register(0x001D, "CancelApplication");
        Register(0x001E, "StartLibraryApplet", &AptInterface::StartLibraryApplet);
        Register(0x001F, "StartSystemApplet", &AptInterface::StartSystemApplet);
        Register(0x0020, "StartNewestHomeMenu");
        Register(0x0021, "OrderToCloseApplication");
        Register(0x0022, "PrepareToCloseApplication", &AptInterface::PrepareToCloseApplication);
        Register(0x0023, "PrepareToJumpToApplication");
        Register(0x0024, "JumpToApplication");
        Register(0x0025, "PrepareToCloseLibraryApplet");
        Register(0x0026, "PrepareToCloseSystemApplet");
        Register(0x0027, "CloseApplication", &AptInterface::CloseApplication);
        Register(0x0028, "CloseLibraryApplet");
        Register(0x0029, "CloseSystemApplet");
        Register(0x002A, "OrderToCloseSystemApplet");
        Register(0x002B, "PrepareToJumpToHomeMenu", &AptInterface::PrepareToJumpToHomeMenu);
        Register(0x002C, "JumpToHomeMenu", &AptInterface::JumpToHomeMenu);
        Register(0x002D, "PrepareToLeaveHomeMenu");
        Register(0x002E, "LeaveHomeMenu");
        Register(0x002F, "PrepareToLeaveResidentApplet");
        Register(0x0030, "LeaveResidentApplet");
        Register(0x0031, "PrepareToDoApplicationJump", &AptInterface::PrepareToDoApplicationJump);
        Register(0x0032, "DoApplicationJump", &AptInterface::DoApplicationJump);
        Register(0x0033, "GetProgramIdOnApplicationJump");
        Register(0x0034, "SendDeliverArg");
        Register(0x0035, "ReceiveDeliverArg");
        Register(0x0036, "LoadSysMenuArg");
        Register(0x0037, "StoreSysMenuArg");
        Register(0x0038, "PreloadResidentApplet");
        Register(0x0039, "PrepareToStartResidentApplet");
        Register(0x003A, "StartResidentApplet");
        Register(0x003B, "CancelLibraryApplet", &AptInterface::CancelLibraryApplet);
        Register(0x003C, "SendDspSleep");
        Register(0x003D, "SendDspWakeUp");
        Register(0x003E, "ReplySleepQuery", &AptInterface::ReplySleepQuery);
        Register(0x003F, "ReplySleepNotificationComplete", &AptInterface::ReplySleepNotificationComplete);
        Register(0x0040, "SendCaptureBufferInfo", &AptInterface::SendCaptureBufferInfo);
        Register(0x0041, "ReceiveCaptureBufferInfo");
        Register(0x0042, "SleepSystem");
        Register(0x0043, "NotifyToWait", &AptInterface::NotifyToWait);
        Register(0x0044, "GetSharedFont");
        Register(0x0045, "GetWirelessRebootInfo");
        Register(0x0046, "Wrap");
        Register(0x0047, "Unwrap");
        Register(0x0048, "GetProgramInfo");
        Register(0x0049, "Reboot");
        Register(0x004A, "GetCaptureInfo");
        Register(0x004B, "AppletUtility", &AptInterface::AppletUtility);
        Register(0x004C, "SetFatalErrDispMode");
        Register(0x004D, "GetAppletProgramInfo");
        Register(0x004E, "HardwareResetAsync");
        Register(0x004F, "SetAppCpuTimeLimit", &AptInterface::SetAppCpuTimeLimit);
        Register(0x0050, "GetAppCpuTimeLimit", &AptInterface::GetAppCpuTimeLimit);
        Register(0x0051, "GetStartupArgument");
        Register(0x0052, "Wrap1");
        Register(0x0053, "Unwrap1");
        Register(0x0054, "Unknown54");
        Register(0x0055, "SetScreenCapturePostPermission", &AptInterface::SetScreenCapturePostPermission);
        Register(0x0056, "GetScreenCapturePostPermission");
        Register(0x0057, "WakeupApplication2");
        Register(0x0058, "GetProgramId");
        Register(0x0101, "GetTargetPlatform", &AptInterface::GetTargetPlatform);
        Register(0x0102, "CheckNew3DS");
        Register(0x0103, "GetApplicationRunningMode", &AptInterface::GetApplicationRunningMode);
        Register(0x0104, "IsStandardMemoryLayout");
        Register(0x0105, "IsTitleAllowed");
        // clang-format on
    }

private:
    void Success(Request& request) { request.MakeReply(1, 0).Push(kResultSuccess); }

    // Inputs: 1: the applet id, 2: the attributes. Outputs: 1: result, 2: translate header, 3-4:
    // the notification event and the parameter event.
    void Initialize(Request& request) {
        const auto app_id = request.Pop<AppletId>();
        const auto attributes = request.Pop<u32>();

        MH4N_LOG(Debug, kLog, "called app_id={:#010X}, attributes={:#010X}", static_cast<u32>(app_id),
                 attributes);

        const ResultVal<InitializeResult> result =
            apt_.Initialize(app_id, AppletAttributes(attributes));
        if (result.Failed()) {
            request.MakeReply(1, 0).Push(result.code);
        } else {
            Reply& reply = request.MakeReply(1, 3);
            reply.Push(kResultSuccess);
            reply.PushCopyObjects(result.value.notification_event, result.value.parameter_event);
        }
    }

    // Inputs: 1: the attributes. Outputs: 1: result, 2: the corrected attributes, 3: the state,
    // 4: translate header, 5: the lock.
    void GetLockHandle(Request& request) {
        // Bits [0:2] are the applet type (System, Library, etc). Bit 5 tells the application that
        // there's a pending APT parameter, this will cause the app to wait until parameter_event is
        // signaled.
        const auto attributes = request.Pop<u32>();

        MH4N_LOG(Debug, kLog, "called applet_attributes={:#010X}", attributes);

        const ResultVal<GetLockHandleResult> result = apt_.GetLockHandle(AppletAttributes(attributes));
        if (result.Failed()) {
            request.MakeReply(1, 0).Push(result.code);
        } else {
            Reply& reply = request.MakeReply(3, 2);
            reply.Push(kResultSuccess);
            reply.Push<u32>(result.value.corrected_attributes.raw);
            reply.Push<u32>(result.value.state);
            reply.PushCopyObjects(result.value.lock);
        }
    }

    void Enable(Request& request) {
        const auto attributes = request.Pop<u32>();

        MH4N_LOG(Debug, kLog, "called attributes={:#010X}", attributes);

        request.MakeReply(1, 0).Push(apt_.Enable(AppletAttributes(attributes)));
    }

    // Inputs: 1: the applet position. Outputs: 1: result, 2: the active position, 3: the requested
    // applet, 4: the HOME menu id, 5: the active applet.
    void GetAppletManInfo(Request& request) {
        const auto applet_pos = request.Pop<AppletPos>();

        MH4N_LOG(Debug, kLog, "called, applet_pos={:08X}", static_cast<u32>(applet_pos));

        const ResultVal<AppletManInfo> info = apt_.GetAppletManInfo(applet_pos);
        if (info.Failed()) {
            request.MakeReply(1, 0).Push(info.code);
        } else {
            Reply& reply = request.MakeReply(5, 0);
            reply.Push(kResultSuccess);
            reply.Push(info.value.active_applet_pos);
            reply.Push(info.value.requested_applet_id);
            reply.Push(info.value.home_menu_applet_id);
            reply.Push(info.value.active_applet_id);
        }
    }

    void IsRegistered(Request& request) {
        const auto app_id = request.Pop<AppletId>();

        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(apt_.IsRegistered(app_id));

        MH4N_LOG(Debug, kLog, "called app_id={:#010X}", static_cast<u32>(app_id));
    }

    void InquireNotification(Request& request) {
        const auto app_id = request.Pop<AppletId>();

        MH4N_LOG(Debug, kLog, "called app_id={:#010X}", static_cast<u32>(app_id));

        const ResultVal<Notification> notification = apt_.InquireNotification(app_id);
        if (notification.Failed()) {
            request.MakeReply(1, 0).Push(notification.code);
        } else {
            Reply& reply = request.MakeReply(2, 0);
            reply.Push(kResultSuccess);
            reply.Push(static_cast<u32>(notification.value));
        }
    }

    // Inputs: 1: the sender, 2: the destination, 3: the signal, 4: the size, 5-6: a handle,
    // 7-8: a static buffer.
    void SendParameter(Request& request) {
        const auto src_app_id = request.Pop<AppletId>();
        const auto dst_app_id = request.Pop<AppletId>();
        const auto signal_type = request.Pop<SignalType>();
        const auto buffer_size = request.Pop<u32>();
        std::shared_ptr<Object> object = request.PopGenericObject();
        const StaticBuffer* buffer = request.PopStaticBuffer();

        MH4N_LOG(Debug, kLog,
                 "called src_app_id={:#010X}, dst_app_id={:#010X}, signal_type={:#010X}, "
                 "buffer_size={:#010X}",
                 static_cast<u32>(src_app_id), static_cast<u32>(dst_app_id),
                 static_cast<u32>(signal_type), buffer_size);

        request.MakeReply(1, 0).Push(apt_.SendParameter({
            .sender_id = src_app_id,
            .destination_id = dst_app_id,
            .signal = signal_type,
            .object = std::move(object),
            .buffer = BufferData(buffer),
        }));
    }

    // The reply of ReceiveParameter and GlanceParameter. Outputs: 1: result, 2: the sender, 3: the
    // signal, 4: the size, 5: translate header, 6: the object, 7-8: the buffer (always as large as
    // the game asked for).
    void ReplyParameter(Request& request, ResultVal<MessageParameter> next_parameter, u32 buffer_size) {
        if (next_parameter.Failed()) {
            request.MakeReply(1, 0).Push(next_parameter.code);
            return;
        }

        MessageParameter& parameter = next_parameter.value;
        const u32 size = std::min(static_cast<u32>(parameter.buffer.size()), buffer_size);
        parameter.buffer.resize(buffer_size);  // APT always push a buffer with the maximum size

        Reply& reply = request.MakeReply(4, 4);
        reply.Push(kResultSuccess);  // No error
        reply.Push(parameter.sender_id);
        reply.Push(parameter.signal);  // Signal type
        reply.Push(size);              // Parameter buffer size
        reply.PushMoveObjects(parameter.object);
        reply.PushStaticBuffer(std::move(parameter.buffer), 0);
    }

    // Inputs: 1: the applet id, 2: the size of the game's buffer.
    void ReceiveParameter(Request& request) {
        const auto app_id = request.Pop<AppletId>();
        const u32 buffer_size = ClampBufferSize(request.Pop<u32>(), "ReceiveParameter");

        MH4N_LOG(Debug, kLog, "called app_id={:#010X}, buffer_size={:#010X}", static_cast<u32>(app_id),
                 buffer_size);

        ReplyParameter(request, apt_.ReceiveParameter(app_id), buffer_size);
    }

    void GlanceParameter(Request& request) {
        const auto app_id = request.Pop<AppletId>();
        const u32 buffer_size = ClampBufferSize(request.Pop<u32>(), "GlanceParameter");

        MH4N_LOG(Debug, kLog, "called app_id={:#010X}, buffer_size={:#010X}", static_cast<u32>(app_id),
                 buffer_size);

        ReplyParameter(request, apt_.GlanceParameter(app_id), buffer_size);
    }

    // Inputs: 1: check the sender, 2: the sender, 3: check the receiver, 4: the receiver.
    // Outputs: 1: result, 2: whether a parameter was cancelled.
    void CancelParameter(Request& request) {
        const auto check_sender = request.Pop<bool>();
        const auto sender_appid = request.Pop<AppletId>();
        const auto check_receiver = request.Pop<bool>();
        const auto receiver_appid = request.Pop<AppletId>();

        MH4N_LOG(Debug, kLog,
                 "called check_sender={}, sender_appid={:#010X}, check_receiver={}, "
                 "receiver_appid={:#010X}",
                 check_sender, static_cast<u32>(sender_appid), check_receiver,
                 static_cast<u32>(receiver_appid));

        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);  // No error
        reply.Push(apt_.CancelParameter(check_sender, sender_appid, check_receiver, receiver_appid));
    }

    // Inputs: 1: flags (a byte), 2-3: the title id, 4: the media type (a byte).
    void PrepareToDoApplicationJump(Request& request) {
        const auto flags = request.Pop<ApplicationJumpFlags>();
        const auto title_id = request.Pop<u64>();
        const auto media_type = request.Pop<u8>();

        MH4N_LOG(Info, kLog, "called title_id={:016X}, media_type={:#01X}, flags={:#08X}", title_id,
                 media_type, static_cast<u32>(flags));

        request.MakeReply(1, 0).Push(apt_.PrepareToDoApplicationJump(title_id, flags));
    }

    // Inputs: 1: the parameter size, 2: the HMAC size, 3-4 and 5-6: two static buffers.
    void DoApplicationJump(Request& request) {
        const auto param_size = request.Pop<u32>();
        const auto hmac_size = request.Pop<u32>();
        request.PopStaticBuffer();  // the parameter (DeliverArg: nothing reads it back here)
        request.PopStaticBuffer();  // the HMAC

        MH4N_LOG(Info, kLog, "called param_size={:08X}, hmac_size={:08X}", param_size, hmac_size);

        request.MakeReply(1, 0).Push(apt_.DoApplicationJump());
    }

    // The input of AppletUtility is read and not used. Inputs: 1: the utility command, 2: the input
    // size, 3: the output size, 4-5: the input buffer. Outputs: 1: result, 2: the utility's result,
    // 3-4: the output buffer.
    void AppletUtility(Request& request) {
        // These are from 3dbrew - I'm not really sure what they're used for.
        const auto utility_command = request.Pop<u32>();
        const auto input_size = request.Pop<u32>();
        const auto output_size = request.Pop<u32>();
        request.PopStaticBuffer();

        MH4N_LOG(Warning, kLog,
                 "(STUBBED) called command={:#010X}, input_size={:#010X}, output_size={:#010X}",
                 utility_command, input_size, output_size);

        std::vector<u8> out(ClampBufferSize(output_size, "AppletUtility"));
        if (utility_command == 0x6 && output_size > 0) {
            // Command 0x6 (TryLockTransition) expects a boolean return value indicating whether the
            // attempt succeeded. Since we don't implement any of the transition locking stuff yet,
            // fake a success result to avoid app crashes.
            out[0] = true;
        }

        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);  // No error
        reply.Push(kResultSuccess);  // Utility function result
        reply.PushStaticBuffer(std::move(out), 0);
    }

    // Inputs: 1: must be one, 2: the percentage. The thread manager is where the application's CPU
    // time limit lives (N1-27); Azahar's PM::PM_APP::UpdateResourceLimit does the same to the
    // application's resource limit.
    void SetAppCpuTimeLimit(Request& request) {
        const auto must_be_one = request.Pop<u32>();
        const auto value = request.Pop<u32>();

        if (must_be_one != 1) {
            MH4N_LOG(Error, kLog, "This value should be one, but is actually {}!", must_be_one);
        }

        request.MakeReply(1, 0).Push(Context().Threads().SetAppCpuTimeLimit(value));
    }

    // Inputs: 1: must be one. Outputs: 1: result, 2: the current percentage.
    void GetAppCpuTimeLimit(Request& request) {
        const auto must_be_one = request.Pop<u32>();

        if (must_be_one != 1) {
            MH4N_LOG(Error, kLog, "This value should be one, but is actually {}!", must_be_one);
        }

        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(Context().Threads().AppCpuTimeLimit());
    }

    // Inputs: 1: the applet id.
    void PreloadLibraryApplet(Request& request) {
        const auto applet_id = request.Pop<AppletId>();

        MH4N_LOG(Debug, kLog, "called, applet_id={:08X}", static_cast<u32>(applet_id));

        request.MakeReply(1, 0).Push(apt_.PreloadLibraryApplet(applet_id));
    }

    // Inputs: 1: the applet id.
    void FinishPreloadingLibraryApplet(Request& request) {
        const auto applet_id = request.Pop<AppletId>();

        request.MakeReply(1, 0).Push(apt_.FinishPreloadingLibraryApplet(applet_id));

        MH4N_LOG(Warning, kLog, "(STUBBED) called, applet_id={:#05X}", static_cast<u32>(applet_id));
    }

    // Inputs: 1: the applet id.
    void PrepareToStartLibraryApplet(Request& request) {
        const auto applet_id = request.Pop<AppletId>();

        MH4N_LOG(Debug, kLog, "called, applet_id={:08X}", static_cast<u32>(applet_id));

        request.MakeReply(1, 0).Push(apt_.PrepareToStartLibraryApplet(applet_id));
    }

    void PrepareToStartSystemApplet(Request& request) {
        const auto applet_id = request.Pop<AppletId>();

        MH4N_LOG(Debug, kLog, "called, applet_id={:08X}", static_cast<u32>(applet_id));

        request.MakeReply(1, 0).Push(apt_.PrepareToStartSystemApplet(applet_id));
    }

    // Inputs: 1: the applet id, 2: the size, 3-4: a handle, 5-6: a static buffer.
    void StartLibraryApplet(Request& request) {
        const auto applet_id = request.Pop<AppletId>();
        const auto buffer_size = request.Pop<u32>();
        std::shared_ptr<Object> object = request.PopGenericObject();
        const StaticBuffer* buffer = request.PopStaticBuffer();

        MH4N_LOG(Debug, kLog, "called, applet_id={:08X}, size={:08X}", static_cast<u32>(applet_id),
                 buffer_size);

        request.MakeReply(1, 0).Push(
            apt_.StartLibraryApplet(applet_id, std::move(object), BufferData(buffer)));
    }

    void StartSystemApplet(Request& request) {
        const auto applet_id = request.Pop<AppletId>();
        const auto buffer_size = request.Pop<u32>();
        std::shared_ptr<Object> object = request.PopGenericObject();
        const StaticBuffer* buffer = request.PopStaticBuffer();

        MH4N_LOG(Debug, kLog, "called, applet_id={:08X}, size={:08X}", static_cast<u32>(applet_id),
                 buffer_size);

        request.MakeReply(1, 0).Push(
            apt_.StartSystemApplet(applet_id, std::move(object), BufferData(buffer)));
    }

    // Inputs: 1: return to the system (a byte).
    void PrepareToCloseApplication(Request& request) {
        const bool return_to_sys = request.Pop<u8>() != 0;

        MH4N_LOG(Debug, kLog, "called return_to_sys={}", return_to_sys);

        request.MakeReply(1, 0).Push(apt_.PrepareToCloseApplication(return_to_sys));
    }

    // Inputs: 1: the parameter size, 2-3: a handle, 4-5: a static buffer.
    void CloseApplication(Request& request) {
        const auto parameter_size = request.Pop<u32>();
        std::shared_ptr<Object> object = request.PopGenericObject();
        const StaticBuffer* buffer = request.PopStaticBuffer();

        MH4N_LOG(Debug, kLog, "called size={}", parameter_size);

        request.MakeReply(1, 0).Push(apt_.CloseApplication(std::move(object), BufferData(buffer)));
    }

    void CancelLibraryApplet(Request& request) {
        const auto app_exiting = request.Pop<bool>();

        MH4N_LOG(Debug, kLog, "called app_exiting={}", app_exiting);

        request.MakeReply(1, 0).Push(apt_.CancelLibraryApplet(app_exiting));
    }

    // Stubbed in Azahar too: they answer success and log.
    void ReplySleepQuery(Request& request) {
        const auto from_app_id = request.Pop<AppletId>();
        const auto reply_value = request.Pop<u32>();

        MH4N_LOG(Warning, kLog, "(STUBBED) called, from_app_id={:08X}, reply_value={:08X}",
                 static_cast<u32>(from_app_id), reply_value);

        Success(request);
    }

    void ReplySleepNotificationComplete(Request& request) {
        const auto from_app_id = request.Pop<AppletId>();

        MH4N_LOG(Warning, kLog, "(STUBBED) called, from_app_id={:08X}", static_cast<u32>(from_app_id));

        Success(request);
    }

    void PrepareToJumpToHomeMenu(Request& request) {
        MH4N_LOG(Debug, kLog, "called");

        request.MakeReply(1, 0).Push(apt_.PrepareToJumpToHomeMenu());
    }

    // Inputs: 1: the parameter size, 2-3: a handle, 4-5: a static buffer.
    void JumpToHomeMenu(Request& request) {
        const auto parameter_size = request.Pop<u32>();
        std::shared_ptr<Object> object = request.PopGenericObject();
        const StaticBuffer* buffer = request.PopStaticBuffer();

        MH4N_LOG(Debug, kLog, "called size={}", parameter_size);

        request.MakeReply(1, 0).Push(apt_.JumpToHomeMenu(std::move(object), BufferData(buffer)));
    }

    // Inputs: 1: the size, 2-3: the static buffer.
    void SendCaptureBufferInfo(Request& request) {
        request.Pop<u32>();  // the size
        const StaticBuffer* buffer = request.PopStaticBuffer();

        MH4N_LOG(Debug, kLog, "called");

        apt_.SendCaptureBufferInfo(BufferData(buffer));

        Success(request);
    }

    void NotifyToWait(Request& request) {
        const auto app_id = request.Pop<u32>();

        Success(request);

        MH4N_LOG(Warning, kLog, "(STUBBED) app_id={}", app_id);
    }

    void SetScreenCapturePostPermission(Request& request) {
        const auto permission = request.Pop<u32>();

        MH4N_LOG(Debug, kLog, "called, permission={}", permission);

        state_->screen_capture_post_permission = permission & 0xF;

        Success(request);
    }

    void GetTargetPlatform(Request& request) {
        MH4N_LOG(Debug, kLog, "called");

        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(apt_.GetTargetPlatform());
    }

    void GetApplicationRunningMode(Request& request) {
        MH4N_LOG(Debug, kLog, "called");

        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(apt_.GetApplicationRunningMode());
    }

    std::shared_ptr<AptState> state_;
    AppletManager& apt_;
};

// The names Azahar installs: the three services share the commands and the manager.
MH4N_REGISTER(ServiceFactory, "APT:U",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<AptInterface>(c, "APT:U"); },
                             kMaxAptSessions));
MH4N_REGISTER(ServiceFactory, "APT:A",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<AptInterface>(c, "APT:A"); },
                             kMaxAptSessions));
MH4N_REGISTER(ServiceFactory, "APT:S",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<AptInterface>(c, "APT:S"); },
                             kMaxAptSessions));

// Azahar makes the module when it makes the system, before the game runs, and the application slot
// gets the program id of the process that registers: here the program that was loaded. (A run that
// is not a program, the replay tool, has none: 0.) The name sorts after "services.ipc", which makes
// the service manager the state lives in.
void StartApt(Runtime& rt) {
    ServiceManager* manager = FindServiceManager(rt);
    if (manager == nullptr) {
        MH4N_LOG(Error, kLog, "the APT module needs the service manager");
        return;
    }
    const std::shared_ptr<AptState> state = manager->Context().State<AptState>("apt");
    if (const ProcessImage* image = rt.Image()) {
        state->program_id = image->program_id;
    }
    state->Ensure(manager->Context());
    state->applet_manager->SetProgramId(state->program_id);
}

MH4N_REGISTER(BootStep, "services.ipc.apt", BootStep{Phase::Services, StartApt});

}  // namespace

}  // namespace mh4n::apt
