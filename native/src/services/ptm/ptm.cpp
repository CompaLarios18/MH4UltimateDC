// Derived from Azahar src/core/hle/service/ptm/ptm.cpp, src/core/hle/service/ptm/ptm.h,
// src/core/hle/service/ptm/ptm_u.cpp and src/core/hle/service/ptm/ptm_sysm.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the commands of ptm:u, ptm:s and ptm:sysm with their ids and replies, the charge levels,
// and the defaults (shell open, battery charging and full, pedometer not counting, no steps).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (ptm.cpp, ptm.h, ptm_sysm.cpp)
// Copyright 2014 Citra Emulator Project (ptm_u.cpp)
//
// ptm:u (N1-15), plus ptm:s and ptm:sysm for the commands they share with it and CheckNew3DS. State
// is Azahar's defaults and never changes: the shell is open, the battery is full and charging, the
// adapter is plugged in, the pedometer is not counting and every hour has 0 steps
// (`Settings::values.steps_per_hour` = 0).
//
// Departures from Azahar:
//  - ptm:u has no CheckNew3DS in Azahar (it is a ptm:s/ptm:sysm command, 0x040A); it is answered the
//    same way here: Old 3DS (false), D10.
//  - GetStepHistory with a buffer that is not 2 bytes per hour asserts in Azahar; here it answers the
//    invalid size result and leaves the buffer alone.
//  - ptm:gets, ptm:play and ptm:sets, the info LED commands and GetSystemTime of ptm:s/ptm:sysm (they
//    need the MCU and the shared page) are not provided: no application of the game sends them.
//  - The play coin file (gamecoin.dat) that Azahar's ptm creates at start is not made.
#include <memory>
#include <vector>

#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.PTM";
constexpr u32 kMaxSessions = 26;
constexpr u32 kChargeCompletelyFull = 5;  // ChargeLevels::CompletelyFull
constexpr Result kResultInvalidSize = MakeResult(1004, 53 /*ErrorModule::PTM*/, 8, 27);

enum class Kind { User, System };

class Ptm final : public Service {
public:
    Ptm(ServiceContext& context, const char* name, Kind kind) : Service(context, name) {
        Register(0x0001, "RegisterAlarmClient");
        Register(0x0002, "SetRtcAlarm");
        Register(0x0003, "GetRtcAlarm");
        Register(0x0004, "CancelRtcAlarm");
        Register(0x0005, "GetAdapterState", &Ptm::GetAdapterState);
        Register(0x0006, "GetShellState", &Ptm::GetShellState);
        Register(0x0007, "GetBatteryLevel", &Ptm::GetBatteryLevel);
        Register(0x0008, "GetBatteryChargeState", &Ptm::GetBatteryChargeState);
        if (kind == Kind::User) {
            Register(0x0009, "GetPedometerState", &Ptm::GetPedometerState);
        } else {
            Register(0x0009, "GetPedometerState");
        }
        Register(0x000A, "GetStepHistoryEntry");
        Register(0x000B, "GetStepHistory", &Ptm::GetStepHistory);
        Register(0x000C, "GetTotalStepCount", &Ptm::GetTotalStepCount);
        Register(0x000D, "SetPedometerRecordingMode");
        Register(0x000E, "GetPedometerRecordingMode");
        Register(0x000F, "GetStepHistoryAll");
        if (kind == Kind::System) {
            Register(0x040A, "CheckNew3DS", &Ptm::CheckNew3DS);
            Register(0x080F, "GetSoftwareClosedFlag", &Ptm::GetSoftwareClosedFlag);
        }
    }

private:
    // Outputs: 1: result, 2: 1 if the adapter is plugged in (Azahar: the charging flag).
    void GetAdapterState(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(kBatteryIsCharging);
    }
    void GetShellState(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(kShellOpen);
    }
    void GetBatteryLevel(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(kChargeCompletelyFull);
    }
    void GetBatteryChargeState(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(kBatteryIsCharging);
    }
    void GetPedometerState(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(kPedometerIsCounting);
    }

    // Inputs: 1: hours, 2-3: the start time, 4-5: the output buffer (2 bytes per hour).
    void GetStepHistory(Request& request) {
        const u32 hours = request.Pop<u32>();
        const u64 start_time = request.Pop<u64>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        if (buffer == nullptr) {
            request.MakeReply(1, 0).Push(kResultInvalidBufferDescriptor);
            return;
        }
        if (static_cast<u64>(hours) * sizeof(u16) != buffer->Size()) {
            MH4N_LOG(Error, kLog, "Buffer for steps count has incorrect size");
            Reply& reply = request.MakeReply(1, 2);
            reply.Push(kResultInvalidSize);
            reply.PushMappedBuffer(*buffer);
            return;
        }
        const std::vector<u8> steps(buffer->Size(), 0);  // steps_per_hour = 0 for every hour
        buffer->Write(steps.data(), 0, static_cast<u32>(steps.size()));
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushMappedBuffer(*buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, from time(raw): 0x{:x}, for {} hours", start_time,
                 hours);
    }

    void GetTotalStepCount(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
    }

    void GetSoftwareClosedFlag(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(false);
    }

    // Old 3DS only (D10): `Settings::values.is_new_3ds` is off.
    void CheckNew3DS(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(false);
    }

    static constexpr bool kShellOpen = true;
    static constexpr bool kBatteryIsCharging = true;
    static constexpr bool kPedometerIsCounting = false;
};

template <Kind kind> ServiceFactory Factory(const char* name) {
    return ServiceFactory(
        [name](ServiceContext& c) { return std::make_unique<Ptm>(c, name, kind); }, kMaxSessions);
}

MH4N_REGISTER(ServiceFactory, "ptm:u", Factory<Kind::User>("ptm:u"));
MH4N_REGISTER(ServiceFactory, "ptm:s", Factory<Kind::System>("ptm:s"));
MH4N_REGISTER(ServiceFactory, "ptm:sysm", Factory<Kind::System>("ptm:sysm"));

}  // namespace

}  // namespace mh4n
