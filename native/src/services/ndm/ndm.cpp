// Derived from Azahar src/core/hle/service/ndm/ndm_u.cpp and src/core/hle/service/ndm/ndm_u.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the commands of ndm:u with their ids, replies and result codes, and the state they keep
// (the daemon masks and statuses, the suspend counters, the exclusive state, the intervals).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
//
// ndm:u (N1-15): the network daemons manager, as Azahar's stub: it keeps the state the commands set
// and reads and starts no daemon. All sessions share the state, as Azahar's one service object does.
//
// Departures from Azahar:
//  - QueryStatus and GetDaemonDisableCount with a daemon number above 3 end Azahar with an exception
//    (std::array::at); here they answer the out of range result (description 1021,
//    module NDM) and leave the state alone.
#include <array>
#include <memory>

#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.NDM";
constexpr u32 kMaxSessions = 6;
constexpr u32 kModuleNdm = 26;
constexpr Result kResultOutOfRange = MakeResult(1021, kModuleNdm, 8, 27);

// Azahar's DaemonMask / DaemonStatus / ExclusiveState / default intervals.
constexpr u32 kMaskCec = 1 << 0, kMaskFriend = 1 << 3;
constexpr u32 kMaskDefault = kMaskCec | kMaskFriend;
enum DaemonStatus : u32 { kBusy = 0, kIdle = 1, kSuspending = 2, kSuspended = 3 };
constexpr u32 kDefaultRetryInterval = 10;
constexpr u32 kDefaultScanInterval = 30;

struct State {
    u32 daemon_bit_mask = kMaskDefault;
    u32 default_daemon_bit_mask = kMaskDefault;
    std::array<u32, 4> daemon_status = {kIdle, kIdle, kIdle, kIdle};
    std::array<u32, 4> daemon_suspend_counter{};
    u32 exclusive_state = 0;
    u32 scan_interval = kDefaultScanInterval;
    u32 retry_interval = kDefaultRetryInterval;
    bool daemon_lock_enabled = false;
};

class Ndm final : public Service {
public:
    explicit Ndm(ServiceContext& context) : Service(context, "ndm:u"), s_(*context.State<State>("ndm:u")) {
        Register(0x0001, "EnterExclusiveState", &Ndm::EnterExclusiveState);
        Register(0x0002, "LeaveExclusiveState", &Ndm::LeaveExclusiveState);
        Register(0x0003, "QueryExclusiveMode", &Ndm::QueryExclusiveMode);
        Register(0x0004, "LockState", &Ndm::LockState);
        Register(0x0005, "UnlockState", &Ndm::UnlockState);
        Register(0x0006, "SuspendDaemons", &Ndm::SuspendDaemons);
        Register(0x0007, "ResumeDaemons", &Ndm::ResumeDaemons);
        Register(0x0008, "SuspendScheduler", &Ndm::SuspendScheduler);
        Register(0x0009, "ResumeScheduler", &Ndm::ResumeScheduler);
        Register(0x000A, "GetCurrentState");
        Register(0x000B, "GetTargetState");
        Register(0x000C, "<Stubbed>");
        Register(0x000D, "QueryStatus", &Ndm::QueryStatus);
        Register(0x000E, "GetDaemonDisableCount", &Ndm::GetDaemonDisableCount);
        Register(0x000F, "GetSchedulerDisableCount", &Ndm::GetSchedulerDisableCount);
        Register(0x0010, "SetScanInterval", &Ndm::SetScanInterval);
        Register(0x0011, "GetScanInterval", &Ndm::GetScanInterval);
        Register(0x0012, "SetRetryInterval", &Ndm::SetRetryInterval);
        Register(0x0013, "GetRetryInterval", &Ndm::GetRetryInterval);
        Register(0x0014, "OverrideDefaultDaemons", &Ndm::OverrideDefaultDaemons);
        Register(0x0015, "ResetDefaultDaemons", &Ndm::ResetDefaultDaemons);
        Register(0x0016, "GetDefaultDaemons", &Ndm::GetDefaultDaemons);
        Register(0x0017, "ClearHalfAwakeMacFilter", &Ndm::ClearHalfAwakeMacFilter);
    }

private:
    void Success(Request& request) { request.MakeReply(1, 0).Push(kResultSuccess); }

    // Inputs: 1: the exclusive state, 2-3: the calling PID.
    void EnterExclusiveState(Request& request) {
        s_.exclusive_state = request.Pop<u32>();
        request.PopPid();
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) exclusive_state=0x{:08X}", s_.exclusive_state);
    }
    void LeaveExclusiveState(Request& request) {
        request.PopPid();
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED)");
    }
    void QueryExclusiveMode(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(s_.exclusive_state);
    }
    void LockState(Request& request) {
        request.PopPid();
        s_.daemon_lock_enabled = true;
        Success(request);
    }
    void UnlockState(Request& request) {
        request.PopPid();
        s_.daemon_lock_enabled = false;
        Success(request);
    }

    void SuspendDaemons(Request& request) {
        const u32 bit_mask = request.Pop<u32>() & 0xF;
        s_.daemon_bit_mask = s_.default_daemon_bit_mask & ~bit_mask;
        for (std::size_t index = 0; index < s_.daemon_status.size(); ++index) {
            if ((bit_mask & (1u << index)) != 0 && s_.daemon_suspend_counter[index]++ == 0) {
                s_.daemon_status[index] = kSuspended;
            }
        }
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) bit_mask=0x{:08X}", bit_mask);
    }

    void ResumeDaemons(Request& request) {
        const u32 bit_mask = request.Pop<u32>() & 0xF;
        s_.daemon_bit_mask &= ~bit_mask;
        MH4N_LOG(Warning, kLog, "(STUBBED) bit_mask=0x{:08X}", bit_mask);
        for (std::size_t index = 0; index < s_.daemon_status.size(); ++index) {
            if ((bit_mask & (1u << index)) != 0 && s_.daemon_suspend_counter[index] == 0) {
                request.MakeReply(1, 0).Push(MakeResult(13, kModuleNdm, 5, 25));  // InvalidState, Status
                return;
            }
        }
        for (std::size_t index = 0; index < s_.daemon_status.size(); ++index) {
            if ((bit_mask & (1u << index)) != 0 && --s_.daemon_suspend_counter[index] == 0) {
                s_.daemon_status[index] = kIdle;
            }
        }
        Success(request);
    }

    void SuspendScheduler(Request& request) {
        const bool perform_in_background = request.Pop<bool>();
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) perform_in_background={}", perform_in_background);
    }
    void ResumeScheduler(Request& request) {
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED)");
    }

    void QueryStatus(Request& request) {
        const u8 daemon = request.Pop<u8>();
        Reply& reply = request.MakeReply(2, 0);
        if (daemon >= s_.daemon_status.size()) {
            reply.Push(kResultOutOfRange);
            reply.Push<u32>(0);
            return;
        }
        reply.Push(kResultSuccess);
        reply.Push(s_.daemon_status[daemon]);
        MH4N_LOG(Warning, kLog, "(STUBBED) daemon=0x{:02X}", daemon);
    }

    void GetDaemonDisableCount(Request& request) {
        const u8 daemon = request.Pop<u8>();
        Reply& reply = request.MakeReply(3, 0);
        if (daemon >= s_.daemon_suspend_counter.size()) {
            reply.Push(kResultOutOfRange);
            reply.Push<u32>(0);
            reply.Push<u32>(0);
            return;
        }
        reply.Push(kResultSuccess);
        // Azahar: "TODO(PabloMK7): Implement separate per process count"
        reply.Push(s_.daemon_suspend_counter[daemon]);  // current process disable count
        reply.Push(s_.daemon_suspend_counter[daemon]);  // total disable count
        MH4N_LOG(Warning, kLog, "(STUBBED) daemon=0x{:02X}", daemon);
    }

    void GetSchedulerDisableCount(Request& request) {
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);  // current process disable count
        reply.Push<u32>(0);  // total disable count
    }

    void SetScanInterval(Request& request) {
        s_.scan_interval = request.Pop<u32>();
        Success(request);
    }
    void GetScanInterval(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(s_.scan_interval);
    }
    void SetRetryInterval(Request& request) {
        s_.retry_interval = request.Pop<u32>();
        Success(request);
    }
    void GetRetryInterval(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(s_.retry_interval);
    }

    void OverrideDefaultDaemons(Request& request) {
        const u32 bit_mask = request.Pop<u32>() & 0xF;
        s_.default_daemon_bit_mask = bit_mask;
        s_.daemon_bit_mask = s_.default_daemon_bit_mask;
        for (std::size_t index = 0; index < s_.daemon_status.size(); ++index) {
            if ((bit_mask & (1u << index)) != 0) {
                s_.daemon_status[index] = kIdle;
            }
        }
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) bit_mask=0x{:08X}", bit_mask);
    }
    void ResetDefaultDaemons(Request& request) {
        s_.default_daemon_bit_mask = kMaskDefault;
        Success(request);
    }
    void GetDefaultDaemons(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(s_.default_daemon_bit_mask);
    }
    void ClearHalfAwakeMacFilter(Request& request) { Success(request); }

    State& s_;
};

MH4N_REGISTER(ServiceFactory, "ndm:u",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<Ndm>(c); }, kMaxSessions));

}  // namespace

}  // namespace mh4n
