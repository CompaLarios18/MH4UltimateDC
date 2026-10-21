// Derived from Azahar src/core/hle/service/hid/hid.cpp, src/core/hle/service/hid/hid.h,
// src/core/hle/service/hid/hid_user.cpp, src/core/hle/service/hid/hid_spvr.cpp and
// src/common/settings.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the two services' command tables and the commands (GetIPCHandles and the others) with
// their replies, and the default sound volume (settings.h).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.cpp, hid.h)
// Copyright 2015 Citra Emulator Project (hid_user.cpp, hid_spvr.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (settings.h)
//
// hid:USER and hid:SPVR (N1-16): the same commands over one module (hid_state.h). Both accept six
// sessions, as Azahar's. The commands Azahar lists without a handler (CalibrateTouchScreen,
// UpdateTouchConfig, StartAnalogStickCalibration, GetAnalogStickCalibrateParam) are answered as any
// unknown command is.
#include <memory>

#include "hid_state.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

namespace mh4n::hid {

namespace {

constexpr const char* kLog = "Service.HID";

// settings.h: volume{1.f, 0.f, 1.f}; GetSoundVolume answers 0x3F * volume.
constexpr u8 kSoundVolume = 0x3F;

class HidInterface final : public Service {
public:
    HidInterface(ServiceContext& context, const char* name)
        : Service(context, name), hid_(GetShared(context)) {
        Register(0x0001, "CalibrateTouchScreen");
        Register(0x0002, "UpdateTouchConfig");
        Register(0x000A, "GetIPCHandles", &HidInterface::GetIPCHandles);
        Register(0x000B, "StartAnalogStickCalibration");
        Register(0x000E, "GetAnalogStickCalibrateParam");
        Register(0x0011, "EnableAccelerometer", &HidInterface::EnableAccelerometer);
        Register(0x0012, "DisableAccelerometer", &HidInterface::DisableAccelerometer);
        Register(0x0013, "EnableGyroscopeLow", &HidInterface::EnableGyroscopeLow);
        Register(0x0014, "DisableGyroscopeLow", &HidInterface::DisableGyroscopeLow);
        Register(0x0015, "GetGyroscopeLowRawToDpsCoefficient",
                 &HidInterface::GetGyroscopeLowRawToDpsCoefficient);
        Register(0x0016, "GetGyroscopeLowCalibrateParam",
                 &HidInterface::GetGyroscopeLowCalibrateParam);
        Register(0x0017, "GetSoundVolume", &HidInterface::GetSoundVolume);
    }

private:
    // Outputs: 1: result, 2: translate header, 3: the shared memory, 4-8: the events (pad or touch
    // twice, accelerometer, gyroscope, debug pad)
    void GetIPCHandles(Request& request) {
        if (!hid_->Memory()) {
            request.MakeReply(1, 0).Push(kResultOutOfMemory);  // no shared memory could be made
            return;
        }
        Reply& reply = request.MakeReply(1, 7);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(hid_->Memory(), hid_->PadOrTouch1(), hid_->PadOrTouch2(),
                              hid_->AccelerometerEvent(), hid_->GyroscopeEvent(),
                              hid_->DebugPadEvent());
    }

    void EnableAccelerometer(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
        hid_->EnableAccelerometer();
    }
    void DisableAccelerometer(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
        hid_->DisableAccelerometer();
    }
    void EnableGyroscopeLow(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
        hid_->EnableGyroscope();
    }
    void DisableGyroscopeLow(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
        hid_->DisableGyroscope();
    }

    // Outputs: 1: result, 2: the coefficient (a float)
    void GetGyroscopeLowRawToDpsCoefficient(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(kGyroscopeCoefficient);
    }

    // Outputs: 1: result, 2-6: the 18 bytes of GyroscopeCalibrateParam
    void GetGyroscopeLowCalibrateParam(Request& request) {
        Reply& reply = request.MakeReply(6, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(DefaultGyroscopeCalibrateParam());
        MH4N_LOG(Warning, kLog, "(STUBBED) GetGyroscopeLowCalibrateParam called");
    }

    // Outputs: 1: result, 2: the volume (a byte)
    void GetSoundVolume(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(kSoundVolume);
    }

    std::shared_ptr<Shared> hid_;
};

MH4N_REGISTER(ServiceFactory, "hid:USER", ServiceFactory(
                                              [](ServiceContext& context) {
                                                  return std::make_unique<HidInterface>(context,
                                                                                        "hid:USER");
                                              },
                                              6));
MH4N_REGISTER(ServiceFactory, "hid:SPVR", ServiceFactory(
                                              [](ServiceContext& context) {
                                                  return std::make_unique<HidInterface>(context,
                                                                                        "hid:SPVR");
                                              },
                                              6));

// Azahar makes the module when it makes the system, before the game runs: the first pad update is
// one period after that. The name sorts after "services.ipc", which makes the service manager the
// state lives in.
void StartHid(Runtime& rt) {
    ServiceManager* manager = FindServiceManager(rt);
    if (manager == nullptr) {
        MH4N_LOG(Error, kLog, "the HID module needs the service manager");
        return;
    }
    GetShared(manager->Context());
}

MH4N_REGISTER(BootStep, "services.ipc.hid", BootStep{Phase::Services, StartHid});

}  // namespace

}  // namespace mh4n::hid
